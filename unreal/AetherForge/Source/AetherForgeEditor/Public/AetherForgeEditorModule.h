// Copyright AetherForge. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AetherForgeProtocolTypes.h"
#include "Containers/Ticker.h"
#include "Modules/ModuleInterface.h"
#include "UObject/StrongObjectPtr.h"

class FAetherForgeClient;
class FAetherForgeSidecarManager;
class FAetherForgeSpawner;
class FSpawnTabArgs;
class SDockTab;
class UAetherForgeManifest;

/**
 * AetherForge editor module: owns the sidecar lifecycle, the WebSocket client, the
 * time-budgeted spawner, and the dockable Slate panel. Module type is Editor — nothing
 * ships in packaged builds.
 */
class AETHERFORGEEDITOR_API FAetherForgeEditorModule : public IModuleInterface
{
public:
	/** Nomad tab id for the AetherForge panel. */
	static const FName PanelTabName;

	static FAetherForgeEditorModule& Get();

	//~ IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> GetClient() const { return Client; }
	TSharedPtr<FAetherForgeSidecarManager> GetSidecarManager() const { return SidecarManager; }
	TSharedPtr<FAetherForgeSpawner> GetSpawner() const { return Spawner; }

	/**
	 * The asset catalog. Loaded from /AetherForge/DA_AetherForgeManifest on first use;
	 * falls back to an empty transient manifest (with a warning) so the handshake
	 * still works in a project that has not authored one yet.
	 */
	UAetherForgeManifest* GetOrLoadManifest();

	/**
	 * Make the backend usable: launch the sidecar if it is not running and connect
	 * (with retries while the sidecar boots), then send hello.
	 */
	void EnsureBackend();

	/**
	 * Mint a generation_id, open the spawner's transaction, and send generate with a
	 * square placement region of AreaMeters per side, centered on the origin.
	 * Returns the generation_id, or empty if not connected.
	 */
	FString StartGeneration(const FString& Prompt, const TOptional<int64>& Seed, double AreaMeters);

	/** Send cancel for the active generation and close its (partial, undoable) transaction. */
	void CancelActiveGeneration();

	const FString& GetActiveGenerationId() const { return ActiveGenerationId; }

	/** True while EnsureBackend still has connection attempts in flight or queued. */
	bool IsConnectRetryPending() const
	{
		return ReconnectTickerHandle.IsValid() || ConnectAttemptsRemaining > 0;
	}

private:
	TSharedRef<SDockTab> SpawnPanelTab(const FSpawnTabArgs& Args);
	void RegisterMenus();

	void TryConnect();
	void HandleConnectionError(const FString& Reason);

	// Generation lifecycle -> spawner wiring (panel listens separately for UI updates).
	void HandleChunk(const FAetherForgeChunkMessage& Chunk);
	void HandleComplete(const FAetherForgeCompleteMessage& Complete);
	void HandleServerError(const FAetherForgeErrorMessage& Error);
	void HandleClosed(int32 StatusCode, const FString& Reason);

	TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> Client;
	TSharedPtr<FAetherForgeSidecarManager> SidecarManager;
	TSharedPtr<FAetherForgeSpawner> Spawner;
	TStrongObjectPtr<UAetherForgeManifest> Manifest;

	FString ActiveGenerationId;
	int32 ConnectAttemptsRemaining = 0;
	FTSTicker::FDelegateHandle ReconnectTickerHandle;
};
