#!/usr/bin/env python3
"""
E555_ab_analyze.py -- did the exclusion list make the beam go deeper?

    python3 tests/E555_ab_analyze.py ab_out/baseline/run.log ab_out/excluded/run.log

Reads the [sweep] lines of two beamer logs and compares the DEPTH each border
configuration reached. One line per config:

    [sweep] s0_ab12_b7l0 filled=11 width=... reason=stop_row ...   -> reached 11
    [sweep] s0_ab12_b8l0 died=2    width=... reason=extinct ...    -> reached 1

so "depth" is the last row the beam actually completed. A run that sweeps more
borders is not automatically better -- excluding pieces shrinks the database and
speeds every config up -- so the headline is the RATE at which borders reach a
given depth, with a confidence interval, not the raw count.

Reported:
  * the full depth distribution for each arm
  * P(reach depth >= D) for each D, with a Wilson interval per arm and a
    Newcombe interval on the difference
  * a Mann-Whitney U test on the whole depth distribution, which uses every
    config rather than collapsing to one threshold
  * throughput, so a speed win is visible separately from a depth win
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import Counter
from pathlib import Path

# Consecutive identical deaths collapse into one line, which puts an "x<n>"
# token between the config id and the depth field:
#   [sweep] s0_ab12_b3l0-l19 x17 died=1 width=1 reason=extinct(clue_row)
# Without the optional group those lines parse as nothing and a run of barren
# borders vanishes from the denominator -- which would bias the rate upward for
# whichever arm collapsed more.
SWEEP = re.compile(r"^\[sweep\]\s+(\S+)(?:\s+x(\d+))?\s+(filled|died)=(\d+)")


def parse_log(path):
    """Depth reached per config. died=R means row R failed, so R-1 completed."""
    depths = []
    wall = 0.0
    emitted = 0
    for line in Path(path).read_text(errors="replace").splitlines():
        m = SWEEP.match(line)
        if m:
            n = int(m.group(2)) if m.group(2) else 1
            kind, val = m.group(3), int(m.group(4))
            depths.extend([val if kind == "filled" else val - 1] * n)
            continue
        if line.startswith("[done]") or "sol_total=" in line:
            mm = re.search(r"sol_total=(\d+)", line)
            if mm:
                emitted = max(emitted, int(mm.group(1)))
        mm = re.search(r"wall=([\d.]+)s", line)
        if mm:
            wall += float(mm.group(1))
    return depths, wall, emitted


def wilson(k, n, z=1.96):
    if n == 0:
        return (0.0, 0.0, 0.0)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return p, max(0.0, c - h), min(1.0, c + h)


def newcombe(k1, n1, k2, n2, z=1.96):
    """CI for p2 - p1 built from the two Wilson intervals (Newcombe method 10)."""
    p1, l1, u1 = wilson(k1, n1, z)
    p2, l2, u2 = wilson(k2, n2, z)
    d = p2 - p1
    lo = d - math.sqrt((p2 - l2) ** 2 + (u1 - p1) ** 2)
    hi = d + math.sqrt((u2 - p2) ** 2 + (p1 - l1) ** 2)
    return d, lo, hi


def mannwhitney(a, b):
    """Normal-approximation U test with tie correction. Returns (U, z, p)."""
    n1, n2 = len(a), len(b)
    if n1 == 0 or n2 == 0:
        return float("nan"), float("nan"), float("nan")
    allv = sorted(a + b)
    ranks = {}
    i = 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1] == allv[i]:
            j += 1
        ranks[allv[i]] = (i + j) / 2.0 + 1.0
        i = j + 1
    r1 = sum(ranks[v] for v in a)
    u1 = r1 - n1 * (n1 + 1) / 2.0
    mu = n1 * n2 / 2.0
    counts = Counter(allv)
    n = n1 + n2
    tie = sum(t ** 3 - t for t in counts.values())
    sd = math.sqrt(n1 * n2 / 12.0 * ((n + 1) - tie / (n * (n - 1)))) if n > 1 else 0.0
    if sd == 0:
        return u1, float("nan"), float("nan")
    z = (u1 - mu) / sd
    p = 2 * 0.5 * math.erfc(abs(z) / math.sqrt(2))
    return u1, z, p


def main(argv=None):
    ap = argparse.ArgumentParser(description="Compare two beamer runs by depth.")
    ap.add_argument("baseline_log")
    ap.add_argument("excluded_log")
    ap.add_argument("--json_out", default=None)
    ap.add_argument("--label_a", default="baseline")
    ap.add_argument("--label_b", default="excluded")
    args = ap.parse_args(argv)

    A, wa, ea = parse_log(args.baseline_log)
    B, wb, eb = parse_log(args.excluded_log)
    if not A or not B:
        sys.exit("[ab] one of the logs has no [sweep] lines")

    dmax = max(max(A), max(B))
    ca, cb = Counter(A), Counter(B)

    print(f"[ab] {args.label_a}: {len(A)} configs, mean depth {sum(A)/len(A):.3f}")
    print(f"[ab] {args.label_b}: {len(B)} configs, mean depth {sum(B)/len(B):.3f}")
    print()
    print("depth   " + f"{args.label_a:>22}   {args.label_b:>22}")
    for d in range(0, dmax + 1):
        pa = ca.get(d, 0) / len(A) * 100
        pb = cb.get(d, 0) / len(B) * 100
        if ca.get(d, 0) or cb.get(d, 0):
            print(f"  {d:>3}   {ca.get(d,0):>8} ({pa:5.1f}%)   "
                  f"{cb.get(d,0):>8} ({pb:5.1f}%)")

    print()
    print("P(reach depth >= D)")
    rows = []
    for d in range(max(1, dmax - 4), dmax + 1):
        ka = sum(1 for v in A if v >= d)
        kb = sum(1 for v in B if v >= d)
        pa, la, ua = wilson(ka, len(A))
        pb, lb, ub = wilson(kb, len(B))
        diff, dlo, dhi = newcombe(ka, len(A), kb, len(B))
        sig = "*" if (dlo > 0 or dhi < 0) else " "
        print(f"  D>={d}: {args.label_a} {pa*100:6.2f}% [{la*100:5.2f},{ua*100:5.2f}]"
              f"   {args.label_b} {pb*100:6.2f}% [{lb*100:5.2f},{ub*100:5.2f}]"
              f"   diff {diff*100:+6.2f}pp [{dlo*100:+6.2f},{dhi*100:+6.2f}] {sig}")
        rows.append({"D": d, "ka": ka, "na": len(A), "kb": kb, "nb": len(B),
                     "pa": pa, "pb": pb, "diff": diff, "lo": dlo, "hi": dhi,
                     "significant": bool(dlo > 0 or dhi < 0)})

    # The verdict comes from the deepest threshold the BASELINE still reaches
    # often enough to estimate (>= 30 configs). That is the tail we are trying to
    # fatten, and the one a practical run cares about.
    usable = [r for r in rows if r["ka"] >= 30]
    head = usable[-1] if usable else (rows[-1] if rows else None)
    u, z, p = mannwhitney(A, B)
    print()
    if head:
        D = head["D"]
        if head["lo"] > 0:
            verdict = (f"the exclusion HELPS: P(reach {D}) rises "
                       f"{head['diff']*100:+.2f}pp "
                       f"[{head['lo']*100:+.2f},{head['hi']*100:+.2f}]")
        elif head["hi"] < 0:
            verdict = (f"the exclusion HURTS: P(reach {D}) falls "
                       f"{head['diff']*100:+.2f}pp "
                       f"[{head['lo']*100:+.2f},{head['hi']*100:+.2f}]")
        else:
            verdict = (f"no effect on P(reach {D}): {head['diff']*100:+.2f}pp "
                       f"[{head['lo']*100:+.2f},{head['hi']*100:+.2f}] "
                       f"straddles zero")
    else:
        verdict = "not enough configs reached any useful depth"
    print(f"[ab] VERDICT: {verdict}")
    print(f"[ab] (secondary) Mann-Whitney over the WHOLE depth distribution: "
          f"z = {z:+.3f}, p = {p:.4g}")
    print("[ab] Note: most borders die at row 1-2 in both arms, so the rank test "
          "is dominated by\n[ab] that bulk and can disagree with the deep tail. "
          "The threshold above is the one to read.")
    print()
    print(f"[ab] throughput: {args.label_a} {len(A)} configs, "
          f"{args.label_b} {len(B)} configs "
          f"({len(B)/max(len(A),1):.2f}x)  -- a smaller database sweeps faster, "
          f"which is a separate win from depth")

    if args.json_out:
        Path(args.json_out).write_text(json.dumps({
            "label_a": args.label_a, "label_b": args.label_b,
            "n_a": len(A), "n_b": len(B),
            "mean_a": sum(A)/len(A), "mean_b": sum(B)/len(B),
            "hist_a": {str(d): ca.get(d, 0) for d in range(dmax+1)},
            "hist_b": {str(d): cb.get(d, 0) for d in range(dmax+1)},
            "thresholds": rows, "mw_z": z, "mw_p": p, "verdict": verdict,
            "headline_D": (head or {}).get("D"),
        }, indent=1))
        print(f"[out] {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
