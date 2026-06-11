package orchestrator

import (
	"context"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"aetherforge/backend/internal/llm"
	"aetherforge/backend/internal/manifest"
	"aetherforge/backend/internal/protocol"
)

// chanSender funnels every outbound message through an unbuffered channel so
// tests control pacing (and can cancel mid-stream deterministically).
type chanSender struct {
	ch chan any
}

func newChanSender() *chanSender { return &chanSender{ch: make(chan any)} }

func (s *chanSender) Send(msg any) error {
	s.ch <- msg
	return nil
}

// collectSender records messages without blocking.
type collectSender struct {
	mu   sync.Mutex
	msgs []any
}

func (s *collectSender) Send(msg any) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.msgs = append(s.msgs, msg)
	return nil
}

func (s *collectSender) snapshot() []any {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]any(nil), s.msgs...)
}

func fixtureStore() *manifest.Store {
	st := manifest.NewStore()
	st.Set(map[string]protocol.ManifestEntry{
		"deciduous_tree": {FootprintRadius: 250, GroundSnap: true, Instanceable: true, DisplayName: "Deciduous Tree"},
		"shrub":          {FootprintRadius: 75, GroundSnap: true, Instanceable: true, DisplayName: "Shrub"},
		"boulder":        {FootprintRadius: 120, GroundSnap: true, Instanceable: true, DisplayName: "Boulder"},
		"dog_large":      {FootprintRadius: 150, GroundSnap: true, Instanceable: false, DisplayName: "Large Dog"},
	})
	return st
}

func fixtureGenerate(id string, seed int64) *protocol.Generate {
	return &protocol.Generate{
		ProtocolVersion: protocol.Version,
		Type:            protocol.TypeGenerate,
		GenerationID:    id,
		Prompt:          "a forest clearing with Pacific Northwest deciduous trees, scattered shrubs, and two large dogs",
		Seed:            &seed,
		Bounds: protocol.Bounds{
			Min: protocol.Vec2{X: -5000, Y: -5000},
			Max: protocol.Vec2{X: 5000, Y: 5000},
		},
	}
}

func fixtureRecipe() *llm.Recipe {
	return &llm.Recipe{Items: []llm.CategoryRecipe{
		{Category: "deciduous_tree", Count: 80, Clustering: 0.7, Tags: []string{"flora", "canopy"}},
		{Category: "shrub", Count: 55, Clustering: 0.8, Tags: []string{"flora", "understory"}},
		{Category: "boulder", Count: 5, Clustering: 0.3, Tags: []string{"rock"}},
		{Category: "dog_large", Count: 2, Clustering: 0, Tags: []string{"fauna", "creature"}},
	}}
}

func waitDone(t *testing.T, o *Orchestrator) {
	t.Helper()
	deadline := time.After(5 * time.Second)
	for {
		o.mu.Lock()
		cur := o.cur
		o.mu.Unlock()
		if cur == nil {
			return
		}
		select {
		case <-deadline:
			t.Fatal("generation did not finish in time")
		case <-time.After(time.Millisecond):
		}
	}
}

