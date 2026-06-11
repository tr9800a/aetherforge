// Copyright AetherForge. All Rights Reserved.

// UE Automation tests for the protocol v1 contract (spec Phase 0 / plan tasks
// 0.6 + 2.7): both the Go test suite and these tests consume the exact shared
// fixture bytes under contract/fixtures/, so the two sides cannot drift apart
// silently. Run headless with:
//   UnrealEditor-Cmd <Project>.uproject -ExecCmds="Automation RunTests AetherForge.Protocol; Quit" \
//       -unattended -nopause -nullrhi -nosplash -log

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AetherForgeProtocolTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace AetherForgeProtocolTests
{
	/**
	 * Fixture directory resolution (recorded in contract/README.md):
	 * 1. AETHERFORGE_CONTRACT_FIXTURES environment variable, if set (CI /
	 *    plugin-copied-into-a-project layouts);
	 * 2. otherwise <PluginBaseDir>/../../contract/fixtures — the in-repo layout,
	 *    where the plugin lives at unreal/AetherForge/.
	 */
	FString FixturesDir()
	{
		const FString FromEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("AETHERFORGE_CONTRACT_FIXTURES"));
		if (!FromEnv.IsEmpty())
		{
			return FromEnv;
		}
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AetherForge"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		FString Dir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(Plugin->GetBaseDir(), TEXT("../../contract/fixtures")));
		FPaths::CollapseRelativeDirectories(Dir);
		return Dir;
	}

	bool LoadFixture(FAutomationTestBase& Test, const FString& Dir, const FString& Name,
		FString& OutText, TSharedPtr<FJsonObject>& OutObject)
	{
		const FString Path = FPaths::Combine(Dir, Name);
		if (!FFileHelper::LoadFileToString(OutText, *Path))
		{
			Test.AddError(FString::Printf(TEXT("Cannot read fixture %s — set AETHERFORGE_CONTRACT_FIXTURES "
				"to the contract/fixtures directory if the plugin is not in the AetherForge repo layout"), *Path));
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutText);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("Fixture %s is not valid JSON: %s"), *Path, *Reader->GetErrorMessage()));
			return false;
		}
		return true;
	}

	bool ParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	/** Semantic JSON equality — parsed values, never raw strings (45.0 == 45). */
	bool JsonEquals(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return false;
		}
		return FJsonValue::CompareEqual(FJsonValueObject(A), FJsonValueObject(B));
	}

	FString Condense(const TSharedPtr<FJsonObject>& Object)
	{
		FString Result;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Result;
	}

	/** Round-trip helper for server -> client fixtures: original bytes -> ParseServerMessage -> USTRUCT -> JSON -> semantic equality. */
	template <typename TMessage>
	void CheckServerFixtureRoundTrip(FAutomationTestBase& Test, const FString& Name,
		const TSharedPtr<FJsonObject>& Original, const TMessage& ParsedStruct)
	{
		const TSharedPtr<FJsonObject> Reexported = FJsonObjectConverter::UStructToJsonObject(ParsedStruct);
		if (!Test.TestTrue(Name + TEXT(": re-export to JSON succeeds"), Reexported.IsValid()))
		{
			return;
		}
		Test.TestTrue(Name + TEXT(": round-trip semantically equal to fixture bytes (got: ") + Condense(Reexported) + TEXT(")"),
			JsonEquals(Original, Reexported));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAetherForgeProtocolFixtureRoundTripTest,
	"AetherForge.Protocol.FixtureRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAetherForgeProtocolFixtureRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace AetherForgeProtocolTests;

	const FString Dir = FixturesDir();
	if (!TestFalse(TEXT("fixture directory resolved"), Dir.IsEmpty()))
	{
		return false;
	}

	// --- server -> client fixtures: through the strict inbound parser ---
	const TCHAR* ServerFixtures[] = { TEXT("plan.json"), TEXT("chunk.json"), TEXT("complete.json"), TEXT("error.json") };
	for (const TCHAR* Name : ServerFixtures)
	{
		FString Text;
		TSharedPtr<FJsonObject> Original;
		if (!LoadFixture(*this, Dir, Name, Text, Original))
		{
			continue;
		}
		FAetherForgeServerMessage Message;
		const bool bParsed = FAetherForgeProtocol::ParseServerMessage(Text, Message);
		if (!TestTrue(FString(Name) + TEXT(": ParseServerMessage succeeds (") + Message.ParseError + TEXT(")"), bParsed))
		{
			continue;
		}
		switch (Message.Type)
		{
		case EAetherForgeServerMessageType::Plan:
			TestEqual(FString(Name) + TEXT(": parsed as plan"), FString(Name), FString(TEXT("plan.json")));
			CheckServerFixtureRoundTrip(*this, Name, Original, Message.Plan);
			TestEqual(TEXT("plan.seed"), Message.Plan.seed, static_cast<int64>(1337));
			TestEqual(TEXT("plan.total_assets"), Message.Plan.total_assets, 142);
			break;
		case EAetherForgeServerMessageType::Chunk:
			TestEqual(FString(Name) + TEXT(": parsed as chunk"), FString(Name), FString(TEXT("chunk.json")));
			CheckServerFixtureRoundTrip(*this, Name, Original, Message.Chunk);
			if (TestEqual(TEXT("chunk.assets count"), Message.Chunk.assets.Num(), 6))
			{
				const FAetherForgeAssetEntry& First = Message.Chunk.assets[0];
				TestEqual(TEXT("chunk.assets[0].category"), First.category, FString(TEXT("deciduous_tree")));
				TestEqual(TEXT("chunk.assets[0].transform.location.x"), First.transform.location.x, 150.0);
				TestEqual(TEXT("chunk.assets[0].transform.location.y"), First.transform.location.y, -200.0);
				TestEqual(TEXT("chunk.assets[0].transform.yaw"), First.transform.yaw, 45.0);
				TestEqual(TEXT("chunk.assets[0].transform.scale"), First.transform.scale, 1.2);
				TestTrue(TEXT("chunk.assets[0].ground_snap"), First.ground_snap);
				TestEqual(TEXT("chunk.assets[0].tags count"), First.tags.Num(), 2);
			}
			break;
		case EAetherForgeServerMessageType::Complete:
			TestEqual(FString(Name) + TEXT(": parsed as complete"), FString(Name), FString(TEXT("complete.json")));
			CheckServerFixtureRoundTrip(*this, Name, Original, Message.Complete);
			TestEqual(TEXT("complete.stats.assets"), Message.Complete.stats.assets, 142);
			TestEqual(TEXT("complete.stats.elapsed_ms"), Message.Complete.stats.elapsed_ms, static_cast<int64>(4815));
			TestEqual(TEXT("complete.stats.llm_ms"), Message.Complete.stats.llm_ms, static_cast<int64>(2930));
			break;
		case EAetherForgeServerMessageType::Error:
			TestEqual(FString(Name) + TEXT(": parsed as error"), FString(Name), FString(TEXT("error.json")));
			CheckServerFixtureRoundTrip(*this, Name, Original, Message.Error);
			TestEqual(TEXT("error.code"), Message.Error.code, FString(TEXT("llm_invalid_output")));
			TestTrue(TEXT("error.recoverable"), Message.Error.recoverable);
			break;
		default:
			AddError(FString(Name) + TEXT(": unexpected parsed message type"));
			break;
		}
	}

	// --- client -> server fixtures: fixture -> USTRUCT -> Serialize* -> semantic equality ---
	{
		FString Text;
		TSharedPtr<FJsonObject> Original;
		if (LoadFixture(*this, Dir, TEXT("hello.json"), Text, Original))
		{
			FAetherForgeHelloMessage Hello;
			if (TestTrue(TEXT("hello.json: import to USTRUCT"),
					FJsonObjectConverter::JsonObjectToUStruct(Original.ToSharedRef(), &Hello, 0, 0)))
			{
				TestEqual(TEXT("hello.manifest entry count"), Hello.manifest.Num(), 4);
				const FAetherForgeWireManifestEntry* Tree = Hello.manifest.Find(TEXT("deciduous_tree"));
				if (TestNotNull(TEXT("hello.manifest contains deciduous_tree"), Tree))
				{
					TestEqual(TEXT("deciduous_tree.footprint_radius"), Tree->footprint_radius, 250.0);
					TestTrue(TEXT("deciduous_tree.ground_snap"), Tree->ground_snap);
					TestTrue(TEXT("deciduous_tree.instanceable"), Tree->instanceable);
					TestEqual(TEXT("deciduous_tree.display_name"), Tree->display_name, FString(TEXT("Deciduous Tree")));
				}
				const FAetherForgeWireManifestEntry* Dog = Hello.manifest.Find(TEXT("dog_large"));
				if (TestNotNull(TEXT("hello.manifest contains dog_large"), Dog))
				{
					TestFalse(TEXT("dog_large not instanceable"), Dog->instanceable);
				}

				const FString Serialized = FAetherForgeProtocol::SerializeHello(Hello.manifest);
				TSharedPtr<FJsonObject> Reparsed;
				if (TestTrue(TEXT("SerializeHello output parses"), ParseJsonObject(Serialized, Reparsed)))
				{
					TestTrue(TEXT("hello.json: SerializeHello semantically equal to fixture (got: ") + Serialized + TEXT(")"),
						JsonEquals(Original, Reparsed));
				}
			}
		}
	}
	{
		FString Text;
		TSharedPtr<FJsonObject> Original;
		if (LoadFixture(*this, Dir, TEXT("generate.json"), Text, Original))
		{
			FAetherForgeGenerateMessage Generate;
			if (TestTrue(TEXT("generate.json: import to USTRUCT"),
					FJsonObjectConverter::JsonObjectToUStruct(Original.ToSharedRef(), &Generate, 0, 0)))
			{
				TestEqual(TEXT("generate.seed"), Generate.seed, static_cast<int64>(1337));
				TestEqual(TEXT("generate.bounds.min.x"), Generate.bounds.min.x, -5000.0);
				TestEqual(TEXT("generate.bounds.max.y"), Generate.bounds.max.y, 5000.0);

				const FString Serialized = FAetherForgeProtocol::SerializeGenerate(
					Generate.generation_id, Generate.prompt, TOptional<int64>(Generate.seed), Generate.bounds);
				TSharedPtr<FJsonObject> Reparsed;
				if (TestTrue(TEXT("SerializeGenerate output parses"), ParseJsonObject(Serialized, Reparsed)))
				{
					TestTrue(TEXT("generate.json: SerializeGenerate semantically equal to fixture (got: ") + Serialized + TEXT(")"),
						JsonEquals(Original, Reparsed));
				}
			}
		}
	}
	{
		FString Text;
		TSharedPtr<FJsonObject> Original;
		if (LoadFixture(*this, Dir, TEXT("cancel.json"), Text, Original))
		{
			FAetherForgeCancelMessage Cancel;
			if (TestTrue(TEXT("cancel.json: import to USTRUCT"),
					FJsonObjectConverter::JsonObjectToUStruct(Original.ToSharedRef(), &Cancel, 0, 0)))
			{
				TestEqual(TEXT("cancel.generation_id"), Cancel.generation_id, FString(TEXT("gen-7f3a9c12")));

				const FString Serialized = FAetherForgeProtocol::SerializeCancel(Cancel.generation_id);
				TSharedPtr<FJsonObject> Reparsed;
				if (TestTrue(TEXT("SerializeCancel output parses"), ParseJsonObject(Serialized, Reparsed)))
				{
					TestTrue(TEXT("cancel.json: SerializeCancel semantically equal to fixture (got: ") + Serialized + TEXT(")"),
						JsonEquals(Original, Reparsed));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAetherForgeProtocolInvalidFixturesTest,
	"AetherForge.Protocol.InvalidFixturesRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAetherForgeProtocolInvalidFixturesTest::RunTest(const FString& Parameters)
{
	using namespace AetherForgeProtocolTests;

	const FString Dir = FixturesDir();
	if (!TestFalse(TEXT("fixture directory resolved"), Dir.IsEmpty()))
	{
		return false;
	}
	const FString InvalidDir = FPaths::Combine(Dir, TEXT("invalid"));

	// Structural violations the strict parser must reject: a bad
	// protocol_version, a forbidden extra `z` on a location, pitch/roll on a
	// transform. None of these may ever be silently coerced/dropped.
	const TCHAR* RejectedByParser[] = {
		TEXT("bad_protocol_version.json"),
		TEXT("asset_with_z.json"),
		TEXT("asset_full_rotation.json"),
	};
	for (const TCHAR* Name : RejectedByParser)
	{
		FString Text;
		TSharedPtr<FJsonObject> Original;
		if (!LoadFixture(*this, InvalidDir, Name, Text, Original))
		{
			continue;
		}
		FAetherForgeServerMessage Message;
		const bool bParsed = FAetherForgeProtocol::ParseServerMessage(Text, Message);
		TestFalse(FString(Name) + TEXT(": must be rejected by ParseServerMessage"), bParsed);
		if (!bParsed)
		{
			TestFalse(FString(Name) + TEXT(": rejection carries a descriptive ParseError"),
				Message.ParseError.IsEmpty());
			AddInfo(FString(Name) + TEXT(" rejected: ") + Message.ParseError);
		}
	}

	// Semantic violation: a structurally valid chunk whose category is not in
	// the manifest advertised at hello — rejected by manifest validation.
	{
		FString HelloText;
		TSharedPtr<FJsonObject> HelloObject;
		FString ChunkText;
		TSharedPtr<FJsonObject> ChunkObject;
		if (LoadFixture(*this, Dir, TEXT("hello.json"), HelloText, HelloObject) &&
			LoadFixture(*this, InvalidDir, TEXT("unknown_category.json"), ChunkText, ChunkObject))
		{
			FAetherForgeHelloMessage Hello;
			if (TestTrue(TEXT("hello.json imports"),
					FJsonObjectConverter::JsonObjectToUStruct(HelloObject.ToSharedRef(), &Hello, 0, 0)))
			{
				TSet<FString> Categories;
				Hello.manifest.GetKeys(Categories);

				FAetherForgeServerMessage Message;
				if (TestTrue(TEXT("unknown_category.json passes structural parse"),
						FAetherForgeProtocol::ParseServerMessage(ChunkText, Message)) &&
					TestEqual(TEXT("unknown_category.json parses as chunk"),
						static_cast<int32>(Message.Type), static_cast<int32>(EAetherForgeServerMessageType::Chunk)))
				{
					FString ValidationError;
					TestFalse(TEXT("unknown_category.json: must be rejected by manifest category validation"),
						FAetherForgeProtocol::ValidateChunkCategories(Message.Chunk, Categories, ValidationError));
					TestFalse(TEXT("category rejection carries a descriptive error"), ValidationError.IsEmpty());
					AddInfo(TEXT("unknown_category.json rejected: ") + ValidationError);
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAetherForgeProtocolGenerateSeedOptionalTest,
	"AetherForge.Protocol.GenerateSeedOptional",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAetherForgeProtocolGenerateSeedOptionalTest::RunTest(const FString& Parameters)
{
	using namespace AetherForgeProtocolTests;

	FAetherForgeWireBounds Bounds;
	Bounds.min.x = -100.0;
	Bounds.min.y = -100.0;
	Bounds.max.x = 100.0;
	Bounds.max.y = 100.0;

	// Unset seed must be genuinely omitted from the payload.
	{
		const FString Serialized = FAetherForgeProtocol::SerializeGenerate(
			TEXT("g1"), TEXT("p"), TOptional<int64>(), Bounds);
		TSharedPtr<FJsonObject> Parsed;
		if (TestTrue(TEXT("seedless generate parses"), ParseJsonObject(Serialized, Parsed)))
		{
			TestFalse(TEXT("omitted seed must not appear on the wire"), Parsed->HasField(TEXT("seed")));
			TestTrue(TEXT("protocol_version present"), Parsed->HasField(TEXT("protocol_version")));
		}
	}

	// Set seed must appear.
	{
		const FString Serialized = FAetherForgeProtocol::SerializeGenerate(
			TEXT("g1"), TEXT("p"), TOptional<int64>(1337), Bounds);
		TSharedPtr<FJsonObject> Parsed;
		if (TestTrue(TEXT("seeded generate parses"), ParseJsonObject(Serialized, Parsed)))
		{
			double Seed = 0.0;
			TestTrue(TEXT("seed field present"), Parsed->TryGetNumberField(TEXT("seed"), Seed));
			TestEqual(TEXT("seed value"), Seed, 1337.0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
