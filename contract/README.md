# AetherForge Protocol Contract — v1

This directory is the **single source of truth** for the wire protocol between the UE5
plugin (client) and the Go orchestration service (server). The canonical JSON fixtures in
[`fixtures/`](fixtures/) are consumed byte-for-byte by both the Go test suite and the UE
Automation tests, so both sides verify against identical data.

Spec of record: [`docs/superpowers/specs/2026-06-10-aetherforge-prd-design.md`](../docs/superpowers/specs/2026-06-10-aetherforge-prd-design.md).

## Transport & framing

- JSON messages over a single WebSocket connection on `127.0.0.1:8080` (localhost only).
- One JSON object per WebSocket text frame.
- Every message carries:
  - `protocol_version` — integer, currently `1`.
  - `type` — one of `hello`, `generate`, `plan`, `chunk`, `complete`, `error`, `cancel`.
  - `generation_id` — string, on every message **after** `hello` (the client mints it on
    `generate`; the server echoes it on everything it sends for that generation).

## Versioning semantics

- `protocol_version` starts at `1` and is bumped on any breaking wire change.
- The server validates `protocol_version` at `hello`. A mismatch **fails loudly**: the
  server replies with an `error` message (`code: "protocol_version_mismatch"`,
  `recoverable: false`) and closes the connection. No best-effort negotiation.

## Message lifecycle

```
client → server   hello     {protocol_version, manifest}
client → server   generate  {generation_id, prompt, seed?, bounds}
server → client   plan      {generation_id, seed, total_assets, categories: {key: count}}
server → client   chunk     {generation_id, assets: [...]}        (repeated)
server → client   complete  {generation_id, stats: {assets, elapsed_ms, llm_ms}}
server → client   error     {generation_id, code, message, recoverable}
client → server   cancel    {generation_id}
```

- `hello` is sent exactly once per connection, before anything else.
- `plan` always arrives before the first `chunk`; it drives the determinate progress bar
  ("0 / 1,240").
- `chunk` repeats (~50 asset entries per message) until the generation is done.
- A generation terminates with exactly one of `complete`, `error`, or cancellation.
- One generation per connection at a time: a new `generate` while one is running cancels
  the old one.

## Determinism & seeding

- `seed` is **optional** on `generate`. If omitted, the server picks one and **echoes it
  in `plan`** (so `seed` is always present on `plan`).
- Same prompt + same seed ⇒ identical recipe + identical placement ⇒ identical asset
  stream. The placement engine is pure: `(recipe, seed, bounds, manifest) → []AssetEntry`.

## Cancellation semantics

- The client sends `cancel` (or simply closes the socket). Either cancels the
  generation's `context.Context` in Go, stopping the LLM call and the placement loop.
- The server stops emitting `chunk` messages for that `generation_id`. No `complete`
  follows a cancellation.
- The plugin keeps already-spawned assets inside its open `FScopedTransaction`, so a
  cancelled (partial) generation is still removed atomically by Ctrl+Z.

---

## Message reference

### `hello` — client → server

Manifest handshake. The plugin advertises its asset catalog; from this point on the
backend works **only** in these semantic categories — it never sees Unreal asset paths.
The backend validates every LLM recipe against this manifest, and the LLM may only select
from these keys.

| Field | Type | Required | Description |
|---|---|---|---|
| `protocol_version` | integer | yes | Must be `1`. Mismatch ⇒ `error` + connection close. |
| `type` | string | yes | `"hello"` |
| `manifest` | object | yes | Map of semantic key → manifest entry (see schema below). |

Fixture: [`fixtures/hello.json`](fixtures/hello.json)

### `generate` — client → server

Starts a generation. Sending `generate` while another generation is in flight on the same
connection cancels the in-flight one.

| Field | Type | Required | Description |
|---|---|---|---|
| `protocol_version` | integer | yes | `1` |
| `type` | string | yes | `"generate"` |
| `generation_id` | string | yes | Client-minted unique id; echoed on all server messages for this generation. |
| `prompt` | string | yes | Natural-language scene description. |
| `seed` | integer | no | Reproducibility seed. If omitted, the server picks one and echoes it in `plan`. |
| `bounds` | object | yes | 2D generation area: `{"min": {"x", "y"}, "max": {"x", "y"}}` in Unreal world units (cm). |

