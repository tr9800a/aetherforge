// Copyright AetherForge. All Rights Reserved.

#include "SAetherForgePanel.h"

#include "AetherForgeEditorModule.h"
#include "Modules/ModuleManager.h"
#include "Net/AetherForgeClient.h"
#include "Sidecar/AetherForgeSidecarManager.h"
#include "Spawn/AetherForgeSpawner.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "AetherForgePanel"

void SAetherForgePanel::Construct(const FArguments& /*InArgs*/)
{
	ChildSlot
	[
		SNew(SVerticalBox)

		// Error banner (visible only in the Error state).
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("RoundedError")))
			.Visibility(this, &SAetherForgePanel::GetErrorBannerVisibility)
			.Padding(8.0f)
			[
				SNew(STextBlock)
				.Text(this, &SAetherForgePanel::GetErrorBannerText)
				.AutoWrapText(true)
			]
		]

		// Prompt.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PromptLabel", "Prompt"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 0.0f, 4.0f, 4.0f)
		[
			SAssignNew(PromptTextBox, SMultiLineEditableTextBox)
			.HintText(LOCTEXT("PromptHint",
				"e.g. a forest clearing with Pacific Northwest deciduous trees, scattered shrubs, and two large dogs"))
			.AutoWrapText(true)
		]

		// Seed field + actions.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SeedLabel", "Seed"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SAssignNew(SeedTextBox, SEditableTextBox)
				.HintText(LOCTEXT("SeedHint", "(random)"))
				.ToolTipText(LOCTEXT("SeedTooltip",
					"Optional reproducibility seed. Leave empty to let the server pick one (echoed back after planning)."))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.5f)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SAssignNew(AreaTextBox, SEditableTextBox)
				.HintText(LOCTEXT("AreaHint", "area: 70 m"))
				.ToolTipText(LOCTEXT("AreaTooltip",
					"Side length of the square placement region in meters, centered on the origin. "
					"Keep it within your level's walkable ground: entries that ground-snap onto nothing are skipped."))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("GenerateButton", "Generate"))
				.IsEnabled(this, &SAetherForgePanel::IsGenerateEnabled)
				.OnClicked(this, &SAetherForgePanel::OnGenerateClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("NewVariationButton", "New Variation"))
				.ToolTipText(LOCTEXT("NewVariationTooltip", "Re-run the prompt with a fresh random seed."))
				.IsEnabled(this, &SAetherForgePanel::IsGenerateEnabled)
				.OnClicked(this, &SAetherForgePanel::OnNewVariationClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("CancelButton", "Cancel"))
				.IsEnabled(this, &SAetherForgePanel::IsCancelEnabled)
				.OnClicked(this, &SAetherForgePanel::OnCancelClicked)
			]
		]

		// Determinate progress bar + label.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SProgressBar)
				.Percent(this, &SAetherForgePanel::GetProgressPercent)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SAetherForgePanel::GetProgressLabel)
			]
		]

		// Connection state + reconnect.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SAetherForgePanel::GetStateText)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("ReconnectButton", "Reconnect"))
				.Visibility_Lambda([this]()
				{
					return IsReconnectVisible() ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.OnClicked(this, &SAetherForgePanel::OnReconnectClicked)
			]
		]

		// Live stats line.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(STextBlock)
			.Text(this, &SAetherForgePanel::GetStatsText)
		]

		// Scrolling status log.
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			[
				SAssignNew(LogListView, SListView<TSharedPtr<FString>>)
				.ListItemsSource(&LogEntries)
				.OnGenerateRow(this, &SAetherForgePanel::GenerateLogRow)
				.SelectionMode(ESelectionMode::None)
			]
		]
	];

	BindBackendEvents();

	// Make the backend usable as soon as the panel opens (launch sidecar + connect).
	FAetherForgeEditorModule& Module = FAetherForgeEditorModule::Get();
	if (Module.GetClient()->IsConnected())
	{
		SetState(EAetherForgePanelState::ConnectedIdle);
	}
	else
	{
		SetState(EAetherForgePanelState::StartingSidecar);
		AppendLog(TEXT("Starting sidecar and connecting..."));
		Module.EnsureBackend();
	}
}

SAetherForgePanel::~SAetherForgePanel()
{
	UnbindBackendEvents();
}