// TestPipelineFullStream runs the orchestrator against the FakeLLM and
// asserts the full plan → chunk… → complete lifecycle with correct ids,
// versions, chunk sizing and totals.
func TestPipelineFullStream(t *testing.T) {
	sender := &collectSender{}
	o := New(&llm.FakeLLM{Recipe: fixtureRecipe()}, fixtureStore(), sender)

	o.Generate(context.Background(), fixtureGenerate("gen-test-1", 1337))
	waitDone(t, o)

	msgs := sender.snapshot()
	if len(msgs) < 3 {
		t.Fatalf("expected plan + chunks + complete, got %d messages", len(msgs))
	}

	plan, ok := msgs[0].(*protocol.Plan)
	if !ok {
		t.Fatalf("first message is %T, want *protocol.Plan", msgs[0])
	}
	if plan.GenerationID != "gen-test-1" || plan.ProtocolVersion != protocol.Version {
		t.Fatalf("plan header mismatch: %+v", plan)
	}
	if plan.Seed != 1337 {
		t.Fatalf("plan.seed = %d, want explicit request seed 1337", plan.Seed)
	}
	if plan.TotalAssets != 142 {
		t.Fatalf("plan.total_assets = %d, want 142", plan.TotalAssets)
	}
	sum := 0
	for _, c := range plan.Categories {
		sum += c
	}
	if sum != plan.TotalAssets {
		t.Fatalf("plan.categories sum %d != total_assets %d", sum, plan.TotalAssets)
	}

	complete, ok := msgs[len(msgs)-1].(*protocol.Complete)
	if !ok {
		t.Fatalf("last message is %T, want *protocol.Complete", msgs[len(msgs)-1])
	}
	if complete.GenerationID != "gen-test-1" || complete.Stats.Assets != 142 {
		t.Fatalf("complete mismatch: %+v", complete)
	}
	if complete.Stats.ElapsedMS < 0 || complete.Stats.LLMMS < 0 {
		t.Fatalf("negative stats: %+v", complete.Stats)
	}

	streamed := 0
	for i, m := range msgs[1 : len(msgs)-1] {
		chunk, ok := m.(*protocol.Chunk)
		if !ok {
			t.Fatalf("middle message %d is %T, want *protocol.Chunk", i, m)
		}
		if chunk.GenerationID != "gen-test-1" {
			t.Fatalf("chunk generation_id mismatch: %q", chunk.GenerationID)
		}
		if len(chunk.Assets) == 0 || len(chunk.Assets) > DefaultChunkSize {
			t.Fatalf("chunk %d has %d assets, want 1..%d", i, len(chunk.Assets), DefaultChunkSize)
		}
		streamed += len(chunk.Assets)
	}
	if streamed != plan.TotalAssets {
		t.Fatalf("streamed %d assets, plan said %d", streamed, plan.TotalAssets)
	}
	wantChunks := (plan.TotalAssets + DefaultChunkSize - 1) / DefaultChunkSize
	if got := len(msgs) - 2; got != wantChunks {
		t.Fatalf("got %d chunks, want %d", got, wantChunks)
	}
}

// TestPipelineDeterministicAcrossRuns: same prompt + same seed ⇒ identical
// asset stream, end to end through the orchestrator.
func TestPipelineDeterministicAcrossRuns(t *testing.T) {
	run := func() []any {
		sender := &collectSender{}
		o := New(&llm.FakeLLM{Recipe: fixtureRecipe()}, fixtureStore(), sender)
		o.Generate(context.Background(), fixtureGenerate("gen-d", 99))
		waitDone(t, o)
		return sender.snapshot()
	}
	a, b := run(), run()
	if len(a) != len(b) {
		t.Fatalf("run lengths differ: %d vs %d", len(a), len(b))
	}
	for i := range a {
		ca, aok := a[i].(*protocol.Chunk)
		cb, bok := b[i].(*protocol.Chunk)
		if aok != bok {
			t.Fatalf("message %d type mismatch", i)
		}
		if !aok {
			continue
		}
		if !reflect.DeepEqual(ca.Assets, cb.Assets) {
			t.Fatalf("chunk %d differs between identical runs", i)
		}
	}
}

// TestPipelinePicksSeedWhenOmitted: seed omitted on generate ⇒ server picks
// one and echoes it in plan.
func TestPipelinePicksSeedWhenOmitted(t *testing.T) {
	sender := &collectSender{}
	o := New(&llm.FakeLLM{Recipe: fixtureRecipe()}, fixtureStore(), sender)
	req := fixtureGenerate("gen-noseed", 0)
	req.Seed = nil
	o.Generate(context.Background(), req)
	waitDone(t, o)

	msgs := sender.snapshot()
	if len(msgs) == 0 {
		t.Fatal("no messages")
	}
	plan, ok := msgs[0].(*protocol.Plan)
	if !ok {
		t.Fatalf("first message is %T, want plan", msgs[0])
	}
	if plan.Seed < 0 {
		t.Fatalf("plan.seed = %d, want non-negative server-chosen seed", plan.Seed)
	}
	if _, ok := msgs[len(msgs)-1].(*protocol.Complete); !ok {
		t.Fatalf("last message is %T, want complete", msgs[len(msgs)-1])
	}
}

