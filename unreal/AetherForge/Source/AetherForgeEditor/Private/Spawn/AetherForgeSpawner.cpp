// Copyright AetherForge. All Rights Reserved.

#include "AetherForgeSpawner.h"

#include "AetherForgeManifest.h"
#include "CollisionQueryParams.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "ScopedTransaction.h"

DEFINE_LOG_CATEGORY_STATIC(LogAetherForgeSpawner, Log, All);

#define LOCTEXT_NAMESPACE "AetherForgeSpawner"

namespace
{
	/** Vertical extent of the ground-snap trace, in cm (1 km up / 1 km down). */
	constexpr double GroundTraceHalfHeight = 100000.0;
}

FAetherForgeSpawner::~FAetherForgeSpawner()
{
	// Shutdown() should have run already (module shutdown); belt-and-braces.
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void FAetherForgeSpawner::Initialize()
{
	check(IsInGameThread());
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateSP(this, &FAetherForgeSpawner::SpawnTick));
	}
}

void FAetherForgeSpawner::Shutdown()
{
	check(IsInGameThread());
	if (IsGenerationActive())
	{
		EndGeneration(/*bDiscardPending*/ true);
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void FAetherForgeSpawner::BeginGeneration(const FString& GenerationId, const FString& Prompt,
	const UAetherForgeManifest* Manifest)
{
	check(IsInGameThread());

	if (IsGenerationActive())
	{
		// One generation at a time: a new generate cancels the old one (contract).
		UE_LOG(LogAetherForgeSpawner, Warning,
			TEXT("BeginGeneration('%s') while '%s' is open; finalizing the old generation."),
			*GenerationId, *ActiveGenerationId);
		EndGeneration(/*bDiscardPending*/ true);
	}

	ActiveGenerationId = GenerationId;
	ActivePrompt = Prompt;
	ActiveGenerationTag = FName(*GenerationId);
	OutlinerFolderPath = FName(*FString::Printf(TEXT("AetherForge/%s"), *SanitizeForOutlinerFolder(Prompt)));
	bFinalizeWhenDrained = false;
	ContainerActor.Reset();
	ReadyQueue.Reset();
	ReadyQueueHead = 0;
	Stats = FAetherForgeSpawnStats();
	GenerationStartSeconds = FPlatformTime::Seconds();

	// Snapshot the manifest into per-category runtime state. Asset loads are kicked
	// lazily, on first sight of each category in the stream.
	Categories.Reset();
	if (Manifest)
	{
		for (const TPair<FString, FAetherForgeManifestAssetEntry>& Pair : Manifest->Entries)
		{
			FCategoryRuntime Runtime;
			Runtime.AssetPath = Pair.Value.AssetPath;
			Runtime.bInstanceable = Pair.Value.bInstanceable;
			Runtime.bGroundSnap = Pair.Value.bGroundSnap;
			Categories.Add(Pair.Key, MoveTemp(Runtime));
		}
	}

	// One transaction for the whole generation: Ctrl+Z removes it atomically.
	// Held open across ticks while chunks stream in — see the header note on this
	// pattern's risk.
	ActiveTransaction = MakeUnique<FScopedTransaction>(
		FText::Format(LOCTEXT("GenerationTransaction", "AetherForge Generation ({0})"),
			FText::FromString(GenerationId)));

	UE_LOG(LogAetherForgeSpawner, Log, TEXT("Generation '%s' opened (%d manifest categories)."),
		*GenerationId, Categories.Num());
}

void FAetherForgeSpawner::EnqueueEntries(const TArray<FAetherForgeAssetEntry>& Entries)
{
	check(IsInGameThread());

	if (!IsGenerationActive())
	{
		UE_LOG(LogAetherForgeSpawner, Warning, TEXT("EnqueueEntries with no open generation; %d entries dropped."),
			Entries.Num());
		return;
	}

	for (const FAetherForgeAssetEntry& Entry : Entries)
	{
		FCategoryRuntime* Runtime = Categories.Find(Entry.category);
		if (!Runtime)
		{
			// The backend validates recipes against the hello manifest, so this means
			// contract drift — log loudly, skip safely.
			UE_LOG(LogAetherForgeSpawner, Error,
				TEXT("Entry references unknown category '%s' (not in the advertised manifest); skipped."),
				*Entry.category);
			++Stats.SkippedCount;
			continue;
		}

		if (Runtime->bLoadFailed)
		{
			++Stats.SkippedCount;
			continue; // already logged when the load failed
		}

		if (!Runtime->bLoadRequested)
		{
			Runtime->bLoadRequested = true;
			if (Runtime->AssetPath.IsValid())
			{
				// Async preload; entries for this category wait until it resolves.
				// Never a synchronous load in the spawn tick.
				Runtime->LoadHandle = Streamable.RequestAsyncLoad(
					Runtime->AssetPath,
					FStreamableDelegate::CreateSP(this, &FAetherForgeSpawner::OnCategoryAssetLoaded, Entry.category));
			}
			else
			{
				Runtime->bLoadFailed = true;
				UE_LOG(LogAetherForgeSpawner, Error,
					TEXT("Category '%s' has no AssetPath in the manifest; its entries will be skipped."),
					*Entry.category);
				++Stats.SkippedCount;
				continue;
			}
		}

		if (Runtime->LoadedAsset.IsValid())
		{
			ReadyQueue.Add(Entry);
		}
		else
		{
			Runtime->WaitingEntries.Add(Entry);
		}
	}

	Stats.PendingCount = NumPendingEntries();
}

void FAetherForgeSpawner::OnCategoryAssetLoaded(FString CategoryKey)
{
	check(IsInGameThread());

	FCategoryRuntime* Runtime = Categories.Find(CategoryKey);
	if (!Runtime)
	{
		return; // generation already finalized and state reset
	}

	UObject* Loaded = Runtime->LoadHandle.IsValid() ? Runtime->LoadHandle->GetLoadedAsset() : nullptr;
	if (!Loaded)
	{
		Loaded = Runtime->AssetPath.ResolveObject();
	}

	if (!Loaded)
	{
		Runtime->bLoadFailed = true;
		Stats.SkippedCount += Runtime->WaitingEntries.Num();
		Runtime->WaitingEntries.Empty();
		UE_LOG(LogAetherForgeSpawner, Error,
			TEXT("Failed to load asset '%s' for category '%s'; its entries will be skipped."),
			*Runtime->AssetPath.ToString(), *CategoryKey);
		return;
	}

	Runtime->LoadedAsset = Loaded;
	UE_LOG(LogAetherForgeSpawner, Verbose, TEXT("Category '%s' asset loaded: %s (%d entries now eligible)."),
		*CategoryKey, *Loaded->GetName(), Runtime->WaitingEntries.Num());

	// Promote everything that was waiting on this load.
	ReadyQueue.Append(MoveTemp(Runtime->WaitingEntries));
	Runtime->WaitingEntries.Reset();
}

bool FAetherForgeSpawner::SpawnTick(float /*DeltaTime*/)
{
	check(IsInGameThread());

	if (!IsGenerationActive())
	{
		return true;
	}

	const bool bQueueEmpty = ReadyQueueHead >= ReadyQueue.Num();
	if (bQueueEmpty)
	{
		if (bFinalizeWhenDrained && NumPendingEntries() == 0)
		{
			FinalizeGeneration();
		}
		return true;
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return true;
	}

	// Budget-by-time, never fixed-count. Ground-snap traces count against the budget.
	const double TickStart = FPlatformTime::Seconds();
	if (Stats.SpawnedCount == 0)
	{
		// Start the throughput clock at the first real spawn, not at generate-click —
		// otherwise assets/s averages in the LLM's thinking time.
		GenerationStartSeconds = TickStart;
	}
	while (ReadyQueueHead < ReadyQueue.Num() &&
		(FPlatformTime::Seconds() - TickStart) < SpawnBudgetSeconds)
	{
		// Copy: SpawnEntry can reallocate ReadyQueue indirectly via callbacks.
		const FAetherForgeAssetEntry Entry = ReadyQueue[ReadyQueueHead++];
		if (FCategoryRuntime* Runtime = Categories.Find(Entry.category))
		{
			SpawnEntry(Entry, *Runtime, World);
		}
	}

	// Compact once fully drained so the array does not grow without bound.
	if (ReadyQueueHead >= ReadyQueue.Num())
	{
		ReadyQueue.Reset();
		ReadyQueueHead = 0;
	}

	Stats.LastSpawnTickMs = (FPlatformTime::Seconds() - TickStart) * 1000.0;
	Stats.MaxSpawnTickMs = FMath::Max(Stats.MaxSpawnTickMs, Stats.LastSpawnTickMs);
	Stats.PendingCount = NumPendingEntries();
	const double Elapsed = FPlatformTime::Seconds() - GenerationStartSeconds;
	Stats.AssetsPerSecond = Elapsed > 0.0 ? Stats.SpawnedCount / Elapsed : 0.0;

	if (bFinalizeWhenDrained && NumPendingEntries() == 0)
	{
		FinalizeGeneration();
	}
	return true;
}

void FAetherForgeSpawner::SpawnEntry(const FAetherForgeAssetEntry& Entry, FCategoryRuntime& Runtime, UWorld* World)
{
	double Z = 0.0;
	if (Entry.ground_snap)
	{
		if (!ResolveGroundZ(World, Entry.transform.location.x, Entry.transform.location.y, Z))
		{
			// Defined fallback when the trace finds nothing: skip + log.
			UE_LOG(LogAetherForgeSpawner, Warning,
				TEXT("Ground snap found no terrain under (%.1f, %.1f) for '%s'; entry skipped."),
				Entry.transform.location.x, Entry.transform.location.y, *Entry.category);
			++Stats.SkippedCount;
			return;
		}
	}

	if (Entry.ground_snap)
	{
		Z += BoundsGroundLift(Runtime.LoadedAsset.Get(), Entry.transform.scale);
	}

	// Wire transform is 2D location + yaw-only rotation + uniform scale; the plugin
	// owns Z (just resolved above).
	const FTransform Transform(
		FRotator(0.0, Entry.transform.yaw, 0.0),
		FVector(Entry.transform.location.x, Entry.transform.location.y, Z),
		FVector(Entry.transform.scale));

	if (Runtime.bInstanceable)
	{
		SpawnInstanced(Entry, Runtime, World, Transform);
	}
	else
	{
		SpawnAsActor(Entry, Runtime, World, Transform);
	}
}

void FAetherForgeSpawner::SpawnInstanced(const FAetherForgeAssetEntry& Entry, FCategoryRuntime& Runtime,
	UWorld* World, const FTransform& Transform)
{
	UStaticMesh* Mesh = Cast<UStaticMesh>(Runtime.LoadedAsset.Get());
	if (!Mesh)
	{
		UE_LOG(LogAetherForgeSpawner, Error,
			TEXT("Category '%s' is instanceable but its asset is not a UStaticMesh; entry skipped."),
			*Entry.category);
		++Stats.SkippedCount;
		return;
	}

	UHierarchicalInstancedStaticMeshComponent* Hism = GetOrCreateHism(Entry.category, Runtime, Mesh, World);
	if (!Hism)
	{
		++Stats.SkippedCount;
		return;
	}

	Hism->Modify(); // record in the open transaction so undo removes the instance
	Hism->AddInstance(Transform, /*bWorldSpace*/ true);
	++Stats.SpawnedCount;
}

void FAetherForgeSpawner::SpawnAsActor(const FAetherForgeAssetEntry& Entry, FCategoryRuntime& Runtime,
	UWorld* World, const FTransform& Transform)
{
	UObject* Asset = Runtime.LoadedAsset.Get();

	UClass* SpawnClass = nullptr;
	if (UClass* AsClass = Cast<UClass>(Asset))
	{
		SpawnClass = AsClass;
	}
	else if (const UBlueprint* AsBlueprint = Cast<UBlueprint>(Asset))
	{
		SpawnClass = AsBlueprint->GeneratedClass;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transactional; // participate in the open transaction
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Spawned = nullptr;
	if (SpawnClass && SpawnClass->IsChildOf(AActor::StaticClass()))
	{
		Spawned = World->SpawnActor(SpawnClass, &Transform, SpawnParams);
		if (Spawned && Entry.ground_snap)
		{
			// Class/Blueprint assets have unknown pivot conventions (BoundsGroundLift
			// returned 0): rest the actor's bounds bottom on the resolved ground Z.
			FVector Origin, Extent;
			Spawned->GetActorBounds(/*bOnlyCollidingComponents*/ false, Origin, Extent);
			if (Extent.Z > UE_KINDA_SMALL_NUMBER)
			{
				Spawned->AddActorWorldOffset(
					FVector(0.0, 0.0, Transform.GetLocation().Z - (Origin.Z - Extent.Z)));
			}
		}
	}
	else if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		// Non-instanceable static mesh: a plain StaticMeshActor.
		AStaticMeshActor* MeshActor = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), Transform, SpawnParams);
		if (MeshActor)
		{
			MeshActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Spawned = MeshActor;
		}
	}
	else
	{
		UE_LOG(LogAetherForgeSpawner, Error,
			TEXT("Category '%s': asset '%s' is neither an actor class/Blueprint nor a static mesh; entry skipped."),
			*Entry.category, Asset ? *Asset->GetName() : TEXT("<null>"));
		++Stats.SkippedCount;
		return;
	}

	if (!Spawned)
	{
		UE_LOG(LogAetherForgeSpawner, Error, TEXT("SpawnActor failed for category '%s'; entry skipped."),
			*Entry.category);
		++Stats.SkippedCount;
		return;
	}

	// generation_id tag + semantic tags, Outliner folder, readable label.
	Spawned->Tags.Add(ActiveGenerationTag);
	for (const FString& Tag : Entry.tags)
	{
		Spawned->Tags.AddUnique(FName(*Tag));
	}
	Spawned->SetActorLabel(FString::Printf(TEXT("%s_%d"), *Entry.category, Stats.SpawnedCount));
	Spawned->SetFolderPath(OutlinerFolderPath);

	++Stats.SpawnedCount;
}

