# ROKKDOXX: A GPU-Accelerated Mathematical Search for Bedrock Pattern Locations in Minecraft v26.2

**Version:** 1.0  
**Status:** Draft for Review  

---

## Abstract

This whitepaper presents a high-performance computing methodology for locating specific bedrock formations in Minecraft worlds without using the game engine itself. The system mathematically reproduces the game's world-generation rules for bedrock placement and performs a massively parallel search over the two‑dimensional coordinate space.

The problem is formulated as a large‑scale binary pattern‑matching task. The bedrock layer is represented as a binary matrix, and a target formation is represented as a smaller binary matrix. The search identifies all occurrences of the smaller matrix within the larger one.

Because individual candidate coordinates are independent, the search is highly amenable to GPU parallelization using OpenCL. The proposed architecture divides the world space into tiles, processed by GPU work‑items. Overlapping halo regions ensure that formations crossing tile boundaries are not missed. SIMD execution, compact binary representations, and early rejection techniques drastically reduce the computational cost compared with conventional CPU‑based methods. The resulting system can search coordinate spaces vastly larger than practical in‑game exploration while maintaining a direct mathematical relationship between discovered coordinates and their in‑game locations.

---

## 1. Introduction

Locating specific naturally generated structures in Minecraft typically requires the game to generate and load terrain for inspection. For massive search areas, this becomes computationally prohibitive. The overhead of the game engine—chunk management, terrain generation, lighting, disk I/O—dominates execution time.

This proposal rests on a key observation: **the world‑generation algorithm can be treated as a mathematical function.** There is no fundamental requirement to instantiate the world through the game engine if the relevant portion of its algorithm can be reproduced independently.

For a fixed world seed, a coordinate can be mapped to a predicted world‑generation output. For the problem at hand, this is a function that determines the presence or absence of bedrock at a given location:

`B(seed, x, z, y) -> {0, 1}`

where `seed` is the world seed, `x` and `z` are horizontal coordinates, `y` is the vertical coordinate, `1` indicates bedrock, and `0` indicates its absence. This abstraction transforms the vast Minecraft world into a discrete mathematical search space, independent of the game engine.

---

## 2. Problem Definition

The objective is to identify every coordinate `(x, z)` where a predetermined bedrock formation occurs.

Let the generated bedrock field be a binary function `M(x, z)`, where `M` represents the bedrock distribution on a specific vertical plane:

`M(x, z) = 1` if the block contains bedrock, `0` otherwise.

The target formation is a binary pattern `P` with dimensions `Wp × Hp`. A candidate location `(x, z)` is a match if and only if:

`M(x + i, z + j) = P(i, j)`

for all `0 ≤ i < Wp` and `0 ≤ j < Hp`. This reframes the task as a classic two‑dimensional pattern‑matching problem.

---

## 3. Rationale for a Non‑Game‑Engine Approach

Using the Minecraft engine introduces substantial unnecessary work. A conventional in‑game approach would require:

1. Loading a chunk.
2. Initializing chunk‑generation state.
3. Generating terrain.
4. Placing blocks.
5. Maintaining chunk data structures.
6. Handling lighting and related operations.
7. Storing the chunk.
8. Reading the relevant blocks.
9. Comparing the resulting pattern.

Most of these operations are irrelevant to the search. If the desired property depends only on a subset of the generation algorithm, the proposed system can compute that subset directly.

The ideal computational path is:

`(seed, x, z) → Mathematical Generator → Bedrock Result → Pattern Comparison`

rather than:

`(seed, x, z) → Minecraft → Chunk → Terrain → Bedrock → Pattern Comparison`

This distinction is critical when searching billions or trillions of candidate locations.

---

## 4. System Architecture Overview

The proposed system consists of two major components:

- **Host (CPU):** Manages configuration, OpenCL setup, tile scheduling, result collection, and independent verification.
- **Device (GPU):** Executes the mathematical world‑generation logic, determines bedrock presence, compares against the target pattern, and filters candidates.

The search space is divided into rectangular tiles. Each tile is processed independently and sequentially, enabling a streaming execution model where only the data for the current tile resides in memory. Halo regions around each tile ensure that patterns crossing boundaries are correctly detected.

GPU work‑items are assigned to candidate coordinates. Each work‑item evaluates a single candidate, using optimised pattern‑matching techniques to minimise per‑candidate cost.

---

## 5. Tiled Search with Halo Regions

Given the immense size of the coordinate space (e.g., `30,000,000 × 30,000,000` positions), it is infeasible to store an explicit representation of the entire world matrix. The system therefore employs a tiled search architecture.

**Tile decomposition:** The global search space is split into a grid of rectangular tiles, each containing a subset of candidate starting coordinates. For example, a tile might cover `4096 × 4096` candidate positions. The optimal tile size is determined experimentally based on GPU memory, cache behaviour, work‑group size, and generator complexity.

**Halo regions:** A target pattern can cross the boundary between two adjacent tiles. To avoid missing such formations, each tile is augmented with a surrounding *halo* of readable data. If the pattern is `Wp × Hp`, the halo must extend at least `Wp - 1` coordinates in the x‑direction and `Hp - 1` in the z‑direction beyond the tile’s owned region.

The halo is used for reading only; it does not confer search ownership. A work‑item may read halo data while evaluating a candidate, but it only reports matches for candidate origins that lie within its assigned tile. This guarantees that every possible candidate has exactly one owner, producing deterministic, duplicate‑free results.

---

## 6. GPU Parallelization with OpenCL

OpenCL is used to implement the mathematical generator and pattern matcher, enabling execution across a wide range of GPU architectures.

**Work decomposition:** Each candidate coordinate `(x, z)` is assigned to a distinct GPU work‑item. The work‑item derives its coordinate from its global ID and the tile’s origin:

`x = origin_x + (global_id % tile_width)`  
`z = origin_z + (global_id / tile_width)`

**Kernel structure:** A simplified kernel might appear as follows (in OpenCL C):

```c
__kernel void search_tile(
    ulong seed,
    long origin_x,
    long origin_z,
    __global const uint *pattern,
    __global uint *results)
{
    int x_offset = get_global_id(0);
    int z_offset = get_global_id(1);
    long x = origin_x + x_offset;
    long z = origin_z + z_offset;

    if (candidate_matches(seed, x, z, pattern))
        record_match(x, z, results);
}