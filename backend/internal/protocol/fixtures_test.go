package protocol

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

const (
	fixturesDir        = "../../../contract/fixtures"
	invalidFixturesDir = fixturesDir + "/invalid"
)

// expectedTypes maps each canonical fixture file to the Go type Decode must
// produce for it.
var expectedTypes = map[string]reflect.Type{
	"hello.json":    reflect.TypeOf(&Hello{}),
	"generate.json": reflect.TypeOf(&Generate{}),
	"plan.json":     reflect.TypeOf(&Plan{}),
	"chunk.json":    reflect.TypeOf(&Chunk{}),
	"complete.json": reflect.TypeOf(&Complete{}),
	"error.json":    reflect.TypeOf(&Error{}),
	"cancel.json":   reflect.TypeOf(&Cancel{}),
}

// TestFixtureRoundTrip loads every shared contract fixture and verifies
// parse → model → serialize → parse with semantic equality to the original
// bytes, per the contract README.
func TestFixtureRoundTrip(t *testing.T) {
	files, err := filepath.Glob(filepath.Join(fixturesDir, "*.json"))
	if err != nil {
		t.Fatalf("globbing fixtures: %v", err)
	}
	if len(files) == 0 {
		t.Fatalf("no fixtures found in %s", fixturesDir)
	}
	if len(files) != len(expectedTypes) {
		t.Fatalf("found %d fixtures, expected %d (new message type? update this test)",
			len(files), len(expectedTypes))
	}

	for _, path := range files {
		name := filepath.Base(path)
		t.Run(name, func(t *testing.T) {
			original, err := os.ReadFile(path)
			if err != nil {
				t.Fatalf("reading fixture: %v", err)
			}

			// parse → model
			msg, err := Decode(original)
			if err != nil {
				t.Fatalf("Decode: %v", err)
			}
			want, ok := expectedTypes[name]
			if !ok {
				t.Fatalf("unknown fixture %s — add it to expectedTypes", name)
			}
			if got := reflect.TypeOf(msg); got != want {
				t.Fatalf("Decode produced %v, want %v", got, want)
			}

			// model → serialize
			reencoded, err := json.Marshal(msg)
			if err != nil {
				t.Fatalf("Marshal: %v", err)
			}

			// serialize → parse, assert semantic equality with original
			var wantJSON, gotJSON any
			if err := json.Unmarshal(original, &wantJSON); err != nil {
				t.Fatalf("unmarshal original: %v", err)
			}
			if err := json.Unmarshal(reencoded, &gotJSON); err != nil {
				t.Fatalf("unmarshal re-encoded: %v", err)
			}
			if !reflect.DeepEqual(wantJSON, gotJSON) {
				t.Fatalf("round-trip not semantically equal\noriginal:   %s\nreencoded:  %s",
					original, reencoded)
			}

			// parse the serialized form back into the typed model too
			msg2, err := Decode(reencoded)
			if err != nil {
				t.Fatalf("Decode(reencoded): %v", err)
			}
			if !reflect.DeepEqual(msg, msg2) {
				t.Fatalf("typed model not stable across round-trip:\n%+v\n%+v", msg, msg2)
			}
		})
	}
}

