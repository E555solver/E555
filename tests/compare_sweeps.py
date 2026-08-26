#!/usr/bin/env python3
"""compare_sweeps.py -- turn two beamer/finalizer logs into a paired comparison.

    python3 tests/compare_sweeps.py old.log new.log
    python3 tests/compare_sweeps.py --labels before,after a.log b.log

Reads the per-configuration [sweep] lines both tools print under --verbose and
reports, per arm: configurations run, how many reached the stop row, boards
emitted, and survivors per hour. Then it pairs the two arms on the border row
and bottom (beamer) or partial line (finalizer) so the comparison is between
arms that saw the SAME starting configurations, and runs a sign test over those
pairs.

Rank on survivors and on extinction depth, never on beam width: width climbs
while survival collapses, and that trap has caught this project before.

The sign test is exact and two-sided over the pairs that moved; ties carry no
information about direction and are excluded, which is what makes 4 wins and 1
loss p = 0.375 rather than anything to act on.
"""
import collections
import re
import sys

# [sweep] r3b0l2 ... (beamer)   [sweep] p0r1l17 ... (finalizer)
SWEEP_RE = re.compile(
    r'\[sweep\]\s+(?P<id>(?:r\d+b\d+l\d+)|(?:p\d+r\d+l\d+))\s+'
    r'row=(?P<row>\d+)\s+width=(?P<width>\d+)\s+emitted=(?P<emitted>\d+)\s+'
    r'partials=(?P<partials>\d+)\s+reason=(?P<reason>\S+).*?wall=(?P<wall>[\d.]+)s')
ID_RE = re.compile(r'([a-z])(\d+)')


def parse(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = SWEEP_RE.search(line)
            if not m:
                continue
            parts = ID_RE.findall(m.group("id"))
            rows.append(dict(
                # the first two id components name the starting configuration;
                # the last is which column/repeat was tried from it
                pair=tuple(parts[:2]),
                row=int(m.group("row")), width=int(m.group("width")),
                emitted=int(m.group("emitted")), reason=m.group("reason"),
                wall=float(m.group("wall"))))
    if not rows:
        sys.exit("no [sweep] configuration lines in %s -- was it run with "
                 "--verbose?" % path)
    return rows


def sign_test(wins, losses):
    """Exact two-sided sign test. Ties are excluded, not counted as agreement."""
    n = wins + losses
    if n == 0:
        return 1.0
    from math import comb
    k = min(wins, losses)
    tail = sum(comb(n, i) for i in range(k + 1))
    return min(1.0, 2.0 * tail / (2 ** n))


def main():
    argv = sys.argv[1:]
    labels = ["old", "new"]
    if argv and argv[0] == "--labels":
        labels = argv[1].split(",")
        argv = argv[2:]
    if len(argv) != 2:
        sys.exit(__doc__)

    arms = dict(zip(labels, (parse(p) for p in argv)))

    print("%-8s %6s %8s %7s %8s %8s %8s %7s" % (
        "arm", "cfgs", "reached", "rate", "emitted", "wall_s", "per_hr", "r1dead"))
    for label, rows in arms.items():
        n = len(rows)
        hit = sum(r["reason"] == "stop_row" for r in rows)
        wall = sum(r["wall"] for r in rows)
        print("%-8s %6d %8d %7.3f %8d %8.0f %8.1f %7d" % (
            label, n, hit, hit / max(n, 1), sum(r["emitted"] for r in rows),
            wall, 3600 * hit / max(wall, 1e-9),
            sum(r["row"] <= 1 for r in rows)))

    print("\nrows reached (a configuration dying at row R filled R-1):")
    for label, rows in arms.items():
        c = collections.Counter(r["row"] for r in rows)
        print("  %-6s " % label + "  ".join("r%d:%d" % kv for kv in sorted(c.items())))

    print("\npaired on the starting configuration -- only those both arms saw:")
    per_arm = {}
    for label, rows in arms.items():
        agg = collections.defaultdict(lambda: [0, 0])
        for r in rows:
            agg[r["pair"]][0] += r["reason"] == "stop_row"
            agg[r["pair"]][1] += r["emitted"]
        per_arm[label] = agg

    a, b = labels[0], labels[1]
    shared = sorted(set(per_arm[a]) & set(per_arm[b]))
    if not shared:
        print("  none in common -- the two arms did not see the same configurations")
        return 0

    wins = losses = ties = 0
    for key in shared:
        ha, hb = per_arm[a][key][0], per_arm[b][key][0]
        if hb > ha:
            wins += 1
            mark = "  " + b
        elif ha > hb:
            losses += 1
            mark = "  " + a
        else:
            ties += 1
            mark = "  =="
        name = "".join("%s%s" % kv for kv in key)
        print("  %-8s %s %d (%d boards)   %s %d (%d boards)%s" % (
            name, a, ha, per_arm[a][key][1], b, hb, per_arm[b][key][1], mark))

    p = sign_test(wins, losses)
    print("\n%s better on %d, %s better on %d, tied %d of %d shared "
          "configurations" % (b, wins, a, losses, ties, len(shared)))
    print("exact two-sided sign test over the %d that moved: p = %.3f%s"
          % (wins + losses, p,
             "" if p < 0.05 else "  -- not significant, do not ship on this alone"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
