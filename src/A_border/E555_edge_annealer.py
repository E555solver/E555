#!/usr/bin/env python3
"""
E555_edge_annealer.py -- Stage A of the E555 pipeline: border annealer.

WHAT IT DOES

    Searches for an assignment of the 60 border pieces (4 corners + 56 edges)
    to the four sides of the 16x16 board that makes every side rich in valid
    orderings. Each side defines a directed multigraph (nodes = interface
    colors, arcs = the color pair each edge piece exposes); a legal ordering
    of the pieces along a side is an Euler trail of that graph, and the exact
    trail count is computed with the BEST theorem (arborescence count via an
    exact integer Bareiss determinant, times factorials of out-degrees).

    A simulated-annealing loop with a tabu list swaps edge pieces between
    sides (and corner pieces between corners early in each restart), driven
    by one of two objectives:

      default          maximize  sum_over_sides(w_side * log(euler_count)),
                       subject to all four sides being Euler-trail feasible;
                       hard penalties push infeasible states toward
                       feasibility, then the signed log-count takes over.
                       With the default weights -- all four +1 -- this simply
                       pushes every side as rich as it will go, which is the
                       usual thing to want. A negative weight minimizes its
                       side instead, so +9/-2/-5/-2 asks for one very rich
                       side and three starved ones.
      --target_scale N  drive every side toward its own target of w_side * N
                       trails rather than as far as it will go. Each side is
                       scored on how many DECADES it sits from its target, so
                       the penalty depends on the ratio alone: a side 2x off
                       costs the same whether its target is 250 or 15000, and
                       a big side cannot drown out a small one. 100 points =
                       every side on target, 25 points lost per decade per
                       side. Use it when a side that is too rich is as
                       unhelpful as one that is too poor.

WARM START -- refining a border you already have

    --input rotations.csv --row N takes one row of this script's own output as
    the starting border for every restart, so a border with the shape you want
    can be refined instead of rediscovered. The row is re-searched --restarts
    times; the restarts start from the same border and diverge only through
    their RNG streams. The weights in force need not be the ones that produced
    the file -- a row found under one objective is a fine starting point for
    another -- and the header reports the row's score under both.

    Rows are numbered from 0 over data rows only, the same numbering the
    beamer's --start_row uses. The four trail counts are recomputed from the
    row and checked against its comment, which catches the wrong seed file
    before any time is spent. A warm start keeps the corner seats the row came
    with and never swaps them, and it cannot lose ground: the starting border
    is itself eligible to be the restart's best, so a refinement hands back
    that row or something better than it.

    --input FILE without --row refines EVERY row of the file, and writes one
    row back per input row, in input order: each row gets --restarts restarts
    and only its best is kept. The (row, restart) jobs share one worker pool,
    so --restarts 1 is one worker per row. Every row is checked, and its
    schedule probed, in the parent before any restart runs, and each row is
    run exactly as `--row N` would run it -- same seeds, same schedule -- so
    any row of the result can be reproduced on its own with --row N and the
    same --rng_seed. stdout gets one line per row, with the score and every
    side's trail count before and after:

        row  3  polish  score=    9.2658 ->    10.4890 (+1.2232)  TOP 483840->483840 (+0)  ...

DOUBLE DECKER -- two-tall border segments (--double_decker [SIDES])

    A classic side is scored by the orderings of its 14 edge pieces. With
    --double_decker, the named sides (comma list; bare = all four) are scored
    by their TWO rows instead: every order of the edges times every way to put
    an inner piece under each so the inner row chains too, drawn from a
    RESERVE of --decker_reserve (24) inner pieces the search picks for that
    side, each used at most once. The count is exact, and it is the side's
    Decker= figure. The search finds the border and the reserves together, so
    what comes out is a border whose requested sides can be completed two rows
    deep in many ways from a small, named set of pieces -- the outer two rings
    being exactly where the best known boards still break.

    Why a reserve: pairing each edge with one inner piece and counting the
    reorderings gave 6-36 on real borders, and re-pairing the same 12 pieces
    gave no more -- a fixed pairing is a rigid block. On one top strip a
    searched reserve of 20/24/30 pieces reached ~208/256/1,094 layouts, and the
    whole free pool bounds it at ~1e8 (the DeckerPool= figure). A larger
    reserve allows more layouts; every reserved piece is one a later stage
    would have to hold back.

    Every corner next to a two-tall side is a fixed 2x2 BLOCK -- the corner
    piece, the edge piece on each side of it and the inner piece diagonal to it
    -- because the second ring's corner cells touch two sides at once. A strip
    runs between the blocks at its ends; a classic side next to a block keeps
    that block's edge fixed at its end. The five clue pieces, whose cells lie
    elsewhere, are never reserved.

    The moves swap reserve pieces (mostly for ones that chain with what is
    already reserved), trade them between sides, change a block, or move edges
    between sides; --decker_keep_border forbids the last, so the spins that
    come back are the input's. A cold restart first runs the classic walk for
    --decker_warmup (0.3) of its steps to find a usable border; a warm start
    builds on the input row and cannot come back below its seeded reserve.

    Each written border then carries:
      - in the comment, `Decker=416/-/-/-` (each two-tall side's exact reserve
        count, TOP/RIGHT/BOTTOM/LEFT, `-` for a classic side), `DeckerPool=`
        (the whole-pool bound) and `Board=dd<seed>_r<N>`; TOP=.. and the rest
        stay the CLASSIC counts of the spins, so every reader of the file
        reads it as before, and Score= is the double-decker objective;
      - spin 1 instead of 0 on every reserved piece and every block's inner
        piece -- nothing reads an inner spin out of a rotations row today, so
        this only marks them, for a later reservation;
      - one board per row in --decker_out (default <out stem>_decker.csv), in
        the beamer's own line format under the Board= name, preceded by a `#`
        line listing each side's reserve: the border ring plus the two-tall
        sides' first inner ring, laid out as one layout drawn uniformly from the
        counted ones, 999 elsewhere. The finalizer loads it in fixed-sides mode
        at --finalize_from 0, and at 1 when BOTTOM is two tall.
    E555_sort_rotations.py never turns such a row and E555_rotate.py refuses
    the file: a turn would leave the board, the flags and Decker= behind.

TEMPERATURE

    --T0/--Tf are resolved from the starting state unless given explicitly,
    and the header says what was chosen and why. There are two schedules,
    ~50x apart, and a startup probe of the starting state picks between them:

      polishing   the border already sits near an optimum of the weights in
                  force, so heat only destroys it: T0 = 0.5 * the probed move
                  scale. This is the --input case when you are refining a row
                  under weights it already suits.
      searching   the border is far from what these weights want -- a random
                  start always, an --input row scored against an objective it
                  was never annealed for sometimes -- so it wants the cold
                  schedule, anchored to the feasibility cliff.

    What the temperature straddles is that cliff, not the objective: best
    borders are harvested from every candidate evaluated, only a feasible one
    is eligible, and an infeasible state scores at least 45 points worse. So
    T0 decides how much of a run is spent harvesting nothing. See
    AnnealingConfig.cold_schedule for the cold anchor and the sweep behind it,
    probe_move_scale for the measurement, and resolve_schedule for how the two
    are told apart.

OUTPUT

    Every restart's best border -- every row's best, when a whole file is
    refined -- is appended to a rotations CSV that Stage B reads directly:
    one `#` comment line with the per-side trail counts, then
    `id, spin[0..255]` (the 60 border spins from the search, zeros for the
    196 inner pieces). That file is the deliverable and is written in both
    output modes. A warm run adds `From=<file>:row<N>` to each comment, so a
    refined pool still says what it was refined from.

    --out names the file. Without it the borders go to <stem>_refined.csv
    beside the --input file, or to rotations.csv in the current directory on
    a cold run -- there is always a file. (A run without --out used to print
    its borders nowhere outside --verbose, and so lose every one of them.)

    On stdout the default is one line per restart -- score, the four trail
    counts, the step the best was found at, and the time:

        restart  3/50  score=  79.0011  TOP= 4102  ...  step=299002  19.8s

    --verbose instead prints the whole search: the full config, the per-step
    temperature/acceptance/count reports, and each restart's border as a
    labelled line that `grep '^BEST,' `collects:

        BEST,Restart,r,Step,s,Score,x,TOP=..,RIGHT=..,BOTTOM=..,LEFT=..,Rot,<60 spins>

PARALLELISM

    Restarts are independent, so --threads runs them in parallel, one worker
    process per restart (processes, not threads: the hot loop is pure Python
    and the GIL would serialize it anyway). --threads 0, the default, uses
    every core. Each restart derives its own RNG seed from --rng_seed and its
    own index, so THE THREAD COUNT NEVER CHANGES THE RESULT, and the parent
    emits every restart in restart order however the workers finish. When a
    whole file is refined the jobs are (row, restart) pairs, and the parent
    writes each row once all its restarts are in, in input order.

USAGE

    # the default: push all four sides as rich as they will go
    python3 -u E555_edge_annealer.py seed_Edge5.txt --out rotations.csv \
      --restarts 50 --steps 500000 --threads 8

    # refine the row you liked out of that pool
    python3 -u E555_edge_annealer.py seed_Edge5.txt --out refined.csv \
      --input rotations.csv --row 3 --restarts 8 --steps 500000

    # or refine every row of it, one output row per input row, in order
    # (no --out: this writes rotations_refined.csv)
    python3 -u E555_edge_annealer.py seed_Edge5.txt \
      --input rotations.csv --restarts 2 --steps 500000 --threads 8

    # make the top two rows rich, with a reserve of 24 inner pieces and a
    # witness board per border (rotations_refined.csv + ..._decker.csv);
    # add --decker_keep_border to keep the borders exactly as they are
    python3 -u E555_edge_annealer.py seed_Edge5.txt \
      --input rotations.csv --double_decker TOP --restarts 2 --steps 20000

    # shape it instead: one rich side, three starved
    python3 -u E555_edge_annealer.py seed_Edge5.txt --out rotations.csv \
      --restarts 10 --steps 100000 --w_bottom -3 --w_left -1 --w_right 3 --w_top 5

    # or aim every side at a size rather than at a maximum
    python3 -u E555_edge_annealer.py seed_Edge5.txt --out rotations.csv \
      --restarts 10 --steps 100000 --threads 8 --verbose \
      --target_scale 250  --w_bottom 1 --w_left 20 --w_right 20 --w_top 60
"""
from __future__ import annotations

import argparse
import functools
import signal
import math
import os
import re
import time
import random
from collections import Counter, defaultdict, deque
from concurrent.futures import CancelledError, ProcessPoolExecutor
from dataclasses import dataclass, field, asdict, replace
from enum import IntEnum
from math import factorial
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

# =============================================================================
# Geometry
# =============================================================================

class Side(IntEnum):
    TOP = 0; RIGHT = 1; BOTTOM = 2; LEFT = 3

class Corner(IntEnum):
    TL = 0; TR = 1; BR = 2; BL = 3

EDGE_PER_SIDE = 14          # 56 edge pieces over four sides
N_BORDER      = 60          # 4 corners + 56 edges, always pieces 1..60
N_SPINS       = 256         # the full spin vector a rotations row carries

SIDE_NAMES   = {Side.TOP: "TOP", Side.RIGHT: "RIGHT", Side.BOTTOM: "BOTTOM", Side.LEFT: "LEFT"}
CORNER_NAMES = {Corner.TL: "TL", Corner.TR: "TR", Corner.BR: "BR", Corner.BL: "BL"}

CORNER_ZERO_SIDES = {
    Corner.TL: frozenset((Side.TOP, Side.LEFT)),
    Corner.TR: frozenset((Side.TOP, Side.RIGHT)),
    Corner.BR: frozenset((Side.BOTTOM, Side.RIGHT)),
    Corner.BL: frozenset((Side.BOTTOM, Side.LEFT)),
}

def get_fixed_corner_mapping(option: int, sorted_c: List[int]) -> Dict[int, Corner]:
    if option == 1:
        indices = {Corner.TL: 1, Corner.TR: 0, Corner.BR: 2, Corner.BL: 3}
    elif option == 2:
        indices = {Corner.TL: 2, Corner.TR: 0, Corner.BR: 1, Corner.BL: 3}
    else:
        raise ValueError(f"Invalid --fix_corners value: {option}")
    return {sorted_c[idx]: c for c, idx in indices.items()}

# =============================================================================
# Configuration
# =============================================================================

@dataclass(frozen=True)
class AnnealingConfig:
    # Schedule
    restarts:            int   = 3
    steps_per_restart:   int   = 250_000
    random_seed:         int   = 0
    # The restart is the unit of parallelism: each one is an independent walk
    # from its own random start, seeded from (random_seed, restart index) so
    # the thread count never changes the result. 0 = one worker per core.
    threads:             int   = 0
    # What the temperature has to straddle is the FEASIBILITY CLIFF, not the
    # objective. Best boards are harvested from every candidate evaluated, not
    # only from accepted ones -- but only a feasible candidate is eligible, and
    # an infeasible state scores -(infeasible_band + hard), a drop of 45 points
    # at the very least. So T0 decides how much of the run is spent below the
    # cliff harvesting nothing, and that is the whole effect: see
    # cold_schedule() for the anchor and the sweep behind it.
    #
    # These two defaults are cold_schedule()'s values for the penalty constants
    # below; resolve_schedule() recomputes them from whatever the constants
    # actually are, and calibrates them to the starting state on a warm start.
    T0:                  float = 11.25     # initial temperature
    Tf:                  float =  9.0      # final temperature
    report_every:        int   = 25_000

    # Scoring function weights. In --target_scale mode they set each side's
    # target (w_side * target_scale) and must be positive; in the default
    # linear mode a positive weight maximizes that side's Euler count and a
    # negative one minimizes it.
    w_top:    float = 1.0   #  9.0
    w_right:  float = 1.0   # -2.0
    w_bottom: float = 1.0   # -5.0
    w_left:   float = 1.0   # -2.0

    # Hard feasibility penalties, in the same points unit as the objective.
    # An infeasible state scores -(infeasible_band + hard): the band is one
    # side sitting a full decade off target, small enough that early in the
    # schedule the search can still tunnel through an infeasible state to
    # cross a barrier, and hopeless by the time the temperature is down.
    infeasible_band:                     float = 25.0
    balance_penalty_weight:              float = 10.0
    disconnected_penalty_weight:         float = 10.0
    infeasible_euler_penalty_weight:     float = 25.0
    infeasible_inventory_penalty_weight: float = 100.0

    # Move mix: fraction that are edge swaps (remainder = corner swaps, first 20% only)
    p_edge: float = 0.90

    # Tabu list length on edge-swap pairs (0 = disabled)
    tabu_length: int = 128

    # Corner mode: 0 = random, 1 = fix by edge-commutativity, 2 = fix by corner-commutativity
    fix_corners: int = 0

    # Per-side trail target = w_side * target_scale (None/0 = linear objective)
    target_scale: int = None

    # Warm start: the 60 border spins every restart begins from (--input),
    # or None for a fresh random assignment per restart.
    start_spins: Optional[Tuple[int, ...]] = None

    # Cold starts permute the corners early; a warm start keeps the corner
    # seats its input row came with, because that row was chosen for the
    # border it is and the four corners set every side's endpoints.
    swap_corners: bool = True

    # Print the whole search (config dump, per-step reports, BEST lines)
    # instead of one summary line per restart.
    verbose: bool = False

    # --double_decker: the sides scored as two-tall strips (empty = classic),
    # the inner pieces reserved per such side, whether the border is frozen,
    # the share of a cold restart spent in the classic warm-up, the weight of
    # one broken match inside a corner block, and an explicit schedule for the
    # double-decker phase of a cold restart (None = probed in the worker).
    decker_sides:         Tuple[int, ...] = ()
    decker_reserve:       int = 24
    decker_keep_border:   bool = False
    decker_warmup:        float = 0.3
    block_penalty_weight: float = 10.0
    decker_T0:            Optional[float] = None
    decker_Tf:            Optional[float] = None

    def w_side(self, side: Side) -> float:
        return {Side.TOP: self.w_top, Side.RIGHT: self.w_right,
                Side.BOTTOM: self.w_bottom, Side.LEFT: self.w_left}[side]

    @property
    def cliff_floor(self) -> float:
        """The shallowest the feasibility cliff can be, in score points.

        An infeasible state scores -(infeasible_band + hard), and the smallest
        possible hard is one unbalanced side: degree surpluses sum to zero, so
        a side that fails balance is off by at least +1 somewhere and -1
        somewhere else, for a balance_penalty of 2. Every deeper violation only
        widens the gap, so this is the floor -- and it is set by the penalty
        constants alone, which is why it is the same number in both scoring
        modes."""
        return self.infeasible_band + 2.0 * self.balance_penalty_weight

    def cold_schedule(self) -> Tuple[float, float]:
        """T0/Tf for a random start, as fractions of the cliff floor.

        Swept at 16 restarts x 60k steps x 2 master seeds = 32 samples a cell.
        On the default objective the optimum plateau is T0 in [10, 20] with Tf
        in [8, 12] -- best cell (10, 10) at mean 8.267, against 7.600 for the
        (1000, 8) this script used to ship, which was the worst cell tested.
        The same T0 region won in log-sum +9/-2/-5/-2 (1.22 vs 0.98) and in
        --target_scale 250 (86.3 vs 81.7), and that the winner is the same
        ABSOLUTE temperature in modes whose move sizes differ 40x is what says
        the cliff sets it, not the objective.

        Re-checked at the depth real runs use -- 16 restarts x 500k steps --
        where these ratios are the best of the four cells tried and the old
        default is half a point behind:

            T0=11.25 Tf=9   mean 8.6901  best 9.2374   (cliff/4, cliff/5)
            T0=10    Tf=10  mean 8.5862  best 8.8442
            T0=20    Tf=8   mean 8.5670  best 8.9254
            T0=1000  Tf=8   mean 8.1791  best 8.8072   (what used to ship)

        Expressed as ratios so they follow the penalty constants if those are
        ever retuned."""
        return self.cliff_floor / 4.0, self.cliff_floor / 5.0

# =============================================================================
# Core immutable types
# =============================================================================

@dataclass(frozen=True)
class Piece:
    id: int
    sides: Tuple[int, int, int, int]   # (TOP, RIGHT, BOTTOM, LEFT) colors

    @property
    def zero_count(self) -> int:
        return sum(1 for v in self.sides if v == 0)

@dataclass(frozen=True)
class SideArc:
    """One edge piece's contribution to a side's directed multigraph."""
    piece_id:     int
    source_color: int
    target_color: int
    inward_color: int

@dataclass
class SideEvaluation:
    side:            Side
    start_color:     int
    end_color:       int
    arcs:            List[SideArc]
    balance_penalty: int
    # Only defined when balance_penalty == 0. Nothing reads it otherwise (see
    # `feasible` below and hard_penalty), which is what lets evaluate_side skip
    # the connectivity BFS on an already-unbalanced side.
    weakly_connected: bool
    euler_count:     int
    outdegree:       Counter = field(default_factory=Counter)
    indegree:        Counter = field(default_factory=Counter)

    @property
    def feasible(self) -> bool:
        return self.balance_penalty == 0 and self.weakly_connected and self.euler_count > 0

    @property
    def log_raw(self) -> float:
        return math.log(max(1, self.euler_count))

# =============================================================================
# Swap result containers (avoid mutable tuple indexing errors)
# =============================================================================

@dataclass
class EdgeSwapResult:
    new_score:  float
    hard:       float                # 0.0 iff the candidate border is usable
    side_a:     Side
    side_b:     Side
    new_arcs_a: Dict[int, SideArc]   # new arc dict for side_a
    new_arcs_b: Dict[int, SideArc]   # new arc dict for side_b
    new_se_a:   SideEvaluation
    new_se_b:   SideEvaluation
    new_inward: Counter
    new_arc_a:  SideArc   # arc for pid_a on its new side (side_b)
    new_arc_b:  SideArc   # arc for pid_b on its new side (side_a)

