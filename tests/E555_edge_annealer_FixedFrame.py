#!/usr/bin/env python3
"""
E555_edge_annealer_FixedFrame.py -- an EXPERIMENT, not a pipeline stage.

A copy of src/A_border/E555_edge_annealer.py that (a) pins the four corner
pieces to the frame the fixed-frame corner study was measured in, and (b) adds
that study's measured border preferences to the objective.

WHY

    Stage A currently optimises one thing: the number of Euler trails each side
    admits, which is exactly the number of orderings Stage B gets to enumerate
    for that side. That is a real quantity and worth maximising -- but among the
    astronomically many assignments that score well on it, nothing has been
    choosing. The corners were arbitrary and the piece-to-side assignment was
    decided by trail count alone.

    The corner study now has something to say about both. It ran the beamer in
    ONE pinned frame from all four sides, canonicalised 55,712 partials onto that
    frame, and measured where each piece lands in boards that survived to row 10.
    tests/E555_border_prior.py reduces that corpus to the only question Stage A
    asks -- which of the 56 edge pieces belong on which side -- and this program
    puts the answer in the objective next to the trail count.

    The prior transfers here and nowhere else. A zone prior measured in one frame
    is meaningless on a board in another, and no board in data/ is in this one.
    But the ANNEALER BUILDS THE FRAME: pin the same corners the study pinned and
    the table applies verbatim, with nothing to re-key. That is the whole reason
    this is Stage A and not Stage C.

WHAT IS PINNED

    --canon_BL/BR/TL/TR, defaulting to BL=3 BR=2 TL=0 TR=1 in the beamer's
    0-based piece numbering -- the same four flags, the same four numbers, as
    tests/E555_beamer_FixedFrame.c. Corner swaps are therefore off: the corner
    assignment is an input, not a search variable.

    It is also a BET. No clue pins a corner, so the four corner pieces can fill
    the four corners 4! = 24 ways and exactly one of them is the solution's. Every
    border this program emits, and every number in the prior, is conditional on
    that choice. Change all four flags together and re-measure the prior, or
    change neither.

THE OBJECTIVE, IN THREE PARTS, ALL IN THE SAME 100-POINT UNIT

    trails     what the original program optimises: each side scored on how many
               DECADES its Euler-trail count sits from its own target of
               w_side x --target_scale. 100 = every side on target, 25 points per
               decade per side. Feasibility is hard: a side with no Euler trail,
               or an inner-colour inventory the border starves, scores below the
               band and cannot be recorded.

    affinity   100 x (A - A_random) / (A_optimum - A_random), where A is the sum
               over the 56 edge pieces of the log10 lift of the side it sits on.
               0 means "no better than dealing the pieces out at random"; 100
               means the best assignment the prior admits, which is what you
               would get if trail count did not matter at all. Both constants are
               measured by the prior builder and carried in the prior file --
               A_optimum by an exact transportation DP, not a bound.

    spread     100 x (1 - C / C_random), where C is CROWDING, in pieces: a side
               has 4 cells near one corner, 6 in the middle and 4 near the other,
               and each of its 14 pieces has a measured opinion about which. C is
               the transport distance between what the side's pieces want and
               what the side has to offer. C = 0 means every preference can be
               honoured; C = 2 means two pieces' worth cannot be, whatever
               ordering Stage B picks.

               This term is the one that could not be guessed. A side made
               entirely of pieces that all want the bottom-left end scores well
               on affinity and is useless, because only four of them can have it.
               The within-side signal is also the better measured of the two:
               cross-view Spearman +0.65 against +0.38 for the side aggregate.

    score = trails + --w_affinity x affinity + --w_spread x spread

    Set both weights to 0 and this program is the original one with pinned
    corners. There is no mode in which the prior overrides feasibility: the hard
    penalties are applied first and a border that fails them is never recorded.

OUTPUT

    The same rotations CSV Stage B reads -- `id, spin[0..255]` under a `#`
    comment -- so it drops straight into bin/E555_beamer_FixedFrame with a
    matching --clue_orient. The comment now carries the frame and all three
    score components, so a pool file says what each row was selected for.

    --pool N keeps the N best DISTINCT borders per restart instead of only the
    best. Restarts remain the real source of diversity: each is an independent
    walk from its own seed, and the top N of one walk are mostly the same border.

USAGE

    python3 tests/E555_border_prior.py CORPUS.csv --out_dir ff_out/prior

    python3 -u tests/E555_edge_annealer_FixedFrame.py data/seed_Edge5.txt \
        --prior ff_out/prior/border_prior.txt \
        --out borders_ff.csv --restarts 16 --steps 200000 --threads 8

    python3 tests/check_frame_border.py data/seed_Edge5.txt borders_ff.csv \
        --prior ff_out/prior/border_prior.txt

    bash tests/run_fixedframe_border.sh          # the whole loop, and the A/B
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

# The canonical assignment, in the SAME 0-based piece numbering the beamer and
# the study use: role order BL, BR, TL, TR. The annealer numbers pieces from 1
# (piece id = row of the seed file), so these are converted on use and nowhere
# else -- one place to get it wrong instead of four.
CANON_ROLE_ORDER = (Corner.BL, Corner.BR, Corner.TL, Corner.TR)
CANON_CORNER_DEFAULT = (3, 2, 0, 1)


def get_fixed_corner_mapping(option: int, sorted_c: List[int],
                             canon: Optional[Sequence[int]] = None) -> Dict[int, Corner]:
    """piece id -> corner, for the three fixed-corner modes.

    Modes 1 and 2 are the original program's: pick corners by commutativity, an
    arbitrary but reproducible rule. Mode 3 is this program's reason to exist --
    the corner assignment the fixed-frame study measured its prior in, named by
    piece rather than derived from one, so it matches
    `--canon_BL/BR/TL/TR` on bin/E555_beamer_FixedFrame exactly.
    """
    if option == 3:
        c = list(CANON_CORNER_DEFAULT if canon is None else canon)
        if sorted(c) != sorted(x - 1 for x in sorted_c):
            raise SystemExit(
                f"--canon_BL/BR/TL/TR name pieces {sorted(c)} (0-based), but the "
                f"seed file's corner pieces are {sorted(x-1 for x in sorted_c)}. "
                f"These flags name PIECES, not positions.")
        return {c[i] + 1: role for i, role in enumerate(CANON_ROLE_ORDER)}
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

    # Move mix: fraction that are edge swaps (remainder = corner swaps, first 20%
    # only). Inert at the default fix_corners=3: with the corners pinned every
    # move is an edge swap, and this only comes back into play under
    # --fix_corners 0, where there is no prior to be consistent with anyway.
    p_edge: float = 0.90

    # Tabu list length on edge-swap pairs (0 = disabled)
    tabu_length: int = 128

    # Corner mode: 0 = random, 1 = fix by edge-commutativity, 2 = fix by
    # corner-commutativity, 3 = the canonical frame named by canon_corner.
    # 3 is the default HERE and nowhere else: a prior measured in one frame is
    # meaningless under another, so the program that reads the prior pins the
    # frame by default rather than on request.
    fix_corners: int = 3
    # Role order BL, BR, TL, TR, in 0-based piece numbering -- the beamer's.
    canon_corner: Tuple[int, int, int, int] = CANON_CORNER_DEFAULT

    # Per-side trail target = w_side * target_scale (None/0 = linear objective).
    # Target mode is the default here because the prior terms below are scored in
    # POINTS, and only target mode puts the trail term on the same 100-point
    # scale. Under the linear objective the trail term is a weighted mean of
    # natural logs -- single digits -- and a 100-point prior would swallow it, so
    # --target_scale 0 also demands that the prior weights be scaled to match.
    #
    # 1000, not the 250 this was first tried at, and the reason is worth knowing:
    # the target term penalises a side for OVERSHOOTING as hard as for falling
    # short, and the prior pushes sides past a low target. Measured, 6 restarts
    # x 120k steps, --w_spread 1 throughout:
    #
    #   target  w_aff   trails    aff   weakest side   sum of 4
    #      250      0     88.0   11.6            192       1980
    #      250      3     84.7   24.7            288       2304
    #     1000      0     90.1   15.1            432       4056
    #     1000      3     94.3   26.6            648       4704   <- the default
    #     4000      0     74.0   18.8            684       6360
    #     4000      3     84.2   34.7            912       8112
    #
    # At 250 the prior costs 3.3 trail points. At 1000 and 4000 it BUYS them --
    # 94.3 against 90.1, and more trails on every side as well. So the two
    # objectives were never really in conflict; a target low enough to punish a
    # rich border was. Raise it further to trade balance for raw breadth.
    target_scale: int = 1000

    # Prior weights, in points. 0 disables a term outright: with both at 0 this
    # program is the original annealer with its corners pinned.
    #
    # These two came out of a sweep, 6 restarts x 120k steps per setting, all at
    # --target_scale 250. Reading the trail term against what it buys:
    #
    #   w_aff w_spr   trails    aff   spread
    #       0     0     92.6    7.8     42.1     <- the original objective
    #       0     3     83.2   17.9     87.7
    #       1     1     82.1   19.1     74.2
    #       3     1     84.7   24.7     69.3     <- the default
    #       3     3     80.2   22.7     84.7
    #      10     0     41.0   41.4     27.2
    #      30     0     10.0   46.8      9.0
    #
    # Spread is nearly free -- it costs a few trail points and drags affinity up
    # with it, because a side whose pieces all want the same end is also a side
    # whose pieces came from the same corner. Affinity is not: past w=10 the
    # search abandons the trail targets to chase it, and 47 points is where it
    # saturates anyway. Euler feasibility, not the weight, is what bounds it.
    w_affinity: float = 3.0
    w_spread:   float = 1.0

    # Best DISTINCT borders kept per restart (1 = the original behaviour).
    pool: int = 1

    # Print the whole search (config dump, per-step reports, BEST lines)
    # instead of one summary line per restart.
    verbose: bool = False

    def w_side(self, side: Side) -> float:
        return {Side.TOP: self.w_top, Side.RIGHT: self.w_right,
                Side.BOTTOM: self.w_bottom, Side.LEFT: self.w_left}[side]

# =============================================================================
# The measured border prior
# =============================================================================

NEUTRAL_OCC = tuple(c / 14.0 for c in (4, 6, 4))


@dataclass(frozen=True)
class BorderPrior:
    """What tests/E555_border_prior.py measured, as this program needs it.

    Indexed by the annealer's own Side enum, never by the file's column order:
    the file names its columns and the loader maps them, so the two orderings
    are free to differ and a reordered file cannot silently transpose the table.

    Piece ids are the annealer's 1-based ones. The file uses the beamer's 0-based
    numbering -- the conversion happens in the loader and nowhere else.
    """
    path:        str
    orient:      int
    canon:       Tuple[int, int, int, int]        # BL BR TL TR, 0-based pieces
    cap:         Tuple[int, int, int]             # cells per within-side bucket
    aff:         Dict[int, Tuple[float, ...]]     # pid -> affinity by Side
    occ:         Dict[int, Tuple[Tuple[float, ...], ...]]   # pid -> 3 per Side
    aff_random:  float
    aff_optimum: float
    tv_random:   float

    def a(self, pid: int, side: Side) -> float:
        row = self.aff.get(pid)
        return 0.0 if row is None else row[int(side)]

    def q(self, pid: int, side: Side) -> Tuple[float, ...]:
        row = self.occ.get(pid)
        return NEUTRAL_OCC if row is None else row[int(side)]


def load_prior(path: str, edge_ids: Sequence[int],
               canon: Sequence[int]) -> BorderPrior:
    """Read a border_prior.txt, and refuse one measured in a different frame.

    The refusal is the important part. Every number in the file is conditional
    on which corner piece sits in which corner; under a different assignment
    they are not merely noisier, they are about a different puzzle. Silently
    optimising against the wrong table would look exactly like optimising
    against the right one.
    """
    name_to_side = {SIDE_NAMES[x]: x for x in Side}
    order: List[Side] = []
    cap = (4, 6, 4)
    orient = 0
    file_canon = tuple(canon)
    aff: Dict[int, Tuple[float, ...]] = {}
    occ: Dict[int, Tuple[Tuple[float, ...], ...]] = {}
    norms: Dict[str, float] = {}

    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            tok = line.split()
            key = tok[0]
            if key == "sides":
                order = [name_to_side[t] for t in tok[1:]]
            elif key == "buckets":
                cap = tuple(int(t) for t in tok[1:])
            elif key == "frame":
                kv = dict(zip(tok[1::2], tok[2::2]))
                orient = int(kv["orient"])
                file_canon = tuple(int(kv[r]) for r in ("BL", "BR", "TL", "TR"))
            elif key == "norm":
                norms = {k: float(v) for k, v in zip(tok[1::2], tok[2::2])}
            elif key in ("aff", "occ"):
                if not order:
                    raise SystemExit(f"{path}:{lineno}: '{key}' before the 'sides' line")
                pid = int(tok[1]) + 1                 # 0-based file -> 1-based here
                vals = [float(t) for t in tok[2:]]
                if key == "aff":
                    if len(vals) != len(order):
                        raise SystemExit(f"{path}:{lineno}: expected {len(order)} affinities")
                    row = [0.0] * 4
                    for j, side in enumerate(order):
                        row[int(side)] = vals[j]
                    aff[pid] = tuple(row)
                else:
                    nb = len(cap)
                    if len(vals) != len(order) * nb:
                        raise SystemExit(f"{path}:{lineno}: expected {len(order)*nb} occupancies")
                    row_q = [NEUTRAL_OCC] * 4
                    for j, side in enumerate(order):
                        row_q[int(side)] = tuple(vals[j * nb:(j + 1) * nb])
                    occ[pid] = tuple(row_q)
            elif key == "ci":
                continue                               # diagnostic only
            else:
                raise SystemExit(f"{path}:{lineno}: unknown record '{key}'")

    if len(order) != 4:
        raise SystemExit(f"{path}: no 'sides' line naming all four sides")
    for k in ("aff_random", "aff_optimum", "tv_random"):
        if k not in norms:
            raise SystemExit(f"{path}: the 'norm' line is missing {k}")
    if norms["aff_optimum"] <= norms["aff_random"]:
        raise SystemExit(f"{path}: aff_optimum must exceed aff_random")
    if norms["tv_random"] <= 0:
        raise SystemExit(f"{path}: tv_random must be positive")

    if file_canon != tuple(canon):
        raise SystemExit(
            f"[frame] {path} was measured with corners BL={file_canon[0]} "
            f"BR={file_canon[1]} TL={file_canon[2]} TR={file_canon[3]}, but this run "
            f"pins BL={canon[0]} BR={canon[1]} TL={canon[2]} TR={canon[3]}.\n"
            f"        A prior is conditional on its corner assignment -- one of "
            f"4! = 24 -- so these numbers do not describe the frame you are "
            f"searching. Re-measure the prior in this frame, or pin that one.")

    missing = [pid for pid in edge_ids if pid not in aff or pid not in occ]
    if missing:
        raise SystemExit(
            f"{path}: no entry for edge piece(s) {missing[:8]}"
            f"{'...' if len(missing) > 8 else ''} "
            f"(the file has {len(aff)} affinity rows; the seed has {len(edge_ids)} edges)")

    return BorderPrior(path=path, orient=orient, canon=tuple(canon), cap=cap,
                       aff=aff, occ=occ, aff_random=norms["aff_random"],
                       aff_optimum=norms["aff_optimum"], tv_random=norms["tv_random"])


def crowding(prior: BorderPrior, hist: Dict[Side, List[float]]) -> float:
    """Preference a border cannot honour however Stage B orders it, in pieces.

    A side offers 4 cells at one end, 6 in the middle and 4 at the other. Its 14
    pieces between them want `hist`. Half the L1 distance between the two is the
    mass that has to move -- the classic transport distance, and here it counts
    whole pieces because both vectors sum to 14.
    """
    tv = 0.0
    for side in Side:
        h = hist[side]
        tv += 0.5 * sum(abs(h[b] - prior.cap[b]) for b in range(len(prior.cap)))
    return tv


def prior_terms(prior: Optional[BorderPrior], aff_sum: float,
                hist: Dict[Side, List[float]]) -> Tuple[float, float]:
    """(affinity points, spread points). 0 = a random border, 100 = the best."""
    if prior is None:
        return 0.0, 0.0
    a = 100.0 * (aff_sum - prior.aff_random) / (prior.aff_optimum - prior.aff_random)
    sp = 100.0 * (1.0 - crowding(prior, hist) / prior.tv_random)
    return a, sp


def prior_bonus(prior: Optional[BorderPrior], config: "AnnealingConfig",
                aff_sum: float, hist: Dict[Side, List[float]]) -> float:
    a, sp = prior_terms(prior, aff_sum, hist)
    return config.w_affinity * a + config.w_spread * sp


def build_prior_state(prior: Optional[BorderPrior],
                      edge_side: Dict[int, Side]) -> Tuple[float, Dict[Side, List[float]]]:
    """The two running totals the prior terms need, from scratch."""
    hist = {side: [0.0] * 3 for side in Side}
    if prior is None:
        return 0.0, hist
    hist = {side: [0.0] * len(prior.cap) for side in Side}
    total = 0.0
    for pid, side in edge_side.items():
        total += prior.a(pid, side)
        q = prior.q(pid, side)
        h = hist[side]
        for b in range(len(h)):
            h[b] += q[b]
    return total, hist


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
    new_aff:    float                      # running affinity total after the swap
    new_hist:   Dict[Side, List[float]]    # per-side occupancy wishlist after it
    bonus:      float                      # the prior's contribution to new_score

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
    # The three parts of `score`, kept apart so a pool file can say what each
    # row was selected FOR -- a border can be here for its trail counts or for
    # its fit to the prior, and the sum alone does not distinguish them.
    trail_pts:  float = 0.0
    aff_pts:    float = 0.0
    spread_pts: float = 0.0
    # Which side each edge piece sits on, as a hashable key. Two borders with
    # the same signature are the same border: the spins are a function of it.
    sig:        Tuple[int, ...] = ()

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
    pool:    List[BestRecord] = field(default_factory=list)   # best first

class BorderPool:
    """The best DISTINCT borders one restart found, capped at `cap`.

    Distinct means a different piece-to-side assignment, not a different score:
    the spins are a function of the assignment, so two borders with the same
    signature are the same row in the output file and keeping both would pad a
    pool with duplicates. A better score for a signature already held replaces
    it rather than adding to it.

    `worth` is the cheap gate. It is asked before the expensive part -- building
    the signature and then the rotation vector -- so a candidate that cannot
    make the pool costs one float comparison.
    """

    def __init__(self, cap: int):
        self.cap = max(1, cap)
        self.by_sig: Dict[Tuple[int, ...], BestRecord] = {}
        self.floor = -math.inf

    def worth(self, score: float) -> bool:
        return len(self.by_sig) < self.cap or score > self.floor

    def add(self, rec: BestRecord) -> None:
        held = self.by_sig.get(rec.sig)
        if held is not None and held.score >= rec.score:
            return
        self.by_sig[rec.sig] = rec
        if len(self.by_sig) > self.cap:
            worst = min(self.by_sig.values(), key=lambda r: r.score)
            del self.by_sig[worst.sig]
        self.floor = (min(r.score for r in self.by_sig.values())
                      if len(self.by_sig) >= self.cap else -math.inf)

    def records(self) -> List[BestRecord]:
        return sorted(self.by_sig.values(), key=lambda r: -r.score)

    def best(self) -> Optional[BestRecord]:
        recs = self.records()
        return recs[0] if recs else None


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
    # Prior bookkeeping, maintained incrementally alongside the arcs. aff_sum is
    # a plain sum over pieces and hist a per-side 3-vector, so an edge swap
    # updates both in constant time -- the prior costs the search nothing.
    aff_sum:      float = 0.0
    hist:         Dict[Side, List[float]] = field(default_factory=dict)
    bonus:        float = 0.0

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
                  hard: Optional[float] = None,
                  bonus: float = 0.0) -> float:
    """The trail objective, plus whatever the prior is worth on this border.

    `bonus` is added only inside the feasible region, and deliberately: an
    infeasible border has no Stage B value however well its pieces are placed,
    and letting the prior lift such a state toward the cliff would make the
    hard penalties negotiable. They are not.
    """
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
        return bonus + 100.0 - TARGET_DECADE_PENALTY * sum(
            math.log10(evals[s].euler_count / target_for(s, config)) ** 2
            for s in Side)

    # Default linear scoring: weighted sum of log trail counts. The divisor is
    # cosmetic -- it only keeps the number readable -- so it must be the sum of
    # the ABSOLUTE weights: signed weights (the point of this mode) can sum to
    # zero, which used to raise ZeroDivisionError, or to a negative, which used
    # to silently inverse the whole objective. The documented shaping example
    # +9/-2/-5/-2 sums to exactly 0.
    total_weight = sum(abs(config.w_side(s)) for s in Side) or 1.0
    return bonus + (1.0 / total_weight) * sum(
        config.w_side(s) * evals[s].log_raw for s in Side)


def trail_points(evals: Dict[Side, SideEvaluation], config: AnnealingConfig) -> float:
    """The trail term alone, for reporting -- score minus the prior's share."""
    if config.target_scale:
        return 100.0 - TARGET_DECADE_PENALTY * sum(
            math.log10(max(1, evals[s].euler_count) / target_for(s, config)) ** 2
            for s in Side)
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
                     config: AnnealingConfig,
                     prior: Optional[BorderPrior] = None) -> RunState:
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
    aff_sum, hist = build_prior_state(prior, edge_side)
    bonus = prior_bonus(prior, config, aff_sum, hist)
    score = compute_score(evals, inward_tally, inner_capacity, config, bonus=bonus)

    return RunState(edge_side=edge_side, corner_pos=corner_pos, arcs=arcs,
                    evals=evals, endpoints=endpoints, inward_tally=inward_tally,
                    score=score, aff_sum=aff_sum, hist=hist, bonus=bonus)

