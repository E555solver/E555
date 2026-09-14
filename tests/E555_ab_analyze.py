#!/usr/bin/env python3
"""
E555_ab_analyze.py -- did the exclusion list make the search healthier?

    python3 tests/E555_ab_analyze.py ab_out/baseline/run.log ab_out/excluded/run.log

WHAT TO MEASURE, AND WHY NOT EMISSIONS

    On a small machine, with all the clues on, a run at --stop_row 12 emits
    essentially nothing: the beam dies before it gets there on all but a freak
    border. Counting emitted boards would compare zero against zero.

    So both arms run with --verbose, and the comparison uses what the beam
    reports on the way up. Each row prints

        [beam] <config> row=R cands=N uniq=U beam=B/K smax=S t=Ts

    and `uniq` -- distinct surviving states after the dedup -- is the useful one.
    It is continuous, it is recorded for every config at every row it reaches,
    and it is what "the search still has room" actually means. A binary
    did-it-reach-row-12 throws all of that away and then has almost no events
    left to count.

    Two things are reported per row, and they answer different questions:

      survival   what fraction of borders got to row R at all. Wilson interval
                 per arm, Newcombe on the difference.
      breadth    among the borders that got there, how wide the frontier was.
                 Median `uniq`, with a bootstrap interval on the RATIO between
                 arms, plus Mann-Whitney. This is the sensitive one.

    The headline is breadth at the deepest row where both arms still have enough
    configs to estimate it.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Consecutive identical deaths collapse into one line, which puts an "x<n>"
# token between the config id and the depth field:
#   [sweep] s0_ab12_b3l0-l19 x17 died=1 width=1 reason=extinct(clue_row)
# Without the optional group those lines parse as nothing and a run of barren
# borders vanishes from the denominator -- which would bias the rate upward for
# whichever arm collapsed more.
SWEEP = re.compile(r"^\[sweep\]\s+(\S+)(?:\s+x(\d+))?\s+(filled|died)=(\d+)")
BEAM = re.compile(r"^\[beam\]\s+(\S+)\s+row=(\d+)\s+cands=(\d+)\s+uniq=(\d+)")

MIN_CONFIGS = 30          # below this a per-row estimate is not worth reporting


def parse_log(path):
    """(depths per config, uniq[row] -> list of widths, n_configs)."""
    depths = []
    uniq = defaultdict(list)
    for line in Path(path).read_text(errors="replace").splitlines():
        m = SWEEP.match(line)
        if m:
            n = int(m.group(2)) if m.group(2) else 1
            kind, val = m.group(3), int(m.group(4))
            depths.extend([val if kind == "filled" else val - 1] * n)
            continue
        m = BEAM.match(line)
        if m:
            uniq[int(m.group(2))].append(int(m.group(4)))
    return depths, uniq


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
    return (d,
            d - math.sqrt((p2 - l2) ** 2 + (u1 - p1) ** 2),
            d + math.sqrt((u2 - p2) ** 2 + (p1 - l1) ** 2))


def median(v):
    if not v:
        return float("nan")
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def ratio_ci(a, b, n_boot=4000, seed=7):
    """Bootstrap CI for median(b)/median(a), resampling configs in each arm."""
    if not a or not b:
        return float("nan"), float("nan"), float("nan")
    import random
    rng = random.Random(seed)
    ma, mb = median(a), median(b)
    if ma <= 0:
        return float("nan"), float("nan"), float("nan")
    out = []
    for _ in range(n_boot):
        ra = median([a[rng.randrange(len(a))] for _ in range(len(a))])
        rb = median([b[rng.randrange(len(b))] for _ in range(len(b))])
        if ra > 0:
            out.append(rb / ra)
    out.sort()
    if not out:
        return mb / ma, float("nan"), float("nan")
    return mb / ma, out[int(.025 * len(out))], out[int(.975 * len(out))]


def mannwhitney(a, b):
    """Normal-approximation U test with tie correction. Returns (U, z, p)."""
    n1, n2 = len(a), len(b)
    if n1 == 0 or n2 == 0:
        return float("nan"), float("nan"), float("nan")
    allv = sorted(a + b)
    ranks, i = {}, 0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1] == allv[i]:
            j += 1
        ranks[allv[i]] = (i + j) / 2.0 + 1.0
        i = j + 1
    u1 = sum(ranks[v] for v in a) - n1 * (n1 + 1) / 2.0
    mu = n1 * n2 / 2.0
    counts = Counter(allv)
    n = n1 + n2
    tie = sum(t ** 3 - t for t in counts.values())
    sd = math.sqrt(n1 * n2 / 12.0 * ((n + 1) - tie / (n * (n - 1)))) if n > 1 else 0.0
    if sd == 0:
        return u1, float("nan"), float("nan")
    z = (u1 - mu) / sd
    return u1, z, math.erfc(abs(z) / math.sqrt(2))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Compare two beamer runs.")
    ap.add_argument("baseline_log")
    ap.add_argument("excluded_log")
    ap.add_argument("--json_out", default=None)
    ap.add_argument("--label_a", default="baseline")
    ap.add_argument("--label_b", default="excluded")
    ap.add_argument("--n_boot", type=int, default=4000)
    args = ap.parse_args(argv)

    A, UA = parse_log(args.baseline_log)
    B, UB = parse_log(args.excluded_log)
    if not A or not B:
        sys.exit("[ab] one of the logs has no [sweep] lines")
    la, lb = args.label_a, args.label_b
    na, nb = len(A), len(B)

    print(f"[ab] {la}: {na:,} borders, mean depth {sum(A)/na:.3f}")
    print(f"[ab] {lb}: {nb:,} borders, mean depth {sum(B)/nb:.3f}")
    if not UA and not UB:
        print("[ab] WARNING: no [beam] lines -- the runs were not --verbose, so "
              "only survival can be compared", file=sys.stderr)

    dmax = max(max(A), max(B), max(UA or [0]), max(UB or [0]))
    rows = []
    print()
    print(f"{'row':>4}  {'survival ' + la:>22}  {'survival ' + lb:>22}  "
          f"{'diff':>9}   {'median uniq':>21}  {'ratio':>18}  {'MW p':>8}")
    for r in range(1, dmax + 1):
        ka = sum(1 for v in A if v >= r)
        kb = sum(1 for v in B if v >= r)
        if ka == 0 and kb == 0:
            continue
        pa, la_, ua_ = wilson(ka, na)
        pb, lb_, ub_ = wilson(kb, nb)
        d, dlo, dhi = newcombe(ka, na, kb, nb)
        ua, ub = UA.get(r, []), UB.get(r, [])
        enough = len(ua) >= MIN_CONFIGS and len(ub) >= MIN_CONFIGS
        rat, rlo, rhi = ratio_ci(ua, ub, args.n_boot) if enough else (float("nan"),) * 3
        _, _, p = mannwhitney(ua, ub) if enough else (0, 0, float("nan"))
        sig = "*" if (dlo > 0 or dhi < 0) else " "
        rsig = "*" if enough and (rlo > 1 or rhi < 1) else " "
        med_a = median(ua) if ua else float("nan")
        med_b = median(ub) if ub else float("nan")
        cell_med = (f"{med_a:>9,.0f} {med_b:>9,.0f}" if ua and ub
                    else f"{'-':>9} {'-':>9}")
        cell_rat = (f"{rat:6.3f}[{rlo:5.2f},{rhi:5.2f}]{rsig}" if enough
                    else f"{'(too few)':>18}")
        cell_p = f"{p:>8.3g}" if enough else f"{'-':>8}"
        print(f"{r:>4}  {ka:>7,} {pa*100:6.2f}% [{la_*100:5.2f},{ua_*100:5.2f}]  "
              f"{kb:>7,} {pb*100:6.2f}% [{lb_*100:5.2f},{ub_*100:5.2f}]  "
              f"{d*100:+6.2f}pp{sig}  {cell_med}  {cell_rat}  {cell_p}")
        rows.append({"row": r, "ka": ka, "na": na, "kb": kb, "nb": nb,
                     "surv_a": pa, "surv_b": pb, "surv_diff": d,
                     "surv_lo": dlo, "surv_hi": dhi,
                     "n_uniq_a": len(ua), "n_uniq_b": len(ub),
                     "med_a": med_a, "med_b": med_b,
                     "ratio": rat, "ratio_lo": rlo, "ratio_hi": rhi,
                     "mw_p": p, "estimable": bool(enough)})

    # Headline: breadth at the deepest row both arms still populate enough to
    # estimate. That is where the search is actually struggling, and where a
    # change has to show up to matter.
    est = [r for r in rows if r["estimable"]]
    head = est[-1] if est else None
    print()
    if head:
        R, rat, rlo, rhi = head["row"], head["ratio"], head["ratio_lo"], head["ratio_hi"]
        if rlo > 1:
            verdict = (f"the exclusion HELPS: at row {R} the frontier is "
                       f"{rat:.2f}x wider [{rlo:.2f},{rhi:.2f}]")
        elif rhi < 1:
            verdict = (f"the exclusion HURTS: at row {R} the frontier is "
                       f"{rat:.2f}x as wide [{rlo:.2f},{rhi:.2f}]")
        else:
            verdict = (f"no effect on frontier width at row {R}: {rat:.2f}x "
                       f"[{rlo:.2f},{rhi:.2f}] straddles 1.00")
    else:
        verdict = (f"neither arm reached any row with {MIN_CONFIGS}+ configs -- "
                   f"run longer, or lower --stop_row")
    print(f"[ab] VERDICT: {verdict}")

    deep = [r for r in rows if r["ka"] + r["kb"] > 0]
    if deep:
        d = deep[-1]
        print(f"[ab] deepest row reached at all: {d['row']} "
              f"({d['ka']} {la}, {d['kb']} {lb})")
    print(f"[ab] throughput: {na:,} vs {nb:,} borders ({nb/max(na,1):.2f}x) -- a "
          f"smaller database sweeps faster, which is a win separate from depth")

    if args.json_out:
        Path(args.json_out).write_text(json.dumps({
            "label_a": la, "label_b": lb, "n_a": na, "n_b": nb,
            "mean_a": sum(A)/na, "mean_b": sum(B)/nb,
            "hist_a": {str(d): sum(1 for v in A if v == d) for d in range(dmax+1)},
            "hist_b": {str(d): sum(1 for v in B if v == d) for d in range(dmax+1)},
            "rows": rows, "verdict": verdict,
            "headline_row": (head or {}).get("row"),
        }, indent=1))
        print(f"[out] {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
