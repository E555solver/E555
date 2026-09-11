#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
E555_ender.py -- adaptive Stage C CP-SAT closer for full E555 boards.

NORMAL USE

    python3 E555_ender.py seed.txt boards.csv output.csv
    python3 E555_ender.py seed.txt elites.csv output.csv --profile deep
    python3 E555_ender.py seed.txt elites.csv output.csv \
        --profile superdeep --board_time_limit 36000

The default ``overnight`` profile is intended for a large board corpus.  It
first tries several small break-centered neighborhoods, then spends the rest of
the true per-board budget on progressively broader models.  ``deep`` and
``superdeep`` add larger Hamming radii, independent random restarts, and whole-
frame escalation.  Singleton donor cells are disabled: an isolated donor
usually creates a four-sided source cavity and measured worse than connected
repair neighborhoods.

SEARCH MODES

``--search_mode improve`` is the intended mode.  Every CP-SAT call receives the
hard target ``output breaks <= current breaks - 1``.  The first feasible witness
is accepted, the neighborhood is rebuilt around the new damage, and descent
continues.  ``INFEASIBLE`` therefore means only that the exact bounded
neighborhood cannot improve; ``UNKNOWN`` means its call budget expired without
a witness or a proof.

``--search_mode optimize`` keeps the incumbent feasible and minimizes breaks,
collateral breaks, damaged-region compactness, and changed cells.  It is useful
for a small elite set or equal-break compaction, but it is deliberately not the
default for a large corpus.

BUDGETS

``--board_time_limit`` is the total wall-clock allowance for one input board,
including every focused pool, broad rung, restart, and post-improvement restart.
``--attempt_time`` is only a ceiling on one individual ``CpSolver.Solve()``
call.  Normally leave it unset so the selected profile can use short calls for
small models and longer calls for difficult broad models.  The board deadline
always wins.

SAFETY AND MODELING

The change budget is an exact Hamming radius over piece and rotation.  The
profile permits a small number of *collateral* new breaks (three by default),
because the supplied experiment showed that late improvements often require
moving damage before reducing it.  By default those new breaks are measured
against the original input board, so repeated accepted moves cannot accumulate
unbounded damage.  The strongest existing clean foundation is protected inside
the CP-SAT model, not merely checked after the first witness.

Input rows are read from their final 512 position/rotation fields, so legacy
leading metadata is accepted.  Output is canonical:
``config_id, score, pos[256], rot[256]`` with ``score = 480 - breaks``.
Ordinary output is one summary line per board; ``--verbose`` prints every model.
Use ``--show_advanced --help`` only when conducting a controlled experiment.
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

# Clockwise border ring, beginning at the bottom-left corner.  Local ring
# neighborhoods are arcs in this order rather than the whole 60-cell frame.
BORDER_RING = tuple(
    [c for c in range(SIDE)]
    + [r * SIDE + SIDE - 1 for r in range(1, SIDE)]
    + [(SIDE - 1) * SIDE + c for c in range(SIDE - 2, -1, -1)]
    + [r * SIDE for r in range(SIDE - 2, 0, -1)]
)
BORDER_INDEX = {cell: i for i, cell in enumerate(BORDER_RING)}

DEFAULT_INNER_CAP = 48
LADDER_START, LADDER_STEP = 4, 4


@dataclass(frozen=True)
class EffortProfile:
    """A small, opinionated search portfolio.

    ``attempt_seconds`` values are scaled when the user overrides the profile's
    default board budget.  They are caps, not reservations: an infeasible model
    or a found witness can finish much earlier.  The final board deadline is
    always authoritative.
    """
    board_seconds: float
    workers: int
    focus_share: float
    focus_limit: int
    focus_reach: int
    focus_inner_cap: int
    focus_ring_radius: int
    focus_changes: tuple
    focus_attempt_seconds: float
    global_plan: tuple
    inner_cap: int
    ring_radius: int
    max_clean_loss: int
    duplicate_policy: str


# Each global-plan entry is:
#   (border_scope, reach, max_changes, max_new_breaks, seconds, repeats)
#
# These defaults are deliberately conservative about structural damage.  The
# user's max_new_breaks=3 experiment showed that a little break relocation is
# essential, while singleton donor cells were pure cost.  The clean-foundation
# requirement is now a hard model constraint, so the first witness cannot spend
# the relocation allowance by poisoning the locked foundation.
EFFORT_PROFILES = {
    "overnight": EffortProfile(
        board_seconds=180.0,
        workers=4,
        focus_share=0.20,
        focus_limit=6,
        focus_reach=1,
        focus_inner_cap=20,
        focus_ring_radius=4,
        focus_changes=(4,),
        focus_attempt_seconds=6.0,
        global_plan=(
            ("local", 1, 4, 3, 50.0, 1),
            ("local", 1, 8, 3, 40.0, 1),
            ("local", 2, 12, 3, 45.0, 1),
            ("local", 3, 16, 3, 75.0, 1),
        ),
        inner_cap=48,
        ring_radius=8,
        max_clean_loss=1,
        duplicate_policy="reuse",
    ),
    "deep": EffortProfile(
        board_seconds=900.0,
        workers=8,
        focus_share=0.20,
        focus_limit=12,
        focus_reach=2,
        focus_inner_cap=32,
        focus_ring_radius=5,
        focus_changes=(4, 8),
        focus_attempt_seconds=15.0,
        global_plan=(
            ("local", 1, 4, 3, 120.0, 2),
            ("local", 1, 8, 3, 180.0, 2),
            ("local", 2, 12, 3, 240.0, 2),
            ("local", 3, 16, 4, 300.0, 2),
            ("local", 4, 20, 4, 450.0, 2),
            ("full", 2, 16, 4, 450.0, 1),
        ),
        inner_cap=72,
        ring_radius=10,
        max_clean_loss=1,
        duplicate_policy="rerun",
    ),
    "superdeep": EffortProfile(
        board_seconds=7200.0,
        workers=12,
        focus_share=0.15,
        focus_limit=24,
        focus_reach=2,
        focus_inner_cap=40,
        focus_ring_radius=6,
        focus_changes=(4, 8, 12),
        focus_attempt_seconds=45.0,
        global_plan=(
            ("local", 1, 4, 3, 300.0, 3),
            ("local", 1, 8, 3, 450.0, 3),
            ("local", 2, 12, 4, 720.0, 3),
            ("local", 3, 16, 4, 900.0, 4),
            ("local", 4, 20, 5, 1200.0, 4),
            ("local", 5, 24, 5, 1800.0, 4),
            ("full", 2, 16, 4, 1200.0, 2),
            ("full", 3, 24, 5, 1800.0, 3),
        ),
        inner_cap=96,
        ring_radius=14,
        max_clean_loss=1,
        duplicate_policy="rerun",
    ),
}


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

def _project_to_border(cells):
    """Border cells directly beside, or nearest to, a damage cluster."""
    out = {c for c in cells if c in BORDER_CELLS}
    for cell in cells:
        r, c = divmod(cell, SIDE)
        # A one-cell interior halo should be able to call on the adjacent frame.
        if r <= 1: out.add(c)
        if r >= SIDE - 2: out.add((SIDE - 1) * SIDE + c)
        if c <= 1: out.add(r * SIDE)
        if c >= SIDE - 2: out.add(r * SIDE + SIDE - 1)
    return out


def _local_border_arc(seed_cells, radius):
    """Union of radius-cell arcs around the frame sites nearest the damage."""
    seeds = _project_to_border(seed_cells)
    if not seeds:
        return set()
    n = len(BORDER_RING)
    out = set()
    for cell in seeds:
        i = BORDER_INDEX[cell]
        for d in range(-radius, radius + 1):
            out.add(BORDER_RING[(i + d) % n])
    return out


