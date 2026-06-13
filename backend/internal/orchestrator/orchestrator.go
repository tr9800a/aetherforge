// Package orchestrator runs the generation pipeline for one connection:
//
//	generate → LLM recipe (validated, one retry) → deterministic placement
//	→ plan → chunk… → complete   (or a structured error)
//
// Concurrency model (spec §5): one generation per connection at a time; a new
// generate while one is running cancels the old one. Each generation runs in
// its own goroutine tied to a cancellable context; client cancel (or socket
// close) cancels that context, stopping the LLM call and the streaming loop.
package orchestrator

import (
	"context"
	"errors"
	"fmt"
	"log"
	"math/rand/v2"
	"strings"
	"sync"
	"time"

	"aetherforge/backend/internal/llm"
	"aetherforge/backend/internal/manifest"
	"aetherforge/backend/internal/placement"
	"aetherforge/backend/internal/protocol"
)

// Hard caps clamp runaway LLM counts (spec §3.1, §5).
const (
	MaxAssetsPerCategory = 2000
	MaxAssetsTotal       = 5000
	// DefaultChunkSize is how many asset entries go into each chunk
	// message (~50 per spec).
	DefaultChunkSize = 50
	// DefaultLLMTimeout bounds a single LLM call (local models stall).
	DefaultLLMTimeout = 60 * time.Second
)

// Sender delivers server → client protocol messages. The server implements
// it over the WebSocket; tests implement it with channels.
type Sender interface {
	Send(msg any) error
}

// Orchestrator owns the generation lifecycle for a single connection.
type Orchestrator struct {
	llm        llm.Client
	store      *manifest.Store
	sender     Sender
	chunkSize  int
	llmTimeout time.Duration

	mu  sync.Mutex
	cur *generation
}

type generation struct {
	id     string
	cancel context.CancelFunc
	done   chan struct{}
}

// New builds an orchestrator for one connection.
func New(client llm.Client, store *manifest.Store, sender Sender) *Orchestrator {
	return &Orchestrator{
		llm:        client,
		store:      store,
		sender:     sender,
		chunkSize:  DefaultChunkSize,
		llmTimeout: DefaultLLMTimeout,
	}
}

// SetChunkSize overrides the per-chunk asset count (tests).
func (o *Orchestrator) SetChunkSize(n int) {
	if n > 0 {
		o.chunkSize = n
	}
}

// Generate starts a new generation, cancelling and waiting out any in-flight
// one first so streams never interleave. It returns once the new generation
// goroutine is launched.
func (o *Orchestrator) Generate(parent context.Context, req *protocol.Generate) {
	o.mu.Lock()
	old := o.cur
	o.cur = nil
	o.mu.Unlock()
	if old != nil {
		old.cancel()
		<-old.done
	}

	ctx, cancel := context.WithCancel(parent)
	g := &generation{id: req.GenerationID, cancel: cancel, done: make(chan struct{})}
	o.mu.Lock()
	o.cur = g
	o.mu.Unlock()

	go func() {
		defer close(g.done)
		defer cancel()
		o.run(ctx, req)
		o.mu.Lock()
		if o.cur == g {
			o.cur = nil
		}
		o.mu.Unlock()
	}()
}

// Cancel cancels the in-flight generation if its id matches (or
// unconditionally for an empty id). It does not wait for the goroutine.
func (o *Orchestrator) Cancel(generationID string) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.cur != nil && (generationID == "" || o.cur.id == generationID) {
		o.cur.cancel()
	}
}

// Shutdown cancels any in-flight generation and waits for it to finish.
// Called when the connection closes.
func (o *Orchestrator) Shutdown() {
	o.mu.Lock()
	cur := o.cur
	o.cur = nil
	o.mu.Unlock()
	if cur != nil {
		cur.cancel()
		<-cur.done
	}
}