def make_random_state(pieces_by_id: Dict[int, Piece],
                      corner_ids: List[int], edge_ids: List[int],
                      rng: random.Random, config: AnnealingConfig,
                      inner_capacity: Counter,
                      prior: Optional[BorderPrior] = None) -> RunState:
    if config.fix_corners in (1, 2, 3):
        corner_pos = get_fixed_corner_mapping(config.fix_corners, sorted(corner_ids),
                                              config.canon_corner)
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

    return _build_run_state(pieces_by_id, edge_side, corner_pos, inner_capacity,
                            config, prior)

# =============================================================================
# Incremental swap operations
# =============================================================================

def try_edge_swap(state: RunState, pieces_by_id: Dict[int, Piece],
                  pid_a: int, pid_b: int,
                  inner_capacity: Counter,
                  config: AnnealingConfig,
                  prior: Optional[BorderPrior] = None) -> Optional[EdgeSwapResult]:
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
    # The prior after the swap. Two pieces change sides and nothing else does,
    # so both running totals update in place from four table lookups -- the cost
    # does not depend on how many pieces the border has.
    new_aff = state.aff_sum
    new_hist = state.hist
    if prior is not None:
        new_aff = (state.aff_sum
                   - prior.a(pid_a, side_a) - prior.a(pid_b, side_b)
                   + prior.a(pid_a, side_b) + prior.a(pid_b, side_a))
        new_hist = {sd: list(h) for sd, h in state.hist.items()}
        for pid, frm, to in ((pid_a, side_a, side_b), (pid_b, side_b, side_a)):
            q_out, q_in = prior.q(pid, frm), prior.q(pid, to)
            h_out, h_in = new_hist[frm], new_hist[to]
            for b in range(len(h_out)):
                h_out[b] -= q_out[b]
                h_in[b] += q_in[b]

    bonus = prior_bonus(prior, config, new_aff, new_hist)
    hard = hard_penalty(new_evals, new_inward, inner_capacity, config)
    new_score = compute_score(new_evals, new_inward, inner_capacity, config,
                              hard=hard, bonus=bonus)

    return EdgeSwapResult(
        new_score=new_score, hard=hard, side_a=side_a, side_b=side_b,
        new_arcs_a=new_arcs_a, new_arcs_b=new_arcs_b,
        new_se_a=new_se_a, new_se_b=new_se_b,
        new_inward=new_inward,
        new_arc_a=new_arc_a, new_arc_b=new_arc_b,
        new_aff=new_aff, new_hist=new_hist, bonus=bonus,
    )

