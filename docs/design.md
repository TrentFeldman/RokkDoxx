# RokkDoxx design

How the search is put together, and why the GPU gets the same answer as the CPU.

For the world-generation math itself see
[`bedrock-generation.md`](bedrock-generation.md). For the original framing see
[`../whitepaper.md`](../whitepaper.md) (note: the whitepaper describes a *halo*
scheme that the implementation does not use — see "No halo" below).

---

## Three tiers

The CPU organizes; the compute device crunches; the front-ends only render.

```
  rokktui ────┐                              ┌─ CpuWorker    (std::thread scan)
  rokksearch ─┼─ SearchClient ─▶ SearchService ┤
  (your code) ┘   in-process        (orchestrator)   └─ OpenclWorker (one GPU)
                    or unix socket ─▶ rokkd ─▶ SearchService
```

### Tier 1 — front-ends (clients)

`rokktui` (interactive) and `rokksearch` (headless) contain **no search logic**.
They build a `SearchRequest`, hand it to a `SearchClient`, and poll `JobStatus`
until the job finishes.

`SearchClient` (`src/svc/client.hpp`) has two transports, picked by a target
string:

| target | transport |
|---|---|
| `local` (default) | `InProcessClient` — owns a `SearchService` in the same process |
| `unix:/path/to.sock` | `SocketClient` — talks to a `rokkd` daemon |

Same interface either way: `submit → JobId`, `poll → JobStatus`, `results`,
`cancel`, `backend_name`.

### Tier 2 — orchestrator (`librokksvc`)

`SearchService` (`src/svc/search_service.{hpp,cpp}`) runs each job on its own
`std::thread`:

1. `BedrockGenerator(seed)` once → `derived_lo` / `derived_hi` + the per-plane
   `threshold` (see "Bit-exactness" below).
2. `TileScheduler` cuts the region into a grid of tiles. The tile side is a
   floor that keeps doubling until the tile count is sane — so a 30 M × 30 M
   world sweep does not create billions of tiles. Finished tiles are tracked and
   can be written to / resumed from a checkpoint file.
3. A `Worker` is built via the `WorkerFactory` and `configure()`d once.
4. The pump loop: `scheduler.next(tile)` → `worker.run_tile(tile)` →
   `ResultSink.add()` → `scheduler.mark_done()` → refresh `JobStatus`. It checks
   a cancel flag between tiles and saves the checkpoint every ~5 s.
5. `ResultSink` deduplicates matches by `(x, z)`, OR-ing the orientation bitmask
   (a symmetric pattern can match under several of the 8 orientations at one
   origin), caps the count, and sorts.

The scheduler / worker / sink interfaces are **device-count-agnostic**: v1
drives one device, but adding a second worker on a shared tile queue is a
change, not a rewrite.

`rokkd` (`tools/rokkd.cpp` + `src/svc/daemon.cpp`) is a thin wrapper: one
`SearchService`, a Unix-socket accept loop, length-prefixed JSON
(`src/svc/protocol.hpp`), one request/response per connection.

### Tier 3 — workers (compute)

`Worker` (`src/svc/service_types.hpp`): `configure(WorkerConfig)` once, then
`run_tile(Tile) → vector<Match>`.

- `CpuWorker` (`src/svc/workers.cpp`) — a multi-threaded scan, `rk_bits24_at`
  per pattern cell, early-out on the first mismatch, rarer cell type checked
  first.
- `OpenclWorker` (`src/svc/opencl_worker.cpp`) — one OpenCL device running
  `src/cl/search_tile.cl`, one work-item per candidate origin. Built only when
  CMake finds OpenCL.

Backend selection (`src/svc/workers.cpp`): `"cpu"`, `"opencl:N"`, or `"auto"`
(first GPU, else CPU). `rokksearch --list-backends` shows what this build/host
has.

### Job lifecycle

```
submit ─▶ pending ─▶ running ─┬─▶ done       (results ready)
                              ├─▶ cancelled  (partial results kept)
                              └─▶ error      (JobStatus.error set)
```

`poll` is cheap and safe to call at ~10 Hz — that is how both front-ends and the
socket client show live progress.

---

## Bit-exactness on any GPU

The generator is integer-only except for one step: vanilla places bedrock iff
`(double)nextFloat() < prob`. We remove the float entirely.

- `nextFloat()` is exactly `bits * 2⁻²⁴`, where `bits` is the top 24 bits of one
  Xoroshiro128++ output. Widened to `double` it is *still exactly* `bits / 2²⁴`.
- `prob_double * 2²⁴` is an **exact** power-of-two scaling (no rounding).
- For an integer `bits` and a real `t`: `bits < t  ⇔  bits < ceil(t)` (they are
  equal when `t` is integral; `ceil(t) = floor(t) + 1` otherwise). Here
  `t < 2²⁴`, so `ceil(t)` is exact in `double`.

