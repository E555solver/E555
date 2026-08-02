#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
E555_topper.py -- Stage C break-minimizer: push breaks to the NEAREST corner.

WHAT IT DOES

    Takes partial (or complete-but-broken) Eternity II boards and re-places
    pieces with Google's OR-Tools CP-SAT solver to minimize the number of
    edge mismatches ("breaks"), while herding the unavoidable ones toward
    the closest corner of the board, where later stages can attack them.

    The pull is toward the NEAREST corner, not a single hard-coded one, so the
    longest trip a break can face is 7 rows + 7 columns instead of 15 + 15, and
    the four corners share the load. --side then chooses WHICH border band is
    opened for mutation, so breaks stranded on the left or the bottom can be
    attacked where they are instead of being dragged across the whole board.

EXAMPLE RUNS

    # The final push of a top sliding window (see the STRATEGY GUIDE below):
    #   open the top 4 rows, herd breaks up into the top corners.
    python3 E555_topper.py seed_Edge5.txt board.csv topped.csv \
            --side T --work-rows 4 --workers 8 --verbose

    # Clean-up pass: fold whatever is left into the top-right corner, and ask
    # for the 3 best genuinely-different boards to carry into the next step.
    python3 E555_topper.py seed_Edge5.txt topped.csv corner.csv \
            --side TR --work-rows 4 --report_best 3

    # Recover breaks stranded on a side the top window never opened.
    python3 E555_topper.py seed_Edge5.txt corner.csv left.csv \
            --side L --work-rows 4

THE MODEL

    - Constraint formulation: the board is a grid of (piece, rotation, four
      edge colors) variables; domain filtering enforces edge matching and
      the frame rule (grey edges only on the outer border).
    - Lexicographic objective, via dominating weights, in strict priority:
        1. minimize the total number of breaks globally;
        2. push unavoidable breaks toward the nearest horizontal border
           (top for the upper half, bottom for the lower half);
        3. slide the remainder toward the nearest vertical border
           (right for the right half, left for the left half).
      Together, (2) then (3) mean "go to the closest corner".
    - Iterative solving: the outermost rows/cols of the open band can be
      locked empty (--unused_rows) while the rest mutates (--work-rows),
      enabling the sliding-window strategy below.
    - Beam search: --report_best N emits N genuinely DIFFERENT near-optimal
      boards (see --beam_diff / --beam_slack), not N successive incumbents.
    - Smart scoring: --allow_break_increase trades a slightly worse total
      for a longer push (useful in the middle window steps).
    - Chunked CSV reading (--row / --count) for Slurm array jobs; writes
      are retried once on transient network-filesystem errors.

WHAT IS OPEN FOR MUTATION

    band   = the --side bands, each --work-rows deep
    unused = the outermost --unused_rows rows/cols of those same bands
    free   = band + break cells orthogonally adjacent to the band - unused

    Two rules keep a --side run confined to that side:

      * A break cell only joins the free set if it lies in, or touches, the
        band. A break trapped on the far side of the board stays locked --
        that is what keeps a --side R run a right-side run.
      * An empty cell outside the band stays empty. Only the band, and the
        breaks touching it, are opened; the rest of the board is frozen.

    --unused_rows N actively UNSETS those cells: any piece sitting there is
    lifted to 999 before the model is built, and the cells stay empty for
    this run. Note the lifted pieces do rejoin the free pool -- the solver
    may re-use them elsewhere in the band. The output therefore shows those
    rows as 999 with lots of breaks around them; the next window recovers
    them.

    --holes FILE replaces all of that with an explicit 16x16 0/1 mask (the
    same dialect the ender and the backtracker read, see data/holes_*.csv):
    free = exactly the cells marked 1. --side, --work-rows and the
    break-adjacency rule no longer apply, because the mask already says
    precisely which cells may move. Use it when the region you want is not a
    border band -- an L round a corner, a ragged patch round a cluster of
    breaks, or the interior a band can never reach. --unused_rows is defined
    in terms of the bands, so it cannot be combined with a mask.