def build_pool(mode, break_cells, reach, holes, placed, clue_open=(), *,
               inner_cap=DEFAULT_INNER_CAP, patch_shape="cluster",
               border_scope="local", ring_radius=8):
    """Return the currently occupied cells opened by one neighborhood.

    The original ender always opened all 60 frame cells.  That turned a local
    repair into a 100--124-cell global permutation and allowed a one-break gain
    to move damage deep into a previously clean foundation.  Version 2 defaults
    to a frame arc near the damage; ``--border_scope full`` recovers the legacy
    whole-ring neighborhood.

    ``patch_shape=cluster`` grows from the actual broken cells.  The legacy
    bounding-box shape remains available, but no longer silently makes every
    cell in a large rectangle a distance-zero seed before the cap is applied.
    """
    if holes is not None:
        pool = set(holes)
    else:
        if mode == "patch" and patch_shape == "box":
            inner = _inner_box(break_cells, reach, inner_cap)
        else:
            inner = _inner_bfs(break_cells, reach, inner_cap)
        pool = set(inner)

        if border_scope == "full":
            pool |= BORDER_CELLS
        elif border_scope == "local":
            pool |= _local_border_arc(break_cells, ring_radius)
        elif border_scope != "none":
            raise ValueError(f"unknown border scope {border_scope!r}")

        if mode == "patch":
            # Only open a corner's diagonal inner cell when that corner itself
            # is in the chosen frame arc.  The old unconditional four-cell add
            # coupled otherwise independent corners into every patch model.
            for corner, inner_cell in zip(sorted(CORNER_CELLS),
                                          sorted(CORNER_ADJACENT_INNER)):
                if corner in pool:
                    pool.add(inner_cell)
    pool |= set(clue_open)
    return {c for c in pool if c in placed}          # a closer: skip empty cells


def build_focus_pools(broken_pairs, placed, *, reach, inner_cap,
                      ring_radius, limit, clue_open=()):
    """Build several small, overlapping repair neighborhoods.

    A global pool with 70--100 open cells and a four-change radius asks CP-SAT
    to discover *where* the four interesting cells are as well as how to repair
    them.  This routine moves that first decision outside the solver: one small
    pool is grown around each currently broken junction, duplicate pools are
    removed, and a greedy cover keeps spatially distinct pools that collectively
    touch as much of the damage as possible.  The broad global neighborhoods are
    still tried later, so this is an inexpensive front end, not a completeness
    claim.
    """
    if limit <= 0 or not broken_pairs:
        return []

    candidates = {}
    for idx, pair in enumerate(broken_pairs):
        seeds = set(pair)
        pool = build_pool(
            "patch", seeds, reach, None, placed, clue_open,
            inner_cap=inner_cap, patch_shape="cluster",
            border_scope="local", ring_radius=ring_radius)
        if len(pool) < 2:
            continue
        key = tuple(sorted(pool))
        touched = frozenset(
            j for j, (a, b) in enumerate(broken_pairs)
            if a in pool or b in pool)
        if not touched:
            continue
        # A duplicate geometry can arise from adjacent breaks.  Retain the
        # version with the larger touched set (normally they are identical).
        old = candidates.get(key)
        if old is None or len(touched) > len(old[1]):
            center = min(pair)
            candidates[key] = (set(pool), touched, center)

    remaining = set(range(len(broken_pairs)))
    chosen = []
    pool_list = list(candidates.values())
    while pool_list and len(chosen) < limit:
        def score(item):
            pool, touched, center = item
            new = len(touched & remaining)
            # New damage coverage first; then density and smaller models.
            return (new, len(touched) / max(1, len(pool)),
                    len(touched), -len(pool), -center)

        best = max(pool_list, key=score)
        pool_list.remove(best)
        pool, touched, _center = best
        if not (touched & remaining) and chosen:
            break
        chosen.append(pool)
        remaining.difference_update(touched)

    return chosen


def _neighbor_colors(cell, colors):
    """Yield (side_of_cell, required_color, neighbor_cell) for placed neighbors."""
    r, c = divmod(cell, SIDE)
    for side, nr, nc, other_side in (
            (NORTH, r + 1, c, SOUTH), (EAST, r, c + 1, WEST),
            (SOUTH, r - 1, c, NORTH), (WEST, r, c - 1, EAST)):
        if not (0 <= nr < SIDE and 0 <= nc < SIDE):
            continue
        nbr = nr * SIDE + nc
        if nbr in colors:
            yield side, colors[nbr][other_side], nbr


def enrich_with_donors(free, broken_pairs, pos, rot, tiles,
                       donors_per_target=0, donor_cells_max=0):
    """Add a bounded set of outside holder cells whose pieces fit damage sites.

    The original ender is a closed permutation neighborhood: a useful piece
    outside ``free`` can never enter, however long CP-SAT runs.  This heuristic
    ranks same-commodity outside pieces by their best orientation at each break
    cell, emphasizing colors imposed by neighbors that stay fixed.  Opening a
    donor's current holder cell preserves the permutation model and keeps the
    expansion auditable.  The global cell cap prevents one crowded break set
    from importing the whole board.
    """
    if donors_per_target <= 0 or donor_cells_max <= 0:
        return set(free), set()

    piece_at = {pos[p]: p for p in range(NUM_PIECES)
                if pos[p] != CSV_UNPLACED}
    colors = {cell: rotate_edges(tiles[p], rot[p])
              for cell, p in piece_at.items()}
    incidence = collections.Counter(c for pair in broken_pairs for c in pair)
    targets = sorted((c for c in incidence if c in free and c in piece_at),
                     key=lambda c: (-incidence[c], c))
    added = set()
    base_free = set(free)

    for target in targets:
        if len(added) >= donor_cells_max:
            break
        cls = cell_class(target)
        req = list(_neighbor_colors(target, colors))
        candidates = []
        for holder, pid in piece_at.items():
            if holder in base_free or holder in added:
                continue
            if piece_class(tiles[pid]) != cls:
                continue
            best = None
            tr, tc = divmod(target, SIDE)
            for spin in range(4):
                oriented = rotate_edges(tiles[pid], spin)
                if not frame_rule_ok(tr, tc, oriented):
                    continue
                fixed_matches = sum(oriented[side] == need
                                    for side, need, nbr in req
                                    if nbr not in base_free)
                all_matches = sum(oriented[side] == need
                                  for side, need, _nbr in req)
                # Fixed-boundary compatibility first, then total immediate
                # compatibility.  Prefer a nearby holder only as a tie-break;
                # color fit, not geometry, is why the piece is being imported.
                hr, hc = divmod(holder, SIDE)
                key = (fixed_matches, all_matches,
                       -(abs(hr - tr) + abs(hc - tc)), -pid, -spin)
                if best is None or key > best:
                    best = key
            if best is not None:
                candidates.append((best, holder))
        candidates.sort(reverse=True)
        taken = 0
        for _score, holder in candidates:
            if holder in added:
                continue
            added.add(holder)
            taken += 1
            if taken >= donors_per_target or len(added) >= donor_cells_max:
                break

    return base_free | added, added


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

