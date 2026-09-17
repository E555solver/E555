#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
E555_sort_rotations.py -- order and orient a Stage A rotations file.

WHAT IT DOES

    src/A_border/E555_edge_annealer.py appends one border per restart, in
    restart order, as a pair of lines:

        #  TOP=4320 RIGHT=77760 BOTTOM=3744 LEFT=69120  Score=9.7510
        r7, 0,3,1,2, ... (256 spins)

    Stage B reads the rows in file order and, with --num_rows, takes the first
    N of them, so the order of this file decides which borders get searched.
    And a border is four numbers on four sides: the beam grows bottom-up, so a
    row's BOTTOM count is how many starts it offers and its TOP count is how
    many ways it can be closed. This tool therefore does two things:

      * re-orders the pairs, by score or by any side measure (--sort), and
      * optionally turns each row onto a chosen side (--max_top and friends),
        every row by its own angle, so the whole file presents the side you
        want to attack from.

    Everything not turned is written back byte for byte.

WHY IT IS A TOOL AND NOT THREE LINES OF awk

    The score and the four trail counts live in a COMMENT, so reading them
    means parsing the annealer's prose. The pipeline used to do that inline
    with `FS = "Score="`, which silently mis-pairs a comment with the wrong row
    the moment the annealer prints any other comment line -- it now writes a
    `# run ...` provenance marker, which that awk survives only by luck. Doing
    it here means the format is read in one place, checked, and reported on.

    Two comment forms exist and both are read. The annealer writes `TOP=4320`
    and `Score=9.7510`; older files (data/borders_annealed_fix12.csv) write
    `TOP,4320` and `Score,9.3108`. Reading only the `=` form scored every row
    of that file -inf and degenerated the sort to input order.

TURNING, AND WHAT A TURN CAN AND CANNOT PRESERVE

    A border piece's spin IS its side -- its grey face points at the frame edge
    it belongs to -- so turning a row means `spin += 3n mod 4` on every piece
    with a grey face, exactly as tools/E555_rotate.py --rotations does, and the
    four trail counts follow the board round. Both are rewritten.

    `Score=` is NOT rewritten. It is the annealer's own weighted objective and
    its weights are not in the file, so it cannot be recomputed here; under
    asymmetric weights it is not rotation-invariant either. A turned row keeps
    the score it was found with, and the appended `Turn=` note says how far it
    was turned, so the orientation the annealer scored is always recoverable.

USAGE

    python3 E555_sort_rotations.py raw_rotations.csv -o rotations.csv
    python3 E555_sort_rotations.py raw.csv -o out.csv --top 50
    python3 E555_sort_rotations.py raw.csv --sort min_side      # to stdout
    python3 E555_sort_rotations.py raw.csv --max_top -o up.csv
    python3 E555_sort_rotations.py raw.csv --min_bottom --sort top -o t.csv

    With no -o the rotations file goes to stdout and every diagnostic goes to
    stderr, so the tool pipes. Reading and writing the same path is refused:
    the input would be truncated before it is parsed.
"""

from __future__ import annotations
import argparse, math, os, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import E555_viewer as V                 # seed loading
import E555_rotate as RT                # the rotation rules, already written

SPINS = 256
SIDES = RT.SIDE_NAMES                   # ("top", "right", "bottom", "left")

# One regex for all four counts, so a relabel is a single pass and cannot read
# a value it has already overwritten. Group 2 carries the separator and its
# spacing, which is what lets the `=` and the `,` form each stay themselves.
SIDES_RE = re.compile(r"\b(TOP|RIGHT|BOTTOM|LEFT)(\s*[=,]\s*)(\d+)")
SCORE_RE = re.compile(r"\bScore\s*[=,]\s*(-?[\d.]+(?:[eE][-+]?\d+)?)")

# Sorting is always best-first, as in E555_rank.py: a '-' prefix asks for
# worst-first instead. "Best" for a named side or the score is the larger
# number -- they are magnitudes. The three derived keys exist for the opposite
# question, finding a CONSTRAINED border rather than a big one, so they sort
# constraint-first: tightest minimum, tightest maximum, most lopsided.
SORTABLE = ("score", "top", "right", "bottom", "left",
            "min_side", "max_side", "spread")
HIGH_IS_BETTER = {"score", "top", "right", "bottom", "left", "spread"}

DIRECTIONS = """sort keys, all best-first (--sort=-KEY inverts one):
  score                the annealer's own objective, highest first
  top right bottom left  that side's Euler-trail count, highest first
  min_side             the row's tightest side, TIGHTEST first
  max_side             the row's loosest side, tightest first
  spread               ln(max side) - ln(min side), MOST LOPSIDED first
