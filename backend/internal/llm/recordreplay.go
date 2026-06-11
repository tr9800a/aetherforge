package llm

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
)

// Record/replay (spec §5): RecordingClient wraps a live client and writes
// every successful recipe to disk; ReplayClient serves recorded recipes back
// with no model running — deterministic demos and CI without a GPU.
//
// Recordings are keyed by a hash of (prompt, prior_error), one JSON file per
// distinct request, human-readable and safe to commit as test fixtures.

type recordedCall struct {
	Prompt     string `json:"prompt"`
	PriorError string `json:"prior_error,omitempty"`
	Recipe     Recipe `json:"recipe"`
}

func requestKey(req Request) string {
	h := sha256.Sum256([]byte(req.Prompt + "\x1f" + req.PriorError))
	return hex.EncodeToString(h[:8]) // 16 hex chars: short filenames, no realistic collision risk
}

func recordingPath(dir string, req Request) string {
	return filepath.Join(dir, requestKey(req)+".json")
}

// RecordingClient forwards to Inner and records successful recipes to Dir.
// Recording failures are logged, never fatal — a full disk must not break a
// generation.
type RecordingClient struct {
	Inner Client
	Dir   string
}

func (c *RecordingClient) GenerateRecipe(ctx context.Context, req Request) (*Recipe, error) {
	recipe, err := c.Inner.GenerateRecipe(ctx, req)
	if err != nil {
		return nil, err
	}

	data, marshalErr := json.MarshalIndent(recordedCall{
		Prompt:     req.Prompt,
		PriorError: req.PriorError,
		Recipe:     *recipe,
	}, "", "  ")
	if marshalErr == nil {
		if mkErr := os.MkdirAll(c.Dir, 0o755); mkErr != nil {
			marshalErr = mkErr
		} else {
			marshalErr = os.WriteFile(recordingPath(c.Dir, req), data, 0o644)
		}
	}
	if marshalErr != nil {
		log.Printf("llm: failed to record recipe for prompt %.40q: %v", req.Prompt, marshalErr)
	}
	return recipe, nil
}

// ReplayClient serves recipes recorded by RecordingClient. A request with no
// recording is an error (replay runs must be fully deterministic, never
// silently fall through to a model).
type ReplayClient struct {
	Dir string
}

func (c *ReplayClient) GenerateRecipe(_ context.Context, req Request) (*Recipe, error) {
	data, err := os.ReadFile(recordingPath(c.Dir, req))
	if err != nil {
		return nil, fmt.Errorf("llm: no recording for prompt %.60q in %s: %w", req.Prompt, c.Dir, err)
	}
	var call recordedCall
	if err := json.Unmarshal(data, &call); err != nil {
		return nil, fmt.Errorf("llm: corrupt recording %s: %w", recordingPath(c.Dir, req), err)
	}
	return &call.Recipe, nil
}
