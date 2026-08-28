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
                       feasibility, then the signed log-count takes over
      --target_scale N  drive every side toward its own target of w_side * N
                       trails -- the preferred mode when Stage B needs all
                       four sides healthy at once. Each side is scored on how
                       many DECADES it sits from its target, so the penalty
                       depends on the ratio alone: a side 2x off costs the
                       same whether its target is 250 or 15000, and a big
                       side cannot drown out a small one. 100 points = every
                       side on target, 25 points lost per decade per side.

OUTPUT

    With --out FILE, every restart's best border is appended to a rotations
    CSV that Stage B reads directly: one `#` comment line with the per-side
    trail counts, then `id, spin[0..255]` (the 60 border spins from the
    search, zeros for the 196 inner pieces). That file is the deliverable and
    is written in both output modes.

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
    emits every restart in restart order however the workers finish.

USAGE

    python3 -u E555_edge_annealer.py seed_Edge5.txt --out rotations.csv \
      --restarts 10 --steps 100000 --w_bottom -3 --w_left -1 --w_right 3 --w_top 5

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
import time
import random
from collections import Counter, defaultdict, deque
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field, asdict
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
    # Both objectives score in "points": 100 = every side exactly on target.
    # The move sizes that set these two are NOT the objective's: a random edge
    # swap almost always unbalances the degrees of both sides it touches, so
    # the median worsening move is the ~120-point fall off the feasibility
    # cliff, while moves inside the feasible region cost single digits.
    # T0 therefore has to be far above the cliff to keep the walk mobile (best
    # boards are harvested from every candidate evaluated, not only from
    # accepted ones), and Tf far below it but comparable to the objective, so
    # the run ends polishing inside the feasible region instead of frozen.
    # Measured over 8 seeds x 60k steps: (2000, 1.0) beat (12, 0.25), (100, 2),
    # (400, 5), (500, 1), (5000, 1) and the old (10000, 0.01) on both mean and
    # best score. Tf is the sensitive one -- 0.5 and 2.0 both lose ~5 points.
    T0:                  float = 1000.0    # initial temperature
    Tf:                  float =    8.0    # final temperature
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

    # Print the whole search (config dump, per-step reports, BEST lines)
    # instead of one summary line per restart.
    verbose: bool = False

    def w_side(self, side: Side) -> float:
        return {Side.TOP: self.w_top, Side.RIGHT: self.w_right,
                Side.BOTTOM: self.w_bottom, Side.LEFT: self.w_left}[side]

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

def evaluate_side(side: Side, arc_list: List[SideArc], start: int, end: int) -> SideEvaluation:
    graph = [(a.source_color, a.target_color) for a in arc_list]
    penalty, out, inn = _euler_balance(graph, start, end)
    relevant = {start, end} | {c for a in graph for c in a}
    conn = _weakly_connected(graph, relevant)
    ec = count_euler_trails(graph, start, end) if (penalty == 0 and conn) else 0
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
        se = evals[side]
        hard += config.balance_penalty_weight * se.balance_penalty
        if se.balance_penalty == 0 and not se.weakly_connected:
            hard += config.disconnected_penalty_weight
        if se.balance_penalty == 0 and se.weakly_connected and se.euler_count == 0:
            hard += config.infeasible_euler_penalty_weight

    for color in set(inner_capacity) | set(inward_tally):
        if color == 0:
            continue
        ic = inner_capacity.get(color, 0)
        be = inward_tally.get(color, 0)
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

    sides_pool = [Side.TOP]*14 + [Side.RIGHT]*14 + [Side.BOTTOM]*14 + [Side.LEFT]*14
    rng.shuffle(sides_pool)
    shuffled_e = edge_ids[:]
    rng.shuffle(shuffled_e)
    edge_side = {pid: side for pid, side in zip(shuffled_e, sides_pool)}

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
    new_score = compute_score(new_evals, state.inward_tally, inner_capacity, config)

    return CornerSwapResult(new_score=new_score, new_endpoints=new_endpoints, new_evals=new_evals)

def commit_corner_swap(state: RunState, pid_a: int, pid_b: int, r: CornerSwapResult) -> None:
    state.corner_pos[pid_a], state.corner_pos[pid_b] = state.corner_pos[pid_b], state.corner_pos[pid_a]
    state.endpoints = r.new_endpoints
    state.evals = r.new_evals
    state.score = r.new_score

# =============================================================================
# Rotation-vector computation (for output)
# =============================================================================

def rotation_vector(pieces_by_id: Dict[int, Piece], state: RunState, n: int = 60) -> List[int]:
    rots: Dict[int, int] = {}
    for pid, c in state.corner_pos.items():
        rots[pid] = find_rotation_for_corner(pieces_by_id[pid], c)
    for pid, side in state.edge_side.items():
        rots[pid] = find_rotation_for_edge_side(pieces_by_id[pid], side)
    return [rots[i] for i in range(1, n + 1)]