void SAetherForgePanel::BindBackendEvents()
{
	FAetherForgeEditorModule& Module = FAetherForgeEditorModule::Get();

	const TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> Client = Module.GetClient();
	ConnectedHandle = Client->OnConnected().AddSP(this, &SAetherForgePanel::HandleConnected);
	ConnectionErrorHandle = Client->OnConnectionError().AddSP(this, &SAetherForgePanel::HandleConnectionError);
	ClosedHandle = Client->OnClosed().AddSP(this, &SAetherForgePanel::HandleClosed);
	PlanHandle = Client->OnPlan().AddSP(this, &SAetherForgePanel::HandlePlan);
	ChunkHandle = Client->OnChunk().AddSP(this, &SAetherForgePanel::HandleChunk);
	CompleteHandle = Client->OnComplete().AddSP(this, &SAetherForgePanel::HandleComplete);
	ServerErrorHandle = Client->OnServerError().AddSP(this, &SAetherForgePanel::HandleServerError);
	ProtocolErrorHandle = Client->OnProtocolError().AddSP(this, &SAetherForgePanel::HandleProtocolError);

	SidecarStateHandle = Module.GetSidecarManager()->OnStateChanged().AddSP(
		this, &SAetherForgePanel::HandleSidecarStateChanged);
	GenerationFinalizedHandle = Module.GetSpawner()->OnGenerationFinalized().AddSP(
		this, &SAetherForgePanel::HandleGenerationFinalized);
}

void SAetherForgePanel::UnbindBackendEvents()
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("AetherForgeEditor")))
	{
		return; // module already torn down; nothing to unbind
	}

	FAetherForgeEditorModule& Module = FAetherForgeEditorModule::Get();
	if (const TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> Client = Module.GetClient())
	{
		Client->OnConnected().Remove(ConnectedHandle);
		Client->OnConnectionError().Remove(ConnectionErrorHandle);
		Client->OnClosed().Remove(ClosedHandle);
		Client->OnPlan().Remove(PlanHandle);
		Client->OnChunk().Remove(ChunkHandle);
		Client->OnComplete().Remove(CompleteHandle);
		Client->OnServerError().Remove(ServerErrorHandle);
		Client->OnProtocolError().Remove(ProtocolErrorHandle);
	}
	if (const TSharedPtr<FAetherForgeSidecarManager> Sidecar = Module.GetSidecarManager())
	{
		Sidecar->OnStateChanged().Remove(SidecarStateHandle);
	}
	if (const TSharedPtr<FAetherForgeSpawner> Spawner = Module.GetSpawner())
	{
		Spawner->OnGenerationFinalized().Remove(GenerationFinalizedHandle);
	}
}

// --- UI callbacks ------------------------------------------------------------

FReply SAetherForgePanel::OnGenerateClicked()
{
	StartGeneration(ParseSeedField());
	return FReply::Handled();
}

FReply SAetherForgePanel::OnNewVariationClicked()
{
	// Fresh random seed, surfaced in the seed field for reproducibility.
	const int64 NewSeed = static_cast<int64>(FMath::Rand()) * 32768 + FMath::Rand();
	SeedTextBox->SetText(FText::AsNumber(NewSeed, &FNumberFormattingOptions::DefaultNoGrouping()));
	StartGeneration(NewSeed);
	return FReply::Handled();
}

FReply SAetherForgePanel::OnCancelClicked()
{
	FAetherForgeEditorModule::Get().CancelActiveGeneration();
	AppendLog(TEXT("Generation cancelled (already-spawned assets remain; one Ctrl+Z removes them)."));
	SetState(EAetherForgePanelState::ConnectedIdle);
	return FReply::Handled();
}

FReply SAetherForgePanel::OnReconnectClicked()
{
	SetState(EAetherForgePanelState::StartingSidecar);
	AppendLog(TEXT("Reconnecting..."));
	FAetherForgeEditorModule::Get().EnsureBackend();
	return FReply::Handled();
}

void SAetherForgePanel::StartGeneration(const TOptional<int64>& SeedOverride)
{
	const FString Prompt = PromptTextBox->GetText().ToString().TrimStartAndEnd();
	if (Prompt.IsEmpty())
	{
		AppendLog(TEXT("Enter a prompt first."));
		return;
	}

	const FString GenerationId = FAetherForgeEditorModule::Get().StartGeneration(Prompt, SeedOverride, ParseAreaField());
	if (GenerationId.IsEmpty())
	{
		EnterErrorState(TEXT("Not connected to the sidecar."));
		return;
	}

	TotalAssets = 0;
	ReceivedAssets = 0;
	SetState(EAetherForgePanelState::Generating);
	AppendLog(FString::Printf(TEXT("Generating '%s' (%s)..."), *Prompt, *GenerationId));
}

TOptional<int64> SAetherForgePanel::ParseSeedField() const
{
	const FString SeedText = SeedTextBox->GetText().ToString().TrimStartAndEnd();
	if (SeedText.IsEmpty())
	{
		return TOptional<int64>(); // omitted => the server picks and echoes in plan
	}

	int64 Seed = 0;
	if (LexTryParseString(Seed, *SeedText))
	{
		return Seed;
	}

	return TOptional<int64>(); // non-numeric input: treat as unset
}