def commit_edge_swap(state: RunState, pid_a: int, pid_b: int, r: EdgeSwapResult) -> None:
    state.arcs[r.side_a] = r.new_arcs_a
    state.arcs[r.side_b] = r.new_arcs_b
    state.evals[r.side_a] = r.new_se_a
    state.evals[r.side_b] = r.new_se_b
    state.inward_tally = r.new_inward
    state.edge_side[pid_a] = r.side_b
    state.edge_side[pid_b] = r.side_a
    state.aff_sum = r.new_aff
    state.hist = r.new_hist
    state.bonus = r.bonus
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
    # A corner swap moves no edge piece, so the prior's two running totals are
    # unchanged and its contribution carries over untouched.
    new_score = compute_score(new_evals, state.inward_tally, inner_capacity, config,
                              bonus=state.bonus)

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

def sides_signature(edge_ids: Sequence[int], edge_side: Dict[int, Side],
                    pid_a: int = -1, pid_b: int = -1,
                    side_a: Optional[Side] = None,
                    side_b: Optional[Side] = None) -> Tuple[int, ...]:
    """The border's identity: which side each edge piece sits on, in id order.

    Takes the pending swap as arguments for the same reason
    rotation_vector_with_swap does -- best-tracking runs on CANDIDATES, most of
    which are never committed, and materialising them first would cost more than
    the search.
    """
    return tuple(
        int(side_b if pid == pid_a else (side_a if pid == pid_b else edge_side[pid]))
        for pid in edge_ids)


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

