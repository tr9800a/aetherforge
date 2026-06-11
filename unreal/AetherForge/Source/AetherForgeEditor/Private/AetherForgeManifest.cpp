// Copyright AetherForge. All Rights Reserved.

#include "AetherForgeManifest.h"

TMap<FString, FAetherForgeWireManifestEntry> UAetherForgeManifest::BuildWireManifest() const
{
	TMap<FString, FAetherForgeWireManifestEntry> WireManifest;
	WireManifest.Reserve(Entries.Num());

	for (const TPair<FString, FAetherForgeManifestAssetEntry>& Pair : Entries)
	{
		FAetherForgeWireManifestEntry WireEntry;
		WireEntry.footprint_radius = Pair.Value.FootprintRadius;
		WireEntry.ground_snap = Pair.Value.bGroundSnap;
		WireEntry.instanceable = Pair.Value.bInstanceable;
		WireEntry.display_name = Pair.Value.DisplayName;
		WireManifest.Add(Pair.Key, MoveTemp(WireEntry));
	}

	return WireManifest;
}

const FAetherForgeManifestAssetEntry* UAetherForgeManifest::FindEntry(const FString& CategoryKey) const
{
	return Entries.Find(CategoryKey);
}