void FAetherForgeSpawner::EndGeneration(const bool bDiscardPending)
{
	check(IsInGameThread());

	if (!IsGenerationActive())
	{
		return;
	}

	if (bDiscardPending)
	{
		// Cancel/error: drop everything not yet spawned and close the transaction now.
		// Already-spawned assets stay inside it — one Ctrl+Z removes the partial result.
		const int32 Dropped = NumPendingEntries();
		Stats.SkippedCount += Dropped;
		ReadyQueue.Reset();
		ReadyQueueHead = 0;
		for (TPair<FString, FCategoryRuntime>& Pair : Categories)
		{
			Pair.Value.WaitingEntries.Empty();
			if (Pair.Value.LoadHandle.IsValid() && !Pair.Value.LoadHandle->HasLoadCompleted())
			{
				Pair.Value.LoadHandle->CancelHandle();
			}
		}
		UE_LOG(LogAetherForgeSpawner, Log, TEXT("Generation '%s' ending early; %d pending entries dropped."),
			*ActiveGenerationId, Dropped);
		FinalizeGeneration();
	}
	else if (NumPendingEntries() == 0)
	{
		FinalizeGeneration();
	}
	else
	{
		// complete arrived while the queue is still draining under the budget; close
		// the transaction only after the last entry has spawned inside it.
		bFinalizeWhenDrained = true;
	}
}