// TestCancellationMidStream cancels while chunks are flowing and asserts the
// stream stops with no complete (and no error) for that generation.
func TestCancellationMidStream(t *testing.T) {
	sender := newChanSender()
	o := New(&llm.FakeLLM{Recipe: fixtureRecipe()}, fixtureStore(), sender)
	o.SetChunkSize(10) // plenty of chunks to cancel between

	o.Generate(context.Background(), fixtureGenerate("gen-cancel", 1337))

	var got []any
	// Read plan + two chunks, then cancel.
	for i := 0; i < 3; i++ {
		select {
		case m := <-sender.ch:
			got = append(got, m)
		case <-time.After(5 * time.Second):
			t.Fatal("timed out waiting for stream")
		}
	}
	if _, ok := got[0].(*protocol.Plan); !ok {
		t.Fatalf("first message is %T, want plan", got[0])
	}
	o.Cancel("gen-cancel")

	// Drain whatever was already in flight; the goroutine must stop on its
	// own without emitting complete.
	deadline := time.After(5 * time.Second)
drain:
	for {
		o.mu.Lock()
		cur := o.cur
		o.mu.Unlock()
		select {
		case m := <-sender.ch:
			got = append(got, m)
		case <-time.After(5 * time.Millisecond):
			if cur == nil {
				break drain
			}
		case <-deadline:
			t.Fatal("generation did not stop after cancel")
		}
	}

	total := 142 / 10 // full stream would be 15 chunks
	chunks := 0
	for _, m := range got {
		switch m.(type) {
		case *protocol.Complete:
			t.Fatal("complete must not follow a cancellation")
		case *protocol.Error:
			t.Fatalf("cancellation must not produce an error message: %+v", m)
		case *protocol.Chunk:
			chunks++
		}
	}
	if chunks >= total {
		t.Fatalf("stream did not stop early: %d chunks delivered", chunks)
	}
}

// TestNewGenerateCancelsOld: issuing generate while one is in flight cancels
// the old generation; only the new one completes.
func TestNewGenerateCancelsOld(t *testing.T) {
	sender := newChanSender()
	o := New(&llm.FakeLLM{Recipe: fixtureRecipe()}, fixtureStore(), sender)
	o.SetChunkSize(10)

	o.Generate(context.Background(), fixtureGenerate("gen-old", 1))

	// Take the old generation's plan and one chunk, then preempt it.
	var preDrain []any
	for i := 0; i < 2; i++ {
		select {
		case m := <-sender.ch:
			preDrain = append(preDrain, m)
		case <-time.After(5 * time.Second):
			t.Fatal("timed out waiting for old stream")
		}
	}

	// Generate blocks until the old goroutine exits; it may be blocked on
	// an unbuffered send, so drain concurrently.
	var mu sync.Mutex
	var msgs []any
	msgs = append(msgs, preDrain...)
	stop := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for {
			select {
			case m := <-sender.ch:
				mu.Lock()
				msgs = append(msgs, m)
				mu.Unlock()
			case <-stop:
				return
			}
		}
	}()

	o.Generate(context.Background(), fixtureGenerate("gen-new", 2))
	waitDone(t, o)
	close(stop)
	wg.Wait()

	mu.Lock()
	defer mu.Unlock()
	sawNewComplete := false
	for _, m := range msgs {
		switch v := m.(type) {
		case *protocol.Complete:
			if v.GenerationID == "gen-old" {
				t.Fatal("cancelled old generation must not complete")
			}
			if v.GenerationID == "gen-new" {
				sawNewComplete = true
			}
		}
	}
	if !sawNewComplete {
		t.Fatal("new generation never completed")
	}
}

// TestCancellationDuringLLMCall: cancel while the (slow) LLM call is in
// flight stops the generation silently.
func TestCancellationDuringLLMCall(t *testing.T) {
	sender := &collectSender{}
	fake := &llm.FakeLLM{Recipe: fixtureRecipe(), Delay: 10 * time.Second}
	o := New(fake, fixtureStore(), sender)

	o.Generate(context.Background(), fixtureGenerate("gen-llm-cancel", 1))
	time.Sleep(20 * time.Millisecond) // let the goroutine enter the LLM call
	o.Cancel("gen-llm-cancel")
	waitDone(t, o)

	if msgs := sender.snapshot(); len(msgs) != 0 {
		t.Fatalf("cancelled-during-LLM generation emitted %d messages: %+v", len(msgs), msgs)
	}
}