OUTPUT

    One canonical board row per result:  config_id, score, pos[256], rot[256]
    with score = matched internal edges (0..480), junctions touching an
    unplaced cell counted as breaks, so a plain sort -k2,2nr pruning ranks
    these boards. The input row index is appended to the id as `#<row>` when
    not already tagged, and
    `_r<rank>` distinguishes the ranks of a --report_best beam. --tag_id
    additionally appends `_<score>` to the id (older naming style).

    The break count alone hides what matters between window steps: eighteen
    breaks spread over seven rows is a mess, the same eighteen packed into
    rows 14-15 is nearly finished. `tools/E555_rank.py` derives that from the
    board -- break_rows, span, clean rows per border, solid pieces -- and
    sorts or re-emits the CSV by any of them.

====================================================================================
 *** STRATEGY GUIDE: The Overlapping Sliding Window ***
====================================================================================
To solve the board efficiently without trapping the solver in a greedy dead-end,
you should overlap your work areas. Move the target rows outwards while maintaining
a constant computation size (e.g., a 4-row budget).

Assuming row 0 is the bottom and row 15 is the top, here is how you bring pieces
up from row 9 all the way to the top using overlapping steps (--side T):

Step 1: --work-rows 7 --unused_rows 3
        (Opens rows 9, 10, 11, 12. Rows 13-15 are locked empty)
Step 2: --work-rows 6 --unused_rows 2
        (Opens rows 10, 11, 12, 13. Notice it can still mutate 11-12 to fix mistakes!)
Step 3: --work-rows 5 --unused_rows 1
        (Opens rows 11, 12, 13, 14. Row 15 is locked empty)
Step 4: --work-rows 4 --unused_rows 0
        (Opens rows 12, 13, 14, 15. The final push!)

