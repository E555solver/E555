#!/usr/bin/env python3
"""
E555_border_prior.py -- turn the fixed-frame corpus into a Stage A border prior.

WHAT THIS IS FOR

    The corner study (tests/README.md) measured where each piece likes to sit on
    a 3x3 zone grid. That prior could not be handed to Stage C, because no board
    in data/ is in the study's frame -- most satisfy zero clues -- so the table
    had to be re-keyed before it meant anything there.

    Stage A has no such problem, and that is the point of this file. The
    ANNEALER BUILDS THE FRAME. If it pins the four corners to the same
    assignment the study measured in, the prior applies verbatim: same corners,
    same clue orientation, same coordinates. Nothing has to transfer.

WHAT IT MEASURES

    Stage A decides exactly one thing: which 14 of the 56 edge pieces go on each
    side of the border. So the prior is measured on the border cells alone --
    not on zones, not on the interior:

        BOTTOM  row 0,  cols 1..14        RIGHT  col 15, rows 1..14
        TOP     row 15, cols 1..14        LEFT   col 0,  rows 1..14

    and each side is split into three buckets on the SAME band cuts the zone
    study used -- [1-4] [5-10] [11-14], sizes 4, 6, 4 -- so a bucket is the part
    of a side that lies in one corner zone or in the middle:

        unit = (side, bucket)             12 units, 56 edge pieces

    Every lift is the study's estimator, unchanged: the share of a piece's
    border placements that land in a unit, over the same share for an average
    free edge piece, computed per covering study side and pooled. Boards are
    weighted 1/(boards from their border) and the bootstrap resamples BORDERS.

WHY THE BUCKETS MAKE THE ESTIMATOR CLEAN

    A study side at --stop_row 10 covers eleven rows, so it sees some border
    sides whole and others in part: side 0 fills the bottom row completely but
    only rows 1..10 of the two columns. At CELL granularity that is a partial
    exposure and it has to be corrected for. At BUCKET granularity it vanishes:
    the bands [1-4] [5-10] [11-14] are exactly the cuts a side's coverage falls
    on, so every (study side, unit) pair is either fully covered or not covered
    at all. Measured, not assumed -- `coverage` in the JSON prints it.

    That also gives every unit more than one independent view, which is what
    makes the numbers checkable rather than merely computed.

WHAT CAME OUT (55,712 boards, 1,741 borders)

    side affinity           cross-view Spearman  +0.38   (label-shuffled: -0.03)
    within-side contrast    cross-view Spearman  +0.65

    The views being compared use different borders, a different RNG stream and a
    different search direction -- one side's bottom ROW is another's left COLUMN
    -- so agreement is not the beam agreeing with itself. It also rules out the
    obvious confound: if the pattern were an artefact of how bottoms are sampled
    it would be the SAME on all four sides, and instead adjacent sides correlate
    NEGATIVELY (BOTTOM vs LEFT -0.48), which is the anti-corner structure the
    zone study already found.

    The within-side contrast replicating BETTER than the side aggregate is not a
    surprise either: "which end of this side" is the corner preference, and
    averaging a side's three buckets is exactly what throws it away.

WHAT DID NOT WORK, AND IS THEREFORE NOT IN THE OBJECTIVE

    Blending each piece's affinity toward the affinity of the colour it turns
    inward -- the obvious shrinkage, and the study's stated mechanism -- only
    makes the cross-view agreement worse, monotonically:

        blend  0.00   0.15   0.30   0.50   0.70   1.00
        rho   +0.381 +0.373 +0.363 +0.336 +0.316 +0.255

    So the piece-level estimate is not noise-limited in a way the colour fixes;
    the colour is just a coarser view of the same thing. The colour table is
    written out as a diagnostic and nothing reads it.

OUTPUT

    border_prior.txt   the table the annealer reads (see the header it carries)
    border_prior.json  the same numbers plus every validation figure above

USAGE

    python3 tests/E555_border_prior.py CORPUS.csv --out_dir tests/results
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import E555_viewer as V                                          # noqa: E402
from E555_frame_stats import border_of, side_of, spearman, UNPLACED   # noqa: E402

SIDE = V.SIDE
N_PIECES = V.N_PIECES

# Border sides, in the annealer's own order (Side.TOP=0 RIGHT=1 BOTTOM=2 LEFT=3
# is the ENUM; this file uses board order and names every row it writes, so the
# two never have to agree by position).
SIDE_NAMES = ["BOTTOM", "RIGHT", "TOP", "LEFT"]
N_SIDES = 4

# Bucket cuts within a side: cells 1-4, 5-10, 11-14 of the side's own axis.
BUCKET_CAP = (4, 6, 4)
N_BUCKETS = 3
N_UNITS = N_SIDES * N_BUCKETS

# Which physical corner each bucket end touches, in board coordinates (bucket 1
# is the middle and touches neither). Labels only -- the objective never uses
# them -- but they are what makes a printed table readable.
BUCKET_CORNER = {
    (0, 0): "BL", (0, 2): "BR",
    (1, 0): "BR", (1, 2): "TR",
    (2, 0): "TL", (2, 2): "TR",
    (3, 0): "BL", (3, 2): "TL",
}

N_COLORS = 23


def unit_of(cell):
    """(side, bucket) for a border cell, or -1. Corners belong to no side."""
    r, c = divmod(cell, SIDE)

    def b(v):
        return 0 if v < 5 else (1 if v < 11 else 2)

    if r == 0 and 1 <= c <= 14:
        return 0 * N_BUCKETS + b(c)
    if c == SIDE - 1 and 1 <= r <= 14:
        return 1 * N_BUCKETS + b(r)
    if r == SIDE - 1 and 1 <= c <= 14:
        return 2 * N_BUCKETS + b(c)
    if c == 0 and 1 <= r <= 14:
        return 3 * N_BUCKETS + b(r)
    return -1


# The face a border piece turns INWARD, per side, as an index into the piece's
# (top, right, bottom, left) tuple after its spin has been applied.
INWARD_FACE = (0, 3, 2, 1)
# The face that carries the grey frame colour, per side. Together these two give
# a border piece's inward colour without needing the board: the side fixes the
# spin, and the spin fixes the colour.
OUTWARD_FACE = (2, 1, 0, 3)


# --------------------------------------------------------------------------
# Load

def piece_kind(edges):
    greys = sum(1 for e in edges if e == 0)
    return 2 if greys == 2 else (1 if greys == 1 else 0)


def load_corpus(path, progress=20000):
    """Per-border weighted counts of every piece in each of the 12 border units.

    One row per border, not per board: boards from one border share that
    border exactly, so they are one observation between them.
    """
    umap = np.array([unit_of(c) for c in range(N_PIECES)], np.int64)
    by_border = defaultdict(list)
    total = 0
    for _, cid, _score, pos, rot in V.iter_records(path):
        total += 1
        by_border[border_of(cid)].append((np.asarray(pos, np.int16),
                                          np.asarray(rot, np.int8)))
        if progress and total % progress == 0:
            print(f"[load]   {total} rows...", file=sys.stderr)

    cids = sorted(by_border)
    n_cfg = len(cids)
    cnt = np.zeros((n_cfg, N_PIECES, N_UNITS), np.float32)
    col = np.zeros((n_cfg, N_COLORS, N_UNITS), np.float32)
    sides = np.full(n_cfg, -1, np.int8)
    n_boards = np.zeros(n_cfg, np.int32)

    seed = load_seed_edges()
    # inward[piece, spin, side] -- precomputed so the hot loop is one gather.
    inward = np.zeros((N_PIECES, 4, N_SIDES), np.int64)
    for p in range(N_PIECES):
        for sp in range(4):
            face = [seed[p][(i + sp) % 4] for i in range(4)]
            for u in range(N_SIDES):
                inward[p, sp, u] = face[INWARD_FACE[u]]

    for i, cid in enumerate(cids):
        rows = by_border[cid]
        sides[i] = side_of(cid)
        n_boards[i] = len(rows)
        w = 1.0 / len(rows)
        fp = np.zeros(N_PIECES * N_UNITS)
        fc = np.zeros(N_COLORS * N_UNITS)
        for pos, rot in rows:
            placed = np.flatnonzero(pos != UNPLACED)
            cells = pos[placed].astype(np.int64)
            u = umap[cells]
            keep = u >= 0
            pl, uu = placed[keep], u[keep]
            fp += np.bincount(pl * N_UNITS + uu, minlength=N_PIECES * N_UNITS)
            cc = inward[pl, rot[pl], uu // N_BUCKETS]
            fc += np.bincount(cc * N_UNITS + uu, minlength=N_COLORS * N_UNITS)
        cnt[i] = (fp * w).reshape(N_PIECES, N_UNITS)
        col[i] = (fc * w).reshape(N_COLORS, N_UNITS)
        by_border[cid] = None
    return cids, sides, n_boards, cnt, col, total


def load_seed_edges(path=None):
    """The 256 (top, right, bottom, left) colour tuples."""
    return V.load_seed(V.find_seed(path))


# --------------------------------------------------------------------------
# Lifts

def coverage(cnt, sides):
    """cover[study_side, unit]: did this study side ever place anything there?

    Read off the corpus rather than derived from the rotation algebra, so a farm
    run at a different --stop_row shows up here instead of biasing every lift.
    """
    cov = np.zeros((N_SIDES, N_UNITS), bool)
    for s in range(N_SIDES):
        m = sides == s
        if m.any():
            cov[s] = cnt[m].sum(0).sum(0) > 0
    return cov


def lift_of(sub, edges):
    """lift[piece, unit] from summed counts sub[piece, unit], edge pieces only.

    Share of a piece's border placements that fall in the unit, over the same
    share for an average edge piece. Self-normalising per piece, so a piece that
    is simply placed more often does not read as preferring anything.
    """
    tot = sub.sum(1)
    E = sub[edges]
    base = E.sum(0) / max(E.sum(), 1e-12)
    with np.errstate(invalid="ignore", divide="ignore"):
        share = E / np.maximum(tot[edges, None], 1e-12)
        return share / np.maximum(base, 1e-12)[None, :]


def unit_groups(cov):
    """Units that share a covering set, so one matrix-vector does all of them."""
    g = defaultdict(list)
    for u in range(N_UNITS):
        g[tuple(np.flatnonzero(cov[:, u]))].append(u)
    return {k: v for k, v in g.items() if k}


def pooled_lift(Xf, sides, cov, edges, w=None, groups=None):
    """lift[piece, unit], each unit pooled over only the study sides covering it.

    `Xf` is the per-border count tensor flattened to [border, piece*unit], and
    `w` a per-border weight -- 1 for every border, a 0/1 mask to isolate one
    study side's view, or a multiplicity vector for a bootstrap resample. Every
    caller is then one matrix-vector product per covering set instead of a fresh
    boolean index over 20 MB, which is the difference between a bootstrap that
    takes 20 seconds and one that takes twenty minutes.
    """
    if groups is None:
        groups = unit_groups(cov)
    if w is None:
        w = np.ones(Xf.shape[0], np.float32)
    out = np.full((len(edges), N_UNITS), np.nan)
    for S, us in groups.items():
        m = np.isin(sides, S).astype(np.float32) * w
        if not m.any():
            continue
        L = lift_of((m @ Xf).reshape(N_PIECES, N_UNITS), edges)
        for u in us:
            out[:, u] = L[:, u]
    return out


def side_affinity(lift12):
    """log10 of each piece's expected lift on each side, and its occupancy split.

    A side is 14 cells: 4 + 6 + 4. The expected lift at a uniformly chosen cell
    of the side is the capacity-weighted mean of its three bucket lifts, and the
    occupancy split is the same three numbers normalised -- 'given this piece is
    on this side, which part of it does it sit in'.

    Building both from the bucket tensor rather than measuring the side directly
    is what keeps the partial-coverage problem out: a bucket is covered whole or
    not at all, a side is not.
    """
    cap = np.array(BUCKET_CAP, float)
    L = lift12.reshape(-1, N_SIDES, N_BUCKETS)
    wsum = (L * cap).sum(2)
    aff = np.log10(np.maximum(wsum / cap.sum(), 1e-3))
    with np.errstate(invalid="ignore", divide="ignore"):
        occ = (L * cap) / np.maximum(wsum, 1e-12)[:, :, None]
    return aff, occ


# --------------------------------------------------------------------------
# Normalising constants: what a random border scores, and what the best one could

def assignment_optimum(aff, cap=14):
    """Max over balanced assignments of sum_p aff[p, side(p)], exactly.

    A transportation problem: 56 pieces, four sides, capacity 14 each. Small
    enough for an exact DP over how many pieces each of the first three sides
    has taken -- the fourth is implied -- which is 15^3 states and needs no
    solver library.
    """
    n = aff.shape[0]
    NEG = -1e18
    dp = np.full((cap + 1, cap + 1, cap + 1), NEG)
    dp[0, 0, 0] = 0.0
    for p in range(n):
        nxt = np.full_like(dp, NEG)
        a0, a1, a2, a3 = aff[p]
        # side 3 takes it: counts unchanged
        np.maximum(nxt, dp + a3, out=nxt)
        nxt[1:, :, :] = np.maximum(nxt[1:, :, :], dp[:-1, :, :] + a0)
        nxt[:, 1:, :] = np.maximum(nxt[:, 1:, :], dp[:, :-1, :] + a1)
        nxt[:, :, 1:] = np.maximum(nxt[:, :, 1:], dp[:, :, :-1] + a2)
        dp = nxt
    return float(dp[cap, cap, cap])


def random_tv(occ, seed, draws=4000, cap=14):
    """Mean crowding of a random balanced assignment.

    Takes a SEED and opens its own stream rather than borrowing the caller's.
    This number is a normalising constant that goes into the file and scales
    every spread score the annealer computes, and drawing it from a shared
    stream would make it depend on how many bootstrap resamples happened to run
    first -- so the same corpus at --n_boot 200 and --n_boot 600 would produce
    two slightly different scales. Cheap to get wrong and invisible afterwards.

    Crowding is the transport distance between what a side's 14 pieces WANT --
    the sum of their occupancy splits -- and what the side has to offer, 4/6/4
    cells. It is measured in pieces: 1.0 means one piece's worth of preference
    cannot be honoured wherever Stage B orders that side.
    """
    rng = np.random.default_rng(seed)
    capv = np.array(BUCKET_CAP, float)
    pool = np.repeat(np.arange(N_SIDES), cap)
    tot = 0.0
    for _ in range(draws):
        a = rng.permutation(pool)
        s = 0.0
        for u in range(N_SIDES):
            h = occ[np.flatnonzero(a == u), u, :].sum(0)
            s += 0.5 * np.abs(h - capv).sum()
        tot += s
    return tot / draws


# --------------------------------------------------------------------------
# Validation

def full_view_side(cov, u_side):
    """The one study side that sees this border side whole, or None."""
    for s in range(N_SIDES):
        if all(cov[s, u_side * N_BUCKETS + b] for b in range(N_BUCKETS)):
            return s
    return None


def subset_affinity(L, u_side, buckets):
    """log10 lift over a CHOSEN set of a side's buckets.

    A partial view misses one end of the side, so comparing its side average
    against the full view's would be comparing two different quantities. Both
    views are therefore reduced to the buckets they share before the comparison.
    """
    cap = np.array([BUCKET_CAP[b] for b in buckets], float)
    cols = [u_side * N_BUCKETS + b for b in buckets]
    return np.log10(np.maximum((L[:, cols] * cap).sum(1) / cap.sum(), 1e-3))


def cross_view(Xf, sides, cov, edges, groups=None):
    """Agreement between independent views of the same unit.

    Every border side is seen whole by exactly one study side and in part by two
    others, and those others see it in a different SEARCH ROLE -- one side's
    bottom row is another's left column. Comparing them is therefore a real
    replication and not the heuristic agreeing with itself.
    """
    groups = groups or unit_groups(cov)
    view = {}

    def viewed(s):
        if s not in view:
            view[s] = pooled_lift(Xf, sides, cov, edges,
                                  (sides == s).astype(np.float32), groups)
        return view[s]

    aff_rho, con_rho = [], []
    for u_side in range(N_SIDES):
        sf = full_view_side(cov, u_side)
        if sf is None:
            continue
        Lf = viewed(sf)
        for s in range(N_SIDES):
            if s == sf:
                continue
            bs = [b for b in range(N_BUCKETS) if cov[s, u_side * N_BUCKETS + b]]
            if len(bs) < 2:
                continue
            Lp = viewed(s)
            aff_rho.append(spearman(subset_affinity(Lf, u_side, bs),
                                    subset_affinity(Lp, u_side, bs)))
            # The contrast the partial view can actually see: its lowest covered
            # bucket against its highest.
            lo, hi = bs[0], bs[-1]

            def contrast(L):
                return (np.log10(np.maximum(L[:, u_side * N_BUCKETS + lo], 1e-3))
                        - np.log10(np.maximum(L[:, u_side * N_BUCKETS + hi], 1e-3)))

            con_rho.append(spearman(contrast(Lf), contrast(Lp)))
    return aff_rho, con_rho


def shuffle_null(cnt, sides, cov, edges, rng, draws):
    """The same replication statistic on corpora with the piece labels shuffled.

    Independently per border. A single global permutation only renames the
    pieces and leaves every correlation exactly where it was, which is the
    mistake this null exists to avoid.
    """
    groups = unit_groups(cov)
    out = []
    for _ in range(draws):
        X = cnt.copy()
        for i in range(X.shape[0]):
            X[i][edges] = X[i][rng.permutation(edges)]
        a, _c = cross_view(X.reshape(X.shape[0], -1), sides, cov, edges, groups)
        out.append(float(np.mean(a)))
    return out


def colour_affinity(colf, sides, cov, edges):
    """log10 lift per (colour, side), measured the same way as the piece table."""
    cap = np.array(BUCKET_CAP, float)
    out = {}
    for s in range(N_SIDES):
        m = (sides == s).astype(np.float32)
        sub = (m @ colf).reshape(N_COLORS, N_UNITS)
        tot = sub.sum(1)
        base = sub.sum(0) / max(sub.sum(), 1e-12)
        with np.errstate(invalid="ignore", divide="ignore"):
            cl = (sub / np.maximum(tot[:, None], 1e-12)) / np.maximum(base, 1e-12)[None, :]
        cl = cl.reshape(N_COLORS, N_SIDES, N_BUCKETS)
        out[s] = np.log10(np.maximum((cl * cap).sum(2) / cap.sum(), 1e-3))
    return out


def inward_colour_table(seed, edges):
    """Each edge piece's inward colour on each side; the side fixes the spin."""
    pcol = np.zeros((len(edges), N_SIDES), np.int64)
    for j, p in enumerate(edges):
        for u in range(N_SIDES):
            spin = next(x for x in range(4) if seed[p][(OUTWARD_FACE[u] + x) % 4] == 0)
            face = [seed[p][(i + spin) % 4] for i in range(4)]
            pcol[j, u] = face[INWARD_FACE[u]]
    return pcol