// TestRetryOnInvalidOutput: first LLM response is schema-invalid; the
// orchestrator retries once with the validation error fed back, and the
// generation succeeds.
func TestRetryOnInvalidOutput(t *testing.T) {
	sender := &collectSender{}
	fake := &llm.FakeLLM{Recipe: fixtureRecipe(), InvalidAttempts: 1}
	o := New(fake, fixtureStore(), sender)

	o.Generate(context.Background(), fixtureGenerate("gen-retry", 5))
	waitDone(t, o)

	if fake.Calls != 2 {
		t.Fatalf("LLM called %d times, want 2 (one retry)", fake.Calls)
	}
	if fake.PriorErrors[0] != "" {
		t.Fatalf("first call must have empty PriorError, got %q", fake.PriorErrors[0])
	}
	if fake.PriorErrors[1] == "" {
		t.Fatal("retry must feed the validation error back to the model")
	}
	msgs := sender.snapshot()
	if _, ok := msgs[len(msgs)-1].(*protocol.Complete); !ok {
		t.Fatalf("generation should succeed after retry, last message: %T", msgs[len(msgs)-1])
	}
}

// TestErrorAfterRetryExhausted: two schema-invalid responses produce a
// structured, recoverable llm_invalid_output error.
func TestErrorAfterRetryExhausted(t *testing.T) {
	sender := &collectSender{}
	fake := &llm.FakeLLM{Recipe: fixtureRecipe(), InvalidAttempts: 2}
	o := New(fake, fixtureStore(), sender)

	o.Generate(context.Background(), fixtureGenerate("gen-fail", 5))
	waitDone(t, o)

	if fake.Calls != 2 {
		t.Fatalf("LLM called %d times, want exactly 2 (one retry, no more)", fake.Calls)
	}
	msgs := sender.snapshot()
	if len(msgs) != 1 {
		t.Fatalf("want exactly one error message, got %d messages", len(msgs))
	}
	e, ok := msgs[0].(*protocol.Error)
	if !ok {
		t.Fatalf("got %T, want *protocol.Error", msgs[0])
	}
	if e.Code != protocol.CodeLLMInvalidOutput || !e.Recoverable || e.GenerationID != "gen-fail" {
		t.Fatalf("error mismatch: %+v", e)
	}
}

// TestErrorOnUnknownCategory: a recipe naming a category outside the
// advertised manifest is schema-invalid output (fixture error.json wording).
func TestErrorOnUnknownCategory(t *testing.T) {
	sender := &collectSender{}
	bad := &llm.Recipe{Items: []llm.CategoryRecipe{{Category: "cactus", Count: 3}}}
	o := New(&llm.FakeLLM{Recipe: bad}, fixtureStore(), sender)

	o.Generate(context.Background(), fixtureGenerate("gen-cactus", 5))
	waitDone(t, o)

	msgs := sender.snapshot()
	if len(msgs) != 1 {
		t.Fatalf("want exactly one error message, got %d", len(msgs))
	}
	e := msgs[0].(*protocol.Error)
	if e.Code != protocol.CodeLLMInvalidOutput || !e.Recoverable {
		t.Fatalf("error mismatch: %+v", e)
	}
	if !strings.Contains(e.Message, "cactus") {
		t.Fatalf("error message should name the offending category: %q", e.Message)
	}
}

// TestHardCaps: runaway LLM counts are clamped to the configured caps.
func TestHardCaps(t *testing.T) {
	sender := &collectSender{}
	huge := &llm.Recipe{Items: []llm.CategoryRecipe{
		{Category: "shrub", Count: 1_000_000, Clustering: 0.5, Tags: []string{"flora"}},
	}}
	o := New(&llm.FakeLLM{Recipe: huge}, fixtureStore(), sender)

	req := fixtureGenerate("gen-cap", 5)
	req.Bounds = protocol.Bounds{ // room for the clamped count
		Min: protocol.Vec2{X: -50000, Y: -50000},
		Max: protocol.Vec2{X: 50000, Y: 50000},
	}
	o.Generate(context.Background(), req)
	waitDone(t, o)

	msgs := sender.snapshot()
	plan, ok := msgs[0].(*protocol.Plan)
	if !ok {
		t.Fatalf("first message is %T (%+v), want plan", msgs[0], msgs[0])
	}
	if plan.TotalAssets > MaxAssetsTotal || plan.Categories["shrub"] > MaxAssetsPerCategory {
		t.Fatalf("caps not applied: %+v", plan)
	}
}
