// Copyright AetherForge. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AetherForgeProtocolTypes.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"

#include <atomic>

class IWebSocket;

/**
 * WebSocket client for the AetherForge sidecar (`ws://127.0.0.1:8080`, localhost only).
 *
 * Threading model (spec section 6):
 *  - IWebSocket events arrive on the game thread; the raw frame text is immediately
 *    handed to a background task (AsyncTask) for JSON parsing — parsing never runs on
 *    the game thread.
 *  - Exactly one parse worker drains the raw-frame queue at a time, preserving frame
 *    order (plan before first chunk, chunks in stream order).
 *  - Parsed, typed messages are marshaled back through a thread-safe queue and
 *    dispatched on the game thread by a core ticker — all delegates below fire on the
 *    game thread, in arrival order.
 */
class FAetherForgeClient : public TSharedFromThis<FAetherForgeClient, ESPMode::ThreadSafe>
{
public:
	static constexpr const TCHAR* DefaultUrl = TEXT("ws://127.0.0.1:8080");

	DECLARE_MULTICAST_DELEGATE(FOnConnectedEvent);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnConnectionErrorEvent, const FString& /*Reason*/);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnClosedEvent, int32 /*StatusCode*/, const FString& /*Reason*/);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnPlanEvent, const FAetherForgePlanMessage&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnChunkEvent, const FAetherForgeChunkMessage&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnCompleteEvent, const FAetherForgeCompleteMessage&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnServerErrorEvent, const FAetherForgeErrorMessage&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnProtocolErrorEvent, const FString& /*ParseError*/);

	~FAetherForgeClient();

	/** Register the game-thread dispatch ticker. Call once after construction. */
	void Initialize();

	/** Unregister the ticker and close any open socket. */
	void Shutdown();

	/**
	 * Open the socket and, on connect, send the hello handshake with the given wire
	 * manifest (sent exactly once per connection, before anything else).
	 */
	void Connect(const FString& Url, const TMap<FString, FAetherForgeWireManifestEntry>& WireManifest);

	/** Close the socket (server treats socket close as cancellation of any in-flight generation). */
	void Disconnect();

	bool IsConnected() const;

	/** Send a generate message. Seed unset => omitted; the server picks and echoes it in plan. */
	void SendGenerate(const FString& GenerationId, const FString& Prompt,
		const TOptional<int64>& Seed, const FAetherForgeWireBounds& Bounds);

	/** Send a cancel message for an in-flight generation. */
	void SendCancel(const FString& GenerationId);

	// All events fire on the game thread.
	FOnConnectedEvent& OnConnected() { return ConnectedEvent; }
	FOnConnectionErrorEvent& OnConnectionError() { return ConnectionErrorEvent; }
	FOnClosedEvent& OnClosed() { return ClosedEvent; }
	FOnPlanEvent& OnPlan() { return PlanEvent; }
	FOnChunkEvent& OnChunk() { return ChunkEvent; }
	FOnCompleteEvent& OnComplete() { return CompleteEvent; }
	FOnServerErrorEvent& OnServerError() { return ServerErrorEvent; }
	FOnProtocolErrorEvent& OnProtocolError() { return ProtocolErrorEvent; }

private:
	// Every handler receives the socket that raised the event and ignores it unless it
	// is still the current `Socket` — events from a socket that Disconnect()/Connect()
	// has already replaced must not touch the new connection.
	void HandleSocketConnected(const TSharedPtr<IWebSocket>& FromSocket);
	void HandleSocketConnectionError(const TSharedPtr<IWebSocket>& FromSocket, const FString& Error);
	void HandleSocketClosed(const TSharedPtr<IWebSocket>& FromSocket, int32 StatusCode, const FString& Reason, bool bWasClean);

	/** Game thread (socket callback): enqueue the raw frame and kick the parse worker. */
	void HandleRawMessage(const TSharedPtr<IWebSocket>& FromSocket, const FString& Message);

	/** Background thread: drain RawInbound -> parse -> ParsedInbound, preserving order. */
	void ParseWorker();

	/** Game thread (core ticker): drain ParsedInbound and broadcast typed delegates. */
	bool DispatchTick(float DeltaTime);

	TSharedPtr<IWebSocket> Socket;
	TMap<FString, FAetherForgeWireManifestEntry> PendingHelloManifest;

	/** Raw frames awaiting background parse. Producer: game thread. Consumer: parse worker. */
	TQueue<FString, EQueueMode::Mpsc> RawInbound;

	/** Parsed messages awaiting game-thread dispatch. Producer: parse worker. Consumer: game thread. */
	TQueue<TSharedPtr<FAetherForgeServerMessage>, EQueueMode::Mpsc> ParsedInbound;

	/** Guards the single-active-parse-worker invariant (frame ordering). */
	std::atomic<bool> bParseWorkerActive{false};

	FTSTicker::FDelegateHandle DispatchTickerHandle;

	FOnConnectedEvent ConnectedEvent;
	FOnConnectionErrorEvent ConnectionErrorEvent;
	FOnClosedEvent ClosedEvent;
	FOnPlanEvent PlanEvent;
	FOnChunkEvent ChunkEvent;
	FOnCompleteEvent CompleteEvent;
	FOnServerErrorEvent ServerErrorEvent;
	FOnProtocolErrorEvent ProtocolErrorEvent;
};
