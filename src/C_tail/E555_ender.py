#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
E555_ender.py -- Stage C closer: local CP-SAT repair, two neighbourhoods.

WHAT IT DOES

    Takes a FULL (256-placed) board that still has a handful of breaks -- the
    kind the topper hands over -- and re-solves a slice of it with OR-Tools
    CP-SAT to drive the breaks toward zero. It never returns a board worse than
    the one it was given.

    It opens the whole outer border ring (60 cells) plus a set of interior
    cells, caps how many pieces may actually move, and minimises breaks. Two
    --mode choices decide WHICH interior cells open and what the objective is:

      --mode patch  (default)  Localised repair. The interior cells are a box
                    around the current breaks. The objective minimises breaks
                    and then COMPACTS them (shrinks the broken region and its
                    perimeter), so it both heals and tidies. --holes replaces
                    the box with an explicit movable-cell mask.

      --mode ring   Whole-border sweep. The interior cells are every cell within
                    --reach BFS layers of a break. The objective is pure break
                    count, kept lean so the big border model stays fast, and a
                    break can heal by cascading all the way around the ring (the
                    "avalanche").

    Either way the tool climbs an escalation ladder -- it re-solves from cheap,
    surgical openings (few pieces move, shallow reach) to broad ones, warm-
    starting each rung from the best board so far and stopping the instant
    breaks reach zero. You set only the ceilings (--reach, --max_changes).

EXAMPLE RUNS

    # Default: tidy up whatever breaks a toppered board still has.
    python3 E555_ender.py seed_Edge5.txt topped.csv fixed.csv --verbose

    # Whole-border sweep -- best when breaks sit on/near the border ring.
    python3 E555_ender.py seed_Edge5.txt topped.csv solved.csv \
            --mode ring --reach 2 --max_changes 16 --threads 8

    # Patch only the cells named in a 16x16 0/1 mask (see data/holes_*.csv).
    python3 E555_ender.py seed_Edge5.txt board.csv out.csv \
            --holes data/holes_open_border_TBLR.csv

CLUE PIECES

    --clue_center and --clue_corners hold the published Eternity II hint
    pieces at their cells and spins, so a board that arrives with its clues
    intact does not leave with them permuted. Without them this tool treats a
    clue like any other piece: --mode ring opens the whole border ring, and
    the centre clue sits inside any interior break box.

    Orientation is not a choice -- a solution rotated 90 degrees satisfies
    every edge rule but moves the clues, so a board commits to one of the four
    when its first clue is placed. --clue_orient auto (the default) reads that
    commitment off the input board; only a board carrying no clue at all needs
    an explicit --clue_orient 0..3.

    Clues change two things beyond the pins. The pool gains the cells a
    displaced clue needs (its target cell and the cell its piece is in now),
    since a permutation repair cannot place a piece it has not opened; and the
    never-worse guard becomes lexicographic, clues before breaks, because a
    clue repair is often break-neutral and a break-only test would discard it.

INPUT / OUTPUT

    Input : any canonical E555 board CSV; pos/rot are read from the row tail,
            so extra leading metadata columns are tolerated. Meant for full
            boards -- interior cells that are still empty are simply left out
            of the openable pool (this is a closer, not a filler; use the
            topper or the backtracker to fill first).
    Output: canonical rows  config_id, score, pos[256], rot[256]  with
            score = 480 - breaks. The input id is passed through unchanged.