// TestFixtureFieldSpotChecks pins a few load-bearing fields so a silently
// lenient JSON mapping cannot pass the round-trip by accident.
func TestFixtureFieldSpotChecks(t *testing.T) {
	read := func(name string) any {
		t.Helper()
		data, err := os.ReadFile(filepath.Join(fixturesDir, name))
		if err != nil {
			t.Fatalf("reading %s: %v", name, err)
		}
		msg, err := Decode(data)
		if err != nil {
			t.Fatalf("decoding %s: %v", name, err)
		}
		return msg
	}

	hello := read("hello.json").(*Hello)
	if hello.ProtocolVersion != Version {
		t.Errorf("hello.protocol_version = %d, want %d", hello.ProtocolVersion, Version)
	}
	tree, ok := hello.Manifest["deciduous_tree"]
	if !ok {
		t.Fatal("hello.manifest missing deciduous_tree")
	}
	if tree.FootprintRadius != 250.0 || !tree.GroundSnap || !tree.Instanceable || tree.DisplayName != "Deciduous Tree" {
		t.Errorf("deciduous_tree manifest entry mismatch: %+v", tree)
	}
	if dog := hello.Manifest["dog_large"]; dog.Instanceable {
		t.Error("dog_large should not be instanceable")
	}

	gen := read("generate.json").(*Generate)
	if gen.Seed == nil || *gen.Seed != 1337 {
		t.Errorf("generate.seed = %v, want 1337", gen.Seed)
	}
	if gen.Bounds.Min.X != -5000 || gen.Bounds.Max.Y != 5000 {
		t.Errorf("generate.bounds mismatch: %+v", gen.Bounds)
	}

	plan := read("plan.json").(*Plan)
	if plan.Seed != 1337 || plan.TotalAssets != 142 || plan.Categories["deciduous_tree"] != 80 {
		t.Errorf("plan mismatch: %+v", plan)
	}
	sum := 0
	for _, c := range plan.Categories {
		sum += c
	}
	if sum != plan.TotalAssets {
		t.Errorf("plan.categories sum %d != total_assets %d", sum, plan.TotalAssets)
	}

	chunk := read("chunk.json").(*Chunk)
	if len(chunk.Assets) != 6 {
		t.Fatalf("chunk has %d assets, want 6", len(chunk.Assets))
	}
	first := chunk.Assets[0]
	if first.Category != "deciduous_tree" ||
		first.Transform.Location.X != 150.0 || first.Transform.Location.Y != -200.0 ||
		first.Transform.Yaw != 45.0 || first.Transform.Scale != 1.2 ||
		!first.GroundSnap || !reflect.DeepEqual(first.Tags, []string{"flora", "canopy"}) {
		t.Errorf("first chunk asset mismatch: %+v", first)
	}

	complete := read("complete.json").(*Complete)
	if complete.Stats.Assets != 142 || complete.Stats.ElapsedMS != 4815 || complete.Stats.LLMMS != 2930 {
		t.Errorf("complete.stats mismatch: %+v", complete.Stats)
	}

	errMsg := read("error.json").(*Error)
	if errMsg.Code != CodeLLMInvalidOutput || !errMsg.Recoverable {
		t.Errorf("error fixture mismatch: %+v", errMsg)
	}

	cancel := read("cancel.json").(*Cancel)
	if cancel.GenerationID != "gen-7f3a9c12" {
		t.Errorf("cancel.generation_id = %q", cancel.GenerationID)
	}
}

// TestInvalidFixturesRejected proves the strict-validation half of the
// contract: every negative fixture in contract/fixtures/invalid/ must be
// rejected — forbidden asset-entry fields (extra z, pitch/roll) and a bad
// protocol_version at Decode, an unknown category at manifest validation.
func TestInvalidFixturesRejected(t *testing.T) {
	helloData, err := os.ReadFile(filepath.Join(fixturesDir, "hello.json"))
	if err != nil {
		t.Fatalf("reading hello.json: %v", err)
	}
	helloMsg, err := Decode(helloData)
	if err != nil {
		t.Fatalf("decoding hello.json: %v", err)
	}
	manifest := helloMsg.(*Hello).Manifest

	files, err := filepath.Glob(filepath.Join(invalidFixturesDir, "*.json"))
	if err != nil {
		t.Fatalf("globbing invalid fixtures: %v", err)
	}
	if len(files) != 4 {
		t.Fatalf("found %d invalid fixtures, expected 4 (new negative case? update this test)", len(files))
	}

	for _, path := range files {
		name := filepath.Base(path)
		t.Run(name, func(t *testing.T) {
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatalf("reading fixture: %v", err)
			}
			msg, err := Decode(data)
			switch name {
			case "unknown_category.json":
				// Structurally valid JSON; the violation is semantic and is
				// caught by validating categories against the hello manifest.
				if err != nil {
					t.Fatalf("unknown_category.json should pass structural Decode, got: %v", err)
				}
				chunk, ok := msg.(*Chunk)
				if !ok {
					t.Fatalf("unknown_category.json decoded to %T, want *Chunk", msg)
				}
				if verr := chunk.ValidateCategories(manifest); verr == nil {
					t.Fatal("unknown category must be rejected by manifest validation")
				}
			case "bad_protocol_version.json":
				if !errors.Is(err, ErrVersionMismatch) {
					t.Fatalf("bad_protocol_version.json must fail with ErrVersionMismatch, got: %v", err)
				}
			default: // asset_with_z.json, asset_full_rotation.json
				if err == nil {
					t.Fatalf("invalid fixture must be rejected, but decoded to %T", msg)
				}
			}
		})
	}
}

// TestGenerateSeedOptional verifies seed omission round-trips: absent on the
// wire ⇒ nil in the model ⇒ absent when re-serialized.
func TestGenerateSeedOptional(t *testing.T) {
	wire := []byte(`{"protocol_version":1,"type":"generate","generation_id":"g1",` +
		`"prompt":"p","bounds":{"min":{"x":0,"y":0},"max":{"x":100,"y":100}}}`)
	msg, err := Decode(wire)
	if err != nil {
		t.Fatal(err)
	}
	gen := msg.(*Generate)
	if gen.Seed != nil {
		t.Fatalf("seed should be nil when omitted, got %v", *gen.Seed)
	}
	out, err := json.Marshal(gen)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatal(err)
	}
	if _, present := m["seed"]; present {
		t.Fatalf("omitted seed must not reappear on the wire: %s", out)
	}
}
