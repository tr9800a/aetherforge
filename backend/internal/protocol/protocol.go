// Package protocol defines the AetherForge wire protocol (v1): JSON message
// types exchanged between the UE5 plugin (client) and the Go orchestration
// service (server) over a single WebSocket connection.
//
// The contract of record is contract/README.md and the canonical fixtures in
// contract/fixtures/, which these types must round-trip byte-for-byte
// (semantically).
package protocol

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
)

// ErrVersionMismatch is wrapped into the error returned by Decode when a
// message carries a protocol_version other than Version. Callers (the server
// handshake in particular) detect it with errors.Is to emit the
// protocol_version_mismatch error code.
var ErrVersionMismatch = errors.New("protocol version mismatch")

// Version is the current protocol version. It is bumped on any breaking wire
// change, and validated at the hello handshake: a mismatch fails loudly.
const Version = 1

// Message type discriminators (the "type" field).
const (
	TypeHello    = "hello"
	TypeGenerate = "generate"
	TypePlan     = "plan"
	TypeChunk    = "chunk"
	TypeComplete = "complete"
	TypeError    = "error"
	TypeCancel   = "cancel"
)

// Error codes used in Error.Code.
const (
	CodeProtocolVersionMismatch = "protocol_version_mismatch"
	CodeExpectedHello           = "expected_hello"
	CodeLLMInvalidOutput        = "llm_invalid_output"
	CodeLLMTimeout              = "llm_timeout"
	CodeUnknownCategory         = "unknown_category"
	CodeInternal                = "internal"
)

// Vec2 is a 2D point/vector in Unreal world units (cm). The backend never
// emits Z; the plugin owns Z via ground snapping.
type Vec2 struct {
	X float64 `json:"x"`
	Y float64 `json:"y"`
}

// Bounds is the 2D generation area.
type Bounds struct {
	Min Vec2 `json:"min"`
	Max Vec2 `json:"max"`
}

// Transform is the 2D layout transform of a placed asset: 2D location,
// yaw-only rotation (degrees), uniform scale. No Z, no pitch/roll.
type Transform struct {
	Location Vec2    `json:"location"`
	Yaw      float64 `json:"yaw"`
	Scale    float64 `json:"scale"`
}

// AssetEntry is one placed asset inside a chunk message. The wire never
// carries Unreal asset paths: Category is a semantic key resolved by the
// plugin through the manifest it advertised at hello.
type AssetEntry struct {
	Category   string    `json:"category"`
	Transform  Transform `json:"transform"`
	GroundSnap bool      `json:"ground_snap"`
	Tags       []string  `json:"tags"`
}

// ManifestEntry is the per-category metadata advertised by the plugin at
// hello. Only metadata crosses the wire — never asset paths.
type ManifestEntry struct {
	FootprintRadius float64 `json:"footprint_radius"`
	GroundSnap      bool    `json:"ground_snap"`
	Instanceable    bool    `json:"instanceable"`
	DisplayName     string  `json:"display_name"`
}

// Stats is the payload of a complete message.
type Stats struct {
	Assets    int   `json:"assets"`
	ElapsedMS int64 `json:"elapsed_ms"`
	LLMMS     int64 `json:"llm_ms"`
}

// Hello — client → server, exactly once per connection, before anything else.
type Hello struct {
	ProtocolVersion int                      `json:"protocol_version"`
	Type            string                   `json:"type"`
	Manifest        map[string]ManifestEntry `json:"manifest"`
}

// Generate — client → server. Starts a generation; a new generate while one
// is in flight on the same connection cancels the in-flight one.
type Generate struct {
	ProtocolVersion int    `json:"protocol_version"`
	Type            string `json:"type"`
	GenerationID    string `json:"generation_id"`
	Prompt          string `json:"prompt"`
	// Seed is optional; if nil the server picks one and echoes it in plan.
	Seed   *int64 `json:"seed,omitempty"`
	Bounds Bounds `json:"bounds"`
}

