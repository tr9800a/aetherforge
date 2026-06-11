// Copyright AetherForge. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"

/** Lifecycle states of the Go orchestration sidecar process. */
enum class EAetherForgeSidecarState : uint8
{
	/** Never launched (initial state). */
	NotStarted,
	/** CreateProc issued; waiting for the process/WS endpoint to come up. */
	Starting,
	/** Process is alive (process-level health; WS connectability is the client's job). */
	Running,
	/** Process died unexpectedly (watchdog) or failed to launch. */
	Dead,
	/** The sidecar binary was not found under the plugin directory. */
	BinaryMissing,
	/** Terminated deliberately by the plugin (shutdown). */
	Stopped,
};

/**
 * Launches, health-checks, and terminates the Go sidecar (`aetherforge-server`) via
 * FPlatformProcess. The plugin owns the sidecar lifecycle end to end — designers never
 * touch a terminal — and never leaks the child process across editor sessions.
 *
 * Game-thread only.
 */
class FAetherForgeSidecarManager : public TSharedFromThis<FAetherForgeSidecarManager>
{
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnStateChanged, EAetherForgeSidecarState /*NewState*/);

	~FAetherForgeSidecarManager();

	/**
	 * Launch the sidecar binary with `-addr 127.0.0.1:<Port>`. No-op if already running.
	 * Transitions to Running on success, BinaryMissing/Dead on failure, and starts a
	 * 1 Hz watchdog that demotes to Dead if the process exits behind our back.
	 */
	void Launch(int32 Port = 8080);

	/** Terminate the child process (kill tree) and stop the watchdog. Safe to re-call. */
	void Terminate();

	/** Live process-level check (FPlatformProcess::IsProcRunning). */
	bool IsRunning() const;

	EAetherForgeSidecarState GetState() const { return State; }

	FOnStateChanged& OnStateChanged() { return StateChangedEvent; }

	/** Last human-readable failure description (empty when healthy). */
	const FString& GetLastError() const { return LastError; }

	/**
	 * Expected binary location: <PluginDir>/Binaries/Sidecar/aetherforge-server[.exe].
	 * The Go build drops its output here (see plugin README).
	 */
	static FString GetSidecarBinaryPath();

private:
	void SetState(EAetherForgeSidecarState NewState);
	bool WatchdogTick(float DeltaTime);

	FProcHandle ProcessHandle;
	uint32 ProcessId = 0;
	EAetherForgeSidecarState State = EAetherForgeSidecarState::NotStarted;
	FString LastError;
	FTSTicker::FDelegateHandle WatchdogHandle;
	FOnStateChanged StateChangedEvent;
};
