package llm

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"

	"aetherforge/backend/internal/protocol"
)

// Defaults for the local Ollama server (Phase 4, spec §3).
const (
	DefaultOllamaURL   = "http://127.0.0.1:11434"
	DefaultOllamaModel = "llama3.2:3b"
)

// OllamaClient implements Client against a local Ollama server's /api/chat
// endpoint. Output is schema-constrained via Ollama's structured-output
// "format" field — the schema's category enum is built from the advertised
// manifest, so the model cannot emit an unknown category at the grammar
// level. Validation downstream (orchestrator.validateRecipe) still applies.
//
// Timeouts and cancellation are the caller's job via ctx (the orchestrator
// applies the 60s timeout and one retry with Request.PriorError fed back).
type OllamaClient struct {
	// BaseURL of the Ollama server; DefaultOllamaURL if empty.
	BaseURL string
	// Model name (must already be pulled); DefaultOllamaModel if empty.
	Model string
	// HTTPClient overrides the transport; http.DefaultClient if nil. The
	// per-call deadline comes from ctx, never from the client.
	HTTPClient *http.Client
}

// Wire types for /api/chat (non-streaming).
type ollamaChatRequest struct {
	Model    string          `json:"model"`
	Messages []ollamaMessage `json:"messages"`
	Stream   bool            `json:"stream"`
	Format   json.RawMessage `json:"format,omitempty"`
	Options  map[string]any  `json:"options,omitempty"`
}

type ollamaMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type ollamaChatResponse struct {
	Message ollamaMessage `json:"message"`
}

// The recipe JSON the model is constrained to produce.
type recipeJSON struct {
	Categories []categoryJSON `json:"categories"`
}

type categoryJSON struct {
	Category   string   `json:"category"`
	Count      int      `json:"count"`
	Clustering float64  `json:"clustering"`
	Tags       []string `json:"tags"`
}

func (c *OllamaClient) GenerateRecipe(ctx context.Context, req Request) (*Recipe, error) {
	if len(req.Manifest) == 0 {
		return nil, fmt.Errorf("llm: empty manifest; nothing to select from")
	}
	keys := sortedManifestKeys(req.Manifest)

	body, err := json.Marshal(ollamaChatRequest{
		Model:    c.model(),
		Messages: buildMessages(req, keys, req.Manifest),
		Stream:   false,
		Format:   recipeSchema(keys),
		// Low temperature: recipes should be faithful to the prompt, not creative.
		Options: map[string]any{"temperature": 0.2},
	})
	if err != nil {
		return nil, fmt.Errorf("llm: marshal ollama request: %w", err)
	}

	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost,
		strings.TrimRight(c.baseURL(), "/")+"/api/chat", bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("llm: build ollama request: %w", err)
	}
	httpReq.Header.Set("Content-Type", "application/json")

	resp, err := c.httpClient().Do(httpReq)
	if err != nil {
		return nil, fmt.Errorf("llm: ollama request failed (is `ollama serve` running at %s?): %w", c.baseURL(), err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		detail, _ := io.ReadAll(io.LimitReader(resp.Body, 2048))
		return nil, fmt.Errorf("llm: ollama returned HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(detail)))
	}

	var out ollamaChatResponse
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, fmt.Errorf("llm: decode ollama response: %w", err)
	}

	var rj recipeJSON
	if err := json.Unmarshal([]byte(out.Message.Content), &rj); err != nil {
		// Schema-invalid model output: InvalidOutputError so the orchestrator
		// retries once with the detail fed back.
		return nil, &InvalidOutputError{Detail: fmt.Sprintf("model output is not valid recipe JSON: %v", err)}
	}

	items := make([]CategoryRecipe, 0, len(rj.Categories))
	for _, cat := range rj.Categories {
		if cat.Count <= 0 {
			continue // "none of these" — omit rather than carry zero entries
		}
		items = append(items, CategoryRecipe{
			Category:   cat.Category,
			Count:      cat.Count,
			Clustering: clamp01(cat.Clustering),
			Tags:       cat.Tags,
		})
	}
	return &Recipe{Items: items}, nil
}

func (c *OllamaClient) baseURL() string {
	if c.BaseURL != "" {
		return c.BaseURL
	}
	return DefaultOllamaURL
}

func (c *OllamaClient) model() string {
	if c.Model != "" {
		return c.Model
	}
	return DefaultOllamaModel
}

func (c *OllamaClient) httpClient() *http.Client {
	if c.HTTPClient != nil {
		return c.HTTPClient
	}
	return http.DefaultClient
}

// recipeSchema constrains generation at the grammar level: category is an
// enum of the advertised manifest keys, counts are non-negative integers,
// clustering is bounded [0,1].
func recipeSchema(keys []string) json.RawMessage {
	schema := map[string]any{
		"type": "object",
		"properties": map[string]any{
			"categories": map[string]any{
				"type": "array",
				"items": map[string]any{
					"type": "object",
					"properties": map[string]any{
						"category":   map[string]any{"type": "string", "enum": keys},
						"count":      map[string]any{"type": "integer", "minimum": 0},
						"clustering": map[string]any{"type": "number", "minimum": 0, "maximum": 1},
						"tags":       map[string]any{"type": "array", "items": map[string]any{"type": "string"}},
					},
					"required": []string{"category", "count", "clustering"},
				},
			},
		},
		"required": []string{"categories"},
	}
	raw, err := json.Marshal(schema)
	if err != nil {
		panic(err) // static structure; cannot fail
	}
	return raw
}

func buildMessages(req Request, keys []string, man map[string]protocol.ManifestEntry) []ollamaMessage {
	var catalog strings.Builder
	for _, k := range keys {
		fmt.Fprintf(&catalog, "- %s (%s)\n", k, man[k].DisplayName)
	}

	system := fmt.Sprintf(`You translate a level designer's scene description into a placement recipe for a game engine.

Available asset categories (you may ONLY use these keys; ignore anything in the scene that has no suitable category):
%s
For each category that belongs in the scene, output how many instances to place ("count") and how clumped they should be ("clustering": 0.0 = spread out evenly, like animals keeping their distance; 1.0 = tightly grouped, like trees in a dense stand). Optionally add short semantic "tags".

Be faithful to the description: respect explicit quantities ("two large dogs" means count 2), use sensible counts for vague ones ("a forest" might be 40-150 trees), and omit categories the scene does not call for. Respond with JSON only.`, catalog.String())

	messages := []ollamaMessage{{Role: "system", Content: system}}

	user := req.Prompt
	if req.PriorError != "" {
		user = fmt.Sprintf("%s\n\nYour previous answer was rejected: %s\nProduce a corrected recipe.", req.Prompt, req.PriorError)
	}
	return append(messages, ollamaMessage{Role: "user", Content: user})
}

func sortedManifestKeys(man map[string]protocol.ManifestEntry) []string {
	keys := make([]string, 0, len(man))
	for k := range man {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

func clamp01(v float64) float64 {
	if v < 0 {
		return 0
	}
	if v > 1 {
		return 1
	}
	return v
}