// Plan — server → client, once per generation, before the first chunk.
type Plan struct {
	ProtocolVersion int            `json:"protocol_version"`
	Type            string         `json:"type"`
	GenerationID    string         `json:"generation_id"`
	Seed            int64          `json:"seed"`
	TotalAssets     int            `json:"total_assets"`
	Categories      map[string]int `json:"categories"`
}

// Chunk — server → client, repeated (~50 asset entries per message).
type Chunk struct {
	ProtocolVersion int          `json:"protocol_version"`
	Type            string       `json:"type"`
	GenerationID    string       `json:"generation_id"`
	Assets          []AssetEntry `json:"assets"`
}

// Complete — server → client, terminal message for a successful generation.
type Complete struct {
	ProtocolVersion int    `json:"protocol_version"`
	Type            string `json:"type"`
	GenerationID    string `json:"generation_id"`
	Stats           Stats  `json:"stats"`
}

// Error — server → client, terminal message for a failed generation (or a
// handshake failure, in which case GenerationID is the empty string).
type Error struct {
	ProtocolVersion int    `json:"protocol_version"`
	Type            string `json:"type"`
	GenerationID    string `json:"generation_id"`
	Code            string `json:"code"`
	Message         string `json:"message"`
	Recoverable     bool   `json:"recoverable"`
}

// Cancel — client → server.
type Cancel struct {
	ProtocolVersion int    `json:"protocol_version"`
	Type            string `json:"type"`
	GenerationID    string `json:"generation_id"`
}

// Decode parses a single wire message and returns the corresponding typed
// struct (*Hello, *Generate, *Plan, *Chunk, *Complete, *Error or *Cancel),
// dispatching on the "type" discriminator field.
//
// Decode is strict, per the contract (contract/README.md):
//   - protocol_version must be present and equal to Version on every message
//     (mismatch errors wrap ErrVersionMismatch);
//   - unknown fields are rejected (json.Decoder DisallowUnknownFields), so a
//     forbidden asset-entry field — an extra "z" on a location, pitch/roll on
//     a transform, per-axis scale — fails instead of being silently dropped.
func Decode(data []byte) (any, error) {
	var head struct {
		Type            string `json:"type"`
		ProtocolVersion *int   `json:"protocol_version"`
	}
	if err := json.Unmarshal(data, &head); err != nil {
		return nil, fmt.Errorf("protocol: invalid JSON: %w", err)
	}
	if head.ProtocolVersion == nil {
		return nil, errors.New(`protocol: missing required field "protocol_version"`)
	}
	if *head.ProtocolVersion != Version {
		return nil, fmt.Errorf("protocol: %w: server speaks v%d, message carries v%d",
			ErrVersionMismatch, Version, *head.ProtocolVersion)
	}
	var msg any
	switch head.Type {
	case TypeHello:
		msg = &Hello{}
	case TypeGenerate:
		msg = &Generate{}
	case TypePlan:
		msg = &Plan{}
	case TypeChunk:
		msg = &Chunk{}
	case TypeComplete:
		msg = &Complete{}
	case TypeError:
		msg = &Error{}
	case TypeCancel:
		msg = &Cancel{}
	default:
		return nil, fmt.Errorf("protocol: unknown message type %q", head.Type)
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(msg); err != nil {
		return nil, fmt.Errorf("protocol: malformed %q message: %w", head.Type, err)
	}
	return msg, nil
}

// ValidateCategories enforces the semantic-categories rule on a chunk: every
// asset entry's category must exist in the manifest advertised at hello.
// An unknown category is a contract violation.
func (c *Chunk) ValidateCategories(manifest map[string]ManifestEntry) error {
	for i, a := range c.Assets {
		if _, ok := manifest[a.Category]; !ok {
			return fmt.Errorf("protocol: chunk asset %d: unknown category %q (not in the manifest advertised at hello)",
				i, a.Category)
		}
	}
	return nil
}