Then, if breaks remain stuck on a side the window never opened, run a clean-up
pass there: --side L (or R, or B) with --work-rows 4 --unused_rows 0. --side TR
and --side TL open an L-shaped band and drive breaks into that one corner;
--side TB opens both horizontal borders at once, which is the cheapest way to
find out whether the two opposite sides can be re-cut against each other.
====================================================================================
"""
from __future__ import annotations
import argparse, csv, os, signal, sys, threading, time, random
from dataclasses import dataclass
try:
    from ortools.sat.python import cp_model
except ImportError:
    sys.exit("E555_topper.py needs OR-Tools, which is the one non-stdlib\n"
             "dependency in the toolkit:  pip install ortools")
import itertools

_STOP = False
def _request_stop(signum, frame):
    """
    Gracefully handle Ctrl-C so the solver saves its current best progress before exiting.
    """
    global _STOP
    _STOP = True
    print("\n[Ctrl-C] Signal caught. Finishing current board solve then stopping...", flush=True)

# Board and game constants
SIDE, NUM_PIECES, NUM_EDGES, GREY, CSV_UNPLACED = 16, 256, 480, 0, 999
NORTH, EAST, SOUTH, WEST = 0, 1, 2, 3

# Sets for quick topological lookups
CORNER_CELLS = frozenset({0, SIDE - 1, (SIDE - 1) * SIDE, SIDE * SIDE - 1})
BORDER_CELLS = frozenset(c for c in range(NUM_PIECES)
                         if c // SIDE in (0, SIDE - 1) or c % SIDE in (0, SIDE - 1))

# Precompute all valid adjacent junctions (pairs of cells that share a border)
ALL_JUNCTIONS = []
for _cell in range(NUM_PIECES):
    _r, _c = _cell // SIDE, _cell % SIDE
    if _c + 1 < SIDE: ALL_JUNCTIONS.append((_cell, _cell + 1, EAST, WEST))
    if _r + 1 < SIDE: ALL_JUNCTIONS.append((_cell, _cell + SIDE, NORTH, SOUTH))

# ---------------------------------------------------------------------------
# Cost metrics: distance to the nearest corner, split into its two components.
# A cell in the upper half measures up to the top border, one in the lower half
# down to the bottom; likewise left/right. Minimizing the vertical part first
# and the horizontal part second sends a break to the corner closest to it.
# ---------------------------------------------------------------------------
def _v(cell):
    """Rows between a cell and the nearest horizontal border (0..7)."""
    r = cell // SIDE
    return min(r, SIDE - 1 - r)

def _h(cell):
    """Columns between a cell and the nearest vertical border (0..7)."""
    c = cell % SIDE
    return min(c, SIDE - 1 - c)

def row_cost(a, b):
    """Combined vertical distance-to-border of two adjacent cells (0..14)."""
    return _v(a) + _v(b)

def col_cost(a, b):
    """Combined horizontal distance-to-border of two adjacent cells (0..14)."""
    return _h(a) + _h(b)

# Both costs peak when both cells sit on the centre lines: 2 * (8 - 1).
MAX_ROW_COST = MAX_COL_COST = 2 * (SIDE // 2 - 1)

# ---------------------------------------------------------------------------
# Border bands: which cells --side opens
# ---------------------------------------------------------------------------
SIDES = {"T": ("T",), "B": ("B",), "R": ("R",), "L": ("L",),
         "TR": ("T", "R"), "TL": ("T", "L"), "TB": ("T", "B"),
         "BR": ("B", "R"), "BL": ("B", "L"), "LR": ("L", "R")}

def band_cells(which, depth):
    """The `depth` outermost rows (T/B) or columns (R/L) of one border."""
    if depth <= 0:
        return set()
    d = min(depth, SIDE)
    if which == "T":
        return {r * SIDE + c for r in range(SIDE - d, SIDE) for c in range(SIDE)}
    if which == "B":
        return {r * SIDE + c for r in range(d) for c in range(SIDE)}
    if which == "R":
        return {r * SIDE + c for r in range(SIDE) for c in range(SIDE - d, SIDE)}
    if which == "L":
        return {r * SIDE + c for r in range(SIDE) for c in range(d)}
    raise ValueError(f"unknown border {which!r}")

def side_cells(side, depth):
    """The union of the bands named by a --side value (e.g. 'TR')."""
    out = set()
    for which in SIDES[side]:
        out |= band_cells(which, depth)
    return out

def read_holes_file(path):
    """Cells marked 1 in a 16x16 0/1 mask; first data line is row 0 (bottom)."""
    values = []
    with open(path) as fh:
        for line in fh:
            if not line.strip().startswith("#"):
                values.extend(line.replace(",", " ").split())
    return {i for i, v in enumerate(values) if int(v) == 1}

def rotate_edges(base, spin):
    """Returns the colors of a piece shifted by a given rotation [0-3]."""
    return (base[(NORTH + spin) & 3], base[(EAST + spin) & 3],
            base[(SOUTH + spin) & 3], base[(WEST + spin) & 3])

def frame_rule_ok(r, c, oriented):
    """Ensures grey edges only ever point to the outside border of the 16x16 grid."""
    n, e, s, w = oriented
    return ((n == GREY) == (r == SIDE - 1) and (e == GREY) == (c == SIDE - 1) and
            (s == GREY) == (r == 0) and (w == GREY) == (c == 0))

def piece_class(tile):
    """Categorizes a tile based on its grey edges (corner, edge, or inner)."""
    g = sum(1 for x in tile if x == GREY)
    return "corner" if g == 2 else "edge" if g == 1 else "inner"

def read_seed(path):
    """Reads the text file containing the piece definitions."""
    with open(path) as fh:
        return [tuple(int(x) for x in line.split()) for line in fh if len(line.split()) == 4]

@dataclass
class Partial:
    config_id: str; pos: list; rot: list

def parse_partial_line(fields):
    """Parses a CSV row into a Partial dataclass."""
    f = [x.strip() for x in fields]
    return Partial(f[0], [int(x) for x in f[-512:-256]], [int(x) for x in f[-256:]])

def broken_junctions(pos, rot, tiles):
    """Identifies mismatching adjacent colors on the current board."""
    col = {pos[p]: rotate_edges(tiles[p], rot[p]) for p in range(NUM_PIECES) if pos[p] != CSV_UNPLACED}
    out = []
    for a, b, da, db in ALL_JUNCTIONS:
        if a in col and b in col and col[a][da] == col[b][db]:
            continue
        out.append((a, b))
    return out

def board_metrics(pos, rot, tiles):
    """Returns metrics describing the board's current fitness."""
    br = broken_junctions(pos, rot, tiles)
    if not br:
        return 0, 0, 0, 0
    # The depth heuristic here is strictly for reporting metadata, not the solver weight
    ROWW = MAX_COL_COST + 1
    return (len(br),
            sum(row_cost(a, b) for a, b in br),
            sum(col_cost(a, b) for a, b in br),
            max((ROWW * row_cost(a, b) + col_cost(a, b)) for a, b in br))

