// Package server exposes the WebSocket endpoint the UE5 plugin connects to.
// It enforces the hello handshake (protocol_version check, manifest intake)
// and routes generate/cancel messages to a per-connection orchestrator.
package server

import (
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"aetherforge/backend/internal/llm"
	"aetherforge/backend/internal/manifest"
	"aetherforge/backend/internal/orchestrator"
	"aetherforge/backend/internal/protocol"
)

// DefaultAddr binds localhost only: the sidecar is reachable solely by the
// editor process on the same machine.
const DefaultAddr = "127.0.0.1:8080"

var upgrader = websocket.Upgrader{
	ReadBufferSize:  64 << 10,
	WriteBufferSize: 64 << 10,
	// Localhost sidecar: the UE client sends no browser Origin header.
	CheckOrigin: func(r *http.Request) bool { return true },
}

// Server hosts the WebSocket handler.
type Server struct {
	addr string
	llm  llm.Client

	httpServer *http.Server
}

// New creates a Server bound to addr (DefaultAddr if empty), using client for
// recipe generation.
func New(addr string, client llm.Client) *Server {
	if addr == "" {
		addr = DefaultAddr
	}
	return &Server{addr: addr, llm: client}
}

// Handler returns the HTTP handler (WebSocket upgrade on every path).
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", s.handleWS)
	return mux
}

// ListenAndServe blocks serving the WebSocket endpoint.
func (s *Server) ListenAndServe() error {
	ln, err := net.Listen("tcp", s.addr)
	if err != nil {
		return fmt.Errorf("server: listen %s: %w", s.addr, err)
	}
	log.Printf("aetherforge-server listening on ws://%s (protocol v%d)", s.addr, protocol.Version)
	s.httpServer = &http.Server{Handler: s.Handler()}
	return s.httpServer.Serve(ln)
}

// wsSender serializes concurrent writes to one WebSocket connection
// (gorilla/websocket allows at most one concurrent writer).
type wsSender struct {
	mu   sync.Mutex
	conn *websocket.Conn
}

func (w *wsSender) Send(msg any) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.conn.WriteJSON(msg)
}

func (s *Server) handleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("server: upgrade failed: %v", err)
		return
	}
	defer conn.Close()
	sender := &wsSender{conn: conn}

	// --- hello handshake: required first message, hard version check ---
	store := manifest.NewStore()
	if !s.handshake(conn, sender, store) {
		return
	}

	orch := orchestrator.New(s.llm, store, sender)
	defer orch.Shutdown() // socket close cancels any in-flight generation

	for {
		_, data, err := conn.ReadMessage()
		if err != nil {
			return
		}
		msg, err := protocol.Decode(data)
		if err != nil {
			if errors.Is(err, protocol.ErrVersionMismatch) {
				sendError(sender, "", protocol.CodeProtocolVersionMismatch, err.Error(), false)
				return
			}
			sendError(sender, "", protocol.CodeInternal, err.Error(), true)
			continue
		}
		switch m := msg.(type) {
		case *protocol.Generate:
			orch.Generate(r.Context(), m)
		case *protocol.Cancel:
			orch.Cancel(m.GenerationID)
		case *protocol.Hello:
			sendError(sender, "", protocol.CodeInternal,
				"hello is sent exactly once per connection, before anything else", true)
		default:
			sendError(sender, "", protocol.CodeInternal,
				"unexpected client message type", true)
		}
	}
}

// handshake reads the first frame, which must be a hello with a matching
// protocol_version and a non-empty manifest. On failure it sends a
// non-recoverable error and reports false (caller closes the connection).
func (s *Server) handshake(conn *websocket.Conn, sender *wsSender, store *manifest.Store) bool {
	_ = conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	_, data, err := conn.ReadMessage()
	if err != nil {
		return false
	}
	_ = conn.SetReadDeadline(time.Time{})

	msg, err := protocol.Decode(data)
	if err != nil {
		// protocol.Decode validates protocol_version on every message, so a
		// version mismatch at hello surfaces here — fail loudly with the
		// dedicated code, per the contract.
		code := protocol.CodeExpectedHello
		if errors.Is(err, protocol.ErrVersionMismatch) {
			code = protocol.CodeProtocolVersionMismatch
		}
		sendError(sender, "", code, err.Error(), false)
		return false
	}
	hello, ok := msg.(*protocol.Hello)
	if !ok {
		sendError(sender, "", protocol.CodeExpectedHello,
			"first message on a connection must be hello", false)
		return false
	}
	if len(hello.Manifest) == 0 {
		sendError(sender, "", protocol.CodeExpectedHello,
			"hello.manifest must contain at least one category", false)
		return false
	}
	store.Set(hello.Manifest)
	return true
}

func sendError(s *wsSender, generationID, code, message string, recoverable bool) {
	err := s.Send(&protocol.Error{
		ProtocolVersion: protocol.Version,
		Type:            protocol.TypeError,
		GenerationID:    generationID,
		Code:            code,
		Message:         message,
		Recoverable:     recoverable,
	})
	if err != nil && !errors.Is(err, net.ErrClosed) {
		log.Printf("server: send error message failed: %v", err)
	}
}
