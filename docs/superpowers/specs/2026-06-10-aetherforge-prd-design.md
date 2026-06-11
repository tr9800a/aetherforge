# AetherForge — Revised PRD

**Unreal Engine GenAI World-Building Plugin**
Revision date: 2026-06-10 · Supersedes: `project_idea.txt`

## 1. Project Overview

AetherForge is an Unreal Engine 5 (UE5) Editor plugin paired with a Go orchestration service that procedurally generates and populates 3D environments from natural-language prompts. A designer types "a forest clearing with Pacific Northwest deciduous trees, scattered shrubs, and two large dogs," and the level fills in — streamed into the viewport as it generates, undoable with Ctrl+Z, and reproducible from a seed.

**Design principle (governs every decision below):** the LLM is used only for what it is good at — translating intent into a small semantic *scene recipe*. Everything else — spatial placement, collision avoidance, asset resolution, validation — is deterministic code. This is what makes the system stable, testable, and fast.

**Performance contract:** the editor viewport must never hitch. All spawning is time-budgeted (~2 ms per tick); a 1,000-asset generation streams in over multiple frames with no frame exceeding 16 ms.

## 2. Architecture & Tech Stack

| Layer | Technology |
|---|---|
| Orchestrator | Go service (the plugin launches and manages it as a sidecar) |
| Engine client | UE 5.x C++ Editor plugin (module type: `Editor`), Slate UI |
| Transport | WebSocket on `127.0.0.1:8080` (localhost only) |
| AI | Local LLM via Ollama (e.g. Llama 3), using structured output (`format` with a JSON schema) |
| Placement | Deterministic Go engine: Poisson disk sampling + spatial hash overlap rejection |

### Component responsibilities

```
UE5 Plugin                          Go Service
┌─────────────────────────┐        ┌──────────────────────────────┐
│ Slate panel (prompt,    │  WS    │ wsHandler                    │
│  progress, cancel)      │◄──────►│   └─ orchestrator            │
│ Sidecar process manager │        │       ├─ llmClient (Ollama,  │
│ Asset manifest provider │        │       │   behind interface)  │
│ Network client + JSON   │        │       ├─ placementEngine     │
│ Time-budgeted spawner   │        │       │   (pure, seeded)     │
│ Undo/transaction layer  │        │       └─ manifestStore       │
└─────────────────────────┘        └──────────────────────────────┘
```

- **The plugin owns the sidecar lifecycle.** On module startup it launches the Go binary via `FPlatformProcess::CreateProc`, health-checks it, and terminates it on shutdown. Designers never touch a terminal.
- **The backend never sees Unreal asset paths.** It works in semantic categories; the plugin resolves them via its manifest.
- **The backend emits 2D layout (X/Y, yaw, scale).** The plugin owns Z: assets flagged `ground_snap` are line-traced to the terrain at spawn time.

## 3. The Pipeline

1. **Recipe generation (LLM, one call).** The prompt plus the list of available semantic categories goes to Ollama with a JSON-schema-constrained response. Output is a *scene recipe*: categories, counts, density, clustering hints, exclusion zones (e.g. "clearing in the center"). Hard caps clamp runaway counts. Invalid output gets one retry with the validation error fed back; a second failure returns a structured `error` message.
2. **Placement (deterministic Go).** The placement engine converts (recipe, seed, bounds) into transforms: Poisson disk sampling for natural scatter, per-category footprint radii from the manifest, spatial-hash rejection so nothing overlaps, cluster rules (flora clumps; creatures get clear space). Fully deterministic: same recipe + same seed = identical layout.
3. **Streaming.** Transforms are chunked (~50 assets per message) and streamed to the plugin as they are produced, so the world grows in live.
4. **Spawning (UE, time-budgeted).** The plugin async-loads each category's asset via `FStreamableManager`, then drains its spawn queue under a ~2 ms-per-tick budget. Instanceable categories (trees, rocks, shrubs) go into `HierarchicalInstancedStaticMeshComponent` pools; interactive categories (creatures) use `SpawnActor`. The whole generation is wrapped in one `FScopedTransaction` (Ctrl+Z removes it atomically), tagged with its `generation_id`, and grouped in a World Outliner folder.

