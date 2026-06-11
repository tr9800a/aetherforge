// Package placement is the deterministic, pure placement engine:
//
//	(recipe, seed, bounds, manifest) → []AssetEntry
//
// No I/O, no clock, no global state. Same recipe + same seed + same bounds +
// same manifest yields a byte-identical asset stream.
//
// Algorithm (per spec §3.2): Poisson-disk-style dart-throw sampling for
// natural scatter, per-category footprint radii from the manifest, a spatial
// hash for overlap rejection (no two footprints may overlap), and
// cluster-friendly scatter — categories with a clustering hint clump near
// previously placed members; categories without it (creatures) scatter
// uniformly with clear space.
package placement

import (
	"fmt"
	"math"
	"math/rand/v2"
	"sort"

	"aetherforge/backend/internal/llm"
	"aetherforge/backend/internal/protocol"
)

const (
	// maxAttempts is the dart-throwing budget per instance before
	// declaring the area overcrowded.
	maxAttempts = 512
	// scaleMin/scaleMax bound the uniform scale variation.
	scaleMin = 0.85
	scaleMax = 1.25
)

// Error codes surfaced to the orchestrator.
type Error struct {
	Category string
	Reason   string
}

func (e *Error) Error() string {
	return fmt.Sprintf("placement: %s (category %q)", e.Reason, e.Category)
}

type placed struct {
	x, y, r float64
}

// spatialHash buckets placed footprints into grid cells for O(1)-ish
// neighborhood overlap queries.
type spatialHash struct {
	cell  float64
	maxR  float64
	cells map[[2]int][]placed
}

func newSpatialHash(maxRadius float64) *spatialHash {
	cell := maxRadius * 2
	if cell <= 0 {
		cell = 1
	}
	return &spatialHash{cell: cell, maxR: maxRadius, cells: make(map[[2]int][]placed)}
}

func (h *spatialHash) key(x, y float64) [2]int {
	return [2]int{int(math.Floor(x / h.cell)), int(math.Floor(y / h.cell))}
}

func (h *spatialHash) insert(p placed) {
	k := h.key(p.x, p.y)
	h.cells[k] = append(h.cells[k], p)
}

// overlaps reports whether a footprint of radius r at (x, y) would overlap
// any placed footprint (center distance < sum of radii).
func (h *spatialHash) overlaps(x, y, r float64) bool {
	reach := r + h.maxR
	span := int(math.Ceil(reach / h.cell))
	base := h.key(x, y)
	for dx := -span; dx <= span; dx++ {
		for dy := -span; dy <= span; dy++ {
			for _, p := range h.cells[[2]int{base[0] + dx, base[1] + dy}] {
				minDist := r + p.r
				ddx, ddy := x-p.x, y-p.y
				if ddx*ddx+ddy*ddy < minDist*minDist {
					return true
				}
			}
		}
	}
	return false
}

// Place converts a validated recipe into concrete asset transforms. It is
// pure and deterministic: the only randomness is a PCG stream derived from
// seed, consumed in a fixed order.
//
// Invariants guaranteed (and property-tested across hundreds of seeds):
//   - no two footprints overlap (footprint radii from the manifest);
//   - every footprint lies entirely within bounds;
//   - per-category entry counts exactly match the recipe;
//   - identical inputs ⇒ identical output, including ordering.
func Place(recipe *llm.Recipe, seed int64, bounds protocol.Bounds, manifest map[string]protocol.ManifestEntry) ([]protocol.AssetEntry, error) {
	rng := rand.New(rand.NewPCG(uint64(seed), 0x9e3779b97f4a7c15))

	// Deterministic placement order: largest footprints first (big things
	// are hardest to fit), name as tie-break.
	items := make([]llm.CategoryRecipe, len(recipe.Items))
	copy(items, recipe.Items)
	sort.SliceStable(items, func(i, j int) bool {
		ri := manifest[items[i].Category].FootprintRadius
		rj := manifest[items[j].Category].FootprintRadius
		if ri != rj {
			return ri > rj
		}
		return items[i].Category < items[j].Category
	})

	maxR := 0.0
	total := 0
	for _, it := range items {
		me, ok := manifest[it.Category]
		if !ok {
			return nil, &Error{Category: it.Category, Reason: "category not in manifest"}
		}
		if me.FootprintRadius > maxR {
			maxR = me.FootprintRadius
		}
		total += it.Count
	}

	hash := newSpatialHash(maxR)
	entries := make([]protocol.AssetEntry, 0, total)

	for _, it := range items {
		me := manifest[it.Category]
		r := me.FootprintRadius
		// Keep the whole footprint in bounds.
		loX, hiX := bounds.Min.X+r, bounds.Max.X-r
		loY, hiY := bounds.Min.Y+r, bounds.Max.Y-r
		if loX > hiX || loY > hiY {
			return nil, &Error{Category: it.Category, Reason: "bounds too small for footprint"}
		}

		// Anchors for cluster-friendly scatter: previously placed
		// members of this category.
		var anchors []placed

		for i := 0; i < it.Count; i++ {
			ok := false
			var px, py float64
			for attempt := 0; attempt < maxAttempts; attempt++ {
				if it.Clustering > 0 && len(anchors) > 0 && rng.Float64() < it.Clustering {
					// Sample in an annulus around a random
					// existing member of the cluster.
					a := anchors[rng.IntN(len(anchors))]
					ang := rng.Float64() * 2 * math.Pi
					dist := r * (2.05 + 3*rng.Float64())
					px = clamp(a.x+dist*math.Cos(ang), loX, hiX)
					py = clamp(a.y+dist*math.Sin(ang), loY, hiY)
				} else {
					px = loX + rng.Float64()*(hiX-loX)
					py = loY + rng.Float64()*(hiY-loY)
				}
				if !hash.overlaps(px, py, r) {
					ok = true
					break
				}
			}
			if !ok {
				return nil, &Error{Category: it.Category, Reason: "could not fit footprint after max attempts (area overcrowded)"}
			}
			p := placed{x: px, y: py, r: r}
			hash.insert(p)
			anchors = append(anchors, p)

			yaw := rng.Float64() * 360.0
			scale := scaleMin + rng.Float64()*(scaleMax-scaleMin)
			tags := make([]string, len(it.Tags))
			copy(tags, it.Tags)
			entries = append(entries, protocol.AssetEntry{
				Category: it.Category,
				Transform: protocol.Transform{
					Location: protocol.Vec2{X: px, Y: py},
					Yaw:      yaw,
					Scale:    scale,
				},
				GroundSnap: me.GroundSnap,
				Tags:       tags,
			})
		}
	}
	return entries, nil
}

func clamp(v, lo, hi float64) float64 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}
