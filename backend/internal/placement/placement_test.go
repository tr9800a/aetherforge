package placement

import (
	"math/rand/v2"
	"reflect"
	"testing"

	"aetherforge/backend/internal/llm"
	"aetherforge/backend/internal/protocol"
)

// fixtureManifest mirrors contract/fixtures/hello.json.
func fixtureManifest() map[string]protocol.ManifestEntry {
	return map[string]protocol.ManifestEntry{
		"deciduous_tree": {FootprintRadius: 250, GroundSnap: true, Instanceable: true, DisplayName: "Deciduous Tree"},
		"shrub":          {FootprintRadius: 75, GroundSnap: true, Instanceable: true, DisplayName: "Shrub"},
		"boulder":        {FootprintRadius: 120, GroundSnap: true, Instanceable: true, DisplayName: "Boulder"},
		"dog_large":      {FootprintRadius: 150, GroundSnap: true, Instanceable: false, DisplayName: "Large Dog"},
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

func fixtureBounds() protocol.Bounds {
	return protocol.Bounds{
		Min: protocol.Vec2{X: -5000, Y: -5000},
		Max: protocol.Vec2{X: 5000, Y: 5000},
	}
}

// TestPlacementInvariants is the property-style suite from the spec: across
// at least 200 random seeds — no two footprints overlap, every footprint is
// fully in bounds, per-category counts exactly match the recipe, transforms
// are well-formed, and the same seed reproduces identical output.
func TestPlacementInvariants(t *testing.T) {
	manifest := fixtureManifest()
	recipe := fixtureRecipe()
	bounds := fixtureBounds()

	seedRNG := rand.New(rand.NewPCG(42, 42)) // deterministic test, random-looking seeds
	const seedCount = 200

	for i := 0; i < seedCount; i++ {
		seed := int64(seedRNG.Uint64() >> 1)
		entries, err := Place(recipe, seed, bounds, manifest)
		if err != nil {
			t.Fatalf("seed %d: Place failed: %v", seed, err)
		}

		// Counts match recipe, per category and in total.
		wantTotal := 0
		wantCounts := map[string]int{}
		for _, it := range recipe.Items {
			wantCounts[it.Category] = it.Count
			wantTotal += it.Count
		}
		gotCounts := map[string]int{}
		for _, e := range entries {
			gotCounts[e.Category]++
		}
		if len(entries) != wantTotal {
			t.Fatalf("seed %d: got %d entries, want %d", seed, len(entries), wantTotal)
		}
		if !reflect.DeepEqual(gotCounts, wantCounts) {
			t.Fatalf("seed %d: per-category counts %v, want %v", seed, gotCounts, wantCounts)
		}

		// All footprints fully in bounds; transforms well-formed.
		for j, e := range entries {
			r := manifest[e.Category].FootprintRadius
			loc := e.Transform.Location
			if loc.X-r < bounds.Min.X || loc.X+r > bounds.Max.X ||
				loc.Y-r < bounds.Min.Y || loc.Y+r > bounds.Max.Y {
				t.Fatalf("seed %d: entry %d (%s) footprint out of bounds at (%g, %g)",
					seed, j, e.Category, loc.X, loc.Y)
			}
			if e.Transform.Yaw < 0 || e.Transform.Yaw >= 360 {
				t.Fatalf("seed %d: entry %d yaw %g out of [0, 360)", seed, j, e.Transform.Yaw)
			}
			if e.Transform.Scale <= 0 {
				t.Fatalf("seed %d: entry %d non-positive scale %g", seed, j, e.Transform.Scale)
			}
			if e.GroundSnap != manifest[e.Category].GroundSnap {
				t.Fatalf("seed %d: entry %d ground_snap mismatch with manifest", seed, j)
			}
			if e.Tags == nil {
				t.Fatalf("seed %d: entry %d has nil tags (must serialize as [])", seed, j)
			}
		}

		// No two footprints overlap (exhaustive pairwise check).
		for a := 0; a < len(entries); a++ {
			ra := manifest[entries[a].Category].FootprintRadius
			la := entries[a].Transform.Location
			for b := a + 1; b < len(entries); b++ {
				rb := minDistRadius(manifest, entries[b].Category)
				lb := entries[b].Transform.Location
				dx, dy := la.X-lb.X, la.Y-lb.Y
				minDist := ra + rb
				if dx*dx+dy*dy < minDist*minDist {
					t.Fatalf("seed %d: footprints overlap: entry %d (%s) and entry %d (%s)",
						seed, a, entries[a].Category, b, entries[b].Category)
				}
			}
		}

		// Same seed ⇒ identical output (including ordering).
		again, err := Place(recipe, seed, bounds, manifest)
		if err != nil {
			t.Fatalf("seed %d: second Place failed: %v", seed, err)
		}
		if !reflect.DeepEqual(entries, again) {
			t.Fatalf("seed %d: placement not deterministic", seed)
		}
	}
}

func minDistRadius(m map[string]protocol.ManifestEntry, cat string) float64 {
	return m[cat].FootprintRadius
}

// TestPlacementDifferentSeedsDiffer guards against a degenerate engine that
// ignores the seed entirely.
func TestPlacementDifferentSeedsDiffer(t *testing.T) {
	a, err := Place(fixtureRecipe(), 1, fixtureBounds(), fixtureManifest())
	if err != nil {
		t.Fatal(err)
	}
	b, err := Place(fixtureRecipe(), 2, fixtureBounds(), fixtureManifest())
	if err != nil {
		t.Fatal(err)
	}
	if reflect.DeepEqual(a, b) {
		t.Fatal("different seeds produced identical layouts")
	}
}

// TestPlacementUnknownCategory verifies the engine rejects recipes naming
// categories outside the manifest.
func TestPlacementUnknownCategory(t *testing.T) {
	recipe := &llm.Recipe{Items: []llm.CategoryRecipe{{Category: "cactus", Count: 1}}}
	_, err := Place(recipe, 1, fixtureBounds(), fixtureManifest())
	if err == nil {
		t.Fatal("expected error for category not in manifest")
	}
}

// TestPlacementBoundsTooSmall verifies a footprint that cannot fit at all is
// a structured error, not a hang or a violation.
func TestPlacementBoundsTooSmall(t *testing.T) {
	tiny := protocol.Bounds{Min: protocol.Vec2{X: 0, Y: 0}, Max: protocol.Vec2{X: 100, Y: 100}}
	recipe := &llm.Recipe{Items: []llm.CategoryRecipe{{Category: "deciduous_tree", Count: 1}}}
	_, err := Place(recipe, 1, tiny, fixtureManifest())
	if err == nil {
		t.Fatal("expected error for bounds smaller than the footprint")
	}
}

// TestPlacementOvercrowded verifies the engine fails cleanly (rather than
// violating the overlap invariant) when the recipe physically cannot fit.
func TestPlacementOvercrowded(t *testing.T) {
	small := protocol.Bounds{Min: protocol.Vec2{X: 0, Y: 0}, Max: protocol.Vec2{X: 1200, Y: 1200}}
	recipe := &llm.Recipe{Items: []llm.CategoryRecipe{{Category: "deciduous_tree", Count: 500}}}
	_, err := Place(recipe, 7, small, fixtureManifest())
	if err == nil {
		t.Fatal("expected overcrowded error")
	}
}

// TestPlacementClusteringHint sanity-checks the cluster-friendly scatter: a
// strongly clustered category should have a smaller mean nearest-neighbor
// distance than an unclustered one with the same footprint, on average.
func TestPlacementClusteringHint(t *testing.T) {
	manifest := map[string]protocol.ManifestEntry{
		"clumped":   {FootprintRadius: 50, GroundSnap: true, Instanceable: true, DisplayName: "Clumped"},
		"scattered": {FootprintRadius: 50, GroundSnap: true, Instanceable: true, DisplayName: "Scattered"},
	}
	bounds := protocol.Bounds{Min: protocol.Vec2{X: -20000, Y: -20000}, Max: protocol.Vec2{X: 20000, Y: 20000}}
	recipe := &llm.Recipe{Items: []llm.CategoryRecipe{
		{Category: "clumped", Count: 60, Clustering: 0.95},
		{Category: "scattered", Count: 60, Clustering: 0},
	}}

	var clumpedSum, scatteredSum float64
	const trials = 20
	for s := int64(0); s < trials; s++ {
		entries, err := Place(recipe, s, bounds, manifest)
		if err != nil {
			t.Fatal(err)
		}
		clumpedSum += meanNearestNeighbor(entries, "clumped")
		scatteredSum += meanNearestNeighbor(entries, "scattered")
	}
	if clumpedSum >= scatteredSum {
		t.Fatalf("clustering hint had no effect: clumped mean NN %.1f >= scattered %.1f",
			clumpedSum/trials, scatteredSum/trials)
	}
}

func meanNearestNeighbor(entries []protocol.AssetEntry, cat string) float64 {
	var pts []protocol.Vec2
	for _, e := range entries {
		if e.Category == cat {
			pts = append(pts, e.Transform.Location)
		}
	}
	total := 0.0
	for i := range pts {
		best := -1.0
		for j := range pts {
			if i == j {
				continue
			}
			dx, dy := pts[i].X-pts[j].X, pts[i].Y-pts[j].Y
			d := dx*dx + dy*dy
			if best < 0 || d < best {
				best = d
			}
		}
		total += best
	}
	return total / float64(len(pts))
}