## 4. Protocol Contract (v1)

All messages are JSON over one WebSocket connection. Every message carries `protocol_version` (integer, starts at 1) and, after `hello`, a `generation_id`. Mismatched versions fail loudly at `hello`.

### Message lifecycle

```
client → server   hello     {protocol_version, manifest}
client → server   generate  {generation_id, prompt, seed?, bounds}
server → client   plan      {generation_id, total_assets, categories: {key: count}}
server → client   chunk     {generation_id, assets: [...]}        (repeated)
server → client   complete  {generation_id, stats: {assets, elapsed_ms, llm_ms}}
server → client   error     {generation_id, code, message, recoverable}
client → server   cancel    {generation_id}
```

- **`hello` / manifest handshake:** the plugin advertises its asset catalog — for each semantic key: footprint radius, `ground_snap`, `instanceable`, display name. The backend validates every recipe against this manifest and the LLM only ever selects from these keys.
- **`plan`** arrives before the first chunk and drives the determinate progress bar ("0 / 1,240").
- **`cancel`** (or socket close) cancels the generation's `context.Context` in Go, stopping the LLM call and placement loop. The plugin keeps already-spawned assets inside the open transaction so undo still works.
- **`seed`** is optional on `generate`; if omitted the server picks one and echoes it in `plan`. Same prompt + same seed reproduces the world exactly.

### Asset entry schema (inside `chunk`)

```json
{
  "category": "deciduous_tree",
  "transform": {
    "location": {"x": 150.0, "y": -200.0},
    "yaw": 45.0,
    "scale": 1.2
  },
  "ground_snap": true,
  "tags": ["flora", "canopy"]
}
```

No `z`, no full rotation, no asset paths: the plugin resolves `category` through its manifest, snaps to terrain, and applies yaw-only rotation (uniform scale) — sufficient for environment scatter and far harder to get wrong.

## 5. Backend Requirements (Go)

- **Structure:** `wsHandler → orchestrator → {llmClient, placementEngine, manifestStore}`. The LLM sits behind an interface with a canned-response fake so the full pipeline runs in tests with no model loaded.
- **Placement engine is pure:** `(recipe, seed, bounds, manifest) → []AssetEntry`. No I/O, no clock, no global state.
- **LLM hygiene:** request timeout (local models stall), one retry on schema-invalid output with the error fed back, hard caps on counts and recipe size.
- **Record/replay:** a flag records LLM responses to disk and replays them — deterministic demos and CI without a GPU.
- **Concurrency:** one generation per connection at a time; a new `generate` while one is running cancels the old one. Each generation runs in its own goroutine tied to a cancellable context.
- **Not a requirement:** GC tuning. Payloads are a few thousand small structs; chunked streaming already bounds memory. (Removed from the original PRD as a non-problem.)

## 6. Frontend Requirements (UE5 C++ Plugin)

- **Module type `Editor`** — nothing ships in packaged builds.
- **Slate panel**, dockable: multi-line prompt box, Generate button, seed field + "New Variation" button, determinate progress bar, Cancel button, live stats line (assets/sec, frame time, LLM latency), scrolling status log.
- **Sidecar manager:** launch/health-check/terminate the Go binary; surface a clear error state in the panel if it dies.
- **Network client:** `IWebSocket` (WebSockets module). JSON parsing happens off the game thread (`AsyncTask`); parsed entries are marshaled back to a game-thread spawn queue.
- **Spawner:**
  - Time-budgeted: each editor tick, spawn until ~2 ms (`FPlatformTime`) is spent, then yield. Never a fixed count per frame.
  - Async-load all assets via `FStreamableManager` before their entries are eligible to spawn; never synchronous-load in the spawn tick.
  - Instanceable categories → per-category HISM components; others → `SpawnActor`.
  - Ground snap via line trace per `ground_snap` entry.
  - One `FScopedTransaction` per generation; actors/instances tagged with `generation_id`; grouped in an Outliner folder named for the prompt.
