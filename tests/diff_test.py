#!/usr/bin/env python3
# Differential test: the compiled C++ `dump_bedrock` must produce byte-identical
# output to the independent Python reference for a range of seeds and regions
# (including negative coordinates and coordinates that overflow the 32-bit x
# multiply in Mth.getSeed).
#
#   diff_test.py /path/to/dump_bedrock

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, "reference", "bedrock_ref.py")

# (seed, x0, z0, width, height)
CASES = [
    (0, 0, 0, 48, 48),
    (1, 0, 0, 64, 40),
    (-1, -32, -32, 40, 40),
    (3257840388504953787, -1000, -1000, 50, 50),
    (-4000000000000, 29999900, -29999900, 24, 24),   # near +/- world border
    (42, 2147483000, -2147483000, 16, 16),           # 32-bit-overflow x multiply
    (9223372036854775807, 123, -456, 33, 17),
    (-9223372036854775808, -7, 9, 20, 20),
]


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"command failed ({p.returncode}): {' '.join(cmd)}\n{p.stderr}")
    return p.stdout


def main(argv):
    if len(argv) != 2:
        raise SystemExit("usage: diff_test.py /path/to/dump_bedrock")
    dump = argv[1]
    failures = 0
    for seed, x0, z0, w, h in CASES:
        args = [str(seed), str(x0), str(z0), str(w), str(h), "all"]
        cpp = run([dump, *args])
        py = run([sys.executable, REF, *args])
        if cpp != py:
            failures += 1
            print(f"MISMATCH seed={seed} x0={x0} z0={z0} w={w} h={h}")
            cl, pl = cpp.splitlines(), py.splitlines()
            for i in range(max(len(cl), len(pl))):
                a = cl[i] if i < len(cl) else "<none>"
                b = pl[i] if i < len(pl) else "<none>"
                if a != b:
                    print(f"  line {i}: cpp={a!r} py={b!r}")
                    break
        else:
            print(f"ok   seed={seed} region=({x0},{z0}) {w}x{h}")
    if failures:
        raise SystemExit(f"{failures} mismatch(es)")
    print("all differential cases match")


if __name__ == "__main__":
    main(sys.argv)
