#!/usr/bin/env python3
# Independent reference implementation of Minecraft 26.2 Overworld bedrock-floor
# generation, written from the algorithm spec (not ported from the C++). Used to
# differentially test src/gen. Also runnable as a CLI matching tools/dump_bedrock.
#
#   bedrock_ref.py <seed> <x0> <z0> <width> <height> [y|all]

import hashlib
import struct
import sys

MASK64 = (1 << 64) - 1

SILVER = 0x6A09E667F3BCC909   # frac(sqrt 2) * 2^64
GOLDEN = 0x9E3779B97F4A7C15   # golden ratio * 2^64  (== -7046029254386353131)

FLOOR_MIN_Y = -64
FLOOR_MAX_Y = -59

_F32_MUL = struct.unpack("f", struct.pack("f", 5.9604645e-8))[0]


def _f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def _rotl(x, k):
    x &= MASK64
    return ((x << k) | (x >> (64 - k))) & MASK64


def _mix_stafford13(z):
    z &= MASK64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


def _s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def _s64(v):
    v &= MASK64
    return v - (1 << 64) if v & (1 << 63) else v


class Xoroshiro:
    __slots__ = ("lo", "hi")

    def __init__(self, lo, hi):
        lo &= MASK64
        hi &= MASK64
        if (lo | hi) == 0:
            lo, hi = GOLDEN, SILVER
        self.lo = lo
        self.hi = hi

    @classmethod
    def from_seed(cls, seed):
        l = (seed ^ SILVER) & MASK64
        m = (l + GOLDEN) & MASK64
        return cls(_mix_stafford13(l), _mix_stafford13(m))

    def next(self):
        l = self.lo
        m = self.hi
        n = (_rotl((l + m) & MASK64, 17) + l) & MASK64
        m ^= l
        self.lo = _rotl(l, 49) ^ m ^ ((m << 21) & MASK64)
        self.lo &= MASK64
        self.hi = _rotl(m, 28)
        return n

    def next_float(self):
        # Java: (float)(next() >>> 40) * 5.9604645e-8f, evaluated in float32.
        a = _f32(float(self.next() >> 40))          # exact: value < 2^24
        return _f32(a * _F32_MUL)                    # single float32 rounding


def block_pos_seed(x, y, z):
    # Java Mth.getSeed:
    #   long l = (long)(x * 3129871) ^ (long)z * 116129781L ^ (long)y;
    #   l = l * l * 42317861L + l * 11L;
    #   return l >> 16;
    xw = _s32(x * 3129871) & MASK64                  # 32-bit wrap, sign-extend
    zw = (z * 116129781) & MASK64                    # 64-bit multiply
    yw = y & MASK64
    u = xw ^ zw ^ yw
    u = (u * u * 42317861 + u * 11) & MASK64
    return _s64(u) >> 16                             # arithmetic shift


def make_floor_factory(seed):
    world = Xoroshiro.from_seed(seed & MASK64)
    a = world.next()
    b = world.next()                                 # d0 = forkPositional()

    digest = hashlib.md5(b"minecraft:bedrock_floor").digest()
    hlo = int.from_bytes(digest[0:8], "big")
    hhi = int.from_bytes(digest[8:16], "big")

    r1 = Xoroshiro((hlo ^ a) & MASK64, (hhi ^ b) & MASK64)
    c = r1.next()
    e = r1.next()                                    # floor factory
    return c, e


class BedrockGenerator:
    def __init__(self, seed):
        self.lo, self.hi = make_floor_factory(seed)

    def is_bedrock_floor(self, x, y, z):
        if y <= FLOOR_MIN_Y:
            return True
        if y > FLOOR_MAX_Y:
            return False
        prob = 1.0 - (y - FLOOR_MIN_Y) / (FLOOR_MAX_Y - FLOOR_MIN_Y)
        h = block_pos_seed(x, y, z) & MASK64
        r = Xoroshiro((h ^ self.lo) & MASK64, self.hi)
        return float(r.next_float()) < prob


def _dump_layer(gen, y, x0, z0, w, h):
    print(f"# y={y}")
    bedrock = 0
    for dz in range(h):
        row = []
        for dx in range(w):
            b = gen.is_bedrock_floor(x0 + dx, y, z0 + dz)
            row.append("X" if b else ".")
            bedrock += b
        print("".join(row))
    frac = bedrock / (w * h)
    print(f"y={y}: {bedrock} / {w * h} bedrock ({frac:.4f})", file=sys.stderr)


def main(argv):
    if not (6 <= len(argv) <= 7):
        print("usage: bedrock_ref.py <seed> <x0> <z0> <width> <height> [y|all]",
              file=sys.stderr)
        return 2
    seed = int(argv[1])
    x0, z0, w, h = (int(argv[2]), int(argv[3]), int(argv[4]), int(argv[5]))
    gen = BedrockGenerator(seed)
    print(f"# seed={seed} x0={x0} z0={z0} w={w} h={h}")
    if len(argv) == 7 and argv[6] != "all":
        _dump_layer(gen, int(argv[6]), x0, z0, w, h)
    else:
        for y in range(FLOOR_MIN_Y, FLOOR_MAX_Y):
            _dump_layer(gen, y, x0, z0, w, h)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
