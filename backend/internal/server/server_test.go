package server

import (
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"aetherforge/backend/internal/llm"
	"aetherforge/backend/internal/protocol"
)

func dial(t *testing.T) (*websocket.Conn, func()) {
	t.Helper()
	srv := New("", &llm.FakeLLM{})
	ts := httptest.NewServer(srv.Handler())
	url := "ws" + strings.TrimPrefix(ts.URL, "http")
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		ts.Close()
		t.Fatalf("dial: %v", err)
	}
	return conn, func() {
		conn.Close()
		ts.Close()
	}
}

func helloMsg(version int) *protocol.Hello {
	return &protocol.Hello{
		ProtocolVersion: version,
		Type:            protocol.TypeHello,
		Manifest: map[string]protocol.ManifestEntry{
			"shrub": {FootprintRadius: 75, GroundSnap: true, Instanceable: true, DisplayName: "Shrub"},
		},
	}
}

func readMsg(t *testing.T, conn *websocket.Conn) any {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	_, data, err := conn.ReadMessage()
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	msg, err := protocol.Decode(data)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return msg
}

// TestHandshakeAndGenerate covers the Phase 1 acceptance criterion: a client
// sends hello + generate over a real WebSocket and receives a complete,
// schema-valid plan → chunk… → complete stream.
func TestHandshakeAndGenerate(t *testing.T) {
	conn, done := dial(t)
	defer done()

	if err := conn.WriteJSON(helloMsg(protocol.Version)); err != nil {
		t.Fatal(err)
	}
	seed := int64(1337)
	gen := &protocol.Generate{
		ProtocolVersion: protocol.Version,
		Type:            protocol.TypeGenerate,
		GenerationID:    "gen-ws-1",
		Prompt:          "a shrubbery",
		Seed:            &seed,
		Bounds: protocol.Bounds{
			Min: protocol.Vec2{X: -5000, Y: -5000},
			Max: protocol.Vec2{X: 5000, Y: 5000},
		},
	}
	if err := conn.WriteJSON(gen); err != nil {
		t.Fatal(err)
	}

	plan, ok := readMsg(t, conn).(*protocol.Plan)
	if !ok || plan.GenerationID != "gen-ws-1" || plan.Seed != 1337 {
		t.Fatalf("expected plan for gen-ws-1 with seed 1337, got %+v", plan)
	}

	streamed := 0
	for {
		switch m := readMsg(t, conn).(type) {
		case *protocol.Chunk:
			streamed += len(m.Assets)
		case *protocol.Complete:
			if streamed != plan.TotalAssets || m.Stats.Assets != plan.TotalAssets {
				t.Fatalf("streamed %d, complete says %d, plan said %d",
					streamed, m.Stats.Assets, plan.TotalAssets)
			}
			return
		default:
			t.Fatalf("unexpected message: %+v", m)
		}
	}
}

// TestHandshakeVersionMismatch: a hello with the wrong protocol_version fails
// loudly — error with code protocol_version_mismatch, recoverable false, then
// connection close.
func TestHandshakeVersionMismatch(t *testing.T) {
	conn, done := dial(t)
	defer done()

	if err := conn.WriteJSON(helloMsg(99)); err != nil {
		t.Fatal(err)
	}
	e, ok := readMsg(t, conn).(*protocol.Error)
	if !ok {
		t.Fatalf("expected error message, got %T", e)
	}
	if e.Code != protocol.CodeProtocolVersionMismatch || e.Recoverable || e.GenerationID != "" {
		t.Fatalf("error mismatch: %+v", e)
	}
	// The server must close the connection after the handshake failure.
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, _, err := conn.ReadMessage(); err == nil {
		t.Fatal("connection should be closed after version mismatch")
	}
}

// TestHandshakeRequiresHelloFirst: any non-hello first frame is rejected and
// the connection closed.
func TestHandshakeRequiresHelloFirst(t *testing.T) {
	conn, done := dial(t)
	defer done()

	cancel := &protocol.Cancel{ProtocolVersion: protocol.Version, Type: protocol.TypeCancel, GenerationID: "g"}
	if err := conn.WriteJSON(cancel); err != nil {
		t.Fatal(err)
	}
	e, ok := readMsg(t, conn).(*protocol.Error)
	if !ok || e.Code != protocol.CodeExpectedHello || e.Recoverable {
		t.Fatalf("expected non-recoverable expected_hello error, got %+v", e)
	}
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, _, err := conn.ReadMessage(); err == nil {
		t.Fatal("connection should be closed when first message is not hello")
	}
}
