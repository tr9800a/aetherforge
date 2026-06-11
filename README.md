# AetherForge

Generate and populate Unreal Engine levels from natural-language prompts. A designer types *"a forest clearing with Pacific Northwest deciduous trees, scattered shrubs, and two large dogs"* and the level fills in — streamed into the viewport as it generates, undoable with Ctrl+Z, and exactly reproducible from a seed.

AetherForge is two pieces working over one strict protocol:

- **A Go orchestration service** (`backend/`) that turns prompts into placed assets. An LLM is used *only* to translate intent into a small semantic "scene recipe" (categories, counts, clustering hints); a deterministic placement engine (Poisson-disk scatter + spatial-hash overlap rejection) turns that recipe into non-overlapping 2D transforms. Same prompt + same seed = identical world.
- **A UE5 editor plugin** (`unreal/AetherForge/`) that hosts the prompt UI, launches and manages the Go service as a sidecar, streams results over a localhost WebSocket, and spawns assets under a strict per-frame time budget so the editor never hitches. The plugin owns asset resolution (semantic category → `FSoftObjectPath` via a manifest `UDataAsset`) and the Z axis (ground-snap line traces) — the backend never sees engine asset paths or terrain.

The wire contract between them lives in `contract/` — a protocol reference plus canonical JSON fixtures that **both** test suites round-trip byte-for-byte, so the two sides cannot drift apart silently.

## Repo layout

```
contract/            Protocol v1 reference + shared JSON fixtures (incl. negative fixtures)
backend/             Go service: protocol, manifest store, LLM client interface (+fake),
                     placement engine, orchestrator, WebSocket server
unreal/AetherForge/  UE5 editor plugin (source of truth)
unreal/HostProject/  Minimal UE project for compiling the plugin and running its tests
docs/superpowers/    Design spec (PRD) and phased implementation plan
```

Key documents:

- [Design spec / PRD](docs/superpowers/specs/2026-06-10-aetherforge-prd-design.md)
- [Implementation plan](docs/superpowers/plans/2026-06-11-aetherforge-implementation-plan.md)
- [Protocol contract](contract/README.md)

## Requirements

- **Backend:** Go 1.26+
- **Plugin:** Unreal Engine 5.7 (installed via Epic Games Launcher) and full Xcode on macOS
- **Phase 4+ (not yet wired):** a local LLM via [Ollama](https://ollama.com)

## Quick start

### Backend

```sh
cd backend
go test ./...                              # full suite: fixtures, placement invariants, e2e
go build -o /tmp/aetherforge-server ./cmd/aetherforge-server
/tmp/aetherforge-server                    # listens on ws://127.0.0.1:8080 (localhost only)
```

The server currently runs the Phase 1 mock pipeline (a fake LLM returning canned recipes through the real placement engine and streaming protocol).

### Plugin

Compile against your engine install:

```sh
"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin \
  -Plugin="$(pwd)/unreal/AetherForge/AetherForge.uplugin" \
  -Package=/tmp/AetherForgeBuild -TargetPlatforms=Mac
```

Copy the packaged output to `unreal/HostProject/Plugins/AetherForge/` (gitignored — it is a build artifact), then run the automation tests headlessly:

```sh
export AETHERFORGE_CONTRACT_FIXTURES="$(pwd)/contract/fixtures"
"/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$(pwd)/unreal/HostProject/AetherForgeHost.uproject" \
  -ExecCmds="Automation RunTests AetherForge; Quit" \
  -nullrhi -unattended -nop4 -nosplash -stdout
```

To use the panel interactively: start the backend, open `unreal/HostProject/AetherForgeHost.uproject` in the editor, and open the AetherForge tab.

## Status

| Phase | Scope | Status |
|---|---|---|
| 0 | Contract, fixtures, toolchain proof | ✅ Done — Go and UE suites round-trip the same fixtures |
| 1 | Go service with mock pipeline + real placement engine | ✅ Done — all tests green, live e2e verified |
| 2 | Plugin scaffold: panel, sidecar manager, WebSocket client | ✅ Compiles on UE 5.7; protocol tests pass |
| 3 | Time-budgeted spawning, HISM pools, undo transactions | 🔧 Skeleton written; spawn-budget test + editor verification pending |
| 4 | Real Ollama LLM client (behind existing `llm.Client` interface) | ⬜ Not started |
| 5 | Polish: seed variations UI, live stats, demo level | ⬜ Not started |

Verified so far: 29 Go test cases (fixture round-trips, placement invariants across 200 seeds, cancellation, hard caps, real-WebSocket e2e), 3 UE automation tests against the same fixtures, a clean UE 5.7 compile, and a live binary-to-binary session streaming 158 assets (plan → 4 chunks → complete).
