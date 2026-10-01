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

REBOOT RECOMMENDED

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


`--list-backends` should show an `opencl:N … [dedicated gpu]` (or `[integrated gpu]`) row. If CMake printed `OpenCL not found
-- building CPU worker only`, the toolchain file wasn't picked up. A vendor SDK (CUDA
Toolkit, AMD HIP SDK via `-DOpenCL_ROOT=…`, Intel oneAPI) also works instead of vcpkg.

A freshly built unsigned `.exe` may trigger SmartScreen ("More info → Run anyway", or
`Unblock-File`). RokkDoxx makes no network connections and only writes files you name
(saving a session also writes `<name>.ckpt` beside it).

**Reading the Linux commands below on Windows.** Run them in CMD from the `RokkDoxx` folder
(File Explorer → click the address bar → type `cmd` → Enter), with these swaps:

| README says | On Windows |
|---|---|
| `build/rokktui`, `rokksearch …`, `build/dump_bedrock …` | `build\Release\rokktui.exe`, `build\Release\rokksearch.exe …`, `build\Release\dump_bedrock.exe …` (`/` in paths becomes `\`) |
| `cmake --build build`, `ctest --test-dir build` | add `--config Release` / `-C Release`, as in the steps above |
| `./build.sh` | not available — use CMake |
| `# …` after a command, a leading `$ ` | don't type them |
| `ls`, `cat f`, `cp a b`, `rm -rf d` | `dir`, `type f`, `copy a b`, `rmdir /s /q d` |

### Build — macOS

# !!!UNSUPPORTED!!! 

Apple deprecated OpenCL in 2018 (frozen at 1.2) and can remove it in any macOS update, and the kernel leans on
64-bit integer math that Apple GPUs may handle slowly. It may not build, may crash, or may
quietly give wrong answers — the `gpu` test below is the check. No support is promised. If the GPU
path fails, `--backend cpu` still works. Use at your own risk. This section is more of a "What-If" than a 
supported feature. 

**Before you start** — macOS ships none of this except Terminal (Spotlight → "Terminal"). Run
these in it, Intel or Apple Silicon:

1. **Command Line Tools** (compiler, `git`, Python 3; a dialog opens, takes a few minutes):
   ```sh
   xcode-select --install
   ```
2. **Homebrew**, the package manager — *not* included with macOS:
   ```sh
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```
   On Apple Silicon it ends by printing two commands (`echo … >> ~/.zprofile`, then
   `eval "$(/opt/homebrew/bin/brew shellenv)"`): run them, then quit and reopen Terminal.
   `brew --version` should now work.

Then:

```sh
brew install cmake opencl-clhpp-headers
git clone https://github.com/TrentFeldman/RokkDoxx && cd RokkDoxx
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/rokksearch --list-backends             # want an `opencl:N … gpu` row
ctest --test-dir build --output-on-failure   # the `gpu` test must pass before trusting the GPU
```

Without `opencl-clhpp-headers` CMake says so and builds CPU only. `./build.sh` also works
(`ROKK_OPENCL=1` for the GPU).

## Usage

### `rokktui` — interactive

```sh
build/rokktui [--load pattern.txt | --resume session.txt] [--backend auto|cpu|dedicated|integrated|opencl:N] [--checkpoint run.ckpt]
```

1. **Parameters.** Up/Down/Tab move, type to edit, Left/Right change, `Del` clears.
   - `seed` — number or text (text is hashed like Minecraft does).
   - `width`/`height` — pattern size, up to 32×32.
   - `Y layer` — `-64 … -59`. Use `-60`: it has the most detail (P(bedrock) = 0.2).
   - `center X/Z`, `radius` — the square to search. `radius -1` = the whole world inside the
     ±29,999,984 border.
   - `orientations` — `all 8` tries every rotation/mirror, so the picture needn't face north.
   - `stop at first` — `yes` ends the search at the first match. It scans outward from the
     region's center (spawn, by default), so a nearby build is found in seconds.
   - `backend`, `checkpoint file` — device choice; optional progress file.
2. **Pattern editor** (`Enter`). Type the grid like text: `b` bedrock, `e` empty
   (not-bedrock), `.` unknown; each moves the cursor on, wrapping to the next row, and
   `Backspace` steps back. Arrows/`hjkl` move, `space` cycles a cell. Unknown cells are
   wildcards. `P` fills from the real world at the center (a round-trip test), `C` clears,
   `S` saves, `Enter` searches.
3. **Search.** The region is drawn as a grid of `#` (the caption gives the blocks each covers;
   north-west is top left), searched in a spiral from the center. Grey = not yet · **yellow**
   (pulsing) = searching now · blue = partly done · cyan = done · **green** = holds a match
   (flashes when found). The map flashes magenta when the search ends; windows under 18 rows
   get a plain progress bar. Keys: `p` pause/resume · `c` cancel · `s` save session.
4. **Results.** Matches and the orientations that fit. `S` saves them as `x z orient_mask`
   lines, `r` continues a cancelled or stopped search, `m` brings the map back.

**Save and resume.** `s` writes `NAME` (the pattern, its settings and a `checkpoint` line) and
`NAME.ckpt` (finished tiles and matches so far). Quit whenever; later:

```sh
rokktui --resume NAME       # or headless: rokksearch --pattern NAME
```

Saves are as of the last few seconds; pause first for an exact one. A resumed session keeps
updating its `.ckpt`.

### `rokksearch` — headless

```sh
rokksearch --pattern p.txt                                   # region from the file
rokksearch --pattern p.txt --center 0,0 --radius 2000000 --backend opencl:0
rokksearch --pattern p.txt --region -1000000,1000000,-500000,500000 --orientations exact
```

Progress (with ETA) goes to stderr, matches (`x z orient_mask`) to stdout. `--json` for
machine output, `--checkpoint FILE` to make a run resumable (Ctrl-C, then rerun),
`--first` to stop at the first match, `--help` for the rest.

The search spirals out from the region's center (spawn, for `--radius -1`). With `--first`, a
target 14k blocks from spawn takes ~0.3 s and one 3M out ~3 min, against ~6.7 h for the whole
world.

**Try it without Minecraft.** `--demo` writes a pattern file to search for:

```sh
rokksearch --demo 5m                   # random seed and spot, sized to take ~5 min here
rokksearch --pattern demo_pattern.txt  # finds the spot it printed as "expect match"
```

Time is `90`, `90s`, `5m`, `2h`, or `max` (the whole world). Also `--out`, `--seed`, `--y -63..-60`,
`--backend`, `--orientations exact`. The pattern is copied from the real world there and grown
until a second match is unlikely.

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
middle, not the top-left corner. A saved session adds `checkpoint <file>` (and
`stop_at_first yes`) before `size`.

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
  `OpenclWorker`). The service scans in a spiral from the region's center, can pause, keeps its
  progress as checkpoint text, and reports a per-area state map that `rokktui` draws.

