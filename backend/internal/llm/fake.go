package llm

import (
	"context"
	"sort"
	"sync"
	"time"
)

// FakeLLM is a canned-response Client so the full pipeline runs in tests and
// demos with no model loaded. The zero value returns a deterministic default
// recipe derived from the manifest; fields allow tests to inject specific
// recipes, errors, retry behavior, and latency.
type FakeLLM struct {
	// Recipe, if non-nil, is returned verbatim (after InvalidAttempts and
	// Err are exhausted/checked).
	Recipe *Recipe
	// Err, if non-nil, is returned on every call.
	Err error
	// InvalidAttempts makes the first N calls fail with
	// *InvalidOutputError, exercising the orchestrator's retry-with-
	// feedback path.
	InvalidAttempts int
	// Delay simulates model latency; the call respects ctx cancellation
	// while waiting (so cancellation mid-LLM-call is testable).
	Delay time.Duration

	mu sync.Mutex
	// Calls counts GenerateRecipe invocations.
	Calls int
	// PriorErrors records Request.PriorError per call, so tests can assert
	// the validation error was fed back on retry.
	PriorErrors []string
}

var _ Client = (*FakeLLM)(nil)

// GenerateRecipe implements Client.
func (f *FakeLLM) GenerateRecipe(ctx context.Context, req Request) (*Recipe, error) {
	f.mu.Lock()
	f.Calls++
	call := f.Calls
	f.PriorErrors = append(f.PriorErrors, req.PriorError)
	f.mu.Unlock()

	if f.Delay > 0 {
		t := time.NewTimer(f.Delay)
		defer t.Stop()
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-t.C:
		}
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if f.Err != nil {
		return nil, f.Err
	}
	if call <= f.InvalidAttempts {
		return nil, &InvalidOutputError{Detail: "canned schema violation from FakeLLM"}
	}
	if f.Recipe != nil {
		cp := *f.Recipe
		return &cp, nil
	}
	return defaultRecipe(req), nil
}

// defaultRecipe builds a deterministic canned recipe straight from the
// manifest: every advertised category appears, with counts inversely
// proportional to footprint radius (small things scatter densely, big things
// sparsely), instanceable categories clumped like flora and non-instanceable
// ones (creatures) scattered with clear space.
func defaultRecipe(req Request) *Recipe {
	keys := make([]string, 0, len(req.Manifest))
	for k := range req.Manifest {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	r := &Recipe{}
	for _, k := range keys {
		e := req.Manifest[k]
		count := 8
		if e.FootprintRadius > 0 {
			count = int(6000 / e.FootprintRadius)
		}
		clustering := 0.0
		tags := []string{}
		if e.Instanceable {
			clustering = 0.7
			tags = append(tags, "scatter")
			count = max(count, 4)
		} else {
			tags = append(tags, "creature")
			count = min(max(count/10, 1), 4)
		}
		count = min(count, 80)
		r.Items = append(r.Items, CategoryRecipe{
			Category:   k,
			Count:      count,
			Clustering: clustering,
			Tags:       tags,
		})
	}
	return r
}