def prior_str(prior: Optional[BorderPrior], aff_sum: float,
              hist: Dict[Side, List[float]]) -> str:
    """The two prior terms, as points. Empty when there is no prior, so every
    line in a --w_affinity 0 --w_spread 0 run reads exactly as it used to."""
    if prior is None:
        return ""
    a, sp = prior_terms(prior, aff_sum, hist)
    return f"  aff={a:6.1f}  spread={sp:6.1f}"


def prior_str_rec(rec: BestRecord, prior: Optional[BorderPrior]) -> str:
    if prior is None:
        return ""
    return f"  aff={rec.aff_pts:6.1f}  spread={rec.spread_pts:6.1f}"


def progress_str(prefix: str, score: float, evals: Dict[Side, SideEvaluation],
                 config: Optional[AnnealingConfig] = None,
                 extra: str = "") -> str:
    return f"{prefix}: score={score:10.4f}  {_counts_str(evals, config)}{extra}"

def restart_str(restart: int, config: AnnealingConfig,
                rec: Optional[BestRecord], elapsed: float,
                prior: Optional[BorderPrior] = None) -> str:
    """The default mode's one line per restart: the result, not the search.
    Fixed-width fields, so a run's restarts line up and read as a table
    without needing a header row."""
    w    = len(str(config.restarts))
    head = f"  restart {restart:>{w}}/{config.restarts}"
    if rec is None:
        return f"{head}  no feasible border found  ({elapsed:.1f}s)"
    counts = "  ".join(f"{SIDE_NAMES[s]}={rec.euler_counts[s]:>6}" for s in Side)
    return (f"{head}  score={rec.score:10.4f}  {counts}{prior_str_rec(rec, prior)}"
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

def append_rotations(restart: int, rec: BestRecord, out_path: str,
                     index: int = 0, prior: Optional[BorderPrior] = None) -> None:
    """Append a Stage-B-readable rotations row -- the 60 border spins padded
    with zeros to the full 256-spin vector -- under a `#` comment carrying
    the per-side counts. Called only from the parent process, so the comment
    and the row it describes stay adjacent however many workers ran.

    The comment also carries the three score components, because a pool file
    holds rows chosen for different reasons and the total does not say which:
    two borders on the same line of a sweep can have the same score with one
    rich in trails and the other fitted to the prior."""
    full = list(rec.rot_vec) + [0] * (256 - len(rec.rot_vec))
    parts = f"#  {best_counts_str(rec).replace(',', ' ')}  Score={rec.score:.4f}"
    if prior is not None:
        parts += (f"  Trails={rec.trail_pts:.2f}  Aff={rec.aff_pts:.2f}"
                  f"  Spread={rec.spread_pts:.2f}")
    with open(out_path, "a") as f:
        f.write(parts + "\n")
        f.write(f"r{restart}.{index}, " + ",".join(map(str, full)) + "\n")

def frame_str(config: AnnealingConfig) -> str:
    """The pinned frame, in the beamer's own words, so the two command lines can
    be compared by eye instead of by arithmetic."""
    bl, br, tl, tr = config.canon_corner
    return f"--canon_BL {bl} --canon_BR {br} --canon_TL {tl} --canon_TR {tr}"


def print_header(pieces: Sequence[Piece], corner_ids: List[int], edge_ids: List[int],
                 config: AnnealingConfig,
                 prior: Optional[BorderPrior] = None) -> None:
    print("\n=== E555 edge_annealer (fixed frame) ===\n")
    if config.fix_corners == 3:
        print(f"[frame] corners pinned: {frame_str(config)}   (0-based piece ids;"
              f" pass the same four to bin/E555_beamer_FixedFrame)")
    if prior is not None:
        print(f"[prior] {prior.path}")
        print(f"[prior] headroom {prior.aff_optimum - prior.aff_random:.2f} decades over "
              f"{len(prior.aff)} edge pieces;  a random border crowds "
              f"{prior.tv_random:.2f} pieces")
        print(f"[prior] weights: affinity x{config.w_affinity:g}  spread x{config.w_spread:g}")
    else:
        print("[prior] none: this is the original objective with pinned corners")

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
                       config: AnnealingConfig,
                       prior: Optional[BorderPrior] = None) -> RestartResult:
    """One independent annealing walk. This runs in a worker process, so it
    prints nothing and touches no file: everything it has to say goes into
    the returned log for the parent to replay in restart order."""
    log: List[str] = []
    t0  = time.perf_counter()
    rng = random.Random(restart_seed(config.random_seed, restart))

    state = make_random_state(pieces_by_id, corner_ids, edge_ids, rng, config,
                              inner_capacity, prior)
    sorted_edges = sorted(edge_ids)
    pool = BorderPool(config.pool)

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
        log.append(progress_str("  initial", state.score, state.evals, config,
                                prior_str(prior, state.aff_sum, state.hist)))

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
            er = try_edge_swap(state, pieces_by_id, pid_a, pid_b, inner_capacity,
                               config, prior)
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
                is_new_best  = candidate_ok and pool.worth(er.new_score)

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
                        sig = sides_signature(sorted_edges, state.edge_side,
                                              pid_a, pid_b, er.side_a, er.side_b)
                        held = pool.by_sig.get(sig)
                        if held is None or held.score < er.new_score:
                            rv = rotation_vector_with_swap(
                                pieces_by_id, state, pid_a, pid_b, er.side_a, er.side_b
                            )
                            ec = {
                                er.side_a: er.new_se_a.euler_count,
                                er.side_b: er.new_se_b.euler_count,
                                **{s: state.evals[s].euler_count
                                   for s in Side if s != er.side_a and s != er.side_b},
                            }
                            a_pts, sp_pts = prior_terms(prior, er.new_aff, er.new_hist)
                            pool.add(BestRecord(
                                score=er.new_score, euler_counts=ec, rot_vec=rv,
                                step=step, trail_pts=er.new_score - er.bonus,
                                aff_pts=a_pts, spread_pts=sp_pts, sig=sig))
                            best = pool.best()

        if evaluated:
            step_window += 1
        if config.report_every and step % config.report_every == 0:
            acc_last = accepted_window / step_window
            if config.verbose:
                log.append(
                    f"  step {step:>7}  T={temperature:7.3g}  "
                    f"acc={acc_last:5.1%}  feas={feasible_seen:>5}  "
                    f"score={state.score:10.4f}  {_counts_str(state.evals, config)}"
                    f"{prior_str(prior, state.aff_sum, state.hist)}"
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
                f"{prior_str_rec(best, prior)}"
            )
            log.append(best_line(restart, best))
        else:
            log.append(f"  restart best: no feasible border found")
        log.append(f"  time: {elapsed:.1f}s  "
                   f"({config.steps_per_restart/elapsed:.0f} steps/s)")
    else:
        # One line, then the caveat under it if there is one.
        log.append(restart_str(restart, config, best, elapsed, prior))
        if warning:
            log.append(warning)

    return RestartResult(restart=restart, best=best, log=log, elapsed=elapsed,
                         pool=pool.records())