void FAetherForgeSpawner::FinalizeGeneration()
{
	const double Elapsed = FPlatformTime::Seconds() - GenerationStartSeconds;
	UE_LOG(LogAetherForgeSpawner, Log,
		TEXT("Generation '%s' finalized: %d spawned, %d skipped in %.1f s (%.0f assets/s); worst spawn tick %.2f ms (budget %.1f ms)."),
		*ActiveGenerationId, Stats.SpawnedCount, Stats.SkippedCount, Elapsed,
		Stats.AssetsPerSecond, Stats.MaxSpawnTickMs, SpawnBudgetSeconds * 1000.0);

	ActiveTransaction.Reset(); // closes the scoped transaction => one undo step
	bFinalizeWhenDrained = false;
	Stats.PendingCount = 0;
	GenerationFinalizedEvent.Broadcast(Stats);

	// Release per-generation state. LoadHandles go with it; spawned actors/HISMs hold
	// their own references to the assets from here on.
	Categories.Reset();
	ReadyQueue.Reset();
	ReadyQueueHead = 0;
	ContainerActor.Reset();
	ActiveGenerationId.Reset();
	ActivePrompt.Reset();
	ActiveGenerationTag = NAME_None;
	OutlinerFolderPath = NAME_None;
}

bool FAetherForgeSpawner::ResolveGroundZ(UWorld* World, const double X, const double Y, double& OutZ) const
{
	const FVector Start(X, Y, GroundTraceHalfHeight);
	const FVector End(X, Y, -GroundTraceHalfHeight);

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(AetherForgeGroundSnap), /*bTraceComplex*/ true);
	if (const AActor* Container = ContainerActor.Get())
	{
		QueryParams.AddIgnoredActor(Container);
	}

	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, QueryParams))
	{
		OutZ = Hit.ImpactPoint.Z;
		return true;
	}
	return false;
}

