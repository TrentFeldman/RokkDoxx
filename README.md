# RokkDoxx

**Got Bedrock? Get Locations.**

RokkDoxx reproduces Minecraft 26.2's Overworld bedrock-floor generation as a plain function
and searches the world for a bedrock pattern without running the game. Give it a seed and a
picture of some bedrock; it returns every `(x, z)` where that pattern occurs. A GPU
(OpenCL) sweeps the whole 60M × 60M world in hours; a CPU handles a few thousand blocks
around a rough location in seconds.

The scope is deliberately narrow: one Overworld bedrock-floor layer per pattern.

## Build - Linux

Needs a C++20 compiler (`g++` ≥ 13, `clang++` ≥ 16, or MSVC 17.8+), CMake ≥ 3.16, and
Python 3 for the tests. OpenCL is optional (Linux: `opencl-headers`, `opencl-clhpp`, plus a
runtime such as `rocm-opencl-runtime`, `pocl` or `opencl-mesa`).

```sh
git clone https://github.com/TrentFeldman/RokkDoxx && cd RokkDoxx
cmake -B build -DCMAKE_BUILD_TYPE=Release -DROKK_ENABLE_OPENCL=ON
cmake --build build
ctest --test-dir build --output-on-failure
build/rokktui
```

Without OpenCL the build still works, CPU only. No `make`/`ninja`? `./build.sh`
(`ROKK_OPENCL=1 ./build.sh` for the GPU, `./build.sh test` to run the tests; Linux/macOS).



### Build — Windows

