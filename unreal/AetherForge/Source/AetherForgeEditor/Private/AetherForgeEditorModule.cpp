// Copyright AetherForge. All Rights Reserved.

#include "AetherForgeEditorModule.h"

#include "AetherForgeManifest.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Net/AetherForgeClient.h"
#include "Sidecar/AetherForgeSidecarManager.h"
#include "Spawn/AetherForgeSpawner.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "UI/SAetherForgePanel.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogAetherForge, Log, All);

#define LOCTEXT_NAMESPACE "AetherForgeEditor"

const FName FAetherForgeEditorModule::PanelTabName(TEXT("AetherForgePanel"));

namespace
{
	constexpr int32 SidecarPort = 8080;
	constexpr int32 MaxConnectAttempts = 10;
	constexpr float ConnectRetryDelaySeconds = 0.5f;

	/** Project-relative path of the team-authored manifest data asset. */
	const TCHAR* DefaultManifestObjectPath = TEXT("/AetherForge/DA_AetherForgeManifest.DA_AetherForgeManifest");

	/** Hardcoded default generation region (cm) until the panel grows bounds controls (Phase 5). */
	FAetherForgeWireBounds DefaultBounds()
	{
		FAetherForgeWireBounds Bounds;
		Bounds.min.x = -5000.0;
		Bounds.min.y = -5000.0;
		Bounds.max.x = 5000.0;
		Bounds.max.y = 5000.0;
		return Bounds;
	}
}

FAetherForgeEditorModule& FAetherForgeEditorModule::Get()
{
	return FModuleManager::LoadModuleChecked<FAetherForgeEditorModule>(TEXT("AetherForgeEditor"));
}

void FAetherForgeEditorModule::StartupModule()
{
	UE_LOG(LogAetherForge, Log, TEXT("AetherForgeEditor module starting up."));

	SidecarManager = MakeShared<FAetherForgeSidecarManager>();

	Client = MakeShared<FAetherForgeClient, ESPMode::ThreadSafe>();
	Client->Initialize();

	Spawner = MakeShared<FAetherForgeSpawner>();
	Spawner->Initialize();

	// Generation lifecycle -> spawner. (The panel binds its own handlers for UI.)
	Client->OnConnectionError().AddRaw(this, &FAetherForgeEditorModule::HandleConnectionError);
	Client->OnChunk().AddRaw(this, &FAetherForgeEditorModule::HandleChunk);
	Client->OnComplete().AddRaw(this, &FAetherForgeEditorModule::HandleComplete);
	Client->OnServerError().AddRaw(this, &FAetherForgeEditorModule::HandleServerError);
	Client->OnClosed().AddRaw(this, &FAetherForgeEditorModule::HandleClosed);

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(PanelTabName,
		FOnSpawnTab::CreateRaw(this, &FAetherForgeEditorModule::SpawnPanelTab))
		.SetDisplayName(LOCTEXT("PanelTabTitle", "AetherForge"))
		.SetTooltipText(LOCTEXT("PanelTabTooltip", "Generate and populate environments from natural-language prompts."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("LevelEditor.Tabs.Outliner")));

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(
		this, &FAetherForgeEditorModule::RegisterMenus));
}

void FAetherForgeEditorModule::ShutdownModule()
{
	UE_LOG(LogAetherForge, Log, TEXT("AetherForgeEditor module shutting down."));

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PanelTabName);
	}

	if (ReconnectTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTickerHandle);
		ReconnectTickerHandle.Reset();
	}

	if (Client.IsValid())
	{
		Client->OnConnectionError().RemoveAll(this);
		Client->OnChunk().RemoveAll(this);
		Client->OnComplete().RemoveAll(this);
		Client->OnServerError().RemoveAll(this);
		Client->OnClosed().RemoveAll(this);
		Client->Shutdown();
		Client.Reset();
	}

	if (Spawner.IsValid())
	{
		Spawner->Shutdown();
		Spawner.Reset();
	}

	// Never leak the child process across editor sessions.
	if (SidecarManager.IsValid())
	{
		SidecarManager->Terminate();
		SidecarManager.Reset();
	}

	Manifest.Reset();
}

TSharedRef<SDockTab> FAetherForgeEditorModule::SpawnPanelTab(const FSpawnTabArgs& /*Args*/)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SAetherForgePanel)
		];
}

void FAetherForgeEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	FToolMenuSection& Section = ToolsMenu->FindOrAddSection(TEXT("AetherForge"));
	Section.AddMenuEntry(
		TEXT("OpenAetherForgePanel"),
		LOCTEXT("OpenPanelLabel", "AetherForge"),
		LOCTEXT("OpenPanelTooltip", "Open the AetherForge world-generation panel."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("LevelEditor.Tabs.Outliner")),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(PanelTabName);
		})));
}

