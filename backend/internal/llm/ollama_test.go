package llm

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"aetherforge/backend/internal/protocol"
)

func testManifest() map[string]protocol.ManifestEntry {
	return map[string]protocol.ManifestEntry{
		"deciduous_tree": {FootprintRadius: 150, GroundSnap: true, Instanceable: true, DisplayName: "Deciduous Tree"},
		"dog_large":      {FootprintRadius: 80, GroundSnap: true, Instanceable: false, DisplayName: "Large Dog"},
	}
}

// ollamaStub fakes /api/chat, capturing the request and returning Content.
func ollamaStub(t *testing.T, content string, capture *ollamaChatRequest) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/api/chat" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		if capture != nil {
			if err := json.NewDecoder(r.Body).Decode(capture); err != nil {
				t.Errorf("decode request: %v", err)
			}
		}
		json.NewEncoder(w).Encode(ollamaChatResponse{
			Message: ollamaMessage{Role: "assistant", Content: content},
		})
	}))
}

func TestOllamaGenerateRecipe(t *testing.T) {
	content := `{"categories":[
		{"category":"deciduous_tree","count":80,"clustering":0.8,"tags":["flora"]},
		{"category":"dog_large","count":2,"clustering":0.0},
		{"category":"deciduous_tree","count":0,"clustering":0.5}
	]}`
	var got ollamaChatRequest
	srv := ollamaStub(t, content, &got)
	defer srv.Close()

	c := &OllamaClient{BaseURL: srv.URL, Model: "test-model"}
	recipe, err := c.GenerateRecipe(context.Background(), Request{
		Prompt:   "a forest with two large dogs",
		Manifest: testManifest(),
	})
	if err != nil {
		t.Fatalf("GenerateRecipe: %v", err)
	}

	// Zero-count entries are dropped.
	if len(recipe.Items) != 2 {
		t.Fatalf("want 2 items, got %d: %+v", len(recipe.Items), recipe.Items)
	}
	if recipe.Items[0].Category != "deciduous_tree" || recipe.Items[0].Count != 80 {
		t.Errorf("item 0 wrong: %+v", recipe.Items[0])
	}
	if recipe.Items[1].Category != "dog_large" || recipe.Items[1].Count != 2 {
		t.Errorf("item 1 wrong: %+v", recipe.Items[1])
	}

	// Request hygiene: model, non-streaming, schema enum from the manifest,
	// prompt present in the user message.
	if got.Model != "test-model" || got.Stream {
		t.Errorf("model/stream wrong: %+v", got)
	}
	schema := string(got.Format)
	for _, key := range []string{"deciduous_tree", "dog_large"} {
		if !strings.Contains(schema, key) {
			t.Errorf("schema enum missing %q: %s", key, schema)
		}
	}
	if len(got.Messages) != 2 || got.Messages[1].Role != "user" ||
		!strings.Contains(got.Messages[1].Content, "two large dogs") {
		t.Errorf("messages wrong: %+v", got.Messages)
	}
}

func TestOllamaPriorErrorFeedback(t *testing.T) {
	var got ollamaChatRequest
	srv := ollamaStub(t, `{"categories":[{"category":"dog_large","count":1,"clustering":0}]}`, &got)
	defer srv.Close()

	c := &OllamaClient{BaseURL: srv.URL}
	_, err := c.GenerateRecipe(context.Background(), Request{
		Prompt:     "a dog",
		Manifest:   testManifest(),
		PriorError: "categories.cactus is not a key in the advertised manifest",
	})
	if err != nil {
		t.Fatalf("GenerateRecipe: %v", err)
	}
	if !strings.Contains(got.Messages[1].Content, "cactus") {
		t.Errorf("prior error not fed back to the model: %q", got.Messages[1].Content)
	}
}

func TestOllamaInvalidOutput(t *testing.T) {
	srv := ollamaStub(t, `here are some trees!`, nil)
	defer srv.Close()

	c := &OllamaClient{BaseURL: srv.URL}
	_, err := c.GenerateRecipe(context.Background(), Request{Prompt: "x", Manifest: testManifest()})
	var invalid *InvalidOutputError
	if !errors.As(err, &invalid) {
		t.Fatalf("want InvalidOutputError, got %v", err)
	}
}

func TestOllamaHTTPError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, `{"error":"model not found"}`, http.StatusNotFound)
	}))
	defer srv.Close()

	c := &OllamaClient{BaseURL: srv.URL}
	_, err := c.GenerateRecipe(context.Background(), Request{Prompt: "x", Manifest: testManifest()})
	if err == nil || !strings.Contains(err.Error(), "404") {
		t.Fatalf("want HTTP 404 error, got %v", err)
	}
	var invalid *InvalidOutputError
	if errors.As(err, &invalid) {
		t.Fatalf("transport errors must not be InvalidOutputError (no retry-with-feedback): %v", err)
	}
}

func TestOllamaHonorsContext(t *testing.T) {
	block := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		<-block
	}))
	defer srv.Close()
	defer close(block)

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		c := &OllamaClient{BaseURL: srv.URL}
		_, err := c.GenerateRecipe(ctx, Request{Prompt: "x", Manifest: testManifest()})
		done <- err
	}()
	cancel()

	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("want context.Canceled, got %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("GenerateRecipe did not return after ctx cancel")
	}
}
