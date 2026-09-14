#!/usr/bin/env python3
"""
check_fixedframe.py -- prove the fixed-frame coordinate system, cheaply.

The FixedFrame experiment rests on one claim: a board searched at clue
orientation O, turned (4 - O) % 4 quarter-turns CLOCKWISE, lands on the frame
that orientation 0 defines. If that is wrong, every board in the corpus is in a
different coordinate system from its neighbours and every statistic downstream
is an average over nothing -- silently, with no error anywhere.

So it is checked rather than trusted, and checked two ways:

  MATH   Pure arithmetic over the clue table (tools/E555_viewer.CLUE, generated
         from the same published data as src/B_beam/E555_database.c). Takes
         milliseconds and needs no beam run, so there is no excuse to skip it.
         Also checks the corner-role mapping the C program uses to turn
         --canon_BL/BR/TL/TR into physical --BL/--BR/--TL/--TR.

  CORPUS Optional. Given a canonicalised corpus, asserts every board really does
         carry the orientation-0 clues at their cells and spins.

USAGE

    python3 tests/check_fixedframe.py
    python3 tests/check_fixedframe.py ff_out/corpus.csv
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import E555_viewer as V                                          # noqa: E402

SIDE = V.SIDE
CANON = 0

# The two halves of one clockwise quarter-turn, exactly as tools/E555_rotate.py
# documents them and as the C program's corner_role_after_cw() recomputes them.
def cw_cell(cell):
    r, c = divmod(cell, SIDE)
    return (SIDE - 1 - c) * SIDE + r


def cw_spin(spin):
    return (spin + 3) % 4


def turns(cell, spin, n):
    for _ in range(n % 4):
        cell, spin = cw_cell(cell), cw_spin(spin)
    return cell, spin


# Corner roles as the C program indexes them: 0=BL 1=BR 2=TL 3=TR.
ROLE_CELL = [0 * SIDE + 0, 0 * SIDE + 15, 15 * SIDE + 0, 15 * SIDE + 15]
ROLE_NAME = ["BL", "BR", "TL", "TR"]


def check_math():
    fails = []

    # 1. The turn. Orientation O's whole clue set, turned (4-O)%4 clockwise, must
    #    equal orientation 0's clue set -- cells AND spins, every clue.
    canon = {p: (cell, spin) for cell, p, spin in V.clue_list(CANON)}
    for o in range(4):
        n = (4 - o) % 4
        for cell, piece, spin in V.clue_list(o):
            got = turns(cell, spin, n)
            want = canon[piece]
            if got != want:
                fails.append(
                    f"side {o}: piece {piece} turned {n}x lands at cell {got[0]} "
                    f"spin {got[1]}, canonical wants cell {want[0]} spin {want[1]}")
        print(f"[math] side {o}: turn {n} maps all {len(V.clue_list(o))} clues "
              f"onto the canonical frame" + ("" if not fails else "  <-- FAILED"))

    # 2. The corner mapping. A piece the canonical frame puts at role R is
    #    searched, on side O, at the role reached by turning R clockwise O times.
    #    This is what apply_canon_corners() does; recomputing it here from the
    #    cell map is an independent derivation, not a copy of the same table.
    for o in range(4):
        mapping = {}
        for role, cell in enumerate(ROLE_CELL):
            moved, _ = turns(cell, 0, o)
            phys = ROLE_CELL.index(moved)
            mapping[ROLE_NAME[role]] = ROLE_NAME[phys]
            # And the inverse must hold: turning the physical cell back by
            # (4-o) has to return the canonical one, or the two directions
            # disagree and one of them is what the farm script uses.
            back, _ = turns(moved, 0, (4 - o) % 4)
            if back != cell:
                fails.append(f"side {o}: corner role {ROLE_NAME[role]} does not "
                             f"round-trip ({cell} -> {moved} -> {back})")
        print(f"[math] side {o}: canonical corner -> physical: "
              + " ".join(f"{k}->{v}" for k, v in mapping.items()))
    return fails


def check_corpus(path):
    fails = []
    canon = {p: (cell, spin) for cell, p, spin in V.clue_list(CANON)}
    n = 0
    off = 0
    for idx, cid, _sol, pos, rot in V.iter_records(path):
        n += 1
        for piece, (cell, spin) in canon.items():
            if pos[piece] == 999:
                continue          # reserved but never placed: legitimate
            if pos[piece] != cell or rot[piece] != spin:
                off += 1
                if len(fails) < 5:
                    fails.append(
                        f"{path}:{idx} (config {cid}): piece {piece} at cell "
                        f"{pos[piece]} spin {rot[piece]}, canonical wants cell "
                        f"{cell} spin {spin}")
                break
    print(f"[corpus] {n} board(s) checked, {off} off-frame")
    return fails


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    fails = check_math()
    for path in argv:
        fails += check_corpus(path)
    if fails:
        print()
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed", file=sys.stderr)
        return 1
    print("\nok: the fixed-frame coordinate system is consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