double SAetherForgePanel::ParseAreaField() const
{
	const FString AreaText = AreaTextBox->GetText().ToString().TrimStartAndEnd();
	double AreaMeters = 0.0;
	if (!AreaText.IsEmpty() && LexTryParseString(AreaMeters, *AreaText) && AreaMeters > 0.0)
	{
		return AreaMeters; // the module clamps to its sane range
	}
	return DefaultAreaMeters; // empty or non-numeric input
}

bool SAetherForgePanel::IsGenerateEnabled() const
{
	return State == EAetherForgePanelState::ConnectedIdle;
}

bool SAetherForgePanel::IsCancelEnabled() const
{
	return State == EAetherForgePanelState::Generating;
}

bool SAetherForgePanel::IsReconnectVisible() const
{
	return State == EAetherForgePanelState::Error || State == EAetherForgePanelState::Disconnected;
}

TOptional<float> SAetherForgePanel::GetProgressPercent() const
{
	if (State != EAetherForgePanelState::Generating || TotalAssets <= 0)
	{
		return 0.0f;
	}
	return FMath::Clamp(static_cast<float>(ReceivedAssets) / static_cast<float>(TotalAssets), 0.0f, 1.0f);
}

FText SAetherForgePanel::GetProgressLabel() const
{
	if (TotalAssets > 0)
	{
		return FText::Format(LOCTEXT("ProgressFormat", "{0} / {1}"),
			FText::AsNumber(ReceivedAssets), FText::AsNumber(TotalAssets));
	}
	return LOCTEXT("ProgressIdle", "0 / 0");
}

FText SAetherForgePanel::GetStateText() const
{
	switch (State)
	{
	case EAetherForgePanelState::Disconnected:    return LOCTEXT("StateDisconnected", "Disconnected");
	case EAetherForgePanelState::StartingSidecar: return LOCTEXT("StateStarting", "Starting sidecar...");
	case EAetherForgePanelState::ConnectedIdle:   return LOCTEXT("StateIdle", "Connected (idle)");
	case EAetherForgePanelState::Generating:      return LOCTEXT("StateGenerating", "Generating...");
	case EAetherForgePanelState::Error:           return LOCTEXT("StateError", "Error");
	default:                                      return FText::GetEmpty();
	}
}

FText SAetherForgePanel::GetStatsText() const
{
	const FAetherForgeSpawnStats& Stats = FAetherForgeEditorModule::Get().GetSpawner()->GetStats();
	return FText::Format(
		LOCTEXT("StatsFormat", "{0} spawned | {1} assets/s | spawn tick {2} ms (worst {3} ms) | LLM {4} ms"),
		FText::AsNumber(Stats.SpawnedCount),
		FText::AsNumber(FMath::RoundToInt(Stats.AssetsPerSecond)),
		FText::AsNumber(Stats.LastSpawnTickMs),
		FText::AsNumber(Stats.MaxSpawnTickMs),
		FText::AsNumber(LastLlmMs));
}

FText SAetherForgePanel::GetErrorBannerText() const
{
	return FText::FromString(ErrorMessage);
}

EVisibility SAetherForgePanel::GetErrorBannerVisibility() const
{
	return State == EAetherForgePanelState::Error ? EVisibility::Visible : EVisibility::Collapsed;
}

TSharedRef<ITableRow> SAetherForgePanel::GenerateLogRow(TSharedPtr<FString> Item,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
	[
		SNew(STextBlock)
		.Text(FText::FromString(Item.IsValid() ? *Item : FString()))
	];
}

// --- backend events ------------------------------------------------------------

void SAetherForgePanel::HandleConnected()
{
	AppendLog(TEXT("Connected to sidecar; hello sent."));
	if (State != EAetherForgePanelState::Generating)
	{
		SetState(EAetherForgePanelState::ConnectedIdle);
	}
}

void SAetherForgePanel::HandleConnectionError(const FString& Reason)
{
	// The module retries while the sidecar boots; only surface the terminal failure.
	FAetherForgeEditorModule& Module = FAetherForgeEditorModule::Get();
	if (Module.GetSidecarManager()->IsRunning() && Module.IsConnectRetryPending())
	{
		return; // retry in flight
	}
	EnterErrorState(FString::Printf(TEXT("Connection failed: %s"), *Reason));
}

void SAetherForgePanel::HandleClosed(const int32 StatusCode, const FString& Reason)
{
	AppendLog(FString::Printf(TEXT("Connection closed (code %d): %s"), StatusCode, *Reason));
	if (State != EAetherForgePanelState::Error)
	{
		SetState(EAetherForgePanelState::Disconnected);
	}
}