def solve_frontier(pos, rot, tiles, free_cells, args, verbose):
    """
    The core CP-SAT logic. Builds a constraint model to fill 'free_cells' using available pieces.
    Returns a list of best configurations found to support Beam Search.
    """
    model = cp_model.CpModel()
    maxc = max(max(t) for t in tiles)
    piece_at = {pos[p]: p for p in range(NUM_PIECES) if pos[p] != CSV_UNPLACED}
    unplaced = [p for p in range(NUM_PIECES) if pos[p] == CSV_UNPLACED]

    free_pieces = unplaced + [piece_at[c] for c in free_cells if c in piece_at]
    dom = {"corner": [], "edge": [], "inner": []}
    for p in free_pieces:
        dom[piece_class(tiles[p])].append(p)
    for k in dom: dom[k].sort()

    def cell_class(cell):
        return "corner" if cell in CORNER_CELLS else "edge" if cell in BORDER_CELLS else "inner"

    # Define variables for piece ID, rotation, and the resulting 4 edge colors for every free cell
    piece_of, rot_of, col_of = {}, {}, {}
    for cell in sorted(free_cells):
        r, c = cell // SIDE, cell % SIDE
        d = dom[cell_class(cell)]
        pv = model.NewIntVarFromDomain(cp_model.Domain.FromValues(d), f"pc_{cell}")
        rv = model.NewIntVar(0, 3, f"ro_{cell}")
        nv, ev, sv, wv = (model.NewIntVar(0, maxc, f"{x}_{cell}") for x in "NESW")
        
        # Domain filtering: Only allow assignments that satisfy the frame rules (grey edges)
        allowed = [(p, s) + rotate_edges(tiles[p], s) for p in d for s in range(4)
                   if frame_rule_ok(r, c, rotate_edges(tiles[p], s))]
        model.AddAllowedAssignments([pv, rv, nv, ev, sv, wv], allowed)
        piece_of[cell], rot_of[cell], col_of[cell] = pv, rv, (nv, ev, sv, wv)

    # Ensure no piece is used twice
    for kls in ("corner", "edge", "inner"):
        cells = [c for c in free_cells if cell_class(c) == kls]
        if len(cells) > 1:
            model.AddAllDifferent([piece_of[c] for c in cells])

    fixed_col = {c: rotate_edges(tiles[piece_at[c]], rot[piece_at[c]])
                 for c in piece_at if c not in free_cells}
                 
    def ecolor(cell, d):
        if cell in col_of: return col_of[cell][d]
        if cell in fixed_col: return fixed_col[cell][d]
        return None  # Cell is completely empty and excluded (e.g. from unused_top_rows)

    # One Bool per junction the solver can actually break; junctions between two
    # fixed cells are constants and junctions touching a hole do not constrain.
    breaks = []
    for a, b, da, db in ALL_JUNCTIONS:
        ca, cb = ecolor(a, da), ecolor(b, db)
        if ca is None or cb is None: continue  # Unplaced holes do not constrain
        if isinstance(ca, int) and isinstance(cb, int): continue  # Fixed -> constant

        brk = model.NewBoolVar(f"brk_{a}_{b}")
        model.Add(ca == cb).OnlyEnforceIf(brk.Not())
        model.Add(ca != cb).OnlyEnforceIf(brk)
        breaks.append((brk, row_cost(a, b), col_cost(a, b)))

    # ---------------------------------------------------------
    # WEIGHTING SCHEME
    # ---------------------------------------------------------
    # The three priorities are packed into one integer. For the order to be
    # STRICT, each weight must dominate the largest total its junior terms can
    # reach *summed over every break at once* -- not the largest a single
    # junction can contribute. With n breakable junctions in this model:
    #
    #     w_row > n * MAX_COL_COST
    #     w_b   > n * (w_row * MAX_ROW_COST + MAX_COL_COST)
    #
    # Sizing them from n (rather than from all 480 junctions, as the topper
    # does) and from the corner-pull maxima of 14 (rather than 30) keeps the
    # objective one to two orders of magnitude smaller, which is a materially
    # easier LP relaxation for CP-SAT.
    n = max(1, len(breaks))
    if args.allow_break_increase:
        # Smart Scoring: deliberately NOT lexicographic -- 1 break is tradeable
        # for 5 units of row cost (2.5 rows of push), as in the topper. In this
        # mode decode_obj's split is indicative only.
        w_row = MAX_COL_COST + 1                                   # 15
        w_b = w_row * 5                                            # 75
    else:
        # Strict Lexicographic: never trade breaks for position
        w_row = n * MAX_COL_COST + 1
        w_b = n * (w_row * MAX_ROW_COST + MAX_COL_COST) + 1

    def decode_obj(v):
        """Decodes the packed integer objective back into interpretable sub-metrics."""
        B, rem = divmod(v, w_b)
        Pr, Pc = divmod(rem, w_row)
        return B, Pr, Pc

    # Objective: minimize breaks; among equal-break boards, pull them to the
    # nearest horizontal border, then along it to the nearest corner.
    obj_expr = sum((w_b + w_row * rc + cc) * brk for brk, rc, cc in breaks)
    model.Minimize(obj_expr)

    # Give the solver a hint based on the starting state of the board
    for cell in free_cells:
        if cell in piece_at:
            p = piece_at[cell]
            model.AddHint(piece_of[cell], p)
            model.AddHint(rot_of[cell], rot[p])
            for d, v in enumerate(col_of[cell]):
                model.AddHint(v, rotate_edges(tiles[p], rot[p])[d])

    # ---------------------------------------------------------
    # ONE SOLVE, with incumbent telemetry and a stall watchdog
    # ---------------------------------------------------------
    class _Tracker(cp_model.CpSolverSolutionCallback):
        """Reports incumbents as they arrive and feeds the stall watchdog."""
        def __init__(self):
            cp_model.CpSolverSolutionCallback.__init__(self)
            self._t0 = self.last = time.monotonic()
            self.found = 0

        def on_solution_callback(self):
            self.last = time.monotonic()
            self.found += 1
            if verbose:
                B, Pr, Pc = decode_obj(int(round(self.ObjectiveValue())))
                print(f"      [inc] t={self.last-self._t0:6.1f}s | open-break={B} | "
                      f"corner-dist={Pr + Pc} ({Pr} up/down + {Pc} sideways)", flush=True)

    def run_solve(budget):
        """Solves the current model for at most `budget` seconds."""
        solver = cp_model.CpSolver()
        solver.parameters.num_search_workers = args.workers
        solver.parameters.linearization_level = 1
        solver.parameters.symmetry_level = 0
        solver.parameters.random_seed = (args.rng_seed & 0x7fffffff
                                         if args.rng_seed >= 0 else random.randrange(1 << 31))
        solver.parameters.max_time_in_seconds = float(budget)

        tracker = _Tracker()
        done, state = threading.Event(), {"stalled": False}

        # Watchdog thread to stop the solver early if IMPROVEMENT stalls. It only
        # arms once a first solution exists: for ranks 2..N the search may have
        # to prove that no distinct board is left, and that is not a stall.
        def watchdog():
            while not done.wait(0.5):
                if (args.stall_time > 0 and tracker.found > 0
                        and time.monotonic() - tracker.last > args.stall_time):
                    state["stalled"] = True
                    if verbose: print("      [!] Solver stalled. Halting search early.", flush=True)
                    solver.StopSearch()
                    return

        w = threading.Thread(target=watchdog, daemon=True); w.start()
        status = solver.Solve(model, tracker)
        done.set(); w.join()

        rsn = ("optimal" if status == cp_model.OPTIMAL else
               "stalled" if state["stalled"] else
               "max-time" if status == cp_model.FEASIBLE else
               "infeasible" if status == cp_model.INFEASIBLE else "no-sol")
        ok = status in (cp_model.OPTIMAL, cp_model.FEASIBLE)
        return ok, solver, rsn

    def board_from(solver):
        """Reads the free cells out of a solved model onto a copy of the board."""
        state = {c: (solver.Value(piece_of[c]), solver.Value(rot_of[c])) for c in free_cells}
        temp_pos, temp_rot = list(pos), list(rot)
        # 1. Lift every piece the solver was allowed to move, to prevent collisions
        for p in free_pieces:
            temp_pos[p] = CSV_UNPLACED
            temp_rot[p] = 0
        # 2. Apply the solver's chosen placements
        for c, (p, r) in state.items():
            temp_pos[p], temp_rot[p] = c, r
        return (temp_pos, temp_rot), state

    def forbid(state, k):
        """Requires at least `k` free cells to hold a piece other than in `state`."""
        diffs = []
        for c, (p, _r) in state.items():
            bv = model.NewBoolVar(f"df_{c}_{len(diffs)}")
            model.Add(piece_of[c] != p).OnlyEnforceIf(bv)
            model.Add(piece_of[c] == p).OnlyEnforceIf(bv.Not())
            diffs.append(bv)
        model.Add(sum(diffs) >= min(k, len(diffs)))

    # ---------------------------------------------------------
    # BEAM: rank 1 is the optimum; each further rank is a board that differs
    # from every earlier one in >= --beam_diff cells and costs at most
    # --beam_slack extra breaks. Successive incumbents of a single search are
    # near-duplicates of the optimum, so they are NOT used as ranks.
    # ---------------------------------------------------------
    ranks = max(1, args.report_best)
    budget = float(args.max_time) if ranks == 1 else float(args.max_time) / ranks
    results, wall, rsn = [], 0.0, "no-sol"

    for rank in range(ranks):
        ok, solver, reason = run_solve(budget)
        wall += solver.WallTime()
        if rank == 0:
            rsn = reason
        if not ok:
            if rank == 0:
                results.append((pos, rot))       # Fallback if no solution found
            elif verbose:
                print(f"      [beam] rank {rank+1}: no distinct board within slack ({reason})", flush=True)
            break

        board, state = board_from(solver)
        results.append(board)
        if rank == 0 and ranks > 1:
            # Keep every later rank within --beam_slack breaks of the optimum
            best_obj = int(round(solver.ObjectiveValue()))
            model.Add(obj_expr <= best_obj + args.beam_slack * w_b)
            # The warm start points at the board we are about to forbid
            if hasattr(model, "ClearHints"): model.ClearHints()
        if rank + 1 < ranks:
            forbid(state, args.beam_diff)

    return results, rsn, wall