Several keys comma-separated break each other's ties, left to right."""


# ---- reading ------------------------------------------------------------------

def sides_of(comment):
    """The four trail counts as {side: int}, or None if the comment has no set.

    All four must be present: three counts and a guess is not a border, and a
    turn would relabel the missing one into silence."""
    if not comment:
        return None
    found = {m.group(1).lower(): int(m.group(3)) for m in SIDES_RE.finditer(comment)}
    return found if len(found) == len(SIDES) else None


def score_of(comment):
    """The annealer's Score, or None when the comment does not carry one."""
    if not comment:
        return None
    m = SCORE_RE.search(comment)
    return float(m.group(1)) if m else None


def measures(score, counts):
    """One record of every sortable measure; None for one the comment lacks.

    A row with counts but no score still sorts correctly by a side key, and the
    other way round, because sort_key_of() sends only the missing measure last.

    `spread` is taken in logs because the counts span five decades: 483840 over
    2880 and 4320 over 3744 are both "one side much richer", and on any linear
    reading the first swamps every comparison."""
    rec = {k: None for k in SORTABLE}
    rec["unknown"] = 0 if (score is not None and counts is not None) else 1
    rec["score"] = score
    if counts is None:
        return rec
    vals = [counts[s] for s in SIDES]
    for s in SIDES:
        rec[s] = float(counts[s])
    rec["min_side"] = float(min(vals))
    rec["max_side"] = float(max(vals))
    rec["spread"] = math.log(max(1, max(vals))) - math.log(max(1, min(vals)))
    return rec