**Prerequisites (install these first):**
- [Git for Windows](https://git-scm.com/install/windows) — Default installation.
- [CMake](https://cmake.org/download/) — Version 4.2+, Windows x64. **Add to system PATH** during installation.
- [Visual Studio Community](https://aka.ms/vs/stable/vs_community.exe) — **Select Desktop development with C++** during installation.
- Up-to-date GPU drivers (AMD, NVIDIA, or Intel).

**Restart Windows after installation.**

**Don't know what you're doing?** Copy and paste the following into Windows Command Prompt (CMD):

```bat
cd /d "%USERPROFILE%"
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg.exe install opencl:x64-windows

REBOOT RECCOMENDED

cd ..
git clone https://github.com/TrentFeldman/RokkDoxx.git
cd RokkDoxx

cmake -S . -B build -DROKK_ENABLE_OPENCL=ON "-DCMAKE_TOOLCHAIN_FILE=..\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\rokksearch.exe --list-backends
```

CMake automatically selects your installed Visual Studio version. No specific version is required.

**Build failed?** Delete the old configuration with `rmdir /s /q build` and retry.

If everything worked, the final command should display your available compute backends, including your OpenCL GPU.


`--list-backends` should show an `opencl:0 … [gpu]` row. If CMake printed `OpenCL not found
-- building CPU worker only`, the toolchain file wasn't picked up. A vendor SDK (CUDA
Toolkit, AMD HIP SDK via `-DOpenCL_ROOT=…`, Intel oneAPI) also works instead of vcpkg.

A freshly built unsigned `.exe` may trigger SmartScreen ("More info → Run anyway", or
`Unblock-File`). RokkDoxx makes no network connections and only writes files you name.

## Usage

### `rokktui` — interactive

```sh
build/rokktui [--load pattern.txt] [--backend auto|cpu|opencl:N] [--checkpoint run.ckpt]
```

1. **Parameters.** Up/Down/Tab move, type to edit, Left/Right change, `Del` clears.
   - `seed` — number or text (text is hashed like Minecraft does).
   - `width`/`height` — pattern size, up to 32×32.
   - `Y layer` — `-64 … -59`. Use `-60`: it has the most detail (P(bedrock) = 0.2).
   - `center X/Z`, `radius` — the square to search. `radius -1` = the whole world inside the
     ±29,999,984 border.
   - `orientations` — `all 8` tries every rotation/mirror, so the picture needn't face north.
   - `backend`, `checkpoint file` — device choice; optional resume file.
2. **Pattern editor** (`Enter`). Type the grid like text: `b` bedrock, `e` empty
   (not-bedrock), `.` unknown; each moves the cursor on, wrapping to the next row, and
   `Backspace` steps back. Arrows/`hjkl` move, `space` cycles a cell. Unknown cells are
   wildcards. `P` fills from the
   real world at the center (a round-trip test), `C` clears, `S` saves, `Enter` searches.
3. **Results.** Progress, rate, elapsed and ETA while running (`c` cancels), then every match
   and the orientations that fit. `S` saves them as `x z orient_mask` lines.

### `rokksearch` — headless

```sh
rokksearch --pattern p.txt                                   # region from the file
rokksearch --pattern p.txt --center 0,0 --radius 2000000 --backend opencl:0
rokksearch --pattern p.txt --region -1000000,1000000,-500000,500000 --orientations exact
```

Progress (with ETA) goes to stderr, matches (`x z orient_mask`) to stdout. `--json` for
machine output, `--checkpoint FILE` to make a run resumable (Ctrl-C, then rerun),
`--help` for the rest.

**Pattern file** (what `rokktui` saves):

```
# rokkdoxx pattern
seed 12345
y -60
center 0 0
radius 5000          # -1 = whole world
orientations all     # or exact
size 6 5
oo###o
oo#oo#
####o#
o#oo#o
#oo#o#
```

`#` bedrock, `o` not bedrock, `.` unknown (wildcard). This one is copied from the real world
at (1036, -966); `rokksearch --pattern` on it prints the single match `1038 -964 1`.

A match `(x, z)` is the world position of the pattern's *anchor*: a rare cell near its
middle, not the top-left corner.

### `dump_bedrock` — print a region

```sh
$ build/dump_bedrock <seed> <x0> <z0> <width> <height> [y|all]
$ build/dump_bedrock 0 0 0 32 4 -60
# seed=0 x0=0 z0=0 w=32 h=4
# y=-60
...X....X....X.X.X......X.X..X.X
.........X...X...........X......
................X...X........XX.
XXX.........X...................
```

Rows are z (south = down), columns x (east = right), `X` = bedrock.
`tests/reference/bedrock_ref.py` is an independent Python implementation with the same CLI.

### As a library

```cpp
#include "gen/bedrock.hpp"
rokkdoxx::BedrockGenerator gen(12345);
bool b = gen.is_bedrock_floor(100, -61, -40);
```

Link `rokkdoxx_gen`; for searches, link `rokksvc` and use `rokkdoxx::svc::make_client("auto")`.

## How it works

- Bedrock floor is the surface rule `minecraft:bedrock_floor` with a `vertical_gradient`:
  `y = -64` always bedrock, `-63 … -60` with probability 0.8 … 0.2, `≥ -59` never. It depends
  only on seed and coordinates (no biome, terrain or structures), so no game engine is needed.
- The RNG is Xoroshiro128++ positional randomness, unchanged since Java 1.18.
- Vanilla places bedrock when `(double)nextFloat() < prob`. The host precomputes
  `threshold = ceil(prob · 2²⁴)` and every device compares `bits24 < threshold`: integer-only,
  no fp64, so the GPU is bit-exact with the CPU.
- The pattern is recentred on a rare anchor cell: one test there rejects all 8 orientations
  at once, and orientations a symmetric pattern shares are collapsed.
- On the GPU, each block of a tile is generated exactly once into a 1-bit-per-block plane,
  then 32 candidates are tested at a time with word operations. Generation is the expensive
  part, so all 8 orientations cost little more than one.
- Three tiers in one process: front-ends (`rokktui`, `rokksearch`) → `SearchService`
  (tiling, scheduling, dedup, progress, cancel, checkpoints) → one `Worker` (`CpuWorker` or
  `OpenclWorker`).

## Performance

`G` = 10⁹ candidate origins per second. The standard figure is the 15-minute sustained
all-8 run; the other columns are the ~30 s quick benchmark.

| machine | backend | **sustained all-8 G** (15 min) | exact G | all-8 G | all-8 sym G | notes |
|---|---|---|---|---|---|---|
| RX 7900 XTX | opencl | **149.0** (148.6–153.4, −2.7% first→last) | 222.1 | 157.4 | 212.8 | ROCm, 48 CU |
| Ryzen 5 5600 | cpu | — | 1.42 | 0.61 | 1.43 | 12 threads, gcc 16 |

A whole-world sweep (3.6·10¹⁵ candidates) depends on your gpu. time (hours) ≈ 1000 / throughput (Gcands/s)
Dont see your gpu in the benchmarks? Estimate. Or run one! Submit a push request if you do, so I can add to the DB.


```sh
build/rokksearch --benchmark-long 15   # standard: sustained all-8, ~30 s sweeps
build/rokksearch --benchmark           # quick: exact / all-8 / all-8 symmetric
```

Both use a fixed workload (seed 0, a 6×6 pattern), so results compare across machines;
`--backend cpu` for the CPU, `--json` for a pasteable result. `--benchmark-long` also checks
that every sweep returns the identical matches (count + hash) and exits 1 if not, which
catches throttling or a device that goes wrong under heat.

## Status

| | Feature | Notes |
|:-:|---|---|
| ✅ | Overworld bedrock-floor generation | bit-exact vs Java RNG vectors + a Python reference |
| ✅ | CPU search | multi-threaded |
| ✅ | OpenCL GPU search | bit-plane kernel, bit-exact with the CPU |
| ✅ | All 8 orientations | shared anchor, symmetric patterns collapsed |
| ✅ | Whole-world search | `radius -1` |
| ✅ | Resumable runs | `--checkpoint` |
| ✅ | ETA + sustained benchmark | `--benchmark-long 15` checks results stay identical |
| ✅ | `rokktui` / `rokksearch` on Linux | |
| 🧪 | Windows (`rokktui`, `rokksearch`, GPU) | beta |
| ⬜ | Faster CPU all-8 | still ~0.4× exact; the GPU's bit-plane idea should apply |
| ⬜ | Reattach to a running search | |
| ⬜ | Multi-Y patterns | several layers in one pattern |
| ⬜ | Nether roof (`bedrock_roof`) | |

## Verification

`ctest` runs: the generator against Java-generated RNG vectors and byte-for-byte against the
Python reference (`test_bedrock`, `diff_test.py`); the search against a brute-force scan and
across tile sizes (`test_search`); GPU vs CPU bit-exactness and search parity (`test_gpu`,
needs a device); and the TUI's logic without a terminal (`test_tui`).

## License

GPL-3.0 — see [`LICENSE`](LICENSE).