_STOP = False
def _request_stop(signum, frame):
    """Ctrl-C: stop after the restart in flight. Restarts are independent, so
    everything already appended to --out is complete and usable -- losing it
    all because the run was interrupted is pure waste."""
    global _STOP
    _STOP = True
    print("\n[Ctrl-C] finishing the current restart then stopping...", flush=True)

def run_annealing(pieces: Sequence[Piece], config: AnnealingConfig,
                  out_path: Optional[str] = None,
                  prior: Optional[BorderPrior] = None) -> None:
    pieces_by_id  = {p.id: p for p in pieces}
    corner_ids, edge_ids = classify_boundary_pieces(pieces)
    inner_capacity = build_inner_capacity(pieces)

    print_header(pieces, corner_ids, edge_ids, config, prior)

    # At most one worker per restart: --restarts 1 must not start a pool of 8.
    workers = max(1, min(config.threads, config.restarts))
    task = functools.partial(anneal_one_restart,
                             pieces_by_id=pieces_by_id,
                             corner_ids=corner_ids,
                             edge_ids=edge_ids,
                             inner_capacity=inner_capacity,
                             config=config,
                             prior=prior)

    wall0     = time.perf_counter()
    work_time = 0.0
    feasible  = 0
    written   = 0
    champion: Optional[RestartResult] = None

    def consume(res: RestartResult) -> None:
        """Everything that reaches stdout or the --out file happens here, in
        the parent, one restart at a time and in restart order."""
        nonlocal work_time, feasible, champion, written
        if res.log:
            print("\n".join(res.log), flush=True)
        work_time += res.elapsed
        if res.best is not None:
            feasible += 1
            if champion is None or res.best.score > champion.best.score:
                champion = res
            if out_path:
                for i, rec in enumerate(res.pool):
                    append_rotations(res.restart, rec, out_path, i, prior)
                    written += 1

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
    if config.pool > 1:
        print(f"[sum] {written} border(s) written ({config.pool} kept per restart at most)")
    if champion is not None:
        ec = champion.best.euler_counts
        print(f"[sum] best: restart {champion.restart}  score={champion.best.score:.4f}  "
              f"TOP={ec[Side.TOP]} RIGHT={ec[Side.RIGHT]} "
              f"BOTTOM={ec[Side.BOTTOM]} LEFT={ec[Side.LEFT]}"
              f"{prior_str_rec(champion.best, prior)}")
        if prior is not None:
            print(f"[sum] the best border captures {champion.best.aff_pts:.1f}% of the "
                  f"affinity the prior admits and leaves "
                  f"{(1 - champion.best.spread_pts/100) * prior.tv_random:.2f} "
                  f"piece(s) of preference unplaceable")
    if out_path:
        print(f"[sum] rotations appended to {out_path}")

