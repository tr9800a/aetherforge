// Copyright AetherForge. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AetherForgeProtocolTypes.generated.h"

/**
 * AetherForge wire protocol (v1) — C++ mirror of contract/README.md.
 *
 * These USTRUCTs are FJsonObjectConverter-compatible. NAMING NOTE: UPROPERTY names
 * below intentionally use snake_case so they match the wire contract byte-for-byte:
 *  - On export, FJsonObjectConverter::StandardizeCase only lowercases the first
 *    character of the property name, so `protocol_version` serializes as
 *    "protocol_version".
 *  - On import, JSON keys are matched to FProperties by FName, which is
 *    case-insensitive, so "protocol_version" finds `protocol_version`.
 * This is a deliberate deviation from UE property naming conventions, scoped to this
 * header only; the contract (and the shared fixtures consumed by both the Go and the
 * UE Automation test suites) is the source of truth.
 */

namespace AetherForgeProtocol
{
	/** Current protocol version; validated at hello, mismatch fails loudly. */
	constexpr int32 Version = 1;

	// Message type discriminators (the `type` field).
	inline const TCHAR* const Type_Hello = TEXT("hello");
	inline const TCHAR* const Type_Generate = TEXT("generate");
	inline const TCHAR* const Type_Plan = TEXT("plan");
	inline const TCHAR* const Type_Chunk = TEXT("chunk");
	inline const TCHAR* const Type_Complete = TEXT("complete");
	inline const TCHAR* const Type_Error = TEXT("error");
	inline const TCHAR* const Type_Cancel = TEXT("cancel");
}

/** 2D point in Unreal world units (cm). The wire never carries Z — the plugin owns Z. */
USTRUCT()
struct FAetherForgeWireVec2
{
	GENERATED_BODY()

	UPROPERTY()
	double x = 0.0;

	UPROPERTY()
	double y = 0.0;
};

/** 2D generation area, `generate.bounds`. */
USTRUCT()
struct FAetherForgeWireBounds
{
	GENERATED_BODY()

	UPROPERTY()
	FAetherForgeWireVec2 min;

	UPROPERTY()
	FAetherForgeWireVec2 max;
};

/**
 * Wire transform: 2D location + yaw-only rotation (degrees) + uniform scale.
 * No z, no pitch/roll, no per-axis scale — by contract.
 */
USTRUCT()
struct FAetherForgeWireTransform
{
	GENERATED_BODY()

	UPROPERTY()
	FAetherForgeWireVec2 location;

	UPROPERTY()
	double yaw = 0.0;

	UPROPERTY()
	double scale = 1.0;
};

/** One element of `chunk.assets`. Semantic category only — never an asset path. */
USTRUCT()
struct FAetherForgeAssetEntry
{
	GENERATED_BODY()

	/** Semantic key; must exist in the manifest advertised at hello. */
	UPROPERTY()
	FString category;

	UPROPERTY()
	FAetherForgeWireTransform transform;

	/** true => line-trace to terrain at spawn time to find Z. */
	UPROPERTY()
	bool ground_snap = false;

	/** Semantic tags (e.g. "flora", "canopy"); may be empty. */
	UPROPERTY()
	TArray<FString> tags;
};

/** Values of `hello.manifest` — metadata only; asset paths never cross the wire. */
USTRUCT()
struct FAetherForgeWireManifestEntry
{
	GENERATED_BODY()

	/** Collision footprint radius in world units (cm), for placement overlap rejection. */
	UPROPERTY()
	double footprint_radius = 0.0;

	UPROPERTY()
	bool ground_snap = false;

	/** true => HISM pool; false => SpawnActor (interactive categories). */
	UPROPERTY()
	bool instanceable = false;

	UPROPERTY()
	FString display_name;
};

/** client -> server: manifest handshake. Sent exactly once per connection, first. */
USTRUCT()
struct FAetherForgeHelloMessage
{
	GENERATED_BODY()

	UPROPERTY()
	int32 protocol_version = AetherForgeProtocol::Version;

	UPROPERTY()
	FString type = AetherForgeProtocol::Type_Hello;

	UPROPERTY()
	TMap<FString, FAetherForgeWireManifestEntry> manifest;
};

/**
 * client -> server: start a generation.
 * NOTE: `seed` is optional on the wire. FJsonObjectConverter cannot omit fields, so
 * outbound serialization goes through FAetherForgeProtocol::SerializeGenerate(), which
 * drops `seed` when unset. This struct is still importable for fixture round-trips.
 */
USTRUCT()
struct FAetherForgeGenerateMessage
{
	GENERATED_BODY()

	UPROPERTY()
	int32 protocol_version = AetherForgeProtocol::Version;

	UPROPERTY()
	FString type = AetherForgeProtocol::Type_Generate;

	UPROPERTY()
	FString generation_id;

	UPROPERTY()
	FString prompt;

	UPROPERTY()
	int64 seed = 0;

	UPROPERTY()
	FAetherForgeWireBounds bounds;
};

/** server -> client: arrives once, before the first chunk; drives the progress bar. */
USTRUCT()
struct FAetherForgePlanMessage
{
	GENERATED_BODY()

	UPROPERTY()
	int32 protocol_version = 0;