def colour_blend_curve(Xf, colf, sides, cov, edges, seed, weights):
    """Does shrinking a piece toward its inward colour help? Measured, not assumed."""
    groups = unit_groups(cov)
    lc = colour_affinity(colf, sides, cov, edges)
    pcol = inward_colour_table(seed, edges)
    view = {s: pooled_lift(Xf, sides, cov, edges,
                           (sides == s).astype(np.float32), groups)
            for s in range(N_SIDES)}
    curve = []
    for w in weights:
        rs = []
        for u_side in range(N_SIDES):
            sf = full_view_side(cov, u_side)
            if sf is None:
                continue
            for s in range(N_SIDES):
                if s == sf:
                    continue
                bs = [b for b in range(N_BUCKETS) if cov[s, u_side * N_BUCKETS + b]]
                if len(bs) < 2:
                    continue

                def blend(v):
                    a = subset_affinity(view[v], u_side, bs)
                    c = np.nan_to_num(lc[v][pcol[:, u_side], u_side])
                    return (1 - w) * a + w * c

                rs.append(spearman(blend(sf), blend(s)))
        curve.append((w, float(np.mean(rs))))
    return curve


# --------------------------------------------------------------------------
# Output

def write_prior(path, aff, occ, lo, hi, edges, meta):
    cap = BUCKET_CAP
    with open(path, "w") as f:
        f.write("# E555 border prior -- measured, not assumed.\n")
        f.write("#\n")
        f.write("# Built by tests/E555_border_prior.py from the fixed-frame corpus.\n")
        f.write("# Read by tests/E555_edge_annealer_FixedFrame.py. Nothing else uses it.\n")
        f.write("#\n")
        f.write("# The frame this was measured in. The annealer refuses a prior whose\n")
        f.write("# frame is not the one it is pinning: a corner assignment is one of\n")
        f.write("# 4! = 24 and the numbers below mean nothing under a different one.\n")
        f.write(f"frame orient {meta['orient']} "
                f"BL {meta['canon']['BL']} BR {meta['canon']['BR']} "
                f"TL {meta['canon']['TL']} TR {meta['canon']['TR']}\n")
        f.write(f"sides {' '.join(SIDE_NAMES)}\n")
        f.write(f"buckets {' '.join(str(c) for c in cap)}\n")
        f.write("#\n")
        f.write("# Normalising constants, so a score can be read as a percentage of what\n")
        f.write("# is achievable rather than as an arbitrary number of decades.\n")
        f.write("#   aff_random   sum of affinities of a random balanced assignment (exact)\n")
        f.write("#   aff_optimum  the best balanced assignment, ignoring Euler feasibility\n")
        f.write("#   tv_random    mean crowding of a random balanced assignment, in pieces\n")
        f.write(f"norm aff_random {meta['aff_random']:.6f} "
                f"aff_optimum {meta['aff_optimum']:.6f} "
                f"tv_random {meta['tv_random']:.6f}\n")
        f.write("#\n")
        f.write("# aff <piece> <log10 lift on BOTTOM RIGHT TOP LEFT>\n")
        f.write("#   0 = this piece sits on that side exactly as often as an average edge\n")
        f.write("#   piece does. +0.10 is about 1.26x, -0.10 about 0.79x.\n")
        for j, p in enumerate(edges):
            f.write(f"aff {p:3d} " + " ".join(f"{aff[j, u]:+.5f}" for u in range(N_SIDES)) + "\n")
        f.write("#\n")
        f.write("# occ <piece> <12 numbers: P(bucket | side), for BOTTOM RIGHT TOP LEFT>\n")
        f.write("#   Buckets run along the side's own axis: cells 1-4, 5-10, 11-14.\n")
        f.write("#   These are what the crowding term compares against 4/6/4.\n")
        for j, p in enumerate(edges):
            vals = " ".join(f"{occ[j, u, b]:.5f}" for u in range(N_SIDES)
                            for b in range(N_BUCKETS))
            f.write(f"occ {p:3d} {vals}\n")
        f.write("#\n")
        f.write("# ci <piece> <lo BOTTOM..LEFT> <hi BOTTOM..LEFT> -- 95% bootstrap over\n")
        f.write("# BORDERS on the affinity above. Diagnostic: the objective uses the\n")
        f.write("# point estimates, and this is how you see which of them are worth\n")
        f.write("# anything. An interval containing 0 has shown you nothing.\n")
        for j, p in enumerate(edges):
            f.write(f"ci {p:3d} "
                    + " ".join(f"{lo[j, u]:+.5f}" for u in range(N_SIDES)) + " "
                    + " ".join(f"{hi[j, u]:+.5f}" for u in range(N_SIDES)) + "\n")
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Measure a Stage A border prior from the fixed-frame corpus",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("corpus", help="canonicalised corpus CSV (run_fixedframe_farm.sh)")
    ap.add_argument("--out_dir", default="ff_out/prior")
    ap.add_argument("--seed_file", default=None, help="piece file (default: auto)")
    ap.add_argument("--n_boot", type=int, default=600, help="bootstrap resamples over borders")
    ap.add_argument("--n_perm", type=int, default=8, help="label-shuffle null draws")
    ap.add_argument("--rng_seed", type=int, default=20260914)
    ap.add_argument("--orient", type=int, default=0, help="clue orientation the corpus was measured in")
    ap.add_argument("--canon_BL", type=int, default=3)
    ap.add_argument("--canon_BR", type=int, default=2)
    ap.add_argument("--canon_TL", type=int, default=0)
    ap.add_argument("--canon_TR", type=int, default=1)
    args = ap.parse_args(argv)

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.rng_seed)

    seed = load_seed_edges(args.seed_file)
    kind = np.array([piece_kind(seed[p]) for p in range(N_PIECES)])
    edges = np.flatnonzero(kind == 1)
    print(f"[prior] {len(edges)} edge pieces")

    t0 = time.time()
    cids, sides, n_boards, cnt, col, total = load_corpus(args.corpus)
    print(f"[prior] {total} boards from {len(cids)} borders in {time.time()-t0:.1f}s")
    per_side = {int(s): int((sides == s).sum()) for s in range(N_SIDES)}
    print(f"[prior] borders per study side: {per_side}")

    Xf = np.ascontiguousarray(cnt.reshape(len(cids), -1))
    colf = np.ascontiguousarray(col.reshape(len(cids), -1))

    cov = coverage(cnt, sides)
    groups = unit_groups(cov)
    for u in range(N_UNITS):
        s_, b_ = divmod(u, N_BUCKETS)
        seen = [s for s in range(N_SIDES) if cov[s, u]]
        print(f"[cover] {SIDE_NAMES[s_]:7s} bucket {b_} "
              f"({BUCKET_CAP[b_]} cells, {BUCKET_CORNER.get((s_, b_), 'middle')}) "
              f"seen by study sides {seen}")

    lift12 = pooled_lift(Xf, sides, cov, edges, None, groups)
    aff, occ = side_affinity(lift12)

    # Bootstrap over borders -- the independent unit.
    print(f"[prior] bootstrapping {args.n_boot} resamples over {len(cids)} borders...")
    n_cfg = len(cids)
    boot = np.empty((args.n_boot, len(edges), N_SIDES))
    for b in range(args.n_boot):
        w = np.bincount(rng.integers(0, n_cfg, n_cfg),
                        minlength=n_cfg).astype(np.float32)
        boot[b] = side_affinity(pooled_lift(Xf, sides, cov, edges, w, groups))[0]
    lo = np.nanpercentile(boot, 2.5, axis=0)
    hi = np.nanpercentile(boot, 97.5, axis=0)
    clears = int(((lo > 0) | (hi < 0)).sum())
    print(f"[prior] {clears} of {len(edges)*N_SIDES} (piece, side) affinities "
          f"have a 95% interval clear of 0")

    aff_rho, con_rho = cross_view(Xf, sides, cov, edges, groups)
    print(f"[check] side affinity   cross-view rho = {np.mean(aff_rho):+.3f} "
          f"over {len(aff_rho)} view pairs")
    print(f"[check] within-side contrast        rho = {np.mean(con_rho):+.3f} "
          f"over {len(con_rho)} view pairs")
    nulls = shuffle_null(cnt, sides, cov, edges, rng, args.n_perm)
    print(f"[check] label-shuffled null          rho = {np.mean(nulls):+.3f} "
          f"(range {min(nulls):+.3f} .. {max(nulls):+.3f}, {args.n_perm} draws)")

    curve = colour_blend_curve(Xf, colf, sides, cov, edges, seed,
                               (0.0, 0.15, 0.30, 0.50, 0.70, 1.0))
    print("[check] colour blend:  " +
          "  ".join(f"w={w:.2f} rho={r:+.3f}" for w, r in curve))

    aff_random = float(len(edges) * aff.mean())
    aff_optimum = assignment_optimum(aff)
    tv_random = float(random_tv(occ, args.rng_seed ^ 0x7C))
    print(f"[norm ] affinity: random {aff_random:+.3f}  optimum {aff_optimum:+.3f}  "
          f"headroom {aff_optimum - aff_random:.3f} decades over 56 pieces")
    print(f"[norm ] crowding: random {tv_random:.3f} pieces displaced")

    meta = {
        "corpus": str(args.corpus), "boards": int(total), "borders": int(len(cids)),
        "borders_per_study_side": per_side,
        "orient": args.orient,
        "canon": {"BL": args.canon_BL, "BR": args.canon_BR,
                  "TL": args.canon_TL, "TR": args.canon_TR},
        "edge_pieces": [int(p) for p in edges],
        "coverage": {f"{SIDE_NAMES[u//N_BUCKETS]}{u%N_BUCKETS}":
                     [s for s in range(N_SIDES) if cov[s, u]] for u in range(N_UNITS)},
        "aff_random": aff_random, "aff_optimum": aff_optimum, "tv_random": tv_random,
        "significant_affinities": clears,
        "n_affinities": int(len(edges) * N_SIDES),
        "cross_view_affinity_rho": float(np.mean(aff_rho)),
        "cross_view_contrast_rho": float(np.mean(con_rho)),
        "null_rho_mean": float(np.mean(nulls)),
        "null_rho_draws": [float(x) for x in nulls],
        "colour_blend_curve": [[float(w), float(r)] for w, r in curve],
        "n_boot": args.n_boot,
    }

    txt = write_prior(out / "border_prior.txt", aff, occ, lo, hi, edges, meta)
    (out / "border_prior.json").write_text(json.dumps(meta, indent=1))
    print(f"[out  ] {txt}")
    print(f"[out  ] {out/'border_prior.json'}")

    # The readable summary: who wants which side.
    print("\nstrongest side preferences (log10 lift, x an average edge piece):")
    for u in range(N_SIDES):
        order = np.argsort(-aff[:, u])[:8]
        cells = "  ".join(f"{edges[j]}({10**aff[j,u]:.2f}x)" for j in order)
        print(f"  {SIDE_NAMES[u]:7s} {cells}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
