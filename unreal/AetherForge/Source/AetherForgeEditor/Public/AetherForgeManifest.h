// Copyright AetherForge. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AetherForgeProtocolTypes.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPath.h"
#include "AetherForgeManifest.generated.h"

/**
 * One spawnable asset bound to a semantic category key.
 * The metadata (footprint radius, ground snap, instanceable, display name) is what
 * crosses the wire at hello; the AssetPath never leaves the editor process.
 */
USTRUCT(BlueprintType)
struct FAetherForgeManifestAssetEntry
{
	GENERATED_BODY()

	/**
	 * Asset to spawn for this category. UStaticMesh for scatter (instanceable)
	 * categories; a Blueprint (or actor class) for interactive ones such as creatures.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AetherForge")
	FSoftObjectPath AssetPath;

	/**
	 * Collision footprint radius in world units (cm). Used by the backend's
	 * placement engine for spatial-hash overlap rejection — no two footprints overlap.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AetherForge", meta = (ClampMin = "0.0", Units = "cm"))
	float FootprintRadius = 100.0f;

	/**
	 * true => spawned into a per-category HierarchicalInstancedStaticMeshComponent pool
	 * (requires AssetPath to be a UStaticMesh); false => SpawnActor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AetherForge")
	bool bInstanceable = true;

	/** true => spawned entries line-trace down to the terrain to find Z. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AetherForge")
	bool bGroundSnap = true;

	/** Human-readable name, used in UI/logging and advertised to the backend. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AetherForge")
	FString DisplayName;
};

/**
 * The plugin's asset catalog: semantic category key (e.g. "deciduous_tree") -> asset +
 * metadata. Advertised to the Go sidecar at hello; the LLM may only ever select from
 * these keys, and the plugin resolves them back to assets at spawn time.
 *
 * Project teams extend this data asset to expose their own asset libraries — no code
 * changes required. The module looks for the default instance at
 * /AetherForge/DA_AetherForgeManifest (see FAetherForgeEditorModule::GetOrLoadManifest).
 */
UCLASS(BlueprintType)
class AETHERFORGEEDITOR_API UAetherForgeManifest : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Semantic category key -> asset binding. Keys are lower_snake_case by convention. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AetherForge")
	TMap<FString, FAetherForgeManifestAssetEntry> Entries;

	/** Project the metadata (and only the metadata) into the wire form sent at hello. */
	TMap<FString, FAetherForgeWireManifestEntry> BuildWireManifest() const;

	/** Find the entry for a semantic key; nullptr if the category is unknown. */
	const FAetherForgeManifestAssetEntry* FindEntry(const FString& CategoryKey) const;
};