## Performance

`G` = 10⁹ candidate origins per second. The standard figure is the 15-minute sustained
all-8 run; the other columns are the ~30 s quick benchmark.

| machine | backend | **sustained all-8 G** (15 min) | exact G | all-8 G | all-8 sym G | notes |
|---|---|---|---|---|---|---|
| RX 7900 XTX | opencl | **149.0** (148.6–153.4, −2.7% first→last) | 222.1 | 157.4 | 212.8 | ROCm, 48 CU |
| Ryzen 5 5600 | cpu | — | 1.42 | 0.61 | 1.43 | 12 threads, gcc 16 |
| RTX 4060 Laptop GPU | opencl | - | 97.05 | 70.79 | 89.41 | CUDA, 24 CU |
| Intel Ultra 9 185H | cpu | — | 0.97 | 0.45 | 1.10 | 22 threads, msvc 1951 |	
| Intel Arc Graphics(9 185H) | opencl | - | 33.70 | 26.91 | 31.76 | NEO, 128 CU |

A whole-world sweep (3.6·10¹⁵ candidates) depends on your gpu. time (hours) ≈ 1000 / throughput (Gcands/s)
Dont see your gpu in the benchmarks? Estimate. Or run one! Submit a push request if you do, so I can add to the DB.


```sh
build/rokksearch --benchmark-long 15   # standard: sustained all-8, ~30 s sweeps
build/rokksearch --benchmark           # quick: exact / all-8 / all-8 symmetric
```

Both use a fixed workload (seed 0, a 6×6 pattern), so results compare across machines;
`--backend cpu` for the CPU (or `dedicated` / `integrated` to pick a GPU by kind, `opencl:N` by
index; plain `auto` prefers dedicated, then integrated, then cpu), `--json` for a pasteable result. `--benchmark-long` also checks
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
| ✅ | Outward search, stop at first match | nearest-to-spawn first; `--first` |
| ✅ | Resumable runs | `--checkpoint`, or save/resume sessions |
| ✅ | Live map, pause/resume (`rokktui`) | `p`, `s`, `r`; `rokktui --resume` |
| ✅ | Demo searches | `rokksearch --demo` |
| ✅ | ETA + sustained benchmark | `--benchmark-long 15` checks results stay identical |
| ✅ | `rokktui` / `rokksearch` on Linux | |
| 🧪 | Windows (`rokktui`, `rokksearch`, GPU) | beta |
| ⛔ | macOS (OpenCL) | **unsupported** — never run on a Mac |
| ⬜ | Faster CPU all-8 | still ~0.4× exact; the GPU's bit-plane idea should apply |
| ⬜ | Reattach to a running search | |
| ⬜ | Multi-Y patterns | several layers in one pattern |
| ⬜ | Nether roof (`bedrock_roof`) | |

## Verification

`ctest` runs: the generator against Java-generated RNG vectors and byte-for-byte against the
Python reference (`test_bedrock`, `diff_test.py`); the search against a brute-force scan, across
tile sizes, scan order, pause/resume and checkpoints (`test_search`); GPU vs CPU bit-exactness
and search parity (`test_gpu`, needs a device); the TUI without a terminal, including
pause/save/resume (`test_tui`); and `--demo` plus session resume end to end (`demo_test.py`).

## License

GPL-3.0 — see [`LICENSE`](LICENSE).
