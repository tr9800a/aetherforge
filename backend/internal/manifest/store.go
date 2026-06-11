// Package manifest holds the per-connection asset manifest advertised by the
// UE5 plugin at the hello handshake. From that point on the backend works
// only in these semantic categories — it never sees Unreal asset paths.
package manifest

import (
	"sort"
	"sync"

	"aetherforge/backend/internal/protocol"
)

// Store is a thread-safe holder for the manifest received at hello.
// One Store exists per connection.
type Store struct {
	mu      sync.RWMutex
	entries map[string]protocol.ManifestEntry
}

// NewStore returns an empty Store (no manifest received yet).
func NewStore() *Store {
	return &Store{}
}

// Set replaces the stored manifest with the one from a hello message.
func (s *Store) Set(m map[string]protocol.ManifestEntry) {
	cp := make(map[string]protocol.ManifestEntry, len(m))
	for k, v := range m {
		cp[k] = v
	}
	s.mu.Lock()
	s.entries = cp
	s.mu.Unlock()
}

// Ready reports whether a non-empty manifest has been received.
func (s *Store) Ready() bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return len(s.entries) > 0
}

// Get returns the entry for a semantic category key.
func (s *Store) Get(key string) (protocol.ManifestEntry, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	e, ok := s.entries[key]
	return e, ok
}

// Snapshot returns a copy of the full manifest map.
func (s *Store) Snapshot() map[string]protocol.ManifestEntry {
	s.mu.RLock()
	defer s.mu.RUnlock()
	cp := make(map[string]protocol.ManifestEntry, len(s.entries))
	for k, v := range s.entries {
		cp[k] = v
	}
	return cp
}

// Keys returns the semantic category keys in sorted (deterministic) order.
func (s *Store) Keys() []string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	keys := make([]string, 0, len(s.entries))
	for k := range s.entries {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
