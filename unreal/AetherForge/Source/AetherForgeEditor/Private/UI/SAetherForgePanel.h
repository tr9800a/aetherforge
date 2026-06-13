// Copyright AetherForge. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AetherForgeProtocolTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ITableRow;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class STableViewBase;
enum class EAetherForgeSidecarState : uint8;

/**
 * Explicit UI states; one enum drives enabled/disabled controls and the error banner
 * (plan task 2.2).
 */
enum class EAetherForgePanelState : uint8
{
	Disconnected,
	StartingSidecar,
	ConnectedIdle,
	Generating,
	Error,
};

/**
 * The dockable AetherForge panel: multi-line prompt box, Generate button, seed field +
 * "New Variation" button, determinate progress bar (plan.total_assets vs received),
 * Cancel button, live stats line, scrolling status log.
 *
 * Pure UI: backend wiring (sidecar, client, spawner) lives in FAetherForgeEditorModule;
 * the panel only renders state and forwards button presses. All delegates it binds
 * fire on the game thread.
 */
class SAetherForgePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAetherForgePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SAetherForgePanel() override;

private:
	// --- UI callbacks -------------------------------------------------------
	FReply OnGenerateClicked();
	FReply OnNewVariationClicked();
	FReply OnCancelClicked();
	FReply OnReconnectClicked();
	bool IsGenerateEnabled() const;
	bool IsCancelEnabled() const;
	bool IsReconnectVisible() const;
	TOptional<float> GetProgressPercent() const;
	FText GetProgressLabel() const;
	FText GetStatsText() const;
	FText GetStateText() const;
	FText GetErrorBannerText() const;
	EVisibility GetErrorBannerVisibility() const;
	TSharedRef<ITableRow> GenerateLogRow(TSharedPtr<FString> Item, const TSharedRef<STableViewBase>& OwnerTable);

	// --- backend events (game thread) ---------------------------------------
	void HandleConnected();
	void HandleConnectionError(const FString& Reason);
	void HandleClosed(int32 StatusCode, const FString& Reason);
	void HandlePlan(const FAetherForgePlanMessage& Plan);
	void HandleChunk(const FAetherForgeChunkMessage& Chunk);
	void HandleComplete(const FAetherForgeCompleteMessage& Complete);
	void HandleServerError(const FAetherForgeErrorMessage& Error);
	void HandleProtocolError(const FString& ParseError);
	void HandleSidecarStateChanged(EAetherForgeSidecarState NewState);
	void HandleGenerationFinalized(const struct FAetherForgeSpawnStats& SpawnStats);

	// --- helpers -------------------------------------------------------------
	void StartGeneration(const TOptional<int64>& SeedOverride);
	TOptional<int64> ParseSeedField() const;
	double ParseAreaField() const;
	void SetState(EAetherForgePanelState NewState);
	void EnterErrorState(const FString& Message);
	void AppendLog(const FString& Line);
	void BindBackendEvents();
	void UnbindBackendEvents();

	/** Placement region side length used when the area field is empty or invalid. */
	static constexpr double DefaultAreaMeters = 70.0;

	// --- widgets -------------------------------------------------------------
	TSharedPtr<SMultiLineEditableTextBox> PromptTextBox;
	TSharedPtr<SEditableTextBox> SeedTextBox;
	TSharedPtr<SEditableTextBox> AreaTextBox;
	TSharedPtr<SListView<TSharedPtr<FString>>> LogListView;
	TArray<TSharedPtr<FString>> LogEntries;

	// --- state ---------------------------------------------------------------
	EAetherForgePanelState State = EAetherForgePanelState::Disconnected;
	FString ErrorMessage;
	int32 TotalAssets = 0;
	int32 ReceivedAssets = 0;
	int64 LastLlmMs = 0;

	// Delegate handles for clean unbinding (the client/sidecar outlive the panel).
	FDelegateHandle ConnectedHandle;
	FDelegateHandle ConnectionErrorHandle;
	FDelegateHandle ClosedHandle;
	FDelegateHandle PlanHandle;
	FDelegateHandle ChunkHandle;
	FDelegateHandle CompleteHandle;
	FDelegateHandle ServerErrorHandle;
	FDelegateHandle ProtocolErrorHandle;
	FDelegateHandle SidecarStateHandle;
	FDelegateHandle GenerationFinalizedHandle;
};