	UPROPERTY()
	FString type;

	UPROPERTY()
	FString generation_id;

	/** Seed in effect (echo of the request seed, or server-chosen). Always present on plan. */
	UPROPERTY()
	int64 seed = 0;

	UPROPERTY()
	int32 total_assets = 0;

	/** Semantic key -> count; counts sum to total_assets. */
	UPROPERTY()
	TMap<FString, int32> categories;
};

/** server -> client: a batch (~50) of placed assets; repeated. */
USTRUCT()
struct FAetherForgeChunkMessage
{
	GENERATED_BODY()

	UPROPERTY()
	int32 protocol_version = 0;

	UPROPERTY()
	FString type;

	UPROPERTY()
	FString generation_id;

	UPROPERTY()
	TArray<FAetherForgeAssetEntry> assets;
};

/** `complete.stats`. */
USTRUCT()
struct FAetherForgeCompleteStats
{
	GENERATED_BODY()

	UPROPERTY()
	int32 assets = 0;

	UPROPERTY()
	int64 elapsed_ms = 0;

	UPROPERTY()
	int64 llm_ms = 0;
};

/** server -> client: terminal message for a successful generation. */
USTRUCT()
struct FAetherForgeCompleteMessage
{
	GENERATED_BODY()

	UPROPERTY()
	int32 protocol_version = 0;

	UPROPERTY()
	FString type;

	UPROPERTY()
	FString generation_id;

	UPROPERTY()
	FAetherForgeCompleteStats stats;
};

/** server -> client: terminal message for a failed generation (or handshake failure). */
USTRUCT()
struct FAetherForgeErrorMessage
{
	GENERATED_BODY()

	UPROPERTY()
	int32 protocol_version = 0;

	UPROPERTY()
	FString type;

	/** Empty string for pre-generation errors (e.g. protocol_version_mismatch at hello). */
	UPROPERTY()
	FString generation_id;

	/** e.g. protocol_version_mismatch, llm_invalid_output, llm_timeout, unknown_category, internal. */
	UPROPERTY()
	FString code;

	UPROPERTY()
	FString message;

	/** true => connection stays usable; false => the server closes the connection. */
	UPROPERTY()
	bool recoverable = false;
};

/** client -> server: cancel an in-flight generation. */
USTRUCT()
struct FAetherForgeCancelMessage
{
	GENERATED_BODY()

	UPROPERTY()
	int32 protocol_version = AetherForgeProtocol::Version;

	UPROPERTY()
	FString type = AetherForgeProtocol::Type_Cancel;

	UPROPERTY()
	FString generation_id;
};

/** Discriminator for parsed inbound (server -> client) messages. */
enum class EAetherForgeServerMessageType : uint8
{
	Plan,
	Chunk,
	Complete,
	Error,
	Invalid,
};

/**
 * Tagged union of all server -> client messages. Produced by
 * FAetherForgeProtocol::ParseServerMessage on a background thread, consumed on the
 * game thread. Plain struct (not a USTRUCT) — never serialized itself.
 */
struct FAetherForgeServerMessage
{
	EAetherForgeServerMessageType Type = EAetherForgeServerMessageType::Invalid;

	FAetherForgePlanMessage Plan;
	FAetherForgeChunkMessage Chunk;
	FAetherForgeCompleteMessage Complete;
	FAetherForgeErrorMessage Error;

	/** Set when Type == Invalid: why parsing/validation failed (surfaced to the status log). */
	FString ParseError;
};

/** Serialization/parsing entry points for the v1 protocol. */
struct AETHERFORGEEDITOR_API FAetherForgeProtocol
{
	/** Build the hello message JSON from the wire manifest. */
	static FString SerializeHello(const TMap<FString, FAetherForgeWireManifestEntry>& Manifest);

	/**
	 * Build the generate message JSON. Seed is omitted from the payload when unset
	 * (the server then picks one and echoes it in plan).
	 * Note: JSON numbers limit exact integer seeds to +/- 2^53; ample for v1.
	 */
	static FString SerializeGenerate(const FString& GenerationId, const FString& Prompt,
		const TOptional<int64>& Seed, const FAetherForgeWireBounds& Bounds);

	/** Build the cancel message JSON. */
	static FString SerializeCancel(const FString& GenerationId);

	/**
	 * Strict parse of one inbound frame. Unknown protocol_version, unknown type,
	 * malformed payloads, or unknown/forbidden fields (e.g. an extra `z` on a
	 * location, pitch/roll on a transform, per-axis scale — contract violations)
	 * yield Type == Invalid with ParseError set — never silently coerced or
	 * dropped. Safe to call off the game thread (no UObject allocation).
	 * Returns false iff the result is Invalid.
	 */
	static bool ParseServerMessage(const FString& JsonText, FAetherForgeServerMessage& OutMessage);

	/**
	 * Semantic-categories rule: every asset entry in a chunk must reference a
	 * category present in the manifest advertised at hello. Returns false with
	 * OutError naming the first unknown category (a contract violation).
	 */
	static bool ValidateChunkCategories(const FAetherForgeChunkMessage& Chunk,
		const TSet<FString>& ManifestCategories, FString& OutError);
};