void SAetherForgePanel::HandlePlan(const FAetherForgePlanMessage& Plan)
{
	TotalAssets = Plan.total_assets;
	ReceivedAssets = 0;

	// The seed in effect (server-chosen when the field was empty): echo it into the
	// seed field so the result is reproducible.
	SeedTextBox->SetText(FText::AsNumber(Plan.seed, &FNumberFormattingOptions::DefaultNoGrouping()));

	FString CategorySummary;
	for (const TPair<FString, int32>& Pair : Plan.categories)
	{
		CategorySummary += FString::Printf(TEXT("%s%s: %d"),
			CategorySummary.IsEmpty() ? TEXT("") : TEXT(", "), *Pair.Key, Pair.Value);
	}
	AppendLog(FString::Printf(TEXT("Planned %d assets (seed %lld) — %s"),
		Plan.total_assets, Plan.seed, *CategorySummary));
}

void SAetherForgePanel::HandleChunk(const FAetherForgeChunkMessage& Chunk)
{
	ReceivedAssets += Chunk.assets.Num();

	// Per-category tally for a compact log line, e.g. "+42 deciduous_tree, +8 shrub".
	TMap<FString, int32> Counts;
	for (const FAetherForgeAssetEntry& Entry : Chunk.assets)
	{
		++Counts.FindOrAdd(Entry.category);
	}
	FString Summary;
	for (const TPair<FString, int32>& Pair : Counts)
	{
		Summary += FString::Printf(TEXT("%s+%d %s"),
			Summary.IsEmpty() ? TEXT("") : TEXT(", "), Pair.Value, *Pair.Key);
	}
	AppendLog(Summary);
}

void SAetherForgePanel::HandleComplete(const FAetherForgeCompleteMessage& Complete)
{
	LastLlmMs = Complete.stats.llm_ms;
	AppendLog(FString::Printf(TEXT("Complete: %d assets in %lld ms (LLM %lld ms)."),
		Complete.stats.assets, Complete.stats.elapsed_ms, Complete.stats.llm_ms));
	SetState(EAetherForgePanelState::ConnectedIdle);
}

void SAetherForgePanel::HandleGenerationFinalized(const FAetherForgeSpawnStats& SpawnStats)
{
	AppendLog(FString::Printf(
		TEXT("Spawned %d (%d skipped) | %.0f assets/s | worst spawn tick %.2f ms (budget %.1f ms)."),
		SpawnStats.SpawnedCount, SpawnStats.SkippedCount, SpawnStats.AssetsPerSecond,
		SpawnStats.MaxSpawnTickMs, FAetherForgeSpawner::SpawnBudgetSeconds * 1000.0));
}

void SAetherForgePanel::HandleServerError(const FAetherForgeErrorMessage& Error)
{
	AppendLog(FString::Printf(TEXT("Server error [%s]: %s"), *Error.code, *Error.message));
	if (Error.recoverable)
	{
		// Connection stays usable; back to idle so a new generate can be issued.
		SetState(EAetherForgePanelState::ConnectedIdle);
	}
	else
	{
		// The server closes the connection after a non-recoverable error.
		EnterErrorState(Error.message);
	}
}

void SAetherForgePanel::HandleProtocolError(const FString& ParseError)
{
	AppendLog(FString::Printf(TEXT("Protocol error: %s"), *ParseError));
}

void SAetherForgePanel::HandleSidecarStateChanged(const EAetherForgeSidecarState NewState)
{
	switch (NewState)
	{
	case EAetherForgeSidecarState::Starting:
		SetState(EAetherForgePanelState::StartingSidecar);
		break;
	case EAetherForgeSidecarState::Running:
		AppendLog(TEXT("Sidecar process running."));
		break;
	case EAetherForgeSidecarState::Dead:
		EnterErrorState(FAetherForgeEditorModule::Get().GetSidecarManager()->GetLastError());
		break;
	case EAetherForgeSidecarState::BinaryMissing:
		EnterErrorState(FAetherForgeEditorModule::Get().GetSidecarManager()->GetLastError());
		break;
	case EAetherForgeSidecarState::Stopped:
		SetState(EAetherForgePanelState::Disconnected);
		break;
	default:
		break;
	}
}

// --- helpers --------------------------------------------------------------------

void SAetherForgePanel::SetState(const EAetherForgePanelState NewState)
{
	State = NewState;
	if (NewState != EAetherForgePanelState::Error)
	{
		ErrorMessage.Reset();
	}
}

void SAetherForgePanel::EnterErrorState(const FString& Message)
{
	ErrorMessage = Message;
	State = EAetherForgePanelState::Error;
	AppendLog(FString::Printf(TEXT("ERROR: %s"), *Message));
}

void SAetherForgePanel::AppendLog(const FString& Line)
{
	LogEntries.Add(MakeShared<FString>(
		FString::Printf(TEXT("[%s] %s"), *FDateTime::Now().ToString(TEXT("%H:%M:%S")), *Line)));
	if (LogListView.IsValid())
	{
		LogListView->RequestListRefresh();
		LogListView->ScrollToBottom();
	}
}

#undef LOCTEXT_NAMESPACE