# =============================================================================
# CLI
# =============================================================================

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="E555 edge annealer, fixed frame -- an EXPERIMENT. Pins the "
                    "corner assignment the fixed-frame study measured in, and "
                    "adds that study's border prior to the trail objective.",
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
    g.add_argument("--fix_corners", type=int, choices=[0, 1, 2, 3],
                   default=AnnealingConfig.fix_corners, dest="fix_corners",
                   help="0=random  1=edge-commutativity  2=corner-commutativity  "
                        "3=the canonical frame from --canon_BL/BR/TL/TR. Anything "
                        "but 3 leaves the frame the prior was measured in, so "
                        "--prior is refused with it")
    g.add_argument("--pool", type=int, default=AnnealingConfig.pool,
                   help="best DISTINCT borders to keep per restart. Restarts are "
                        "still where diversity comes from -- one walk's top N are "
                        "mostly the same border with two pieces exchanged")
    g.add_argument("--target_scale",  type=int, default=AnnealingConfig.target_scale,
                   dest="target_scale",
                   help="Score every side on how far its trail count sits from "
                        "its own target of w_side x SCALE, measured in decades, "
                        "so all four sides matter equally no matter how large "
                        "their targets are (omit for the linear objective)")

    g = p.add_argument_group(
        "the fixed frame (what this program adds)")
    g.add_argument("--prior", default=None, metavar="FILE",
                   help="border prior from tests/E555_border_prior.py. Omit and "
                        "this is the original objective with pinned corners")
    g.add_argument("--w_affinity", type=float, default=AnnealingConfig.w_affinity,
                   help="points for the prior's side preference, 100 = the best "
                        "assignment it admits, 0 = a random one")
    g.add_argument("--w_spread", type=float, default=AnnealingConfig.w_spread,
                   help="points for spreading each side's pieces over the 4/6/4 "
                        "cells it has, 100 = every preference honourable")
    g.add_argument("--canon_BL", type=int, default=CANON_CORNER_DEFAULT[0],
                   help="corner pieces in 0-BASED numbering (the beamer's), so "
                        "these four flags take the same values as the matching "
                        "flags on bin/E555_beamer_FixedFrame")
    g.add_argument("--canon_BR", type=int, default=CANON_CORNER_DEFAULT[1])
    g.add_argument("--canon_TL", type=int, default=CANON_CORNER_DEFAULT[2])
    g.add_argument("--canon_TR", type=int, default=CANON_CORNER_DEFAULT[3])

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
        target_scale         = args.target_scale,
        canon_corner         = (args.canon_BL, args.canon_BR,
                                args.canon_TL, args.canon_TR),
        w_affinity           = args.w_affinity,
        w_spread             = args.w_spread,
        pool                 = max(1, args.pool),
    )

    if len(set(config.canon_corner)) != 4:
        raise SystemExit(f"--canon_BL/BR/TL/TR must name four DIFFERENT pieces; "
                         f"got {config.canon_corner}")

    # A target of w_side x scale only means something for a positive weight.
    if config.target_scale:
        bad = [SIDE_NAMES[s] for s in Side if config.w_side(s) <= 0]
        if bad:
            raise SystemExit(
                f"--target_scale needs a positive weight per side to set a target; "
                f"got <= 0 for {', '.join(bad)}"
            )

    pieces = read_pieces(args.seed_file)
    corner_ids, edge_ids = classify_boundary_pieces(pieces)

    prior = None
    if args.prior:
        if config.fix_corners != 3:
            raise SystemExit(
                "--prior needs --fix_corners 3. The table is conditional on WHICH "
                "corner piece sits in which corner; under modes 0, 1 and 2 the "
                "frame is something else and the numbers do not apply to it.")
        prior = load_prior(args.prior, sorted(edge_ids), config.canon_corner)
    elif config.w_affinity or config.w_spread:
        # Weights with nothing to weight is the one mistake that produces a
        # plausible-looking run that quietly optimises nothing.
        print("[warn] --w_affinity/--w_spread are set but no --prior was given; "
              "both terms are 0 for this run", flush=True)

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
                # The frame, in the file, next to the rows it produced. A
                # rotations CSV is read months later by a beamer that has to be
                # given the SAME four corners, and a file that does not say
                # which ones cannot be used safely at all.
                fh.write(f"# frame {frame_str(config)}"
                         f"{'  prior ' + args.prior if args.prior else '  (no prior)'}\n")
        except OSError as e:
            raise SystemExit(f"[ERROR] cannot write --out {args.out}: {e}")

    # Ctrl-C: let the workers finish the restart they are in and keep whatever
    # has already been written, instead of losing every in-flight restart.
    signal.signal(signal.SIGINT, _request_stop)
    run_annealing(pieces, config, out_path=args.out, prior=prior)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