UAetherForgeManifest* FAetherForgeEditorModule::GetOrLoadManifest()
{
	if (Manifest.IsValid())
	{
		return Manifest.Get();
	}

	if (UAetherForgeManifest* Loaded = LoadObject<UAetherForgeManifest>(nullptr, DefaultManifestObjectPath))
	{
		Manifest = TStrongObjectPtr<UAetherForgeManifest>(Loaded);
		return Manifest.Get();
	}

	UE_LOG(LogAetherForge, Warning,
		TEXT("No manifest data asset at '%s'; using an empty transient manifest. ")
		TEXT("Author a UAetherForgeManifest there to expose your asset library."),
		DefaultManifestObjectPath);

	Manifest = TStrongObjectPtr<UAetherForgeManifest>(
		NewObject<UAetherForgeManifest>(GetTransientPackage(), TEXT("TransientAetherForgeManifest")));
	return Manifest.Get();
}

void FAetherForgeEditorModule::EnsureBackend()
{
	if (!SidecarManager->IsRunning())
	{
		SidecarManager->Launch(SidecarPort);
	}

	if (SidecarManager->GetState() == EAetherForgeSidecarState::Running && !Client->IsConnected())
	{
		ConnectAttemptsRemaining = MaxConnectAttempts;
		TryConnect();
	}
}

void FAetherForgeEditorModule::TryConnect()
{
	if (Client->IsConnected())
	{
		return;
	}
	--ConnectAttemptsRemaining;
	Client->Connect(FAetherForgeClient::DefaultUrl, GetOrLoadManifest()->BuildWireManifest());
}

void FAetherForgeEditorModule::HandleConnectionError(const FString& Reason)
{
	// The sidecar may still be booting its WS listener; retry briefly before giving
	// up (the panel surfaces the terminal failure).
	if (ConnectAttemptsRemaining > 0 && SidecarManager->IsRunning())
	{
		if (ReconnectTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTickerHandle);
		}
		ReconnectTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this](float)
			{
				ReconnectTickerHandle.Reset();
				TryConnect();
				return false; // one-shot
			}),
			ConnectRetryDelaySeconds);
	}
	else
	{
		UE_LOG(LogAetherForge, Error, TEXT("Could not connect to the sidecar: %s"), *Reason);
	}
}

FString FAetherForgeEditorModule::StartGeneration(const FString& Prompt, const TOptional<int64>& Seed)
{
	if (!Client->IsConnected())
	{
		UE_LOG(LogAetherForge, Warning, TEXT("StartGeneration while disconnected; ignored."));
		return FString();
	}

	// Client-minted unique id, echoed on all server messages for this generation.
	ActiveGenerationId = FString::Printf(TEXT("gen-%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower());

	Spawner->BeginGeneration(ActiveGenerationId, Prompt, GetOrLoadManifest());
	Client->SendGenerate(ActiveGenerationId, Prompt, Seed, DefaultBounds());
	return ActiveGenerationId;
}

void FAetherForgeEditorModule::CancelActiveGeneration()
{
	if (ActiveGenerationId.IsEmpty())
	{
		return;
	}
	Client->SendCancel(ActiveGenerationId);
	// No complete follows a cancellation; close the transaction now so the partial
	// result is one undo step.
	Spawner->EndGeneration(/*bDiscardPending*/ true);
	ActiveGenerationId.Reset();
}

void FAetherForgeEditorModule::HandleChunk(const FAetherForgeChunkMessage& Chunk)
{
	if (Chunk.generation_id == ActiveGenerationId)
	{
		Spawner->EnqueueEntries(Chunk.assets);
	}
}

void FAetherForgeEditorModule::HandleComplete(const FAetherForgeCompleteMessage& Complete)
{
	if (Complete.generation_id == ActiveGenerationId)
	{
		Spawner->EndGeneration(/*bDiscardPending*/ false);
		ActiveGenerationId.Reset();
	}
}

void FAetherForgeEditorModule::HandleServerError(const FAetherForgeErrorMessage& Error)
{
	// error is terminal for its generation; keep what already spawned, undoable.
	if (!ActiveGenerationId.IsEmpty() && Error.generation_id == ActiveGenerationId)
	{
		Spawner->EndGeneration(/*bDiscardPending*/ true);
		ActiveGenerationId.Reset();
	}
}

void FAetherForgeEditorModule::HandleClosed(int32 /*StatusCode*/, const FString& /*Reason*/)
{
	// Socket close cancels any in-flight generation server-side; mirror that here.
	if (!ActiveGenerationId.IsEmpty())
	{
		Spawner->EndGeneration(/*bDiscardPending*/ true);
		ActiveGenerationId.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAetherForgeEditorModule, AetherForgeEditor)