- **Manifest:** a `UDataAsset` mapping semantic keys → `FSoftObjectPath` + metadata (footprint radius, instanceable, ground_snap). Project teams extend it to expose their own asset libraries — no code changes.

## 7. Testing Strategy

- **Shared contract fixtures:** canned JSON for every message type lives in one directory, consumed by both the Go test suite and UE Automation tests, so both sides verify against identical bytes.
- **Placement invariants (Go, property-style):** across hundreds of random seeds — no two footprints overlap, all assets within bounds, counts match recipe, identical seed ⇒ identical output.
- **Pipeline tests (Go):** full orchestrator runs against the fake LLM, including the invalid-JSON retry path and cancellation mid-stream.
- **UE Automation tests:** fixture round-trip through the JSON layer; spawn-queue budget test (inject 1,000 mock entries, assert no tick exceeds budget).
- **Performance proof:** Unreal Insights trace of a 1,000-asset generation showing no frame over 16 ms — captured as a phase artifact, not assumed.

## 8. Implementation Phases

Each phase has an acceptance criterion; a phase is done when its criterion is demonstrated, not before.

**Phase 0 — Contract & toolchain (de-risk first)**
Write the protocol fixtures and the manifest `UDataAsset` format; compile a hello-world UE plugin to prove the toolchain.
*Criterion: Go tests and a UE Automation test both round-trip the shared fixtures; empty plugin loads in the editor.*

**Phase 1 — Backend scaffold (Go)**
WebSocket server, mock pipeline (fake LLM + real placement engine) streaming plan → chunks → complete.
*Criterion: `websocat` sends `hello` + `generate` and receives a complete, schema-valid stream; placement invariant tests pass.*

**Phase 2 — Frontend scaffold (UE)**
Dockable Slate panel, sidecar launch, WebSocket connection, JSON layer.
*Criterion: panel connects, streams mock chunks into the status log with a live progress bar; killing the sidecar shows the error state.*

**Phase 3 — Spawning loop**
Time-budgeted spawner, HISM pools, ground snapping, transactions, tagging, cancellation.
*Criterion: 1,000 mock assets spawn with no frame over 16 ms (Insights trace); Ctrl+Z removes the entire generation; Cancel stops the stream and leaves an undoable partial result.*

**Phase 4 — LLM integration**
Ollama client with schema-constrained output, retry-on-invalid, caps, record/replay.
*Criterion: 10 varied prompts produce schema-valid recipes with zero violations; replay mode reruns a recorded session with no model running.*

**Phase 5 — Polish & demo**
Seed/"New Variation" flow, live stats, semantic placement touches (clearings clear, edge-density gradients), curated demo level and script.
*Criterion: scripted end-to-end demo — prompt → world grows in live → variation → undo — runs reliably back to back.*

**Out of scope for v1 (explicit):** incremental edits to an existing generation ("add a stream through the clearing"), runtime (in-game) generation, multi-user collaboration, cloud-hosted LLMs. Incremental edits are the headline v2 candidate.

## 9. Risks

| Risk | Mitigation |
|---|---|
| UE C++ plugin is the heaviest lift (build times, Slate learning curve) | Toolchain proof in Phase 0; UI kept minimal until Phase 5 |
| Local LLM output quality varies by machine/model | Schema-constrained output + validation + retry; record/replay decouples demos and CI from the model |
| Editor viewport isn't real-time by default, muddying FPS claims | Performance criteria defined as per-frame time in an Insights trace, not viewport FPS |
| Contract drift between Go and UE | Shared fixtures + `protocol_version` hard-fail at handshake |
