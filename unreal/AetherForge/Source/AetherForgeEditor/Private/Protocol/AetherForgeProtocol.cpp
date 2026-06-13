// Copyright AetherForge. All Rights Reserved.

#include "AetherForgeProtocolTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** One JSON object per WebSocket text frame, condensed (no pretty-printing). */
	FString CondensedJsonString(const TSharedRef<FJsonObject>& JsonObject)
	{
		FString Result;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
		FJsonSerializer::Serialize(JsonObject, Writer);
		return Result;
	}

	bool MakeInvalid(FAetherForgeServerMessage& OutMessage, FString&& Reason)
	{
		OutMessage.Type = EAetherForgeServerMessageType::Invalid;
		OutMessage.ParseError = MoveTemp(Reason);
		return false;
	}

	/**
	 * Recursively finds the first field in Incoming with no counterpart in Known
	 * (the JSON re-exported from the imported USTRUCT). FJsonObjectConverter
	 * silently ignores unknown JSON keys on import; the contract forbids them
	 * (an extra `z`, pitch/roll, per-axis scale are violations, not noise), so
	 * any such key is detected here and rejected. Returns true with the dotted
	 * path of the offender in OutOffendingPath.
	 */
	bool FindUnknownField(const TSharedPtr<FJsonObject>& Incoming, const TSharedPtr<FJsonObject>& Known,
		const FString& Path, FString& OutOffendingPath)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Incoming->Values)
		{
			const FString FieldPath = Path.IsEmpty() ? Pair.Key : Path + TEXT(".") + Pair.Key;
			const TSharedPtr<FJsonValue> KnownValue = Known->TryGetField(Pair.Key);
			if (!KnownValue.IsValid())
			{
				OutOffendingPath = FieldPath;
				return true;
			}
			if (!Pair.Value.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* IncomingObject = nullptr;
			const TSharedPtr<FJsonObject>* KnownObject = nullptr;
			if (Pair.Value->TryGetObject(IncomingObject) && KnownValue->TryGetObject(KnownObject))
			{
				if (FindUnknownField(*IncomingObject, *KnownObject, FieldPath, OutOffendingPath))
				{
					return true;
				}
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* IncomingArray = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* KnownArray = nullptr;
			if (Pair.Value->TryGetArray(IncomingArray) && KnownValue->TryGetArray(KnownArray))
			{
				const int32 Count = FMath::Min(IncomingArray->Num(), KnownArray->Num());
				for (int32 Index = 0; Index < Count; ++Index)
				{
					const TSharedPtr<FJsonObject>* IncomingElement = nullptr;
					const TSharedPtr<FJsonObject>* KnownElement = nullptr;
					if ((*IncomingArray)[Index].IsValid() && (*KnownArray)[Index].IsValid() &&
						(*IncomingArray)[Index]->TryGetObject(IncomingElement) &&
						(*KnownArray)[Index]->TryGetObject(KnownElement))
					{
						if (FindUnknownField(*IncomingElement, *KnownElement,
								FString::Printf(TEXT("%s[%d]"), *FieldPath, Index), OutOffendingPath))
						{
							return true;
						}
					}
				}
			}
		}
		return false;
	}

	/**
	 * JsonObjectToUStruct, then re-export the struct and reject any incoming key
	 * the struct does not model (strict parse — the mandated rejection of
	 * forbidden asset-entry fields lives here).
	 */
	template <typename TMessage>
	bool StrictDeserialize(const TSharedRef<FJsonObject>& Root, TMessage* OutStruct,
		const TCHAR* TypeName, FString& OutError)
	{
		if (!FJsonObjectConverter::JsonObjectToUStruct(Root, OutStruct, 0, 0))
		{
			OutError = FString::Printf(TEXT("Failed to deserialize '%s' message"), TypeName);
			return false;
		}
		const TSharedPtr<FJsonObject> Known = FJsonObjectConverter::UStructToJsonObject(*OutStruct);
		if (!Known.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to re-export '%s' message for strict validation"), TypeName);
			return false;
		}
		FString OffendingPath;
		if (FindUnknownField(Root, Known, FString(), OffendingPath))
		{
			OutError = FString::Printf(
				TEXT("'%s' message carries unknown/forbidden field '%s' (contract violation)"),
				TypeName, *OffendingPath);
			return false;
		}
		return true;
	}
}

