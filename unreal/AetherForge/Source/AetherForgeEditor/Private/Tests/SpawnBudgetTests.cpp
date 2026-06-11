// Copyright AetherForge. All Rights Reserved.

// Phase 3 acceptance test (spec section 8): 1,000 mock assets stream through the
// real time-budgeted spawner with no spawn tick over budget, and one Ctrl+Z
// (UndoTransaction) removes the entire generation. Run headless with:
//   UnrealEditor-Cmd <Project>.uproject -ExecCmds="Automation RunTests AetherForge.Spawner; Quit" \
//       -unattended -nopause -nullrhi -nosplash -log

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AetherForgeEditorModule.h"
#include "AetherForgeManifest.h"
#include "AetherForgeProtocolTypes.h"
#include "Spawn/AetherForgeSpawner.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"

namespace AetherForgeSpawnerTests
{
	constexpr int32 TotalEntries = 1000;
	constexpr int32 ActorEntries = 100; // non-instanceable share (SpawnActor path)
	const FString GenerationId = TEXT("gen-spawn-budget-test");

	/**
	 * Worst-tolerated spawn tick. The budget is 2 ms; the last entry started inside
	 * the budget may overshoot it, and CI machines are noisy — but anything past
	 * 8 ms means budgeting is broken (the level acceptance criterion is a 16 ms
	 * frame, and spawning must stay a fraction of the frame).
	 */
	constexpr double MaxToleratedTickMs = 8.0;

	int32 CountTaggedActors(UWorld* World)
	{
		const FName Tag(*GenerationId);
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->Tags.Contains(Tag))
			{
				++Count;
			}
		}
		return Count;
	}

	UAetherForgeManifest* MakeTestManifest()
	{
		UAetherForgeManifest* Manifest = NewObject<UAetherForgeManifest>(GetTransientPackage());

		// ground_snap=false everywhere: the test level is empty, and Z resolution is
		// not what this test measures (the wire is 2D; Z falls back to 0).
		FAetherForgeManifestAssetEntry Cube;
		Cube.AssetPath = FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube"));
		Cube.FootprintRadius = 50.0f;
		Cube.bInstanceable = true;
		Cube.bGroundSnap = false;
		Cube.DisplayName = TEXT("Budget Cube (HISM path)");
		Manifest->Entries.Add(TEXT("budget_cube"), Cube);

		FAetherForgeManifestAssetEntry Sphere;
		Sphere.AssetPath = FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		Sphere.FootprintRadius = 50.0f;
		Sphere.bInstanceable = false; // exercises the SpawnActor (AStaticMeshActor) path
		Sphere.bGroundSnap = false;
		Sphere.DisplayName = TEXT("Budget Sphere (actor path)");
		Manifest->Entries.Add(TEXT("budget_sphere"), Sphere);

		return Manifest;
	}

	TArray<FAetherForgeAssetEntry> MakeEntries()
	{
		TArray<FAetherForgeAssetEntry> Entries;
		Entries.Reserve(TotalEntries);
		for (int32 i = 0; i < TotalEntries; ++i)
		{
			FAetherForgeAssetEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.category = (i < ActorEntries) ? TEXT("budget_sphere") : TEXT("budget_cube");
			Entry.transform.location.x = (i % 32) * 200.0;
			Entry.transform.location.y = (i / 32) * 200.0;
			Entry.transform.yaw = (i * 7) % 360;
			Entry.transform.scale = 1.0;
			Entry.ground_snap = false;
			Entry.tags = { TEXT("budget_test") };
		}
		return Entries;
	}
}

/**
 * Latent command: waits (across real editor ticks, which is what drives SpawnTick)
 * until the generation finalizes, then runs the assertions and the undo check.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FAetherForgeWaitForSpawnDrain,
	FAutomationTestBase*, Test, double, StartSeconds);

bool FAetherForgeWaitForSpawnDrain::Update()
{
	using namespace AetherForgeSpawnerTests;

	const TSharedPtr<FAetherForgeSpawner> Spawner = FAetherForgeEditorModule::Get().GetSpawner();
	if (!Spawner.IsValid())
	{
		Test->AddError(TEXT("Spawner disappeared while draining."));
		return true;
	}

	if (Spawner->IsGenerationActive())
	{
		if (FPlatformTime::Seconds() - StartSeconds > 120.0)
		{
			Test->AddError(FString::Printf(TEXT("Timed out draining the spawn queue (%d still pending)."),
				Spawner->GetStats().PendingCount));
			Spawner->EndGeneration(/*bDiscardPending*/ true);
			return true;
		}
		return false; // keep waiting; SpawnTick drains a budget's worth per editor tick
	}

	// Generation finalized — stats survive until the next BeginGeneration.
	const FAetherForgeSpawnStats& Stats = Spawner->GetStats();
	Test->TestEqual(TEXT("All entries spawned"), Stats.SpawnedCount, TotalEntries);
	Test->TestEqual(TEXT("No entries skipped"), Stats.SkippedCount, 0);
	Test->TestEqual(TEXT("Queue fully drained"), Stats.PendingCount, 0);

	if (Stats.MaxSpawnTickMs > MaxToleratedTickMs)
	{
		Test->AddError(FString::Printf(
			TEXT("Spawn budget broken: worst tick %.2f ms exceeds %.1f ms tolerance (budget %.1f ms)."),
			Stats.MaxSpawnTickMs, MaxToleratedTickMs, FAetherForgeSpawner::SpawnBudgetSeconds * 1000.0));
	}
	else
	{
		Test->AddInfo(FString::Printf(TEXT("Worst spawn tick: %.2f ms (budget %.1f ms)."),
			Stats.MaxSpawnTickMs, FAetherForgeSpawner::SpawnBudgetSeconds * 1000.0));
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		Test->AddError(TEXT("No editor world for the undo check."));
		return true;
	}

	// The SpawnActor path must have produced tagged actors (container + spheres);
	// then ONE undo must remove the entire generation atomically. This is the named
	// Phase 3 risk: the transaction stays open across many editor ticks.
	const int32 TaggedBefore = CountTaggedActors(World);
	if (TaggedBefore < ActorEntries + 1)
	{
		Test->AddError(FString::Printf(TEXT("Expected at least %d tagged actors before undo, found %d."),
			ActorEntries + 1, TaggedBefore));
	}

	GEditor->UndoTransaction();

	const int32 TaggedAfter = CountTaggedActors(World);
	Test->TestEqual(TEXT("Single undo removes the whole generation"), TaggedAfter, 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAetherForgeSpawnBudgetTest,
	"AetherForge.Spawner.SpawnBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAetherForgeSpawnBudgetTest::RunTest(const FString& Parameters)
{
	using namespace AetherForgeSpawnerTests;

	const TSharedPtr<FAetherForgeSpawner> Spawner = FAetherForgeEditorModule::Get().GetSpawner();
	if (!TestTrue(TEXT("Module spawner exists"), Spawner.IsValid()))
	{
		return false;
	}

	UAetherForgeManifest* Manifest = MakeTestManifest();
	Spawner->BeginGeneration(GenerationId, TEXT("spawn budget acceptance test"), Manifest);
	Spawner->EnqueueEntries(MakeEntries());
	// Complete semantics: keep draining under the budget, finalize when empty.
	Spawner->EndGeneration(/*bDiscardPending*/ false);

	ADD_LATENT_AUTOMATION_COMMAND(FAetherForgeWaitForSpawnDrain(this, FPlatformTime::Seconds()));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