Fixture: [`fixtures/generate.json`](fixtures/generate.json)

### `plan` — server → client

Sent once per generation, before the first `chunk`. Drives the determinate progress bar.

| Field | Type | Required | Description |
|---|---|---|---|
| `protocol_version` | integer | yes | `1` |
| `type` | string | yes | `"plan"` |
| `generation_id` | string | yes | Echo of the `generate` id. |
| `seed` | integer | yes | The seed in effect (echo of the request seed, or server-chosen if the request omitted it). |
| `total_assets` | integer | yes | Total asset count for the whole generation. |
| `categories` | object | yes | Map of semantic key → integer count; counts sum to `total_assets`. |

Fixture: [`fixtures/plan.json`](fixtures/plan.json)

### `chunk` — server → client (repeated)

A batch of placed assets (~50 per message), streamed as placement produces them.

| Field | Type | Required | Description |
|---|---|---|---|
| `protocol_version` | integer | yes | `1` |
| `type` | string | yes | `"chunk"` |
| `generation_id` | string | yes | Echo of the `generate` id. |
| `assets` | array | yes | Array of asset entries (see schema below). |

Fixture: [`fixtures/chunk.json`](fixtures/chunk.json)

### `complete` — server → client

Terminal message for a successful generation.

| Field | Type | Required | Description |
|---|---|---|---|
| `protocol_version` | integer | yes | `1` |
| `type` | string | yes | `"complete"` |
| `generation_id` | string | yes | Echo of the `generate` id. |
| `stats` | object | yes | `{"assets": int, "elapsed_ms": int, "llm_ms": int}` — total assets streamed, wall-clock generation time, and time spent in the LLM call. |

Fixture: [`fixtures/complete.json`](fixtures/complete.json)

### `error` — server → client

Terminal message for a failed generation (or a handshake failure, in which case
`generation_id` is the empty string).

| Field | Type | Required | Description |
|---|---|---|---|
| `protocol_version` | integer | yes | `1` |
| `type` | string | yes | `"error"` |
| `generation_id` | string | yes | Echo of the `generate` id (`""` for pre-generation errors such as a `hello` version mismatch). |
| `code` | string | yes | Machine-readable code, e.g. `protocol_version_mismatch`, `llm_invalid_output`, `llm_timeout`, `unknown_category`, `internal`. |
| `message` | string | yes | Human-readable description, surfaced in the panel's status log. |
| `recoverable` | boolean | yes | `true` ⇒ the connection stays usable and the client may issue a new `generate`; `false` ⇒ the server closes the connection. |

Notes: LLM output that fails schema validation gets **one** retry with the validation
error fed back; a second failure produces this message (`recoverable: true`).

Fixture: [`fixtures/error.json`](fixtures/error.json)

### `cancel` — client → server

| Field | Type | Required | Description |
|---|---|---|---|
| `protocol_version` | integer | yes | `1` |
| `type` | string | yes | `"cancel"` |
| `generation_id` | string | yes | The generation to cancel. |

