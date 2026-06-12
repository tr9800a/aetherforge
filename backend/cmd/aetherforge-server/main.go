// Command aetherforge-server is the Go orchestration sidecar launched and
// managed by the AetherForge UE5 editor plugin. It serves the v1 WebSocket
// protocol on 127.0.0.1:8080 (localhost only).
//
// LLM backends (-llm): "ollama" (default; local model via Ollama with
// schema-constrained output) or "fake" (canned recipes, no model needed).
// -record/-replay capture and replay recipes for deterministic demos and CI.
package main

import (
	"flag"
	"io"
	"log"
	"os"

	"aetherforge/backend/internal/llm"
	"aetherforge/backend/internal/server"
)

func main() {
	addr := flag.String("addr", server.DefaultAddr, "listen address (localhost only)")
	llmMode := flag.String("llm", "ollama", "llm backend: ollama|fake")
	ollamaURL := flag.String("ollama-url", llm.DefaultOllamaURL, "Ollama server base URL")
	ollamaModel := flag.String("ollama-model", llm.DefaultOllamaModel, "Ollama model name (must be pulled)")
	recordDir := flag.String("record", "", "record LLM recipes to this directory")
	replayDir := flag.String("replay", "", "replay LLM recipes from this directory (no model needed; overrides -llm)")
	logFile := flag.String("logfile", "", "also append logs to this file (the UE sidecar manager points this at the project's Saved/Logs)")
	flag.Parse()

	// The plugin launches the sidecar hidden, so stderr goes nowhere; -logfile
	// is how a live session stays diagnosable after the fact.
	if *logFile != "" {
		f, err := os.OpenFile(*logFile, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
		if err != nil {
			log.Printf("cannot open -logfile %s: %v (continuing on stderr only)", *logFile, err)
		} else {
			defer f.Close()
			log.SetOutput(io.MultiWriter(os.Stderr, f))
		}
	}

	var client llm.Client
	switch {
	case *replayDir != "":
		client = &llm.ReplayClient{Dir: *replayDir}
		log.Printf("llm: replaying recorded recipes from %s", *replayDir)
	case *llmMode == "ollama":
		client = &llm.OllamaClient{BaseURL: *ollamaURL, Model: *ollamaModel}
		log.Printf("llm: ollama at %s, model %s", *ollamaURL, *ollamaModel)
	case *llmMode == "fake":
		client = &llm.FakeLLM{}
		log.Printf("llm: fake (canned recipes)")
	default:
		log.Fatalf("unknown -llm mode %q (want ollama or fake)", *llmMode)
	}
	if *recordDir != "" {
		client = &llm.RecordingClient{Inner: client, Dir: *recordDir}
		log.Printf("llm: recording recipes to %s", *recordDir)
	}

	srv := server.New(*addr, client)
	if err := srv.ListenAndServe(); err != nil {
		log.Fatal(err)
	}
}
