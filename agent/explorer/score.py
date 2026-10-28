#!/usr/bin/env python3
"""
score.py -- the explorer kit's measure of any board file (agent/explorer/EXPLORER.md).
It imports the repository's own tools/E555_viewer.py, E555_rank.py and
E555_rotate.py and reads the repository's own data/seed_Edge5.txt, so editing
the copies in a campaign's lab cannot change a number. agent/explorer/lab.sh
calls it on every run's output; run it by hand on any file, on any machine.

    python3 agent/explorer/score.py FILE... [--clues center] [--top N --out F]
                                    [--best_out F] [--deepest_out F]
    python3 agent/explorer/score.py --self_test
    SCORE files=1 boards=3077 malformed=0 valid=12 best_score=461 exact_boards=2950
          best_exact=387 best_exact_placed=208 hist=360:2950,380:41,400:0,...
          clue=center solved=no

Two scores, both out of 480 internal edges:
  score  a COMPLETE board's matched edges, counted only when the board is
         VALID: all 256 pieces placed once, every piece legal on its cell
         (grey sides exactly where the cell faces out, which is the
         referee's frame check), and the clue held. The scale of the 465
         record (agent/referee.sh gives the same number on a VALID board).
  exact  a board of ANY size with no break: every edge between two placed
         neighbours matches, every placed piece is legal on its cell, and the
         clue is not contradicted. Its value is the number of those matched
         edges, so scattered pieces earn nothing and a solution scores 480.
         Rows 0..10 complete score 325, rows 0..12 387, rows 0..14 449; the
         196 inner cells with the frame open score 364.
The clue (--clues none|center|all, default center): the published Eternity II
pieces, in one of their four board orientations (tools/E555_viewer.py, CLUE).
A complete board must hold them. A partial board may leave a clue open (its
cell empty and its piece unplaced); a clue cell holding another piece, or a
clue piece elsewhere, contradicts it.

valid / exact_boards count the boards that qualify; hist counts the exact
boards at or above each threshold. solved=yes when a VALID board scores 480.
--top N --out F writes the N best VALID boards by score and the N best exact
boards by exact (a board in both lists once), best first, as canonical rows:
id, its score (or exact), pos[256], rot[256]. A name ending in .gz is written
gzip-compressed. --best_out / --deepest_out write the single best of each.
None of the three writes a file when it has no board. Input files may be
gzipped; any row whose last 512 fields are pos[256] and rot[256] is a board,
other rows (comments, headers) are skipped.
Exit status 0; 2 on bad usage or an unreadable file.
"""
from __future__ import annotations

import argparse
import csv
import gzip
import heapq
import sys
from pathlib import Path

sys.dont_write_bytecode = True               # leave no __pycache__ in tools/
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import E555_viewer as V                      # noqa: E402  seed, rows, clues
import E555_rank as R                        # noqa: E402  junctions, frame sides

SIDE, N_PIECES, UNPLACED, N_EDGES = V.SIDE, V.N_PIECES, V.UNPLACED, V.N_EDGES
SEED = REPO / "data" / "seed_Edge5.txt"
HIST = (360, 380, 400, 420, 440, 460, 480)
CLUE_MASKS = {"none": 0, "center": V.CLUE_CENTER, "all": V.CLUE_ALL}


