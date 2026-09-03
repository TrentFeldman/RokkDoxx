# RokkDoxx

**Got Bedrock? Get Locations.**

RokkDoxx reproduces Minecraft's bedrock world-generation rule as a plain function and
searches the coordinate space for a target bedrock pattern without running the game
engine. Given a world seed and a picture of some bedrock, it tells you where in 
the world that pattern occurs.

The end goal is a GPU/OpenCL search over the full `30,000,000 × 30,000,000` world. The
architecture is three tiers: a thin TUI front end, a CPU scheduler that tiles the region and
validates results, and a compute worker (GPU or CPU). As a full time student, I'm keeping
this project's scope very narrow — see [Non-goals](#non-goals).

---

## Contents

- [Quick start](#quick-start)
- [What's implemented](#whats-implemented)
- [Architecture](#architecture)
- [Requirements](#requirements)
- [Build](#build)
- [Usage](#usage) — [`rokktui`](#rokktui--interactive-pattern-search),
  [`rokksearch`](#rokksearch--headless-search),
  [`dump_bedrock`](#dump_bedrock--print-the-bedrock-layer-for-a-seed), [Python](#testsreferencebedrock_refpy--same-thing-in-python), [C++ library](#c-library)
- [How to use it to actually find a location](#how-to-use-it-to-actually-find-a-location)
- [Performance](#performance)
- [Verification](#verification)
- [Non-goals](#non-goals)
- [Contributing](#contributing)
- [License](#license)

---

## Quick start

```sh
git clone https://github.com/TrentFeldman/RokkDoxx RokkDoxx && cd RokkDoxx

cmake -B build -DCMAKE_BUILD_TYPE=Release -DROKK_ENABLE_OPENCL=ON    # DROKK_ENABLE_OPENCL=ON for GPU compute
cmake --build build
ctest --test-dir build --output-on-failure

build/rokktui                             # interactive pattern search
```

On Windows, use the Visual Studio generator:

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\rokksearch.exe --benchmark
```

That first build skips OpenCL (zero dependencies). For GPU compute, a Defender
note, and how to check your OpenCL setup, see [Build → Windows](#windows-msvc).

No `make`/`ninja` and not on Windows? Use the fallback: `./build.sh` (or
`ROKK_OPENCL=1 ./build.sh`), then `./build.sh test`.

---

## What's implemented

| Component | State |
|---|---|
| Minecraft 26.2 Overworld bedrock-floor generation (`B(seed, x, y, z)`) | ✅ verified |
| Shared C/OpenCL generation core (`bedrock_core.h`) | ✅ |
| Search — tiling, scheduling, dedup, progress, cancel | ✅ |
| `rokktui` (interactive) / `rokksearch` (headless) | ✅ |
| CPU worker (multi-threaded) | ✅ |
| OpenCL worker — all 8 orientations, bit-exact with CPU | ✅ |
| Windows: `rokksearch` + `dump_bedrock` + tests, CPU + GPU | 🔨 builds on MSVC (CPU path user-confirmed); GPU-on-Windows untested but supported |
| Windows: `rokktui` | ⬜ later (needs a console backend) |
| Resumable long runs (`--checkpoint`) | ⬜ revisit |
| Local-memory tile cache, async tile pipelining | ⬜ later |
| Nether roof (`bedrock_roof`), multi-Y patterns | ⬜ later |
| `rokktui` redo | ⬜ later |
| Further optimizations, early test rejections | ⬜ later |
| Work on optimizing 8 direction search slowdowns | ⬜ later |
| Reattach to a running / detached search | ⬜ later |

How the generation works, short version:

- Bedrock floor is a **surface rule** (`minecraft:bedrock_floor`) with a `vertical_gradient`
  condition. It depends only on the seed and block coordinates — not biome, terrain, or
  structures — which is what makes an engine-free search possible.
- `y = -64` is always bedrock; `y ≥ -59` never is; `y = -63..-60` fade out linearly
  (probability 0.8, 0.6, 0.4, 0.2).
- The RNG is **Xoroshiro128++** (`XoroshiroRandomSource`), unchanged since Java 1.18.
- The GPU search is **bit-exact** with the CPU. Vanilla places bedrock when
  `(double)nextFloat() < prob`; the host precomputes `threshold = ceil(prob · 2²⁴)` once and
  the kernel does the integer compare `bits24 < threshold` instead. No `fp64`, no rounding
  modes — identical results on any OpenCL device.

---

## Architecture

Three tiers, so the compute scales independently of the UI:

- **Front-ends** (`rokktui`, `rokksearch`) — thin. They build a search request and poll for
  progress; no search logic of their own.
- **`SearchService`** (CPU) — cuts the region into tiles, schedules them, deduplicates
  matches (a symmetric pattern can hit under several orientations at one origin), tracks
  progress, and supports cancel.
- **`Worker`** — one compute device: `CpuWorker` (multi-threaded) or `OpenclWorker`.

It all runs in one process — there is no daemon and no IPC.

---

## Requirements

- A C++20 compiler:
  - Linux / macOS: `g++` ≥ 13 or `clang++` ≥ 16
  - Windows: MSVC (Visual Studio 2022 / Build Tools ≥ 17.8), or MinGW-w64 with a
    `std::thread`-capable runtime (posix thread model, or GCC ≥ 13)
- **Primary build:** CMake ≥ 3.16 + a generator (`make`, `ninja`, or Visual Studio)
- **Fallback build (Linux / macOS only):** just `bash` — [`build.sh`](build.sh)
  compiles everything with one `g++` invocation per target, no build system needed
- Python 3 — for the reference implementation and the differential test
- *Optional, for the GPU worker:* OpenCL headers + an ICD loader, and one OpenCL
  platform. Linux: `opencl-headers`, `opencl-clhpp`, plus `rocm-opencl-runtime`
  (AMD), `pocl` (CPU OpenCL), or `opencl-mesa` (Rusticl). Windows: see below.

---

## Build

### CMake (preferred)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DROKK_ENABLE_OPENCL=ON    # -DROKK_ENABLE_OPENCL=ON for GPU compute
cmake --build build
ctest --test-dir build --output-on-failure
```

Produces `build/{dump_bedrock, rokksearch, test_*}` (plus `rokktui` on
Linux/macOS — it uses a POSIX raw-terminal backend and is not built on Windows).

### Windows (MSVC)

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

build\Release\rokksearch.exe --benchmark        :: add an OpenCL SDK for GPU compute, see below
```

The Visual Studio generator is multi-config, so pass `--config Release` at build
time and `-C Release` to `ctest`. The command above **skips OpenCL**, so the
build has zero external dependencies — a quick way to confirm the toolchain
works. The GPU path is fully supported on Windows and produces the same
bit-exact results as Linux; it just needs the OpenCL SDK below at build time.

**OpenCL on Windows.** Two separate pieces:

- **At runtime** — `OpenCL.dll` (the ICD loader) ships with Windows and every
  modern GPU driver (AMD Adrenalin, NVIDIA, Intel). Nothing to install.
- **At build time** — you need the OpenCL **headers** + an `OpenCL.lib` import
  stub, which MSVC does *not* bundle. Easiest path:

```bat
vcpkg install opencl
cmake -B build -DROKK_ENABLE_OPENCL=ON ^
  -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Alternatives: a [Khronos OpenCL-SDK](https://github.com/KhronosGroup/OpenCL-SDK)
release (`-DOpenCL_ROOT=C:/path/to/OpenCL-SDK`), or an installed CUDA Toolkit
(sets `CUDA_PATH`, which CMake's `find_package(OpenCL)` checks).

**Check it worked:**

1. *Did CMake find the SDK?* The configure step prints one of:
   ```
   -- OpenCL found: C:/.../OpenCL.lib
   -- OpenCL not found -- building CPU worker only
   ```
   Re-run `cmake -B build -DROKK_ENABLE_OPENCL=ON ...` and read that line.
2. *Is a GPU visible at runtime?* After building:
   ```bat
   build\Release\rokksearch.exe --list-backends
   ```
   Expect an `opencl:0  <device name>  [gpu]` row next to `cpu`. If only `cpu`
   appears, the GPU driver / ICD isn't being seen — update your GPU driver.
3. *`OpenCL.dll` present?* `where OpenCL.dll` should hit `C:\Windows\System32`.
   Vendor ICDs register under `HKLM\SOFTWARE\Khronos\OpenCL\Vendors` (a current
   GPU driver installs one). For a full device dump, `clinfo` (Khronos) or
   GPU-Z also work.
4. *End to end:* `build\Release\rokksearch.exe --benchmark` (no `--backend`)
   auto-selects the GPU when present — the `device :` header line names it.

> **Windows Defender / SmartScreen.** A freshly built, **unsigned** `.exe` —
> especially one that pins every CPU core and drives the GPU — commonly trips a
> SmartScreen "Windows protected your PC" prompt or a Defender heuristic. This is
> expected for any small tool without a paid code-signing certificate; it is not
> a detection of anything. RokkDoxx makes **no network connections** and writes
> only the files you name on the command line — if you built it from this
> source, trust the binary as much as you trust the source. To proceed: click
> **More info → Run anyway**, or in PowerShell
> `Unblock-File .\build\Release\rokksearch.exe`.

### Fallback (`build.sh`, no build system, not reccomended, Linux / macOS only)

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
   -center X / center Z and radius — defines the square area to search around the center point. 
    The radius controls how far the search extends in each direction.
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

The search runs in this process. `--backend opencl:0` picks the GPU (`auto` is the
default and prefers a GPU if present). `q` quits.

### `rokksearch` — headless search

```sh
rokksearch --pattern p.txt --center 0,0 --radius 2000000        # p.txt carries seed/y too
rokksearch --seed 12345 --y -60 --size 8x8 --region -1000000,1000000,-1000000,1000000 --backend opencl:0
rokksearch --list-backends
rokksearch --benchmark                                          # standard reproducible benchmark
rokksearch --bench --pattern p.txt --center 0,0 --radius 5000000  # rate of *this* search
```

Streams a progress line to stderr; prints matches (`x z orient_mask`) to stdout. `--json`
for machine output, `--checkpoint FILE` to make a long run resumable (Ctrl-C, then re-run
the same command). `--help` for everything.

`--benchmark` runs a **fixed** workload (seed 0, a 6×6 pattern, region auto-sized per run)
so numbers are comparable across machines — warm-up + 5 timed iterations, reported as a
median with min/max. `--backend cpu` for the CPU figure; `--json` for a pasteable result;
`--benchmark-seconds` / `--benchmark-iters` to trade run time for stability. See
[Performance](#performance).

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
link `rokksvc` and use `rokkdoxx::svc::make_client(...)`.

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

On CPU this covers a region a few thousand blocks<sup>2</sup>  in seconds; a whole-world
sweep is the job the GPU worker exists for (build with `-DROKK_ENABLE_OPENCL=ON`). See
[Performance](#performance).

---

## Performance

**`G` = 10⁹** (one billion) candidate origins checked per second.

| | CPU (6c/12t Ryzen 5 5600) | GPU (RX 7900 XTX) | speedup |
|---|---|---|---|
| **exact** orientation | ~0.8 G | **~76 G** | ~95× |
| **all 8** orientations | ~0.14 G | **~10.5 G** | ~75× |

A full Overworld-border sweep (9·10¹⁴ candidate origins) is **~3.5 h** on the GPU (exact
orientation), vs. weeks on the CPU. So: the CPU is fine once you know your rough location;
the GPU makes a blind whole-world sweep practical.

### Benchmark it yourself

```sh
build/rokksearch --benchmark              # GPU (or CPU if no GPU / --backend cpu)
build/rokksearch --backend cpu --benchmark
```

Fixed workload, auto-sized region, warm-up + 5 timed iterations. Takes ~30 s. This is where
the table above comes from. 

| machine | backend | exact G | all-8 G | notes | date |
|---|---|---|---|---|---|
| RX 7900 XTX (gfx1100) | opencl | 76 | 10.5 | ROCm driver 3581, 48 CU | 2026-08 |
| Ryzen 5 5600 | cpu | 0.80 | 0.14 | 12 threads, gcc | 2026-08 |

### Why not just use Minecraft to check?

As expected, the speedup vs in-engine usage is drastically increased. 

- A vanilla client only uses JIT (Just-In-Time) generation, which limits large-scale sweeps like this program does
- Headless generation ignores all wasted computation time for in-game objects such as mobs, entity calculations, etc. 


---

## Verification

- The generator itself is checked against Java-generated RNG vectors and, byte-for-byte,
  against an independent Python reimplementation over adversarial seeds and coordinates
  (`test_bedrock`, `diff_test.py`).
- **The search** is checked against a direct `BedrockGenerator` brute force (all 8
  orientations) and proven independent of tile size (`test_search`).
- **The GPU kernel** is diffed byte-for-byte against the CPU and checked for CPU/GPU search
  parity (`test_gpu`, needs a device).


---

## License

RokkDoxx is free software under the **GNU General Public License v3.0** — see
[`LICENSE`](LICENSE).