// run executes the full pipeline for one generation. On cancellation it
// simply stops emitting (no complete follows a cancellation, per contract).
// Every stage logs its duration so a session is fully reconstructable from
// the server log (live the sidecar appends to a file via -logfile).
func (o *Orchestrator) run(ctx context.Context, req *protocol.Generate) {
	start := time.Now()
	log.Printf("generation %s: prompt %.120q", req.GenerationID, req.Prompt)

	if !o.store.Ready() {
		o.sendError(req.GenerationID, protocol.CodeInternal,
			"no manifest: hello handshake must precede generate", true)
		return
	}
	man := o.store.Snapshot()

	// --- LLM: one call, one retry with the validation error fed back ---
	llmStart := time.Now()
	recipe, err := o.recipeWithRetry(ctx, req, man)
	llmElapsed := time.Since(llmStart)
	if err != nil {
		log.Printf("generation %s: recipe failed after %s: %v", req.GenerationID, llmElapsed.Round(time.Millisecond), err)
		o.reportRecipeError(ctx, req.GenerationID, err)
		return
	}
	clampCounts(recipe)
	log.Printf("generation %s: recipe in %s: %s", req.GenerationID, llmElapsed.Round(time.Millisecond), recipeSummary(recipe))

	// --- Seed: echo the request's, or pick one (echoed in plan) ---
	var seed int64
	if req.Seed != nil {
		seed = *req.Seed
	} else {
		// Stay within JSON's exact-integer range (|seed| < 2^53): the seed is echoed
		// in plan and the UE client parses JSON numbers through a double, so a wider
		// seed would round and break exact reproducibility. 2^53 seeds is ample.
		seed = int64(rand.Uint64() >> 11) // top 53 bits, non-negative
	}

	// --- Placement: pure and deterministic ---
	placeStart := time.Now()
	entries, err := placement.Place(recipe, seed, req.Bounds, man)
	if err != nil {
		log.Printf("generation %s: placement failed: %v", req.GenerationID, err)
		if ctx.Err() == nil {
			o.sendError(req.GenerationID, protocol.CodeInternal, err.Error(), true)
		}
		return
	}
	log.Printf("generation %s: placed %d assets in %s (seed %d)",
		req.GenerationID, len(entries), time.Since(placeStart).Round(time.Microsecond), seed)

	// --- plan → chunk… → complete, checking cancellation between sends ---
	categories := make(map[string]int, len(recipe.Items))
	for _, it := range recipe.Items {
		categories[it.Category] = it.Count
	}
	plan := &protocol.Plan{
		ProtocolVersion: protocol.Version,
		Type:            protocol.TypePlan,
		GenerationID:    req.GenerationID,
		Seed:            seed,
		TotalAssets:     len(entries),
		Categories:      categories,
	}
	if !o.send(ctx, plan) {
		return
	}

	for off := 0; off < len(entries); off += o.chunkSize {
		end := min(off+o.chunkSize, len(entries))
		chunk := &protocol.Chunk{
			ProtocolVersion: protocol.Version,
			Type:            protocol.TypeChunk,
			GenerationID:    req.GenerationID,
			Assets:          entries[off:end],
		}
		if !o.send(ctx, chunk) {
			return
		}
	}

	complete := &protocol.Complete{
		ProtocolVersion: protocol.Version,
		Type:            protocol.TypeComplete,
		GenerationID:    req.GenerationID,
		Stats: protocol.Stats{
			Assets:    len(entries),
			ElapsedMS: time.Since(start).Milliseconds(),
			LLMMS:     llmElapsed.Milliseconds(),
		},
	}
	if o.send(ctx, complete) {
		log.Printf("generation %s: complete — %d assets in %s (llm %s)",
			req.GenerationID, len(entries), time.Since(start).Round(time.Millisecond), llmElapsed.Round(time.Millisecond))
	} else {
		log.Printf("generation %s: cancelled mid-stream", req.GenerationID)
	}
}

// recipeSummary renders a recipe as "deciduous_tree:60, shrub:30" for logs,
// in recipe (model) order.
func recipeSummary(r *llm.Recipe) string {
	var b strings.Builder
	for i, it := range r.Items {
		if i > 0 {
			b.WriteString(", ")
		}
		fmt.Fprintf(&b, "%s:%d", it.Category, it.Count)
	}
	return b.String()
}