def rotation_vector_with_swap(pieces_by_id: Dict[int, Piece], state: RunState,
                               pid_a: int, pid_b: int,
                               side_a: Side, side_b: Side,
                               n: int = 60) -> List[int]:
    """Rotation vector as if pid_a→side_b, pid_b→side_a were committed."""
    rots: Dict[int, int] = {}
    for pid, c in state.corner_pos.items():
        rots[pid] = find_rotation_for_corner(pieces_by_id[pid], c)
    for pid, side in state.edge_side.items():
        effective = side_b if pid == pid_a else (side_a if pid == pid_b else side)
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

def append_rotations(restart: int, rec: BestRecord, out_path: str) -> None:
    """Append a Stage-B-readable rotations row -- the 60 border spins padded
    with zeros to the full 256-spin vector -- under a `#` comment carrying
    the per-side counts. Called only from the parent process, so the comment
    and the row it describes stay adjacent however many workers ran."""
    full = list(rec.rot_vec) + [0] * (256 - len(rec.rot_vec))
    with open(out_path, "a") as f:
        f.write(f"#  {best_counts_str(rec).replace(',', ' ')}  Score={rec.score:.4f}\n")
        f.write(f"r{restart}, " + ",".join(map(str, full)) + "\n")

def print_header(pieces: Sequence[Piece], corner_ids: List[int], edge_ids: List[int],
                 config: AnnealingConfig) -> None:
    print("\n=== E555 edge_annealer ===\n")

    if config.verbose:
        for k, v in asdict(config).items():
            print(f"[cfg] {k} = {v}")
        if config.target_scale:
            targets = "  ".join(f"{SIDE_NAMES[s]}={target_for(s, config):.0f}" for s in Side)
            print(f"[cfg] targets  {targets}   ({TARGET_DECADE_PENALTY:.0f} points per decade off)")
        print(f"[init] pieces={len(pieces)}  corners={corner_ids}  edges={len(edge_ids)}")
        return

    print(f"[cfg] seed={config.random_seed}  "
          f"restarts={config.restarts} x {config.steps_per_restart} steps  "
          f"threads={config.threads}  T0={config.T0:g} Tf={config.Tf:g}  "
          f"tabu={config.tabu_length}  fix_corners={config.fix_corners}")
    if config.target_scale:
        targets = " ".join(f"{SIDE_NAMES[s]}={target_for(s, config):.0f}" for s in Side)
        print(f"[cfg] objective=target_scale({config.target_scale})  targets {targets}"
              f"  ({TARGET_DECADE_PENALTY:.0f} points per decade off)")
    else:
        weights = " ".join(f"{SIDE_NAMES[s]}={config.w_side(s):g}" for s in Side)
        print(f"[cfg] objective=log-sum  weights {weights}")
    print(f"[init] {len(pieces)} pieces: {len(corner_ids)} corners, "
          f"{len(edge_ids)} edges, {len(pieces) - len(corner_ids) - len(edge_ids)} inner")

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
    log: List[str] = []
    t0  = time.perf_counter()
    rng = random.Random(restart_seed(config.random_seed, restart))

    state = make_random_state(pieces_by_id, corner_ids, edge_ids, rng, config, inner_capacity)

    best: Optional[BestRecord] = None
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

        allow_corners = (config.fix_corners == 0 and progress < 0.20)
        do_corner     = allow_corners and (rng.random() >= config.p_edge)
        evaluated     = True   # did this step actually score a candidate?

        if do_corner:
            pid_a, pid_b = rng.sample(corner_ids, 2)
            cr = try_corner_swap(state, pieces_by_id, pid_a, pid_b, inner_capacity, config)
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
                        rv = rotation_vector_with_swap(
                            pieces_by_id, state, pid_a, pid_b, er.side_a, er.side_b
                        )
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

    return RestartResult(restart=restart, best=best, log=log, elapsed=elapsed)

_STOP = False
def _request_stop(signum, frame):
    """Ctrl-C: stop after the restart in flight. Restarts are independent, so
    everything already appended to --out is complete and usable -- losing it
    all because the run was interrupted is pure waste."""
    global _STOP
    _STOP = True
    print("\n[Ctrl-C] finishing the current restart then stopping...", flush=True)

