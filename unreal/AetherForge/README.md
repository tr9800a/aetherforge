# AetherForge — UE5 Editor Plugin

GenAI world-building for Unreal Engine 5: type a natural-language prompt, and the plugin
streams a procedurally generated, seeded, fully undoable environment into your level. The
heavy lifting (LLM recipe generation, deterministic placement) happens in a local Go
sidecar (`aetherforge-server`) that this plugin launches and manages — designers never touch a
terminal.

- Spec of record: [`docs/superpowers/specs/2026-06-10-aetherforge-prd-design.md`](../../docs/superpowers/specs/2026-06-10-aetherforge-prd-design.md)
- Wire contract (v1): [`contract/README.md`](../../contract/README.md) with canonical fixtures in [`contract/fixtures/`](../../contract/fixtures/)

## Requirements

- Unreal Engine 5.3+ (uses `FTSTicker`, `FAppStyle`, LWC-era APIs)
- A C++ UE project (the plugin compiles from source)
- The Go sidecar binary (see [Sidecar binary](#sidecar-binary))

## Installation

1. Copy (or symlink) this directory into your project:
   `YourProject/Plugins/AetherForge/`
2. Place the sidecar binary (see below).
3. Regenerate project files and build the editor target, or just launch the editor and
   accept the rebuild prompt.
4. Enable **AetherForge** in *Edit → Plugins* if it is not already enabled, then open the
   panel via **Tools → AetherForge**.

The module type is `Editor`: nothing from this plugin ships in packaged builds.

### Sidecar binary

The plugin launches the Go orchestration service from a fixed location inside the plugin
directory:

```
AetherForge/Binaries/Sidecar/aetherforge-server        (macOS / Linux)
AetherForge/Binaries/Sidecar/aetherforge-server.exe    (Windows)
```

Build it from the repo's `backend/` tree and copy it there, e.g.:

```sh
go build -o unreal/AetherForge/Binaries/Sidecar/aetherforge-server ./backend/cmd/aetherforge-server
```

The plugin starts it with `-addr 127.0.0.1:8080`, health-checks it (process watchdog at 1 Hz plus
WebSocket connectability with retries), and terminates it on editor shutdown — it is
never leaked across editor sessions. If the binary is missing or the process dies, the
panel shows an explicit error state with a Reconnect button.

The sidecar defaults to the real LLM (`-llm=ollama`), which needs Ollama running locally
with the model pulled (`ollama serve`, `ollama pull llama3.2:3b`). Without Ollama,
generations fail with a structured `llm` error in the panel; run the server manually
with `-llm=fake` for canned recipes instead.

### Asset manifest

Author a `UAetherForgeManifest` Data Asset at:

```
/AetherForge/DA_AetherForgeManifest        (i.e. AetherForge plugin Content root)
```

(Right-click in the plugin's Content folder → *Miscellaneous → Data Asset* →
`AetherForgeManifest`.) Each entry maps a semantic category key (e.g. `deciduous_tree`)
to:

| Field | Meaning |
|---|---|
| `AssetPath` | `FSoftObjectPath` to a Static Mesh (instanceable categories) or Blueprint/actor class (interactive ones). Never crosses the wire. |
| `FootprintRadius` | Collision footprint (cm) used by the backend's overlap rejection. |
| `bInstanceable` | `true` → per-category HISM pool; `false` → `SpawnActor`. |
| `bGroundSnap` | `true` → spawned entries line-trace down to terrain for Z. |
| `DisplayName` | Human-readable name for UI/logging. |

Only the metadata is advertised to the backend at `hello`; the LLM can only ever select
from these keys. Project teams extend this asset to expose their own libraries — no code
changes. If the asset is missing, the plugin warns and connects with an empty manifest so
the handshake still works.

## What's here (Phase 2 + Phase 3 skeleton)

| Piece | File(s) | Status |
|---|---|---|
| Plugin descriptor | `AetherForge.uplugin` | Done |
| Module + tab/menu registration | `Source/AetherForgeEditor/{Public/AetherForgeEditorModule.h, Private/AetherForgeEditorModule.cpp}` | Done |
| Protocol v1 types + strict JSON layer | `Public/AetherForgeProtocolTypes.h`, `Private/Protocol/AetherForgeProtocol.cpp` | Done (mirrors `contract/README.md` exactly) |
| Manifest Data Asset | `Public/AetherForgeManifest.h`, `Private/AetherForgeManifest.cpp` | Done |
| Slate panel | `Private/UI/SAetherForgePanel.{h,cpp}` | Done — prompt, Generate, seed + New Variation, determinate progress bar, Cancel, stats line, scrolling log, explicit error states |
| Sidecar manager | `Private/Sidecar/AetherForgeSidecarManager.{h,cpp}` | Done — `FPlatformProcess::CreateProc`, watchdog, terminate-on-shutdown |
| WebSocket client | `Private/Net/AetherForgeClient.{h,cpp}` | Done — `IWebSocket` to `ws://127.0.0.1:8080`, hello + generate + cancel, off-game-thread JSON parsing, ordered game-thread dispatch |
| Time-budgeted spawner | `Private/Spawn/AetherForgeSpawner.{h,cpp}` | Skeleton (Phase 3) — see below |
| Protocol Automation tests | `Private/Tests/ProtocolFixtureTests.cpp` | Done — round-trips the shared `contract/fixtures/` bytes, rejects every `fixtures/invalid/` negative fixture, verifies seed omission |

### Threading model

JSON parsing never runs on the game thread: raw WebSocket frames are queued to a single
background parse worker (`AsyncTask`), which preserves frame order; parsed, typed
messages flow back through a thread-safe queue drained by a game-thread ticker. All
client delegates fire on the game thread.

### Spawner (Phase 3 skeleton)

- Spends at most ~2 ms (`FPlatformTime`) per editor tick draining the spawn queue —
  budget-by-time, never fixed-count, so the viewport never hitches.
- Async-preloads each category's asset via `FStreamableManager`; entries become eligible
  only after their asset resolves (no synchronous loads in the spawn tick).
- Instanceable categories → per-category `HierarchicalInstancedStaticMeshComponent`
  pools on a per-generation container actor; others → `SpawnActor`
  (Blueprint/actor-class assets, or `AStaticMeshActor` for plain meshes).
- `ground_snap` entries line-trace down (±1 km, `ECC_WorldStatic`) for Z; no hit ⇒ skip + log.
- One `FScopedTransaction` spans the whole generation (Ctrl+Z removes everything
  atomically — including cancelled partial results); everything is tagged with the
  `generation_id` and grouped in an Outliner folder named for the prompt.

## What is stubbed / not yet implemented

- **No compilation has been run on this scaffold yet** — there was no UE installation on
  the authoring machine. Treat the first build as a verification step.
- **Multi-frame `FScopedTransaction`** is the named Phase 3 risk: the transaction is held
  open across editor ticks while chunks stream in. If it misbehaves with interleaved
  editor transactions, switch to explicit `GEditor->BeginTransaction()/EndTransaction()`.
  Verify this pattern first.
- **Generation bounds are hardcoded** to ±5000 cm (`DefaultBounds()` in
  `AetherForgeEditorModule.cpp`); panel controls for bounds are Phase 5.
- **Stats line** shows spawner-side numbers plus the last `complete.llm_ms`; frame-time
  display and assets/sec polish are Phase 5 (performance proof is an Insights trace).
- **Spawn-budget Automation test** (inject 1,000 mock entries, assert no tick exceeds the
  ~2 ms budget) is Phase 3 work and not yet written. The protocol fixture round-trip
  tests *are* written (`Private/Tests/ProtocolFixtureTests.cpp`, run with
  `Automation RunTests AetherForge.Protocol`); they resolve the shared fixtures at
  `<PluginBaseDir>/../../contract/fixtures` (override with the
  `AETHERFORGE_CONTRACT_FIXTURES` environment variable when the plugin is copied into a
  project — see `contract/README.md`). Like the rest of this scaffold they have not been
  compiled/run yet (no UE installation on the authoring machine).
- **Manifest path is fixed** (`/AetherForge/DA_AetherForgeManifest`); a settings panel /
  `UDeveloperSettings` entry is future work.
- **Reconnect is manual** (button in the error state); auto-retry beyond the initial
  sidecar-boot window is polish.
- **Seed field accepts any text**; non-numeric input is treated as "no seed" (server
  picks one and echoes it in `plan`, which the panel writes back into the field).

## Protocol notes (for maintainers)

- The USTRUCTs in `AetherForgeProtocolTypes.h` deliberately use snake_case UPROPERTY
  names (`protocol_version`, `generation_id`, ...) so `FJsonObjectConverter` round-trips
  the contract fixtures byte-compatibly. Do not "fix" the naming.
- `generate.seed` is optional on the wire, so outbound `generate` is serialized by hand
  (`FAetherForgeProtocol::SerializeGenerate`) to genuinely omit an unset seed;
  everything else goes through `FJsonObjectConverter`.
- Inbound parsing is strict: unknown `protocol_version`, unknown `type`, structurally
  broken entries, or unknown/forbidden fields (an extra `z` on a location, pitch/roll on
  a transform, per-axis scale) surface as protocol errors in the status log — never
  silently coerced or dropped. `ParseServerMessage` re-exports each imported USTRUCT and
  rejects any incoming JSON key the struct does not model; unknown chunk categories are
  rejected by `FAetherForgeProtocol::ValidateChunkCategories` against the `hello`
  manifest. The negative fixtures in `contract/fixtures/invalid/` pin this behavior on
  both sides.