FString FAetherForgeProtocol::SerializeHello(const TMap<FString, FAetherForgeWireManifestEntry>& Manifest)
{
	FAetherForgeHelloMessage Message;
	Message.protocol_version = AetherForgeProtocol::Version;
	Message.type = AetherForgeProtocol::Type_Hello;
	Message.manifest = Manifest;

	FString Result;
	FJsonObjectConverter::UStructToJsonObjectString(Message, Result, /*CheckFlags*/ 0, /*SkipFlags*/ 0,
		/*Indent*/ 0, /*ExportCb*/ nullptr, /*bPrettyPrint*/ false);
	return Result;
}

FString FAetherForgeProtocol::SerializeGenerate(const FString& GenerationId, const FString& Prompt,
	const TOptional<int64>& Seed, const FAetherForgeWireBounds& Bounds)
{
	// Built by hand (not via FJsonObjectConverter) so that an unset seed is genuinely
	// omitted from the payload, per the contract ("seed is optional on generate").
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("protocol_version"), AetherForgeProtocol::Version);
	Root->SetStringField(TEXT("type"), AetherForgeProtocol::Type_Generate);
	Root->SetStringField(TEXT("generation_id"), GenerationId);
	Root->SetStringField(TEXT("prompt"), Prompt);
	if (Seed.IsSet())
	{
		// Write the seed as an exact integer literal, NOT via SetNumberField (which
		// stores a double and serializes large values in scientific notation that the
		// Go server cannot unmarshal into int64). FJsonValueNumberString emits the
		// digits verbatim as an unquoted JSON number, lossless for the full int64 range.
		Root->SetField(TEXT("seed"), MakeShared<FJsonValueNumberString>(LexToString(Seed.GetValue())));
	}

	const TSharedPtr<FJsonObject> BoundsObject = FJsonObjectConverter::UStructToJsonObject(Bounds);
	check(BoundsObject.IsValid());
	Root->SetObjectField(TEXT("bounds"), BoundsObject);

	return CondensedJsonString(Root);
}

FString FAetherForgeProtocol::SerializeCancel(const FString& GenerationId)
{
	FAetherForgeCancelMessage Message;
	Message.protocol_version = AetherForgeProtocol::Version;
	Message.type = AetherForgeProtocol::Type_Cancel;
	Message.generation_id = GenerationId;

	FString Result;
	FJsonObjectConverter::UStructToJsonObjectString(Message, Result, /*CheckFlags*/ 0, /*SkipFlags*/ 0,
		/*Indent*/ 0, /*ExportCb*/ nullptr, /*bPrettyPrint*/ false);
	return Result;
}

