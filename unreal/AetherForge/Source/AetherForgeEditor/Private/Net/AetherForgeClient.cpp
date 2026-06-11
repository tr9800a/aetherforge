// Copyright AetherForge. All Rights Reserved.

#include "AetherForgeClient.h"

#include "Async/Async.h"
#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "WebSocketsModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogAetherForgeClient, Log, All);

FAetherForgeClient::~FAetherForgeClient()
{
	// Shutdown() should have run already (module shutdown); this is belt-and-braces.
	if (DispatchTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DispatchTickerHandle);
		DispatchTickerHandle.Reset();
	}
}

void FAetherForgeClient::Initialize()
{
	check(IsInGameThread());
	if (!DispatchTickerHandle.IsValid())
	{
		DispatchTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateSP(this, &FAetherForgeClient::DispatchTick));
	}
}

void FAetherForgeClient::Shutdown()
{
	check(IsInGameThread());
	Disconnect();
	if (DispatchTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DispatchTickerHandle);
		DispatchTickerHandle.Reset();
	}
}

void FAetherForgeClient::Connect(const FString& Url, const TMap<FString, FAetherForgeWireManifestEntry>& WireManifest)
{
	check(IsInGameThread());

	if (IsConnected())
	{
		UE_LOG(LogAetherForgeClient, Verbose, TEXT("Already connected; ignoring Connect."));
		return;
	}

	PendingHelloManifest = WireManifest;

	FWebSocketsModule& WebSocketsModule = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	Socket = WebSocketsModule.CreateWebSocket(Url, FString());

	// Socket callbacks are delivered on the game thread by the WebSockets module.
	// Weak captures: the socket may outlive a client torn down at module shutdown, and
	// the handlers compare WeakSocket against the current member to drop stale events.
	TWeakPtr<FAetherForgeClient, ESPMode::ThreadSafe> WeakThis(AsShared());
	TWeakPtr<IWebSocket> WeakSocket(Socket);

	Socket->OnConnected().AddLambda([WeakThis, WeakSocket]()
	{
		if (const TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> This = WeakThis.Pin())
		{
			This->HandleSocketConnected(WeakSocket.Pin());
		}
	});

	Socket->OnConnectionError().AddLambda([WeakThis, WeakSocket](const FString& Error)
	{
		if (const TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> This = WeakThis.Pin())
		{
			This->HandleSocketConnectionError(WeakSocket.Pin(), Error);
		}
	});

	Socket->OnClosed().AddLambda([WeakThis, WeakSocket](const int32 StatusCode, const FString& Reason, const bool bWasClean)
	{
		if (const TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> This = WeakThis.Pin())
		{
			This->HandleSocketClosed(WeakSocket.Pin(), StatusCode, Reason, bWasClean);
		}
	});

	Socket->OnMessage().AddLambda([WeakThis, WeakSocket](const FString& Message)
	{
		if (const TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> This = WeakThis.Pin())
		{
			This->HandleRawMessage(WeakSocket.Pin(), Message);
		}
	});

	UE_LOG(LogAetherForgeClient, Log, TEXT("Connecting to %s ..."), *Url);
	Socket->Connect();
}

void FAetherForgeClient::Disconnect()
{
	if (Socket.IsValid())
	{
		// Unbind before closing: a queued OnClosed/OnConnectionError from this socket
		// must not fire into a client that may already own a replacement connection.
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnClosed().Clear();
		Socket->OnMessage().Clear();
		if (Socket->IsConnected())
		{
			// Server treats socket close as cancellation of any in-flight generation.
			Socket->Close();
		}
		Socket.Reset();
	}
}

bool FAetherForgeClient::IsConnected() const
{
	return Socket.IsValid() && Socket->IsConnected();
}

void FAetherForgeClient::SendGenerate(const FString& GenerationId, const FString& Prompt,
	const TOptional<int64>& Seed, const FAetherForgeWireBounds& Bounds)
{
	check(IsInGameThread());
	if (!IsConnected())
	{
		UE_LOG(LogAetherForgeClient, Warning, TEXT("SendGenerate while disconnected; dropped."));
		return;
	}
	Socket->Send(FAetherForgeProtocol::SerializeGenerate(GenerationId, Prompt, Seed, Bounds));
}

void FAetherForgeClient::SendCancel(const FString& GenerationId)
{
	check(IsInGameThread());
	if (!IsConnected())
	{
		UE_LOG(LogAetherForgeClient, Warning, TEXT("SendCancel while disconnected; dropped."));
		return;
	}
	Socket->Send(FAetherForgeProtocol::SerializeCancel(GenerationId));
}

