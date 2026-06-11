// Package llm defines the LLM client interface used by the orchestrator to
// turn a natural-language prompt into a semantic scene recipe, plus the
// recipe types themselves.
//
// Per the spec, the LLM is used only for what it is good at — translating
// intent into a small semantic recipe. Everything spatial is deterministic
// code downstream. The client sits behind an interface so the full pipeline
// runs in tests with no model loaded (FakeLLM); a future Ollama-backed
// implementation (schema-constrained output, request timeout, record/replay)
// slots in by implementing Client.
package llm

import (
	"context"
	"fmt"

	"aetherforge/backend/internal/protocol"
)

// Recipe is the LLM's output: a small semantic plan for the scene. It names
// only semantic categories from the manifest advertised at hello — never
// asset paths — and carries no spatial coordinates beyond hints.
type Recipe struct {
	// Items is ordered (slice, not map) so downstream consumers are
	// deterministic with no map-iteration-order hazards.
	Items []CategoryRecipe
}

// CategoryRecipe is the per-category portion of a recipe.
type CategoryRecipe struct {
	// Category is a semantic key; must exist in the hello manifest.
	Category string
	// Count is how many instances to place (clamped by orchestrator caps).
	Count int
	// Clustering in [0, 1] is a scatter hint: 0 = uniform scatter (e.g.
	// creatures, which want clear space), 1 = strongly clumped (e.g. flora).
	Clustering float64
	// Tags are semantic tags copied onto every placed entry of this
	// category (e.g. "flora", "canopy"). May be empty.
	Tags []string
}

// Request is the input to a recipe generation call.
type Request struct {
	// Prompt is the designer's natural-language scene description.
	Prompt string
	// Manifest is the asset catalog from hello; the LLM may only select
	// from these keys.
	Manifest map[string]protocol.ManifestEntry
	// PriorError is non-empty on the single retry after schema-invalid
	// output: the validation error is fed back to the model.
	PriorError string
}

// Client generates a scene recipe from a prompt. Implementations must honor
// ctx cancellation (local models stall; the orchestrator applies timeouts and
// cancels on a new generate or client cancel).
type Client interface {
	GenerateRecipe(ctx context.Context, req Request) (*Recipe, error)
}

// InvalidOutputError reports LLM output that failed schema validation. The
// orchestrator retries exactly once with Detail fed back via
// Request.PriorError; a second failure produces a structured protocol error
// (code "llm_invalid_output", recoverable).
type InvalidOutputError struct {
	Detail string
}

func (e *InvalidOutputError) Error() string {
	return fmt.Sprintf("llm: output failed schema validation: %s", e.Detail)
}