@dataclass
class CornerSwapResult:
    new_score:     float
    hard:          float                # 0.0 iff the candidate border is usable
    new_endpoints: Dict[Side, Tuple[int, int]]
    new_evals:     Dict[Side, SideEvaluation]

# =============================================================================
# Best record — lightweight snapshot for reporting
# =============================================================================

@dataclass
class BestRecord:
    score:        float
    euler_counts: Dict[Side, int]
    rot_vec:      List[int]   # rotations for pieces 1..60
    step:         int
    # --double_decker only. euler_counts stays the CLASSIC count of the border
    # (what the comment's TOP=.. fields and every reader of them expect);
    # dd_counts is the objective's own per-side count, board the witness
    # layout as 0-based (pos, rot) vectors, flags the reserved inner pieces.
    dd_counts:    Optional[Dict[Side, int]] = None
    board:        Optional[Tuple[List[int], List[int]]] = None
    flags:        Tuple[int, ...] = ()
    dd_pool:      Optional[Dict[Side, int]] = None           # whole-pool bounds
    reserve:      Optional[Dict[Side, Tuple[int, ...]]] = None

@dataclass
class RestartResult:
    """One restart's whole yield, shipped back from a worker process. The
    worker never prints and never touches the --out file: it hands its log
    to the parent, which replays the restarts in order whatever order they
    finished in."""
    restart: int
    best:    Optional[BestRecord]   # None = no feasible border found
    log:     List[str]
    elapsed: float
    warning: Optional[str] = None   # the freeze warning, when the log has one

@dataclass
class RowPlan:
    """One --input row of a whole-file refinement, settled in the parent
    before any restart runs: the config its restarts get (its spins, its own
    T0/Tf) and what its stdout line compares the result against."""
    row:      int
    lineno:   int
    config:   AnnealingConfig
    start:    Dict[Side, int]       # the row's own trail counts
    baseline: Optional[float]       # its score now; None = not a usable border
    kind:     str                   # "polish" or "search"
    checked:  bool                  # its comment carried counts to verify
    header:   List[str]             # warm start + schedule lines, for --verbose

# =============================================================================
# Mutable hot state (maintained incrementally)
# =============================================================================

@dataclass
class RunState:
    edge_side:    Dict[int, Side]                # edge piece_id → current side
    corner_pos:   Dict[int, Corner]              # corner piece_id → current position
    arcs:         Dict[Side, Dict[int, SideArc]] # side → {piece_id: SideArc}
    evals:        Dict[Side, SideEvaluation]
    endpoints:    Dict[Side, Tuple[int, int]]    # (start_color, end_color) per side
    inward_tally: Counter
    score:        float

    def all_feasible(self) -> bool:
        return all(se.feasible for se in self.evals.values())

    def euler_counts(self) -> Dict[Side, int]:
        return {s: self.evals[s].euler_count for s in Side}

# =============================================================================
# Input / rotation utilities
# =============================================================================