void FAetherForgeClient::HandleSocketConnected(const TSharedPtr<IWebSocket>& FromSocket)
{
	if (!FromSocket.IsValid() || FromSocket != Socket)
	{
		return; // stale event from a replaced connection
	}
	// hello is sent exactly once per connection, before anything else.
	FromSocket->Send(FAetherForgeProtocol::SerializeHello(PendingHelloManifest));
	UE_LOG(LogAetherForgeClient, Log, TEXT("Connected; hello sent (%d manifest categories)."),
		PendingHelloManifest.Num());
	ConnectedEvent.Broadcast();
}

void FAetherForgeClient::HandleSocketConnectionError(const TSharedPtr<IWebSocket>& FromSocket, const FString& Error)
{
	if (!FromSocket.IsValid() || FromSocket != Socket)
	{
		return; // stale event from a replaced connection
	}
	UE_LOG(LogAetherForgeClient, Warning, TEXT("Connection error: %s"), *Error);
	Socket.Reset();
	ConnectionErrorEvent.Broadcast(Error);
}

void FAetherForgeClient::HandleSocketClosed(const TSharedPtr<IWebSocket>& FromSocket, const int32 StatusCode, const FString& Reason, const bool bWasClean)
{
	if (!FromSocket.IsValid() || FromSocket != Socket)
	{
		return; // stale event from a replaced connection
	}
	UE_LOG(LogAetherForgeClient, Log, TEXT("Socket closed (code %d, clean=%d): %s"),
		StatusCode, bWasClean ? 1 : 0, *Reason);
	Socket.Reset();
	ClosedEvent.Broadcast(StatusCode, Reason);
}

void FAetherForgeClient::HandleRawMessage(const TSharedPtr<IWebSocket>& FromSocket, const FString& Message)
{
	if (!FromSocket.IsValid() || FromSocket != Socket)
	{
		return; // frame from a replaced connection — drop it
	}
	RawInbound.Enqueue(Message);

	// Kick a parse worker if none is active. Exactly one worker drains the queue at a
	// time so frames are parsed (and therefore dispatched) in arrival order.
	bool bExpected = false;
	if (bParseWorkerActive.compare_exchange_strong(bExpected, true))
	{
		TWeakPtr<FAetherForgeClient, ESPMode::ThreadSafe> WeakThis(AsShared());
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [WeakThis]()
		{
			if (const TSharedPtr<FAetherForgeClient, ESPMode::ThreadSafe> This = WeakThis.Pin())
			{
				This->ParseWorker();
			}
		});
	}
}

void FAetherForgeClient::ParseWorker()
{
	// Background thread. Touches only thread-safe queues and POD-ish structs — no
	// UObjects, no Slate (the mechanical rule: parse off-thread, everything else on
	// the game thread).
	for (;;)
	{
		FString Raw;
		while (RawInbound.Dequeue(Raw))
		{
			TSharedPtr<FAetherForgeServerMessage> Parsed = MakeShared<FAetherForgeServerMessage>();
			FAetherForgeProtocol::ParseServerMessage(Raw, *Parsed);
			ParsedInbound.Enqueue(MoveTemp(Parsed));
		}

		bParseWorkerActive.store(false);

		// Close the race where a frame was enqueued after the final Dequeue but before
		// the flag was cleared: if work remains and we can re-acquire the flag, loop.
		bool bExpected = false;
		if (RawInbound.IsEmpty() || !bParseWorkerActive.compare_exchange_strong(bExpected, true))
		{
			break;
		}
	}
}

bool FAetherForgeClient::DispatchTick(float /*DeltaTime*/)
{
	check(IsInGameThread());

	TSharedPtr<FAetherForgeServerMessage> Message;
	while (ParsedInbound.Dequeue(Message))
	{
		switch (Message->Type)
		{
		case EAetherForgeServerMessageType::Plan:
			PlanEvent.Broadcast(Message->Plan);
			break;
		case EAetherForgeServerMessageType::Chunk:
			ChunkEvent.Broadcast(Message->Chunk);
			break;
		case EAetherForgeServerMessageType::Complete:
			CompleteEvent.Broadcast(Message->Complete);
			break;
		case EAetherForgeServerMessageType::Error:
			ServerErrorEvent.Broadcast(Message->Error);
			break;
		case EAetherForgeServerMessageType::Invalid:
		default:
			UE_LOG(LogAetherForgeClient, Error, TEXT("Protocol error: %s"), *Message->ParseError);
			ProtocolErrorEvent.Broadcast(Message->ParseError);
			break;
		}
	}
	return true; // keep ticking
}