"""

from __future__ import annotations
import argparse, csv, collections, itertools, random, signal, sys, threading, time
from dataclasses import dataclass
from pathlib import Path
try:
    from ortools.sat.python import cp_model
except ImportError:
    sys.exit("E555_ender.py needs OR-Tools, which is the one non-stdlib\n"
             "dependency in the toolkit:  pip install ortools")

_STOP = False
def _request_stop(signum, frame):
    global _STOP
    _STOP = True
    print("\n[Ctrl-C] finishing current stage then stopping...", flush=True)

# ---------------------------------------------------------------------------
# geometry (identical semantics to the rest of the E555 toolkit)
# ---------------------------------------------------------------------------
SIDE, NUM_PIECES, NUM_EDGES, GREY, CSV_UNPLACED = 16, 256, 480, 0, 999
NORTH, EAST, SOUTH, WEST = 0, 1, 2, 3

CORNER_CELLS = frozenset({0, SIDE - 1, (SIDE - 1) * SIDE, SIDE * SIDE - 1})      # BL BR TL TR
BORDER_CELLS = frozenset(c for c in range(NUM_PIECES)
                         if c // SIDE in (0, SIDE - 1) or c % SIDE in (0, SIDE - 1))
# the four inner cells diagonally inside each corner: always worth opening with
# the corner so a corner can be re-threaded together with its inner neighbour.
CORNER_ADJACENT_INNER = frozenset({SIDE + 1, 2 * SIDE - 2,
                                   (SIDE - 2) * SIDE + 1, (SIDE - 1) * SIDE - 2})
MAX_INNER_POOL = 60            # hard cap on interior candidate cells (model stays sane)
LADDER_START, LADDER_STEP = 4, 4   # change-budget ladder (was --changes-start/-step)


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

# Every interior junction, as (cell_a, cell_b, side_of_a, side_of_b).
ALL_JUNCTIONS = []
for _cell in range(NUM_PIECES):
    _r, _c = _cell // SIDE, _cell % SIDE
    if _c + 1 < SIDE: ALL_JUNCTIONS.append((_cell, _cell + 1, EAST, WEST))
    if _r + 1 < SIDE: ALL_JUNCTIONS.append((_cell, _cell + SIDE, NORTH, SOUTH))

def broken_junctions(pos, rot, edges):
    """Junctions that are not satisfied, walking all 480 of them.

    A junction with an UNPLACED cell on either side counts as broken -- the
    same rule E555_topper.py and tools/E555_rank.py use ("an unplaced neighbour
    leaves the piece unsatisfied"). This tool used to count only junctions with
    both cells placed, which agrees on the full boards it is written for but
    silently flatters a partial one, so its score was not comparable with the
    score every other tool in the pipeline writes into field 2."""
    col = {pos[p]: rotate_edges(edges[p], rot[p])
           for p in range(NUM_PIECES) if pos[p] != CSV_UNPLACED}
    out = []
    for a, b, da, db in ALL_JUNCTIONS:
        if a in col and b in col and col[a][da] == col[b][db]:
            continue
        out.append((a, b))
    return out

def break_cells_of(pos, rot, edges):
    return {c for pair in broken_junctions(pos, rot, edges) for c in pair}

def read_holes_file(path):
    values = []
    with open(path) as fh:
        for line in fh:
            if not line.strip().startswith("#"):
                values.extend(line.replace(",", " ").split())
    return {i for i, v in enumerate(values) if int(v) == 1}

# ---------------------------------------------------------------------------
# candidate pool: the whole border ring + an interior set that depends on --mode
# ---------------------------------------------------------------------------
def _inner_bfs(seed_cells, reach, cap):
    """Interior cells within `reach` 4-neighbour layers of any seed cell,
    nearest first, capped. Seed cells at distance 0 are included."""
    dist = {c: 0 for c in seed_cells}
    dq = collections.deque(seed_cells)
    while dq:
        x = dq.popleft()
        if dist[x] >= reach:
            continue
        r, c = x // SIDE, x % SIDE
        for nr, nc in ((r - 1, c), (r + 1, c), (r, c - 1), (r, c + 1)):
            if 0 <= nr < SIDE and 0 <= nc < SIDE:
                y = nr * SIDE + nc
                if y not in dist:
                    dist[y] = dist[x] + 1
                    dq.append(y)
    inner = [c for c, d in dist.items() if c not in BORDER_CELLS]
    inner.sort(key=lambda c: (dist[c], c))
    return set(inner[:cap])

def _inner_box(break_cells, reach, cap):
    """The interior bounding box of the breaks, expanded outward by `reach`
    layers (the old endgame localiser)."""
    seeds = {c for c in break_cells if c not in BORDER_CELLS}
    if not seeds:   # breaks sit only on the border: seed from their inner neighbours
        seeds = {c + d for c in break_cells for d in (SIDE, -SIDE, 1, -1)
                 if 0 <= c + d < NUM_PIECES and (c + d) not in BORDER_CELLS}
    if not seeds:
        return set()
    rows = [c // SIDE for c in seeds]; cols = [c % SIDE for c in seeds]
    bbox = {r * SIDE + c
            for r in range(min(rows), max(rows) + 1)
            for c in range(min(cols), max(cols) + 1)
            if (r * SIDE + c) not in BORDER_CELLS}
    return _inner_bfs(bbox, reach, cap)

def build_pool(mode, break_cells, reach, holes, placed, clue_open=()):
    """The set of openable, currently-placed cells for one rung.

    `clue_open` names cells a clue repair needs open: the cell a displaced clue
    must end up in, and the cell its piece currently sits in. This tool is a
    permutation repair -- the piece domains are exactly the pieces already in
    the pool -- so a clue whose piece is outside the pool cannot be pinned at
    all, and pinning it anyway would make the model INFEASIBLE instead of
    merely leaving the clue unsatisfied. Only clues that are actually wrong
    contribute cells, so a board with its clues already in place opens nothing
    extra. This is the one thing that widens a --holes mask, and only by the
    handful of cells the clue you asked for cannot do without.
    """
    if holes is not None:
        pool = set(holes)
    else:
        inner = (_inner_box(break_cells, reach, MAX_INNER_POOL) if mode == "patch"
                 else _inner_bfs(break_cells, reach, MAX_INNER_POOL))
        pool = set(BORDER_CELLS) | inner
        if mode == "patch":
            pool |= CORNER_ADJACENT_INNER
    pool |= set(clue_open)
    return {c for c in pool if c in placed}          # a closer: skip empty cells


def clue_open_cells(CL, mask, orient, pos, rot):
    """Cells build_pool must open so the wrong clues of `orient` can be fixed.

    A clue already at its cell and spin contributes nothing: leaving it locked
    keeps the model small and spends none of the --max_changes budget. The
    donor cell is only offered when it is an interior cell, which it always is
    on a well-formed board (every clue piece is an inner piece); the guard just
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

def draw_pool(free, break_cells):
    """ASCII map of the open pool, row 15 (top) first, matching the viewer."""
    print(f"      >> open pool: {len(free)} cells")
    for r in range(SIDE - 1, -1, -1):
        row = []
        for c in range(SIDE):
            cell = r * SIDE + c
            row.append("#" if cell in free and cell in BORDER_CELLS
                       else "&" if cell in free
                       else "x" if cell in break_cells else ".")
        print("      " + " ".join(row))
    print("      (# ring open, & inner open, x break-locked, . locked)\n")

# ---------------------------------------------------------------------------
# one CP-SAT stage: open `free_cells`, move at most `max_changes`, cut breaks
# ---------------------------------------------------------------------------
# compact-objective weights: minimise  W_BREAK*breaks + W_AREA*area + perimeter,
# strictly lexicographic (breaks >> broken-region area >> its perimeter).
W_AREA = NUM_EDGES + 1                      # 481  > max perimeter (<= 480)
W_BREAK = W_AREA * (NUM_PIECES + 1)         # dominates area*256 + perimeter

def solve_stage(pos, rot, tiles, free_cells, max_changes, compact,
                max_time, stall_time, workers, rseed, verbose, pins=()):
    model = cp_model.CpModel()
    maxc = max(max(t) for t in tiles)
    piece_at = {pos[p]: p for p in range(NUM_PIECES) if pos[p] != CSV_UNPLACED}

    corner_cells = [c for c in free_cells if c in CORNER_CELLS]
    edge_cells   = [c for c in free_cells if c in BORDER_CELLS and c not in CORNER_CELLS]
    inner_cells  = [c for c in free_cells if c not in BORDER_CELLS]
    corner_dom = sorted(piece_at[c] for c in corner_cells)
    edge_dom   = sorted(piece_at[c] for c in edge_cells)
    inner_dom  = sorted(piece_at[c] for c in inner_cells)

    def domain_of(cell):
        if cell in CORNER_CELLS: return corner_dom
        if cell in BORDER_CELLS: return edge_dom
        return inner_dom

    piece_of, rot_of, color_of = {}, {}, {}
    for cell in sorted(free_cells):
        r, c = cell // SIDE, cell % SIDE
        dom = domain_of(cell)
        pv = model.NewIntVarFromDomain(cp_model.Domain.FromValues(dom), f"pc_{cell}")
        rv = model.NewIntVar(0, 3, f"ro_{cell}")
        nv, ev, sv, wv = (model.NewIntVar(0, maxc, f"{x}_{cell}") for x in "NESW")
        allowed = [(p, s) + rotate_edges(tiles[p], s)
                   for p in dom for s in range(4)
                   if frame_rule_ok(r, c, rotate_edges(tiles[p], s))]
        model.AddAllowedAssignments([pv, rv, nv, ev, sv, wv], allowed)
        piece_of[cell], rot_of[cell], color_of[cell] = pv, rv, (nv, ev, sv, wv)

    for cls in (corner_cells, edge_cells, inner_cells):        # 3 commodities
        if len(cls) > 1:
            model.AddAllDifferent([piece_of[c] for c in cls])

    # Hard clue pins. Every clue cell is an interior cell and every clue piece
    # is an inner piece, so this only ever touches the `inner` commodity, and
    # the AddAllDifferent above already bars a pinned piece from every other
    # pool cell -- fixing a clue therefore costs no extra variable and leaves
    # the objective untouched. Repairing a displaced clue does spend at least
    # two of `max_changes` (the clue cell and its donor), so the cheapest rungs
    # of the ladder may come back infeasible on a clue-broken board; the ladder
    # simply climbs to a wider budget, which is the behaviour it already has
    # for any repair too big for the current rung.
    for cell, piece, spin in pins:
        model.Add(piece_of[cell] == piece)
        model.Add(rot_of[cell] == spin)

    # change budget: at most `max_changes` cells differ from the incumbent
    kept = []
    for cell in free_cells:
        p0 = piece_at[cell]
        k = model.NewBoolVar(f"kept_{cell}")
        model.Add(piece_of[cell] == p0).OnlyEnforceIf(k)
        model.Add(rot_of[cell] == rot[p0]).OnlyEnforceIf(k)
        model.AddHint(k, 1)                                    # safe: no forced disruption
        kept.append(k)
    if kept:
        model.Add(sum(kept) >= len(free_cells) - max_changes)

    fixed_col = {c: rotate_edges(tiles[piece_at[c]], rot[piece_at[c]])
                 for c in range(NUM_PIECES) if c not in free_cells and c in piece_at}
    def ecolor(cell, d):
        if cell in color_of: return color_of[cell][d]
        if cell in fixed_col: return fixed_col[cell][d]
        return None                                            # empty neighbour: no constraint

    break_terms, const_breaks, junctions = [], 0, []
    for cell in range(NUM_PIECES):
        r, c = cell // SIDE, cell % SIDE
        for nbr, da, db in ((cell + 1, EAST, WEST), (cell + SIDE, NORTH, SOUTH)):
            if da == EAST and c + 1 >= SIDE:  continue
            if da == NORTH and r + 1 >= SIDE: continue
            ac, bc = ecolor(cell, da), ecolor(nbr, db)
            if ac is None or bc is None:
                continue                                       # junction touches an empty cell
            if isinstance(ac, int) and isinstance(bc, int):    # both fixed -> constant
                if ac != bc:
                    const_breaks += 1
                    junctions.append((cell, nbr, True))
                continue
            brk = model.NewBoolVar(f"brk_{cell}_{nbr}")
            model.Add(ac == bc).OnlyEnforceIf(brk.Not())
            model.Add(ac != bc).OnlyEnforceIf(brk)
            break_terms.append(brk)
            junctions.append((cell, nbr, brk))

    if compact:
        # secondary objective: shrink the broken region (area) and its perimeter.
        shows_break = {c: model.NewBoolVar(f"sb_{c}") for c in range(NUM_PIECES)}
        boundary = []
        for ca, cb, m in junctions:
            if m is True:                                      # constant break
                model.Add(shows_break[ca] == 1); model.Add(shows_break[cb] == 1)
            else:
                model.AddImplication(m, shows_break[ca]); model.AddImplication(m, shows_break[cb])
            b = model.NewBoolVar(f"be_{ca}_{cb}")
            model.Add(shows_break[ca] != shows_break[cb]).OnlyEnforceIf(b)
            model.Add(shows_break[ca] == shows_break[cb]).OnlyEnforceIf(b.Not())
            boundary.append(b)
        model.Minimize(W_BREAK * (const_breaks + sum(break_terms))
                       + W_AREA * sum(shows_break.values()) + sum(boundary))
    else:
        model.Minimize(sum(break_terms))                       # pure break count (const is constant)

    for cell in free_cells:                                    # hint current arrangement
        p = piece_at[cell]
        model.AddHint(piece_of[cell], p)
        model.AddHint(rot_of[cell], rot[p])
        for d, v in enumerate(color_of[cell]):
            model.AddHint(v, rotate_edges(tiles[p], rot[p])[d])

    solver = cp_model.CpSolver()
    solver.parameters.num_search_workers = workers
    solver.parameters.linearization_level = 1
    solver.parameters.symmetry_level = 0
    solver.parameters.random_seed = rseed & 0x7fffffff
    solver.parameters.max_time_in_seconds = float(max_time)

    def breaks_of(objval):
        return (objval // W_BREAK) if compact else (const_breaks + objval)

    class _Tracker(cp_model.CpSolverSolutionCallback):
        def __init__(self):
            cp_model.CpSolverSolutionCallback.__init__(self)
            self._t0 = self.last = time.monotonic()
        def on_solution_callback(self):
            self.last = time.monotonic()
            total = breaks_of(int(round(self.ObjectiveValue())))
            if verbose:
                print(f"        [inc] t={self.last-self._t0:5.1f}s | breaks={total}", flush=True)
            if total == 0:                                     # solved puzzle -> stop at once
                self.StopSearch()

    tracker = _Tracker()
    done, state = threading.Event(), {"stalled": False}
    def watchdog():
        while not done.wait(0.5):
            if stall_time > 0 and time.monotonic() - tracker.last > stall_time:
                state["stalled"] = True; solver.StopSearch(); return
    w = threading.Thread(target=watchdog, daemon=True); w.start()
    status = solver.Solve(model, tracker)
    done.set(); w.join()

    best_pos, best_rot = list(pos), list(rot)
    if status in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        for cell in free_cells:
            p = solver.Value(piece_of[cell])
            best_pos[p], best_rot[p] = cell, solver.Value(rot_of[cell])

    out_breaks = len(broken_junctions(best_pos, best_rot, tiles))
    rsn = ("optimal" if status == cp_model.OPTIMAL else
           "stalled" if state["stalled"] else
           "max-time" if status == cp_model.FEASIBLE else "no-sol")
    if verbose:
        print("        ---- CP-SAT stats ----", flush=True)
        for line in solver.ResponseStats().strip().splitlines():
            print(f"        {line}", flush=True)
    # never-worse guard: keep the incumbent unless this stage strictly improved.
    if status not in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        return list(pos), list(rot), len(broken_junctions(pos, rot, tiles)), "no-sol", solver.WallTime()
    return best_pos, best_rot, out_breaks, rsn, solver.WallTime()

# ---------------------------------------------------------------------------
# escalation ladder: (reach, budget) rungs, cheap -> broad
# ---------------------------------------------------------------------------
def make_ladder(reach, cmax):
    rungs = []
    for r in range(1, reach + 1):
        m, emitted = LADDER_START, False
        while m <= cmax:
            rungs.append((r, m)); emitted = True; m += LADDER_STEP
        if not emitted:
            rungs.append((r, cmax))
    return rungs

def solve_board(mode, partial, tiles, ladder, reach, holes,
                max_time, stall_time, workers, base_seed, verbose,
                CL=None, clue_mask=None, orient=None):
    pos, rot = list(partial.pos), list(partial.rot)
    placed = {pos[p] for p in range(NUM_PIECES) if pos[p] != CSV_UNPLACED}
    breaks = len(broken_junctions(pos, rot, tiles))
    clues_on = clue_mask and orient is not None

    def clue_hits(p, r):
        """Enabled clues of `orient` sitting at their cell and spin."""
        if not clues_on:
            return 0
        return sum(1 for cell, piece, spin in CL.clue_list(orient, clue_mask)
                   if p[piece] == cell and r[piece] == spin)

    # A break-free board is still work if a clue is displaced and reachable.
    if breaks == 0 and not (clues_on and clue_open_cells(CL, clue_mask, orient, pos, rot)):
        return pos, rot, 0, "input-solved", "-", 0.0

    # reachability check at the widest opening: a break with BOTH endpoints
    # outside the max pool cannot be healed here.
    bc0 = break_cells_of(pos, rot, tiles)
    free_max = build_pool(mode, bc0, reach, holes, placed,
                          clue_open_cells(CL, clue_mask, orient, pos, rot) if clues_on else ())
    unreachable = sum(1 for a, b in broken_junctions(pos, rot, tiles)
                      if a not in free_max and b not in free_max)
    if unreachable:
        print(f"      [WARN] {unreachable} break(s) outside the widest pool "
              f"({mode} mode); best achievable here >= {unreachable}.", flush=True)

    total_t, stage_tag, reason = 0.0, "-", "exhausted"
    for k, (r, m) in enumerate(ladder):
        if _STOP:
            reason = "interrupted"; break
        bc = break_cells_of(pos, rot, tiles)               # re-centre on current breaks
        # Both the pool and the pins are rebuilt every rung: the board changes
        # under us, so a clue fixed by an earlier rung stops asking for cells.
        pins = ()
        if clues_on:
            free = build_pool(mode, bc, r, holes, placed,
                              clue_open_cells(CL, clue_mask, orient, pos, rot))
            pins, locked_ok, skipped = CL.clue_pins(pos, rot, free, orient,
                                                    clue_mask, unplaced_ok=False)
            note = "; ".join(why for _c, _p, _s, why in skipped)
            print(f"      [clue] orient={orient} | pin {len(pins)} | "
                  f"locked-ok {locked_ok} | skip {len(skipped)}"
                  + (f" ({note})" if note else ""), flush=True)
        else:
            free = build_pool(mode, bc, r, holes, placed)
        if verbose:
            draw_pool(free, bc)
        npos, nrot, nb, rsn, sec = solve_stage(
            pos, rot, tiles, free, m, mode == "patch",
            max_time, stall_time, workers, base_seed + k, verbose, pins)
        total_t += sec
        tag = "   *** SOLVED 480/480 ***" if nb == 0 else ""
        print(f"      [r{r} m{m:<2}] {breaks:>3} -> {nb:>3} | {rsn:<8} | "
              f"{sec:5.1f}s | pool={len(free)}{tag}", flush=True)
        # Never-worse guard. With clues on this is lexicographic -- clues first,
        # then breaks -- because a clue repair is usually break-neutral or
        # slightly break-costly, and a plain `nb < breaks` test would throw the
        # repaired board away and keep the one with the clue in the wrong place.
        if (clue_hits(npos, nrot), -nb) > (clue_hits(pos, rot), -breaks):
            pos, rot, breaks = npos, nrot, nb
        stage_tag = f"r{r}m{m}"
        if breaks == 0 and not (clues_on and clue_open_cells(CL, clue_mask, orient, pos, rot)):
            reason = "solved"; break
        reason = rsn
    return pos, rot, breaks, reason, stage_tag, total_t

# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Stage C closer: local CP-SAT repair of a full board, "
                    "with a localized patch mode (default) or a whole-border "
                    "ring-sweep mode.")
    ap.add_argument("seed"); ap.add_argument("partials"); ap.add_argument("output")
    ap.add_argument("--mode", choices=("patch", "ring"), default="patch",
                    help="patch = localized repair around the breaks (default); "
                         "ring = whole-border escalation sweep")
    ap.add_argument("--holes", default=None,
                    help="patch mode: 16x16 0/1 mask of movable cells "
                         "(overrides the automatic box)")
    ap.add_argument("--reach", type=int, default=2,
                    help="interior layers opened around the breaks (ladder ceiling)")
    ap.add_argument("--max_changes", type=int, default=16,
                    help="ceiling on pieces that may move per solve")
    ap.add_argument("--time_limit", type=float, default=120.0, help="seconds per stage solve")
    ap.add_argument("--stall_time", type=float, default=40.0, help="no-improvement cutoff per stage")
    ap.add_argument("--threads", type=int, default=8, help="CP-SAT workers")
    ap.add_argument("--rng_seed", type=int, default=0,
                    help="base seed; rung k uses seed+k (0 = auto-random)")
    ap.add_argument("--start_row", type=int, default=0,
                    help="first input CSV row to process (0-indexed)")
    ap.add_argument("--num_rows", type=int, default=0,
                    help="how many rows to process (default 0 = every remaining "
                         "row); with --start_row, splits a board list across "
                         "array tasks")
    ap.add_argument("--clue_center", action="store_true",
                    help="Hold piece 138 at the board's centre clue cell and spin.")
    ap.add_argument("--clue_corners", action="store_true",
                    help="Hold the four corner clues at their cells and spins.")
    ap.add_argument("--clue_orient", default="auto",
                    choices=("auto", "0", "1", "2", "3"),
                    help="Which of the four board orientations the clues follow; "
                         "auto (default) reads it off each input board.")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    # 0 = auto-random, matching the rest of the toolkit; resolved here so the
    # [cfg] dump below reports the seed the run actually used.
    if args.rng_seed == 0:
        args.rng_seed = random.randint(1_000_000, 9_999_999)

    if args.reach < 1:
        sys.exit("[ERROR] --reach must be >= 1.")
    holes = read_holes_file(args.holes) if args.holes else None
    if holes is not None and args.mode == "ring":
        print("[note] --holes is a patch-mode mask; ignoring it under --mode ring.", flush=True)
        holes = None
    # With a fixed hole mask the pool does not grow with reach, so climb the
    # change-budget only (reach fixed at 1).
    ladder = make_ladder(1 if holes is not None else args.reach, args.max_changes)
    CL = clue_mask = None
    if args.clue_center or args.clue_corners:
        CL = load_clues()
        clue_mask = ((CL.CLUE_CENTER if args.clue_center else 0)
                     | (CL.CLUE_CORNERS if args.clue_corners else 0))
    tiles = read_seed(args.seed)
    signal.signal(signal.SIGINT, _request_stop)

    print("\n=== E555 ender ===\n")
    for arg, value in vars(args).items():
        print(f"[cfg] {arg} = {value}")

    run_start = time.time()
    done = gained = 0
    with open(args.partials) as src, open(args.output, "w", newline="") as out:
        # lineterminator: the csv module defaults to CRLF, which every other
        # writer in the toolkit overrides.
        writer = csv.writer(out, lineterminator="\n")
        reader = csv.reader(src)
        # --start_row/--num_rows so a board list can be split across array tasks,
        # exactly as E555_topper.py slices its input: BOARD rows, not raw CSV
        # lines, so the '#' header bin/E555_backtracker writes neither breaks
        # the parse nor eats a slot in the slice. --num_rows 0 means every
        # remaining row from --start_row.
        boards = (row for row in reader
                  if row and row[0].strip()
                  and not row[0].lstrip().startswith(("#", "%")))
        stop = None if args.num_rows == 0 else args.start_row + args.num_rows
        rows = itertools.islice(boards, args.start_row, stop)
        for i, row in enumerate(rows, args.start_row + 1):
            if _STOP: break
            partial = parse_partial_line(row)
            bin_ = len(broken_junctions(partial.pos, partial.rot, tiles))
            print(f"\n[{i}] {partial.config_id} | input breaks {bin_}", flush=True)

            orient = None
            if clue_mask:
                if args.clue_orient == "auto":
                    orient, _n = CL.clue_orient(partial.pos, partial.rot, clue_mask)
                else:
                    orient = int(args.clue_orient)
                if orient is None:
                    print("      [WARN] board carries no clue: cannot tell which "
                          "orientation to impose; pass --clue_orient 0..3. "
                          "Solving this board unpinned.", flush=True)

            pos, rot, breaks, reason, stage, sec = solve_board(
                args.mode, partial, tiles, ladder, args.reach, holes,
                args.time_limit, args.stall_time, args.threads, args.rng_seed, args.verbose,
                CL, clue_mask, orient)

            score = NUM_EDGES - breaks
            out_row = [partial.config_id, str(score)] + [str(x) for x in pos] + [str(x) for x in rot]
            writer.writerow(out_row); out.flush()
            done += 1
            gained += bin_ - breaks
            print(f"[{i}] Score: {score}/{NUM_EDGES} | Breaks: {breaks} | "
                  f"Stage: {stage} | End: {reason} | Time: {sec:.1f}s", flush=True)

    print("\n=== run summary ===")
    print(f"[sum] {done} board(s) in {time.time() - run_start:.1f}s, "
          f"{gained:+d} break(s) net -> {args.output}")
    if _STOP:
        print("[sum] stopped early on Ctrl-C; the boards already written are complete")

if __name__ == "__main__":
    main()
