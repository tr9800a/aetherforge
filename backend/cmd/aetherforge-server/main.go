// Command aetherforge-server is the Go orchestration sidecar launched and
// managed by the AetherForge UE5 editor plugin. It serves the v1 WebSocket
// protocol on 127.0.0.1:8080 (localhost only).
//
// Phase 1 scaffold: the pipeline runs against the canned FakeLLM (mock
// recipes, real deterministic placement). The Ollama-backed llm.Client
// implementation slots in behind the same interface in Phase 4.
package main

import (
	"flag"
	"log"

	"aetherforge/backend/internal/llm"
	"aetherforge/backend/internal/server"
)

func main() {
	addr := flag.String("addr", server.DefaultAddr, "listen address (localhost only)")
	flag.Parse()

	// Phase 1: fake LLM with canned scene recipes. Swap for the Ollama
	// client (Phase 4) without touching the orchestrator or server.
	var client llm.Client = &llm.FakeLLM{}

	srv := server.New(*addr, client)
	if err := srv.ListenAndServe(); err != nil {
		log.Fatal(err)
	}
}