def neighbours(cell):
    """The orthogonally adjacent cells of `cell`, clipped to the board."""
    r, c = cell // SIDE, cell % SIDE
    for nr, nc in ((r - 1, c), (r + 1, c), (r, c - 1), (r, c + 1)):
        if 0 <= nr < SIDE and 0 <= nc < SIDE:
            yield nr * SIDE + nc

def build_free(pos, rot, tiles, side, work_rows, unused_rows, holes=None):
    """
    Determines which cells the solver is allowed to mutate, and which are held
    empty. Returns (free, unused).

    free   = the --side bands, --work-rows deep, plus any break cell that lies
             in or touches those bands, minus the unused region.
    unused = the outermost --unused_rows rows/cols of the same bands: locked
             empty for this run (main() lifts any piece sitting there to 999).

    This does NOT free every break cell on the board, nor every empty cell: a
    break stranded away from the open bands stays locked, which is what keeps a
    one-sided run one-sided.

    A --holes mask replaces the band entirely and is taken literally: no
    adjacency expansion, nothing held empty. The mask has already named every
    cell that may move, so widening it would break the guarantee the caller
    asked for.
    """
    if holes is not None:
        return set(holes), set()

    band = side_cells(side, work_rows)
    unused = side_cells(side, unused_rows) if unused_rows > 0 else set()

    breakc = {c for pair in broken_junctions(pos, rot, tiles) for c in pair}
    adjacent = {c for c in breakc
                if c in band or any(n in band for n in neighbours(c))}

    return (band | adjacent) - unused, unused