bool FAetherForgeProtocol::ParseServerMessage(const FString& JsonText, FAetherForgeServerMessage& OutMessage)
{
	OutMessage = FAetherForgeServerMessage();

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return MakeInvalid(OutMessage, FString::Printf(TEXT("Malformed JSON frame: %s"), *Reader->GetErrorMessage()));
	}

	// protocol_version is validated on every message; mismatches fail loudly.
	int32 ProtocolVersion = 0;
	if (!Root->TryGetNumberField(TEXT("protocol_version"), ProtocolVersion))
	{
		return MakeInvalid(OutMessage, TEXT("Missing required field 'protocol_version'"));
	}
	if (ProtocolVersion != AetherForgeProtocol::Version)
	{
		return MakeInvalid(OutMessage, FString::Printf(
			TEXT("Unsupported protocol_version %d (expected %d)"), ProtocolVersion, AetherForgeProtocol::Version));
	}

	FString Type;
	if (!Root->TryGetStringField(TEXT("type"), Type))
	{
		return MakeInvalid(OutMessage, TEXT("Missing required field 'type'"));
	}

	const TSharedRef<FJsonObject> RootRef = Root.ToSharedRef();

	FString StrictError;

	if (Type == AetherForgeProtocol::Type_Plan)
	{
		if (!StrictDeserialize(RootRef, &OutMessage.Plan, TEXT("plan"), StrictError))
		{
			return MakeInvalid(OutMessage, MoveTemp(StrictError));
		}
		if (OutMessage.Plan.generation_id.IsEmpty())
		{
			return MakeInvalid(OutMessage, TEXT("'plan' message missing generation_id"));
		}
		if (OutMessage.Plan.total_assets < 0)
		{
			return MakeInvalid(OutMessage, TEXT("'plan' message has negative total_assets"));
		}
		OutMessage.Type = EAetherForgeServerMessageType::Plan;
		return true;
	}

	if (Type == AetherForgeProtocol::Type_Chunk)
	{
		if (!StrictDeserialize(RootRef, &OutMessage.Chunk, TEXT("chunk"), StrictError))
		{
			return MakeInvalid(OutMessage, MoveTemp(StrictError));
		}
		if (OutMessage.Chunk.generation_id.IsEmpty())
		{
			return MakeInvalid(OutMessage, TEXT("'chunk' message missing generation_id"));
		}
		// FJsonObjectConverter leaves fields it cannot find at their defaults; reject
		// entries that are structurally broken rather than silently spawning garbage.
		for (int32 Index = 0; Index < OutMessage.Chunk.assets.Num(); ++Index)
		{
			if (OutMessage.Chunk.assets[Index].category.IsEmpty())
			{
				return MakeInvalid(OutMessage, FString::Printf(
					TEXT("'chunk' asset entry %d missing category"), Index));
			}
		}
		OutMessage.Type = EAetherForgeServerMessageType::Chunk;
		return true;
	}

	if (Type == AetherForgeProtocol::Type_Complete)
	{
		if (!StrictDeserialize(RootRef, &OutMessage.Complete, TEXT("complete"), StrictError))
		{
			return MakeInvalid(OutMessage, MoveTemp(StrictError));
		}
		if (OutMessage.Complete.generation_id.IsEmpty())
		{
			return MakeInvalid(OutMessage, TEXT("'complete' message missing generation_id"));
		}
		OutMessage.Type = EAetherForgeServerMessageType::Complete;
		return true;
	}

	if (Type == AetherForgeProtocol::Type_Error)
	{
		if (!StrictDeserialize(RootRef, &OutMessage.Error, TEXT("error"), StrictError))
		{
			return MakeInvalid(OutMessage, MoveTemp(StrictError));
		}
		// generation_id may legitimately be empty here (pre-generation errors).
		if (OutMessage.Error.code.IsEmpty())
		{
			return MakeInvalid(OutMessage, TEXT("'error' message missing code"));
		}
		OutMessage.Type = EAetherForgeServerMessageType::Error;
		return true;
	}

	return MakeInvalid(OutMessage, FString::Printf(TEXT("Unknown or unexpected message type '%s'"), *Type));
}

bool FAetherForgeProtocol::ValidateChunkCategories(const FAetherForgeChunkMessage& Chunk,
	const TSet<FString>& ManifestCategories, FString& OutError)
{
	for (int32 Index = 0; Index < Chunk.assets.Num(); ++Index)
	{
		if (!ManifestCategories.Contains(Chunk.assets[Index].category))
		{
			OutError = FString::Printf(
				TEXT("'chunk' asset entry %d references unknown category '%s' (not in the manifest advertised at hello)"),
				Index, *Chunk.assets[Index].category);
			return false;
		}
	}
	OutError.Reset();
	return true;
}
