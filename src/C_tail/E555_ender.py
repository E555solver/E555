#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
E555_ender.py -- Stage C CP-SAT closer for complete E555 boards.

NORMAL USE (from the repository root; the script is executable, so the
leading python3 is optional)

    src/C_tail/E555_ender.py data/seed_Edge5.txt boards.csv output.csv
    src/C_tail/E555_ender.py data/seed_Edge5.txt elites.csv output.csv --profile deep --threads 20
    src/C_tail/E555_ender.py data/seed_Edge5.txt elites.csv output.csv --profile superdeep

It prints the files, the plan and an estimate of the run time first, then
one block per board: every gain as it is found (with the time into the
board), a progress line every few minutes on long boards, and a closing
line; the run ends with the scores before and after.  --verbose adds every
CP-SAT call and redive.

WHAT A BOARD LOOKS LIKE WHEN IT GETS HERE

A board from the beamer's --end_dive/--end_polish (or E555_diver) has been
hill-climbed over every re-rotation and every swap of two dived cells, and
kicked and re-polished tens of thousands of times.  Measured on such boards: every
4x4 window touching a break is already OPTIMAL (CP-SAT proves it), nearly
every 4x6 window is, no exchange cycle over non-adjacent cells anywhere on the
board gains an edge, and no larger exact region tried (up to the dived rows
plus the whole frame, 102 cells, 300 s) found a gain either.  Re-diving the
same rows with more seeds gains nothing; re-diving them together with the
clean row they were built on gained an edge on a quarter of the boards --
the last exact row decided which pieces the top could use.  PROJECT_E555.md
has the numbers.

HOW IT SEARCHES

One exact engine.  A region of the board is opened: its pieces may permute
(within their class) and re-rotate, everything else is locked.  The model is
the Boolean encoding of M. Heule, "Solving edge-matching problems with
satisfiability solvers" (2008): a literal per (cell, piece, spin), exactly one
per cell and -- the redundancy Heule found decisive for a complete solver --
exactly one per piece; a colour literal per cell side; a match literal per
junction and colour.  The incumbent is kept feasible (breaks <= current), so
CP-SAT's LNS workers improve from it from the first second, and a call stops
at the first strictly better board.  A region solved to OPTIMAL is remembered
and skipped until a cell in it or on its boundary changes.

Neighbourhoods, cheapest first; the plan restarts after every gain:

  swap     exact re-assignment over a maximal set of pairwise non-adjacent
           cells (a linear assignment problem, milliseconds): every exchange
           cycle of any length, anywhere on the board
  redive   lift the outer rows holding the damage -- together with the clean
           row a dived band was built on -- and re-dive them with
           bin/E555_diver, several copies; keep the best if strictly better.
           On polished boards this is the move that still gains (below)
  window   every h x w rectangle touching a break
  corner   a corner block with the full frame arms on both of its sides, so
           the frame's five border colours can re-thread round the corner
  band     the densest full-width rows (the whole dived top)
  frame    all 60 frame cells plus the damaged cells beside them

Corralling (--corral; on in deep and superdeep) is E555_topper.py's pull,
used as a plateau move: when a pass finds no strict gain, windows are
re-solved with the tie-break "breaks nearest the corner", and an equal-break
board with a smaller pull is accepted.  Breaks herded together are what a
later window can remove two at a time.  It is off in overnight because a
corral call must optimise the pull and so runs to its cap; on polished boards
it moved rarely (one plateau move in 900 board-seconds).

Several region models run at once (--jobs, default one per 4 threads), each
with its share of --threads: CP-SAT releases the GIL while it solves.

WHAT CHANGED FROM THE PREVIOUS ENDER, AND WHY

  * The old --search_mode improve demanded breaks <= current-1, which made
    the incumbent infeasible: CP-SAT had to find a solution from nothing and
    its LNS workers never started.  Measured on polished boards, every broad
    call of the old deep profile (89-103 open cells) ended UNKNOWN.
  * The old model (piece, spin and four colour integers under a table
    constraint, a reified equality per junction) has no useful relaxation;
    on the same regions the Boolean encoding found more gains in the same
    time and gave tighter bounds.
  * The Hamming cap and the collateral-break cap are gone by default (still
    available as --max_changes / --max_new_breaks): the floor already makes
    every accepted board no worse, and a coordinated re-arrangement is exactly
    what a small Hamming radius forbids.

Input rows are read from their final 512 position/rotation fields, so legacy
leading metadata is accepted.  Output is canonical:
``config_id, score, pos[256], rot[256]`` with ``score = 480 - breaks``.
"""

from __future__ import annotations
import argparse, csv, collections, itertools, math, random, signal, sys, threading, time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
try:
    from ortools.sat.python import cp_model
    from ortools.graph.python import linear_sum_assignment
except ImportError:
    sys.exit("E555_ender.py needs OR-Tools, which is the one non-stdlib\n"
             "dependency in the toolkit:  pip install ortools")

_STOP = False
def _request_stop(signum, frame):
    global _STOP
    _STOP = True
    print("\n[Ctrl-C] finishing current call then stopping...", flush=True)

# ---------------------------------------------------------------------------
# geometry (identical semantics to the rest of the E555 toolkit)
# ---------------------------------------------------------------------------
SIDE, NUM_PIECES, NUM_EDGES, GREY, CSV_UNPLACED = 16, 256, 480, 0, 999
NORTH, EAST, SOUTH, WEST = 0, 1, 2, 3

CORNER_CELLS = frozenset({0, SIDE - 1, (SIDE - 1) * SIDE, SIDE * SIDE - 1})      # BL BR TL TR
BORDER_CELLS = frozenset(c for c in range(NUM_PIECES)
                         if c // SIDE in (0, SIDE - 1) or c % SIDE in (0, SIDE - 1))


def load_clues():
    """The clue table and its helpers, from tools/E555_viewer.py.

    That module is the toolkit's shared Python primitives (tools/E555_rank.py
    imports it the same way), and it holds the one copy of the Eternity II clue
    table -- the one datum where a second, independently typed copy could put a
    wrong piece on a board that still matches every edge. Imported lazily, so a
    run without --clue_center / --clue_corners has no dependency on tools/ and
    still works from a lone copy of this script.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
    try:
        import E555_viewer
    except ImportError:
        sys.exit("[ERROR] --clue_* needs tools/E555_viewer.py, which holds the "
                 "clue table; run this script from inside the E555 tree.")
    return E555_viewer

def rotate_edges(base, spin):
    return (base[(NORTH + spin) & 3], base[(EAST + spin) & 3],
            base[(SOUTH + spin) & 3], base[(WEST + spin) & 3])

def frame_rule_ok(r, c, oriented):
    n, e, s, w = oriented
    return ((n == GREY) == (r == SIDE - 1) and (e == GREY) == (c == SIDE - 1) and
            (s == GREY) == (r == 0) and (w == GREY) == (c == 0))

def piece_class(tile):
    zeros = sum(x == GREY for x in tile)
    return "corner" if zeros == 2 else "edge" if zeros == 1 else "inner"

def cell_class(cell):
    return ("corner" if cell in CORNER_CELLS else
            "edge" if cell in BORDER_CELLS else "inner")

def neighbour(cell, side):
    """The cell across `side` of `cell`, or -1 off the board."""
    r, c = divmod(cell, SIDE)
    if side == NORTH:
        return cell + SIDE if r + 1 < SIDE else -1
    if side == EAST:
        return cell + 1 if c + 1 < SIDE else -1
    if side == SOUTH:
        return cell - SIDE if r > 0 else -1
    return cell - 1 if c > 0 else -1

def read_seed(path):
    with open(path) as fh:
        return [tuple(int(x) for x in line.split()) for line in fh if len(line.split()) == 4]

@dataclass
class Partial:
    config_id: str; pos: list; rot: list

def parse_partial_line(fields):
    """Read a canonical row, taking pos/rot from the tail (tolerates extra
    leading columns, exactly like E555_topper.py / E555_rank.py)."""
    f = [x.strip() for x in fields]
    return Partial(f[0], [int(x) for x in f[-512:-256]], [int(x) for x in f[-256:]])

# Every interior junction, as (cell_a, cell_b, side_of_a, side_of_b), a < b.
ALL_JUNCTIONS = []
for _cell in range(NUM_PIECES):
    _r, _c = _cell // SIDE, _cell % SIDE
    if _c + 1 < SIDE: ALL_JUNCTIONS.append((_cell, _cell + 1, EAST, WEST))
    if _r + 1 < SIDE: ALL_JUNCTIONS.append((_cell, _cell + SIDE, NORTH, SOUTH))

def broken_junctions(pos, rot, edges):
    """Junctions that are not satisfied, walking all 480 of them.

    A junction with an UNPLACED cell on either side counts as broken -- the
    same rule E555_topper.py and tools/E555_rank.py use ("an unplaced neighbour
    leaves the piece unsatisfied")."""
    col = {pos[p]: rotate_edges(edges[p], rot[p])
           for p in range(NUM_PIECES) if pos[p] != CSV_UNPLACED}
    out = []
    for a, b, da, db in ALL_JUNCTIONS:
        if a in col and b in col and col[a][da] == col[b][db]:
            continue
        out.append((a, b))
    return out

