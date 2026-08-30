# Minecraft 26.2 — Overworld bedrock-floor generation

This is the world-generation logic RokkDoxx reproduces: the function

```
B(seed, x, y, z) -> {bedrock, not bedrock}
```

for the Overworld bedrock floor. It depends **only** on the world seed and the block
coordinates — not on biome, terrain noise, structures, or chunk state — which is what makes
the whitepaper's engine-free pattern search possible.

Reference C++ implementation: [`src/gen/`](../src/gen). Independent Python cross-check:
[`tests/reference/bedrock_ref.py`](../tests/reference/bedrock_ref.py).

## Where it lives in the game

Bedrock is placed by a **surface rule**, not a feature or carver. In the Overworld noise
settings (`minecraft:overworld`):

```
SurfaceRules.ifTrue(
    SurfaceRules.verticalGradient("minecraft:bedrock_floor",
        VerticalAnchor.bottom(),          // true_at_and_below  = -64
        VerticalAnchor.aboveBottom(5)),   // false_at_and_above = -59
    BEDROCK)
```

`VerticalGradientConditionSource` evaluates, for a column position `(x, y, z)`:

```
factory = randomState.getOrCreateRandomFactory(ResourceLocation("minecraft:bedrock_floor"))
rand    = factory.at(x, y, z)
P(bedrock) = Mth.map(y, -64, -59, 1.0, 0.0) = 1 - (y + 64) / 5
place bedrock  iff  rand.nextFloat() < P(bedrock)
```

So: `y = -64` is always bedrock, `y >= -59` is never bedrock, and `-63..-60` fade out
linearly (P = 0.8, 0.6, 0.4, 0.2).

## The RNG

The Overworld uses `legacy_random_source: false` → **Xoroshiro128++**
(`XoroshiroRandomSource` / `RandomSupport`). Unchanged since Java 1.18; 26.1 and 26.2 touch
`noise_gradient` / `noise_threshold` surface rules only, not `vertical_gradient` and not the
RNG.

### Primitives (see [`xoroshiro128pp.hpp`](../src/gen/xoroshiro128pp.hpp))

| Step | Definition |
|---|---|
| `next()` | `n = rotl(lo+hi,17)+lo; m = hi^lo; lo = rotl(lo,49)^m^(m<<21); hi = rotl(m,28); return n` |
| `nextFloat()` | `(next() >>> 40) * 2^-24` (one `next()` call) |
| seed upgrade | `l = seed ^ 0x6A09E667F3BCC909; m = l + 0x9E3779B97F4A7C15; state = (mixStafford13(l), mixStafford13(m))` |
| all-zero fallback | if both halves are 0 → `(0x9E3779B97F4A7C15, 0x6A09E667F3BCC909)` |
| `mixStafford13(z)` | `z=(z^z>>>30)*0xBF58476D1CE4E5B9; z=(z^z>>>27)*0x94D049BB133111EB; z^z>>>31` |

### Positional factory (see [`positional_random.hpp`](../src/gen/positional_random.hpp))

- `forkPositional()` consumes **two** outputs of the source → `(seedLo, seedHi)`.
- `fromHashOf(id)` = `MD5(id)` split into two big-endian `u64`, XORed into the forked state
  (raw constructor, no re-upgrade). `MD5("minecraft:bedrock_floor") =
  bbf7928b7bf1d285c4dc7cf90e1b3b94`.
- `at(x, y, z)` = raw `Xoroshiro(getSeed(x,y,z) ^ seedLo, seedHi)`.
- `getSeed(x, y, z)` (`Mth.getSeed`):
  ```
  l = (long)(x * 3129871)  ^  (long)z * 116129781L  ^  (long)y   // x-mul wraps in 32 bits
  l = l*l*42317861L + l*11L                                       // 64-bit wrapping
  return l >> 16                                                  // arithmetic shift
  ```

### Per-seed setup

```
world  = Xoroshiro.fromSeed(worldSeed)
d0     = world.forkPositional()
r1     = Xoroshiro.raw( MD5("minecraft:bedrock_floor")  XOR  d0 )
floor  = r1.forkPositional()      // <-- (derived_lo, derived_hi): the only per-seed
                                  //     state the OpenCL kernel needs
```

## Verification status

- **RNG**: matches Java-generated test vectors (seed upgrade, `nextLong`, `nextFloat`) from
  `Xevion/seedcrack-portal` (MC 1.21.4).
- **Full generator**: C++ and the independent Python reference agree byte-for-byte across
  seeds and regions, including negative coordinates and the 32-bit `getSeed` overflow
  (`tests/diff_test.py`).
- **Cross-checked** against three independent community implementations:
  `Developer-Mike/minecraft-bedrock-generator` (Java), `silversquirl/bedrock-finder-118`
  (Zig), `Xevion/seedcrack-portal` (Rust).
- **Still to do**: a gold check against the actual 26.2 client for one known seed +
  bedrock screenshot (needs a JRE / the game — not available in this environment). Also
  re-confirm `minecraft:overworld` still carries `legacy_random_source: false` in the 26.2
  jar; if Mojang ever flips it, the legacy-LCG path
  (`JorianWoltjer/BedrockFinder::LegacyGenerator`) is the fallback.

## Out of scope here

This document covers only the generation function `B(seed, x, y, z)`. The search
that calls it — tiling, the GPU kernel, the service — is in
[`design.md`](design.md). Things deliberately not built yet (Nether roof,
volumetric patterns, ...) are listed in [`SCOPE.md`](SCOPE.md).
