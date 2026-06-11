// Copyright AetherForge. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AetherForgeProtocolTypes.h"
#include "Containers/Ticker.h"
#include "Engine/StreamableManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class FScopedTransaction;
class UAetherForgeManifest;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;
class UWorld;

/** Live spawn statistics for the panel's stats line. */
struct FAetherForgeSpawnStats
{
	int32 SpawnedCount = 0;
	int32 SkippedCount = 0;
	/** Entries queued (ready + waiting on asset loads), not yet spawned. */
	int32 PendingCount = 0;
	double AssetsPerSecond = 0.0;
	double LastSpawnTickMs = 0.0;
};

/**
 * Time-budgeted spawn queue (Phase 3 skeleton).
 *
 * Performance contract: the editor viewport must never hitch. Each editor tick the
 * queue is drained only until ~2 ms (FPlatformTime) has been spent — budget-by-time,
 * never fixed-count — so a 1,000-asset generation streams in over multiple frames.
 *
 * - Assets are async-preloaded via FStreamableManager on first sight of a category;
 *   entries become spawn-eligible only once their asset has resolved. Never a
 *   synchronous load in the spawn tick.
 * - Instanceable categories go into per-category HISM pools on a per-generation
 *   container actor; others use SpawnActor.
 * - ground_snap entries are line-traced down to the terrain to find Z (the wire is
 *   2D — the plugin owns Z). No hit => skip + log.
 * - One FScopedTransaction spans the whole generation: Ctrl+Z removes everything
 *   (HISM instances and actors) atomically, including cancelled partial results.
 * - Everything is tagged with the generation_id and grouped in a World Outliner
 *   folder named for the prompt.
 *
 * Game-thread only.
 */
class FAetherForgeSpawner : public TSharedFromThis<FAetherForgeSpawner>
{
public:
	/** ~2 ms per editor tick (spec sections 1 and 6). */
	static constexpr double SpawnBudgetSeconds = 0.002;

	~FAetherForgeSpawner();

	/** Register the editor-tick drain loop. Call once after construction. */
	void Initialize();

	/** Finalize any open generation and unregister the ticker. */
	void Shutdown();

	bool IsGenerationActive() const { return ActiveTransaction.IsValid(); }

	/**
	 * Open a generation: starts the transaction, snapshots the manifest categories,
	 * and derives the Outliner folder from the prompt. Implicitly finalizes any
	 * generation still open (one generation at a time).
	 */
	void BeginGeneration(const FString& GenerationId, const FString& Prompt, const UAetherForgeManifest* Manifest);

	/**
	 * Queue a chunk's entries. Unknown categories are skipped with a log (the backend
	 * validates against the hello manifest, so this indicates contract drift).
	 */
	void EnqueueEntries(const TArray<FAetherForgeAssetEntry>& Entries);

	/**
	 * Close the generation.
	 * - bDiscardPending = false (complete): pending entries keep draining under the
	 *   budget; the transaction closes once the queue is empty.
	 * - bDiscardPending = true (cancel/error): pending entries are dropped and the
	 *   transaction closes now — already-spawned assets stay inside it, so a cancelled
	 *   partial result is still removed atomically by a single Ctrl+Z.
	 */
	void EndGeneration(bool bDiscardPending);

	const FAetherForgeSpawnStats& GetStats() const { return Stats; }

private:
	/** Per-category state for the active generation. */
	struct FCategoryRuntime
	{
		FSoftObjectPath AssetPath;
		bool bInstanceable = true;
		bool bGroundSnap = true;
		bool bLoadRequested = false;
		bool bLoadFailed = false;
		/** Keeps a hard reference on the loaded asset for the generation's lifetime. */
		TSharedPtr<FStreamableHandle> LoadHandle;
		TWeakObjectPtr<UObject> LoadedAsset;
		TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> Hism;
		/** Entries received before the category's asset finished loading. */
		TArray<FAetherForgeAssetEntry> WaitingEntries;
	};

	/** Editor tick: drain ReadyQueue until the time budget is spent. */
	bool SpawnTick(float DeltaTime);

	void SpawnEntry(const FAetherForgeAssetEntry& Entry, FCategoryRuntime& Runtime, UWorld* World);
	void SpawnInstanced(const FAetherForgeAssetEntry& Entry, FCategoryRuntime& Runtime, UWorld* World,
		const FTransform& Transform);
	void SpawnAsActor(const FAetherForgeAssetEntry& Entry, FCategoryRuntime& Runtime, UWorld* World,
		const FTransform& Transform);

	/** FStreamableManager completion callback (game thread). */
	void OnCategoryAssetLoaded(FString CategoryKey);

	/** Closes the transaction and releases per-generation state. */
	void FinalizeGeneration();

	/** Line trace straight down to find terrain Z. Returns false on no hit (=> skip + log). */
	bool ResolveGroundZ(UWorld* World, double X, double Y, double& OutZ) const;

	UHierarchicalInstancedStaticMeshComponent* GetOrCreateHism(const FString& CategoryKey,
		FCategoryRuntime& Runtime, UStaticMesh* Mesh, UWorld* World);
	AActor* GetOrCreateContainerActor(UWorld* World);
	UWorld* GetEditorWorld() const;
	int32 NumPendingEntries() const;

	static FString SanitizeForOutlinerFolder(const FString& Prompt);

	FStreamableManager Streamable;
	FTSTicker::FDelegateHandle TickerHandle;

	/**
	 * Held open across editor ticks while the generation streams in, then closed by
	 * FinalizeGeneration so the whole generation is one undo step. NOTE (named plan
	 * risk): a multi-frame FScopedTransaction is the riskiest assumption of Phase 3 —
	 * if it misbehaves with interleaved editor transactions, switch to an explicit
	 * GEditor->BeginTransaction()/EndTransaction() pair. Verify early.
	 */
	TUniquePtr<FScopedTransaction> ActiveTransaction;

	FString ActiveGenerationId;
	FName ActiveGenerationTag;
	FName OutlinerFolderPath;
	FString ActivePrompt;
	bool bFinalizeWhenDrained = false;

	/** Per-generation container actor hosting the HISM pools. */
	TWeakObjectPtr<AActor> ContainerActor;

	TMap<FString, FCategoryRuntime> Categories;

	/** Spawn-eligible entries, FIFO (Head avoids O(n) RemoveAt(0)). Game thread only. */
	TArray<FAetherForgeAssetEntry> ReadyQueue;
	int32 ReadyQueueHead = 0;

	FAetherForgeSpawnStats Stats;
	double GenerationStartSeconds = 0.0;
};
