#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
E555_sort_rotations.py -- order a Stage A rotations file, best border first.

WHAT IT DOES

    src/A_border/E555_edge_annealer.py appends one border per restart, in
    restart order, as a pair of lines:

        #  B=... L=... R=... T=...  Score=1234.5678
        r7, 0,3,1,2, ... (256 spins)

    Stage B reads the rows in file order and, with --num_rows, takes the
    first N of them -- so the order of this file decides which borders get
    searched. This tool re-orders the pairs by score, best first, and writes
    them back out unchanged otherwise.

WHY IT IS A TOOL AND NOT THREE LINES OF awk

    The score lives in a COMMENT, so sorting means parsing the annealer's
    prose. The pipeline used to do that inline with `FS = "Score="`, which
    silently mis-pairs a comment with the wrong row the moment the annealer
    prints any other comment line -- it now writes a `# run ...` provenance
    marker, which that awk survives only by luck. Doing it here means the
    format is read in one place, checked, and reported on.

USAGE

    python3 E555_sort_rotations.py raw_rotations.csv -o rotations.csv
    python3 E555_sort_rotations.py raw.csv -o out.csv --top 50
    python3 E555_sort_rotations.py raw.csv                 # report only

    Reading and writing the same path is refused: the input would be
    truncated before it is parsed.
"""

from __future__ import annotations
import argparse, os, sys

SPINS = 256


def parse(path):
    """Return (borders, preamble) where borders is a list of
    (score, comment_or_None, row) and preamble is the leading comment lines
    that describe no row (the annealer's `# run` marker)."""
    borders, preamble, pending = [], [], None
    with open(path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            if line.lstrip().startswith("#"):
                # A comment describes the row that FOLLOWS it. Two comments in
                # a row means the first one described nothing -- that is the
                # run marker, and it belongs to the file, not to a border.
                if pending is not None:
                    preamble.append(pending)
                pending = line
                continue
            fields = [f.strip() for f in line.split(",")]
            if len(fields) != SPINS + 1:
                raise SystemExit(f"[ERROR] {path}:{lineno}: expected an id plus "
                                 f"{SPINS} spins, got {len(fields)} fields")
            for f in fields[1:]:
                if f not in ("0", "1", "2", "3"):
                    raise SystemExit(f"[ERROR] {path}:{lineno}: spin '{f}' is not 0..3")
            borders.append((score_of(pending, path, lineno), pending, line))
            pending = None
    if pending is not None:
        preamble.append(pending)
    return borders, preamble


def score_of(comment, path, lineno):
    """The annealer writes `... Score=1234.5678` on the comment above each row.
    A row with no readable score sorts last rather than aborting the run: the
    row itself is still perfectly usable by Stage B."""
    if comment and "Score=" in comment:
        tail = comment.split("Score=", 1)[1].split()[0]
        try:
            return float(tail)
        except ValueError:
            pass
    print(f"[warn] {path}:{lineno}: no readable Score= above this row; sorting it last",
          file=sys.stderr)
    return float("-inf")


def main():
    ap = argparse.ArgumentParser(
        description="Order a Stage A rotations file by border score, best first.")
    ap.add_argument("input", help="rotations CSV written by E555_edge_annealer.py --out")
    ap.add_argument("-o", "--out", help="write here (default: report only, no output)")
    ap.add_argument("--top", type=int, default=0,
                    help="keep only the best N borders")
    args = ap.parse_args()

    if args.out and os.path.exists(args.out) and os.path.samefile(args.input, args.out):
        raise SystemExit("[ERROR] --out is the input file; write somewhere else")

    borders, preamble = parse(args.input)
    if not borders:
        raise SystemExit(f"[ERROR] {args.input} holds no rotations rows")
    borders.sort(key=lambda b: b[0], reverse=True)
    kept = borders[:args.top] if args.top > 0 else borders

    print(f"[sum] {len(borders)} border(s) in {args.input}; "
          f"best score {borders[0][0]:.4f}, worst {borders[-1][0]:.4f}")
    if args.out:
        with open(args.out, "w") as fh:
            for line in preamble:
                fh.write(line + "\n")
            for _score, comment, row in kept:
                if comment:
                    fh.write(comment + "\n")
                fh.write(row + "\n")
        print(f"[out] {len(kept)} border(s), best first -> {args.out}")


if __name__ == "__main__":
    main()
