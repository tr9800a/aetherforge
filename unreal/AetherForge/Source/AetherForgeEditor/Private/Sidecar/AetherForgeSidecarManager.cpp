// Copyright AetherForge. All Rights Reserved.

#include "AetherForgeSidecarManager.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogAetherForgeSidecar, Log, All);

namespace
{
	constexpr float WatchdogIntervalSeconds = 1.0f;
}

FAetherForgeSidecarManager::~FAetherForgeSidecarManager()
{
	Terminate();
}

FString FAetherForgeSidecarManager::GetSidecarBinaryPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AetherForge"));
	if (!Plugin.IsValid())
	{
		return FString();
	}

	FString BinaryName = TEXT("aetherforged");
#if PLATFORM_WINDOWS
	BinaryName += TEXT(".exe");
#endif

	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries"), TEXT("Sidecar"), BinaryName));
}

void FAetherForgeSidecarManager::Launch(const int32 Port)
{
	if (IsRunning())
	{
		UE_LOG(LogAetherForgeSidecar, Verbose, TEXT("Sidecar already running (pid %u); ignoring Launch."), ProcessId);
		return;
	}

	// Reset any stale handle from a previous (dead) instance.
	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::CloseProc(ProcessHandle);
	}

	LastError.Reset();
	SetState(EAetherForgeSidecarState::Starting);

	const FString BinaryPath = GetSidecarBinaryPath();
	if (BinaryPath.IsEmpty() || !FPaths::FileExists(BinaryPath))
	{
		LastError = FString::Printf(
			TEXT("Sidecar binary not found at '%s'. Build the Go service and copy it there (see plugin README)."),
			*BinaryPath);
		UE_LOG(LogAetherForgeSidecar, Error, TEXT("%s"), *LastError);
		SetState(EAetherForgeSidecarState::BinaryMissing);
		return;
	}

	const FString Params = FString::Printf(TEXT("--port %d"), Port);
	const FString WorkingDirectory = FPaths::GetPath(BinaryPath);

	UE_LOG(LogAetherForgeSidecar, Log, TEXT("Launching sidecar: %s %s"), *BinaryPath, *Params);

	ProcessHandle = FPlatformProcess::CreateProc(
		*BinaryPath,
		*Params,
		/*bLaunchDetached*/ true,
		/*bLaunchHidden*/ true,
		/*bLaunchReallyHidden*/ true,
		&ProcessId,
		/*PriorityModifier*/ 0,
		*WorkingDirectory,
		/*PipeWriteChild*/ nullptr,
		/*PipeReadChild*/ nullptr);

	if (!ProcessHandle.IsValid())
	{
		LastError = FString::Printf(TEXT("Failed to launch sidecar process '%s'."), *BinaryPath);
		UE_LOG(LogAetherForgeSidecar, Error, TEXT("%s"), *LastError);
		SetState(EAetherForgeSidecarState::Dead);
		return;
	}

	UE_LOG(LogAetherForgeSidecar, Log, TEXT("Sidecar launched (pid %u)."), ProcessId);
	SetState(EAetherForgeSidecarState::Running);

	// Watchdog: if the process dies behind our back, surface the error state in the
	// panel (this is half of the Phase 2 acceptance criterion).
	if (!WatchdogHandle.IsValid())
	{
		WatchdogHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateSP(this, &FAetherForgeSidecarManager::WatchdogTick),
			WatchdogIntervalSeconds);
	}
}

bool FAetherForgeSidecarManager::WatchdogTick(float /*DeltaTime*/)
{
	if (State == EAetherForgeSidecarState::Running && !IsRunning())
	{
		LastError = FString::Printf(TEXT("Sidecar process (pid %u) exited unexpectedly."), ProcessId);
		UE_LOG(LogAetherForgeSidecar, Error, TEXT("%s"), *LastError);
		SetState(EAetherForgeSidecarState::Dead);
	}
	return true; // keep ticking
}

bool FAetherForgeSidecarManager::IsRunning() const
{
	if (!ProcessHandle.IsValid())
	{
		return false;
	}
	// IsProcRunning takes a non-const ref; the handle itself is not mutated meaningfully.
	return FPlatformProcess::IsProcRunning(const_cast<FProcHandle&>(ProcessHandle));
}

void FAetherForgeSidecarManager::Terminate()
{
	if (WatchdogHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(WatchdogHandle);
		WatchdogHandle.Reset();
	}

	if (ProcessHandle.IsValid())
	{
		if (IsRunning())
		{
			UE_LOG(LogAetherForgeSidecar, Log, TEXT("Terminating sidecar (pid %u)."), ProcessId);
			FPlatformProcess::TerminateProc(ProcessHandle, /*KillTree*/ true);
		}
		FPlatformProcess::CloseProc(ProcessHandle);
		ProcessHandle = FProcHandle();
		ProcessId = 0;
		SetState(EAetherForgeSidecarState::Stopped);
	}
}

void FAetherForgeSidecarManager::SetState(const EAetherForgeSidecarState NewState)
{
	if (State != NewState)
	{
		State = NewState;
		StateChangedEvent.Broadcast(NewState);
	}
}