So the **host** computes, once per job,

```
threshold = (uint32_t) ceil( prob_double * 16777216.0 )
```

and the **kernel** does `bits < threshold`. No `fp64`, no rounding modes, no
FMA contraction — the kernel is pure `ulong` / `uint` arithmetic and produces
byte-identical results to the CPU on AMD, NVIDIA, Intel, and PoCL.

`src/gen/bedrock_core.h` holds both the CPU and the kernel copy of this math in
one file (`#if defined(__OPENCL_VERSION__)`), so they cannot drift.
`tests/test_gpu.cpp` dumps a plane on the GPU and diffs it against
`BedrockGenerator` over the same adversarial coordinates the CPU differential
test uses (negative coords, ±world-border, 2³¹-overflow, extreme seeds), then
checks CPU-vs-GPU search parity including all 8 orientations.

---

## The kernel — `src/cl/search_tile.cl`

The host loads `bedrock_core.h` + `search_tile.cl` as one string at runtime and
builds it with `-cl-std=CL1.2`.

One work-item per candidate origin `(x, z)` in a 2D NDRange over a tile. For
each of `n_orient` orientations (1 or 8) it applies the D4 transform to every
known pattern cell, calls `rk_bits24_at`, compares to `threshold`, and breaks on
the first mismatch. A hit does `atomic_inc` on a global counter and appends
`(x, z, orient_mask)` if the counter is still under the cap. The host uploads
the pattern once and does one kernel dispatch + one blocking read-back per tile.

### No halo

Each work-item recomputes whatever cells it needs directly from `(x, y, z)` —
generation is ~2 ns. There is **no** shared bedrock buffer and **no** halo
region around a tile. Tile boundaries are therefore irrelevant: a pattern that
straddles two tiles is still found, because the work-item that owns its origin
reads across the boundary freely. The cost is redundant recompute between
neighbouring candidates; a local-memory tile cache to remove that is a later
optimization.

### Tile size matters

A dispatch + blocking read-back round-trip costs ~1–3 ms regardless of tile
size, so small tiles starve the GPU: 1024² blocks ran at 0.35 Gcand/s, 8192²+
at 30+. `OpenclWorker::preferred_tile_side()` returns 16384 and the scheduler
honours it (the CPU worker keeps small tiles for progress granularity).
Synchronous per-tile dispatch is still the model — async double-buffering is a
later ~1.5× win.

---

## Running the GPU path

OpenCL is optional. CMake builds `OpenclWorker` only if it finds the loader +
headers.

**Arch Linux:**

```sh
sudo pacman -S opencl-headers opencl-clhpp          # build deps
# a platform -- pick one:
sudo pacman -S rocm-opencl-runtime                  # AMD RDNA (RX 7900 XTX = gfx1100)
sudo pacman -S pocl                                 # CPU OpenCL, for CI / no-GPU dev
sudo pacman -S opencl-mesa                          # Rusticl (fallback)
clinfo                                              # confirm the device shows up
```

Then:

```sh
cmake -B build -DROKK_ENABLE_OPENCL=ON && cmake --build build
ctest --test-dir build --output-on-failure          # runs the `gpu` test
build/rokksearch --list-backends
build/rokksearch --backend opencl:0 --bench --pattern p.txt --center 0,0 --radius 2000000
```

`build.sh` path: `ROKK_OPENCL=1 ./build.sh test`.

---

## Measured — RX 7900 XTX (gfx1100, ROCm 7.2.4)

8×8 pattern, `rokksearch --bench`. CPU = 6-core / 12-thread Ryzen 5 5600.

| | candidate origins/s |
|---|---|
| exact orientation | **~76 G** (≈110× the 12-thread CPU) |
| all 8 orientations | **~10 G** |

`dump_plane` (a trivial 1-cell kernel, memory-write-bound): ~6.8 Gcell/s at
4096². The search kernel is *faster* than the dump because it writes almost
nothing — it is pure ALU with near-zero inner-loop memory traffic, exactly the
shape a GPU wants.

Time to search a region (GPU, exact orientation):

| region | candidate origins | GPU exact | GPU all-8 |
|---|---|---|---|
| radius 10 k | 4·10⁸ | instant | instant |
| radius 1 M | 4·10¹² | ~50 s | ~6 min |
| ± Nether world border (±3.75 M) | 5.6·10¹³ | ~12 min | ~1.5 h |
| full Overworld border (30 M × 30 M) | 9·10¹⁴ | **~3.5 h** | ~24 h |

On CPU, multiply by ~110. So: the CPU is fine once you know your rough
location; the GPU makes a blind whole-world sweep practical.

- gfx1100 runs Wave32 by default; the kernel does not assume a wavefront size.
- A full-border run grows tiles past 16384 to stay under the scheduler's
  tile-count cap.