def read_holes_file(path):
    values = []
    with open(path) as fh:
        for line in fh:
            if not line.strip().startswith("#"):
                values.extend(line.replace(",", " ").split())
    return {i for i, v in enumerate(values) if int(v) == 1}

def clue_open_cells(CL, mask, orient, pos, rot):
    """Cells a region must open so the wrong clues of `orient` can be fixed.

    A clue already at its cell and spin contributes nothing. The donor cell is
    only offered when it is an interior cell, which it always is on a
    well-formed board (every clue piece is an inner piece); the guard just
    stops a malformed input from pinning an inner piece into a border
    commodity, where the pin would be infeasible.
    """
    out = set()
    for cell, piece, spin in CL.clue_list(orient, mask):
        if pos[piece] == cell and rot[piece] == spin:
            continue
        out.add(cell)
        if pos[piece] != CSV_UNPLACED and pos[piece] not in BORDER_CELLS:
            out.add(pos[piece])
    return out

def board_quality(pos, rot, edges):
    """(breaks, clean-foundation and compactness figures) for reporting and
    for the clean-foundation guard."""
    broken = broken_junctions(pos, rot, edges)
    if not broken:
        return (0, -SIDE, -4 * SIDE, 0, 0, 0, 0), {
            "breaks": 0, "max_clean": SIDE, "clean_sum": 4 * SIDE,
            "clean": (SIDE, SIDE, SIDE, SIDE), "break_cells": 0}
    cells = {c for pair in broken for c in pair}
    rows = {c // SIDE for c in cells}
    cols = {c % SIDE for c in cells}

    def leading(order, bad):
        n = 0
        for x in order:
            if x in bad:
                break
            n += 1
        return n

    clean = (leading(range(SIDE), rows),
             leading(range(SIDE - 1, -1, -1), rows),
             leading(range(SIDE), cols),
             leading(range(SIDE - 1, -1, -1), cols))
    q = (len(broken), -max(clean), -sum(clean), len(rows) + len(cols), len(cells))
    return q, {"breaks": len(broken), "max_clean": max(clean),
               "clean_sum": sum(clean), "clean": clean, "break_cells": len(cells)}

def board_fingerprint(pos, rot):
    """Stable hashable board identity for duplicate-input detection."""
    return tuple(pos) + tuple(rot)

def draw_pool(free, break_cells, title="open region"):
    """ASCII map of an open region, row 15 (top) first, matching the viewer."""
    print(f"      >> {title}: {len(free)} cells")
    for r in range(SIDE - 1, -1, -1):
        row = []
        for c in range(SIDE):
            cell = r * SIDE + c
            row.append("#" if cell in free and cell in BORDER_CELLS
                       else "&" if cell in free
                       else "x" if cell in break_cells else ".")
        print("      " + " ".join(row))
    print("      (# frame open, & inner open, x break-locked, . locked)\n")

# ---------------------------------------------------------------------------
# board state as a cell map, and the corralling potential
# ---------------------------------------------------------------------------
def at_of(pos, rot):
    """cell -> (piece, spin), None for an empty cell."""
    at = [None] * NUM_PIECES
    for p in range(NUM_PIECES):
        if pos[p] != CSV_UNPLACED:
            at[pos[p]] = (p, rot[p])
    return at

def posrot_of(at):
    pos, rot = [CSV_UNPLACED] * NUM_PIECES, [0] * NUM_PIECES
    for cell, ps in enumerate(at):
        if ps is not None:
            pos[ps[0]], rot[ps[0]] = cell, ps[1]
    return pos, rot

def cell_colours(tiles, at):
    return [None if ps is None else rotate_edges(tiles[ps[0]], ps[1]) for ps in at]

def broken_of(tiles, at):
    col = cell_colours(tiles, at)
    return [(a, b) for a, b, da, db in ALL_JUNCTIONS
            if col[a] is None or col[b] is None or col[a][da] != col[b][db]]

# The topper's pull: an unavoidable break is cheapest on the nearest
# horizontal border, and then along it at the nearest corner.
def _v(cell):
    r = cell // SIDE
    return min(r, SIDE - 1 - r)

def _h(cell):
    c = cell % SIDE
    return min(c, SIDE - 1 - c)

MIN_CALL_SECONDS = 2.0                      # floor under a scaled-down call cap
HEARTBEAT_SECONDS = 120.0                   # progress line on a long, quiet board


def clock(seconds):
    """Elapsed time as m:ss, or h:mm:ss from an hour on."""
    s = int(round(seconds))
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


def duration(seconds):
    """A budget in the unit a person would use."""
    if seconds < 90:
        return f"{seconds:.0f} s"
    if seconds < 5400:
        return f"{seconds / 60:.1f} min"
    return f"{seconds / 3600:.1f} h"

CORRAL_MAX = 2 * (SIDE // 2 - 1)            # 14: both cells on the centre line
CORRAL_ROW_W = CORRAL_MAX + 1               # a row step outweighs any sideways sum

def corral_cost(a, b):
    return CORRAL_ROW_W * (_v(a) + _v(b)) + (_h(a) + _h(b))

def corral_potential(pairs):
    return sum(corral_cost(a, b) for a, b in pairs)

# ---------------------------------------------------------------------------
# the exact region model: Heule's Boolean encoding, incumbent kept feasible
# ---------------------------------------------------------------------------
class RegionModel:
    """One open region of a board as a CP-SAT model.

    The pieces on the region's cells may permute (within their class: corner,
    edge, inner) and re-rotate; every other cell is locked.  Encoding, after
    M. Heule, "Solving edge-matching problems with satisfiability solvers"
    (2008), whose measurements single out the explicit one-on-one mapping as
    decisive for a complete solver:

      x[c,p,s]  piece p on cell c with spin s (class and frame rule applied
                while building, so an illegal placement never has a literal)
      exactly one x per cell, and exactly one x per piece
      L[c,d,k]  colour k shows on side d of cell c: the sum of the x that
                show it, so it is 0/1 and a colour fixes every x at once
      m[j,k]    junction j matched in colour k (implies both sides' L);
                a junction onto a locked cell is just one L literal

    The previous encoding (piece, spin and four colour integers per cell under
    a table constraint, a reified equality per junction) has no usable LP and
    propagates colour through integer bounds; this one gives CP-SAT literals
    to learn on and a relaxation with real bounds.

    ``floor_breaks`` keeps the incumbent board feasible (breaks <= current),
    so CP-SAT's LNS workers start from it instead of from nothing -- the old
    ``improve`` mode demanded breaks <= current-1, which made the incumbent
    infeasible and left every LNS worker idle until a first solution existed.
    """

    def __init__(self, tiles, at, region, *, pins=(), protect=None,
                 reference_broken=None, max_new_breaks=None,
                 max_changes=None, corral=False):
        self.tiles, self.at = tiles, at
        self.region = sorted(c for c in region if at[c] is not None)
        m = self.model = cp_model.CpModel()
        self.empty = self.conflict = False
        rs = set(self.region)
        if len(rs) < 2:
            self.empty = True
            return
        by_class = collections.defaultdict(list)
        for c in self.region:
            by_class[cell_class(c)].append(at[c][0])
        self.x = x = {}
        cell_lits = collections.defaultdict(list)
        piece_lits = collections.defaultdict(list)
        colterms = {c: [collections.defaultdict(list) for _ in range(4)]
                    for c in self.region}
        keep = []
        for c in self.region:
            r, k = divmod(c, SIDE)
            for p in by_class[cell_class(c)]:
                for s in range(4):
                    o = rotate_edges(tiles[p], s)
                    if not frame_rule_ok(r, k, o):
                        continue
                    v = m.NewBoolVar(f"x{c}_{p}_{s}")
                    x[c, p, s] = v
                    cell_lits[c].append(v)
                    piece_lits[p].append(v)
                    for d in range(4):
                        if neighbour(c, d) >= 0:
                            colterms[c][d][o[d]].append(v)
                    if (p, s) == tuple(at[c]):
                        keep.append(v)
        if any(not cell_lits[c] for c in self.region):
            self.empty = True
            return
        for c in self.region:
            m.AddExactlyOne(cell_lits[c])
        for lits in piece_lits.values():
            m.AddExactlyOne(lits)
        for cell, piece, spin in pins:
            lit = x.get((cell, piece, spin))
            if lit is None:
                self.conflict = True
                return
            m.Add(lit == 1)
        L = {}
        for c in self.region:
            for d in range(4):
                for k, lits in colterms[c][d].items():
                    if len(lits) == 1:
                        L[c, d, k] = lits[0]
                    else:
                        v = m.NewBoolVar(f"L{c}_{d}_{k}")
                        m.Add(v == sum(lits))
                        L[c, d, k] = v
        col = cell_colours(tiles, at)
        protect = protect or (lambda a, b: False)
        ref = set(reference_broken) if reference_broken is not None else None
        self.const_match = 0
        self.juncs = []                  # (a, b, [match literals]), variable junctions
        new_terms, const_new = [], 0
        for a, b, da, db in ALL_JUNCTIONS:
            ina, inb = a in rs, b in rs
            if not ina and not inb:
                ok = (col[a] is not None and col[b] is not None
                      and col[a][da] == col[b][db])
                self.const_match += ok
                if not ok:
                    if protect(a, b):
                        self.conflict = True
                    if ref is not None and (a, b) not in ref:
                        const_new += 1
                continue
            if ina and inb:
                terms = []
                for k in set(colterms[a][da]) & set(colterms[b][db]):
                    mv = m.NewBoolVar(f"m{a}_{b}_{k}")
                    m.AddImplication(mv, L[a, da, k])
                    m.AddImplication(mv, L[b, db, k])
                    terms.append(mv)
            else:
                inside, iside, other, oside = ((a, da, b, db) if ina else (b, db, a, da))
                if col[other] is None:
                    terms = []                     # an empty neighbour never matches
                else:
                    lit = L.get((inside, iside, col[other][oside]))
                    terms = [lit] if lit is not None else []
            if protect(a, b):
                if not terms:
                    self.conflict = True
                else:
                    m.AddBoolOr(terms)
            self.juncs.append((a, b, terms))
            if ref is not None and (a, b) not in ref:
                new_terms.append(terms)
        if self.conflict:
            return
        if max_new_breaks is not None and max_new_breaks >= 0 and ref is not None:
            if const_new > max_new_breaks:
                self.conflict = True
                return
            if new_terms:
                m.Add(sum(1 - sum(t) for t in new_terms) <= max_new_breaks - const_new)
        if max_changes is not None:
            m.Add(len(self.region) - sum(keep) <= max_changes)
        self.match_expr = self.const_match + sum(sum(t) for _a, _b, t in self.juncs)
        self.corral = corral
        if corral:
            # Lexicographic by dominating weights: matches first, then the
            # corralling pull.  Sized from this model's junctions (as the
            # topper does), not from all 480.
            w = max(1, len(self.juncs)) * (CORRAL_ROW_W * CORRAL_MAX + CORRAL_MAX) + 1
            self.objective = sum((w + corral_cost(a, b)) * sum(t)
                                 for a, b, t in self.juncs if t)
        else:
            self.objective = self.match_expr
        for (c, p, s), v in x.items():
            m.AddHint(v, 1 if (p, s) == tuple(at[c]) else 0)
        self.solver = None

    def solve(self, seconds, workers, seed, *, floor_breaks=None, stop_below=None,
              first_solution=False, log=False, params=None):
        """Maximize the objective.  ``floor_breaks`` bounds the breaks from above
        (the incumbent's own count keeps the hint feasible); ``stop_below``
        ends the call at the first board with fewer breaks than that.
        Returns (new cell map or None, status name, wall seconds)."""
        m = self.model
        if floor_breaks is not None:
            m.Add(self.match_expr >= NUM_EDGES - floor_breaks)
        m.Maximize(self.objective)
        solver = self.solver = cp_model.CpSolver()
        if hasattr(solver.parameters, "num_workers"):
            solver.parameters.num_workers = workers
        else:
            solver.parameters.num_search_workers = workers
        solver.parameters.max_time_in_seconds = max(0.05, float(seconds))
        solver.parameters.random_seed = seed & 0x7fffffff
        if first_solution:
            solver.parameters.stop_after_first_solution = True
        for name, value in (params or {}).items():
            if value is not None:
                setattr(solver.parameters, name, value)
        if log:
            solver.parameters.log_search_progress = True
        const, juncs = self.const_match, self.juncs

        class _Stop(cp_model.CpSolverSolutionCallback):
            def __init__(self):
                cp_model.CpSolverSolutionCallback.__init__(self)

            def on_solution_callback(self):
                if stop_below is None:
                    return
                mm = const + sum(self.Value(v) for _a, _b, t in juncs for v in t)
                if NUM_EDGES - mm < stop_below:
                    self.StopSearch()

        t0 = time.monotonic()
        status = solver.Solve(m, _Stop())
        wall = time.monotonic() - t0
        name = solver.StatusName(status)
        if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
            return None, name, wall
        out = list(self.at)
        for (c, p, s), v in self.x.items():
            if solver.Value(v):
                out[c] = (p, s)
        return out, name, wall

    def stop(self):
        if self.solver is not None:
            self.solver.StopSearch()


# ---------------------------------------------------------------------------
# swap: exact re-assignment over an independent set (linear assignment)
# ---------------------------------------------------------------------------
def swap_move(tiles, at, cells, protect=None):
    """Best permutation of the pieces on `cells`, pairwise non-adjacent.

    With no two cells adjacent, every cell's matches depend only on its own
    piece and spin, so the best permutation is a linear assignment problem,
    solved exactly in polynomial time -- every exchange cycle, of any length,
    between any cells of the set, including the clean foundation.  Returns
    (gain, new cell map)."""
    col = cell_colours(tiles, at)
    protect = protect or (lambda a, b: False)
    new = list(at)
    gain = 0

    def fit(c, o):
        n, legal = 0, True
        for d in range(4):
            y = neighbour(c, d)
            if y < 0 or col[y] is None:
                continue
            ok = o[d] == col[y][(d + 2) & 3]
            n += ok
            if not ok and protect(min(c, y), max(c, y)):
                legal = False
        return n, legal

    for cls in ("inner", "edge", "corner"):
        group = [c for c in cells if at[c] is not None and cell_class(c) == cls]
        if len(group) < 2:
            continue
        pieces = [at[c][0] for c in group]
        before = sum(fit(c, col[c])[0] for c in group)
        lsa = linear_sum_assignment.SimpleLinearSumAssignment()
        best = {}
        for i, p in enumerate(pieces):
            for j, c in enumerate(group):
                r, k = divmod(c, SIDE)
                bm, bs = -1, None
                for s in range(4):
                    o = rotate_edges(tiles[p], s)
                    if not frame_rule_ok(r, k, o):
                        continue
                    f, legal = fit(c, o)
                    if legal and f > bm:
                        bm, bs = f, s
                if bs is not None:
                    best[i, j] = (bm, bs)
                    lsa.add_arc_with_cost(i, j, 4 - bm)
        if lsa.solve() != lsa.OPTIMAL:
            continue
        after = 0
        placed = []
        for i in range(len(pieces)):
            j = lsa.right_mate(i)
            bm, bs = best[i, j]
            after += bm
            placed.append((group[j], pieces[i], bs))
        if after > before:
            for c, p, s in placed:
                new[c] = (p, s)
            gain += after - before
    return gain, new


def independent_set(rng, first, allowed):
    """A maximal set of pairwise non-adjacent cells from `allowed`, taking the
    cells of `first` (in random order) before the rest."""
    head = [c for c in first if c in allowed]
    rest = [c for c in allowed if c not in first]
    rng.shuffle(head)
    rng.shuffle(rest)
    out, blocked = [], set()
    for c in head + rest:
        if c in blocked:
            continue
        out.append(c)
        blocked.add(c)
        for d in range(4):
            y = neighbour(c, d)
            if y >= 0:
                blocked.add(y)
    return out


# ---------------------------------------------------------------------------
# neighbourhoods
# ---------------------------------------------------------------------------
FRAME_TOP = frozenset((SIDE - 1) * SIDE + c for c in range(SIDE))
FRAME_BOTTOM = frozenset(range(SIDE))
FRAME_LEFT = frozenset(r * SIDE for r in range(SIDE))
FRAME_RIGHT = frozenset(r * SIDE + SIDE - 1 for r in range(SIDE))

def windows(break_cells, h, w, allowed, rng):
    """Every h x w rectangle (and w x h) touching a break, densest first."""
    shapes = {(h, w), (w, h)}
    out = {}
    for hh, ww in shapes:
        if hh > SIDE or ww > SIDE:
            continue
        for r0 in range(SIDE - hh + 1):
            for c0 in range(SIDE - ww + 1):
                cells = frozenset(r * SIDE + c for r in range(r0, r0 + hh)
                                  for c in range(c0, c0 + ww)) & allowed
                hits = len(cells & break_cells)
                if hits and len(cells) >= 2:
                    out[cells] = hits
    keys = list(out)
    rng.shuffle(keys)
    keys.sort(key=lambda k: -out[k])
    return keys

def densest_band(break_cells, k, allowed):
    """The k consecutive full rows (or columns) holding the most damaged cells."""
    best = None
    for horizontal in (True, False):
        for lo in range(SIDE - k + 1):
            cells = frozenset((r * SIDE + c) for r in range(SIDE) for c in range(SIDE)
                              if lo <= (r if horizontal else c) < lo + k) & allowed
            hits = len(cells & break_cells)
            if hits and (best is None or hits > best[0]):
                best = (hits, cells)
    return [] if best is None else [best[1]]

def corner_regions(break_cells, d, allowed):
    """Each corner d x d block holding a break, plus the full frame arms on
    both of its sides, so the frame can re-thread around the corner."""
    out = []
    for corner, arms in ((0, FRAME_BOTTOM | FRAME_LEFT),
                         (SIDE - 1, FRAME_BOTTOM | FRAME_RIGHT),
                         ((SIDE - 1) * SIDE, FRAME_TOP | FRAME_LEFT),
                         (SIDE * SIDE - 1, FRAME_TOP | FRAME_RIGHT)):
        r0, c0 = divmod(corner, SIDE)
        rows = range(0, d) if r0 == 0 else range(SIDE - d, SIDE)
        cols = range(0, d) if c0 == 0 else range(SIDE - d, SIDE)
        block = frozenset(r * SIDE + c for r in rows for c in cols)
        if block & break_cells:
            out.append((block | arms) & allowed)
    return out

def frame_region(break_cells, allowed):
    """All 60 frame cells, plus the damaged cells of the inner ring."""
    ring = {c for c in break_cells
            if (c // SIDE) in (1, SIDE - 2) or (c % SIDE) in (1, SIDE - 2)}
    cells = (BORDER_CELLS | ring) & allowed
    return [frozenset(cells)] if cells & break_cells else []


REDIVE_COVER = 0.9          # a redive band holds this share of its side's damage

def border_band(break_cells, extra, cap):
    """The outer rows (or columns) of the side nearest most of the damage --
    the shape a dive fills -- deep enough to hold REDIVE_COVER of the damaged
    cells on that side's half of the board, plus `extra` rows, at most `cap`.
    None if there is no damage.

    Measured on dived-and-polished boards (damage in rows 12-15, the odd
    damaged cell on row 11): reopening rows 12-15 gained an edge on 4 of 5
    seeds on one board and 3 of 5 on another, rows 11-15 on 2 of 5 and 0 of 5
    -- more open cells is not more search, so one stray deep break must not
    deepen the band."""
    if not break_cells:
        return None
    best = None
    for side in ("T", "B", "L", "R"):
        def dist(c):
            r, k = divmod(c, SIDE)
            return {"T": SIDE - 1 - r, "B": r, "L": k, "R": SIDE - 1 - k}[side]
        near = sorted(dist(c) for c in break_cells if dist(c) < SIDE // 2)
        if not near:
            continue
        need = max(1, math.ceil(len(near) * REDIVE_COVER))
        depth = min(cap, near[need - 1] + 1 + extra)
        cells = frozenset(c for c in range(NUM_PIECES) if dist(c) < depth)
        hits = len(cells & break_cells)
        if best is None or hits > best[0]:
            best = (hits, cells)
    return None if best is None else best[1]


DIVER_OVERRIDE = None                       # --diver PATH
_REDIVE_WARNED = False                      # a failing diver is reported once

def diver_path():
    """The E555_diver binary: --diver, else bin/ of this script's repository."""
    path = (Path(DIVER_OVERRIDE) if DIVER_OVERRIDE
            else Path(__file__).resolve().parents[2] / "bin" / "E555_diver")
    return path if path.is_file() else None


def redive(seed_file, tiles, at, cells, *, copies, dives, polish, threads, rng_seed,
           keep=(), wall=None, plateau=False, prior=0.0, nogo=0.0):
    """Re-dive `cells` of the complete board `at` with E555_diver --reopen: the
    cells (minus `keep`, the clues) go to the diver as a mask, which lifts
    them and dives them in `copies` copies a round (each on random streams of
    its own, --end_dive/--end_polish per copy), round after round from the
    best board so far until `wall` seconds are spent (one round without a
    wall).  The diver never returns a worse board; `plateau` lets its rounds
    move to a different board of the same score.  Returns the finished
    board's cell map, or None.

    This is the dive engine as a large-neighbourhood move.  On boards whose top
    rows were dived and polished, re-diving the SAME rows with fresh seeds
    gained nothing (0 of 8 boards, 5 seeds each), while re-diving them together
    with the clean row beneath gained an edge on 2 of 8: the foundation's last
    row was fixed when the dive chose the top's pieces, and freeing it gives
    the dive different pieces to work with.  CP-SAT on the same 64 cells found
    nothing in 120 s; a dive is the stronger recreate at this size, CP-SAT the
    stronger exact search on a window."""
    global _REDIVE_WARNED
    import os, subprocess, tempfile
    exe = diver_path()
    if exe is None:
        return None
    lift = {c for c in cells if c not in keep and at[c] is not None}
    if not lift or any(v is None for v in at):
        return None
    pos, rot = posrot_of(at)
    with tempfile.TemporaryDirectory(prefix="ender_redive_") as tmp:
        src, dst = os.path.join(tmp, "in.csv"), os.path.join(tmp, "out.csv")
        mask = os.path.join(tmp, "reopen.csv")
        with open(src, "w", newline="") as fh:
            csv.writer(fh, lineterminator="\n").writerow(["redive", 0] + pos + rot)
        with open(mask, "w") as fh:            # the --holes format: row 0 first
            for r in range(SIDE):
                fh.write(",".join("1" if r * SIDE + c in lift else "0"
                                  for c in range(SIDE)) + "\n")
        # --copies, not K rows of one board: the diver keys a board's random
        # streams on its content, so K identical rows would dive identically.
        cmd = [str(exe), seed_file, src, dst, "--reopen", mask, "--copies", str(copies),
               "--end_dive", str(dives), "--end_polish", str(polish), "--emit_score", "0",
               "--threads", str(threads), "--rng_seed", str(rng_seed)]
        if wall is not None and wall != float("inf"):
            # rounds until the wall: the engine stops its dives and polish
            # there, so the overrun is a fraction of a second
            cmd += ["--rounds", "1000000", "--wall_time", str(max(1, int(wall)))]
        if plateau:
            cmd += ["--plateau"]
        if prior > 0:
            cmd += ["--prior", f"{prior:g}"]
        if nogo > 0:
            cmd += ["--nogo", f"{nogo:g}"]
        try:
            subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           check=True, timeout=None if wall is None else wall + 600)
        except subprocess.CalledProcessError as e:
            if not _REDIVE_WARNED:
                tail = (e.stderr or b"").decode(errors="replace").strip().splitlines()[-1:]
                print(f"[warn] redive: {exe} failed ({'; '.join(tail) or e.returncode}); "
                      f"an old binary? rebuild it with `make diver`", flush=True)
                _REDIVE_WARNED = True
            return None
        except (subprocess.SubprocessError, OSError):
            return None
        best = None
        if not os.path.exists(dst):
            return None
        with open(dst, newline="") as fh:
            for row in csv.reader(fh):
                if not row or row[0].lstrip().startswith(("#", "%")):
                    continue
                cand = parse_partial_line(row)
                if CSV_UNPLACED in cand.pos:
                    continue
                cat = at_of(cand.pos, cand.rot)
                nb = len(broken_of(tiles, cat))
                if best is None or nb < best[0]:
                    best = (nb, cat)
        return None if best is None else best[1]


# ---------------------------------------------------------------------------
# effort profiles
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Step:
    """One neighbourhood family.  ``seconds`` caps each CP-SAT call (scaled
    with a user-set board budget); a call ends sooner on a proof or a gain."""
    kind: str                  # swap | redive | window | corner | band | frame
    h: int = 0                 # window height / band depth / corner block size;
                               # redive: extra clean rows beyond the damage
    w: int = 0                 # window width; redive: cap on the band depth
    seconds: float = 10.0      # redive: the diver's wall clock per call
    corral: bool = False
    sets: int = 0              # swap: independent sets per visit; redive: copies
    dives: int = 0             # redive: --end_dive per copy
    polish: int = 0            # redive: --end_polish per copy
    plateau: bool = False      # redive: --plateau
    prior: float = 0.0         # redive: --prior (pull toward the incumbent's clean placements)
    nogo: float = 0.0          # redive: --nogo (push off its broken ones)


def step_name(step):
    """A step's short label: swap, redive, redive+1, window4x6, corner5,
    band4, frame, corral-window5x6."""
    if step.kind == "window":
        name = f"window{step.h}x{step.w}"
    elif step.kind == "redive":
        name = "redive" + (f"+{step.h}" if step.h else "")
    else:
        name = f"{step.kind}{step.h or ''}"
    return ("corral-" if step.corral else "") + name


@dataclass(frozen=True)
class EffortProfile:
    board_seconds: float
    workers: int
    plan: tuple
    corral: bool                # run the plan's corral steps (--corral overrides)
    max_clean_loss: int
    duplicate_policy: str


# The redive steps run 8 copies (16 in superdeep's first) of 1000 dives and
# 10000 kick-and-polish rounds.  On 24 dived-and-polished boards (459-461), the
# whole overnight plan at 30 s a board gained +0.96 edges per board (21 of 24
# improved) against +0.50 (12) with the earlier 3000/1000 and --prior 1
# --nogo 1: 11 boards better, none worse.
EFFORT_PROFILES = {
    "overnight": EffortProfile(
        board_seconds=180.0, workers=4,
        plan=(Step("swap", sets=24),
              Step("redive", 0, 5, 45.0, sets=8, dives=1000, polish=10000),
              Step("window", 3, 5, 4.0),
              Step("window", 4, 5, 8.0),
              Step("window", 4, 7, 15.0),
              Step("band", 3, 0, 30.0),
              Step("window", 4, 6, 8.0, corral=True)),
        corral=False, max_clean_loss=1, duplicate_policy="reuse"),
    "deep": EffortProfile(
        board_seconds=900.0, workers=8,
        plan=(Step("swap", sets=48),
              Step("redive", 0, 5, 90.0, sets=8, dives=1000, polish=10000),
              Step("window", 4, 4, 5.0),
              Step("window", 4, 6, 12.0),
              Step("window", 5, 6, 20.0),
              Step("corner", 5, 0, 45.0),
              Step("redive", 0, 5, 90.0, sets=8, dives=1000, polish=10000),
              Step("window", 5, 8, 40.0),
              Step("band", 4, 0, 120.0),
              Step("frame", 0, 0, 90.0),
              Step("window", 5, 6, 15.0, corral=True)),
        corral=True, max_clean_loss=1, duplicate_policy="rerun"),
    "superdeep": EffortProfile(
        board_seconds=7200.0, workers=12,
        plan=(Step("swap", sets=96),
              Step("redive", 0, 5, 600.0, sets=16, dives=1000, polish=10000),
              Step("window", 4, 4, 8.0),
              Step("window", 4, 6, 20.0),
              Step("window", 5, 6, 40.0),
              Step("corner", 6, 0, 120.0),
              Step("redive", 1, 6, 300.0, sets=8, dives=1000, polish=10000),
              Step("window", 5, 8, 90.0),
              Step("window", 6, 8, 180.0),
              Step("band", 5, 0, 600.0),
              Step("frame", 0, 0, 300.0),
              Step("window", 5, 8, 30.0, corral=True)),
        corral=True, max_clean_loss=1, duplicate_policy="rerun"),
}


# ---------------------------------------------------------------------------
# one board
# ---------------------------------------------------------------------------
def region_key(at, cells, corral, pins):
    """A region's identity for the proof cache: its cells, everything on and
    around them, and whatever else the model depends on."""
    around = set(cells)
    for c in cells:
        for d in range(4):
            y = neighbour(c, d)
            if y >= 0:
                around.add(y)
    return (frozenset(cells), tuple((c, at[c]) for c in sorted(around)),
            corral, tuple(sorted(pins)))


def solve_board(partial, tiles, policy, workers, base_seed, verbose, *,
                holes=None, CL=None, clue_mask=None, orient=None):
    """Run the neighbourhood portfolio on one board; never returns worse."""
    pos, rot = list(partial.pos), list(partial.rot)
    at = at_of(pos, rot)
    placed = frozenset(c for c in range(NUM_PIECES) if at[c] is not None)
    allowed = placed if holes is None else frozenset(holes) & placed
    rng = random.Random(base_seed)
    stats = collections.Counter()
    stage_tag, reason = "-", "exhausted"
    t_start = time.monotonic()
    board_time = policy["board_time"]
    deadline = None if board_time <= 0 else t_start + board_time
    prof = policy["profile"]
    scale = (board_time / prof.board_seconds
             if board_time > 0 and prof.board_seconds > 0 else 1.0)
    jobs = max(1, policy["jobs"])
    search_mode = policy["search_mode"]
    corral_on = policy["corral"]
    plan = list(policy["plan"])
    if holes is not None:
        # --holes names the one region the caller wants searched: solve the
        # whole mask as one model right after the cheap exchange moves, and
        # clip every other neighbourhood to it.
        plan.insert(1 if plan and plan[0].kind == "swap" else 0,
                    Step("mask", seconds=max(s.seconds for s in plan)))

    def remaining():
        return float("inf") if deadline is None else deadline - time.monotonic()

    last_note = [t_start]

    def note(text):
        """One progress line, stamped with the time into this board."""
        now = time.monotonic()
        print(f"      {clock(now - t_start):>7}  {text}", flush=True)
        last_note[0] = now

    # ---- the clean-foundation guard, as hard model constraints -------------
    q0, info0 = board_quality(pos, rot, tiles)
    baseline_broken = set(broken_junctions(pos, rot, tiles))
    clean0 = info0["clean"]
    names = ("B", "T", "L", "R")
    requirements = []
    if policy["preserve_clean"]:
        loss = policy["max_clean_loss"]
        side = policy["preserve_side"]
        if side == "all":
            requirements = [(n, max(0, clean0[i] - loss)) for i, n in enumerate(names)]
        elif side in names:
            requirements = [(side, max(0, clean0[names.index(side)] - loss))]
        elif side == "auto":
            i = max(range(4), key=lambda i: clean0[i])
            requirements = [(names[i], max(0, clean0[i] - loss))]
        # "any" is checked after the solve only

    def protect(a, b):
        ar, ac = divmod(a, SIDE)
        br, bc = divmod(b, SIDE)
        for side, depth in requirements:
            if depth <= 0:
                continue
            if side == "B" and min(ar, br) < depth: return True
            if side == "T" and max(ar, br) >= SIDE - depth: return True
            if side == "L" and min(ac, bc) < depth: return True
            if side == "R" and max(ac, bc) >= SIDE - depth: return True
        return False

    def guard_ok(p, r):
        if not policy["preserve_clean"] or policy["preserve_side"] != "any":
            return True
        _q, info = board_quality(p, r, tiles)
        return max(info["clean"]) >= max(clean0) - policy["max_clean_loss"]

    if verbose and requirements:
        note("guard: clean foundation kept: "
             + ", ".join(f"{s}>={d}" for s, d in requirements))

    clues_on = bool(clue_mask and orient is not None)

    def clue_hits(a):
        if not clues_on:
            return 0
        return sum(1 for cell, piece, spin in CL.clue_list(orient, clue_mask)
                   if a[cell] == (piece, spin))

    def pins_for(a, cells):
        if not clues_on:
            return ()
        p, r = posrot_of(a)
        pins, _ok, _skip = CL.clue_pins(p, r, set(cells), orient, clue_mask,
                                        unplaced_ok=False)
        return tuple(pins)

    breaks = len(broken_of(tiles, at))
    phi = corral_potential(broken_of(tiles, at))
    hits = clue_hits(at)
    n_clues = len(CL.clue_list(orient, clue_mask)) if clues_on else 0
    if breaks == 0 and hits == n_clues:
        return pos, rot, 0, "input-solved", "-", 0.0, stats

    proven = set()
    ref = baseline_broken if policy["max_new_breaks"] is not None else None

    def accept(new, tag, corral):
        """Apply a solver result if it is better; return 'strict', 'plateau'
        or None."""
        nonlocal at, breaks, phi, hits, stage_tag
        new_broken = broken_of(tiles, new)
        nb = len(new_broken)
        nh = clue_hits(new)
        if nh < hits or nb > breaks:
            return None
        # The models hold the guard as hard constraints; a redive does not, so
        # every candidate is checked here as well.
        if any(protect(a, b) for a, b in new_broken):
            return None
        p, r = posrot_of(new)
        if not guard_ok(p, r):
            return None
        nphi = corral_potential(new_broken)
        if nb < breaks or nh > hits:
            kind = "strict"
        elif corral and nphi < phi:
            kind = "plateau"
        else:
            return None
        note(f"{tag:<16} {NUM_EDGES - breaks} -> {NUM_EDGES - nb}"
             + (f"  (same score, breaks pulled together: {phi} -> {nphi})"
                if kind == "plateau" else "")
             + (f"  (clues in place {hits} -> {nh})" if nh != hits else ""))
        at, breaks, phi, hits = new, nb, nphi, nh
        stage_tag = tag
        stats[kind] += 1
        return kind

    def call_cap(step):
        # A shorter board budget shortens every call, but not below the couple
        # of seconds a small window needs for its proof: a call cut off before
        # it can prove anything is pure waste.
        if policy["attempt_override"] is not None:
            cap = policy["attempt_override"]
        else:
            cap = max(MIN_CALL_SECONDS, step.seconds * scale)
        return min(cap, remaining())

    def run_regions(regions, step, label, floor=None):
        """Solve regions on the current board, up to `jobs` models at once,
        sharing the threads.  The first strictly better board stops the rest
        of its batch.  Returns ('strict' | 'plateau' | None, conclusive), where
        conclusive means every call ended in a proof or a gain."""
        corral = step.corral
        todo = []
        for cells in regions:
            pins = pins_for(at, cells)
            key = region_key(at, cells, corral, pins)
            if key in proven:
                stats["cached"] += 1
                continue
            todo.append((cells, pins, key))
        if not todo:
            return None, True
        conclusive = True
        snapshot, floor = at, (breaks if floor is None else floor)
        stop_below = breaks if (search_mode == "improve" and not corral) else None
        idx = 0
        while idx < len(todo):
            if _STOP or remaining() <= 0:
                return None, False
            batch = todo[idx: idx + jobs]
            idx += len(batch)
            cap = call_cap(step)
            if cap <= 0:
                return None, False
            share = max(1, workers // len(batch))
            built = []
            for item in batch:              # model building is Python: keep it here
                cells, pins, key = item
                rm = RegionModel(tiles, snapshot, cells, pins=pins, protect=protect,
                                 reference_broken=ref,
                                 max_new_breaks=policy["max_new_breaks"],
                                 max_changes=policy["max_changes"], corral=corral)
                if rm.empty or rm.conflict:
                    stats["skipped"] += 1
                    continue
                built.append((item, rm, rng.randrange(1 << 30)))
            if not built:
                continue
            found = threading.Event()

            def work(entry):
                item, rm, seed = entry
                out, st, wall = rm.solve(
                    cap, share, seed, floor_breaks=floor, stop_below=stop_below,
                    log=policy["log_search"],
                    params={"symmetry_level": policy["symmetry_level"],
                            "linearization_level": policy["linearization_level"]})
                if out is not None and stop_below is not None \
                        and len(broken_of(tiles, out)) < stop_below:
                    found.set()
                    for _i, other, _s in built:      # CP-SAT releases the GIL, so
                        if other is not rm:          # the others are still running
                            other.stop()
                return item, out, st, wall

            if len(built) == 1:
                results = [work(built[0])]
            else:
                with ThreadPoolExecutor(max_workers=len(built)) as ex:
                    results = list(ex.map(work, built))
            best = None
            for (cells, pins, key), out, st, wall in results:
                stats["calls"] += 1
                stats[st.lower()] += 1
                if verbose:
                    said = {"OPTIMAL": "proved optimal", "INFEASIBLE": "proved: no better",
                            "FEASIBLE": "no gain by the cap", "UNKNOWN": "no answer by the cap"}
                    if out is not None and len(broken_of(tiles, out)) < breaks:
                        said = {st: "better board"}
                    note(f"  {label:<14} {len(cells):3d} cells  {said.get(st, st.lower()):<20}"
                         f" {wall:5.1f} s")
                if out is None:
                    if st == "INFEASIBLE":
                        proven.add(key)          # a proof as well: nothing that good
                    elif not found.is_set():
                        conclusive = False
                    continue
                nb = len(broken_of(tiles, out))
                nphi = corral_potential(broken_of(tiles, out))
                if st == "OPTIMAL" and nb >= breaks and not (corral and nphi < phi):
                    proven.add(key)
                elif nb >= breaks and not found.is_set():
                    conclusive = False
                rank = (nb, -clue_hits(out), nphi)
                if best is None or rank < best[0]:
                    best = (rank, out, cells)
            if best is not None:
                kind = accept(best[1], label, corral)
                if kind:
                    if verbose > 1:
                        draw_pool(best[2], {c for pr in broken_of(tiles, at) for c in pr})
                    return kind, conclusive
        return None, conclusive

    def repair_clues():
        """A displaced clue first: open its cell, its piece's holder and a
        growing halo round both, and allow no extra break for the repair."""
        p, r = posrot_of(at)
        need = clue_open_cells(CL, clue_mask, orient, p, r)
        cells = set(need)
        for _grow in range(3):
            cells |= {y for c in cells for d in range(4)
                      for y in (neighbour(c, d),) if y >= 0}
            kind, _c = run_regions([frozenset(cells & allowed)],
                                   Step("clue", seconds=20.0), "clue")
            if kind:
                return True
        return False

    passes = 0
    clue_tried_on = None            # the board a clue repair last failed on
    while not _STOP:
        if remaining() <= 0:
            reason = "board-time"
            break
        passes += 1
        progress = False
        all_conclusive = True
        if clues_on and hits < n_clues and clue_tried_on is not at:
            if repair_clues():
                continue
            clue_tried_on = at      # not again until a move changes the board
        for step in plan:
            if _STOP:
                break
            if remaining() <= 0:
                reason = "board-time"
                break
            if step.corral and not corral_on:
                continue
            if time.monotonic() - last_note[0] >= HEARTBEAT_SECONDS:
                note(f"still {NUM_EDGES - breaks}; pass {passes}, next {step_name(step)}; "
                     f"{stats['calls']} CP-SAT call(s) ({stats['optimal']} proved optimal), "
                     f"{stats['redives']} redive(s) so far")
            bc = frozenset(c for pr in broken_of(tiles, at) for c in pr)
            if not bc and hits == n_clues:
                break
            label = step_name(step)
            if step.kind == "swap":
                gained = False
                for _i in range(step.sets):
                    s_cells = independent_set(rng, bc, allowed - frozenset(
                        c for c, _p, _s in pins_for(at, allowed)))
                    g, new = swap_move(tiles, at, s_cells, protect)
                    stats["swap_sets"] += 1
                    if g > 0 and accept(new, "swap", False) == "strict":
                        gained = True
                        break
                if gained:
                    progress = True
                    break
                continue
            if step.kind == "redive":
                # the diver fills every empty cell, so only a complete board can
                # be re-dived without turning a partial into something else
                if policy["seed_file"] is None or len(placed) < NUM_PIECES:
                    continue
                band = border_band(bc, step.h, step.w or 5)
                if band is None:
                    continue
                keep = frozenset(c for c, _p, _s in pins_for(at, allowed))
                if remaining() < 5:
                    continue
                new = redive(policy["seed_file"], tiles, at, band & allowed,
                             copies=step.sets, dives=step.dives, polish=step.polish,
                             threads=workers, rng_seed=rng.randrange(1, 1 << 30),
                             keep=keep, plateau=step.plateau,
                             prior=step.prior, nogo=step.nogo,
                             wall=min(max(MIN_CALL_SECONDS, step.seconds * scale),
                                      remaining()))
                stats["redives"] += 1
                if verbose:
                    nb_new = None if new is None else len(broken_of(tiles, new))
                    note(f"  {label:<14} {len(band & allowed):3d} cells  "
                         + ("no board back" if nb_new is None else
                            f"best copy {NUM_EDGES - nb_new}"))
                if new is not None and accept(new, label, False) == "strict":
                    progress = True
                    break
                all_conclusive = False          # a fresh seed may still gain
                continue
            if step.kind == "window":
                regions = windows(bc, step.h, step.w, allowed, rng)
            elif step.kind == "corner":
                regions = corner_regions(bc, step.h, allowed)
            elif step.kind == "band":
                regions = densest_band(bc, step.h, allowed)
            elif step.kind == "frame":
                regions = frame_region(bc, allowed)
            elif step.kind == "mask":
                regions = [allowed] if allowed & bc else []
            else:
                raise ValueError(step.kind)
            kind, conclusive = run_regions(regions, step, label)
            all_conclusive &= conclusive
            if kind == "strict":
                progress = True
                break
            if kind == "plateau":
                progress = True
                break
        if breaks == 0 and hits == n_clues:
            reason = "solved"
            break
        if remaining() <= 0:
            reason = "board-time"
            break
        if not progress:
            if all_conclusive:
                reason = "exhausted"
                break
            # some calls hit their cap: go round again with fresh seeds
            if passes >= policy["max_passes"]:
                reason = "passes"
                break
    if _STOP:
        reason = "interrupted"
    pos, rot = posrot_of(at)
    return pos, rot, breaks, reason, stage_tag, time.monotonic() - t_start, stats


# ---------------------------------------------------------------------------
def _count_data_rows(path):
    n = 0
    with open(path, newline="") as fh:
        for row in csv.reader(fh):
            if (row and row[0].strip()
                    and not row[0].lstrip().startswith(("#", "%"))):
                n += 1
    return n


def main():
    show_advanced = "--show_advanced" in sys.argv
    adv = (lambda text: text if show_advanced else argparse.SUPPRESS)

    ap = argparse.ArgumentParser(
        description=("Stage C CP-SAT closer: exact re-solves of board regions, "
                     "cheapest first, never returning a worse board."),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog="Use --show_advanced --help to display the low-level controls.")
    ap.add_argument("seed", help="piece seed file")
    ap.add_argument("partials", help="input board CSV")
    ap.add_argument("output", help="output board CSV")

    g = ap.add_argument_group("main search controls")
    g.add_argument("--profile", choices=tuple(EFFORT_PROFILES), default="overnight",
                   help=("overnight: throughput over a large corpus; deep: about "
                         "15 min per board; superdeep: about 2 h per board"))
    g.add_argument("--board_time_limit", type=float, default=None,
                   help="true total seconds per input board; 0 means unlimited")
    g.add_argument("--attempt_time", type=float, default=None,
                   help=adv("cap every CP-SAT call at this many seconds"))
    g.add_argument("--threads", type=int, default=None,
                   help="CP-SAT workers used by one process, over all concurrent models")
    g.add_argument("--jobs", type=int, default=0,
                   help=("region models solved at once, sharing --threads; "
                         "0 = one per 4 threads"))
    g.add_argument("--corral", action=argparse.BooleanOptionalAction, default=None,
                   help=("when no strict gain is left, accept equal-break moves "
                         "that pull breaks toward the nearest corner (the topper's "
                         "pull); default: on for deep and superdeep, off for overnight"))
    g.add_argument("--redive", action=argparse.BooleanOptionalAction, default=True,
                   help=("re-dive the damaged rows plus the clean row under them with "
                         "bin/E555_diver (needs `make diver`); skipped with a note "
                         "when the binary is missing"))
    g.add_argument("--search_mode", choices=("improve", "optimize"), default="improve",
                   help=("improve stops a call at its first strictly better board; "
                         "optimize spends each call's cap on the best one"))
    g.add_argument("--rng_seed", type=int, default=0,
                   help="base seed; 0 chooses and reports a random seed")
    g.add_argument("-v", "--verbose", action="count", default=0,
                   help=("also print every CP-SAT call and redive (twice: draw each "
                         "accepted region too); gains are always printed"))
    g.add_argument("--show_advanced", action="store_true",
                   help="with --help, display all low-level controls")

    corpus = ap.add_argument_group("corpus controls")
    corpus.add_argument("--start_row", type=int, default=0,
                        help="first board row in the input CSV")
    corpus.add_argument("--num_rows", type=int, default=0,
                        help="size of the input window; 0 means all remaining")
    corpus.add_argument("--shard_count", type=int, default=1,
                        help="split the selected input window across processes")
    corpus.add_argument("--shard_index", type=int, default=0,
                        help="zero-based member of the shard set")
    corpus.add_argument("--resume", action="store_true",
                        help="append and skip the number of rows already in output")

    clues = ap.add_argument_group("optional published clues")
    clues.add_argument("--clue_center", action="store_true")
    clues.add_argument("--clue_corners", action="store_true")
    clues.add_argument("--clue_orient", default="auto",
                       choices=("auto", "0", "1", "2", "3"))

    ex = ap.add_argument_group("advanced controls")
    ex.add_argument("--holes", default=None,
                    help=adv("16x16 mask: only these cells may move"))
    ex.add_argument("--max_new_breaks", type=int, default=None,
                    help=adv("cap on junctions matched in the input that a move may "
                             "break; -1 or unset: no cap"))
    ex.add_argument("--max_changes", type=int, default=None,
                    help=adv("cap on cells a single move may change (local branching)"))
    ex.add_argument("--preserve_clean", action=argparse.BooleanOptionalAction,
                    default=True, help=adv("hard-protect the selected clean foundation"))
    ex.add_argument("--max_clean_loss", type=int, default=None,
                    help=adv("clean rows/columns the final board may lose"))
    ex.add_argument("--preserve_side", choices=("auto", "any", "all", "B", "T", "L", "R"),
                    default="auto", help=adv("clean foundation to protect"))
    ex.add_argument("--duplicate_policy", choices=("reuse", "rerun"), default=None,
                    help=adv("reuse or independently rerun exact duplicate inputs"))
    ex.add_argument("--max_passes", type=int, default=0,
                    help=adv("stop a board after this many passes over the plan; 0 = budget only"))
    ex.add_argument("--log_search", action="store_true",
                    help=adv("enable detailed OR-Tools search logs"))
    ex.add_argument("--diver", default=None,
                    help=adv("path of the E555_diver binary (default: bin/ of this repository)"))
    ex.add_argument("--symmetry_level", type=int, default=None, choices=range(0, 5),
                    help=adv("OR-Tools symmetry level (solver default when unset)"))
    ex.add_argument("--linearization_level", type=int, default=None, choices=(0, 1, 2),
                    help=adv("OR-Tools linearization level (solver default when unset)"))

    # The previous ender's tuning flags.  They drove a portfolio that no
    # longer exists (focused pools, a Hamming-radius ladder, singleton donors),
    # so they are accepted -- an old script must not die at startup -- and
    # reported as ignored.
    legacy = ap.add_argument_group("previous-ender flags (accepted, ignored)")
    for name, kw in (("--mode", {}), ("--reach", {}), ("--ladder", {}), ("--rungs", {}),
                     ("--changes_start", {}), ("--changes_step", {}),
                     ("--restarts_per_rung", {}), ("--inner_cap", {}),
                     ("--patch_shape", {}), ("--border_scope", {}), ("--ring_radius", {}),
                     ("--focus_limit", {}), ("--focus_reach", {}), ("--focus_inner_cap", {}),
                     ("--focus_ring_radius", {}), ("--focus_attempt_time", {}),
                     ("--focus_share", {}), ("--donors_per_target", {}),
                     ("--donor_cells_max", {}), ("--new_break_reference", {}),
                     ("--stall_time", {}), ("--no_gain_limit", {}),
                     ("--hint_conflicts", {})):
        legacy.add_argument(name, default=None, help=argparse.SUPPRESS, **kw)
    for name in ("--focus", "--accept_equal_compaction", "--repair_hint"):
        legacy.add_argument(name, action=argparse.BooleanOptionalAction, default=None,
                            help=argparse.SUPPRESS)
    args = ap.parse_args()
    ignored = sorted(k for k in ("mode", "reach", "ladder", "rungs", "changes_start",
                                 "changes_step", "restarts_per_rung", "inner_cap",
                                 "patch_shape", "border_scope", "ring_radius",
                                 "focus_limit", "focus_reach", "focus_inner_cap",
                                 "focus_ring_radius", "focus_attempt_time", "focus_share",
                                 "donors_per_target", "donor_cells_max",
                                 "new_break_reference", "stall_time", "no_gain_limit",
                                 "hint_conflicts", "focus", "accept_equal_compaction",
                                 "repair_hint")
                     if getattr(args, k) is not None)
    if ignored:
        print("[note] ignored (previous ender's portfolio): "
              + " ".join("--" + k for k in ignored), flush=True)

    if args.rng_seed == 0:
        args.rng_seed = random.randint(1_000_000, 9_999_999)
    if args.start_row < 0 or args.num_rows < 0:
        sys.exit("[ERROR] --start_row and --num_rows must be non-negative.")
    if args.shard_count < 1 or not 0 <= args.shard_index < args.shard_count:
        sys.exit("[ERROR] require shard_count >= 1 and 0 <= shard_index < shard_count.")
    if args.attempt_time is not None and args.attempt_time <= 0:
        sys.exit("[ERROR] --attempt_time must be positive when specified.")
    if args.board_time_limit is not None and args.board_time_limit < 0:
        sys.exit("[ERROR] --board_time_limit must be >= 0.")

    prof = EFFORT_PROFILES[args.profile]
    board_time = prof.board_seconds if args.board_time_limit is None else args.board_time_limit
    workers = prof.workers if args.threads is None else args.threads
    if workers < 1:
        sys.exit("[ERROR] --threads must be >= 1.")
    jobs = args.jobs if args.jobs > 0 else max(1, workers // 4)
    jobs = min(jobs, workers)
    max_clean_loss = prof.max_clean_loss if args.max_clean_loss is None else args.max_clean_loss
    max_new = args.max_new_breaks
    if max_new is not None and max_new < 0:
        max_new = None
    holes = read_holes_file(args.holes) if args.holes else None
    if holes is not None and not holes:
        sys.exit("[ERROR] --holes marks no movable cell.")

    policy = {
        "profile": prof, "board_time": board_time, "plan": prof.plan,
        "seed_file": args.seed if args.redive else None,
        "attempt_override": args.attempt_time, "jobs": jobs,
        "corral": prof.corral if args.corral is None else args.corral,
        "search_mode": args.search_mode,
        "preserve_clean": args.preserve_clean, "preserve_side": args.preserve_side,
        "max_clean_loss": max_clean_loss, "max_new_breaks": max_new,
        "max_changes": args.max_changes, "log_search": args.log_search,
        "symmetry_level": args.symmetry_level,
        "linearization_level": args.linearization_level,
        "max_passes": args.max_passes if args.max_passes > 0 else 10 ** 9,
        "duplicate_policy": args.duplicate_policy or prof.duplicate_policy,
    }

    CL = clue_mask = None
    if args.clue_center or args.clue_corners:
        CL = load_clues()
        clue_mask = ((CL.CLUE_CENTER if args.clue_center else 0)
                     | (CL.CLUE_CORNERS if args.clue_corners else 0))
    tiles = read_seed(args.seed)
    signal.signal(signal.SIGINT, _request_stop)

    global DIVER_OVERRIDE
    DIVER_OVERRIDE = args.diver
    if args.redive and diver_path() is None:
        print("[note] bin/E555_diver not found (run `make diver`): the redive "
              "step is skipped", flush=True)
        policy["seed_file"] = None

    def step_text(st):
        """What a plan step does, with its real per-call cap at this budget."""
        cap = (args.attempt_time if args.attempt_time is not None
               else max(MIN_CALL_SECONDS, st.seconds * scale))
        if st.kind == "swap":
            return f"exchange cycles over {st.sets} sets of non-adjacent cells (exact, instant)"
        if st.kind == "redive":
            return (f"re-dive the damaged rows" + (f" + {st.h} more" if st.h else "")
                    + f" (at most {st.w or 5} deep): {st.sets} copies of {st.dives} dives"
                    f" + {st.polish} polish rounds, up to {duration(cap)}"
                    + (f", prior {st.prior:g}" if st.prior else "")
                    + (f", nogo {st.nogo:g}" if st.nogo else "")
                    + (", plateau" if st.plateau else ""))
        what = {"window": (f"every {st.h}x{st.w} square" if st.h == st.w else
                           f"every {st.h}x{st.w} and {st.w}x{st.h} rectangle") + " touching a break",
                "corner": f"each damaged {st.h}x{st.h} corner block with its frame arms",
                "band": f"the {st.h} most damaged rows or columns",
                "frame": "the 60 frame cells and the damaged cells beside them"}[st.kind]
        return what + (", breaks pulled together" if st.corral else "") + \
            f", CP-SAT up to {duration(cap)} a call"

    scale = (board_time / prof.board_seconds
             if board_time > 0 and prof.board_seconds > 0 else 1.0)
    plan_steps = [st for st in prof.plan
                  if (st.kind != "redive" or policy["seed_file"])
                  and (not st.corral or policy["corral"])]

    def selected_rows():
        data_idx = 0
        with open(args.partials, newline="") as src:
            for row in csv.reader(src):
                if not (row and row[0].strip()
                        and not row[0].lstrip().startswith(("#", "%"))):
                    continue
                idx = data_idx
                data_idx += 1
                if idx < args.start_row:
                    continue
                if args.num_rows and idx >= args.start_row + args.num_rows:
                    break
                if (idx - args.start_row) % args.shard_count != args.shard_index:
                    continue
                yield idx, row

    resume_rows = 0
    out_path = Path(args.output)
    if args.resume and out_path.exists():
        resume_rows = _count_data_rows(out_path)
        out_mode = "a"
    else:
        out_mode = "w"
    n_selected = sum(1 for _ in selected_rows())
    n_todo = max(0, n_selected - resume_rows)

    try:
        import ortools
        ortools_version = ortools.__version__
    except (ImportError, AttributeError):
        ortools_version = "?"
    print("\n=== E555 ender ===")
    window = ""
    if args.start_row or args.num_rows:
        last = args.start_row + args.num_rows if args.num_rows else None
        window = (f", input rows {args.start_row + 1}-{last}" if last
                  else f", input rows {args.start_row + 1} on")
    if args.shard_count > 1:
        window += f", shard {args.shard_index + 1} of {args.shard_count}"
    print(f"[in]  boards  {args.partials}: {n_todo} board(s) to do{window}"
          + (f" ({resume_rows} already in the output)" if resume_rows else ""))
    print(f"[in]  seed    {args.seed}")
    print(f"[out] boards  {args.output}: one row per board, never worse"
          + (" (appending: --resume)" if out_mode == "a" else ""))
    if board_time > 0:
        total = n_todo * board_time
        finish = time.strftime("%a %H:%M" if total > 20 * 3600 else "%H:%M",
                               time.localtime(time.time() + total))
        budget = (f"up to {duration(board_time)} per board, at most {duration(total)} "
                  f"in all (by {finish}; boards that run out of moves finish sooner)")
    else:
        budget = "no time limit per board (each runs until nothing is left to try)"
    print(f"[cfg] profile {args.profile}: {budget}")
    print(f"[cfg] threads {workers} ({jobs} region model(s) at a time, "
          f"{max(1, workers // jobs)} CP-SAT workers each)")
    print("[cfg] plan, cheapest first, back to the top after every gain:")
    for st in plan_steps:
        print(f"[cfg]   {step_name(st):<18} {step_text(st)}")
    guard = "off"
    if args.preserve_clean:
        guard = {"auto": "the cleanest side's", "any": "any side's",
                 "all": "every side's"}.get(args.preserve_side, f"side {args.preserve_side}'s")
        guard += f" clean rows kept (at most {max_clean_loss} lost)"
    extras = [f"only the {len(holes)} cells of --holes move"] if holes else []
    if max_new is not None:
        extras.append(f"at most {max_new} new break(s) a move")
    if args.max_changes is not None:
        extras.append(f"at most {args.max_changes} cell(s) changed a move")
    print(f"[cfg] guard   {guard}" + "".join("; " + e for e in extras))
    if clue_mask:
        which = " + ".join(n for n, on in (("centre", args.clue_center),
                                           ("corners", args.clue_corners)) if on)
        print(f"[cfg] clues   {which} pinned (orientation {args.clue_orient})")
    print(f"[cfg] tools   OR-Tools {ortools_version}; diver "
          + (str(diver_path()) if policy["seed_file"] else "off (no redive)"))
    print(f"[cfg] rng     --rng_seed {args.rng_seed} (pass it again to repeat this run)",
          flush=True)

    rows = itertools.islice(selected_rows(), resume_rows, None)
    run_start = time.time()
    done = gained = reused = improved = 0
    before_hist, after_hist = collections.Counter(), collections.Counter()
    best_seen = None                        # (score, id, input row)
    duplicate_cache = {}
    reason_text = {"board-time": "time up", "exhausted": "nothing left to try",
                   "solved": "solved", "input-solved": "already solved",
                   "passes": "--max_passes reached", "interrupted": "interrupted"}
    with open(args.output, out_mode, newline="") as out:
        writer = csv.writer(out, lineterminator="\n")
        for input_idx, row in rows:
            if _STOP:
                break
            partial = parse_partial_line(row)
            bin_ = len(broken_junctions(partial.pos, partial.rot, tiles))
            fp = board_fingerprint(partial.pos, partial.rot)
            tag = f"[{done + 1}/{n_todo}]"
            head = f"{tag} {partial.config_id} (input row {input_idx + 1})"
            if policy["duplicate_policy"] == "reuse" and fp in duplicate_cache:
                pos, rot, breaks, first = duplicate_cache[fp]
                reused += 1
                print(f"{head}: same board as input row {first + 1}, its result reused: "
                      f"{NUM_EDGES - bin_} -> {NUM_EDGES - breaks}", flush=True)
            else:
                print(f"{head}: {NUM_EDGES - bin_} ({bin_} breaks)", flush=True)
                orient = None
                if clue_mask:
                    if args.clue_orient == "auto":
                        orient, _n = CL.clue_orient(partial.pos, partial.rot, clue_mask)
                    else:
                        orient = int(args.clue_orient)
                    if orient is None:
                        print("      [warn] no enabled clue identifies an orientation; "
                              "solving unpinned", flush=True)
                pos, rot, breaks, reason, stage, sec, bstats = solve_board(
                    partial, tiles, policy, workers, args.rng_seed + 104729 * input_idx,
                    args.verbose, holes=holes, CL=CL, clue_mask=clue_mask, orient=orient)
                if policy["duplicate_policy"] == "reuse":
                    duplicate_cache[fp] = (list(pos), list(rot), breaks, input_idx)
                skipped = (f", {bstats['cached']} skipped as already proved"
                           if bstats["cached"] else "")
                print(f"      {clock(sec):>7}  done: {NUM_EDGES - bin_} -> {NUM_EDGES - breaks}"
                      + (f" (+{bin_ - breaks})" if breaks < bin_ else "")
                      + f"; {bstats['calls']} CP-SAT call(s) ({bstats['optimal']} proved"
                      f" optimal{skipped}), {bstats['redives']} redive(s); stopped: "
                      + reason_text.get(reason, reason), flush=True)
            writer.writerow([partial.config_id, str(NUM_EDGES - breaks)]
                            + [str(x) for x in pos] + [str(x) for x in rot])
            out.flush()
            done += 1
            gained += bin_ - breaks
            improved += breaks < bin_
            before_hist[NUM_EDGES - bin_] += 1
            after_hist[NUM_EDGES - breaks] += 1
            if best_seen is None or NUM_EDGES - breaks > best_seen[0]:
                best_seen = (NUM_EDGES - breaks, partial.config_id, input_idx + 1)

    def hist(h):
        return ", ".join(f"{k} x{h[k]}" for k in sorted(h, reverse=True)) or "none"

    elapsed = time.time() - run_start
    print("\n=== summary ===")
    print(f"[sum] {done} of {n_todo} board(s) in {clock(elapsed)}: {improved} improved, "
          f"+{gained} edge(s) in all"
          + (f"; {reused} exact duplicate(s) reused" if reused else ""))
    print(f"[sum] scores before: {hist(before_hist)}")
    print(f"[sum] scores after:  {hist(after_hist)}")
    if best_seen:
        print(f"[sum] best: {best_seen[0]} {best_seen[1]} (input row {best_seen[2]})")
    print(f"[sum] written: {args.output}"
          + (f" ({resume_rows} earlier row(s) kept)" if resume_rows else ""))
    if _STOP:
        print("[sum] stopped cleanly; every row already written is complete; "
              "--resume continues from the next board")


if __name__ == "__main__":
    main()