// recipeWithRetry performs the LLM call with a timeout, validates the recipe
// against the manifest, and retries exactly once on schema-invalid output
// with the validation error fed back to the model.
func (o *Orchestrator) recipeWithRetry(ctx context.Context, req *protocol.Generate, man map[string]protocol.ManifestEntry) (*llm.Recipe, error) {
	priorError := ""
	var lastErr error
	for attempt := 0; attempt < 2; attempt++ {
		callCtx, cancel := context.WithTimeout(ctx, o.llmTimeout)
		recipe, err := o.llm.GenerateRecipe(callCtx, llm.Request{
			Prompt:     req.Prompt,
			Manifest:   man,
			PriorError: priorError,
		})
		cancel()
		if err == nil {
			err = validateRecipe(recipe, man)
			if err == nil {
				return recipe, nil
			}
		}
		// Cancellation / timeout: not retryable, surface immediately.
		var invalid *llm.InvalidOutputError
		if !errors.As(err, &invalid) {
			return nil, err
		}
		priorError = invalid.Detail
		lastErr = err
	}
	return nil, fmt.Errorf("after one retry: %w", lastErr)
}

// validateRecipe checks LLM output against the advertised manifest. Failures
// are *llm.InvalidOutputError so the retry path feeds them back to the model.
func validateRecipe(r *llm.Recipe, man map[string]protocol.ManifestEntry) error {
	if r == nil || len(r.Items) == 0 {
		return &llm.InvalidOutputError{Detail: "recipe is empty"}
	}
	seen := make(map[string]bool, len(r.Items))
	for _, it := range r.Items {
		if _, ok := man[it.Category]; !ok {
			return &llm.InvalidOutputError{Detail: fmt.Sprintf(
				"categories.%s is not a key in the advertised manifest", it.Category)}
		}
		if seen[it.Category] {
			return &llm.InvalidOutputError{Detail: fmt.Sprintf(
				"categories.%s appears more than once", it.Category)}
		}
		seen[it.Category] = true
		if it.Count < 0 {
			return &llm.InvalidOutputError{Detail: fmt.Sprintf(
				"categories.%s has negative count %d", it.Category, it.Count)}
		}
	}
	return nil
}

// clampCounts applies the hard caps: per-category first, then a proportional
// scale-down if the total still exceeds the global cap.
func clampCounts(r *llm.Recipe) {
	total := 0
	for i := range r.Items {
		if r.Items[i].Count > MaxAssetsPerCategory {
			r.Items[i].Count = MaxAssetsPerCategory
		}
		total += r.Items[i].Count
	}
	if total <= MaxAssetsTotal || total == 0 {
		return
	}
	scale := float64(MaxAssetsTotal) / float64(total)
	for i := range r.Items {
		c := int(float64(r.Items[i].Count) * scale)
		if r.Items[i].Count > 0 && c < 1 {
			c = 1
		}
		r.Items[i].Count = c
	}
}

// reportRecipeError maps an LLM/validation failure to a structured protocol
// error message. Cancellation produces no message at all.
func (o *Orchestrator) reportRecipeError(ctx context.Context, generationID string, err error) {
	if ctx.Err() != nil && !errors.Is(err, context.DeadlineExceeded) {
		return // generation cancelled: stay silent, per contract
	}
	code := protocol.CodeInternal
	msg := err.Error()
	var invalid *llm.InvalidOutputError
	switch {
	case errors.Is(err, context.DeadlineExceeded):
		code = protocol.CodeLLMTimeout
		msg = "LLM request timed out"
	case errors.As(err, &invalid):
		code = protocol.CodeLLMInvalidOutput
		msg = fmt.Sprintf("LLM output failed schema validation after one retry: %s", invalid.Detail)
	}
	o.sendError(generationID, code, msg, true)
}

// send delivers a message unless the generation has been cancelled. Returns
// false when streaming should stop.
func (o *Orchestrator) send(ctx context.Context, msg any) bool {
	select {
	case <-ctx.Done():
		return false
	default:
	}
	return o.sender.Send(msg) == nil
}

func (o *Orchestrator) sendError(generationID, code, message string, recoverable bool) {
	_ = o.sender.Send(&protocol.Error{
		ProtocolVersion: protocol.Version,
		Type:            protocol.TypeError,
		GenerationID:    generationID,
		Code:            code,
		Message:         message,
		Recoverable:     recoverable,
	})
}