double FAetherForgeSpawner::BoundsGroundLift(const UObject* Asset, const double Scale)
{
	if (const UStaticMesh* Mesh = Cast<const UStaticMesh>(Asset))
	{
		const double MinZ = Mesh->GetBoundingBox().Min.Z;
		return MinZ < 0.0 ? -MinZ * Scale : 0.0;
	}
	return 0.0;
}

UHierarchicalInstancedStaticMeshComponent* FAetherForgeSpawner::GetOrCreateHism(const FString& CategoryKey,
	FCategoryRuntime& Runtime, UStaticMesh* Mesh, UWorld* World)
{
	if (UHierarchicalInstancedStaticMeshComponent* Existing = Runtime.Hism.Get())
	{
		return Existing;
	}

	AActor* Container = GetOrCreateContainerActor(World);
	if (!Container)
	{
		return nullptr;
	}

	Container->Modify();

	UHierarchicalInstancedStaticMeshComponent* Hism = NewObject<UHierarchicalInstancedStaticMeshComponent>(
		Container,
		MakeUniqueObjectName(Container, UHierarchicalInstancedStaticMeshComponent::StaticClass(),
			FName(*FString::Printf(TEXT("HISM_%s"), *CategoryKey))),
		RF_Transactional);
	Hism->SetStaticMesh(Mesh);
	Hism->SetMobility(EComponentMobility::Static);
	Hism->ComponentTags.Add(ActiveGenerationTag);
	Hism->ComponentTags.Add(FName(*CategoryKey));

	Container->AddInstanceComponent(Hism);
	Hism->AttachToComponent(Container->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	Hism->RegisterComponent();

	Runtime.Hism = Hism;
	return Hism;
}

AActor* FAetherForgeSpawner::GetOrCreateContainerActor(UWorld* World)
{
	if (AActor* Existing = ContainerActor.Get())
	{
		return Existing;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transactional;
	SpawnParams.Name = MakeUniqueObjectName(World->GetCurrentLevel(), AActor::StaticClass(),
		FName(*FString::Printf(TEXT("AetherForge_%s"), *ActiveGenerationId)));

	AActor* Container = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!Container)
	{
		UE_LOG(LogAetherForgeSpawner, Error, TEXT("Failed to spawn the generation container actor."));
		return nullptr;
	}

	USceneComponent* Root = NewObject<USceneComponent>(Container, TEXT("Root"), RF_Transactional);
	Container->SetRootComponent(Root);
	Container->AddInstanceComponent(Root);
	Root->RegisterComponent();

	Container->Tags.Add(ActiveGenerationTag);
	Container->SetActorLabel(FString::Printf(TEXT("AetherForge %s"), *ActiveGenerationId));
	Container->SetFolderPath(OutlinerFolderPath);

	ContainerActor = Container;
	return Container;
}

UWorld* FAetherForgeSpawner::GetEditorWorld() const
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

int32 FAetherForgeSpawner::NumPendingEntries() const
{
	int32 Pending = ReadyQueue.Num() - ReadyQueueHead;
	for (const TPair<FString, FCategoryRuntime>& Pair : Categories)
	{
		Pending += Pair.Value.WaitingEntries.Num();
	}
	return Pending;
}

FString FAetherForgeSpawner::SanitizeForOutlinerFolder(const FString& Prompt)
{
	// Outliner folder segment from the prompt: keep it short and path-safe.
	FString Result;
	Result.Reserve(40);
	for (const TCHAR Char : Prompt)
	{
		if (Result.Len() >= 40)
		{
			break;
		}
		if (FChar::IsAlnum(Char) || Char == TEXT(' ') || Char == TEXT('-') || Char == TEXT('_'))
		{
			Result.AppendChar(Char);
		}
	}
	Result.TrimStartAndEndInline();
	return Result.IsEmpty() ? TEXT("Generation") : Result;
}

#undef LOCTEXT_NAMESPACE