See [Cancellation semantics](#cancellation-semantics) above.

Fixture: [`fixtures/cancel.json`](fixtures/cancel.json)

---

## Asset entry schema (elements of `chunk.assets`)

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

| Field | Type | Required | Description |
|---|---|---|---|
| `category` | string | yes | Semantic key; must exist in the manifest sent at `hello`. The plugin resolves it to an asset — the wire never carries Unreal asset paths. |
| `transform.location` | object | yes | 2D position `{"x", "y"}` in world units (cm). **No `z`** — the plugin owns Z. |
| `transform.yaw` | number | yes | Yaw-only rotation in degrees. No pitch/roll. |
| `transform.scale` | number | yes | Uniform scale factor. |
| `ground_snap` | boolean | yes | `true` ⇒ the plugin line-traces the entry to the terrain at spawn time to find Z. |
| `tags` | array of strings | yes | Semantic tags (e.g. `flora`, `canopy`, `fauna`) for outliner grouping/filtering. May be empty. |

**Strictness rule.** Anything beyond the fields above is a contract violation and **must be
rejected by validators on both sides** — never silently dropped. Concretely:

- a `z` on `transform.location`, pitch/roll on `transform`, per-axis (object) `scale`, or
  any other unknown field is rejected at parse time (Go: `json.Decoder` with
  `DisallowUnknownFields`; UE: `FAetherForgeProtocol::ParseServerMessage` re-exports the
  imported struct and rejects any incoming key the struct does not model);
- a `protocol_version` other than `1` is rejected on **every** message, not just `hello`;
- a `category` not present in the `hello` manifest is rejected by manifest validation
  (Go: `Chunk.ValidateCategories`; UE: `FAetherForgeProtocol::ValidateChunkCategories`).

The negative fixtures under [`fixtures/invalid/`](fixtures/invalid/) prove each of these
rejections in both test suites.

## Manifest entry schema (values of `hello.manifest`)

The manifest is a JSON object keyed by semantic category. On the UE side it is backed by
a `UDataAsset` (semantic key → `FSoftObjectPath` + metadata); only the metadata below
crosses the wire — never asset paths.

```json
"deciduous_tree": {
  "footprint_radius": 250.0,
  "ground_snap": true,
  "instanceable": true,
  "display_name": "Deciduous Tree"
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `footprint_radius` | number | yes | Collision footprint radius in world units (cm); used by the placement engine's spatial-hash overlap rejection — no two footprints may overlap. |
| `ground_snap` | boolean | yes | Whether spawned entries of this category snap to terrain via line trace. |
| `instanceable` | boolean | yes | `true` ⇒ spawned into per-category HISM pools; `false` ⇒ `SpawnActor` (interactive categories such as creatures). |
| `display_name` | string | yes | Human-readable name for UI/logging. |

## Fixtures

One canonical fixture per message type, depicting a single coherent generation
(`generation_id: "gen-7f3a9c12"`, seed `1337`, the spec's "forest clearing with Pacific
Northwest deciduous trees, scattered shrubs, and two large dogs" prompt):

| File | Message |
|---|---|
| `fixtures/hello.json` | `hello` with a manifest containing `deciduous_tree`, `shrub`, `boulder`, `dog_large` |
| `fixtures/generate.json` | `generate` with explicit seed and bounds |
| `fixtures/plan.json` | `plan` echoing the seed, with per-category counts |
| `fixtures/chunk.json` | `chunk` with a mixed batch of asset entries |
| `fixtures/complete.json` | `complete` with stats |
| `fixtures/error.json` | `error` (LLM schema-invalid after retry, recoverable) |
| `fixtures/cancel.json` | `cancel` |

Both test suites must round-trip these exact bytes: parse → model → serialize → parse,
and assert semantic equality with the original.

### Negative fixtures (`fixtures/invalid/`)

Each encodes one contract violation; both test suites must assert it is **rejected** with
a descriptive error:

| File | Violation | Rejected by |
|---|---|---|
| `invalid/bad_protocol_version.json` | `hello` with `protocol_version: 2` | version check (Go `protocol.Decode`, UE `ParseServerMessage`) |
| `invalid/unknown_category.json` | `chunk` asset with category `cactus`, absent from the `hello` manifest | manifest category validation |
| `invalid/asset_with_z.json` | `chunk` asset with a forbidden `z` on `transform.location` | unknown-field rejection |
| `invalid/asset_full_rotation.json` | `chunk` asset with forbidden `pitch`/`roll` on `transform` | unknown-field rejection |

### How the test suites consume the fixtures

- **Go** — `backend/internal/protocol/fixtures_test.go` reads
  `../../../contract/fixtures` (repo-root-relative from the package directory;
  `//go:embed` is out since the fixtures live outside the module). Run:
  `cd backend && go test ./internal/protocol/...`
- **UE** — `unreal/AetherForge/Source/AetherForgeEditor/Private/Tests/ProtocolFixtureTests.cpp`
  resolves `<PluginBaseDir>/../../contract/fixtures` via
  `IPluginManager::Get().FindPlugin("AetherForge")->GetBaseDir()` (the in-repo layout);
  set the `AETHERFORGE_CONTRACT_FIXTURES` environment variable to override when the
  plugin is copied into a project. Headless CI invocation:

  ```
  UnrealEditor-Cmd <Project>.uproject -ExecCmds="Automation RunTests AetherForge.Protocol; Quit" -unattended -nopause -nullrhi -nosplash -log
  ```