class Scorer:
    def __init__(self, seed_path, clues):
        seed = V.load_seed(seed_path)
        # colours[p][s] = (N, E, S, W) of piece p at spin s
        self.colours = [[V.rotate_edges(seed[p], s) for s in range(4)] for p in range(N_PIECES)]
        # a piece is legal on a cell when its grey sides are the cell's outward sides
        grey = [[sum(1 << d for d in range(4) if self.colours[p][s][d] == 0) for s in range(4)]
                for p in range(N_PIECES)]
        out = [sum(1 << d for d in R.FRAME_SIDES.get(c, ())) for c in range(N_PIECES)]
        self.legal = [[[grey[p][s] == out[c] for s in range(4)] for p in range(N_PIECES)]
                      for c in range(N_PIECES)]
        self.junctions = R.ALL_JUNCTIONS
        self.mask = CLUE_MASKS[clues]
        self.clue_sets = [V.clue_list(o, self.mask) for o in range(4)] if self.mask else []

    def clue_ok(self, pos, rot, at):
        """(complete_ok, partial_ok): the clue held, or not contradicted."""
        if not self.mask:
            return True, True
        held = compatible = False
        for clues in self.clue_sets:
            all_held, contradicted = True, False
            for cell, piece, spin in clues:
                if pos[piece] == cell and rot[piece] == spin:
                    continue
                all_held = False
                if pos[piece] != UNPLACED or at[cell] is not None:
                    contradicted = True
                    break
            held |= all_held
            compatible |= not contradicted
        return held, compatible

    def measure(self, pos, rot):
        """None if malformed, else (placed, score or -1, exact or -1)."""
        cols, legal = self.colours, self.legal
        cell_col = [None] * N_PIECES
        placed, is_legal = 0, True
        for p, c in enumerate(pos):
            if c == UNPLACED:
                continue
            s = rot[p]
            if not 0 <= c < N_PIECES or cell_col[c] is not None or not 0 <= s <= 3:
                return None
            cell_col[c] = cols[p][s]
            placed += 1
            if is_legal and not legal[c][p][s]:
                is_legal = False
        complete = placed == N_PIECES
        matched = mismatched = 0
        for a, b, da, db in self.junctions:
            x, y = cell_col[a], cell_col[b]
            if x is None or y is None:
                continue
            if x[da] == y[db]:
                matched += 1
            else:
                mismatched += 1
                if not complete:                # neither a VALID board nor exact
                    return placed, -1, -1
        held, compatible = self.clue_ok(pos, rot, cell_col)
        score = matched if complete and is_legal and held else -1
        exact = matched if not mismatched and is_legal and compatible else -1
        return placed, score, exact


def rows_of(path):
    """(id, pos, rot) of every board row: its last 512 fields are pos and rot.
    Comments (# or %), headers and short rows are skipped."""
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt", newline="") as fh:
        for fields in csv.reader(fh):
            if len(fields) < 2 * N_PIECES or fields[0].lstrip()[:1] in ("#", "%"):
                continue
            try:
                pos = list(map(int, fields[-2 * N_PIECES:-N_PIECES]))
                rot = list(map(int, fields[-N_PIECES:]))
            except ValueError:
                continue
            meta = fields[:-2 * N_PIECES]
            yield (meta[0].strip() if meta else "?"), pos, rot


def canonical(rid, value, pos, rot):
    return ",".join([rid, str(value)] + [str(x) for x in pos] + [str(x) for x in rot])


def write_rows(path, lines):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "".join(line + "\n" for line in lines)
    if path.name.endswith(".gz"):
        with gzip.open(path, "wt") as fh:
            fh.write(text)
    else:
        path.write_text(text)


def score_files(paths, scorer, top=0):
    """The summary dict, plus the best rows (score list, exact list)."""
    s = dict(files=len(paths), boards=0, malformed=0, valid=0, best_score=-1,
             exact_boards=0, best_exact=-1, best_exact_placed=0, hist=[0] * len(HIST))
    keep = max(top, 1)
    best_s, best_e = [], []                    # min-heaps of (value, -seq, line)
    seq = 0
    for path in paths:
        for rid, pos, rot in rows_of(path):
            seq += 1
            s["boards"] += 1
            m = scorer.measure(pos, rot)
            if m is None:
                s["malformed"] += 1
                continue
            placed, score, exact = m
            if score >= 0:
                s["valid"] += 1
                s["best_score"] = max(s["best_score"], score)
                item = (score, -seq, canonical(rid, score, pos, rot))
                (heapq.heappush if len(best_s) < keep else heapq.heappushpop)(best_s, item)
            if exact >= 0:
                s["exact_boards"] += 1
                if exact > s["best_exact"] or (exact == s["best_exact"] and placed > s["best_exact_placed"]):
                    s["best_exact"], s["best_exact_placed"] = exact, placed
                for i, t in enumerate(HIST):
                    if exact >= t:
                        s["hist"][i] += 1
                item = ((exact, placed), -seq, canonical(rid, exact, pos, rot))
                (heapq.heappush if len(best_e) < keep else heapq.heappushpop)(best_e, item)
    return s, [x[2] for x in sorted(best_s, reverse=True)], [x[2] for x in sorted(best_e, reverse=True)]