def band_map(free, unused, pos):
    """A 16x16 ASCII picture of the open band, printed top row first."""
    occupied = {pos[p] for p in range(NUM_PIECES) if pos[p] != CSV_UNPLACED}
    out = []
    for r in range(SIDE - 1, -1, -1):
        line = []
        for c in range(SIDE):
            cell = r * SIDE + c
            line.append("x" if cell in unused else
                        "#" if cell in free and cell in occupied else
                        "o" if cell in free else
                        ".")
        out.append("      " + " ".join(line))
    return ("\n".join(out) +
            "\n      (# open+filled, o open+empty, x locked empty, . locked)")

def main():
    """Entry point for parsing arguments and running the CP-SAT processing loop."""
    ap = argparse.ArgumentParser(description="Eternity II Break Minimizer using OR-Tools CP-SAT.")
    ap.add_argument("seed", help="Text file containing piece definitions")
    ap.add_argument("partials", help="Input CSV of partial board states")
    ap.add_argument("output", help="Output CSV for topped-out results")

    # Processing Options
    ap.add_argument("--row",        type=int,   default=0, help="First row of the CSV to process (0-indexed)")
    ap.add_argument("--count",      type=int,   default=1, help="Number of rows to process before exiting")

    # Spatial Options
    ap.add_argument("--side",       default="T", choices=sorted(SIDES),
                    help="Which border band(s) to open: T B R L (one side), "
                         "TR TL BR BL (L-shaped corner), TB (both horizontal "
                         "borders), LR (both vertical borders).")
    ap.add_argument("--work-rows",  type=int,   default=4, help="Depth of EACH open band, in rows (T/B) or columns (R/L).")
    ap.add_argument("--holes", default=None,
                    help="16x16 0/1 mask of movable cells (see data/holes_*.csv). "
                         "Replaces --side / --work-rows and is taken literally; "
                         "cannot be combined with --unused_rows.")
    ap.add_argument("--unused_rows", "--unused_top_rows", type=int, default=0, dest="unused_rows",
                    help="Outermost rows/cols of each band to unset (999) and lock empty.")

    # Heuristic & Output Options
    ap.add_argument("--report_best", type=int, default=1, help="Beam Search: Output N distinct near-optimal boards per input board.")
    ap.add_argument("--beam_diff",  type=int,   default=4, help="Beam Search: minimum cells by which ranks must differ.")
    ap.add_argument("--beam_slack", type=int,   default=1, help="Beam Search: extra breaks a lower rank may cost.")
    ap.add_argument("--allow_break_increase", action="store_true", help="Smart Scoring: Allows more total breaks if pushed further.")
    ap.add_argument("--tag_id", action="store_true", help="Also append _<score> to each output config_id (legacy naming).")

    # Solver Options
    ap.add_argument("--max-time",   type=float, default=300.0, help="Absolute max time allowed per board (seconds)")
    ap.add_argument("--stall-time", type=float, default=120.0, help="Stop solver if no improvements seen in X seconds")
    ap.add_argument("--workers",    type=int,   default=8, help="CP-SAT thread count")
    ap.add_argument("--rng-seed",   type=int,   default=-1, help="Random seed (default: OS entropy)")
    ap.add_argument("--verbose",    action="store_true", help="Print detailed solver telemetry")
    args = ap.parse_args()
    
    if not (1 <= args.work_rows <= SIDE): sys.exit(f"[ERROR] --work-rows must be in 1..{SIDE}.")
    if not (0 <= args.unused_rows < args.work_rows):
        sys.exit(f"[ERROR] --unused_rows must be in 0..{args.work_rows - 1} (below --work-rows).")
    holes = None
    if args.holes:
        if args.unused_rows:
            sys.exit("[ERROR] --unused_rows locks the outer part of a --side band; "
                     "with --holes there is no band. Draw the mask you want instead.")
        holes = read_holes_file(args.holes)
        if not holes:
            sys.exit(f"[ERROR] --holes {args.holes} marks no cell as movable.")
    if args.report_best < 1: sys.exit("[ERROR] --report_best must be >= 1.")
    if args.beam_diff < 1: sys.exit("[ERROR] --beam_diff must be >= 1.")
    if args.beam_slack < 0: sys.exit("[ERROR] --beam_slack must be >= 0.")

    master = args.rng_seed if args.rng_seed >= 0 else int.from_bytes(os.urandom(4), "little")
    random.seed(master)
    tiles = read_seed(args.seed)
    signal.signal(signal.SIGINT, _request_stop)

    print("\n=== E555 corners ===\n")
    for arg, value in vars(args).items():
        print(f"[cfg] {arg} = {value}")
    if holes is not None:
        print(f"[cfg] open cells: {len(holes)} from the --holes mask "
              "(--side / --work-rows do not apply)")
    else:
        print(f"[cfg] open bands: {' + '.join(SIDES[args.side])}, {args.work_rows} deep"
              + (f", outermost {args.unused_rows} locked empty" if args.unused_rows else ""))
    print("[init] starting process loop...", flush=True)

    script_start_time = time.time()
    processed_count = 0
    skipped_count = 0
    short_beams = 0

    # Junctions that can still be matched at all: the ones with both endpoints
    # outside the deliberately-emptied region. The CSV score keeps counting all
    # 480 (unplaced cells count as breaks, as everywhere in Stage C); this is
    # only the honest denominator for the printed telemetry.
    unused_all = side_cells(args.side, args.unused_rows) if args.unused_rows else set()
    max_score = sum(1 for a, b, _da, _db in ALL_JUNCTIONS
                    if a not in unused_all and b not in unused_all)

    with open(args.partials) as src, open(args.output, "w", newline="") as out:
        # lineterminator: the csv module defaults to CRLF, which every other
        # writer in the toolkit overrides.
        writer = csv.writer(out, lineterminator="\n")
        reader = csv.reader(src)
        # --row/--count count BOARD rows, not raw CSV lines. bin/E555_backtracker
        # prefixes its output with two '#' header lines, and counting those made
        # the default `--row 0 --count 1` select a comment and process nothing --
        # the same defect as parsing one, one step later. Skipping them here also
        # keeps the promise the whole pipeline rests on: any tool's output feeds
        # any tool's input.
        boards = (row for row in reader
                  if row and row[0].strip()
                  and not row[0].lstrip().startswith(("#", "%")))
        iterator = itertools.islice(boards, args.row, args.row + args.count)

        for i, row in enumerate(iterator, args.row):
            if _STOP: break
            partial = parse_partial_line(row)
            free, unused = build_free(partial.pos, partial.rot, tiles,
                                      args.side, args.work_rows, args.unused_rows,
                                      holes)

            # Lift every piece sitting in the locked-empty region: those cells
            # stay at 999 for this run, and their pieces rejoin the free pool.
            for p in range(NUM_PIECES):
                if partial.pos[p] in unused:
                    partial.pos[p], partial.rot[p] = CSV_UNPLACED, 0

            B0, _, _, md0 = board_metrics(partial.pos, partial.rot, tiles)
            start_score = NUM_EDGES - B0

            print(f"[{i:04d}] ID: {partial.config_id:<12} | Score: {start_score}/{max_score} (Breaks: {B0:<3}) | Free Cells: {len(free):<3}... ", end="", flush=True)
            if args.verbose:
                print()
                print(band_map(free, unused, partial.pos), flush=True)

            # Fast path: nothing inside the open region can be improved.
            occupied = {partial.pos[p] for p in range(NUM_PIECES) if partial.pos[p] != CSV_UNPLACED}
            broken = {c for pair in broken_junctions(partial.pos, partial.rot, tiles) for c in pair}
            if not (free & (broken | (set(range(NUM_PIECES)) - occupied))):
                results, reason, sec = [(partial.pos, partial.rot)], "clean", 0.0
                skipped_count += 1
            else:
                results, reason, sec = solve_frontier(partial.pos, partial.rot, tiles, free, args, args.verbose)
                if len(results) < args.report_best:
                    short_beams += 1

            # Output the top N configurations found
            for rank, (pos, rot) in enumerate(results, 1):
                B, _, _, _ = board_metrics(pos, rot, tiles)
                score = NUM_EDGES - B
                
                # Print terminal info for the Rank 1 (Best) solution
                if rank == 1:
                    if args.verbose:
                        print(f"      -> Best in {sec:.1f}s ({reason}) | Final Score: {score}/{max_score} | breaks left: {B}")
                    else:
                        print(f"Time:{sec:>5.1f}s ({reason}) | Final Score: {score}/{max_score} (Breaks: {B})")

                # Canonical output row: config_id, score, pos[256], rot[256].
                # The id keeps its provenance: `#<row>` marks the input row it
                # came from (unless already tagged), `_r<rank>` separates the
                # ranks of a beam, and --tag_id restores the legacy `_<score>`.
                cid = partial.config_id if '#' in partial.config_id \
                    else f"{partial.config_id}#{i}"
                if args.report_best > 1:
                    cid += f"_r{rank}"
                if args.tag_id:
                    cid += f"_{score}"
                out_row = [cid, str(score)]
                out_row += [str(x) for x in pos] + [str(x) for x in rot]
                
                # Resilient Write (catches transient NFS/Lustre I/O errors on clusters)
                try:
                    writer.writerow(out_row)
                    out.flush()
                except IOError as e:
                    print(f"\n      [!] Storage I/O Error: {e}. Retrying write in 2s...", flush=True)
                    time.sleep(2)
                    writer.writerow(out_row)
                    out.flush()
                    
            processed_count += 1
            
    total_time = time.time() - script_start_time
    print("\n=== run summary ===")
    print(f"[sum] {processed_count} boards processed in {total_time:.1f}s"
          + (f" ({skipped_count} already clean, no solve)" if skipped_count else ""))
    if short_beams:
        print(f"[sum] {short_beams} board(s) emitted fewer than {args.report_best} ranks: "
              f"no board >= {args.beam_diff} cells different stayed within "
              f"{args.beam_slack} extra break(s). Raise --beam_slack for a wider beam.")

if __name__ == "__main__":
    main()