def run_annealing(pieces: Sequence[Piece], config: AnnealingConfig,
                  out_path: Optional[str] = None) -> None:
    pieces_by_id  = {p.id: p for p in pieces}
    corner_ids, edge_ids = classify_boundary_pieces(pieces)
    inner_capacity = build_inner_capacity(pieces)

    print_header(pieces, corner_ids, edge_ids, config)

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
    champion: Optional[RestartResult] = None

    def consume(res: RestartResult) -> None:
        """Everything that reaches stdout or the --out file happens here, in
        the parent, one restart at a time and in restart order."""
        nonlocal work_time, feasible, champion
        if res.log:
            print("\n".join(res.log), flush=True)
        work_time += res.elapsed
        if res.best is not None:
            feasible += 1
            if champion is None or res.best.score > champion.best.score:
                champion = res
            if out_path:
                append_rotations(res.restart, res.best, out_path)

    if workers > 1:
        print(f"[par] {config.restarts} restarts on {workers} worker processes")
    if not config.verbose:
        print()          # verbose restart blocks open with their own blank line

    stopped = False
    if workers == 1:
        for restart in range(1, config.restarts + 1):
            if _STOP: stopped = True; break
            consume(task(restart))
    else:
        with ProcessPoolExecutor(max_workers=workers) as ex:
            # map yields in submission order, so the restarts are reported in
            # order whatever order the workers actually finished them in. One
            # slow restart therefore delays the REPORTING of the ones after it,
            # not their execution.
            for res in ex.map(task, range(1, config.restarts + 1)):
                consume(res)
                if _STOP: stopped = True; break

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
    if champion is not None:
        ec = champion.best.euler_counts
        print(f"[sum] best: restart {champion.restart}  score={champion.best.score:.4f}  "
              f"TOP={ec[Side.TOP]} RIGHT={ec[Side.RIGHT]} "
              f"BOTTOM={ec[Side.BOTTOM]} LEFT={ec[Side.LEFT]}")
    if out_path:
        print(f"[sum] rotations appended to {out_path}")

# =============================================================================
# CLI
# =============================================================================

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="E555 edge annealer -- Stage A Eternity II border optimizer",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("seed_file", help="4-integer-per-line piece file")
    p.add_argument("--out", default=None, metavar="FILE",
                   help="append each restart's best border to this rotations "
                        "CSV in the format Stage B reads (id + 256 spins)")
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
    g.add_argument("--T0",       type=float, default=AnnealingConfig.T0,
                   help="Initial temperature")
    g.add_argument("--Tf",       type=float, default=AnnealingConfig.Tf,
                   help="Final temperature")

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
                   default=AnnealingConfig.fix_corners, dest="fix_corners",
                   help="0=random  1=edge-commutativity  2=corner-commutativity")
    g.add_argument("--target_scale",  type=int, default=AnnealingConfig.target_scale,
                   dest="target_scale",
                   help="Score every side on how far its trail count sits from "
                        "its own target of w_side x SCALE, measured in decades, "
                        "so all four sides matter equally no matter how large "
                        "their targets are (omit for the linear objective)")

    return p

def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    seed = args.random_seed if args.random_seed != 0 else random.randint(1_000_000, 9_999_999)
    # Resolve 0 here, like the seed, so the value the header reports is the
    # value the run actually used.
    threads = args.threads if args.threads > 0 else (os.cpu_count() or 1)

    config = AnnealingConfig(
        restarts             = args.restarts,
        steps_per_restart    = args.steps_per_restart,
        random_seed          = seed,
        threads              = threads,
        verbose              = args.verbose,
        T0                   = args.T0,
        Tf                   = args.Tf,
        w_top                = args.w_top,
        w_right              = args.w_right,
        w_bottom             = args.w_bottom,
        w_left               = args.w_left,
        tabu_length          = args.tabu_length,
        fix_corners          = args.fix_corners,
        target_scale         = args.target_scale
    )

    # A target of w_side x scale only means something for a positive weight.
    if config.target_scale:
        bad = [SIDE_NAMES[s] for s in Side if config.w_side(s) <= 0]
        if bad:
            raise SystemExit(
                f"--target_scale needs a positive weight per side to set a target; "
                f"got <= 0 for {', '.join(bad)}"
            )

    pieces = read_pieces(args.seed_file)

    if args.out:
        # Fail now, not after the first restart has already been computed: a
        # bad path or an unwritable directory used to surface minutes in, with
        # the work already done and nowhere to put it. Opening in append mode
        # creates the file if needed and leaves an existing one untouched.
        try:
            with open(args.out, "a") as fh:
                # Rows accumulate across runs, which is deliberate -- several
                # short runs build one border pool. Without a marker there is
                # no way to tell afterwards which rows came from which run.
                fh.write(f"# run {time.strftime('%Y-%m-%d %H:%M:%S')}  "
                         f"seed={config.random_seed} restarts={config.restarts} "
                         f"steps={config.steps_per_restart}\n")
        except OSError as e:
            raise SystemExit(f"[ERROR] cannot write --out {args.out}: {e}")

    # Ctrl-C: let the workers finish the restart they are in and keep whatever
    # has already been written, instead of losing every in-flight restart.
    signal.signal(signal.SIGINT, _request_stop)
    run_annealing(pieces, config, out_path=args.out)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