def summary_line(s, clues):
    hist = ",".join(f"{t}:{n}" for t, n in zip(HIST, s["hist"]))
    solved = "yes" if s["best_score"] == N_EDGES else "no"
    return (f"SCORE files={s['files']} boards={s['boards']} malformed={s['malformed']} "
            f"valid={s['valid']} best_score={max(s['best_score'], 0)} "
            f"exact_boards={s['exact_boards']} best_exact={max(s['best_exact'], 0)} "
            f"best_exact_placed={s['best_exact_placed']} hist={hist} clue={clues} solved={solved}")


# -- self test ------------------------------------------------------------------

def self_test():
    synth = Scorer(REPO / "data" / "synth_seed.txt", "none")
    _, pos, rot = next(rows_of(REPO / "data" / "synth_solution_480.csv"))
    fails = []

    def check(name, got, want):
        print(f"  {'ok  ' if got == want else 'FAIL'} {name}: {got} (want {want})")
        if got != want:
            fails.append(name)

    check("synthetic solution", synth.measure(pos, rot), (256, 480, 480))
    p2 = list(pos)                             # swap two inner pieces: breaks
    a, b = p2.index(5 * SIDE + 5), p2.index(9 * SIDE + 9)
    p2[a], p2[b] = p2[b], p2[a]
    m = synth.measure(p2, rot)
    check("two inner pieces swapped: valid but below 480, not exact", (m[1] < 480, m[1] >= 0, m[2]), (True, True, -1))
    r2 = list(rot)                             # a border piece turned: grey inside
    border = pos.index(7)                      # piece on cell 7 (bottom row)
    r2[border] = (r2[border] + 2) % 4
    check("border piece turned inward", synth.measure(pos, r2)[1:], (-1, -1))
    p3 = [c if c // SIDE <= 10 else UNPLACED for c in pos]
    check("rows 0..10 only", synth.measure(p3, rot), (176, -1, 325))
    p4 = [c if c in (0, 2 * SIDE + 2, 4 * SIDE + 4, 6 * SIDE + 9) else UNPLACED for c in pos]
    check("scattered pieces earn nothing", synth.measure(p4, rot), (4, -1, 0))
    check("malformed: two pieces on one cell", synth.measure([0, 0] + pos[2:], rot), None)

    real_c = Scorer(SEED, "center")
    real_n = Scorer(SEED, "none")
    _, p464, r464 = next(rows_of(REPO / "data" / "best_464.csv"))
    _, p465, r465 = next(rows_of(REPO / "data" / "best_465.csv"))
    check("best_464, centre clue", real_c.measure(p464, r464)[1], 464)
    check("best_465, centre clue: not held", real_c.measure(p465, r465)[1], -1)
    check("best_465, no clue", real_n.measure(p465, r465)[1], 465)
    low = [c if c // SIDE <= 5 else UNPLACED for c in p464]
    m = real_c.measure(low, r464)
    check("best_464 rows 0..5, clue open", (m[0], m[2] >= 0), (96, True))
    print("SELF_TEST " + ("failed: " + ", ".join(fails) if fails else "passed"))
    return 1 if fails else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--clues", choices=sorted(CLUE_MASKS), default="center")
    ap.add_argument("--top", type=int, default=0)
    ap.add_argument("--out")
    ap.add_argument("--best_out")
    ap.add_argument("--deepest_out")
    ap.add_argument("--self_test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.files:
        ap.error("give at least one board file")
    if args.out and args.top <= 0:
        ap.error("--out needs --top N")
    for f in args.files:
        if not Path(f).is_file():
            print(f"score: no such file: {f}", file=sys.stderr)
            return 2
    scorer = Scorer(SEED, args.clues)
    s, best_s, best_e = score_files(args.files, scorer, args.top)
    if args.out:
        seen, lines = set(), []
        for line in best_s[:args.top] + best_e[:args.top]:
            board = line.split(",", 2)[2]
            if board not in seen:
                seen.add(board)
                lines.append(line)
        if lines:
            write_rows(args.out, lines)
    if args.best_out and best_s:
        write_rows(args.best_out, best_s[:1])
    if args.deepest_out and best_e:
        write_rows(args.deepest_out, best_e[:1])
    print(summary_line(s, args.clues))
    return 0


if __name__ == "__main__":
    sys.exit(main())