def board_quality(pos, rot, edges):
    """Lexicographic external quality used by the acceptance guard.

    Break count is first.  At equal break count, retain the largest clean
    foundation, then compact the damaged rows/columns and bounding box.  This
    mirrors the quantities E555_rank.py uses, and prevents a small score gain
    from quietly turning 13 clean bottom rows into three.
    """
    broken = broken_junctions(pos, rot, edges)
    if not broken:
        return (0, -SIDE, -4 * SIDE, 0, 0, 0, 0), {
            "breaks": 0, "max_clean": SIDE, "clean_sum": 4 * SIDE,
            "break_rows": 0, "break_cols": 0, "area": 0,
            "break_cells": 0, "corner_d": 0,
            "clean": (SIDE, SIDE, SIDE, SIDE),
            "clean_b": SIDE, "clean_t": SIDE,
            "clean_l": SIDE, "clean_r": SIDE,
        }
    cells = {c for pair in broken for c in pair}
    rows = {c // SIDE for c in cells}
    cols = {c % SIDE for c in cells}
    rr = [c // SIDE for c in cells]
    cc = [c % SIDE for c in cells]
    area = (max(rr) - min(rr) + 1) * (max(cc) - min(cc) + 1)

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
    corner_d = sum(min(a // SIDE, SIDE - 1 - a // SIDE)
                   + min(a % SIDE, SIDE - 1 - a % SIDE)
                   + min(b // SIDE, SIDE - 1 - b // SIDE)
                   + min(b % SIDE, SIDE - 1 - b % SIDE)
                   for a, b in broken)
    q = (len(broken), -max(clean), -sum(clean), len(rows) + len(cols),
         area, len(cells), corner_d)
    info = {"breaks": len(broken), "max_clean": max(clean),
            "clean_sum": sum(clean), "break_rows": len(rows),
            "break_cols": len(cols), "area": area,
            "break_cells": len(cells), "corner_d": corner_d,
            "clean": clean, "clean_b": clean[0], "clean_t": clean[1],
            "clean_l": clean[2], "clean_r": clean[3]}
    return q, info


def board_fingerprint(pos, rot):
    """Stable hashable board identity for duplicate-neighborhood detection."""
    return tuple(pos) + tuple(rot)


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
# Compact-objective weight used only by ``search_mode=optimize``.
W_AREA = NUM_EDGES + 1                      # 481 > maximum perimeter (480)

def solve_stage(pos, rot, tiles, free_cells, max_changes, compact,
                max_time, stall_time, workers, rseed, verbose, pins=(), *,
                target_breaks=None, max_new_breaks=3, search_mode="improve",
                symmetry_level=2, linearization_level=1,
                repair_hint=False, hint_conflicts=1000, log_search=False,
                reference_broken=None, clean_requirements=()):
    """Solve one bounded neighborhood and return a rich result dictionary.

    ``improve`` is a feasibility-style local search: the model contains the hard
    constraint ``breaks <= input_breaks - 1`` and stops at the first witness.
    Its objective is intentionally tiny -- fewer collateral breaks, then fewer
    changed cells -- because the previous 256-cell area/perimeter objective made
    first-improvement models much larger without being optimized to completion.

    ``optimize`` keeps the full lexicographic objective and is appropriate only
    for a small number of elite boards.  ``clean_requirements`` are hard model
    constraints, not merely an after-the-fact acceptance test.
    """
    model = cp_model.CpModel()
    maxc = max(max(t) for t in tiles)
    piece_at = {pos[p]: p for p in range(NUM_PIECES)
                if pos[p] != CSV_UNPLACED}
    input_broken = set(broken_junctions(pos, rot, tiles))
    protected_broken = (input_broken if reference_broken is None
                        else set(reference_broken))
    input_breaks = len(input_broken)

    free_cells = set(free_cells)
    corner_cells = sorted(c for c in free_cells if c in CORNER_CELLS)
    edge_cells = sorted(c for c in free_cells
                        if c in BORDER_CELLS and c not in CORNER_CELLS)
    inner_cells = sorted(c for c in free_cells if c not in BORDER_CELLS)
    corner_dom = sorted(piece_at[c] for c in corner_cells)
    edge_dom = sorted(piece_at[c] for c in edge_cells)
    inner_dom = sorted(piece_at[c] for c in inner_cells)

    def domain_of(cell):
        if cell in CORNER_CELLS:
            return corner_dom
        if cell in BORDER_CELLS:
            return edge_dom
        return inner_dom

    # One table now also carries the exact changed-cell indicator.  This replaces
    # the earlier same-piece, same-rotation, and kept Boolean triple with one
    # Boolean whose value is fixed directly by each allowed (piece, spin) tuple.
    piece_of, rot_of, color_of, changed_of = {}, {}, {}, {}
    for cell in sorted(free_cells):
        r, c = divmod(cell, SIDE)
        dom = domain_of(cell)
        if not dom:
            return {"ok": False, "status": "empty-domain", "pos": list(pos),
                    "rot": list(rot), "breaks": input_breaks, "wall": 0.0,
                    "objective": None, "bound": None, "changed": 0,
                    "new_breaks": 0}
        p0, r0 = piece_at[cell], rot[piece_at[cell]]
        pv = model.NewIntVarFromDomain(cp_model.Domain.FromValues(dom),
                                       f"pc_{cell}")
        rv = model.NewIntVar(0, 3, f"ro_{cell}")
        nv, ev, sv, wv = (model.NewIntVar(0, maxc, f"{x}_{cell}")
                          for x in "NESW")
        ch = model.NewBoolVar(f"chg_{cell}")
        allowed = []
        for p in dom:
            for spin in range(4):
                oriented = rotate_edges(tiles[p], spin)
                if not frame_rule_ok(r, c, oriented):
                    continue
                allowed.append((p, spin) + oriented
                               + (0 if p == p0 and spin == r0 else 1,))
        model.AddAllowedAssignments([pv, rv, nv, ev, sv, wv, ch], allowed)
        piece_of[cell], rot_of[cell] = pv, rv
        color_of[cell], changed_of[cell] = (nv, ev, sv, wv), ch

    for cls in (corner_cells, edge_cells, inner_cells):
        if len(cls) > 1:
            model.AddAllDifferent([piece_of[c] for c in cls])

    for cell, piece, spin in pins:
        model.Add(piece_of[cell] == piece)
        model.Add(rot_of[cell] == spin)

    changed_vars = [changed_of[c] for c in sorted(free_cells)]
    if changed_vars:
        model.Add(sum(changed_vars) <= min(max_changes, len(changed_vars)))

    fixed_col = {c: rotate_edges(tiles[piece_at[c]], rot[piece_at[c]])
                 for c in range(NUM_PIECES)
                 if c not in free_cells and c in piece_at}

    def ecolor(cell, direction):
        if cell in color_of:
            return color_of[cell][direction]
        if cell in fixed_col:
            return fixed_col[cell][direction]
        return None

    def clean_protected(a, b):
        if not clean_requirements:
            return False
        ar, ac = divmod(a, SIDE)
        br, bc = divmod(b, SIDE)
        for side, depth in clean_requirements:
            if depth <= 0:
                continue
            if side == "B" and min(ar, br) < depth:
                return True
            if side == "T" and max(ar, br) >= SIDE - depth:
                return True
            if side == "L" and min(ac, bc) < depth:
                return True
            if side == "R" and max(ac, bc) >= SIDE - depth:
                return True
        return False

    break_terms, new_break_terms, const_breaks, const_new_breaks, junctions = [], [], 0, 0, []
    hard_guard_conflict = False
    for cell in range(NUM_PIECES):
        r, c = divmod(cell, SIDE)
        for nbr, da, db in ((cell + 1, EAST, WEST),
                            (cell + SIDE, NORTH, SOUTH)):
            if da == EAST and c + 1 >= SIDE:
                continue
            if da == NORTH and r + 1 >= SIDE:
                continue
            ac, bc = ecolor(cell, da), ecolor(nbr, db)
            if ac is None or bc is None:
                continue
            pair = (cell, nbr)
            protected_line = clean_protected(cell, nbr)
            if isinstance(ac, int) and isinstance(bc, int):
                if ac != bc:
                    const_breaks += 1
                    if pair not in protected_broken:
                        const_new_breaks += 1
                    junctions.append((cell, nbr, True))
                    if protected_line:
                        hard_guard_conflict = True
                continue
            brk = model.NewBoolVar(f"brk_{cell}_{nbr}")
            model.Add(ac == bc).OnlyEnforceIf(brk.Not())
            model.Add(ac != bc).OnlyEnforceIf(brk)
            if protected_line:
                model.Add(brk == 0)
            model.AddHint(brk, 1 if pair in input_broken else 0)
            break_terms.append(brk)
            if pair not in protected_broken:
                new_break_terms.append(brk)
            junctions.append((cell, nbr, brk))

    if hard_guard_conflict:
        return {"ok": False, "status": "clean-guard", "pos": list(pos),
                "rot": list(rot), "breaks": input_breaks, "wall": 0.0,
                "objective": None, "bound": None, "changed": 0,
                "new_breaks": 0}

    # Cheap exact prechecks.  A break between two locked cells cannot be
    # repaired by this neighborhood, and a collateral break already locked
    # outside the new pool still counts against the board-relative allowance.
    # Returning here avoids asking CP-SAT to rediscover a scalar contradiction.
    if target_breaks is not None and const_breaks > target_breaks:
        return {"ok": False, "status": "fixed-break-bound", "pos": list(pos),
                "rot": list(rot), "breaks": input_breaks, "wall": 0.0,
                "objective": None, "bound": None, "changed": 0,
                "new_breaks": const_new_breaks}
    if max_new_breaks >= 0 and const_new_breaks > max_new_breaks:
        return {"ok": False, "status": "new-break-bound", "pos": list(pos),
                "rot": list(rot), "breaks": input_breaks, "wall": 0.0,
                "objective": None, "bound": None, "changed": 0,
                "new_breaks": const_new_breaks}

    total_break_expr = const_breaks + sum(break_terms)
    if target_breaks is not None and break_terms:
        model.Add(sum(break_terms) <= target_breaks - const_breaks)
    if max_new_breaks >= 0 and new_break_terms:
        model.Add(sum(new_break_terms) <= max_new_breaks - const_new_breaks)

    # The expensive area/perimeter machinery is useful only when the solve is
    # actually allowed to optimize it.  First-improvement stops at the first
    # feasible witness, so constructing those variables there was pure burden.
    compact_expr = 0
    if compact and search_mode == "optimize":
        shows_break = {c: model.NewBoolVar(f"sb_{c}")
                       for c in range(NUM_PIECES)}
        incident = collections.defaultdict(list)
        forced = set()
        for ca, cb, marker in junctions:
            if marker is True:
                forced.update((ca, cb))
            else:
                model.AddImplication(marker, shows_break[ca])
                model.AddImplication(marker, shows_break[cb])
                incident[ca].append(marker)
                incident[cb].append(marker)
        input_break_cells = {c for pair in input_broken for c in pair}
        for cell in range(NUM_PIECES):
            if cell in forced:
                model.Add(shows_break[cell] == 1)
            elif incident[cell]:
                model.AddBoolOr(incident[cell] + [shows_break[cell].Not()])
            else:
                model.Add(shows_break[cell] == 0)
            model.AddHint(shows_break[cell],
                          1 if cell in input_break_cells else 0)
        boundary = []
        for ca, cb, _da, _db in ALL_JUNCTIONS:
            edge = model.NewBoolVar(f"be_{ca}_{cb}")
            model.Add(shows_break[ca] != shows_break[cb]).OnlyEnforceIf(edge)
            model.Add(shows_break[ca] == shows_break[cb]).OnlyEnforceIf(edge.Not())
            model.AddHint(edge, 1 if ((ca in input_break_cells) !=
                                      (cb in input_break_cells)) else 0)
            boundary.append(edge)
        compact_expr = W_AREA * sum(shows_break.values()) + sum(boundary)

    changed_expr = sum(changed_vars) if changed_vars else 0
    new_expr = const_new_breaks + sum(new_break_terms)
    if search_mode == "optimize":
        compact_ub = W_AREA * NUM_PIECES + NUM_EDGES
        new_weight = compact_ub + len(changed_vars) + 1
        break_weight = ((len(new_break_terms) + 1) * new_weight
                        + compact_ub + len(changed_vars) + 1)
        model.Minimize(break_weight * total_break_expr
                       + new_weight * new_expr + compact_expr + changed_expr)
    else:
        # Pure satisfaction model.  The one-break target, collateral cap,
        # Hamming radius, and clean-foundation constraints already define an
        # acceptable move.  Adding even a small objective makes CP-SAT maintain
        # objective bounds and can divert portfolio workers into optimization
        # machinery that has no value when the first witness ends the call.
        # Profiles obtain quality by asking the tight questions first (small
        # pool, m4, low collateral cap), not by optimizing inside one call.
        pass

    # A complete incumbent placement hint.  In improve mode it violates only
    # the one-break-better target -- the case repair_hint was written for, and
    # the case that makes repair_hint abort on OR-Tools 9.15.6755, which is why
    # that parameter is off by default (see the solver setup below).  CP-SAT
    # still uses the hint for value ordering without it.
    for cell in sorted(free_cells):
        p = piece_at[cell]
        model.AddHint(piece_of[cell], p)
        model.AddHint(rot_of[cell], rot[p])
        for d, var in enumerate(color_of[cell]):
            model.AddHint(var, rotate_edges(tiles[p], rot[p])[d])
        model.AddHint(changed_of[cell], 0)

    solver = cp_model.CpSolver()
    if hasattr(solver.parameters, "num_workers"):
        solver.parameters.num_workers = workers
    else:
        solver.parameters.num_search_workers = workers
    solver.parameters.linearization_level = linearization_level
    solver.parameters.symmetry_level = symmetry_level
    solver.parameters.random_seed = rseed & 0x7fffffff
    if max_time != float("inf"):
        solver.parameters.max_time_in_seconds = float(max_time)
    solver.parameters.stop_after_first_solution = (search_mode == "improve")
    # OFF BY DEFAULT, and not merely as a tuning preference. On OR-Tools
    # 9.15.6755 this parameter aborts the process -- "Check failed:
    # heuristics.fixed_search != nullptr", SIGABRT, no output, every board after
    # it in the corpus lost. It fires when the hint violates the model, which in
    # improve mode is EVERY call: the incumbent breaches the one-break-better
    # target by construction. Measured: default settings abort within 25 s on
    # data/board_example_462.csv; --no-repair_hint completes the same run.
    # Re-enable only against an OR-Tools build you have confirmed is fixed.
    if repair_hint and hasattr(solver.parameters, "repair_hint"):
        solver.parameters.repair_hint = True
        solver.parameters.hint_conflict_limit = hint_conflicts
    if log_search:
        solver.parameters.log_search_progress = True
        solver.parameters.log_subsolver_statistics = workers > 1

    class _Tracker(cp_model.CpSolverSolutionCallback):
        def __init__(self):
            cp_model.CpSolverSolutionCallback.__init__(self)
            self.t0 = time.monotonic()
            self.last_improvement = self.t0
            self.best_obj = None
            self.found = 0

        def on_solution_callback(self):
            self.found += 1
            now = time.monotonic()
            if search_mode == "optimize":
                obj = int(round(self.ObjectiveValue()))
                improved = self.best_obj is None or obj < self.best_obj
            else:
                obj = None
                improved = self.best_obj is None
            if improved:
                self.best_obj = 0 if obj is None else obj
                self.last_improvement = now
                if verbose:
                    total = const_breaks + sum(self.Value(b) for b in break_terms)
                    tail = "" if obj is None else f" | objective={obj}"
                    print(f"        [inc] t={now-self.t0:5.1f}s | "
                          f"breaks={total}{tail}", flush=True)
            if target_breaks is not None:
                total = const_breaks + sum(self.Value(b) for b in break_terms)
                if total <= target_breaks:
                    self.StopSearch()

    tracker = _Tracker()
    done, state = threading.Event(), {"stalled": False}

    def watchdog():
        # In improve mode the first solution ends the call, so there is no
        # post-incumbent stall to police.  A no-solution call is governed solely
        # by max_time.  The watchdog is meaningful only for optimize mode.
        while not done.wait(0.25):
            if (search_mode == "optimize" and stall_time > 0
                    and tracker.found > 0
                    and time.monotonic() - tracker.last_improvement > stall_time):
                state["stalled"] = True
                solver.StopSearch()
                return

    watcher = threading.Thread(target=watchdog, daemon=True)
    watcher.start()
    status = solver.Solve(model, tracker)
    done.set()
    watcher.join()

    status_name = ("optimal" if status == cp_model.OPTIMAL else
                   "infeasible" if status == cp_model.INFEASIBLE else
                   "model-invalid" if status == cp_model.MODEL_INVALID else
                   "stalled" if state["stalled"] else
                   "feasible" if status == cp_model.FEASIBLE else "unknown")

    best_pos, best_rot = list(pos), list(rot)
    ok = status in (cp_model.OPTIMAL, cp_model.FEASIBLE)
    if ok:
        pool_pieces = {piece_at[c] for c in free_cells}
        for p in pool_pieces:
            best_pos[p], best_rot[p] = CSV_UNPLACED, 0
        for cell in free_cells:
            p = solver.Value(piece_of[cell])
            best_pos[p], best_rot[p] = cell, solver.Value(rot_of[cell])

    out_breaks = (len(broken_junctions(best_pos, best_rot, tiles))
                  if ok else input_breaks)
    changed_n = sum(1 for c in free_cells
                    if ok and (best_pos[piece_at[c]] != c or
                               best_rot[piece_at[c]] != rot[piece_at[c]]))
    new_breaks_n = (len(set(broken_junctions(best_pos, best_rot, tiles))
                        - protected_broken) if ok else 0)

    if verbose:
        print("        ---- CP-SAT stats ----", flush=True)
        for line in solver.ResponseStats().strip().splitlines():
            print(f"        {line}", flush=True)

    bound = None
    objective = None
    if search_mode == "optimize":
        if status in (cp_model.OPTIMAL, cp_model.FEASIBLE, cp_model.UNKNOWN):
            try:
                bound = solver.BestObjectiveBound()
            except (AttributeError, RuntimeError):
                bound = None
        if ok:
            objective = solver.ObjectiveValue()

    return {"ok": ok, "status": status_name, "pos": best_pos,
            "rot": best_rot, "breaks": out_breaks,
            "wall": solver.WallTime(),
            "objective": objective,
            "bound": bound, "changed": changed_n,
            "new_breaks": new_breaks_n}


# ---------------------------------------------------------------------------
# escalation ladder: (reach, budget) rungs, cheap -> broad
# ---------------------------------------------------------------------------
def parse_rungs(spec):
    """Parse ``R:M,R:M`` into unique (reach, max_changes) pairs."""
    out = []
    for token in spec.split(","):
        try:
            r, m = (int(x) for x in token.split(":", 1))
        except ValueError:
            raise SystemExit(f"[ERROR] bad rung {token!r}; use R:M,R:M")
        if r < 1 or m < 1:
            raise SystemExit("[ERROR] rung reach and change budget must be positive")
        if (r, m) not in out:
            out.append((r, m))
    return out


def make_ladder(reach, cmax, kind="diagonal", start=LADDER_START,
                step=LADDER_STEP, explicit=None):
    if explicit:
        return parse_rungs(explicit)
    mvals = list(range(start, cmax + 1, step))
    if not mvals or mvals[-1] != cmax:
        mvals.append(cmax)
    mvals = sorted(set(max(1, min(cmax, m)) for m in mvals))
    if kind == "cartesian":
        return [(r, m) for r in range(1, reach + 1) for m in mvals]
    # Diagonal allocation: for reach=3, cmax=16 this is exactly
    # (1,4),(1,8),(2,12),(3,16), not twelve full solves.
    n = len(mvals)
    out = []
    for i, m in enumerate(mvals):
        r = 1 + (i * reach // max(1, n))
        r = min(reach, r)
        if (r, m) not in out:
            out.append((r, m))
    if out[-1] != (reach, cmax):
        out.append((reach, cmax))
    return out


def solve_board(mode, partial, tiles, holes, policy, workers, base_seed, verbose,
                *, search_mode="improve", preserve_clean=True,
                preserve_side="auto", new_break_reference="board",
                donors_per_target=0, donor_cells_max=0,
                accept_equal_compaction=True, no_gain_limit=0,
                symmetry_level=2, linearization_level=1,
                repair_hint=False, hint_conflicts=1000, log_search=False,
                CL=None, clue_mask=None, orient=None):
    """Run one adaptive local-search portfolio on one input board."""
    pos, rot = list(partial.pos), list(partial.rot)
    placed = {pos[p] for p in range(NUM_PIECES)
              if pos[p] != CSV_UNPLACED}
    quality, qinfo = board_quality(pos, rot, tiles)
    breaks = qinfo["breaks"]
    baseline_broken = set(broken_junctions(pos, rot, tiles))
    baseline_clean = qinfo["clean"]
    clues_on = bool(clue_mask and orient is not None)

    stats = collections.Counter()
    stage_tag, reason = "-", "exhausted"
    total_t = 0.0
    board_time = policy["board_time"]
    deadline = None if board_time <= 0 else time.monotonic() + board_time
    seen = set()
    no_gain = 0

    def remaining_time():
        return float("inf") if deadline is None else deadline - time.monotonic()

    def clue_hits(p, r):
        if not clues_on:
            return 0
        return sum(1 for cell, piece, spin in CL.clue_list(orient, clue_mask)
                   if p[piece] == cell and r[piece] == spin)

    clean_names = ("B", "T", "L", "R")
    if preserve_side == "auto":
        clean_idx = max(range(4), key=lambda i: baseline_clean[i])
        clean_guard_name = clean_names[clean_idx]
    elif preserve_side in clean_names:
        clean_idx = clean_names.index(preserve_side)
        clean_guard_name = preserve_side
    else:
        clean_idx = None
        clean_guard_name = preserve_side

    max_clean_loss = policy["max_clean_loss"]

    def clean_guard(info):
        if not preserve_clean:
            return True
        now = info["clean"]
        if preserve_side == "all":
            return all(now[i] >= baseline_clean[i] - max_clean_loss
                       for i in range(4))
        if preserve_side == "any":
            return max(now) >= max(baseline_clean) - max_clean_loss
        return now[clean_idx] >= baseline_clean[clean_idx] - max_clean_loss

    # Encode the common clean-side policies directly into every CP-SAT model.
    # The acceptance guard remains as a post-solve assertion and covers the
    # deliberately looser preserve_side=any semantics.
    clean_requirements = []
    if preserve_clean:
        if preserve_side == "all":
            clean_requirements = [
                (name, max(0, baseline_clean[i] - max_clean_loss))
                for i, name in enumerate(clean_names)]
        elif preserve_side != "any":
            clean_requirements = [
                (clean_guard_name,
                 max(0, baseline_clean[clean_idx] - max_clean_loss))]

    if verbose and preserve_clean:
        if preserve_side == "all":
            target = ", ".join(f"{s}>={d}" for s, d in clean_requirements)
        elif preserve_side == "any":
            target = f"some side >= {max(baseline_clean)-max_clean_loss} (post-check)"
        else:
            target = f"{clean_requirements[0][0]} >= {clean_requirements[0][1]}"
        print(f"      [guard] hard clean foundation: {target}; "
              f"new-break reference={new_break_reference}", flush=True)

    if breaks == 0 and not (clues_on and
                            clue_open_cells(CL, clue_mask, orient, pos, rot)):
        return pos, rot, 0, "input-solved", "-", 0.0, stats

    # Time caps in the profile scale with a user-overridden per-board budget.
    prof = policy["profile"]
    if board_time > 0 and prof.board_seconds > 0:
        time_scale = board_time / prof.board_seconds
    else:
        time_scale = 1.0

    def attempt_cap(profile_seconds, phase_deadline=None):
        override = policy.get("attempt_override")
        cap = float(override) if override is not None else max(0.5, profile_seconds * time_scale)
        rem = remaining_time()
        if phase_deadline is not None:
            rem = min(rem, phase_deadline - time.monotonic())
        return min(cap, rem)

    # Check whether the widest profile neighborhood can touch every break.  It
    # is only a warning: focused neighborhoods intentionally touch subsets.
    broken0 = broken_junctions(pos, rot, tiles)
    bc0 = {c for pair in broken0 for c in pair}
    clue0 = (clue_open_cells(CL, clue_mask, orient, pos, rot)
             if clues_on else ())
    widest_scope = ("full" if any(spec[0] == "full"
                                  for spec in policy["global_plan"])
                    else policy["border_scope"])
    widest_reach = max((spec[1] for spec in policy["global_plan"]), default=1)
    free_max = build_pool(
        mode, bc0, widest_reach, holes, placed, clue0,
        inner_cap=policy["inner_cap"], patch_shape=policy["patch_shape"],
        border_scope=widest_scope, ring_radius=policy["ring_radius"])
    free_max, _ = enrich_with_donors(
        free_max, broken0, pos, rot, tiles,
        donors_per_target=donors_per_target,
        donor_cells_max=donor_cells_max)
    unreachable = sum(1 for a, b in broken0
                      if a not in free_max and b not in free_max)
    if unreachable and verbose:
        print(f"      [WARN] {unreachable} break(s) outside the widest pool; "
              "this portfolio cannot remove them.", flush=True)

    attempt_serial = 0

    def run_attempt(label, free, reach_value, changes, new_cap,
                    seconds, rep, donor_cells=(), phase_deadline=None):
        """Run and conditionally accept one model; return (strict, stop)."""
        nonlocal pos, rot, breaks, quality, qinfo, stage_tag, reason
        nonlocal total_t, no_gain, attempt_serial

        budget = attempt_cap(seconds, phase_deadline)
        if budget <= 0:
            # Only the BOARD deadline ends the portfolio. The focused phase has
            # its own, far shorter deadline, and reporting that as "board-time"
            # abandoned the board with most of its budget unspent -- the broad
            # phase 2 neighbourhoods never ran at all.
            if remaining_time() <= 0:
                reason = "board-time"
            return False, True

        key = (board_fingerprint(pos, rot), tuple(sorted(free)),
               min(changes, len(free)), new_cap, search_mode, rep)
        if key in seen:
            stats["duplicate_pool"] += 1
            return False, False
        seen.add(key)

        pins = ()
        if clues_on:
            pins, locked_ok, skipped = CL.clue_pins(
                pos, rot, free, orient, clue_mask, unplaced_ok=False)
            if verbose:
                note = "; ".join(why for _c, _p, _s, why in skipped)
                print(f"      [clue] orient={orient} | pin {len(pins)} | "
                      f"locked-ok {locked_ok} | skip {len(skipped)}"
                      + (f" ({note})" if note else ""), flush=True)
        if verbose:
            print(f"      [try {label}] reach={reach_value} changes={changes} "
                  f"new<={new_cap} cap={budget:.1f}s", flush=True)
            draw_pool(free, {c for pair in broken_junctions(pos, rot, tiles)
                             for c in pair})

        pending_clue = False
        if clues_on:
            pending_clue = clue_hits(pos, rot) < len(CL.clue_list(orient, clue_mask))
        if search_mode == "improve":
            # A displaced mandatory clue is itself progress.  Permit an equal-
            # break clue repair first; once the pins are satisfied, ordinary
            # one-break descent resumes.
            target = breaks if pending_clue else (breaks - 1 if breaks > 0 else None)
        else:
            target = None
        result = solve_stage(
            pos, rot, tiles, free, min(changes, len(free)), mode == "patch",
            budget, policy["stall_time"], workers,
            base_seed + 7919 * attempt_serial + rep,
            verbose, pins, target_breaks=target,
            max_new_breaks=new_cap, search_mode=search_mode,
            symmetry_level=symmetry_level,
            linearization_level=linearization_level,
            repair_hint=repair_hint, hint_conflicts=hint_conflicts,
            log_search=log_search,
            reference_broken=(baseline_broken
                              if new_break_reference == "board" else None),
            clean_requirements=clean_requirements)
        attempt_serial += 1
        total_t += result["wall"]
        stats["attempts"] += 1
        stats[result["status"]] += 1

        npos, nrot, nb = result["pos"], result["rot"], result["breaks"]
        nquality, nq = board_quality(npos, nrot, tiles)
        clue_better = clue_hits(npos, nrot) > clue_hits(pos, rot)
        strict = nb < breaks
        plateau = (search_mode == "optimize" and nb == breaks
                   and nquality < quality and accept_equal_compaction
                   and mode == "patch")
        clean_ok = clean_guard(nq)
        accept = result["ok"] and clean_ok and (clue_better or strict or plateau)

        if verbose:
            arrow = "ACCEPT" if accept else "keep"
            bound = "-" if result["bound"] is None else f"{result['bound']:.0f}"
            print(f"      [{label}] {breaks:>3} -> {nb:>3} | "
                  f"{result['status']:<10} | {result['wall']:6.1f}s | "
                  f"pool={len(free):3d} changed={result['changed']:2d} "
                  f"donors={len(donor_cells):2d} new={result['new_breaks']:2d} "
                  f"objective-bound={bound:>4} | {arrow}", flush=True)

        if accept:
            old_breaks = breaks
            pos, rot, breaks = npos, nrot, nb
            quality, qinfo = nquality, nq
            stage_tag = label
            stats["accepted"] += 1
            no_gain = 0
            if strict:
                stats["strict_gain"] += old_breaks - breaks
                return True, False
            stats["plateau"] += 1
        else:
            no_gain += 1
            if no_gain_limit > 0 and no_gain >= no_gain_limit:
                reason = "no-gain-limit"
                return False, True
        reason = result["status"]
        return False, False

    # Every strict gain recentres the geometry and restarts the inexpensive
    # focused phase.  The single board deadline continues to run.
    while not _STOP:
        if remaining_time() <= 0:
            reason = "board-time"
            break
        accepted_strict = False
        broken_now = broken_junctions(pos, rot, tiles)
        clue_cells = (clue_open_cells(CL, clue_mask, orient, pos, rot)
                      if clues_on else ())

        # Phase 1: small windows around individual damaged junctions.  The phase
        # is capped to a fraction of the remaining board budget so broad models
        # always get a turn.
        if policy["focus_enabled"] and broken_now:
            rem0 = remaining_time()
            nominal = (prof.board_seconds if rem0 == float("inf") else rem0)
            focus_allowance = max(0.0, policy["focus_share"] * nominal)
            focus_deadline = time.monotonic() + focus_allowance
            focus_pools = build_focus_pools(
                broken_now, placed, reach=policy["focus_reach"],
                inner_cap=policy["focus_inner_cap"],
                ring_radius=policy["focus_ring_radius"],
                limit=policy["focus_limit"], clue_open=clue_cells)
            stats["focus_pools"] += len(focus_pools)
            for fi, free in enumerate(focus_pools, 1):
                for changes in policy["focus_changes"]:
                    if time.monotonic() >= focus_deadline or remaining_time() <= 0:
                        break
                    cap = (policy["max_new_override"]
                           if policy["max_new_override"] is not None else
                           min(3, max(0, changes - 1)))
                    # The phase deadline is enforced by reducing the nominal
                    # seconds passed to run_attempt.
                    seconds = min(policy["focus_attempt_seconds"],
                                  max(0.0, focus_deadline - time.monotonic()) /
                                  max(time_scale, 1e-9))
                    strict, stop = run_attempt(
                        f"f{fi}r{policy['focus_reach']}m{changes}", free,
                        policy["focus_reach"], changes, cap, seconds, 0,
                        phase_deadline=focus_deadline)
                    if strict:
                        accepted_strict = True
                        break
                    if stop:
                        break
                if accepted_strict or reason in ("board-time", "interrupted",
                                                  "no-gain-limit"):
                    break

        if accepted_strict:
            seen.clear()
            if breaks == 0 and not (clues_on and
                                    clue_open_cells(CL, clue_mask, orient, pos, rot)):
                reason = "solved"
                break
            continue
        if reason in ("board-time", "interrupted", "no-gain-limit"):
            break

        # Phase 2: progressively broader global neighborhoods.  Profile entries
        # can include independent repetitions with distinct random seeds.
        for scope, reach_value, changes, profile_new, seconds, repeats in policy["global_plan"]:
            actual_scope = policy["border_scope"] if policy["border_scope_forced"] else scope
            actual_new = (policy["max_new_override"]
                          if policy["max_new_override"] is not None else profile_new)
            actual_repeats = (policy["restarts_override"]
                              if policy["restarts_override"] is not None else repeats)
            for rep in range(actual_repeats):
                if _STOP:
                    reason = "interrupted"
                    break
                if remaining_time() <= 0:
                    reason = "board-time"
                    break
                broken_now = broken_junctions(pos, rot, tiles)
                bc = {c for pair in broken_now for c in pair}
                clue_cells = (clue_open_cells(CL, clue_mask, orient, pos, rot)
                              if clues_on else ())
                free = build_pool(
                    mode, bc, reach_value, holes, placed, clue_cells,
                    inner_cap=policy["inner_cap"],
                    patch_shape=policy["patch_shape"],
                    border_scope=actual_scope,
                    ring_radius=policy["ring_radius"])
                free, donor_cells = enrich_with_donors(
                    free, broken_now, pos, rot, tiles,
                    donors_per_target=donors_per_target,
                    donor_cells_max=donor_cells_max)
                label = f"g{actual_scope[0]}r{reach_value}m{changes}s{rep}"
                strict, stop = run_attempt(
                    label, free, reach_value, changes, actual_new,
                    seconds, rep, donor_cells)
                if strict:
                    accepted_strict = True
                    break
                if stop:
                    break
            if accepted_strict or reason in ("board-time", "interrupted",
                                              "no-gain-limit"):
                break

        if reason in ("board-time", "interrupted", "no-gain-limit"):
            break
        if breaks == 0 and not (clues_on and
                                clue_open_cells(CL, clue_mask, orient, pos, rot)):
            reason = "solved"
            break
        if not accepted_strict:
            reason = "portfolio-exhausted"
            break
        seen.clear()

    if _STOP and reason != "interrupted":
        reason = "interrupted"
    return pos, rot, breaks, reason, stage_tag, total_t, stats


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
        description=("Stage C adaptive CP-SAT closer.  The default overnight "
                     "profile searches small focused repairs first, then "
                     "progressively broader neighborhoods."),
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog=("Use --show_advanced --help to display the backwards-compatible "
                "low-level tuning controls."))
    ap.add_argument("seed", help="piece seed file")
    ap.add_argument("partials", help="input board CSV")
    ap.add_argument("output", help="output board CSV")

    main_group = ap.add_argument_group("main search controls")
    main_group.add_argument(
        "--profile", choices=tuple(EFFORT_PROFILES), default="overnight",
        help=("overnight: throughput over a large corpus; deep: about 15 min "
              "per board; superdeep: about 2 h per board and wider models"))
    main_group.add_argument(
        "--board_time_limit", type=float, default=None,
        help="true total seconds per input board; 0 means unlimited")
    main_group.add_argument(
        "--attempt_time", type=float,
        default=None,
        help=adv("optional cap for each individual CP-SAT call; omitted uses the "
                 "profile's shorter-to-longer schedule"))
    main_group.add_argument(
        "--threads", dest="threads", type=int, default=None,
        help="CP-SAT workers used by one process")
    main_group.add_argument(
        "--max_new_breaks", type=int, default=None,
        help=adv("override the profile's collateral-break cap; -1 removes the cap"))
    main_group.add_argument(
        "--search_mode", choices=("improve", "optimize"), default="improve",
        help=("improve asks for one fewer break and stops at the first witness; "
              "optimize spends the attempt minimizing and compacting"))
    main_group.add_argument(
        "--rng_seed", type=int, default=0,
        help="base seed; 0 chooses and reports a random seed")
    main_group.add_argument("--verbose", action="store_true",
                            help="print every neighborhood and CP-SAT statistics")
    main_group.add_argument("--show_advanced", action="store_true",
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

    # Backwards-compatible expert controls.  They stay accepted so old scripts
    # do not break, but ordinary --help is now centered on the three profiles.
    expert = ap.add_argument_group("advanced neighborhood and solver controls")
    expert.add_argument("--mode", choices=("patch", "ring"), default="patch",
                        help=adv("patch is the intended adaptive closer; ring is retained for experiments"))
    expert.add_argument("--holes", default=None,
                        help=adv("16x16 exact movable-cell mask"))
    expert.add_argument("--reach", type=int, default=None,
                        help=adv("custom global-ladder reach ceiling"))
    expert.add_argument("--max_changes", type=int, default=None,
                        help=adv("custom global-ladder Hamming ceiling"))
    expert.add_argument("--ladder", choices=("diagonal", "cartesian"),
                        default="diagonal", help=adv("custom ladder shape"))
    expert.add_argument("--rungs", default=None, metavar="R:M,...",
                        help=adv("explicit global ladder, e.g. 1:4,1:8,2:12,3:16"))
    expert.add_argument("--changes_start", type=int, default=LADDER_START,
                        help=adv("first custom-ladder change radius"))
    expert.add_argument("--changes_step", type=int, default=LADDER_STEP,
                        help=adv("custom-ladder change-radius increment"))
    expert.add_argument("--restarts_per_rung", type=int, default=None,
                        help=adv("override profile repetitions for every global rung"))
    expert.add_argument("--inner_cap", type=int, default=None,
                        help=adv("maximum interior cells in broad automatic pools"))
    expert.add_argument("--patch_shape", choices=("cluster", "box"),
                        default="cluster", help=adv("automatic patch geometry"))
    expert.add_argument("--border_scope", choices=("local", "full", "none"),
                        default=None, help=adv("force one border scope on all global attempts"))
    expert.add_argument("--ring_radius", type=int, default=None,
                        help=adv("half-length of a local frame arc"))
    expert.add_argument("--focus", action=argparse.BooleanOptionalAction,
                        default=None, help=adv("enable small break-centered pools before broad solves"))
    expert.add_argument("--focus_limit", type=int, default=None,
                        help=adv("maximum focused pools per descent cycle"))
    expert.add_argument("--focus_reach", type=int, default=None,
                        help=adv("Manhattan reach of focused pools"))
    expert.add_argument("--focus_inner_cap", type=int, default=None,
                        help=adv("interior-cell cap of one focused pool"))
    expert.add_argument("--focus_ring_radius", type=int, default=None,
                        help=adv("local frame radius of one focused pool"))
    expert.add_argument("--focus_attempt_time", type=float, default=None,
                        help=adv("nominal focused-call cap before profile scaling"))
    expert.add_argument("--focus_share", type=float, default=None,
                        help=adv("maximum share of remaining board budget spent on focused pools"))
    expert.add_argument("--donors_per_target", type=int, default=0,
                        help=adv("experimental singleton donor holders; normally leave at 0"))
    expert.add_argument("--donor_cells_max", type=int, default=12,
                        help=adv("global singleton-donor cell cap"))
    expert.add_argument("--new_break_reference", choices=("board", "attempt"),
                        default="board", help=adv("measure collateral breaks from input board or current step"))
    expert.add_argument("--preserve_clean", action=argparse.BooleanOptionalAction,
                        default=True, help=adv("hard-protect the selected clean foundation"))
    expert.add_argument("--max_clean_loss", type=int, default=None,
                        help=adv("clean rows/columns the final board may lose"))
    expert.add_argument("--preserve_side",
                        choices=("auto", "any", "all", "B", "T", "L", "R"),
                        default="auto", help=adv("clean foundation to protect"))
    expert.add_argument("--accept_equal_compaction",
                        action=argparse.BooleanOptionalAction, default=True,
                        help=adv("optimize mode may retain a better-shaped equal-break board"))
    expert.add_argument("--stall_time", type=float, default=30.0,
                        help=adv("optimize mode only: stop after this long without objective improvement"))
    expert.add_argument("--no_gain_limit", type=int, default=0,
                        help=adv("stop after this many rejected calls; 0 runs the portfolio"))
    expert.add_argument("--duplicate_policy", choices=("reuse", "rerun"),
                        default=None, help=adv("reuse or independently rerun exact duplicate inputs"))
    expert.add_argument("--symmetry_level", type=int, default=2,
                        choices=range(0, 5), help=adv("OR-Tools symmetry level"))
    expert.add_argument("--linearization_level", type=int, default=1,
                        choices=(0, 1, 2), help=adv("OR-Tools linearization level"))
    expert.add_argument("--repair_hint", action=argparse.BooleanOptionalAction,
                        default=False,
                        help=adv("ask CP-SAT to repair the incumbent hint; aborts "
                                 "the process on OR-Tools 9.15.6755"))
    expert.add_argument("--hint_conflicts", type=int, default=1000,
                        help=adv("conflict budget for hint repair"))
    expert.add_argument("--log_search", action="store_true",
                        help=adv("enable detailed OR-Tools subsolver logs"))
    args = ap.parse_args()

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
    if args.max_new_breaks is not None and args.max_new_breaks < -1:
        sys.exit("[ERROR] --max_new_breaks must be -1 or non-negative.")
    if args.restarts_per_rung is not None and args.restarts_per_rung < 1:
        sys.exit("[ERROR] --restarts_per_rung must be >= 1.")
    if args.focus_share is not None and not 0.0 <= args.focus_share <= 1.0:
        sys.exit("[ERROR] --focus_share must be in 0..1.")

    prof = EFFORT_PROFILES[args.profile]
    board_time = (prof.board_seconds if args.board_time_limit is None
                  else args.board_time_limit)
    workers = prof.workers if args.threads is None else args.threads
    if workers < 1:
        sys.exit("[ERROR] --threads must be >= 1.")

    inner_cap = prof.inner_cap if args.inner_cap is None else args.inner_cap
    ring_radius = prof.ring_radius if args.ring_radius is None else args.ring_radius
    max_clean_loss = (prof.max_clean_loss if args.max_clean_loss is None
                      else args.max_clean_loss)
    if inner_cap < 1 or ring_radius < 0 or max_clean_loss < 0:
        sys.exit("[ERROR] inner_cap must be positive; radii/losses non-negative.")

    holes = read_holes_file(args.holes) if args.holes else None
    if holes is not None and not holes:
        sys.exit("[ERROR] --holes marks no movable cell.")
    if holes is not None and args.mode == "ring":
        print("[note] --holes is a patch-mode mask; ignoring --mode ring.",
              flush=True)
        args.mode = "patch"

    # An explicit ladder switches only the broad phase to custom geometry; the
    # focused front end remains available unless --no-focus is passed.
    custom_ladder = any((args.rungs is not None, args.reach is not None,
                         args.max_changes is not None,
                         args.ladder != "diagonal",
                         args.changes_start != LADDER_START,
                         args.changes_step != LADDER_STEP))
    if custom_ladder:
        reach = args.reach if args.reach is not None else 3
        cmax = args.max_changes if args.max_changes is not None else 16
        if reach < 1 or cmax < 1:
            sys.exit("[ERROR] --reach and --max_changes must be positive.")
        ladder = make_ladder(reach, cmax, args.ladder,
                             args.changes_start, args.changes_step, args.rungs)
        nominal = max(5.0, 0.75 * prof.board_seconds / max(1, len(ladder)))
        scope = args.border_scope or "local"
        profile_new = args.max_new_breaks if args.max_new_breaks is not None else 3
        repeats = args.restarts_per_rung or 1
        global_plan = tuple((scope, r, m, profile_new, nominal, repeats)
                            for r, m in ladder)
    else:
        global_plan = prof.global_plan

    focus_enabled = (prof.focus_limit > 0 if args.focus is None else args.focus)
    if holes is not None:
        focus_enabled = False
    policy = {
        "profile": prof,
        "board_time": board_time,
        "attempt_override": args.attempt_time,
        "focus_enabled": focus_enabled,
        "focus_share": prof.focus_share if args.focus_share is None else args.focus_share,
        "focus_limit": prof.focus_limit if args.focus_limit is None else args.focus_limit,
        "focus_reach": prof.focus_reach if args.focus_reach is None else args.focus_reach,
        "focus_inner_cap": (prof.focus_inner_cap if args.focus_inner_cap is None
                            else args.focus_inner_cap),
        "focus_ring_radius": (prof.focus_ring_radius
                              if args.focus_ring_radius is None
                              else args.focus_ring_radius),
        "focus_changes": prof.focus_changes,
        "focus_attempt_seconds": (prof.focus_attempt_seconds
                                  if args.focus_attempt_time is None
                                  else args.focus_attempt_time),
        "global_plan": global_plan,
        "inner_cap": inner_cap,
        "patch_shape": args.patch_shape,
        "border_scope": args.border_scope or "local",
        "border_scope_forced": args.border_scope is not None,
        "ring_radius": ring_radius,
        "max_new_override": args.max_new_breaks,
        "restarts_override": args.restarts_per_rung,
        "max_clean_loss": max_clean_loss,
        "stall_time": args.stall_time,
        "duplicate_policy": args.duplicate_policy or prof.duplicate_policy,
    }
    for key in ("focus_limit", "focus_reach", "focus_inner_cap",
                "focus_ring_radius"):
        if policy[key] < 0 or (key != "focus_ring_radius" and policy[key] == 0):
            sys.exit(f"[ERROR] invalid --{key} value.")

    CL = clue_mask = None
    if args.clue_center or args.clue_corners:
        CL = load_clues()
        clue_mask = ((CL.CLUE_CENTER if args.clue_center else 0)
                     | (CL.CLUE_CORNERS if args.clue_corners else 0))
    tiles = read_seed(args.seed)
    signal.signal(signal.SIGINT, _request_stop)

    plan_text = ", ".join(
        f"{scope}:r{r}/m{m}/new{n}/{sec:g}s/x{rep}"
        for scope, r, m, n, sec, rep in global_plan)
    print("\n=== E555 adaptive ender ===")
    print(f"[cfg] profile={args.profile} search={args.search_mode} "
          f"board_time={board_time:g}s workers={workers} "
          f"focus={'on' if focus_enabled else 'off'} "
          f"duplicates={policy['duplicate_policy']} rng_seed={args.rng_seed}")
    print(f"[cfg] global_plan={plan_text}; max_new="
          f"{'profile' if args.max_new_breaks is None else args.max_new_breaks}; "
          f"hard_clean={args.preserve_clean}/{args.preserve_side} "
          f"loss<={max_clean_loss}")
    if args.attempt_time is not None:
        print(f"[cfg] every CP-SAT call is capped at {args.attempt_time:g}s; "
              "the board deadline still wins")
    if args.search_mode == "improve" and "--stall_time" in sys.argv:
        print("[note] --stall_time applies only to --search_mode optimize; "
              "in improve mode the first witness ends the call. Ignoring it.",
              flush=True)
    if verbose := args.verbose:
        print(f"[cfg] focus_limit={policy['focus_limit']} "
              f"focus_reach={policy['focus_reach']} "
              f"focus_inner_cap={policy['focus_inner_cap']} "
              f"focus_ring_radius={policy['focus_ring_radius']} "
              f"focus_share={policy['focus_share']:.2f}")
        print(f"[cfg] broad_inner_cap={inner_cap} ring_radius={ring_radius} "
              f"mode={args.mode} patch_shape={args.patch_shape}")

    # Select board rows first, then shard the selected window.  This gives every
    # process a balanced interleaving when per-board runtimes vary widely.
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
        print(f"[resume] {resume_rows} completed row(s) already in {out_path}")
    else:
        out_mode = "w"
    rows = itertools.islice(selected_rows(), resume_rows, None)

    run_start = time.time()
    done = gained = reused = 0
    duplicate_cache = {}
    with open(args.output, out_mode, newline="") as out:
        writer = csv.writer(out, lineterminator="\n")
        for input_idx, row in rows:
            if _STOP:
                break
            partial = parse_partial_line(row)
            bin_ = len(broken_junctions(partial.pos, partial.rot, tiles))
            fp = board_fingerprint(partial.pos, partial.rot)

            if policy["duplicate_policy"] == "reuse" and fp in duplicate_cache:
                pos, rot, breaks = duplicate_cache[fp]
                score = NUM_EDGES - breaks
                writer.writerow([partial.config_id, str(score)]
                                + [str(x) for x in pos] + [str(x) for x in rot])
                out.flush()
                done += 1
                reused += 1
                gained += bin_ - breaks
                print(f"[{input_idx + 1}] {partial.config_id} | {bin_:>2}->{breaks:>2} "
                      f"| reused exact duplicate | profile={args.profile}", flush=True)
                continue

            orient = None
            if clue_mask:
                if args.clue_orient == "auto":
                    orient, _n = CL.clue_orient(partial.pos, partial.rot,
                                                clue_mask)
                else:
                    orient = int(args.clue_orient)
                if orient is None:
                    print(f"[{input_idx + 1}] [WARN] no enabled clue identifies "
                          "an orientation; solving unpinned", flush=True)

            if verbose:
                print(f"\n[{input_idx + 1}] {partial.config_id} | input breaks {bin_}",
                      flush=True)
            pos, rot, breaks, reason, stage, sec, bstats = solve_board(
                args.mode, partial, tiles, holes, policy, workers,
                args.rng_seed + 104729 * input_idx, verbose,
                search_mode=args.search_mode,
                preserve_clean=args.preserve_clean,
                preserve_side=args.preserve_side,
                new_break_reference=args.new_break_reference,
                donors_per_target=args.donors_per_target,
                donor_cells_max=args.donor_cells_max,
                accept_equal_compaction=args.accept_equal_compaction,
                no_gain_limit=args.no_gain_limit,
                symmetry_level=args.symmetry_level,
                linearization_level=args.linearization_level,
                repair_hint=args.repair_hint,
                hint_conflicts=args.hint_conflicts,
                log_search=args.log_search,
                CL=CL, clue_mask=clue_mask, orient=orient)

            score = NUM_EDGES - breaks
            writer.writerow([partial.config_id, str(score)]
                            + [str(x) for x in pos] + [str(x) for x in rot])
            out.flush()
            done += 1
            gained += bin_ - breaks
            if policy["duplicate_policy"] == "reuse":
                duplicate_cache[fp] = (list(pos), list(rot), breaks)
            print(f"[{input_idx + 1}] {partial.config_id} | {bin_:>2}->{breaks:>2} "
                  f"| gain={bin_ - breaks:+d} time={sec:6.1f}s "
                  f"attempts={bstats['attempts']:2d} accepted={bstats['accepted']:2d} "
                  f"last={stage} end={reason}", flush=True)

    elapsed = time.time() - run_start
    print("\n=== run summary ===")
    print(f"[sum] {done} board(s) in {elapsed:.1f}s, {gained:+d} break(s) net"
          + (f", {reused} exact duplicate(s) reused" if reused else "")
          + f" -> {args.output}")
    if _STOP:
        print("[sum] stopped cleanly; every row already written is complete")


if __name__ == "__main__":
    main()
