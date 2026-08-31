# RokkDoxx

**Find bedrock formations in a Minecraft world by math, not by exploring.**

RokkDoxx reproduces Minecraft's bedrock world-generation rule as a plain function and
searches the coordinate space for a target bedrock pattern — without running the game
engine. Given a world seed and a picture of some bedrock (e.g. the floor of the Nether, or
a hole dug to bedrock), it tells you where in the world that pattern occurs.

The design goal ([`whitepaper.md`](whitepaper.md)) is a GPU/OpenCL
search over the full `30,000,000 × 30,000,000` world. The architecture is three tiers, a thin
tui front end, a CPU scheduler that tiles the region and validates results, and a compute
worker (GPU or CPU). As a full time student, this project's scope is very narrow.[`docs/SCOPE.md`](docs/SCOPE.md).

---

## Contents

- [Quick start](#quick-start)
- [What's implemented](#whats-implemented)
- [Requirements](#requirements)
- [Build](#build)
- [Usage](#usage) — [`rokktui`](#rokktui--interactive-pattern-search),
  [`rokksearch`](#rokksearch--headless-search), [`rokkd`](#rokkd--search-daemon),
  [`dump_bedrock`](#dump_bedrock--print-the-bedrock-layer-for-a-seed), [Python](#testsreferencebedrock_refpy--same-thing-in-python), [C++ library](#c-library)
- [How to use it to actually find a location](#how-to-use-it-to-actually-find-a-location)
- [Performance](#performance)
- [Project layout](#project-layout)
- [Verification](#verification)
- [Contributing](#contributing)
- [License](#license)

---

## Quick start

```sh
git clone https://github.com/TrentFeldman/RokkDoxx RokkDoxx && cd RokkDoxx

cmake -B build -DROKK_ENABLE_OPENCL=ON     # DROKK_ENABLE_OPENCL=ON for GPU compute
cmake --build build
ctest --test-dir build --output-on-failure

build/rokktui                             # interactive pattern search
```

No `make`/`ninja`? Use the fallback: `./build.sh` (or `ROKK_OPENCL=1 ./build.sh`), then
`./build.sh test`.

---

## What's implemented

| Component | State |
|---|---|
| Minecraft 26.2 Overworld bedrock-floor generation (`B(seed, x, y, z)`) | ✅ verified |
| Shared C/OpenCL generation core (`bedrock_core.h`) | ✅ |
| Search service — tiling, scheduling, dedup, progress, cancel, resume | ✅ |
| `rokkd` daemon + Unix-socket protocol | ✅ |
| `rokktui` (interactive) / `rokksearch` (headless) | ✅ |
| CPU worker (multi-threaded) | ✅ |
| OpenCL worker — all 8 orientations, bit-exact with CPU | ✅ |
| Local-memory tile cache, async tile pipelining | IN PROGRESS |
| Nether roof (`bedrock_roof`), multi-Y patterns | ⬜ later |
| `rokktui` redo | ⬜ later |
| Further optimizations, early test rejections | ⬜ later |
| Work on optimizing 8 direction increased search times| ⬜ later | 

The generation logic is documented in [`docs/bedrock-generation.md`](docs/bedrock-generation.md).
Short version:

- Bedrock floor is a **surface rule** (`minecraft:bedrock_floor`) with a `vertical_gradient`
  condition. It depends only on the seed and block coordinates — not biome, terrain, or
  structures — which is what makes an engine-free search possible.
- `y = -64` is always bedrock; `y ≥ -59` never is; `y = -63..-60` fade out linearly
  (probability 0.8, 0.6, 0.4, 0.2).
- The RNG is **Xoroshiro128++** (`XoroshiroRandomSource`), unchanged since Java 1.18.
- The GPU search is **bit-exact** with the CPU: the one float compare is replaced by a
  host-precomputed integer threshold, so the kernel is pure integer math on any device.
  See [`docs/design.md`](docs/design.md).

---

## Requirements

- A C++20 compiler (`g++` ≥ 13 or `clang++` ≥ 16)
- **Primary build:** CMake ≥ 3.16 + `make` or `ninja`
- **Fallback build:** just `bash` — [`build.sh`](build.sh) compiles everything with one
  `g++` invocation per target, no build system needed
- Python 3 — for the reference implementation and the differential test
- *Optional, for the GPU worker:* `opencl-headers`, `opencl-clhpp`, and one OpenCL
  platform (`rocm-opencl-runtime` for an AMD RX 7900 XTX, `pocl` for CPU OpenCL, or
  `opencl-mesa` for Rusticl). See [`docs/design.md`](docs/design.md).

---
## CURRENTLY LINUX ONLY! WORKING ON WINDOWS COMPATABILITY 

## Build

### CMake (preferred)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release        # add -DROKK_ENABLE_OPENCL=ON for the GPU worker
cmake --build build
ctest --test-dir build --output-on-failure
```

Produces `build/{dump_bedrock, rokktui, rokksearch, rokkd, test_*}`.

### Fallback (`build.sh`, no build system)

```sh
./build.sh                 # CPU only
ROKK_OPENCL=1 ./build.sh   # + GPU worker (needs libOpenCL + headers)
./build.sh test            # build, then run the test suite
```

Both paths build the same targets and run the same tests; `build.sh` exists so the
project builds on a box with a compiler but no `make`/`ninja`.

---

## Usage

### `rokktui` — interactive pattern search

```sh
build/rokktui              # or: build/rokktui --load pattern.txt
```

HOW TO USE ROKKTUI

1. **Parameters screen.** Up/Down (or Tab) to move between fields, type to edit.
   - `seed` — numeric (may be negative), or any text string (hashed the way Minecraft
     hashes non-numeric seeds).
   - `width` / `height` — the size of the bedrock pattern you're going to enter, up to
     32×32. Adjust with Left/Right.
   - `Y layer` — which bedrock layer the pattern is on, `-64 … -59` (Left/Right). `-60` is
     the default and the most useful — it has the most detail per cell. The screen shows
     `P(bedrock)` for the chosen layer and warns if you pick `-64` (solid) or `-59` (empty).
   - `center X` / `center Z` and `radius` — the square region to search:
     `x ∈ [X−r, X+r]`, `z ∈ [Z−r, Z+r]`.
   - `Enter` opens the pattern editor.

2. **Pattern editor.** A grid of the size you chose. Arrow keys / `hjkl` move the cursor.
   - `space` cycles a cell: unknown → **bedrock** (`#`) → **not-bedrock** (`o`) → unknown.
     (`1` / `0` / `.` set them directly.)
   - Unknown cells are wildcards — not checked.
   - `P` fills the whole grid from the actual world at your center point — handy as a
     round-trip test (search should then find that exact spot).
   - `C` clears, `S` saves the pattern to a file, `Tab` goes back to parameters.
   - `Enter` runs the search.

   On the parameters screen, `orientations` = `all 8` tries every rotation/mirror of your
   pattern (you don't have to align the screenshot to world axes); `exact` matches only as
   drawn.

3. **Results screen.** Live progress bar + rate while the job runs (`c` cancels); then
   every matching origin `(x, z)` with the orientation bitmask. `S` saves the list.

`rokktui` is a **client** — it submits the job to the search service and streams progress.
By default the service runs in-process; `--service unix:/path` points it at a `rokkd`
daemon, `--backend opencl:0` picks the GPU (in-process only). `q` quits. 

### `rokksearch` — headless search

```sh
rokksearch --pattern p.txt --center 0,0 --radius 2000000        # p.txt carries seed/y too
rokksearch --seed 12345 --y -60 --size 8x8 --region -1000000,1000000,-1000000,1000000 --backend opencl:0
rokksearch --list-backends
rokksearch --bench --pattern p.txt --center 0,0 --radius 5000000
```

Streams a progress line to stderr; prints matches (`x z orient_mask`) to stdout. `--json`
for machine output, `--checkpoint FILE` to make a long run resumable, `--service unix:...`
to use a daemon. `--help` for everything.

### `rokkd` — search daemon

```sh
rokkd --backend auto            # listens on $XDG_RUNTIME_DIR/rokkd.sock
rokksearch --service unix:$XDG_RUNTIME_DIR/rokkd.sock --pattern p.txt --center 0,0 --radius 3000000
```


One `SearchService` behind a Unix socket. Handy when the box doing the crunching isn't the
box you're driving from, or to keep a warm GPU context around across many searches.

### `dump_bedrock` — print the bedrock layer for a seed

```
dump_bedrock <seed> <x0> <z0> <width> <height> [y | all]
```

- `seed` — the world seed (decimal; may be negative)
- `x0 z0` — north-west corner of the region, in block coordinates
- `width height` — region size in blocks (`x0 .. x0+width-1`, `z0 .. z0+height-1`)
- `y` — a single layer (`-64 .. -59`); omit or pass `all` for every layer

Output is an ASCII grid: rows run along **z** (increasing = south), columns along **x**
(increasing = east), `X` = bedrock, `.` = not bedrock. A per-layer bedrock count is printed
to stderr.

The bedrock output shown in dump_bedrock can be used as a template for the p.txt input. 

```
$ build/dump_bedrock 0 0 0 32 8 -60
y=-60: 47 / 256 bedrock (0.1836)            <- stderr
# seed=0 x0=0 z0=0 w=32 h=8
# y=-60
...X....X....X.X.X......X.X..X.X
.........X...X...........X......
................X...X........XX.
XXX.........X...................
...X........XX...XX.X.........X.
XX...............X.....X......X.
...X....X....X...X.XX.XX........
......X..........X.X...X.XXX....
```

(Each grid row is 32 characters — trailing `.`s just don't stand out.)

`y = -60` is the most useful single layer to compare against a screenshot: it's the highest
level with meaningful variation, so it carries the most distinguishing detail. `y = -64` is
solid everywhere and `y = -59` is empty everywhere.

### `tests/reference/bedrock_ref.py` — same thing, in Python

An independent implementation of the identical algorithm. Same CLI as `dump_bedrock`, so
you can cross-check any region:

```sh
python3 tests/reference/bedrock_ref.py 0 0 0 32 8 -60
```

Import it as a library:

```python
from tests.reference.bedrock_ref import BedrockGenerator

gen = BedrockGenerator(seed=12345)
gen.is_bedrock_floor(x=100, y=-61, z=-40)   # -> True / False
```

### C++ library

```cpp
#include "gen/bedrock.hpp"

rokkdoxx::BedrockGenerator gen(/*world_seed=*/12345);
bool b = gen.is_bedrock_floor(100, -61, -40);
bool m = gen.floor_plane(100, -40);          // M(x, z) on the default y = -60 plane

// What the OpenCL kernel is handed: two per-seed constants + a per-plane cutoff.
uint64_t lo = gen.derived_lo(), hi = gen.derived_hi();
uint32_t t  = gen.threshold(-60);            // rk_bits24_at(lo,hi,x,-60,z) < t  == bedrock
```

Link `rokkdoxx_gen` (`src/gen/bedrock.cpp` + headers). To run a search from your own code,
link `rokksvc` and use `rokkdoxx::svc::make_client(...)` — see
[`docs/design.md`](docs/design.md).

---

## How to use it to actually find a location

1. In-game, stand on the bedrock you want to locate (Nether floor, or a shaft dug to
   bedrock). Note the layout of bedrock / non-bedrock on one layer.
2. Run `rokktui`, enter the seed, set the pattern size and Y layer, and paint what you saw.
   Mark cells you're unsure about as unknown; leave `orientations` on `all 8` so you don't
   have to align the screenshot to north. NOTE: This increases search time drastically
3. Set a search center + radius and run, or use `rokksearch` for a scripted / very large
   run. You get back every `(x, z)` where the pattern occurs — each is already an in-game
   coordinate.

On CPU this covers a region a few hundred thousand blocks across in seconds; a whole-world
sweep is the job the GPU worker exists for (build with `-DROKK_ENABLE_OPENCL=ON`). See
[Performance](#performance).

---

## Performance

Measured, `rokksearch --bench`, an 8×8 pattern. CPU = 6-core / 12-thread Ryzen 5 5600;
GPU = Radeon RX 7900 XTX (gfx1100, ROCm).

| | CPU, 12 threads | GPU | speedup |
|---|---|---|---|
| candidate origins/s, **exact** orientation | ~0.7 G | **~76 G** | ~110× |
| candidate origins/s, **all 8** orientations | ~0.09 G | **~10 G** | ~115× |

A full Overworld-border sweep (9·10¹⁴ candidate origins) is **~3.5 h** on the GPU (exact
orientation), vs. weeks on the CPU. So: the CPU is fine once you know your rough location;
the GPU makes a blind whole-world sweep practical. Per-region timings, the bit-exactness
argument, and the tile-size tuning are in [`docs/design.md`](docs/design.md).

### Why not just use Minecraft to check?

As expected, the speedup vs in-engine usage is drastically increased. 

- A vanilla client only uses JIT (Just-In-Time) generation, which limits large-scale sweeps like this program does
- Headless generation ignores all wasted computation time for in-game objects such as mobs, entity calculations, etc. 


---

## Verification

- **The generator** is checked against Java-generated RNG vectors and, byte-for-byte,
  against an independent Python reimplementation over adversarial seeds and coordinates
  (`test_bedrock`, `diff_test.py`).
- **The service** is checked against a direct `BedrockGenerator` brute force (all 8
  orientations), proven tile-size-independent, and exercised over the socket transport
  (`test_search`, `test_daemon`).
- **The GPU kernel** is diffed byte-for-byte against the CPU and checked for CPU/GPU search
  parity (`test_gpu`, needs a device).


---

## Contributing

The project has a deliberately small, fixed scope — [`docs/SCOPE.md`](docs/SCOPE.md) lists
what it does, what it explicitly will **not** do, and a one-line responsibility for every
source file. Before opening a PR:

- Read the **Non-goals** list. If your change crosses one of those lines, say so explicitly
  in the PR — it's a scope decision, not a detail.
- Keep the test suite green: `ctest --test-dir build --output-on-failure` (or
  `./build.sh test`). Behaviour changes need a test that fails without them.
- Put the four-question checklist from the end of `SCOPE.md` in the PR description.

## License

RokkDoxx is free software under the **GNU General Public License v3.0** — see
[`LICENSE`](LICENSE).