def read_pieces(path: str) -> List[Piece]:
    pieces: List[Piece] = []
    with Path(path).open(encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            stripped = line.strip()
            if not stripped:
                continue
            vals = tuple(int(x) for x in stripped.split())
            if len(vals) != 4:
                raise ValueError(f"Line {lineno}: expected 4 integers, got {len(vals)}")
            pieces.append(Piece(id=len(pieces) + 1, sides=vals))
    return pieces

def rotate_sides(sides: Tuple[int, int, int, int], r: int) -> Tuple[int, int, int, int]:
    r %= 4
    return tuple(sides[(i + r) % 4] for i in range(4))

def find_rotation_for_edge_side(piece: Piece, target: Side) -> int:
    for r in range(4):
        if rotate_sides(piece.sides, r)[target] == 0:
            return r
    raise ValueError(f"Piece {piece.id} has no zero on {SIDE_NAMES[target]}")

def find_rotation_for_corner(piece: Piece, target: Corner) -> int:
    wanted = CORNER_ZERO_SIDES[target]
    for r in range(4):
        rot = rotate_sides(piece.sides, r)
        if frozenset(Side(i) for i, v in enumerate(rot) if v == 0) == wanted:
            return r
    raise ValueError(f"Piece {piece.id} cannot be placed at {CORNER_NAMES[target]}")

def classify_boundary_pieces(pieces: Sequence[Piece]) -> Tuple[List[int], List[int]]:
    boundary = pieces[:60]
    corner_ids = [p.id for p in boundary if p.zero_count == 2]
    edge_ids   = [p.id for p in boundary if p.zero_count == 1]
    return corner_ids, edge_ids

def build_inner_capacity(pieces: Sequence[Piece]) -> Counter:
    cap: Counter = Counter()
    for p in pieces:
        if p.zero_count == 0:
            for c in p.sides:
                cap[c] += 1
    return cap

# =============================================================================
# Warm start -- reading a border back out of a rotations row
# =============================================================================

# A border piece's spin IS its side: the grey (zero) face points at the frame
# edge the piece sits on. So the 60 spins of a rotations row carry the whole
# assignment, and recovering it is only a matter of reading off where each
# grey face points.
CORNER_BY_ZERO_SIDES = {v: k for k, v in CORNER_ZERO_SIDES.items()}

# Both comment forms in circulation: `TOP=4320 ... Score=9.7510` as this script
# writes it, and the older comma form `Score,38.0099,TOP,46080,` still in
# data/borders_annealed_fix12.csv.
SIDE_COUNT_RE = re.compile(r"\b(TOP|RIGHT|BOTTOM|LEFT)\s*[=,]\s*(\d+)")
SCORE_RE      = re.compile(r"\bScore\s*[=,]\s*(-?[\d.]+(?:[eE][-+]?\d+)?)")

def read_rotations(path: str) -> List[Tuple[List[int], Optional[str], int]]:
    """Every data row of a rotations CSV as (spins, the comment above it, lineno).

    Deliberately as tolerant as the C reader this file's output is written for
    (read_one_border_row, src/B_beam/E555_database.c): `#` and `%` comment
    lines and blank lines are skipped and do NOT advance the row index, and the
    LAST 256 fields of a row are the spins -- so the 257-field shape written
    below and the 256/258 variants all parse. Rows are therefore numbered from
    0 over data rows only, the same numbering the beamer's --start_row uses, so
    a row picked out of a file is the same row in both tools.

    The comment rides along because that is where the trail counts and the
    score live, one line above the row they describe."""
    rows: List[Tuple[List[int], Optional[str], int]] = []
    pending: Optional[str] = None
    with Path(path).open(encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\r\n")
            if not line.strip():
                continue
            if line.lstrip().startswith(("#", "%")):
                pending = line
                continue
            fields = [f for f in line.split(",") if f.strip()]
            if len(fields) < N_SPINS:
                raise SystemExit(f"[ERROR] {path}:{lineno}: {len(fields)} field(s), "
                                 f"need at least the {N_SPINS} spins")
            try:
                spins = [int(f) for f in fields[-N_SPINS:]]
            except ValueError:
                raise SystemExit(f"[ERROR] {path}:{lineno}: spin field is not an integer")
            if any(not 0 <= v <= 3 for v in spins):
                raise SystemExit(f"[ERROR] {path}:{lineno}: spin outside 0..3")
            rows.append((spins, pending, lineno))
            pending = None
    return rows

def border_from_spins(pieces_by_id: Dict[int, Piece],
                      spins: Sequence[int]) -> Tuple[Dict[int, Side], Dict[int, Corner]]:
    """(edge_side, corner_pos) from the 60 border spins -- the inverse of
    rotation_vector().

    Validated the way the finalizer's own fin_rot_row_valid does it
    (src/B_beam/E555_finalizer.c): one grey face names a side, two name a
    corner, three or more is not a border orientation; then every side must
    hold 14 pieces and every board corner exactly one. Failure is fatal rather
    than a warning, because the row was named deliberately -- annealing from a
    partition the rest of the pipeline cannot read would waste the whole run."""
    edge_side: Dict[int, Side] = {}
    corner_pos: Dict[int, Corner] = {}
    for pid in range(1, N_BORDER + 1):
        rot  = rotate_sides(pieces_by_id[pid].sides, spins[pid - 1])
        grey = frozenset(Side(i) for i, v in enumerate(rot) if v == 0)
        if len(grey) == 1:
            edge_side[pid] = next(iter(grey))
        elif grey in CORNER_BY_ZERO_SIDES:
            corner_pos[pid] = CORNER_BY_ZERO_SIDES[grey]
        else:
            raise SystemExit(
                f"[ERROR] piece {pid} at spin {spins[pid - 1]} shows {len(grey)} grey "
                f"face(s) {sorted(SIDE_NAMES[g] for g in grey)}: not a border "
                f"orientation. Is this the seed file the row was made from?")
    per_side = Counter(edge_side.values())
    if any(per_side[s] != EDGE_PER_SIDE for s in Side):
        raise SystemExit(
            "[ERROR] not a legal border partition: "
            + " ".join(f"{SIDE_NAMES[s]}={per_side[s]}" for s in Side)
            + f" (every side needs {EDGE_PER_SIDE})")
    if len(set(corner_pos.values())) != len(Corner):
        raise SystemExit(
            f"[ERROR] not a legal border partition: {len(corner_pos)} corner piece(s) "
            f"on {len(set(corner_pos.values()))} distinct seat(s), need one each")
    return edge_side, corner_pos

def counts_in_comment(comment: Optional[str]) -> Optional[Dict[str, int]]:
    """The four trail counts a rotations comment carries, or None when it does
    not carry all four. Used to check a row against the seed file it is being
    annealed with: the counts are recomputed from the reconstructed state, and
    they can only agree if the row and the piece set belong together."""
    if not comment:
        return None
    found = {m.group(1): int(m.group(2)) for m in SIDE_COUNT_RE.finditer(comment)}
    return found if len(found) == len(Side) else None

def score_in_comment(comment: Optional[str]) -> Optional[float]:
    """The score a rotations comment records, or None. It is the score under
    whatever weights produced the file, which need not be the ones in force
    now -- it is reported for comparison, never used."""
    if not comment:
        return None
    m = SCORE_RE.search(comment)
    return float(m.group(1)) if m else None


# =============================================================================
# Geometry helpers
# =============================================================================

def edge_arc(pid: int, side: Side, rotated: Tuple[int, int, int, int]) -> SideArc:
    """Build a SideArc for an edge piece already rotated into position."""
    if side == Side.TOP:    return SideArc(pid, rotated[Side.LEFT],  rotated[Side.RIGHT], rotated[Side.BOTTOM])
    if side == Side.BOTTOM: return SideArc(pid, rotated[Side.LEFT],  rotated[Side.RIGHT], rotated[Side.TOP])
    if side == Side.LEFT:   return SideArc(pid, rotated[Side.TOP],   rotated[Side.BOTTOM], rotated[Side.RIGHT])
    if side == Side.RIGHT:  return SideArc(pid, rotated[Side.TOP],   rotated[Side.BOTTOM], rotated[Side.LEFT])
    raise AssertionError("unreachable")

def corner_endpoints(rotated_corners: Dict[Corner, Tuple[int, int, int, int]]) -> Dict[Side, Tuple[int, int]]:
    tl, tr, br, bl = (rotated_corners[c] for c in (Corner.TL, Corner.TR, Corner.BR, Corner.BL))
    return {
        Side.TOP:    (tl[Side.RIGHT],  tr[Side.LEFT]),
        Side.RIGHT:  (tr[Side.BOTTOM], br[Side.TOP]),
        Side.BOTTOM: (bl[Side.RIGHT],  br[Side.LEFT]),
        Side.LEFT:   (tl[Side.BOTTOM], bl[Side.TOP]),
    }

# =============================================================================
# Exact Euler-trail counting (BEST theorem + Bareiss determinant)
# =============================================================================

def bareiss_determinant(matrix: List[List[int]]) -> int:
    n = len(matrix)
    if n == 0:
        return 1
    a = [row[:] for row in matrix]
    sign, prev = 1, 1
    for k in range(n - 1):
        pivot_row = next((r for r in range(k, n) if a[r][k] != 0), None)
        if pivot_row is None:
            return 0
        if pivot_row != k:
            a[k], a[pivot_row] = a[pivot_row], a[k]
            sign *= -1
        pivot = a[k][k]
        for i in range(k + 1, n):
            for j in range(k + 1, n):
                a[i][j] = (a[i][j] * pivot - a[i][k] * a[k][j]) // prev
        prev = pivot
        for i in range(k + 1, n): a[i][k] = 0
        for j in range(k + 1, n): a[k][j] = 0
    return sign * a[n - 1][n - 1]

def _weakly_connected(arcs: Sequence[Tuple[int, int]], relevant) -> bool:
    verts = set(relevant)
    if not verts:
        return True
    graph: Dict[int, List[int]] = defaultdict(list)
    for u, v in arcs:
        graph[u].append(v); graph[v].append(u)
        verts.update((u, v))
    seen = {next(iter(verts))}
    q = deque(seen)
    while q:
        for v in graph[q.popleft()]:
            if v not in seen:
                seen.add(v); q.append(v)
    return verts <= seen

def _arborescence_count(arcs: Sequence[Tuple[int, int]], root: int) -> int:
    verts = sorted({root} | {c for a in arcs for c in a})
    idx = {v: i for i, v in enumerate(verts)}
    n = len(verts)
    outdeg = Counter(u for u, _ in arcs)
    lap = [[0] * n for _ in range(n)]
    for v in verts:
        lap[idx[v]][idx[v]] = outdeg[v]
    for (u, v), m in Counter(arcs).items():
        lap[idx[u]][idx[v]] -= m
    ri = idx[root]
    minor = [[lap[i][j] for j in range(n) if j != ri] for i in range(n) if i != ri]
    return bareiss_determinant(minor)

def _euler_balance(arcs, start, end):
    out = Counter(u for u, _ in arcs)
    inn = Counter(v for _, v in arcs)
    penalty = 0
    for c in set(out) | set(inn) | {start, end}:
        obs = out[c] - inn[c]
        exp = 0 if start == end else (1 if c == start else (-1 if c == end else 0))
        penalty += abs(obs - exp)
    return penalty, out, inn

def count_euler_trails(arcs: Sequence[Tuple[int, int]], start: int, end: int) -> int:
    penalty, outdeg, _ = _euler_balance(arcs, start, end)
    if penalty:
        return 0
    relevant = {start, end} | {c for a in arcs for c in a}
    if not _weakly_connected(arcs, relevant):
        return 0
    if start == end:
        if not outdeg[start]:
            return 0
        aug, root, first = list(arcs), start, outdeg[start]
    else:
        aug, root, first = list(arcs) + [(end, start)], end, 1
    aug_out = Counter(u for u, _ in aug)
    arb = _arborescence_count(aug, root)
    if arb <= 0:
        return 0
    branch = 1
    for d in aug_out.values():
        if d > 0:
            branch *= factorial(d - 1)
    return arb * branch * first

# =============================================================================
# Per-side evaluation and global score
# =============================================================================

# Points a side loses for sitting one decade (10x) away from its target, over
# or under. Four sides x 25 = the whole 100-point band. Raising it makes the
# targets stricter and widens the score range, which the temperatures in
# AnnealingConfig are matched to -- change both together or neither.
TARGET_DECADE_PENALTY = 25.0

# Fraction of a restart over which corner pieces may still swap seats. Corner
# swaps move all four sides' endpoints at once, so they belong early, while the
# temperature can still undo them.
CORNER_PHASE = 0.20

def evaluate_side(side: Side, arc_list: List[SideArc], start: int, end: int) -> SideEvaluation:
    """One side's exact Euler-trail count, behind the two cheap tests that gate it.

    Unbalanced degrees already force the count to zero, and `weakly_connected`
    is never read in that case, so the connectivity BFS is skipped there. It is
    not a micro-optimization: 97.4% of evaluations in a real restart take that
    branch, and the BFS is the single most expensive thing in the step loop.
    Skipping it makes a 30k-step restart 1.3x faster (measured 1.27x, 1.29x and
    1.32x under flat weights, the pipeline's 2/3/0/1 and --target_scale 250)
    and is exactly equivalence-preserving: same best score, same step, same
    four counts and the same 60 spins in all three."""
    return evaluate_graph(side, [(a.source_color, a.target_color) for a in arc_list],
                          start, end, arc_list)

def evaluate_graph(side: Side, graph: List[Tuple[int, int]], start: int, end: int,
                   arc_list: Sequence[SideArc] = ()) -> SideEvaluation:
    """evaluate_side on bare (source, target) pairs. The node labels are only
    compared, so any ints do: frame colours on a classic side, (frame, inner)
    pairs packed by decker_node on a double-decker strip."""
    penalty, out, inn = _euler_balance(graph, start, end)
    if penalty:
        return SideEvaluation(side=side, start_color=start, end_color=end, arcs=arc_list,
                              balance_penalty=penalty, weakly_connected=False,
                              euler_count=0, outdegree=out, indegree=inn)
    relevant = {start, end} | {c for a in graph for c in a}
    conn = _weakly_connected(graph, relevant)
    ec = count_euler_trails(graph, start, end) if conn else 0
    return SideEvaluation(side=side, start_color=start, end_color=end, arcs=arc_list,
                          balance_penalty=penalty, weakly_connected=conn,
                          euler_count=ec, outdegree=out, indegree=inn)

def hard_penalty(evals: Dict[Side, SideEvaluation],
                 inward_tally: Counter,
                 inner_capacity: Counter,
                 config: AnnealingConfig) -> float:
    """Total feasibility violation of a state; 0.0 means a usable border.

    Covers both what makes a *side* unusable (unbalanced degrees, disconnected
    graph, no Euler trail) and the global inner-colour inventory check, which
    no SideEvaluation can see on its own."""
    hard = 0.0
    for side in Side:
        hard += side_penalty(evals[side], config)
    return hard + inventory_penalty(inward_tally, inner_capacity, config)

def side_penalty(se: SideEvaluation, config: AnnealingConfig) -> float:
    """What makes one side unusable: unbalanced degrees, a disconnected graph,
    or no Euler trail at all."""
    hard = config.balance_penalty_weight * se.balance_penalty
    if se.balance_penalty == 0 and not se.weakly_connected:
        hard += config.disconnected_penalty_weight
    if se.balance_penalty == 0 and se.weakly_connected and se.euler_count == 0:
        hard += config.infeasible_euler_penalty_weight
    return hard

def inventory_penalty(tally: Counter, capacity: Counter, config: AnnealingConfig) -> float:
    """The inner-colour inventory check. `tally` counts the colours the placed
    pieces show the unfilled region, `capacity` the colours on the pieces left
    to fill it: every shown colour must be matched by a leftover face, and what
    is left over pairs up among the leftovers themselves, so it must be even.
    Every term is a multiple of 0.5, so the sum is exact in any order."""
    hard = 0.0
    for color in set(capacity) | set(tally):
        if color == 0:
            continue
        ic = capacity.get(color, 0)
        be = tally.get(color, 0)
        if be > ic:
            hard += config.infeasible_inventory_penalty_weight * (be - ic)
        elif (ic - be) % 2:
            hard += config.infeasible_inventory_penalty_weight * 0.5
    return hard

def target_for(side: Side, config: AnnealingConfig) -> float:
    return config.w_side(side) * config.target_scale

def compute_score(evals: Dict[Side, SideEvaluation],
                  inward_tally: Counter,
                  inner_capacity: Counter,
                  config: AnnealingConfig,
                  hard: Optional[float] = None) -> float:
    if hard is None:
        hard = hard_penalty(evals, inward_tally, inner_capacity, config)
    if hard > 0:
        return -config.infeasible_band - hard  # Unacceptable case score

    if config.target_scale:
        # Target balancing: every side is scored on how many DECADES its trail
        # count sits from its own target of w_side * target_scale, so the
        # penalty depends on the ratio and never on the raw magnitude. A side
        # 2x off its target costs the same 2.26 points whether its target is
        # 250 or 15000 -- which is the whole point: a big side cannot drown out
        # a small one, and the weights only choose targets, never importance.
        # Four sides x 25 points = the full 100-point band, so scores stay
        # comparable across runs with different scales.
        return 100.0 - TARGET_DECADE_PENALTY * sum(
            math.log10(evals[s].euler_count / target_for(s, config)) ** 2
            for s in Side)

    # Default linear scoring: weighted sum of log trail counts. The divisor is
    # cosmetic -- it only keeps the number readable -- so it must be the sum of
    # the ABSOLUTE weights: signed weights (the point of this mode) can sum to
    # zero, which used to raise ZeroDivisionError, or to a negative, which used
    # to silently inverse the whole objective. The documented shaping example
    # +9/-2/-5/-2 sums to exactly 0.
    total_weight = sum(abs(config.w_side(s)) for s in Side) or 1.0
    return (1.0 / total_weight) * sum(config.w_side(s) * evals[s].log_raw for s in Side)

# =============================================================================
# RunState construction
# =============================================================================

def _arc_for_edge(pieces_by_id: Dict[int, Piece], pid: int, side: Side) -> SideArc:
    piece = pieces_by_id[pid]
    rot = find_rotation_for_edge_side(piece, side)
    return edge_arc(pid, side, rotate_sides(piece.sides, rot))

def _build_run_state(pieces_by_id: Dict[int, Piece],
                     edge_side: Dict[int, Side],
                     corner_pos: Dict[int, Corner],
                     inner_capacity: Counter,
                     config: AnnealingConfig) -> RunState:
    rotated_corners = {}
    for pid, c in corner_pos.items():
        piece = pieces_by_id[pid]
        rot = find_rotation_for_corner(piece, c)
        rotated_corners[c] = rotate_sides(piece.sides, rot)

    endpoints = corner_endpoints(rotated_corners)

    arcs: Dict[Side, Dict[int, SideArc]] = {s: {} for s in Side}
    inward_tally: Counter = Counter()
    for pid, side in edge_side.items():
        a = _arc_for_edge(pieces_by_id, pid, side)
        arcs[side][pid] = a
        inward_tally[a.inward_color] += 1

    evals = {s: evaluate_side(s, list(arcs[s].values()), *endpoints[s]) for s in Side}
    score = compute_score(evals, inward_tally, inner_capacity, config)

    return RunState(edge_side=edge_side, corner_pos=corner_pos, arcs=arcs,
                    evals=evals, endpoints=endpoints, inward_tally=inward_tally, score=score)

def make_random_state(pieces_by_id: Dict[int, Piece],
                      corner_ids: List[int], edge_ids: List[int],
                      rng: random.Random, config: AnnealingConfig,
                      inner_capacity: Counter) -> RunState:
    if config.fix_corners in (1, 2):
        corner_pos = get_fixed_corner_mapping(config.fix_corners, sorted(corner_ids))
    else:
        shuffled_c = corner_ids[:]
        rng.shuffle(shuffled_c)
        positions = [Corner.TL, Corner.TR, Corner.BR, Corner.BL]
        rng.shuffle(positions)
        corner_pos = {pid: pos for pid, pos in zip(shuffled_c, positions)}

    sides_pool = [s for s in Side for _ in range(EDGE_PER_SIDE)]
    rng.shuffle(sides_pool)
    shuffled_e = edge_ids[:]
    rng.shuffle(shuffled_e)
    edge_side = {pid: side for pid, side in zip(shuffled_e, sides_pool)}

    return _build_run_state(pieces_by_id, edge_side, corner_pos, inner_capacity, config)

def initial_state(pieces_by_id: Dict[int, Piece],
                  corner_ids: List[int], edge_ids: List[int],
                  rng: random.Random, config: AnnealingConfig,
                  inner_capacity: Counter) -> RunState:
    """The state a restart starts from: the --input row when there is one,
    otherwise a fresh random assignment.

    A warm start is the SAME state for every restart -- the restarts diverge
    only through their RNG streams, which is enough. Perturbing the row first
    was measured and rejected: two random swaps before each restart produced
    the single best score seen (10.1982 against the row's own 9.7601) but left
    8 of 12 restarts with no feasible border at all, because a kick can break
    the degree balance or the inner-colour inventory and a refinement
    temperature can never climb back out. Prefer more --steps to more
    --restarts on a warm run."""
    if config.start_spins is None:
        return make_random_state(pieces_by_id, corner_ids, edge_ids,
                                 rng, config, inner_capacity)
    edge_side, corner_pos = border_from_spins(pieces_by_id, config.start_spins)
    return _build_run_state(pieces_by_id, edge_side, corner_pos, inner_capacity, config)

# =============================================================================
# Incremental swap operations
# =============================================================================

def try_edge_swap(state: RunState, pieces_by_id: Dict[int, Piece],
                  pid_a: int, pid_b: int,
                  inner_capacity: Counter,
                  config: AnnealingConfig) -> Optional[EdgeSwapResult]:
    """Compute the effect of swapping pid_a and pid_b between their sides.
    Returns None if both pieces are already on the same side (no-op)."""
    side_a = state.edge_side[pid_a]
    side_b = state.edge_side[pid_b]
    if side_a == side_b:
        return None

    # Compute new arcs (pieces move to each other's side)
    new_arc_a = _arc_for_edge(pieces_by_id, pid_a, side_b)  # pid_a goes to side_b
    new_arc_b = _arc_for_edge(pieces_by_id, pid_b, side_a)  # pid_b goes to side_a

    old_arc_a = state.arcs[side_a][pid_a]
    old_arc_b = state.arcs[side_b][pid_b]

    # Updated arc dicts for the two affected sides (keyed by piece_id, no ambiguity)
    new_arcs_a = {**state.arcs[side_a], pid_b: new_arc_b}
    del new_arcs_a[pid_a]

    new_arcs_b = {**state.arcs[side_b], pid_a: new_arc_a}
    del new_arcs_b[pid_b]

    new_se_a = evaluate_side(side_a, list(new_arcs_a.values()), *state.endpoints[side_a])
    new_se_b = evaluate_side(side_b, list(new_arcs_b.values()), *state.endpoints[side_b])

    new_evals = {**state.evals, side_a: new_se_a, side_b: new_se_b}

    new_inward = Counter(state.inward_tally)
    new_inward[old_arc_a.inward_color] -= 1
    new_inward[new_arc_a.inward_color] += 1
    new_inward[old_arc_b.inward_color] -= 1
    new_inward[new_arc_b.inward_color] += 1

    # hard is carried on the result so that best-tracking can gate on true
    # feasibility, inner-colour inventory included, and not just on the four
    # SideEvaluations (which cannot see the inventory).
    hard = hard_penalty(new_evals, new_inward, inner_capacity, config)
    new_score = compute_score(new_evals, new_inward, inner_capacity, config, hard=hard)

    return EdgeSwapResult(
        new_score=new_score, hard=hard, side_a=side_a, side_b=side_b,
        new_arcs_a=new_arcs_a, new_arcs_b=new_arcs_b,
        new_se_a=new_se_a, new_se_b=new_se_b,
        new_inward=new_inward,
        new_arc_a=new_arc_a, new_arc_b=new_arc_b,
    )

def commit_edge_swap(state: RunState, pid_a: int, pid_b: int, r: EdgeSwapResult) -> None:
    state.arcs[r.side_a] = r.new_arcs_a
    state.arcs[r.side_b] = r.new_arcs_b
    state.evals[r.side_a] = r.new_se_a
    state.evals[r.side_b] = r.new_se_b
    state.inward_tally = r.new_inward
    state.edge_side[pid_a] = r.side_b
    state.edge_side[pid_b] = r.side_a
    state.score = r.new_score

def try_corner_swap(state: RunState, pieces_by_id: Dict[int, Piece],
                    pid_a: int, pid_b: int,
                    inner_capacity: Counter,
                    config: AnnealingConfig) -> CornerSwapResult:
    """Compute the effect of swapping two corner pieces (full recompute — endpoints change)."""
    pos_a, pos_b = state.corner_pos[pid_a], state.corner_pos[pid_b]

    rotated_corners = {}
    for pid, c in state.corner_pos.items():
        # Use swapped positions for pid_a and pid_b
        effective = pos_b if pid == pid_a else (pos_a if pid == pid_b else c)
        piece = pieces_by_id[pid]
        rot = find_rotation_for_corner(piece, effective)
        rotated_corners[effective] = rotate_sides(piece.sides, rot)

    new_endpoints = corner_endpoints(rotated_corners)
    new_evals = {s: evaluate_side(s, list(state.arcs[s].values()), *new_endpoints[s]) for s in Side}
    # hard is carried for the same reason EdgeSwapResult carries it: it is what
    # lets best-tracking gate on true feasibility, inner-colour inventory
    # included. A corner swap moves no edge piece, so the inventory is the
    # state's own -- but all four Euler counts change with the endpoints, which
    # is exactly why a corner swap can produce the run's best border.
    hard = hard_penalty(new_evals, state.inward_tally, inner_capacity, config)
    new_score = compute_score(new_evals, state.inward_tally, inner_capacity, config, hard=hard)

    return CornerSwapResult(new_score=new_score, hard=hard,
                            new_endpoints=new_endpoints, new_evals=new_evals)

def commit_corner_swap(state: RunState, pid_a: int, pid_b: int, r: CornerSwapResult) -> None:
    state.corner_pos[pid_a], state.corner_pos[pid_b] = state.corner_pos[pid_b], state.corner_pos[pid_a]
    state.endpoints = r.new_endpoints
    state.evals = r.new_evals
    state.score = r.new_score

# =============================================================================
# Temperature schedule
# =============================================================================

PROBE_CANDIDATES   = 3000    # ~0.2s, and yields 100-160 feasible neighbours
PROBE_MIN_SAMPLES  = 8       # fewer than this and the estimate is not worth using
POLISH_FRACTION    = 0.25    # improving neighbours below this = already near an optimum
POLISH_T0_PER_SIGMA = 0.5    # polishing T0 as a multiple of the probed move scale
POLISH_TF_RATIO     = 20.0   # polishing Tf = T0 / this

@dataclass
class MoveProbe:
    """What one pass of sampled moves says about the state it started from."""
    sigma:      Optional[float]   # spread of the score change over feasible moves
    improving:  int               # how many of them scored better
    feasible:   int               # how many landed feasible at all
    candidates: int               # how many were tried

    @property
    def improving_fraction(self) -> float:
        """Of the moves that keep the border usable, the share that improve it.
        Low means the state sits near a local optimum of the objective in
        force; high means it is a long way from what that objective wants."""
        return self.improving / self.feasible if self.feasible else 1.0

def probe_move_scale(state: RunState, pieces_by_id: Dict[int, Piece],
                     edge_ids: List[int], inner_capacity: Counter,
                     config: AnnealingConfig, rng: random.Random,
                     cands: int = PROBE_CANDIDATES,
                     need: int = PROBE_MIN_SAMPLES) -> MoveProbe:
    """Sample moves out of `state` and report what they say about it: how big a
    step the objective takes around it, and how much room there is to improve.

    `sigma` is the standard deviation of the score change over the moves that
    keep the border feasible. Three other estimators were measured and
    rejected, because a mechanism that picks defaults has to be quieter than
    the thing it measures:

      this one                        1.4-10.1% spread across probe seeds
      median |delta|, all feasible    65-137%
      median |delta|, worsening only   0-35%, and EMPTY on a real
                                       --target_scale row whose every
                                       feasible neighbour improved
      T solved for a target accept    30-224%, often no solution at all:
                                       ~92% of candidates fall off the cliff,
                                       so mean acceptance is pinned by the
                                       improving fraction, not by T

    Over 24 (objective, row) cells sigma sat at 0.19-0.32 across all three
    log-sum weightings and at 9.4-30.6 under --target_scale: a 40x spread no
    fixed constant covers, which is the reason to probe at all.

    `sigma` is None when fewer than `need` moves land feasible -- not a failure
    to paper over but the signature of a cold random start, 0 of 300 in
    measurement, where there is no feasible neighbourhood to measure."""
    deltas: List[float] = []
    improving = 0
    for _ in range(cands):
        pid_a, pid_b = rng.sample(edge_ids, 2)
        r = try_edge_swap(state, pieces_by_id, pid_a, pid_b, inner_capacity, config)
        if r is not None and r.hard == 0.0:
            d = r.new_score - state.score
            deltas.append(d)
            if d > 0.0:
                improving += 1
    return move_probe(deltas, improving, cands, need)

def move_probe(deltas: List[float], improving: int, cands: int,
               need: int = PROBE_MIN_SAMPLES) -> MoveProbe:
    """The MoveProbe for a sample of feasible score changes -- shared by the
    classic probe and the double decker's, which sample different moves."""
    if len(deltas) < need:
        return MoveProbe(None, improving, len(deltas), cands)
    mean = sum(deltas) / len(deltas)
    sigma = math.sqrt(sum((d - mean) ** 2 for d in deltas) / len(deltas))
    return MoveProbe(sigma if sigma > 0.0 else None, improving, len(deltas), cands)

def resolve_schedule(config: AnnealingConfig, state: RunState,
                     pieces_by_id: Dict[int, Piece], edge_ids: List[int],
                     inner_capacity: Counter, rng: random.Random,
                     t0_given: Optional[float],
                     tf_given: Optional[float],
                     probe: Optional[MoveProbe] = None) -> Tuple[float, float, List[str], str]:
    """(T0, Tf, lines for the header, "polish" or "search") for this run.

    Two schedules, ~50x apart, and which one is wanted does NOT follow from the
    scoring mode -- it follows from how far the starting border already sits
    from what the weights in force are asking for:

      polishing   the state is near a local optimum, so heat only destroys it.
                  Refining a row scored 9.7601 at the cold schedule left 0 of
                  12 restarts even matching their own input (mean 7.58, two
                  points BELOW the row they were handed); at T0 = 0.5*sigma,
                  12 of 12 matched or beat it, mean 9.92, best 10.06. Swept
                  over three rows: T0 = 0.5*sigma won on the mean at 9.556
                  against 9.39 for every hotter or wider schedule tried, and
                  widening the span instead of lowering T0 does not help --
                  it is the heat that matters, not the range.
      searching   the state is a long way from what these weights want, so it
                  is not a refinement target at all and wants the cold
                  schedule. The same three rows scored under --target_scale
                  250, which they were never annealed for, preferred hot by a
                  wide margin (83.9 against 60.9), and one of them sat stuck
                  at exactly 48.81 for every cool setting tried.

    The probe separates the two cleanly. Of the neighbours that keep the border
    feasible, the share that IMPROVE it was 1.4%, 2.8% and 15.7% on the three
    rows that wanted polishing, and 30.6%, 43.5% and 59.1% on the three that
    wanted searching -- a gap with nothing in it, and POLISH_FRACTION sits in
    that gap, a little above its middle: mistaking a polish for a search is the
    destructive error, mistaking a search for a polish only under-explores.

    Decided ONCE, here in the parent, so the header, every restart and the
    --out marker all report the numbers the run actually used. A double
    decker hands in its own `probe` (dd_probe), which samples its own moves;
    the rule that reads it is the same."""
    if probe is None:
        probe = probe_move_scale(state, pieces_by_id, edge_ids, inner_capacity, config, rng)
    kind = "search"
    if probe.sigma is not None and probe.improving_fraction <= POLISH_FRACTION:
        kind = "polish"
        t0 = POLISH_T0_PER_SIGMA * probe.sigma
        tf = t0 / POLISH_TF_RATIO
        why = (f"polishing: only {probe.improving}/{probe.feasible} feasible moves "
               f"improve this border, so it already sits near an optimum of these "
               f"weights; move scale sigma={probe.sigma:.4g}")
    else:
        t0, tf = config.cold_schedule()
        if probe.sigma is None:
            why = (f"searching: no feasible neighbourhood ({probe.feasible} of "
                   f"{probe.candidates} probes), so nothing local to calibrate to; "
                   f"cliff floor {config.cliff_floor:g}")
        else:
            why = (f"searching: {probe.improving}/{probe.feasible} feasible moves "
                   f"improve this border, so it is far from what these weights want; "
                   f"cliff floor {config.cliff_floor:g}")
    lines = [f"[cfg] schedule: {why}"]
    if t0_given is not None or tf_given is not None:
        t0 = t0 if t0_given is None else t0_given
        tf = tf if tf_given is None else tf_given
        lines.append("[cfg] schedule: overridden on the command line")
    lines.append(f"[cfg] schedule: T0={t0:g} Tf={tf:g}")
    return t0, tf, lines, kind

# =============================================================================
# Rotation-vector computation (for output)
# =============================================================================

def rotation_vector(pieces_by_id: Dict[int, Piece], state: RunState, n: int = 60,
                    *, edge_swap: Optional[Tuple[int, int, Side, Side]] = None,
                    corner_swap: Optional[Tuple[int, int]] = None) -> List[int]:
    """Spins for pieces 1..n, optionally as if one not-yet-committed swap had
    been applied.

    Best-tracking scores a *candidate* and has to record the vector that
    candidate would produce, which is why the swap is an override here rather
    than a commit followed by a rollback. `edge_swap` is
    (pid_a, pid_b, side_a, side_b) and moves each piece to the other's side;
    `corner_swap` is (pid_a, pid_b) and exchanges two corner seats."""
    rots: Dict[int, int] = {}
    ca, cb = corner_swap if corner_swap else (None, None)
    for pid, c in state.corner_pos.items():
        seat = c
        if corner_swap:
            seat = (state.corner_pos[cb] if pid == ca else
                    state.corner_pos[ca] if pid == cb else c)
        rots[pid] = find_rotation_for_corner(pieces_by_id[pid], seat)
    ea, eb, sa, sb = edge_swap if edge_swap else (None, None, None, None)
    for pid, side in state.edge_side.items():
        effective = side
        if edge_swap:
            effective = sb if pid == ea else (sa if pid == eb else side)
        rots[pid] = find_rotation_for_edge_side(pieces_by_id[pid], effective)
    return [rots[i] for i in range(1, n + 1)]

# =============================================================================
# Output helpers
# =============================================================================

def _counts_str(evals: Dict[Side, SideEvaluation],
                config: Optional[AnnealingConfig] = None) -> str:
    """Per-side trail counts, and in target mode each side's count as a
    multiple of its own target -- the quickest way to see which side is off
    and by how much, on a scale that is comparable between sides."""
    out = []
    for s in Side:
        se = evals[s]
        tag = f"{SIDE_NAMES[s]}={se.euler_count}/{'OK' if se.feasible else 'bad'}"
        if config is not None and config.target_scale and se.feasible:
            tag += f"/{se.euler_count / target_for(s, config):.2f}x"
        out.append(tag)
    return "  ".join(out)

def progress_str(prefix: str, score: float, evals: Dict[Side, SideEvaluation],
                 config: Optional[AnnealingConfig] = None) -> str:
    return f"{prefix}: score={score:10.4f}  {_counts_str(evals, config)}"

def restart_str(restart: int, config: AnnealingConfig,
                rec: Optional[BestRecord], elapsed: float) -> str:
    """The default mode's one line per restart: the result, not the search.
    Fixed-width fields, so a run's restarts line up and read as a table
    without needing a header row."""
    w    = len(str(config.restarts))
    head = f"  restart {restart:>{w}}/{config.restarts}"
    if rec is None:
        return f"{head}  no feasible border found  ({elapsed:.1f}s)"
    if rec.dd_counts is not None:
        # The objective's own counts; a star marks a two-tall side.
        counts = "  ".join(f"{SIDE_NAMES[s]}={rec.dd_counts[s]:>6}"
                           f"{'*' if s in config.decker_sides else ' '}" for s in Side)
    else:
        counts = "  ".join(f"{SIDE_NAMES[s]}={rec.euler_counts[s]:>6}" for s in Side)
    return (f"{head}  score={rec.score:10.4f}  {counts}"
            f"  step={rec.step:>7}  {elapsed:5.1f}s")

def best_counts_str(rec: BestRecord) -> str:
    ec = rec.euler_counts
    return (f"TOP={ec[Side.TOP]},RIGHT={ec[Side.RIGHT]},"
            f"BOTTOM={ec[Side.BOTTOM]},LEFT={ec[Side.LEFT]}")

def best_line(restart: int, rec: BestRecord) -> str:
    """A restart's best border as the labelled --verbose stdout line that
    `grep '^BEST,'` collects. Pure formatting, so the worker that found it
    puts this straight into its log."""
    return (f"BEST,Restart,{restart},Step,{rec.step},Score,{rec.score:.4f},"
            f"{best_counts_str(rec)},Rot,{','.join(map(str, rec.rot_vec))}")

def row_delta_str(plan: RowPlan, best: Optional[BestRecord], elapsed: float,
                  width: int) -> str:
    """A whole-file refinement's one line per row: the score and every side's
    trail count before and after, so what the refinement bought shows side by
    side. The rotations CSV gets no such note -- its comment keeps the one
    form every reader of it parses."""
    head = f"  row {plan.row:>{width}}  {plan.kind}"
    if best is None:
        return f"{head}  no feasible border found, nothing written  ({elapsed:.1f}s)"
    if plan.baseline is None:
        score = f"score={'infeasible':>10} -> {best.score:10.4f}"
    else:
        score = (f"score={plan.baseline:10.4f} -> {best.score:10.4f}"
                 f" ({best.score - plan.baseline:+.4f})")
    sides = "  ".join(
        f"{SIDE_NAMES[s]} {plan.start[s]}->{best.euler_counts[s]}"
        f" ({best.euler_counts[s] - plan.start[s]:+d})" for s in Side)
    return f"{head}  {score}  {sides}  {elapsed:5.1f}s"

def append_rotations(row_id: str, rec: BestRecord, out_path: str,
                     provenance: str = "", extra: str = "") -> None:
    """Append a Stage-B-readable rotations row -- the 60 border spins padded
    with zeros to the full 256-spin vector -- under a `#` comment carrying
    the per-side counts. Called only from the parent process, so the comment
    and the row it describes stay adjacent however many workers ran.

    `provenance` names the row a warm run was refined from, so a refined pool
    still says where it came from once it outlives the shell that made it --
    the same posture E555_sort_rotations.py takes with its Turn= note."""
    full = list(rec.rot_vec) + [0] * (N_SPINS - len(rec.rot_vec))
    # A double decker marks the inner pieces its witness placed with spin 1.
    # Nothing reads an inner piece's spin out of a rotations row, so the mark
    # rides through every existing reader; `extra` is its Decker=/Board= note.
    for pid in rec.flags:
        full[pid - 1] = 1
    with open(out_path, "a") as f:
        f.write(f"#  {best_counts_str(rec).replace(',', ' ')}  "
                f"{extra}Score={rec.score:.4f}{provenance}\n")
        f.write(f"{row_id}, " + ",".join(map(str, full)) + "\n")

def print_header(pieces: Sequence[Piece], corner_ids: List[int], edge_ids: List[int],
                 config: AnnealingConfig, extra: Sequence[str] = (),
                 per_row: bool = False) -> None:
    """`per_row`: a whole-file refinement, where the spins and T0/Tf belong to
    each row rather than to the run, and are reported with the rows."""
    print("\n=== E555 edge_annealer ===\n")

    if config.verbose:
        for k, v in asdict(config).items():
            if not config.decker_sides and (k.startswith("decker_")
                                            or k == "block_penalty_weight"):
                continue                    # a classic run's dump stays as it was
            if per_row and k in ("T0", "Tf", "start_spins"):
                v = "<per row>"
            elif k == "start_spins" and v is not None:
                v = f"<{len(v)} spins from --input>"
            print(f"[cfg] {k} = {v}")
        if config.target_scale:
            targets = "  ".join(f"{SIDE_NAMES[s]}={target_for(s, config):.0f}" for s in Side)
            print(f"[cfg] targets  {targets}   ({TARGET_DECADE_PENALTY:.0f} points per decade off)")
        for line in extra:
            print(line)
        print(f"[init] pieces={len(pieces)}  corners={corner_ids}  edges={len(edge_ids)}")
        return

    temps = "T0/Tf per row" if per_row else f"T0={config.T0:g} Tf={config.Tf:g}"
    print(f"[cfg] seed={config.random_seed}  "
          f"restarts={config.restarts} x {config.steps_per_restart} steps  "
          f"threads={config.threads}  {temps}  "
          f"tabu={config.tabu_length}  fix_corners={config.fix_corners}")
    if config.target_scale:
        targets = " ".join(f"{SIDE_NAMES[s]}={target_for(s, config):.0f}" for s in Side)
        print(f"[cfg] objective=target_scale({config.target_scale})  targets {targets}"
              f"  ({TARGET_DECADE_PENALTY:.0f} points per decade off)")
    else:
        weights = " ".join(f"{SIDE_NAMES[s]}={config.w_side(s):g}" for s in Side)
        print(f"[cfg] objective=log-sum  weights {weights}")
    for line in extra:
        print(line)
    print(f"[init] {len(pieces)} pieces: {len(corner_ids)} corners, "
          f"{len(edge_ids)} edges, {len(pieces) - len(corner_ids) - len(edge_ids)} inner")

# =============================================================================
# Double decker -- two-tall border segments (--double_decker)
# =============================================================================
#
# A classic side is scored by the orderings of its 14 edge pieces. A double-
# decker side is scored by its TWO rows: every order of its edges, times every
# way to put an inner piece under each so the inner row chains too, counted
# over a RESERVE of --decker_reserve inner pieces the search picks for that
# side, each used at most once (ReserveCounter). A strip's interfaces are
# (frame colour, inner colour) pairs; decker_node packs one into an int (frame
# colours are 1..5 and inner colours 6..22, so frame<<5|inner is unique).
#
# Why a reserve and not a fixed pairing: pairing each edge with one inner
# piece and counting the reorderings gave 6-36 on real borders, and re-pairing
# those same 12 pieces gave exactly the same -- each fits only under its own
# edge, so a fixed pairing is a rigid block. A searched reserve of 20/24/30
# pieces reached ~208/256/1,094 layouts on one top strip, and the whole free
# pool bounds it at ~1e8 (pool_bound, the DeckerPool= figure).
#
# The four corners of the second ring overlap: (14,1) touches both the top edge
# at (15,1) and the left edge at (14,0). So every corner next to a double-decker
# side becomes an explicit 2x2 BLOCK -- the corner piece, the two edge pieces
# beside it (a on the top/bottom side, b on the left/right side) and the inner
# piece q diagonal to it. A strip runs between the blocks at its two ends; a
# classic side next to a blocked corner keeps that block's edge fixed at its
# end and counts the orderings of the rest.

# The five Eternity II clue pieces (1-based ids; g_clue in E555_database.c).
# Their cells are fixed and none is on the second ring, so a witness that used
# one could never stand in a clue run -- they are kept out of the pool.
CLUE_PIECES = frozenset({139, 181, 208, 249, 255})

# Faces are indexed like Side. Along a side the trail runs as edge_arc runs
# it: top and bottom left to right, left and right top to bottom. DD_SRC and
# DD_DST are the faces an arc leaves and enters by, DD_INWARD the face an edge
# shows the board. A partner touches its edge with its own face `s` (a top
# edge's partner with its TOP face, and so on), so a partner attached by seed
# face f sits at rotation (f - s) mod 4, and shows the centre DD_INWARD[s].
DD_SRC    = {Side.TOP: Side.LEFT,  Side.BOTTOM: Side.LEFT,
             Side.LEFT: Side.TOP,  Side.RIGHT: Side.TOP}
DD_DST    = {Side.TOP: Side.RIGHT, Side.BOTTOM: Side.RIGHT,
             Side.LEFT: Side.BOTTOM, Side.RIGHT: Side.BOTTOM}
DD_INWARD = {Side.TOP: Side.BOTTOM, Side.BOTTOM: Side.TOP,
             Side.LEFT: Side.RIGHT, Side.RIGHT: Side.LEFT}
# The corners a side's trail starts and ends at.
DD_ENDS   = {Side.TOP: (Corner.TL, Corner.TR), Side.BOTTOM: (Corner.BL, Corner.BR),
             Side.LEFT: (Corner.TL, Corner.BL), Side.RIGHT: (Corner.TR, Corner.BR)}
# A side's 14 frame cells, (row, col) bottom-up, in trail order.
DD_CELLS  = {Side.TOP:    [(15, c) for c in range(1, 15)],
             Side.BOTTOM: [(0, c) for c in range(1, 15)],
             Side.LEFT:   [(r, 0) for r in range(14, 0, -1)],
             Side.RIGHT:  [(r, 15) for r in range(14, 0, -1)]}
DD_STEP   = {Side.TOP: (1, 0), Side.RIGHT: (0, 1), Side.BOTTOM: (-1, 0), Side.LEFT: (0, -1)}

def _opp(d: int) -> Side:
    return Side((d + 2) % 4)

def decker_node(frame: int, inner: int) -> int:
    return (frame << 5) | inner

@dataclass(frozen=True)
class CornerGeom:
    """One corner's 2x2 block. a_dir runs from the corner along a_side (and
    from b to q), b_dir along b_side (and from a to q). The block holds when

        C[a_dir] = A[opp a_dir]   C[b_dir] = B[opp b_dir]
        Q[opp b_dir] = A[b_dir]   Q[opp a_dir] = B[a_dir]

    and a_side's trail meets it at (A[a_dir], Q[a_dir]), b_side's at
    (B[b_dir], Q[b_dir]) -- the frame colour alone on a classic side."""
    cell:   Tuple[int, int]
    a_side: Side
    b_side: Side
    a_dir:  Side
    b_dir:  Side

    def at(self, *dirs: Side) -> Tuple[int, int]:
        r, c = self.cell
        for d in dirs:
            r, c = r + DD_STEP[d][0], c + DD_STEP[d][1]
        return r, c

DD_CORNERS = {
    Corner.TL: CornerGeom((15, 0),  Side.TOP,    Side.LEFT,  Side.RIGHT, Side.BOTTOM),
    Corner.TR: CornerGeom((15, 15), Side.TOP,    Side.RIGHT, Side.LEFT,  Side.BOTTOM),
    Corner.BR: CornerGeom((0, 15),  Side.BOTTOM, Side.RIGHT, Side.LEFT,  Side.TOP),
    Corner.BL: CornerGeom((0, 0),   Side.BOTTOM, Side.LEFT,  Side.RIGHT, Side.TOP),
}

# Move weights, renormalized over the moves a run can make (see dd_propose).
# Measured on 6 rows of data/annealer_MaxSides.csv x 2 restarts x 6000 steps
# x 2 seeds, mean log reserve count per two-tall side (TOP only / all four):
#   reserve .70 block .10 swap .10 exchange .10   5.44 / 2.41   <- this
#   reserve .50 block .15 swap .25 exchange .10   4.97 / 2.11
#   reserve .35 block .20 swap .35 exchange .10   5.02 / 1.95
DD_MIX = {"reserve": 0.70, "block": 0.10, "swap": 0.10, "exchange": 0.10}

class DeckerContext:
    """Everything a double-decker search reads and never changes: the piece
    rotations, which sides are two tall, which corners are blocked, and the
    indices the seeding and the moves draw pieces from. Built per restart in
    the worker -- a few milliseconds."""

    def __init__(self, pieces_by_id: Dict[int, Piece], corner_ids: List[int],
                 edge_ids: List[int], sides: Sequence[int], keep_border: bool = False):
        self.pieces_by_id = pieces_by_id
        self.edge_ids   = list(edge_ids)
        self.corner_ids = list(corner_ids)
        self.dd = {s: (int(s) in set(int(x) for x in sides)) for s in Side}
        self.dd_sides = [s for s in Side if self.dd[s]]
        self.blocked = {k: self.dd[g.a_side] or self.dd[g.b_side]
                        for k, g in DD_CORNERS.items()}
        self.blocked_corners = [k for k in Corner if self.blocked[k]]
        self.keep_border = keep_border
        self.rot = {pid: tuple(rotate_sides(p.sides, r) for r in range(4))
                    for pid, p in pieces_by_id.items()}
        self.edge_spin = {pid: {s: find_rotation_for_edge_side(pieces_by_id[pid], s)
                                for s in Side} for pid in edge_ids}
        self.edge_rot = {pid: {s: self.rot[pid][self.edge_spin[pid][s]] for s in Side}
                         for pid in edge_ids}
        self.inward = {pid: self.edge_rot[pid][Side.TOP][Side.BOTTOM] for pid in edge_ids}
        # An edge's two frame colours, read the same way on every side: two
        # edges with the same pair can trade sides without changing either
        # side's frame, so both strips keep a frame order.
        pair = {pid: (self.edge_rot[pid][Side.TOP][Side.LEFT],
                      self.edge_rot[pid][Side.TOP][Side.RIGHT]) for pid in edge_ids}
        self.twins = {pid: [q for q in edge_ids if q != pid and pair[q] == pair[pid]]
                      for pid in edge_ids}
        self.corner_spin = {(pid, k): find_rotation_for_corner(pieces_by_id[pid], k)
                            for pid in corner_ids for k in Corner}
        self.corner_rot = {key: self.rot[key[0]][spin] for key, spin in self.corner_spin.items()}
        inner = sorted(pid for pid, p in pieces_by_id.items() if p.zero_count == 0)
        self.pool = [pid for pid in inner if pid not in CLUE_PIECES]
        # side -> piece -> its four placements under an edge of that side, as
        # (inward colour it meets, colour on the arc's source face, colour on
        # its target face): what reserve_count and pool_bound walk over
        self.opts: Dict[Side, Dict[int, Tuple[Tuple[int, int, int], ...]]] = {}
        for s in Side:
            self.opts[s] = {}
            for pid in self.pool:
                o = []
                for f in range(4):
                    P = self.rot[pid][(f - s) % 4]
                    o.append((P[s], P[DD_SRC[s]], P[DD_DST[s]]))
                self.opts[s][pid] = tuple(o)
        # colour -> pool pieces showing it on some face (each once)
        self.colour_pieces: Dict[int, List[int]] = defaultdict(list)
        # side -> (inward colour, colour on the arc's source face)
        #      -> (piece, face, colour on the arc's target face)
        self.strip_index: Dict[Side, Dict[Tuple[int, int], List[Tuple[int, int, int]]]] = \
            {s: defaultdict(list) for s in Side}
        # side -> (inward, source, target colour) -> (piece, face): the last
        # cell of a strip, which has to close onto the far block exactly
        self.strip_end: Dict[Side, Dict[Tuple[int, int, int], List[Tuple[int, int]]]] = \
            {s: defaultdict(list) for s in Side}
        # corner -> (Q[opp b_dir], Q[opp a_dir]) -> (piece, rotation)
        self.q_index: Dict[Corner, Dict[Tuple[int, int], List[Tuple[int, int]]]] = \
            {k: defaultdict(list) for k in Corner}
        for pid in self.pool:
            sides = pieces_by_id[pid].sides
            for c in sorted(set(sides)):
                self.colour_pieces[c].append(pid)
            for f in range(4):
                for s in Side:
                    P = self.rot[pid][(f - s) % 4]
                    self.strip_index[s][(P[s], P[DD_SRC[s]])].append((pid, f, P[DD_DST[s]]))
                    self.strip_end[s][(P[s], P[DD_SRC[s]], P[DD_DST[s]])].append((pid, f))
            for k, g in DD_CORNERS.items():
                for r in range(4):
                    Q = self.rot[pid][r]
                    self.q_index[k][(Q[_opp(g.b_dir)], Q[_opp(g.a_dir)])].append((pid, r))
        # The move mix (see dd_propose). --decker_keep_border drops the swaps
        # that move edges between sides.
        mix = [("reserve", DD_MIX["reserve"]), ("block", DD_MIX["block"])]
        if not keep_border:
            mix.append(("swap", DD_MIX["swap"]))
        if len(self.dd_sides) > 1:
            mix.append(("exchange", DD_MIX["exchange"]))
        total = sum(w for _, w in mix)
        acc, self.moves = 0.0, []
        for name, w in mix:
            acc += w / total
            self.moves.append((name, acc))

# The exact reserve count walks (placed-edge mask, used-piece mask, node)
# states. Measured on real strips at K=24 it visits ~1,000 of them (~1 ms) and
# at K=30 ~7,500 (5-9 ms); a count that would need more than this many is cut
# short and scores 0 for that candidate -- a guard, not a limit reached in
# measurement.
DD_MEMO_CAP = 400_000

class _MemoFull(Exception):
    pass

class ReserveCounter:
    """The layouts of one two-tall strip from one reserve set: every order of
    the middle edges that the frame colours allow, times every way to put a
    distinct reserved piece, in any rotation, under each edge so the inner row
    chains from the start block's q to the end block's q. Exact. Rotations of
    one piece that happen to look alike count as different placements.

    A memoized walk over (placed-edge mask, used-piece mask, interface node);
    the memo is kept, so `sample` draws a uniformly random layout from the
    counted ones at no extra cost."""

    def __init__(self, ctx: "DeckerContext", s: Side, middle: Sequence[int],
                 start: int, end: int, reserve: Sequence[int]):
        self.s, self.start, self.end = s, start, end
        src, dst = DD_SRC[s], DD_DST[s]
        self.edges = [(e, ctx.edge_rot[e][s][src], ctx.edge_rot[e][s][dst], ctx.inward[e])
                      for e in middle]
        self.pieces = list(reserve)
        by: Dict[Tuple[int, int], List[Tuple[int, int, int]]] = defaultdict(list)
        for j, p in enumerate(self.pieces):
            for f, (iw, si, di) in enumerate(ctx.opts[s][p]):
                by[(iw, si)].append((di, j, f))
        self.by = by
        self.full = (1 << len(self.edges)) - 1
        self.memo: Dict[Tuple[int, int, int], int] = {}

    def _h(self, m: int, pm: int, node: int) -> int:
        key = (m, pm, node)
        got = self.memo.get(key)
        if got is not None:
            return got
        if m == self.full:
            return 1 if node == self.end else 0
        if len(self.memo) >= DD_MEMO_CAP:
            raise _MemoFull
        fn, inn = node >> 5, node & 31
        total = 0
        for i, (_, ef, et, iw) in enumerate(self.edges):
            if m >> i & 1 or ef != fn:
                continue
            for di, j, _ in self.by.get((iw, inn), ()):
                if pm >> j & 1:
                    continue
                total += self._h(m | 1 << i, pm | 1 << j, (et << 5) | di)
        self.memo[key] = total
        return total

    def count(self) -> int:
        try:
            return self._h(0, 0, self.start)
        except _MemoFull:
            return 0

    def sample(self, rng: random.Random) -> List[Tuple[int, int, int]]:
        """One layout, uniformly at random among the counted ones, as
        (edge, piece, seed face touching the edge) in strip order."""
        out: List[Tuple[int, int, int]] = []
        m, pm, node = 0, 0, self.start
        while m != self.full:
            fn, inn = node >> 5, node & 31
            moves = []
            for i, (e, ef, et, iw) in enumerate(self.edges):
                if m >> i & 1 or ef != fn:
                    continue
                for di, j, f in self.by.get((iw, inn), ()):
                    if pm >> j & 1:
                        continue
                    nxt = (m | 1 << i, pm | 1 << j, (et << 5) | di)
                    w = self._h(*nxt)
                    if w:
                        moves.append((w, e, j, f, nxt))
            pick = rng.randrange(sum(w for w, *_ in moves))
            for w, e, j, f, nxt in moves:
                if pick < w:
                    out.append((e, self.pieces[j], f))
                    m, pm, node = nxt
                    break
                pick -= w
        return out

def pool_bound(ctx: "DeckerContext", s: Side, middle: Sequence[int], start: int,
               end: int, exclude: set) -> int:
    """The same layouts with every pool piece outside `exclude` available and
    no once-only rule: an upper bound on what the border allows, the
    DeckerPool= figure. A walk over (placed-edge mask, node), each step
    weighted by how many (piece, face) fit it -- ~40 ms a side."""
    src, dst = DD_SRC[s], DD_DST[s]
    edges = [(ctx.edge_rot[e][s][src], ctx.edge_rot[e][s][dst], ctx.inward[e]) for e in middle]
    mult: Dict[Tuple[int, int], Counter] = defaultdict(Counter)
    for p in ctx.pool:
        if p in exclude:
            continue
        for iw, si, di in ctx.opts[s][p]:
            mult[(iw, si)][di] += 1
    full = (1 << len(edges)) - 1
    memo: Dict[Tuple[int, int], int] = {}

    def h(m: int, node: int) -> int:
        key = (m, node)
        if key in memo:
            return memo[key]
        if m == full:
            return 1 if node == end else 0
        fn, inn = node >> 5, node & 31
        total = 0
        for i, (ef, et, iw) in enumerate(edges):
            if m >> i & 1 or ef != fn:
                continue
            for di, k in mult.get((iw, inn), {}).items():
                total += k * h(m | 1 << i, (et << 5) | di)
        memo[key] = total
        return total

    return h(0, start)

@dataclass
class DeckerState:
    """A border, its corner blocks, and each two-tall side's RESERVE: the K
    inner pieces its two rows are counted from. States are cloned before a
    move and never changed after they are evaluated, so a best can be kept by
    reference."""
    corner_pos: Dict[int, Corner]                     # shared, never changed
    seat:       Dict[Corner, int]                     # shared, never changed
    edge_side:  Dict[int, Side]
    block:      Dict[Corner, Tuple[int, int, int, int]]
    reserve:    Dict[Side, Tuple[int, ...]]
    used:       set                                   # reserve pieces and q's
    evals:      Dict[Side, SideEvaluation] = field(default_factory=dict)
    block_bad:  Dict[Corner, int] = field(default_factory=dict)
    hard:       float = 0.0
    score:      float = 0.0

    def clone(self) -> "DeckerState":
        return DeckerState(self.corner_pos, self.seat, dict(self.edge_side),
                           dict(self.block), dict(self.reserve), set(self.used),
                           dict(self.evals), dict(self.block_bad), self.hard, self.score)

def dd_block_edges(st: DeckerState) -> set:
    return {e for a, b, _, _ in st.block.values() for e in (a, b)}

def dd_endpoint(ctx: DeckerContext, st: DeckerState, s: Side, k: Corner) -> int:
    """Where side s's trail meets corner k: a block's edge (and q, on a
    two-tall side), or the plain corner piece exactly as corner_endpoints
    reads it."""
    g = DD_CORNERS[k]
    d = g.a_dir if g.a_side == s else g.b_dir
    if not ctx.blocked[k]:
        return ctx.corner_rot[(st.seat[k], k)][d]
    a, b, q, qr = st.block[k]
    e = a if g.a_side == s else b
    frame = ctx.edge_rot[e][s][d]
    return decker_node(frame, ctx.rot[q][qr][d]) if ctx.dd[s] else frame

def dd_middle(ctx: DeckerContext, st: DeckerState, s: Side) -> List[int]:
    """Side s's edges that no corner block holds, in edge_side order."""
    fixed = {st.block[k][0 if DD_CORNERS[k].a_side == s else 1]
             for k in DD_ENDS[s] if ctx.blocked[k]}
    return [pid for pid, side in st.edge_side.items() if side == s and pid not in fixed]

def dd_frame_arcs(ctx: DeckerContext, s: Side, middle: Sequence[int]
                  ) -> List[Tuple[int, int, int]]:
    src, dst = DD_SRC[s], DD_DST[s]
    return [(ctx.edge_rot[e][s][src], ctx.edge_rot[e][s][dst], e) for e in middle]

def dd_counter(ctx: DeckerContext, st: DeckerState, s: Side,
               middle: Optional[Sequence[int]] = None,
               reserve: Optional[Sequence[int]] = None) -> ReserveCounter:
    k0, k1 = DD_ENDS[s]
    return ReserveCounter(ctx, s, dd_middle(ctx, st, s) if middle is None else middle,
                          dd_endpoint(ctx, st, s, k0), dd_endpoint(ctx, st, s, k1),
                          st.reserve[s] if reserve is None else reserve)

def dd_eval_side(ctx: DeckerContext, st: DeckerState, s: Side) -> SideEvaluation:
    """A classic side's fixed-end trail count; a two-tall side's frame first
    -- an unbalanced one keeps its balance penalty as the gradient and is not
    counted -- then, on a frame that has an order, the exact number of layouts
    of its reserve (0 is its own penalty, like a frame with no trail)."""
    middle = dd_middle(ctx, st, s)
    k0, k1 = DD_ENDS[s]
    start, end = dd_endpoint(ctx, st, s, k0), dd_endpoint(ctx, st, s, k1)
    arcs = dd_frame_arcs(ctx, s, middle)
    if not ctx.dd[s]:
        return evaluate_graph(s, [(u, v) for u, v, _ in arcs], start, end)
    frame = evaluate_graph(s, [(u, v) for u, v, _ in arcs], start >> 5, end >> 5)
    if frame.euler_count == 0:
        return frame
    n = ReserveCounter(ctx, s, middle, start, end, st.reserve[s]).count()
    return replace(frame, euler_count=n)

def dd_block_bad(ctx: DeckerContext, st: DeckerState, k: Corner) -> int:
    """Broken matches inside corner k's block."""
    g = DD_CORNERS[k]
    a, b, q, qr = st.block[k]
    C = ctx.corner_rot[(st.seat[k], k)]
    A = ctx.edge_rot[a][g.a_side]
    B = ctx.edge_rot[b][g.b_side]
    Q = ctx.rot[q][qr]
    return ((C[g.a_dir] != A[_opp(g.a_dir)]) + (C[g.b_dir] != B[_opp(g.b_dir)])
            + (Q[_opp(g.b_dir)] != A[g.b_dir]) + (Q[_opp(g.a_dir)] != B[g.a_dir]))

def dd_refresh(ctx: DeckerContext, st: DeckerState, sides: Sequence[Side],
               corners: Sequence[Corner], config: AnnealingConfig) -> None:
    """Re-evaluate the named sides and corners, then the state's totals. The
    inner-colour inventory is left out: every edge faces the board on any
    border, so for a border it is one fixed check, not a search signal."""
    for s in sides:
        st.evals[s] = dd_eval_side(ctx, st, s)
    for k in corners:
        st.block_bad[k] = dd_block_bad(ctx, st, k)
    hard = sum(side_penalty(st.evals[s], config) for s in Side)
    hard += config.block_penalty_weight * sum(st.block_bad.values())
    st.hard = hard
    st.score = compute_score(st.evals, Counter(), Counter(), config, hard=hard)

def dd_counts(st: DeckerState) -> Dict[Side, int]:
    return {s: st.evals[s].euler_count for s in Side}

def euler_trail(arcs: Sequence[Tuple[int, int, int]], start: int, end: int,
                rng: random.Random) -> Optional[List[int]]:
    """One Euler trail from start to end over labelled arcs, as its labels in
    order (Hierholzer, with the choices shuffled so repeated calls sample
    different trails). None when the arcs admit no such trail."""
    adj: Dict[int, List[Tuple[int, int]]] = defaultdict(list)
    for u, v, lab in arcs:
        adj[u].append((v, lab))
    for u in list(adj):
        rng.shuffle(adj[u])
    stack: List[Tuple[int, Optional[int]]] = [(start, None)]
    path: List[int] = []
    while stack:
        u, lab = stack[-1]
        if adj[u]:
            v, nxt = adj[u].pop()
            stack.append((v, nxt))
        else:
            stack.pop()
            if lab is not None:
                path.append(lab)
    path.reverse()
    if len(path) != len(arcs):
        return None
    # Hierholzer from `start` ends wherever the degrees force it; check that is
    # `end`, and that the walk is a walk, so a bad graph cannot pass as a trail.
    where = {lab: (u, v) for u, v, lab in arcs}
    node = start
    for lab in path:
        u, v = where[lab]
        if u != node:
            return None
        node = v
    return path if node == end else None

def dd_fill_strip(ctx: DeckerContext, used: set, s: Side, middle: List[int],
                  start_inner: int, end_inner: int, rng: random.Random,
                  budget: int = 4000) -> Optional[List[Tuple[int, int, int]]]:
    """One inner row for a two-tall strip along the frame order `middle`,
    chaining from start_inner to end_inner, as (edge, piece, face): a bounded
    depth-first search over pieces not in `used`. About 2.7 pieces fit each
    cell, so a strip of 12 almost always fills at once. The seeding's start."""
    chosen: List[Tuple[int, int, int]] = []
    taken: set = set()
    spent = [0]

    def dfs(i: int, prev: int) -> bool:
        spent[0] += 1
        if spent[0] > budget:
            return False
        e = middle[i]
        if i == len(middle) - 1:
            # The last cell must close onto the far block exactly: look it up
            # rather than trying every piece that merely fits on the left.
            opts = ctx.strip_end[s].get((ctx.inward[e], prev, end_inner), ())
            n = len(opts)
            o = rng.randrange(n) if n else 0
            for j in range(n):
                p, f = opts[(o + j) % n]
                if p not in used and p not in taken:
                    chosen.append((e, p, f))
                    return True
            return False
        opts = ctx.strip_index[s].get((ctx.inward[e], prev), ())
        n = len(opts)
        o = rng.randrange(n) if n else 0
        for j in range(n):
            p, f, nxt = opts[(o + j) % n]
            if p in used or p in taken:
                continue
            taken.add(p)
            chosen.append((e, p, f))
            if dfs(i + 1, nxt):
                return True
            taken.discard(p)
            chosen.pop()
        return False

    if not middle:
        return [] if start_inner == end_inner else None
    return chosen if dfs(0, start_inner) else None

def dd_useful(ctx: DeckerContext, st: DeckerState, s: Side) -> List[int]:
    """Pool pieces that could sit under some edge of side s at all: those
    showing one of its edges' inward colours. Where reserve draws come from."""
    out: set = set()
    for e in dd_middle(ctx, st, s):
        out.update(ctx.colour_pieces.get(ctx.inward[e], ()))
    return sorted(out)

def dd_targets(ctx: DeckerContext, st: DeckerState, s: Side,
               res: Sequence[int]) -> List[int]:
    """Unused pool pieces that could join a layout of side s's reserve `res`:
    some placement of theirs meets an edge of the side (its inward colour) AND
    shows, on both its source and target faces, inner colours the reserve
    already shows on those faces somewhere. A piece that fits an edge but
    chains to nothing already reserved can only ever add layouts together
    with others; these can add them on their own."""
    inwards = {ctx.inward[e] for e in dd_middle(ctx, st, s)}
    srcs, dsts = set(), set()
    for p in res:
        for iw, si, di in ctx.opts[s][p]:
            if iw in inwards:
                srcs.add(si)
                dsts.add(di)
    # A new piece's source faces a colour some reserved piece leaves on its
    # target face, and vice versa -- or the block colour at either end.
    for k in DD_ENDS[s]:
        node = dd_endpoint(ctx, st, s, k)
        dsts.add(node & 31)
        srcs.add(node & 31)
    out = []
    taken = set(res)
    for p in dd_useful(ctx, st, s):
        if p in st.used or p in taken:
            continue
        if any(iw in inwards and si in dsts and di in srcs
               for iw, si, di in ctx.opts[s][p]):
            out.append(p)
    return out

DD_SEED_TRIES = 64
DD_GROW_SAMPLE = 16      # candidates tried per piece while growing a reserve
# Where a `reserve` move draws its new piece: dd_targets, else any useful
# piece of the side, else anywhere in the pool. On the same bench (mean log
# count, TOP only / all four): targets never 4.62 / 1.57, 60% 4.69 / 1.93,
# 85% 4.97 / 2.11.
DD_DRAW_TARGETS = 0.85
DD_DRAW_USEFUL  = 0.10

def dd_grow(ctx: DeckerContext, st: DeckerState, s: Side, base: List[int],
            k: int, rng: random.Random) -> Tuple[int, ...]:
    """Grow a reserve from `base` to k pieces, one at a time, each the best of
    DD_GROW_SAMPLE unused candidates -- dd_targets when there are any -- by
    the exact count it leads to (ties to the first drawn, so a zero count
    still grows at random)."""
    res = list(base)
    useful = dd_useful(ctx, st, s)
    middle = dd_middle(ctx, st, s)
    while len(res) < k:
        free = dd_targets(ctx, st, s, res)
        if not free:
            free = [p for p in useful if p not in st.used and p not in res]
        if not free:
            free = [p for p in ctx.pool if p not in st.used and p not in res]
        cands = [free[rng.randrange(len(free))] for _ in range(min(DD_GROW_SAMPLE, len(free)))]
        best_p, best_n = cands[0], -1
        for p in cands:
            n = dd_counter(ctx, st, s, middle, res + [p]).count()
            if n > best_n:
                best_p, best_n = p, n
        res.append(best_p)
    return tuple(res)

def dd_seed(ctx: DeckerContext, edge_side: Dict[int, Side],
            corner_pos: Dict[int, Corner], rng: random.Random,
            config: AnnealingConfig) -> Optional[DeckerState]:
    """A double-decker state for a border, built to count from the start.

    Each side gets a random classic trail; its end arcs become the block edges,
    which therefore meet their corner's frame colours by construction, and q
    comes from the index of pieces whose two faces fit them. Each two-tall
    side's reserve starts from one inner row that fits its trail (so it counts
    at least 1) and grows to --decker_reserve by dd_grow. When no attempt
    succeeds -- the border itself not being a usable one, most often -- blocks
    and reserves are drawn at random and the search has to repair them."""
    seat = {k: pid for pid, k in corner_pos.items()}
    K = config.decker_reserve
    plain = {}
    for s in Side:
        arcs = [(ctx.edge_rot[pid][s][DD_SRC[s]], ctx.edge_rot[pid][s][DD_DST[s]], pid)
                for pid, side in edge_side.items() if side == s]
        ends = tuple(ctx.corner_rot[(seat[k], k)][DD_CORNERS[k].a_dir
                                                   if DD_CORNERS[k].a_side == s
                                                   else DD_CORNERS[k].b_dir]
                     for k in DD_ENDS[s])
        plain[s] = (arcs, ends)

    def blank() -> DeckerState:
        return DeckerState(corner_pos=dict(corner_pos), seat=seat,
                           edge_side=dict(edge_side), block={}, reserve={}, used=set())

    trails: Dict[Side, List[int]] = {}
    for _ in range(DD_SEED_TRIES):
        trails = {}
        for s in Side:
            t = euler_trail(plain[s][0], plain[s][1][0], plain[s][1][1], rng)
            if t is None:
                break
            trails[s] = t
        if len(trails) < len(Side):
            break                                   # not a usable border: repair
        st = blank()
        ok = True
        for k in ctx.blocked_corners:
            g = DD_CORNERS[k]
            a = trails[g.a_side][0 if DD_ENDS[g.a_side][0] == k else -1]
            b = trails[g.b_side][0 if DD_ENDS[g.b_side][0] == k else -1]
            A, B = ctx.edge_rot[a][g.a_side], ctx.edge_rot[b][g.b_side]
            opts = [o for o in ctx.q_index[k].get((A[g.b_dir], B[g.a_dir]), ())
                    if o[0] not in st.used]
            if not opts:
                ok = False
                break
            q, qr = opts[rng.randrange(len(opts))]
            st.used.add(q)
            st.block[k] = (a, b, q, qr)
        rows: Dict[Side, List[int]] = {}
        for s in ctx.dd_sides:
            if not ok:
                break
            ends = []
            for k in DD_ENDS[s]:
                g = DD_CORNERS[k]
                _, _, q, qr = st.block[k]
                ends.append(ctx.rot[q][qr][g.a_dir if g.a_side == s else g.b_dir])
            row = dd_fill_strip(ctx, st.used, s, trails[s][1:-1], ends[0], ends[1], rng)
            if row is None:
                ok = False
                break
            rows[s] = [p for _, p, _ in row]
            st.used.update(rows[s])
        if not ok:
            continue
        for s in ctx.dd_sides:
            st.reserve[s] = dd_grow(ctx, st, s, rows[s], K, rng)
            st.used.update(st.reserve[s])
        dd_refresh(ctx, st, list(Side), ctx.blocked_corners, config)
        return st

    # Repair start: blocks from the trail ends where there are trails, else any
    # edge of the side; q and the reserves at random among fitting pieces.
    st = blank()
    taken: set = set()
    for k in ctx.blocked_corners:
        g = DD_CORNERS[k]
        pick = []
        for side in (g.a_side, g.b_side):
            e = trails[side][0 if DD_ENDS[side][0] == k else -1] if side in trails else None
            if e is None or e in taken:
                cands = sorted(pid for pid, sd in edge_side.items()
                               if sd == side and pid not in taken)
                e = cands[rng.randrange(len(cands))]
            taken.add(e)
            pick.append(e)
        a, b = pick
        A, B = ctx.edge_rot[a][g.a_side], ctx.edge_rot[b][g.b_side]
        opts = [o for o in ctx.q_index[k].get((A[g.b_dir], B[g.a_dir]), ())
                if o[0] not in st.used]
        if opts:
            q, qr = opts[rng.randrange(len(opts))]
        else:
            free = [p for p in ctx.pool if p not in st.used]
            q, qr = free[rng.randrange(len(free))], rng.randrange(4)
        st.used.add(q)
        st.block[k] = (a, b, q, qr)
    for s in ctx.dd_sides:
        free = [p for p in dd_useful(ctx, st, s) if p not in st.used]
        if len(free) < K:
            free = [p for p in ctx.pool if p not in st.used]
        if len(free) < K:
            return None
        st.reserve[s] = tuple(rng.sample(free, K))
        st.used.update(st.reserve[s])
    dd_refresh(ctx, st, list(Side), ctx.blocked_corners, config)
    return st

def dd_propose(ctx: DeckerContext, st: DeckerState, rng: random.Random,
               config: AnnealingConfig) -> Optional[Tuple[DeckerState, Optional[frozenset]]]:
    """One candidate move, evaluated, as (candidate, tabu pair or None); None
    when the drawn move cannot be made here. The state itself is untouched.

      reserve   one piece of a side's reserve is swapped for an unused one --
                mostly a USEFUL one, showing an inward colour of that side
      exchange  two two-tall sides trade one reserve piece each
      block     a corner block takes a new frame-matching edge and/or a new q
      swap      two edges trade sides (twins, with the same frame pair, half
                the time); the reserves stay with their sides

    --decker_keep_border never draws `swap`, and keeps a new block edge on the
    block's own side, so the spins cannot change."""
    x = rng.random()
    kind = next(name for name, acc in ctx.moves if x < acc or acc >= 1.0 - 1e-12)
    cand = st.clone()
    touched: set = set()
    corners: Tuple[Corner, ...] = ()
    pair = None

    if kind == "reserve":
        s = ctx.dd_sides[rng.randrange(len(ctx.dd_sides))]
        res = list(cand.reserve[s])
        x = rng.random()
        pool = (dd_targets(ctx, cand, s, res) if x < DD_DRAW_TARGETS else
                dd_useful(ctx, cand, s) if x < DD_DRAW_TARGETS + DD_DRAW_USEFUL
                else ctx.pool)
        p = pool[rng.randrange(len(pool))] if pool else None
        if p is None or p in cand.used:
            return None
        i = rng.randrange(len(res))
        cand.used.discard(res[i])
        cand.used.add(p)
        res[i] = p
        cand.reserve[s] = tuple(res)
        touched = {s}

    elif kind == "exchange":
        s1, s2 = rng.sample(ctx.dd_sides, 2)
        r1, r2 = list(cand.reserve[s1]), list(cand.reserve[s2])
        i, j = rng.randrange(len(r1)), rng.randrange(len(r2))
        r1[i], r2[j] = r2[j], r1[i]
        cand.reserve[s1], cand.reserve[s2] = tuple(r1), tuple(r2)
        touched = {s1, s2}

    elif kind == "swap":
        blocked = dd_block_edges(cand)
        free = [e for e in ctx.edge_ids if e not in blocked]
        e1 = free[rng.randrange(len(free))]
        twins = [t for t in ctx.twins[e1]
                 if t not in blocked and cand.edge_side[t] != cand.edge_side[e1]]
        if twins and rng.random() < 0.5:
            e2 = twins[rng.randrange(len(twins))]
        else:
            e2 = free[rng.randrange(len(free))]
            if e2 == e1:
                return None
        s1, s2 = cand.edge_side[e1], cand.edge_side[e2]
        if s1 == s2:
            return None
        cand.edge_side[e1], cand.edge_side[e2] = s2, s1
        # A two-tall side has just taken an edge whose inward colour its
        # reserve may not cover: swap in a piece that meets it, in place of a
        # reserved piece that meets no edge of the side any more (or any).
        for s_in, e_in in ((s2, e1), (s1, e2)):
            if not ctx.dd[s_in]:
                continue
            res = list(cand.reserve[s_in])
            inwards = {ctx.inward[e] for e in dd_middle(ctx, cand, s_in)}
            fits = [p for p in dd_targets(ctx, cand, s_in, res)
                    if any(iw == ctx.inward[e_in] for iw, _, _ in ctx.opts[s_in][p])]
            if not fits:
                continue
            dead = [i for i, p in enumerate(res)
                    if not any(iw in inwards for iw, _, _ in ctx.opts[s_in][p])]
            i = dead[rng.randrange(len(dead))] if dead else rng.randrange(len(res))
            p = fits[rng.randrange(len(fits))]
            cand.used.discard(res[i])
            cand.used.add(p)
            res[i] = p
            cand.reserve[s_in] = tuple(res)
        touched = {s1, s2}
        pair = frozenset((e1, e2))

    else:                                               # block
        k = ctx.blocked_corners[rng.randrange(len(ctx.blocked_corners))]
        g = DD_CORNERS[k]
        a, b, q, qr = cand.block[k]
        touched = {g.a_side, g.b_side}
        if rng.random() < 0.5:
            # A new block edge, one that meets the corner piece's frame colour;
            # the old one takes its place and side.
            which = rng.randrange(2)
            old, s_old, d = ((a, g.a_side, g.a_dir) if which == 0
                             else (b, g.b_side, g.b_dir))
            need = ctx.corner_rot[(cand.seat[k], k)][d]
            blocked = dd_block_edges(cand)
            opts = [e for e in ctx.edge_ids
                    if e not in blocked and ctx.edge_rot[e][s_old][_opp(d)] == need
                    and (not ctx.keep_border or cand.edge_side[e] == s_old)]
            if not opts:
                return None
            e = opts[rng.randrange(len(opts))]
            s_e = cand.edge_side[e]
            cand.edge_side[e], cand.edge_side[old] = s_old, s_e
            a, b = (e, b) if which == 0 else (a, e)
            touched.add(s_e)
        # A q that fits the block's two edges.
        A, B = ctx.edge_rot[a][g.a_side], ctx.edge_rot[b][g.b_side]
        opts = [o for o in ctx.q_index[k].get((A[g.b_dir], B[g.a_dir]), ())
                if o[0] not in cand.used or o[0] == q]
        if not opts:
            return None
        q2, qr2 = opts[rng.randrange(len(opts))]
        cand.used.discard(q)
        cand.used.add(q2)
        cand.block[k] = (a, b, q2, qr2)
        corners = (k,)

    dd_refresh(ctx, cand, sorted(touched), corners, config)
    return cand, pair

# A double-decker move costs ~1-5 ms, 30-150x a classic one, so its probe
# samples fewer; most land feasible, which still leaves hundreds of score
# changes to measure the spread over.
DD_PROBE_CANDIDATES = 600

def dd_probe(ctx: DeckerContext, st: DeckerState, config: AnnealingConfig,
             rng: random.Random, cands: int = DD_PROBE_CANDIDATES) -> MoveProbe:
    """probe_move_scale over the double decker's own moves."""
    deltas: List[float] = []
    improving = 0
    for _ in range(cands):
        prop = dd_propose(ctx, st, rng, config)
        if prop is not None and prop[0].hard == 0.0:
            d = prop[0].score - st.score
            deltas.append(d)
            if d > 0.0:
                improving += 1
    return move_probe(deltas, improving, cands)

def dd_layout(ctx: DeckerContext, st: DeckerState, rng: random.Random
              ) -> Tuple[List[int], List[int]]:
    """One concrete layout of a feasible state: 0-based (pos, rot) vectors in
    the board format every stage reads (999 = not placed). A classic side takes
    one random trail; a two-tall side one layout drawn uniformly from its
    reserve's. Raises if any two placed neighbours disagree -- a bug."""
    pos = [999] * N_SPINS
    rot = [0] * N_SPINS

    def put(pid: int, cell: Tuple[int, int], spin: int) -> None:
        pos[pid - 1] = cell[0] * 16 + cell[1]
        rot[pid - 1] = spin

    for pid, k in st.corner_pos.items():
        put(pid, DD_CORNERS[k].cell, ctx.corner_spin[(pid, k)])
    for k in ctx.blocked_corners:
        g = DD_CORNERS[k]
        a, b, q, qr = st.block[k]
        put(a, g.at(g.a_dir), ctx.edge_spin[a][g.a_side])
        put(b, g.at(g.b_dir), ctx.edge_spin[b][g.b_side])
        put(q, g.at(g.a_dir, g.b_dir), qr)
    for s in Side:
        k0, k1 = DD_ENDS[s]
        cells = DD_CELLS[s][(1 if ctx.blocked[k0] else 0):(13 if ctx.blocked[k1] else 14)]
        if ctx.dd[s]:
            row = dd_counter(ctx, st, s).sample(rng)
        else:
            order = euler_trail(dd_frame_arcs(ctx, s, dd_middle(ctx, st, s)),
                                dd_endpoint(ctx, st, s, k0), dd_endpoint(ctx, st, s, k1), rng)
            row = [(e, None, None) for e in order] if order is not None else []
        if len(row) != len(cells):
            raise AssertionError(f"double decker: nothing to lay out on {SIDE_NAMES[s]}")
        dr, dc = DD_STEP[DD_INWARD[s]]
        for (e, p, f), cell in zip(row, cells):
            put(e, cell, ctx.edge_spin[e][s])
            if p is not None:
                put(p, (cell[0] + dr, cell[1] + dc), (f - s) % 4)
    bad = board_mismatches(ctx.pieces_by_id, pos, rot)
    if bad:
        raise AssertionError(f"double decker: the laid-out witness has {bad} broken edge(s)")
    return pos, rot

def board_mismatches(pieces_by_id: Dict[int, Piece], pos: Sequence[int],
                     rot: Sequence[int]) -> int:
    """Broken edges between placed neighbours of a 0-based (pos, rot) board."""
    grid: Dict[int, Tuple[int, int, int, int]] = {}
    for i, cell in enumerate(pos):
        if cell != 999:
            grid[cell] = rotate_sides(pieces_by_id[i + 1].sides, rot[i])
    bad = 0
    for cell, S in grid.items():
        r, c = divmod(cell, 16)
        if c < 15 and cell + 1 in grid and S[Side.RIGHT] != grid[cell + 1][Side.LEFT]:
            bad += 1
        if r < 15 and cell + 16 in grid and S[Side.TOP] != grid[cell + 16][Side.BOTTOM]:
            bad += 1
    return bad

def dd_best_record(ctx: DeckerContext, st: DeckerState, step: int,
                   inner_capacity: Counter, config: AnnealingConfig,
                   rng: random.Random) -> BestRecord:
    """A feasible state as the record every writer takes: the border's spins
    and CLASSIC counts (so the comment's TOP=.. fields stay what the spins
    say), the reserve counts and their whole-pool bounds, the reserves, and a
    witness layout drawn from them. The flags are every reserved piece and
    every corner block's q."""
    rs = _build_run_state(ctx.pieces_by_id, st.edge_side, st.corner_pos,
                          inner_capacity, config)
    pos, rot = dd_layout(ctx, st, rng)
    qs = {q for _, _, q, _ in st.block.values()}
    pool = {s: pool_bound(ctx, s, dd_middle(ctx, st, s),
                          dd_endpoint(ctx, st, s, DD_ENDS[s][0]),
                          dd_endpoint(ctx, st, s, DD_ENDS[s][1]), qs)
            for s in ctx.dd_sides}
    flags = sorted(qs | {p for r in st.reserve.values() for p in r})
    return BestRecord(score=st.score, euler_counts=rs.euler_counts(),
                      rot_vec=rotation_vector(ctx.pieces_by_id, rs), step=step,
                      dd_counts=dd_counts(st), board=(pos, rot), flags=tuple(flags),
                      dd_pool=pool, reserve={s: tuple(sorted(r)) for s, r in st.reserve.items()})

def dd_state_str(st: DeckerState, config: AnnealingConfig) -> str:
    return "  ".join(f"{SIDE_NAMES[s]}{'*' if s in config.decker_sides else ''}"
                     f"={st.evals[s].euler_count}/{'OK' if st.evals[s].feasible else 'bad'}"
                     for s in Side) + (f"  blocks-bad={sum(st.block_bad.values())}"
                                       if st.block_bad else "")

def anneal_decker(ctx: DeckerContext, st: DeckerState, steps: int,
                  t0: float, tf: float, config: AnnealingConfig, rng: random.Random,
                  log: List[str]) -> Tuple[Optional[DeckerState], int, Optional[float]]:
    """The double decker's walk: the classic loop's schedule, tabu, candidate
    harvesting and reporting, over dd_propose's moves. Returns (best feasible
    state or None, the step it was found at, last window's acceptance)."""
    best: Optional[DeckerState] = st if st.hard == 0.0 else None
    best_step = 0
    accepted_window = step_window = feasible_seen = 0
    acc_last: Optional[float] = None
    tabu_q: deque = deque()
    tabu_set: set = set()
    if config.verbose:
        log.append(progress_str("  double decker, initial", st.score, st.evals, config)
                   + f"  hard={st.hard:g}")
    for step in range(1, steps + 1):
        temperature = t0 * (tf / t0) ** (step / steps) if t0 > 0 else 0.0
        prop = dd_propose(ctx, st, rng, config)
        if prop is not None:
            cand, pair = prop
            ok = cand.hard == 0.0
            new_best = ok and (best is None or cand.score > best.score)
            tabu_reject = (pair is not None and config.tabu_length > 0
                           and pair in tabu_set and not new_best)
            if not tabu_reject:
                delta = cand.score - st.score
                if delta >= 0 or (temperature > 0 and
                                  rng.random() < math.exp(delta / temperature)):
                    st = cand
                    accepted_window += 1
                    if pair is not None and config.tabu_length > 0 and pair not in tabu_set:
                        tabu_q.append(pair)
                        tabu_set.add(pair)
                        while len(tabu_q) > config.tabu_length:
                            tabu_set.discard(tabu_q.popleft())
            if ok:
                feasible_seen += 1
                if new_best:
                    best, best_step = cand, step
            step_window += 1
        if config.report_every and step % config.report_every == 0:
            acc_last = accepted_window / step_window if step_window else 0.0
            if config.verbose:
                log.append(f"  step {step:>7}  T={temperature:7.3g}  acc={acc_last:5.1%}  "
                           f"feas={feasible_seen:>5}  score={st.score:10.4f}  "
                           f"{dd_state_str(st, config)}")
            accepted_window = step_window = 0
    if step_window:
        acc_last = accepted_window / step_window
    return best, best_step, acc_last

def decker_context(pieces_by_id: Dict[int, Piece], corner_ids: List[int],
                   edge_ids: List[int], config: AnnealingConfig) -> DeckerContext:
    return DeckerContext(pieces_by_id, corner_ids, edge_ids, config.decker_sides,
                         keep_border=config.decker_keep_border)

def anneal_decker_restart(restart: int, pieces_by_id: Dict[int, Piece],
                          corner_ids: List[int], edge_ids: List[int],
                          inner_capacity: Counter,
                          config: AnnealingConfig) -> RestartResult:
    """One --double_decker restart, in a worker.

    Cold: a classic walk over the first --decker_warmup of the steps finds a
    usable border, dd_seed builds the double decker on it, and the schedule for
    the rest is probed right here (a cold restart's border is its own, so there
    is nothing for the parent to decide it from). Warm: the row is seeded the
    way the parent seeded it to probe, and the parent's T0/Tf are used."""
    t_start = time.perf_counter()
    log: List[str] = []
    ctx = decker_context(pieces_by_id, corner_ids, edge_ids, config)
    seed = config.random_seed
    if config.verbose:
        log.append(f"\nRestart {restart}/{config.restarts} (double decker)")

    if config.start_spins is None:
        warm = max(1, int(round(config.decker_warmup * config.steps_per_restart)))
        classic = replace(config, steps_per_restart=warm, decker_sides=(), verbose=False)
        first = anneal_one_restart(restart, pieces_by_id, corner_ids, edge_ids,
                                   inner_capacity, classic)
        if config.verbose:
            log.append(f"  classic warm-up, {warm} steps: " +
                       (f"score={first.best.score:.4f}  {best_counts_str(first.best)}"
                        if first.best is not None else "no usable border"))
        if first.best is None:
            elapsed = time.perf_counter() - t_start
            log.append(restart_str(restart, config, None, elapsed))
            return RestartResult(restart=restart, best=None, log=log, elapsed=elapsed)
        edge_side, corner_pos = border_from_spins(pieces_by_id, first.best.rot_vec)
        steps = config.steps_per_restart - warm
        st = dd_seed(ctx, edge_side, corner_pos,
                     random.Random(f"decker-seed:{seed}:{restart}"), config)
        rng = random.Random(f"decker:{seed}:{restart}")
        if st is not None:
            t0, tf, lines, _ = resolve_schedule(
                config, None, pieces_by_id, edge_ids, inner_capacity, None,
                config.decker_T0, config.decker_Tf,
                probe=dd_probe(ctx, st, config,
                               random.Random(f"decker-probe:{seed}:{restart}")))
            if config.verbose:
                log.extend("  " + line for line in lines)
    else:
        edge_side, corner_pos = border_from_spins(pieces_by_id, config.start_spins)
        st = dd_seed(ctx, edge_side, corner_pos,
                     random.Random(restart_seed(seed, 1)), config)
        steps = config.steps_per_restart
        rng = random.Random(restart_seed(seed, restart))
        t0, tf = config.T0, config.Tf

    best, best_step, acc_last = (None, 0, None)
    if st is not None:
        best, best_step, acc_last = anneal_decker(ctx, st, steps, t0, tf,
                                                  config, rng, log)
    rec = None
    if best is not None:
        rec = dd_best_record(ctx, best, best_step, inner_capacity, config,
                             random.Random(f"decker-layout:{seed}:{restart}"))
    elapsed = time.perf_counter() - t_start
    warning = None
    if acc_last is not None and acc_last < 0.005:
        warning = (f"  [warn] {acc_last:.1%} of moves accepted in the last window:"
                   f" Tf={tf:g} froze the search early")
    if config.verbose:
        if warning:
            log.append(warning)
        if rec is not None:
            log.append(f"  restart best: score={rec.score:.4f}  "
                       f"{dd_state_str(best, config)}  DeckerPool={decker_pool_token(rec, config)}")
            log.append(best_line(restart, rec))
        else:
            log.append("  restart best: no feasible double decker found")
        log.append(f"  time: {elapsed:.1f}s")
    else:
        log.append(restart_str(restart, config, rec, elapsed))
        if warning:
            log.append(warning)
    return RestartResult(restart=restart, best=rec, log=log, elapsed=elapsed,
                         warning=warning)

def decker_token(rec: BestRecord, config: AnnealingConfig) -> str:
    """The Decker= value: each two-tall side's exact reserve layout count,
    TOP/RIGHT/BOTTOM/LEFT, '-' for a classic side. No side names, so no
    comment parser can take it for one of the TOP=.. fields."""
    return "/".join(str(rec.dd_counts[s]) if s in config.decker_sides else "-"
                    for s in Side)

def decker_pool_token(rec: BestRecord, config: AnnealingConfig) -> str:
    """The DeckerPool= value: the whole-pool upper bound per two-tall side."""
    return "/".join(f"{rec.dd_pool[s]:.3g}" if s in config.decker_sides else "-"
                    for s in Side)

def board_name(config: AnnealingConfig, row_id: str) -> str:
    return f"dd{config.random_seed}_{row_id}"

def append_board(name: str, rec: BestRecord, path: str) -> None:
    """One witness board, in the beamer's own line format (E555_beamer.c:
    `config_id, sol_idx, pos[256], rot[256]`), which the finalizer reads,
    under a `#` line naming each side's reserve -- every reader skips `#`
    lines, and it is what lets Decker= be recounted from the file alone."""
    pos, rot = rec.board
    reserve = "  ".join(f"{SIDE_NAMES[s]}=" + ",".join(map(str, rec.reserve[s]))
                        for s in Side if s in rec.reserve)
    with open(path, "a") as f:
        f.write(f"# {name} reserve  {reserve}\n")
        f.write(f"{name}, 0, " + ", ".join(map(str, pos)) + ", "
                + ", ".join(map(str, rot)) + "\n")

def write_best(row_id: str, rec: BestRecord, out_path: str, provenance: str,
               config: AnnealingConfig, decker_out: Optional[str]) -> Optional[str]:
    """Append one written border -- and, for a double decker, its witness
    board under the name its comment carries. Returns that name."""
    if rec.board is None or not decker_out:
        append_rotations(row_id, rec, out_path, provenance)
        return None
    name = board_name(config, row_id)
    append_board(name, rec, decker_out)
    append_rotations(row_id, rec, out_path, provenance,
                     extra=f"Decker={decker_token(rec, config)}  "
                           f"DeckerPool={decker_pool_token(rec, config)}  Board={name}  ")
    return name

# =============================================================================
# Main annealing loop
# =============================================================================

def restart_seed(master: int, restart: int) -> int:
    """The RNG seed for one restart, derived from the master seed rather than
    drawn from a stream shared with the other restarts. That is what makes a
    restart's result depend only on --rng_seed and its own index, never on
    how many workers ran it or in what order they finished."""
    return master * 1_000_003 + restart

def anneal_one_restart(restart: int,
                       pieces_by_id: Dict[int, Piece],
                       corner_ids: List[int],
                       edge_ids: List[int],
                       inner_capacity: Counter,
                       config: AnnealingConfig) -> RestartResult:
    """One independent annealing walk. This runs in a worker process, so it
    prints nothing and touches no file: everything it has to say goes into
    the returned log for the parent to replay in restart order."""
    if config.decker_sides:
        return anneal_decker_restart(restart, pieces_by_id, corner_ids, edge_ids,
                                     inner_capacity, config)
    log: List[str] = []
    t0  = time.perf_counter()
    rng = random.Random(restart_seed(config.random_seed, restart))

    state = initial_state(pieces_by_id, corner_ids, edge_ids, rng, config, inner_capacity)

    # A best is otherwise only ever taken from a CANDIDATE swap, never from the
    # state the restart began in. Harmless for a random start, which is
    # essentially never feasible -- but it would let a warm refinement report,
    # and append to --out, a border worse than the one it was asked to improve.
    # Seeding it here makes refinement monotone by construction: the run can
    # only ever hand back the input row or something better than it.
    best: Optional[BestRecord] = None
    if hard_penalty(state.evals, state.inward_tally, inner_capacity, config) == 0.0:
        best = BestRecord(score=state.score, euler_counts=state.euler_counts(),
                          rot_vec=rotation_vector(pieces_by_id, state), step=0)
    accepted = 0
    accepted_window = 0
    step_window = 0
    feasible_seen = 0
    acc_last:  Optional[float] = None   # last window's acceptance rate

    tabu_q:   deque = deque()
    tabu_set: set   = set()

    def tabu_add(pair: frozenset) -> None:
        if config.tabu_length <= 0 or pair in tabu_set:
            return
        tabu_q.append(pair)
        tabu_set.add(pair)
        while len(tabu_q) > config.tabu_length:
            tabu_set.discard(tabu_q.popleft())

    if config.verbose:
        log.append(f"\nRestart {restart}/{config.restarts}")
        log.append(progress_str("  initial", state.score, state.evals, config))

    for step in range(1, config.steps_per_restart + 1):
        progress = step / config.steps_per_restart
        temperature = config.T0 * (config.Tf / config.T0) ** progress

        allow_corners = (config.swap_corners and config.fix_corners == 0
                         and progress < CORNER_PHASE)
        do_corner     = allow_corners and (rng.random() >= config.p_edge)
        evaluated     = True   # did this step actually score a candidate?

        if do_corner:
            pid_a, pid_b = rng.sample(corner_ids, 2)
            cr = try_corner_swap(state, pieces_by_id, pid_a, pid_b, inner_capacity, config)

            # A corner swap moves every side's endpoints at once, so it is one
            # of the few moves that can lift all four counts together. Tracked
            # here, before the accept decision and from the pre-commit state,
            # for the same reason the edge branch does it: the candidate is
            # what scored, and it would otherwise be evaluated and discarded.
            if cr.hard == 0.0:
                feasible_seen += 1
                if best is None or cr.new_score > best.score:
                    best = BestRecord(
                        score=cr.new_score,
                        euler_counts={s: cr.new_evals[s].euler_count for s in Side},
                        rot_vec=rotation_vector(pieces_by_id, state,
                                                corner_swap=(pid_a, pid_b)),
                        step=step)

            delta = cr.new_score - state.score
            if delta >= 0 or (temperature > 0 and rng.random() < math.exp(delta / temperature)):
                commit_corner_swap(state, pid_a, pid_b, cr)
                accepted += 1
                accepted_window += 1
        else:
            pid_a, pid_b = rng.sample(edge_ids, 2)
            er = try_edge_swap(state, pieces_by_id, pid_a, pid_b, inner_capacity, config)
            if er is None:
                # Both pieces are already on the same side: nothing to
                # evaluate, so this step does not count toward the
                # acceptance rate. It must NOT skip the reporting block
                # below, or ~24% of report lines are silently lost.
                evaluated = False
            else:
                pair = frozenset({pid_a, pid_b})

                # Candidate feasibility: er.hard covers all four sides AND
                # the inner-colour inventory, so a border that starves the
                # inner pieces of a colour can never be recorded as a best.
                candidate_ok = (er.hard == 0.0)
                is_new_best  = candidate_ok and (best is None or er.new_score > best.score)

                # Tabu with aspiration on new global best
                tabu_reject = pair in tabu_set and config.tabu_length > 0 and not is_new_best

                if not tabu_reject:
                    delta = er.new_score - state.score
                    if delta >= 0 or (temperature > 0 and rng.random() < math.exp(delta / temperature)):
                        commit_edge_swap(state, pid_a, pid_b, er)
                        accepted += 1
                        accepted_window += 1
                        tabu_add(pair)

                # Best tracking is independent of SA acceptance
                if candidate_ok:
                    feasible_seen += 1
                    if is_new_best:
                        rv = rotation_vector(
                            pieces_by_id, state,
                            edge_swap=(pid_a, pid_b, er.side_a, er.side_b))
                        ec = {
                            er.side_a: er.new_se_a.euler_count,
                            er.side_b: er.new_se_b.euler_count,
                            **{s: state.evals[s].euler_count
                               for s in Side if s != er.side_a and s != er.side_b},
                        }
                        best = BestRecord(score=er.new_score, euler_counts=ec,
                                          rot_vec=rv, step=step)

        if evaluated:
            step_window += 1
        if config.report_every and step % config.report_every == 0:
            acc_last = accepted_window / step_window
            if config.verbose:
                log.append(
                    f"  step {step:>7}  T={temperature:7.3g}  "
                    f"acc={acc_last:5.1%}  feas={feasible_seen:>5}  "
                    f"score={state.score:10.4f}  {_counts_str(state.evals, config)}"
                )
            accepted_window = 0
            step_window = 0

    # Close the window once more on whatever the tail left. Without this the
    # rate only ever existed at multiples of report_every, so any run shorter
    # than 25000 steps -- the release gate's own 3000 included -- could never
    # report it and never raise the freeze warning below.
    if step_window:
        acc_last = accepted_window / step_window

    elapsed = time.perf_counter() - t0

    # A hot start is fine and even wanted here (see the T0/Tf note in
    # AnnealingConfig), so the only end worth warning about is a cold one:
    # a run that stops accepting anything spends its tail doing nothing.
    # At the tuned Tf the last window sits near 1.5%, and every schedule
    # that scored worse in the sweep reported under 0.3%. Worth saying in
    # either output mode -- it means the schedule, not the seed, is wrong.
    warning = None
    if acc_last is not None and acc_last < 0.005:
        warning = (f"  [warn] {acc_last:.1%} of moves accepted in the last window:"
                   f" Tf={config.Tf:g} froze the search early")

    if config.verbose:
        if warning:
            log.append(warning)
        if best is not None:
            ec = best.euler_counts
            log.append(
                f"  restart best: score={best.score:.4f}  "
                f"TOP={ec[Side.TOP]}  RIGHT={ec[Side.RIGHT]}  "
                f"BOTTOM={ec[Side.BOTTOM]}  LEFT={ec[Side.LEFT]}"
            )
            log.append(best_line(restart, best))
        else:
            log.append(f"  restart best: no feasible border found")
        log.append(f"  time: {elapsed:.1f}s  "
                   f"({config.steps_per_restart/elapsed:.0f} steps/s)")
    else:
        # One line, then the caveat under it if there is one.
        log.append(restart_str(restart, config, best, elapsed))
        if warning:
            log.append(warning)

    return RestartResult(restart=restart, best=best, log=log, elapsed=elapsed,
                         warning=warning)

def anneal_row_job(job: Tuple[int, int, AnnealingConfig],
                   **shared) -> Tuple[int, RestartResult]:
    """One (row, restart) job of a whole-file refinement: the row's own config
    rides along with the job, and the row index comes back with the result so
    the parent can regroup the restarts by row."""
    row, restart, config = job
    return row, anneal_one_restart(restart, config=config, **shared)

_STOP = False
def _request_stop(signum, frame):
    """Ctrl-C: stop after the restart in flight. Restarts are independent, so
    everything already appended to --out is complete and usable -- losing it
    all because the run was interrupted is pure waste."""
    global _STOP
    _STOP = True
    print("\n[Ctrl-C] finishing the current restart then stopping...", flush=True)

def run_jobs(task, jobs: Sequence, workers: int, consume) -> bool:
    """Run `task` over `jobs` and hand every result to `consume` in JOB order,
    whatever order the workers finish in. Returns True if Ctrl-C cut it short.

    ex.map yields in submission order, which is the whole ordering guarantee:
    one slow job delays the REPORTING of the ones after it, not their
    execution. On Ctrl-C the jobs still queued are cancelled -- left to the
    pool's exit, every one of them would run to completion and then be thrown
    away -- while the ones already running finish and are kept. Those are the
    next jobs in order, so consuming up to the first cancelled one keeps the
    output a clean prefix of the jobs."""
    if workers == 1:
        for job in jobs:
            if _STOP:
                return True
            consume(task(job))
        return False
    with ProcessPoolExecutor(max_workers=workers) as ex:
        results = ex.map(task, jobs)
        for res in results:
            consume(res)
            if _STOP:
                ex.shutdown(wait=True, cancel_futures=True)
                try:
                    for res in results:
                        consume(res)
                except CancelledError:
                    pass
                return True
    return False

def run_annealing(pieces: Sequence[Piece], config: AnnealingConfig,
                  out_path: Optional[str] = None, provenance: str = "",
                  baseline: Optional[float] = None,
                  header_extra: Sequence[str] = (),
                  decker_out: Optional[str] = None) -> None:
    pieces_by_id  = {p.id: p for p in pieces}
    corner_ids, edge_ids = classify_boundary_pieces(pieces)
    inner_capacity = build_inner_capacity(pieces)

    print_header(pieces, corner_ids, edge_ids, config, header_extra)

    # At most one worker per restart: --restarts 1 must not start a pool of 8.
    workers = max(1, min(config.threads, config.restarts))
    task = functools.partial(anneal_one_restart,
                             pieces_by_id=pieces_by_id,
                             corner_ids=corner_ids,
                             edge_ids=edge_ids,
                             inner_capacity=inner_capacity,
                             config=config)

    wall0     = time.perf_counter()
    work_time = 0.0
    feasible  = 0
    at_least  = 0      # warm runs: restarts that matched or beat the input row
    champion: Optional[RestartResult] = None

    def consume(res: RestartResult) -> None:
        """Everything that reaches stdout or the --out file happens here, in
        the parent, one restart at a time and in restart order."""
        nonlocal work_time, feasible, at_least, champion
        if res.log:
            print("\n".join(res.log), flush=True)
        work_time += res.elapsed
        if res.best is not None:
            feasible += 1
            if champion is None or res.best.score > champion.best.score:
                champion = res
            if baseline is not None and res.best.score >= baseline - 1e-12:
                at_least += 1
            if out_path:
                write_best(f"r{res.restart}", res.best, out_path, provenance,
                           config, decker_out)

    if workers > 1:
        print(f"[par] {config.restarts} restarts on {workers} worker processes")
    if not config.verbose:
        print()          # verbose restart blocks open with their own blank line

    stopped = run_jobs(task, range(1, config.restarts + 1), workers, consume)

    wall = time.perf_counter() - wall0
    print(f"\n=== run summary ===")
    # Wall clock, plus the mean cost of a restart. Deliberately NOT the sum of
    # the per-restart times: workers share cores, so each one's own clock runs
    # long and that total would read as a speedup the run did not achieve.
    mean  = work_time / config.restarts if config.restarts else 0.0
    detail = f"{workers} workers, " if workers > 1 else ""
    print(f"[sum] {config.restarts} restarts in {wall:.1f}s = {wall/60:.2f} min"
          f"  ({detail}{mean:.1f}s per restart)")
    if stopped:
        print("[sum] stopped early on Ctrl-C; the borders already written are complete")
    print(f"[sum] {feasible}/{config.restarts} restarts found a feasible border")
    if baseline is not None:
        # Seeding best from the starting state makes this a guarantee, not a
        # hope: every restart hands back the input row or something better. A
        # count below the restart total means the seeding is broken.
        what = ("seeded double decker's" if config.decker_sides else "input row's")
        print(f"[sum] {at_least}/{config.restarts} restarts matched or beat the "
              f"{what} {baseline:.4f}")
    if champion is not None:
        ec = champion.best.euler_counts
        print(f"[sum] best: restart {champion.restart}  score={champion.best.score:.4f}  "
              f"TOP={ec[Side.TOP]} RIGHT={ec[Side.RIGHT]} "
              f"BOTTOM={ec[Side.BOTTOM]} LEFT={ec[Side.LEFT]}"
              + (f"  Decker={decker_token(champion.best, config)}  "
                 f"DeckerPool={decker_pool_token(champion.best, config)}  "
                 f"Board={board_name(config, f'r{champion.restart}')}"
                 if champion.best.dd_counts is not None else ""))
    if out_path:
        print(f"[sum] rotations appended to {out_path}")
    if decker_out:
        print(f"[sum] witness boards appended to {decker_out}")

def run_rows(pieces: Sequence[Piece], plans: Sequence[RowPlan],
             config: AnnealingConfig, out_path: str, input_path: str,
             header_extra: Sequence[str] = (),
             decker_out: Optional[str] = None) -> None:
    """Refine every row of --input: --restarts restarts per row, the best of
    them written back, one output row per input row and in input order.

    The jobs are (row, restart) pairs laid out row by row, so the pool keeps
    every core busy however the rows and restarts divide, and run_jobs hands
    them back in that same order. A row is complete -- written, and reported
    -- the moment its last restart comes in, which makes the output file a
    prefix of the input at every moment, Ctrl-C included."""
    pieces_by_id  = {p.id: p for p in pieces}
    corner_ids, edge_ids = classify_boundary_pieces(pieces)
    inner_capacity = build_inner_capacity(pieces)

    print_header(pieces, corner_ids, edge_ids, config, header_extra, per_row=True)

    per_row = config.restarts
    jobs    = [(p.row, r, p.config) for p in plans for r in range(1, per_row + 1)]
    workers = max(1, min(config.threads, len(jobs)))
    task = functools.partial(anneal_row_job,
                             pieces_by_id=pieces_by_id,
                             corner_ids=corner_ids,
                             edge_ids=edge_ids,
                             inner_capacity=inner_capacity)

    by_row   = {p.row: p for p in plans}
    arrived: Dict[int, List[RestartResult]] = defaultdict(list)
    width    = len(str(plans[-1].row))
    wall0    = time.perf_counter()
    work_time = 0.0
    ran = done = written = matched = improved = froze = 0
    gained: Counter = Counter()
    champion: Optional[Tuple[RowPlan, BestRecord]] = None

    def consume(item: Tuple[int, RestartResult]) -> None:
        """Everything that reaches stdout or the --out file happens here, in
        the parent, one row at a time and in input order."""
        nonlocal work_time, ran, done, written, matched, improved, froze, champion
        row, res = item
        work_time += res.elapsed
        ran += 1
        got = arrived[row]
        got.append(res)
        if len(got) < per_row:
            return
        del arrived[row]
        plan = by_row[row]
        done += 1

        # Restart order, strict >: the first of equal scores wins, the same
        # rule --row N applies, so a row here is the best --row N would give.
        best: Optional[BestRecord] = None
        for r in got:
            if r.best is not None and (best is None or r.best.score > best.score):
                best = r.best

        name = None
        if best is not None:
            name = write_best(f"r{row}", best, out_path,
                              f"  From={input_path}:row{row}", config, decker_out)
            written += 1
            if plan.baseline is None or best.score >= plan.baseline - 1e-12:
                matched += 1
            if plan.baseline is None or best.score > plan.baseline + 1e-12:
                improved += 1
            gained.update(s for s in Side if best.euler_counts[s] > plan.start[s])
            if champion is None or best.score > champion[1].score:
                champion = (plan, best)

        if config.verbose:
            print(f"\n=== row {row} (line {plan.lineno}) ===")
            print("\n".join(plan.header))
            for r in got:
                print("\n".join(r.log))
            print()
        print(row_delta_str(plan, best, sum(r.elapsed for r in got), width)
              + (f"  Decker={decker_token(best, config)}  Board={name}" if name else ""),
              flush=True)
        # Counted, not printed per row: under a polishing schedule it fires on
        # most rows, and would bury the table of deltas it sits in.
        if any(r.warning for r in got):
            froze += 1

    if workers > 1:
        print(f"[par] {len(plans)} rows x {per_row} restarts on {workers} worker processes")
    print()

    stopped = run_jobs(task, jobs, workers, consume)

    wall = time.perf_counter() - wall0
    print(f"\n=== run summary ===")
    # Wall clock, and the mean cost of a restart -- see run_annealing for why
    # the per-restart times are never summed into a total.
    mean   = work_time / ran if ran else 0.0
    detail = f"{workers} workers, " if workers > 1 else ""
    print(f"[sum] {len(plans)} rows x {per_row} restarts = {len(jobs)} restarts in "
          f"{wall:.1f}s = {wall/60:.2f} min  ({detail}{mean:.1f}s per restart)")
    if stopped:
        print(f"[sum] stopped early on Ctrl-C: the first {done} of {len(plans)} rows "
              f"are finished and written, the rest are not")
    # Seeding every restart's best from its starting row makes this a
    # guarantee: a count short of the rows finished means the seeding broke
    # (or an input row was not a usable border to begin with).
    what = "their seeded double decker" if config.decker_sides else "their input row"
    print(f"[sum] {matched}/{done} rows matched or beat {what}: "
          f"{improved} improved, {written - improved} unchanged")
    if written < done:
        print(f"[sum] {done - written} row(s) found no feasible border and were not written")
    if froze:
        print(f"[warn] {froze}/{done} rows had a restart accept under 0.5% of moves in "
              f"its last window: Tf froze the search early (--verbose shows which)")
    print("[sum] rows where a side gained trails: "
          + "  ".join(f"{SIDE_NAMES[s]}={gained[s]}" for s in Side))
    if champion is not None:
        plan, rec = champion
        ec = rec.euler_counts
        print(f"[sum] best: row {plan.row}  score={rec.score:.4f}  "
              f"TOP={ec[Side.TOP]} RIGHT={ec[Side.RIGHT]} "
              f"BOTTOM={ec[Side.BOTTOM]} LEFT={ec[Side.LEFT]}")
    print(f"[sum] {written} row(s) appended to {out_path}, in input order")
    if decker_out:
        print(f"[sum] their witness boards appended to {decker_out}, same order")

# =============================================================================
# CLI
# =============================================================================

# Where a cold run's borders go when --out is not given: the name every
# example and runner already hands Stage B.
COLD_OUT = "rotations.csv"

def default_out_path(input_path: Optional[str]) -> str:
    """The rotations file when --out is not given. There is always one: the
    borders are the whole deliverable, and a run that printed only summary
    lines used to lose every one of them. A warm run writes beside its input,
    FILE -> FILE_refined.csv, the FILE_tag naming E555_rotate.py and
    E555_clean_csv.py use for their defaults."""
    if not input_path:
        return COLD_OUT
    src = Path(input_path)
    return str(src.with_name(f"{src.stem}_refined{src.suffix or '.csv'}"))

def open_out(path: str, marker: str) -> None:
    """Fail now, not after the first restart has already been computed: a bad
    path or an unwritable directory used to surface minutes in, with the work
    already done and nowhere to put it. Opening in append mode creates the
    file if needed and leaves an existing one untouched.

    Rows accumulate across runs, which is deliberate -- several short runs
    build one border pool -- so every run opens with a `# run` marker, without
    which there is no way to tell afterwards which rows came from which run."""
    try:
        with open(path, "a") as fh:
            fh.write(marker + "\n")
    except OSError as e:
        raise SystemExit(f"[ERROR] cannot write --out {path}: {e}")

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="E555 edge annealer -- Stage A Eternity II border optimizer",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("seed_file", help="4-integer-per-line piece file")
    p.add_argument("--out", default=None, metavar="FILE",
                   help="append each restart's best border to this rotations "
                        "CSV in the format Stage B reads (id + 256 spins). "
                        "Unset: <stem>_refined.csv beside --input, or "
                        f"{COLD_OUT} in the current directory without it")
    p.add_argument("--verbose", action="store_true",
                   help="print the whole search -- full config, per-step "
                        "progress and the BEST lines -- instead of one "
                        "summary line per restart")

    g = p.add_argument_group("schedule")
    g.add_argument("--restarts", type=int, default=AnnealingConfig.restarts)
    g.add_argument("--steps",    type=int, default=AnnealingConfig.steps_per_restart,
                   dest="steps_per_restart")
    g.add_argument("--rng_seed", type=int, default=0, dest="random_seed",
                   help="Random seed (0 = auto-sample)")
    g.add_argument("--threads",  type=int, default=AnnealingConfig.threads,
                   help="Worker processes; the restarts run in parallel "
                        "(0 = one per core). Never changes the result")
    g.add_argument("--T0",       type=float, default=None,
                   help="Initial temperature; left unset the schedule is "
                        "resolved from the starting state -- calibrated to it "
                        "when it has a feasible neighbourhood, anchored to the "
                        "feasibility cliff when it does not")
    g.add_argument("--Tf",       type=float, default=None,
                   help="Final temperature; unset, as --T0")

    g = p.add_argument_group("warm start -- refine a border you already have")
    g.add_argument("--input", default=None, metavar="FILE",
                   help="rotations CSV to take the starting border from, "
                        "normally an earlier --out of this script. Every "
                        "restart starts from the same row and refines it, so "
                        "the weights now in force need not be the ones that "
                        "produced the file")
    g.add_argument("--row", type=int, default=None, metavar="N",
                   help="which data row of --input to refine, numbered from 0 "
                        "over data rows only -- the same numbering the "
                        "beamer's --start_row uses. Unset: refine EVERY row, "
                        "in parallel, and write the best of each row's "
                        "restarts back in input order, one row per row")

    g = p.add_argument_group(
        "objective weights (with --target_scale: side target = weight x scale, "
        "must be positive; without it: positive=maximize, negative=minimize)")
    g.add_argument("--w_top",    type=float, default=AnnealingConfig.w_top)
    g.add_argument("--w_right",  type=float, default=AnnealingConfig.w_right)
    g.add_argument("--w_bottom", type=float, default=AnnealingConfig.w_bottom)
    g.add_argument("--w_left",   type=float, default=AnnealingConfig.w_left)

    g = p.add_argument_group("search control")
    g.add_argument("--tabu",       type=int,   default=AnnealingConfig.tabu_length,
                   dest="tabu_length", help="Tabu list length (0 = disabled)")
    g.add_argument("--fix_corners", type=int, choices=[0, 1, 2],
                   default=None, dest="fix_corners",
                   help="0=random (the default)  1=edge-commutativity  "
                        "2=corner-commutativity. Not available with --input, "
                        "which brings its own corners")
    g.add_argument("--target_scale",  type=int, default=AnnealingConfig.target_scale,
                   dest="target_scale",
                   help="Score every side on how far its trail count sits from "
                        "its own target of w_side x SCALE, measured in decades, "
                        "so all four sides matter equally no matter how large "
                        "their targets are (omit for the linear objective)")

    g = p.add_argument_group("double decker -- two-tall border segments")
    g.add_argument("--double_decker", nargs="?", const="TOP,RIGHT,BOTTOM,LEFT",
                   default=None, metavar="SIDES",
                   help="score these sides (comma list; bare = all four) by their "
                        "TWO rows: the exact number of layouts of the edge row and "
                        "the inner row under it, drawn from a reserve of inner "
                        "pieces the search picks per side. Flags the reserves with "
                        "spin 1, writes a witness board of the outer two rings per "
                        "border, and notes Decker=/DeckerPool=/Board= in the comment")
    g.add_argument("--decker_reserve", type=int, default=argparse.SUPPRESS, metavar="K",
                   help="inner pieces reserved per two-tall side, 12..32 (default "
                        f"{AnnealingConfig.decker_reserve}); the strip has 12 cells, "
                        "and more pieces allow more layouts")
    g.add_argument("--decker_keep_border", action="store_true",
                   help="never move an edge to another side: only the corner blocks "
                        "and the reserves are searched, so the output spins are the "
                        "input's (or, cold, the classic warm-up's)")
    # SUPPRESS, not None: the help states the real default, and main can still
    # tell a --decker_warmup given without --double_decker.
    g.add_argument("--decker_warmup", type=float, default=argparse.SUPPRESS, metavar="F",
                   help="share of a cold restart's steps spent in the classic "
                        "walk that finds the border the double decker starts "
                        f"from (default {AnnealingConfig.decker_warmup})")
    g.add_argument("--decker_out", default=None, metavar="FILE",
                   help="append the witness boards here, in the beamer's board "
                        "format (valid finalizer input). Unset: <out stem>"
                        "_decker.csv beside the rotations file")

    return p

def parse_decker_sides(text: str) -> Tuple[int, ...]:
    by_name = {v: k for k, v in SIDE_NAMES.items()}
    sides = set()
    for tok in text.split(","):
        name = tok.strip().upper()
        if name not in by_name:
            raise SystemExit(f"[ERROR] --double_decker: unknown side '{tok.strip()}' "
                             f"(use TOP, RIGHT, BOTTOM, LEFT)")
        sides.add(int(by_name[name]))
    return tuple(sorted(sides))

def default_decker_path(out_path: str) -> str:
    """The witness boards sit beside the rotations file they belong to."""
    src = Path(out_path)
    return str(src.with_name(f"{src.stem}_decker{src.suffix or '.csv'}"))

def decker_warm_plan(config: AnnealingConfig, pieces_by_id: Dict[int, Piece],
                     corner_ids: List[int], edge_ids: List[int],
                     inner_capacity: Counter, t0_given: Optional[float],
                     tf_given: Optional[float]
                     ) -> Tuple[float, float, List[str], str, Optional[float]]:
    """A warm start's double decker, seeded exactly as each of its restarts will
    seed it, and the schedule probed from it: (T0, Tf, header lines, schedule
    kind, the seed's score when it is a usable witness, else None)."""
    ctx = decker_context(pieces_by_id, corner_ids, edge_ids, config)
    edge_side, corner_pos = border_from_spins(pieces_by_id, config.start_spins)
    st = dd_seed(ctx, edge_side, corner_pos,
                 random.Random(restart_seed(config.random_seed, 1)), config)
    if st is None:
        raise SystemExit("[ERROR] --double_decker: no inner piece is left to pair "
                         "with some edge of this border")
    t0, tf, lines, kind = resolve_schedule(
        config, None, pieces_by_id, edge_ids, inner_capacity, None, t0_given, tf_given,
        probe=dd_probe(ctx, st, config, random.Random(restart_seed(config.random_seed, 0))))
    baseline = st.score if st.hard == 0.0 else None
    head = [f"[cfg] double decker: seeded on this border  {dd_state_str(st, config)}  "
            + (f"score={st.score:.4f}" if baseline is not None
               else f"not yet usable (hard={st.hard:g}); the search repairs it first")]
    return t0, tf, head + lines, kind, baseline

def warm_start_report(path: str, row: int, lineno: int, comment: Optional[str],
                      state: RunState, config: AnnealingConfig,
                      inner_capacity: Counter) -> Tuple[List[str], Optional[float]]:
    """Header lines for a warm start, and the score a refinement must not fall
    below (None when the input row is not a usable border to begin with).

    The cross-check costs nothing and is worth having: the four trail counts
    are recomputed from the reconstructed border, and a comment that carries
    counts can only agree with them if the row and this seed file describe the
    same pieces. Checked against all 18 rows shipped in data/ and
    tests/datadriven/ -- every one reconstructs to 14/14/14/14 with four
    corners and counts identical to its comment -- so a disagreement means the
    wrong seed file, and annealing on would silently optimize a different
    board."""
    def show(d: Dict[str, int]) -> str:
        return " ".join(f"{SIDE_NAMES[s]}={d[SIDE_NAMES[s]]}" for s in Side)

    got  = {SIDE_NAMES[s]: state.evals[s].euler_count for s in Side}
    want = counts_in_comment(comment)
    if want is not None and want != got:
        raise SystemExit(
            f"[ERROR] {path} row {row} (line {lineno}) does not describe the "
            f"pieces in this seed file.\n"
            f"        its comment says  {show(want)}\n"
            f"        these pieces give {show(got)}\n"
            f"        Wrong seed file, or the row has been edited.")

    lines = [f"[cfg] warm start: {path} row {row} (line {lineno})  {show(got)}"]
    recorded = score_in_comment(comment)
    lines.append(
        f"[cfg] warm start: scores {state.score:.4f} under the weights in force"
        + (f"; the file records {recorded:.4f}" if recorded is not None else ""))
    if want is None:
        lines.append("[cfg] warm start: this row carries no counts to cross-check against")

    hard = hard_penalty(state.evals, state.inward_tally, inner_capacity, config)
    if hard > 0.0:
        lines.append(f"[warn] warm start: the input row is not a usable border "
                     f"(hard={hard:g}); the search has to climb out of that first, "
                     f"and cannot promise to beat it")
        return lines, None
    lines.append("[cfg] warm start: corners kept as given and never swapped; "
                 "every restart refines this same border")
    return lines, state.score

def plan_rows(path: str, rows: Sequence[Tuple[List[int], Optional[str], int]],
              config: AnnealingConfig, pieces_by_id: Dict[int, Piece],
              corner_ids: List[int], edge_ids: List[int], inner_capacity: Counter,
              t0_given: Optional[float], tf_given: Optional[float]) -> List[RowPlan]:
    """Every row of a whole-file refinement, checked and given its schedule
    before any restart runs.

    Each row gets exactly what `--row N` would give it -- the same starting
    state, the same probe stream, so the same T0/Tf -- and its restarts then
    draw the same seeds, which is what makes any row of the result
    reproducible on its own. A row that fails a check stops the run here,
    where it has cost nothing: the probe is ~0.08s a row, and no restart
    starts until every row has passed."""
    seed  = config.random_seed
    plans: List[RowPlan] = []
    for row, (spins, comment, lineno) in enumerate(rows):
        cfg = replace(config, start_spins=tuple(spins), swap_corners=False)
        try:
            state = initial_state(pieces_by_id, corner_ids, edge_ids,
                                  random.Random(restart_seed(seed, 1)), cfg, inner_capacity)
        except SystemExit as e:
            # border_from_spins names the piece; in a whole file, say the row.
            raise SystemExit(f"[ERROR] {path} row {row} (line {lineno}): "
                             f"{str(e.code).removeprefix('[ERROR] ')}")
        report, baseline = warm_start_report(path, row, lineno, comment,
                                             state, cfg, inner_capacity)
        if cfg.decker_sides:
            t0, tf, sched, kind, baseline = decker_warm_plan(
                cfg, pieces_by_id, corner_ids, edge_ids, inner_capacity,
                t0_given, tf_given)
        else:
            t0, tf, sched, kind = resolve_schedule(
                cfg, state, pieces_by_id, edge_ids, inner_capacity,
                random.Random(restart_seed(seed, 0)), t0_given, tf_given)
        plans.append(RowPlan(row=row, lineno=lineno, config=replace(cfg, T0=t0, Tf=tf),
                             start=state.euler_counts(), baseline=baseline, kind=kind,
                             checked=counts_in_comment(comment) is not None,
                             header=report + sched))
    return plans

def rows_header(path: str, plans: Sequence[RowPlan], config: AnnealingConfig,
                overridden: bool) -> List[str]:
    """The header of a whole-file refinement: what every row has in common,
    plus any row that needs saying out loud. The per-row detail -- counts,
    score, schedule -- goes with each row under --verbose."""
    kinds = Counter(p.kind for p in plans)
    lines = [f"[cfg] warm start: {path}, all {len(plans)} rows, best of "
             f"{config.restarts} restart(s) each; one row written back per input "
             f"row, in input order",
             "[cfg] warm start: every row runs exactly as --row N would; corners "
             "kept as given and never swapped"]
    unchecked = sum(not p.checked for p in plans)
    if unchecked:
        lines.append(f"[cfg] warm start: {unchecked} row(s) carry no counts to "
                     f"cross-check against")
    lines.append(f"[cfg] schedule: per row -- polish {kinds['polish']}, "
                 f"search {kinds['search']}"
                 + ("; overridden on the command line" if overridden else ""))
    for p in plans:
        lines.extend(f"[warn] row {p.row}: {l.removeprefix('[warn] ')}"
                     for l in p.header if l.startswith("[warn]"))
    return lines

def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    if args.row is not None and not args.input:
        raise SystemExit("[ERROR] --row picks a row of --input; give --input as well")
    if args.input and args.fix_corners is not None:
        raise SystemExit(
            "[ERROR] --fix_corners and --input both decide where the corners go. "
            "A warm start keeps the corner seats its input row came with.")
    row      = args.row
    all_rows = bool(args.input) and row is None
    out_path = args.out or default_out_path(args.input)

    decker_sides = parse_decker_sides(args.double_decker) if args.double_decker else ()
    warmup_given = getattr(args, "decker_warmup", None)
    reserve_given = getattr(args, "decker_reserve", None)
    if not decker_sides and (args.decker_out or warmup_given is not None
                             or reserve_given is not None or args.decker_keep_border):
        raise SystemExit("[ERROR] --decker_out, --decker_warmup, --decker_reserve and "
                         "--decker_keep_border belong to --double_decker")
    reserve = reserve_given if reserve_given is not None else AnnealingConfig.decker_reserve
    if not 12 <= reserve <= 32:
        raise SystemExit("[ERROR] --decker_reserve must lie in 12..32: a strip has 12 "
                         "cells, and the exact count grows too costly past 32")
    warmup = warmup_given if warmup_given is not None else AnnealingConfig.decker_warmup
    if not 0.0 < warmup < 1.0:
        raise SystemExit("[ERROR] --decker_warmup must lie strictly between 0 and 1")
    decker_out = (args.decker_out or default_decker_path(out_path)) if decker_sides else None

    seed = args.random_seed if args.random_seed != 0 else random.randint(1_000_000, 9_999_999)
    # Resolve 0 here, like the seed, so the value the header reports is the
    # value the run actually used.
    threads = args.threads if args.threads > 0 else (os.cpu_count() or 1)

    pieces = read_pieces(args.seed_file)
    pieces_by_id = {p.id: p for p in pieces}
    corner_ids, edge_ids = classify_boundary_pieces(pieces)
    inner_capacity = build_inner_capacity(pieces)

    rows: List[Tuple[List[int], Optional[str], int]] = []
    start_spins: Optional[Tuple[int, ...]] = None
    comment: Optional[str] = None
    lineno = 0
    provenance = ""
    if args.input:
        rows = read_rotations(args.input)
        if not rows:
            raise SystemExit(f"[ERROR] --input {args.input} holds no data row")
    if row is not None:
        if not 0 <= row < len(rows):
            raise SystemExit(
                f"[ERROR] --row {row} is outside {args.input}: it holds {len(rows)} "
                f"data row(s), numbered 0..{len(rows) - 1}")
        spins, comment, lineno = rows[row]
        start_spins = tuple(spins)
        provenance = f"  From={args.input}:row{row}"

    config = AnnealingConfig(
        restarts             = args.restarts,
        steps_per_restart    = args.steps_per_restart,
        random_seed          = seed,
        threads              = threads,
        verbose              = args.verbose,
        w_top                = args.w_top,
        w_right              = args.w_right,
        w_bottom             = args.w_bottom,
        w_left               = args.w_left,
        tabu_length          = args.tabu_length,
        fix_corners          = args.fix_corners or 0,
        target_scale         = args.target_scale,
        start_spins          = start_spins,
        swap_corners         = not args.input,
        decker_sides         = decker_sides,
        decker_reserve       = reserve,
        decker_keep_border   = args.decker_keep_border,
        decker_warmup        = warmup,
        decker_T0            = args.T0,
        decker_Tf            = args.Tf,
    )

    # A target of w_side x scale only means something for a positive weight.
    if config.target_scale:
        bad = [SIDE_NAMES[s] for s in Side if config.w_side(s) <= 0]
        if bad:
            raise SystemExit(
                f"--target_scale needs a positive weight per side to set a target; "
                f"got <= 0 for {', '.join(bad)}"
            )

    header_extra: List[str] = []
    if not args.out:
        header_extra.append(f"[cfg] --out not given: appending to {out_path}")
    marker = (f"# run {time.strftime('%Y-%m-%d %H:%M:%S')}  "
              f"seed={config.random_seed} restarts={config.restarts} "
              f"steps={config.steps_per_restart} ")
    if decker_sides:
        names = ",".join(SIDE_NAMES[Side(s)] for s in decker_sides)
        blocked = [CORNER_NAMES[k] for k, g in DD_CORNERS.items()
                   if g.a_side in decker_sides or g.b_side in decker_sides]
        header_extra.append(
            f"[cfg] double decker: {names} two tall, {reserve} inner pieces reserved "
            f"each; corner blocks at {' '.join(blocked)}"
            + ("; border kept" if args.decker_keep_border else "")
            + ("" if args.input else
               f"; each restart spends {warmup:.0%} of its steps in a classic warm-up"))
        header_extra.append(f"[cfg] double decker: witness boards -> {decker_out}")
        marker = marker.replace(
            "# run ", f"# run double_decker={names} reserve={reserve}"
                      + (" keep_border" if args.decker_keep_border else "") + " ", 1)

    if all_rows:
        plans = plan_rows(args.input, rows, config, pieces_by_id, corner_ids,
                          edge_ids, inner_capacity, args.T0, args.Tf)
        header_extra.extend(rows_header(args.input, plans, config,
                                        args.T0 is not None or args.Tf is not None))
        temps = " ".join(f"{k}=per-row" if v is None else f"{k}={v:g}"
                         for k, v in (("T0", args.T0), ("Tf", args.Tf)))
        open_out(out_path, marker + temps + f" input={args.input} rows=all({len(plans)})")
        if decker_out:
            open_out(decker_out, marker + temps + f" input={args.input} rows=all({len(plans)})")
        signal.signal(signal.SIGINT, _request_stop)
        run_rows(pieces, plans, config, out_path, args.input, header_extra,
                 decker_out=decker_out)
        return 0

    # The starting state, and then the schedule it implies. Both are settled
    # here, once, in the parent: a worker is handed numbers, never a decision,
    # so the header, every restart and the --out marker cannot disagree. The
    # probe draws on its own stream (restart index 0, which no restart uses),
    # and the state is built from restart 1's, so --rng_seed alone fixes them.
    state = initial_state(pieces_by_id, corner_ids, edge_ids,
                          random.Random(restart_seed(seed, 1)), config, inner_capacity)

    baseline: Optional[float] = None
    if start_spins is not None:
        lines, baseline = warm_start_report(args.input, row, lineno, comment,
                                            state, config, inner_capacity)
        header_extra.extend(lines)

    if decker_sides and start_spins is not None:
        # A warm double decker: seeded here exactly as each restart will seed
        # it, so the schedule and the no-loss baseline are the witness's own.
        t0, tf, sched_lines, _, baseline = decker_warm_plan(
            config, pieces_by_id, corner_ids, edge_ids, inner_capacity, args.T0, args.Tf)
    else:
        # Classic -- or a cold double decker, whose classic warm-up runs on
        # this schedule and whose own phase is probed per restart.
        t0, tf, sched_lines, _ = resolve_schedule(
            config, state, pieces_by_id, edge_ids, inner_capacity,
            random.Random(restart_seed(seed, 0)), args.T0, args.Tf)
    header_extra.extend(sched_lines)
    config = replace(config, T0=t0, Tf=tf)

    marker += f"T0={config.T0:g} Tf={config.Tf:g}"
    if args.input:
        marker += f" input={args.input} row={row}"
    open_out(out_path, marker)
    if decker_out:
        open_out(decker_out, marker)

    # Ctrl-C: let the workers finish the restart they are in and keep whatever
    # has already been written, instead of losing every in-flight restart.
    signal.signal(signal.SIGINT, _request_stop)
    run_annealing(pieces, config, out_path=out_path, provenance=provenance,
                  baseline=baseline, header_extra=header_extra, decker_out=decker_out)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