def parse(path):
    """Return (borders, preamble).

    A border is a dict holding its comment, its verbatim row line, the parsed
    counts and score, and the sortable measures. `preamble` is the leading
    comment lines that describe no row (the annealer's `# run` marker).

    CRLF is normalised away: a stray \\r at the end of a comment would land in
    the middle of the line once a `Turn=` note is appended to it."""
    borders, preamble, pending = [], [], None
    with open(path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\r\n")
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
            counts, score = sides_of(pending), score_of(pending)
            if score is None:
                warn(f"{path}:{lineno}: no readable Score above this row; sorting it last")
            if counts is None:
                warn(f"{path}:{lineno}: no readable TOP/RIGHT/BOTTOM/LEFT set above "
                     "this row; it cannot be turned or sorted by side")
            borders.append({"comment": pending, "row": line, "lineno": lineno,
                            "spins": [int(f) for f in fields[1:]],
                            "counts": counts, "score": score,
                            "m": measures(score, counts)})
            pending = None
    if pending is not None:
        preamble.append(pending)
    return borders, preamble


# ---- turning ------------------------------------------------------------------

def turns_between(src, dst):
    """Quarter-turns clockwise that carry side `src` round to side `dst`."""
    n, cur = 0, src
    while cur != dst:
        cur, n = RT.CW_SIDE[cur], n + 1
    return n


def pick_turn(counts, dst, want_max):
    """Turns that put the largest (or smallest) count on side `dst`.

    Ties take the fewest turns, so a row already oriented is left alone."""
    vals = [counts[s] for s in SIDES]
    target = max(vals) if want_max else min(vals)
    return min(turns_between(s, dst) for s in SIDES if counts[s] == target)


def turn_counts(counts, n):
    """Relabel the four counts: a side's pieces land on CW_SIDE^n of it."""
    out = {}
    for s in SIDES:
        dst = s
        for _ in range(n):
            dst = RT.CW_SIDE[dst]
        out[dst] = counts[s]
    return out


def turn_comment(comment, counts, n, flag):
    """Substitute the relabelled counts in place and append the turn note.

    In place, and keeping each row's own separator, so the rest of the comment
    -- `FixCorner 1:`, `Ratio`, `Overlap`, the score -- rides through untouched
    and a legacy row stays a legacy row. The note is appended even for a zero
    turn: it records that this row was considered under `flag` and was already
    oriented, which is what makes the whole file self-describing."""
    relabelled = SIDES_RE.sub(
        lambda m: m.group(1) + m.group(2) + str(counts[m.group(1).lower()]), comment)
    return f"{relabelled}  Turn={n * 90}({flag})"


def turn_row(line, spins, greyed, n):
    """Rewrite the spin fields of a verbatim row line, keeping its spacing."""
    raw = line.split(",")
    new = list(spins)
    for pid in greyed:
        new[pid] = (new[pid] + 3 * n) % 4
    for i, v in zip(range(len(raw) - SPINS, len(raw)), new):
        piece = raw[i]
        lead = piece[:len(piece) - len(piece.lstrip())]
        tail = piece[len(piece.rstrip()):]
        raw[i] = lead + str(v) + tail
    return ",".join(raw), new


def orient(borders, dst, want_max, flag, seed):
    """Turn every border onto side `dst`; returns the ones that survived.

    The two checks are the ones tools/E555_rotate.py --rotations applies, and
    they are the only thing that can catch a wrong turn: relabelled counts look
    plausible whatever the spins did. A row failing either is dropped, as it
    would be there and in fin_rot_row_valid."""
    greyed = [pid for pid, e in enumerate(seed) if 0 in e]
    kept, turns = [], {}
    for b in borders:
        if b["counts"] is None:
            warn(f"line {b['lineno']}: no trail counts, left unturned")
            kept.append(b)
            continue
        n = pick_turn(b["counts"], dst, want_max)
        before = RT.classify_border(seed, b["spins"])
        sizes = [len(before[s]) for s in SIDES]
        if len(before["corner"]) != 4 or any(s != RT.EDGE_LEN for s in sizes):
            warn(f"line {b['lineno']}: not a legal 14/14/14/14 border partition "
                 f"({sizes}, {len(before['corner'])} corners) -- row dropped")
            continue
        row, spins = turn_row(b["row"], b["spins"], greyed, n)
        after = RT.classify_border(seed, spins)
        want = RT.turned_sides(before, n)
        if any(after[s] != want[s] for s in SIDES) or after["corner"] != want["corner"]:
            warn(f"line {b['lineno']}: the side sets did not follow the turn "
                 "-- row dropped")
            continue
        counts = turn_counts(b["counts"], n)
        b = dict(b, row=row, spins=spins, counts=counts,
                 comment=turn_comment(b["comment"], counts, n, flag),
                 m=measures(b["score"], counts))
        turns[n] = turns.get(n, 0) + 1
        kept.append(b)
    if turns:
        info("[turn] " + ", ".join(f"{c} row(s) by {n * 90} deg"
                                  for n, c in sorted(turns.items())))
    return kept


# ---- sorting ------------------------------------------------------------------

def parse_sort_spec(spec):
    """The `--sort` spec as [(measure, worst_first), ...], validated."""
    keys = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        worst_first = part.startswith("-")
        name = part[1:] if worst_first else part
        if name not in SORTABLE:
            raise SystemExit(f"[ERROR] unknown sort key '{name}'. "
                             f"Choose from: {', '.join(SORTABLE)}")
        keys.append((name, worst_first))
    if not keys:
        raise SystemExit("[ERROR] --sort needs at least one key")
    return keys


def sort_key_of(keys, b, seq):
    """One ascending-is-better tuple, so a plain sort yields best first.

    The tuple ascends, so `inf` is where a measure the comment did not carry
    belongs: last under that key, whichever way the key points. `seq` closes the
    tuple, which keeps ties in input order and makes it unique, so a sort never
    has to compare the records themselves."""
    m = b["m"]
    out = []
    for n, worst_first in keys:
        v = m[n]
        if v is None:
            out.append(float("inf"))
        else:
            out.append(-v if (n in HIGH_IS_BETTER) != worst_first else v)
    return tuple(out) + (seq,)


# ---- reporting ----------------------------------------------------------------

def warn(msg):
    print(f"[warn] {msg}", file=sys.stderr)


def info(msg):
    print(msg, file=sys.stderr)


def build_parser():
    ap = argparse.ArgumentParser(
        description="Order a Stage A rotations file, and optionally turn every "
                    "row onto the side you want to attack from.",
        epilog=DIRECTIONS,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="rotations CSV written by E555_edge_annealer.py --out")
    ap.add_argument("-o", "--out", help="write here (default: stdout)")
    ap.add_argument("--top", type=int, default=0,
                    help="keep only the best N borders")
    ap.add_argument("--sort", default="score",
                    help="comma-separated measures, best board first "
                         "(default: score). Use --sort=-KEY (with the '=') to "
                         "invert one to worst-first.")
    ap.add_argument("--seed_file",
                    help="piece seed file, needed only with a turn "
                         "(default: data/seed_Edge5.txt)")
    # At most one turn: two of these would each undo the other, and there is no
    # order in which both could hold. No aliases, no accept-and-ignore.
    g = ap.add_mutually_exclusive_group()
    for side in SIDES:
        g.add_argument(f"--max_{side}", action="store_true",
                       help=f"turn each row so its largest trail count is on the "
                            f"{side}")
    for side in SIDES:
        g.add_argument(f"--min_{side}", action="store_true",
                       help=f"turn each row so its smallest trail count is on the "
                            f"{side}")
    return ap


def chosen_turn(args):
    """(destination side, want_max, flag) for the turn asked for, or None."""
    for want_max in (True, False):
        for side in SIDES:
            flag = f"--{'max' if want_max else 'min'}_{side}"
            if getattr(args, flag[2:]):
                return side, want_max, flag
    return None


def main():
    args = build_parser().parse_args()

    if args.out and os.path.exists(args.out) and os.path.samefile(args.input, args.out):
        raise SystemExit("[ERROR] --out is the input file; write somewhere else")
    keys = parse_sort_spec(args.sort)

    borders, preamble = parse(args.input)
    if not borders:
        raise SystemExit(f"[ERROR] {args.input} holds no rotations rows")

    turn = chosen_turn(args)
    note = ""
    if turn:
        dst, want_max, flag = turn
        # The seed is read only here: plain sorting needs no seed, and a tool
        # that demanded one would stop working wherever the pipelines run it.
        seed = V.load_seed(V.find_seed(args.seed_file))
        borders = orient(borders, dst, want_max, flag, seed)
        if not borders:
            raise SystemExit("[ERROR] no row survived the turn")
        note = (f", each row turned to put its "
                f"{'largest' if want_max else 'smallest'} count on the {dst}")

    order = ",".join(("-" if w else "") + n for n, w in keys)
    borders.sort(key=lambda b: sort_key_of(keys, b, b["lineno"]))
    kept = borders[:args.top] if args.top > 0 else borders

    info(f"[sum] {len(borders)} border(s) in {args.input}, sort {order}"
         f"{note}; keeping {len(kept)}")
    for tag, b in (("best ", kept[0]["m"]), ("worst", kept[-1]["m"])):
        if b["unknown"]:
            info(f"[{tag}] a row whose comment could not be read")
            continue
        info(f"[{tag}] Score={b['score']:.4f}  "
             + " ".join(f"{s.upper()}={int(b[s])}" for s in SIDES)
             + f"  spread={b['spread']:.2f}")

    out = open(args.out, "w") if args.out else sys.stdout
    try:
        for line in preamble:
            out.write(line + "\n")
        out.write(f"# sorted by {order}{note}, from {Path(args.input).name} "
                  "by E555_sort_rotations.py\n")
        for b in kept:
            if b["comment"]:
                out.write(b["comment"] + "\n")
            out.write(b["row"] + "\n")
    finally:
        if args.out:
            out.close()
    if args.out:
        info(f"[out] {len(kept)} border(s), best first -> {args.out}")


if __name__ == "__main__":
    main()
