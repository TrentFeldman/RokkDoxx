# RokkDoxx — scope

This file exists so that unrequested growth is easy to notice. If a change adds
a file, a dependency, or a capability that is not described here, that is a
signal to stop and check it was actually asked for — not a reason to update this
file to match.

---

## In scope

Reproduce Minecraft **26.2** Overworld bedrock-**floor** generation exactly, as a
pure function `B(seed, x, y, z) → {bedrock, not bedrock}`, and search the block
coordinate space for a user-supplied 2D pattern on one Y layer. Run the search
on the CPU (multi-threaded) or on **one** OpenCL device. Drive it from a
terminal UI or a headless CLI, in-process or through a local daemon.

That is the whole product. Everything below is deliberately *not* built.

## Non-goals

- **No game engine.** No chunk generation, no terrain noise, no block storage,
  no world files. The only Minecraft code path reproduced is the bedrock surface
  rule and the RNG it uses.
- **Overworld floor only.** Not the Nether roof (`minecraft:bedrock_roof`), not
  any other dimension or surface rule.
- **Single Y-plane patterns.** No volumetric (multi-layer) patterns.
- **One compute device per search.** No multi-GPU scheduler, no distributed
  workers. The interfaces are device-count-agnostic on purpose, but only one
  worker is wired up.
- **Local IPC only.** `rokkd` listens on a Unix domain socket. It is not a
  network service and has no authentication, TLS, or remote transport.
- **Terminal only.** `rokktui` (full-screen TUI) and `rokksearch` (CLI). No GUI,
  no web front-end.
- **No new third-party dependencies.** The vendored MD5 and the hand-rolled JSON
  (`src/svc/protocol.hpp`) are what "dependency-free" costs; keep it that way.
  OpenCL (headers + an ICD loader) is the one optional external dependency.
- **No ground-truth check against the real client** is claimed. Verification is
  against Java-generated RNG vectors, an independent Python reimplementation,
  and three community implementations — see
  [`bedrock-generation.md`](bedrock-generation.md).

Known future work is tracked in the plan file, not here. Listing something as a
non-goal is not a promise it will never be built — it means it is not built now
and adding it is a scope decision, not a detail.

---

## File manifest

Every non-generated source file and what it is responsible for. If you are
reviewing a diff, this is the list to check it against.

### `src/gen/` — the generation function (no search, no I/O)

| file | responsibility |
|---|---|
| `bedrock_core.h` | The generation math in the common subset of C99 / C++ / OpenCL C. One source of truth for the CPU and the GPU. Integer-only; the float compare is replaced by `bits24 < threshold`. |
| `xoroshiro128pp.hpp` | C++ wrapper over the core: `Xoroshiro128PP` (seed upgrade, `next`, `nextFloat`). |
| `positional_random.hpp` | C++ wrapper: `forkPositional`, `Mth.getSeed` block-position hash, `bits24_at`. |
| `md5.hpp` | Vendored MD5 (RFC 1321). Host-only; hashes the surface-rule id `"minecraft:bedrock_floor"` once per generator. |
| `bedrock.hpp` / `bedrock.cpp` | `BedrockGenerator`: world seed → the forked per-seed state (`derived_lo/hi`) + per-plane thresholds; `is_bedrock_floor(x, y, z)`. |

### `src/cl/` — the GPU kernel

| file | responsibility |
|---|---|
| `search_tile.cl` | `search_tile` (one work-item per candidate origin, all orientations, atomic append of hits) and `dump_plane` (bit-exactness test helper). `bedrock_core.h` is prepended by the host. |

### `src/svc/` — the search service (`librokksvc`)

| file | responsibility |
|---|---|
| `service_types.hpp` | The plain data types everything else speaks: `Pattern`, `Region`, `SearchRequest`, `Match`, `JobStatus`, `Tile`, `WorkerConfig`, the `Worker` interface; `seed_from_string`. |
| `search_service.hpp` / `.cpp` | Orchestration: `TileScheduler` (tiling + checkpoint/resume), `ResultSink` (dedup + orientation-mask merge), `SearchService` (per-job thread, the tile pump, progress, cancel). |
| `workers.hpp` / `.cpp` | `CpuWorker` (multi-threaded CPU scan) and backend selection (`list_backends`, `make_worker_factory`). Always compiled; never includes an OpenCL header. |
| `opencl_worker.hpp` / `.cpp` | `OpenclWorker`: one OpenCL device, kernel build, one dispatch + read-back per tile. Compiled only when OpenCL is found. |
| `protocol.hpp` / `.cpp` | The `rokkd` wire format: the minimal `Json` type, request/status/matches conversions, 4-byte length-prefixed framing. |
| `client.hpp` / `.cpp` | `SearchClient` and its two transports: `InProcessClient` (owns a `SearchService`) and `SocketClient` (talks to `rokkd`). |
| `daemon.hpp` / `.cpp` | The `rokkd` accept loop: bind a Unix socket, dispatch protocol messages to a `SearchService`. |
| `pattern_io.hpp` / `.cpp` | Load / save the on-disk pattern file format shared by both front-ends. |

### `tools/` — executables

| file | responsibility |
|---|---|
| `dump_bedrock.cpp` | Print the bedrock floor for a seed + region as an ASCII grid. Links only `rokkdoxx_gen`. |
| `rokktui.cpp` | Interactive full-screen client: enter a seed, paint a pattern, run, watch progress. |
| `rokksearch.cpp` | Headless client: flags or a pattern file in, matches out; `--bench`, `--json`, `--checkpoint`. |
| `rokkd.cpp` | The daemon `main`: one `SearchService` behind `serve()`. |

### `tests/`

| file | responsibility |
|---|---|
| `test_bedrock.cpp` | Generator unit + known-answer tests (MD5, Java RNG vectors, gradient stats, grid fingerprints). |
| `test_search.cpp` | Service vs. a direct `BedrockGenerator` brute force; tiling invariance; cancel; checkpoint; pattern-file round-trip. |
| `test_daemon.cpp` | The socket path returns exactly what the in-process path does. |
| `test_gpu.cpp` | GPU `dump_plane` vs. CPU byte-for-byte; CPU/GPU search parity. Passes (with a note) when no device is present. |
| `reference/bedrock_ref.py` | Independent Python reimplementation of `B(seed, x, y, z)`. Same CLI as `dump_bedrock`. |
| `diff_test.py` | Differential test: compiled `dump_bedrock` vs. `bedrock_ref.py`, byte-identical over adversarial seeds/regions. |

---

## Before adding a file, a dependency, or a Non-goal item

Answer these in the commit message:

1. **What asked for this?** A user request, a failing test, a concrete bug — not
   "it seemed useful" or "for future flexibility".
2. **Which Non-goal does it touch?** If it moves one of the lines above, say so
   explicitly; that is a scope change and should be called out, not slipped in.
3. **Can an existing file hold it?** The `src/svc/` layout is 8 units on purpose.
   Prefer growing one over adding a ninth.
4. **What test proves it works, and does the existing suite still pass
   unchanged?**
