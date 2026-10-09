#!/usr/bin/env python3
# `rokksearch --demo` must hand back a search that (a) finishes in roughly the
# time asked for and (b) finds the spot it planted, at the coordinates it
# announces. CPU backend, tiny budget, so it runs anywhere. The seed and spot are
# random each run, but the planted match always exists, so this cannot flake on
# content; the time check is loose (10x + startup) because CI boxes are noisy.
#
#   demo_test.py /path/to/rokksearch

import os
import re
import subprocess
import sys
import tempfile
import time

BUDGET = 0.5  # seconds of search asked for


def run(args):
    return subprocess.run(args, capture_output=True, text=True)


def main(exe):
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "demo.txt")
        for extra in ([], ["--orientations", "exact"], ["--y", "-62"], ["--edition", "bedrock"],
                      ["--edition", "bedrock", "--y", "-61", "--orientations", "exact"]):
            label = " ".join(extra) or "defaults"
            made = run([exe, "--demo", str(BUDGET), "--backend", "cpu", "--out", path] + extra)
            m = re.search(r"expect match: (-?\d+) (-?\d+)", made.stdout)
            if made.returncode != 0 or not m:
                print(f"FAIL [{label}]: --demo failed\n{made.stdout}{made.stderr}")
                failures += 1
                continue
            want = f"{m.group(1)} {m.group(2)}"
            if not re.search(r"^center 0 0$", open(path).read(), re.M):
                print(f"FAIL [{label}]: the region should always be centred on 0 0")
                failures += 1

            t0 = time.time()
            found = run([exe, "--pattern", path, "--backend", "cpu"])
            took = time.time() - t0
            hits = [ln.split() for ln in found.stdout.splitlines()]
            ok = any(f"{h[0]} {h[1]}" == want and int(h[2]) & 1 for h in hits if len(h) == 3)
            print(f"{'ok  ' if ok else 'FAIL'} [{label}] planted {want}, {len(hits)} match(es), {took:.2f}s")
            if not ok:
                failures += 1
            if took > BUDGET * 10 + 5:
                print(f"FAIL [{label}]: asked for ~{BUDGET}s, search took {took:.1f}s")
                failures += 1

        # A saved session (a pattern file with a `checkpoint` line, what rokktui's `s` writes)
        # resumes headless too: the second run finds the same match without searching again.
        ck, session = os.path.join(tmp, "run.ckpt"), os.path.join(tmp, "session.txt")
        made = run([exe, "--demo", str(BUDGET), "--backend", "cpu", "--out", path])
        want = re.search(r"expect match: (-?\d+ -?\d+)", made.stdout).group(1)
        first = run([exe, "--pattern", path, "--backend", "cpu", "--checkpoint", ck])
        text = open(path).read().replace("size ", f"checkpoint {ck}\nsize ", 1)
        open(session, "w").write(text)
        again = run([exe, "--pattern", session, "--backend", "cpu"])
        t1 = float(re.search(r"in ([\d.]+)s", first.stderr).group(1))
        t2 = float(re.search(r"in ([\d.]+)s", again.stderr).group(1))
        if not (want in again.stdout and t2 < 0.4 * t1):
            print(f"FAIL: session did not resume (first {t1}s, from session {t2}s)\n{again.stdout}{again.stderr[-200:]}")
            failures += 1
        else:
            print(f"ok   [session] first run {t1:.2f}s, resumed from the session {t2:.2f}s, same match")

        # Time suffixes: the same budget spelled two ways gives about the same region
        # (radius goes with sqrt(speed), so this is robust to timing noise).
        def radius(budget):
            run([exe, "--demo", budget, "--backend", "cpu", "--out", path])
            return int(re.search(r"^radius (\d+)", open(path).read(), re.M).group(1))

        radii = {b: radius(b) for b in ("36s", "0.6m", "0.01h")}  # all 36 seconds
        if not all(0.75 < r / radii["36s"] < 1.33 for r in radii.values()):
            print(f"FAIL: 36s, 0.6m and 0.01h should size the same region, got {radii}")
            failures += 1

        # "max" = the whole world (too big to search here, so check the file).
        made = run([exe, "--demo", "max", "--backend", "cpu", "--out", path])
        m = re.search(r"expect match: (-?\d+) (-?\d+)", made.stdout)
        body = open(path).read() if os.path.exists(path) else ""
        inside = m and all(abs(int(v)) <= 29_999_984 for v in m.groups())
        if made.returncode != 0 or "radius -1" not in body or not inside:
            print(f"FAIL: 'max' should be the whole world\n{made.stdout}{made.stderr}")
            failures += 1
        for bad in ("5x", "m", "-3", "0", "abc"):
            if run([exe, "--demo", bad, "--backend", "cpu", "--out", path]).returncode != 2:
                print(f"FAIL: --demo {bad!r} should be rejected")
                failures += 1

        # Layers that are all bedrock / all air can't make a distinctive pattern.
        if run([exe, "--demo", "1", "--y", "-59", "--backend", "cpu", "--out", path]).returncode != 2:
            print("FAIL: --y -59 should be rejected")
            failures += 1
        if run([exe, "--demo", "1", "--edition", "bedrock", "--y", "-63", "--backend", "cpu", "--out", path]).returncode != 2:
            print("FAIL: --y -63 should be rejected on Bedrock (it is solid there)")
            failures += 1
    return failures


if __name__ == "__main__":
    sys.exit(1 if main(sys.argv[1]) else 0)
