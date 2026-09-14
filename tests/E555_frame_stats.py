#!/usr/bin/env python3
"""
E555_frame_stats.py -- which pieces belong near which corner?

THE QUESTION

    The frame pins four corner pieces and four corner clues. That is a lot of
    constraint: only a few pieces can sit next to a given corner piece and still
    leave room for the clue two cells in, and only a few can sit next to that
    clue. So the placement distribution near a corner is NOT close to uniform,
    and it is not the same near each of the four corners -- they carry different
    pieces and different clues.

    That is the signal worth mining. Not "this piece likes the middle" but "this
    piece belongs in the bottom-left, and these others belong in the top-right".

THE PARTITION

    A 3x3 grid on the bands [0-4], [5-10], [11-15] -- 5, 6, 5 cells:

        TL  TM  TR      rows 11-15       4 corner zones, 25 cells each
        ML  CC  MR      rows 5-10        4 side zones,   30 cells each
        BL  BM  BR      rows 0-4         1 centre zone,  36 cells

    Those cut points are not arbitrary. A farm run at --stop_row 10 fills eleven
    rows, and canonicalised the four sides cover exactly

        side 0  rows 0..10      side 2  rows 5..15
        side 1  cols 5..15      side 3  cols 0..10

    -- the same cuts. So every zone is either wholly inside a side's band or
    wholly outside it, and each corner zone is covered by exactly two sides:

        BL {0,3}   BR {0,1}   TL {2,3}   TR {1,2}   CC {0,1,2,3}

    Two independent views of every corner is what turns "this piece prefers BL"
    from an assertion into something that can be checked.

WHY EXPOSURE IS THE WHOLE PROBLEM

    Side 0 fills rows 0..10 and therefore CANNOT place anything in TL or TR. A
    naive count says every piece has zero affinity for the top, which is an
    artefact of where the beam was pointed, not a fact about the puzzle.

    So every lift is computed per side first, over only the zones that side
    actually covers, and pooled afterwards across the sides that cover the zone:

        lift[a][z] = (A/B) / (C/D)      over sides S_z that cover z
          A = weighted count of piece a in zone z
          B = weighted count of piece a anywhere, in those sides
          C = weighted count of any same-kind piece in zone z
          D = weighted count of any same-kind piece anywhere, in those sides

    1.00 means "exactly as often as an average piece of its kind". Edge pieces
    and inner pieces get separate denominators: an edge piece can only ever sit
    on the frame, so comparing it against inner pieces would measure the frame,
    not a preference.

UNCERTAINTY

    Boards are not independent -- every board a config emits descends from one
    border, shared exactly. The border config is the independent unit, so every
    board is weighted 1/(boards from its config) and the bootstrap resamples
    CONFIGS, not boards. Confidence intervals are percentiles over 2000 such
    resamples. A piece whose interval straddles 1.00 has not shown us anything.

THE ACTIONABLE OUTPUT

    The beamer grows bottom-up. Run it in the canonical frame to --stop_row 12
    and rows 13-15 are unreachable: any piece that belongs up there is budget the
    search is wasting. `far_score` is

        log2( lift(rows 13-15) / lift(rows 0-2) )

    bootstrapped, and `exclude_pieces.txt` holds the pieces whose interval is
    entirely above zero -- evidence of a far-side preference, not just a point
    estimate that happens to be positive.

USAGE

    python3 tests/E555_frame_stats.py ff_out/corpus.csv --out_dir ff_out/stats
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import E555_viewer as V                                          # noqa: E402

SIDE = V.SIDE
N_PIECES = V.N_PIECES
UNPLACED = 999

CANON_ORIENT = 0
CANON_CLUE_CELL, CANON_CLUE_PIECE, CANON_CLUE_SPIN = V.clue_list(
    CANON_ORIENT, V.CLUE_CENTER)[0]
CLUE_CELL_OF = {cell: p for cell, p, _s in V.clue_list(CANON_ORIENT)}
CLUE_PIECES = set(CLUE_CELL_OF.values())

# 3x3 zones on the bands [0-4], [5-10], [11-15].
BAND_EDGES = (0, 5, 11, 16)
ZONE_NAMES = ["BL", "BM", "BR", "ML", "CC", "MR", "TL", "TM", "TR"]
ZONE_LABEL = {
    "BL": "bottom-left corner", "BM": "bottom edge", "BR": "bottom-right corner",
    "ML": "left edge", "CC": "centre", "MR": "right edge",
    "TL": "top-left corner", "TM": "top edge", "TR": "top-right corner",
}
CORNER_ZONES = ["BL", "BR", "TL", "TR"]
N_ZONES = 9

# Row bands for the actionable score: what a --stop_row 12 run can and cannot
# reach. Rows 13-15 are never filled; rows 0-2 are filled first and always.
ROWBAND_NAMES = ["near(0-2)", "mid(3-12)", "far(13-15)"]
N_RB = 3

KIND_NAMES = ["inner", "edge", "corner"]


def band_of(v):
    return 0 if v < BAND_EDGES[1] else (1 if v < BAND_EDGES[2] else 2)


def zone_of(cell):
    r, c = divmod(cell, SIDE)
    return band_of(r) * 3 + band_of(c)


def rowband_of(cell):
    r = cell // SIDE
    return 0 if r <= 2 else (2 if r >= 13 else 1)


def piece_kind(edges):
    greys = sum(1 for e in edges if e == 0)
    return 2 if greys == 2 else (1 if greys == 1 else 0)


def side_of(config_id):
    if config_id.startswith("s") and "_" in config_id:
        head = config_id[1:config_id.index("_")]
        if head.isdigit():
            return int(head)
    return -1


def border_of(config_id):
    """The independent unit: the BOTTOM row, not the (bottom, column) pair.

    Ids look like s<side>_<runtag>_b<bottom>l<column>. With --top_columns 1 there
    is one column per bottom and this is the config id. Raise --top_columns and
    several configs share a bottom row exactly -- 16 of the 176 cells identical,
    plus everything the bottom forces above it -- so treating them as separate
    observations would inflate the sample. Grouping on the bottom is correct
    under either setting, and identical to the config id under the default.
    """
    i = config_id.rfind("l")
    return config_id[:i] if i > 0 else config_id


def spearman(x, y):
    """Rank correlation of two 1-D arrays (ties averaged)."""
    if len(x) < 3:
        return float("nan")

    def rank(a):
        order = np.argsort(a, kind="mergesort")
        r = np.empty(len(a), float)
        r[order] = np.arange(len(a), dtype=float)
        # average ties
        a_sorted = a[order]
        i = 0
        while i < len(a):
            j = i
            while j + 1 < len(a) and a_sorted[j + 1] == a_sorted[i]:
                j += 1
            if j > i:
                r[order[i:j + 1]] = (i + j) / 2.0
            i = j + 1
        return r

    rx, ry = rank(np.asarray(x, float)), rank(np.asarray(y, float))
    if rx.std() == 0 or ry.std() == 0:
        return float("nan")
    return float(np.corrcoef(rx, ry)[0, 1])


# --------------------------------------------------------------------------
# Load


def load_corpus(path, strict=True, progress=20000):
    """Per-config weighted count tensors. The config is the independent unit.

    Boards are kept as int16 arrays, not Python lists: a 60k-board corpus is
    30 MB that way and roughly half a gigabyte the other. Accumulation goes
    through bincount rather than np.add.at, which is the difference between
    seconds and minutes at this size.
    """
    by_config = defaultdict(list)
    off_frame = total = 0
    for _, cid, _sol, pos, rot in V.iter_records(path):
        total += 1
        if (pos[CANON_CLUE_PIECE] != CANON_CLUE_CELL
                or rot[CANON_CLUE_PIECE] != CANON_CLUE_SPIN):
            off_frame += 1
            continue
        by_config[border_of(cid)].append(np.asarray(pos, np.int16))
        if progress and total % progress == 0:
            print(f"[load]   {total} rows...", file=sys.stderr)
    if off_frame:
        msg = (f"{off_frame} of {total} boards are not on the canonical frame; "
               f"the rotation step in run_fixedframe_farm.sh did not run")
        if strict:
            sys.exit(f"[frame] FATAL: {msg}")
        print(f"[frame] WARNING: {msg}", file=sys.stderr)

    cids = sorted(by_config)
    n_cfg = len(cids)
    zone = np.zeros((n_cfg, N_PIECES, N_ZONES), np.float32)
    rband = np.zeros((n_cfg, N_PIECES, N_RB), np.float32)
    cellc = np.zeros((N_PIECES, N_PIECES), np.float32)     # piece x cell
    sidec = np.zeros((4, N_PIECES), np.float32)            # side x cell coverage
    sides = np.full(n_cfg, -1, np.int8)
    n_boards = np.zeros(n_cfg, np.int32)

    zmap = np.array([zone_of(c) for c in range(256)], np.int64)
    rmap = np.array([rowband_of(c) for c in range(256)], np.int64)

    for i, cid in enumerate(cids):
        boards = by_config[cid]
        s = side_of(cid)
        sides[i] = s
        n_boards[i] = len(boards)
        w = 1.0 / len(boards)          # every border counts once, not every board
        # One flat index per (piece, bucket) pair, summed in a single bincount
        # per board -- np.add.at on the same data is ~50x slower.
        zf = np.zeros(N_PIECES * N_ZONES)
        rf = np.zeros(N_PIECES * N_RB)
        cf = np.zeros(N_PIECES * N_PIECES)
        for pos in boards:
            placed = np.flatnonzero(pos != UNPLACED)
            cells = pos[placed].astype(np.int64)
            zf += np.bincount(placed * N_ZONES + zmap[cells],
                              minlength=N_PIECES * N_ZONES)
            rf += np.bincount(placed * N_RB + rmap[cells],
                              minlength=N_PIECES * N_RB)
            cf += np.bincount(placed * N_PIECES + cells,
                              minlength=N_PIECES * N_PIECES)
        zone[i] = (zf * w).reshape(N_PIECES, N_ZONES)
        rband[i] = (rf * w).reshape(N_PIECES, N_RB)
        cellc += (cf * w).reshape(N_PIECES, N_PIECES)
        if 0 <= s < 4:
            sidec[s] += (cf * w).reshape(N_PIECES, N_PIECES).sum(0)
        by_config[cid] = None          # release as we go
    return cids, sides, n_boards, zone, rband, cellc, sidec, total, off_frame


# --------------------------------------------------------------------------
# Coverage, measured rather than assumed


def measure_coverage(sidec, per="zone"):
    """Which (side, zone) pairs the corpus actually observed.

    Derived from the boards, not from the rotation algebra, so a farm run at a
    different --stop_row -- or a bug in the turn -- shows up here instead of
    silently biasing every lift downstream.
    """
    n = N_ZONES if per == "zone" else N_RB
    mapper = zone_of if per == "zone" else rowband_of
    size = np.zeros(n)
    hit = np.zeros((4, n))
    for cell in range(256):
        k = mapper(cell)
        size[k] += 1
        for s in range(4):
            if sidec[s, cell] > 0:
                hit[s, k] += 1
    frac = hit / np.maximum(size, 1)
    return frac > 0.5, frac


# --------------------------------------------------------------------------
# Lifts


# Pieces the frame nails to a cell: the four corners and the five clues. They
# are not search results, so they must not sit in the denominator either -- the
# baseline is "an average FREE piece of this kind". Two of the clues are not even
# searched; format_board_tail attaches them to every emitted board, which would
# otherwise add a constant to the TL and TR exposure and deflate every piece's
# lift in exactly the two zones the study is about.
PINNED = sorted(CLUE_PIECES)


def lift_from_counts(sub, kind, kinds_of_interest=(0, 1)):
    """lift[b, piece, zone] from summed counts sub[b, piece, zone].

    Denominators are per KIND: an edge piece is compared against edge pieces, an
    inner piece against inner pieces. Mixing them would measure the frame.
    """
    B = sub.shape[0]
    nz = sub.shape[2]
    out = np.full((B, N_PIECES, nz), np.nan, np.float64)
    tot_piece = sub.sum(2)                                   # [B, piece]
    free = np.ones(N_PIECES, bool)
    free[PINNED] = False
    for k in kinds_of_interest:
        m = (kind == k) & free
        if not m.any():
            continue
        C = sub[:, m, :].sum(1)                              # [B, zone]
        D = tot_piece[:, m].sum(1)                           # [B]
        base = C / np.maximum(D, 1e-12)[:, None]             # [B, zone]
        num = kind == k                                      # includes pinned
        with np.errstate(divide="ignore", invalid="ignore"):
            share = sub[:, num, :] / np.maximum(tot_piece[:, num], 1e-12)[:, :, None]
            out[:, num, :] = share / np.maximum(base, 1e-12)[:, None, :]
    return out


def pooled_lift(per_side_counts, cover, kind, n_units):
    """Point-estimate lift per zone, pooling only the sides that cover it."""
    out = np.full((N_PIECES, n_units), np.nan)
    for z in range(n_units):
        S = [s for s in range(4) if cover[s, z]]
        if not S:
            continue
        sub = sum(per_side_counts[s] for s in S)[None, :, :]   # [1, piece, unit]
        out[:, z] = lift_from_counts(sub, kind)[0, :, z]
    return out


def bootstrap_lift(zone, sides, cover, kind, n_boot, rng, n_units):
    """Percentile CIs by resampling CONFIGS -- the independent unit."""
    n_cfg = zone.shape[0]
    flat = zone.reshape(n_cfg, -1)
    idx_by_side = {s: np.flatnonzero(sides == s) for s in range(4)}

    # One multinomial draw over ALL configs per replicate, then split by side, so
    # each replicate is a coherent resample of the whole corpus.
    W = rng.multinomial(n_cfg, np.full(n_cfg, 1.0 / n_cfg), size=n_boot
                        ).astype(np.float32)
    boot_side = {}
    for s, idx in idx_by_side.items():
        if len(idx) == 0:
            boot_side[s] = np.zeros((n_boot, N_PIECES, n_units), np.float32)
            continue
        boot_side[s] = (W[:, idx] @ flat[idx]).reshape(n_boot, N_PIECES, n_units)

    lo = np.full((N_PIECES, n_units), np.nan)
    hi = np.full((N_PIECES, n_units), np.nan)
    for z in range(n_units):
        S = [s for s in range(4) if cover[s, z]]
        if not S:
            continue
        sub = sum(boot_side[s] for s in S)
        lz = lift_from_counts(sub, kind)[:, :, z]            # [B, piece]
        with np.errstate(invalid="ignore"):
            keep = np.isfinite(lz).any(0)                     # pieces ever placed
            lo[keep, z] = np.nanpercentile(lz[:, keep], 2.5, axis=0)
            hi[keep, z] = np.nanpercentile(lz[:, keep], 97.5, axis=0)
    return lo, hi


def bootstrap_far_score(rband, sides, cover_rb, kind, n_boot, rng):
    """log2( lift(rows 13-15) / lift(rows 0-2) ), point estimate + CI.

    The two bands are seen by different side sets (the far band by 1,2,3; the
    near band by 0,1,3), which is exactly why each is normalised against an
    average piece of its own kind within its own sides before the ratio is
    taken -- the normalisation is what makes them comparable.
    """
    n_cfg = rband.shape[0]
    flat = rband.reshape(n_cfg, -1)
    idx_by_side = {s: np.flatnonzero(sides == s) for s in range(4)}
    W = rng.multinomial(n_cfg, np.full(n_cfg, 1.0 / n_cfg), size=n_boot
                        ).astype(np.float32)

    def score_from(get_side):
        vals = []
        for band in (2, 0):                                  # far, near
            S = [s for s in range(4) if cover_rb[s, band]]
            sub = sum(get_side(s) for s in S)
            vals.append(lift_from_counts(sub, kind)[:, :, band])
        far, near = vals
        with np.errstate(divide="ignore", invalid="ignore"):
            return np.log2(np.maximum(far, 1e-6) / np.maximum(near, 1e-6))

    point = score_from(lambda s: rband[idx_by_side[s]].sum(0)[None]
                       if len(idx_by_side[s]) else np.zeros((1, N_PIECES, N_RB)))[0]
    boot_side = {}
    for s, idx in idx_by_side.items():
        boot_side[s] = ((W[:, idx] @ flat[idx]).reshape(n_boot, N_PIECES, N_RB)
                        if len(idx) else np.zeros((n_boot, N_PIECES, N_RB), np.float32))
    bs = score_from(lambda s: boot_side[s])
    keep = np.isfinite(bs).any(0)
    blo = np.full(N_PIECES, np.nan); bhi = np.full(N_PIECES, np.nan)
    blo[keep] = np.nanpercentile(bs[:, keep], 2.5, axis=0)
    bhi[keep] = np.nanpercentile(bs[:, keep], 97.5, axis=0)
    return point, blo, bhi


# --------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("corpus")
    ap.add_argument("--out_dir", default="frame_stats")
    ap.add_argument("--seed_file", default=None)
    ap.add_argument("--n_boot", type=int, default=2000)
    ap.add_argument("--min_obs", type=float, default=10.0,
                    help="weighted observations a piece needs to be ranked")
    ap.add_argument("--exclude_top", type=int, default=12,
                    help="cap on the suggested --exclude_pieces list (default 12; "
                         "the hard ceiling is the piece budget, see --ab_stop_row)")
    ap.add_argument("--ab_stop_row", type=int, default=12,
                    help="the --stop_row the exclusion list will be TESTED at. "
                         "Sets the piece budget: the beam must still be able to "
                         "fill rows 1..R from what is left (default 12)")
    ap.add_argument("--rng_seed", type=int, default=12345)
    ap.add_argument("--n_perm", type=int, default=20,
                    help="label-shuffle replicates used to calibrate how many "
                         "'significant' pieces pure noise would produce (0 = skip)")
    ap.add_argument("--no_strict_frame", action="store_true")
    args = ap.parse_args(argv)

    seed = V.load_seed(V.find_seed(args.seed_file))
    kind = np.array([piece_kind(seed[p]) for p in range(N_PIECES)])

    print("[load] reading corpus...")
    (cids, sides, n_boards, zone, rband, cellc, sidec,
     n_rows, off_frame) = load_corpus(args.corpus, strict=not args.no_strict_frame)
    n_cfg = len(cids)
    if n_cfg == 0:
        sys.exit("[stats] no usable boards")

    w_per_board = np.repeat(1.0 / n_boards, n_boards)
    n_eff = w_per_board.sum() ** 2 / (w_per_board ** 2).sum()
    print(f"[load] {int(n_boards.sum())} boards / {n_cfg} borders / "
          f"N_eff {n_eff:.0f}  (boards grouped by bottom row, which is the "
          f"independent unit)")

    cover_z, frac_z = measure_coverage(sidec, "zone")
    cover_rb, frac_rb = measure_coverage(sidec, "rowband")
    print("[cover] zone -> sides that observed it")
    for z, nm in enumerate(ZONE_NAMES):
        S = [s for s in range(4) if cover_z[s, z]]
        print(f"[cover]   {nm}  sides {S}")

    per_side_z = {s: zone[sides == s].sum(0) if (sides == s).any()
                  else np.zeros((N_PIECES, N_ZONES)) for s in range(4)}

    print(f"[boot] {args.n_boot} resamples over {n_cfg} configs...")
    rng = np.random.default_rng(args.rng_seed)
    lift = pooled_lift(per_side_z, cover_z, kind, N_ZONES)
    lo, hi = bootstrap_lift(zone, sides, cover_z, kind, args.n_boot, rng, N_ZONES)
    far, far_lo, far_hi = bootstrap_far_score(rband, sides, cover_rb, kind,
                                              args.n_boot, rng)

    obs = zone.sum(0).sum(1)                                 # weighted, per piece

    # -- positive control: the pinned pieces must come back at their own zones --
    control = []
    for cell, p in CLUE_CELL_OF.items():
        z = zone_of(cell)
        control.append({"piece": int(p), "zone": ZONE_NAMES[z],
                        "lift": None if np.isnan(lift[p, z]) else round(float(lift[p, z]), 2),
                        "ok": bool(np.isnan(lift[p, z]) or lift[p, z] > 1.5)})
    ctrl_ok = all(c["ok"] for c in control)

    # Pieces worth ranking at all: enough observations, not a pinned corner, not
    # a pinned clue. Defined here because the replication check needs it too --
    # a pinned piece agrees with itself perfectly and would inflate every rho.
    rankable = (obs >= args.min_obs) & (kind < 2) & ~np.isin(
        np.arange(N_PIECES), sorted(CLUE_PIECES))

    # -- cross-side replication: every corner zone is seen by exactly two sides --
    repl = {}
    for z, nm in enumerate(ZONE_NAMES):
        S = [s for s in range(4) if cover_z[s, z]]
        if len(S) < 2:
            continue
        pairs = []
        for i in range(len(S)):
            for j in range(i + 1, len(S)):
                a = lift_from_counts(per_side_z[S[i]][None], kind)[0, :, z]
                b = lift_from_counts(per_side_z[S[j]][None], kind)[0, :, z]
                m = (rankable & (obs >= args.min_obs)
                     & np.isfinite(a) & np.isfinite(b))
                if m.sum() >= 10:
                    pairs.append({"sides": [int(S[i]), int(S[j])],
                                  "rho": round(spearman(a[m], b[m]), 4),
                                  "n": int(m.sum())})
        if pairs:
            repl[nm] = pairs
    corner_rhos = [p["rho"] for nm in CORNER_ZONES for p in repl.get(nm, [])
                   if np.isfinite(p["rho"])]
    corner_rho = float(np.mean(corner_rhos)) if corner_rhos else float("nan")

    # -- is that a lot? ---------------------------------------------------------
    # "116 of 247 pieces have a preference" means nothing until you know what the
    # same procedure returns when there is provably no preference to find. So run
    # it again on corpora where piece identity has been shuffled WITHIN each
    # board, among pieces of the same kind: every board keeps its shape, its
    # occupied cells and its per-kind counts, and only the labels move. Anything
    # the procedure still calls significant is the multiplicity of 247 pieces x 9
    # zones leaking through.
    null_sig, null_rho = [], []
    if args.n_perm > 0:
        print(f"[null] {args.n_perm} label-shuffle replicates for calibration...")
        prng = np.random.default_rng(args.rng_seed + 777)
        free = np.ones(N_PIECES, bool); free[sorted(CLUE_PIECES)] = False
        groups = [np.flatnonzero((kind == k) & free) for k in (0, 1)]
        rowsel = np.arange(n_cfg)[:, None]
        for _ in range(args.n_perm):
            zp = zone.copy()
            for g in groups:
                # INDEPENDENTLY per config. One global permutation would only
                # rename the pieces: both sides' lift vectors would be reordered
                # the same way, leaving every correlation and every count exactly
                # as observed. It has to break the piece-to-position link, which
                # means a different shuffle in every config.
                perm = prng.random((n_cfg, len(g))).argsort(1)
                zp[:, g, :] = zone[rowsel, g[perm], :]
            ps = {s_: zp[sides == s_].sum(0) if (sides == s_).any()
                  else np.zeros((N_PIECES, N_ZONES)) for s_ in range(4)}
            lf = pooled_lift(ps, cover_z, kind, N_ZONES)
            l2, _ = bootstrap_lift(zp, sides, cover_z, kind,
                                   max(200, args.n_boot // 8), rng, N_ZONES)
            null_sig.append(int(((l2 > 1.0) & rankable[:, None]).any(1).sum()))
            rs = []
            for z in [ZONE_NAMES.index(c) for c in CORNER_ZONES]:
                S_ = [s_ for s_ in range(4) if cover_z[s_, z]]
                a = lift_from_counts(ps[S_[0]][None], kind)[0, :, z]
                b = lift_from_counts(ps[S_[1]][None], kind)[0, :, z]
                m = rankable & np.isfinite(a) & np.isfinite(b)
                if m.sum() >= 10:
                    rs.append(spearman(a[m], b[m]))
            if rs:
                null_rho.append(float(np.mean(rs)))

    # -- is the exclusion list one side's opinion? ------------------------------
    # The far band (rows 13-15) is seen by three sides. If the score is driven by
    # just one of them it is a property of that search direction, not the board.
    far_by_side = {}
    for s_ in range(4):
        if not cover_rb[s_, 2] or not (sides == s_).any():
            continue
        sub = rband[sides == s_].sum(0)[None]
        v = lift_from_counts(sub, kind)[0]
        with np.errstate(divide="ignore", invalid="ignore"):
            far_by_side[s_] = np.log2(np.maximum(v[:, 2], 1e-6)
                                      / np.maximum(v[:, 0], 1e-6))
    far_agree = []
    ks = sorted(far_by_side)
    for i in range(len(ks)):
        for j in range(i + 1, len(ks)):
            a, b = far_by_side[ks[i]], far_by_side[ks[j]]
            m = rankable & np.isfinite(a) & np.isfinite(b)
            if m.sum() >= 10:
                far_agree.append({"sides": [ks[i], ks[j]],
                                  "rho": round(spearman(a[m], b[m]), 4),
                                  "n": int(m.sum())})

    # -- colours per zone: the mechanism behind any piece-level preference ------
    # A piece cannot prefer a corner for its own sake; it prefers it because the
    # colours it carries are the ones that corner's pinned pieces demand. Colours
    # are 22 buckets against 256, so this is far better estimated than the piece
    # level and says whether the piece-level pattern has a mechanism under it.
    ncol = 23
    zsum = zone.sum(0)                                       # [piece, zone]
    col_z = np.zeros((ncol, N_ZONES))
    for p in range(N_PIECES):
        if kind[p] == 2:
            continue
        for e in seed[p]:
            if e > 0:
                col_z[e] += zsum[p]
    col_tot = col_z.sum(1, keepdims=True)
    zone_share = col_z.sum(0) / max(col_z.sum(), 1e-12)
    with np.errstate(divide="ignore", invalid="ignore"):
        col_lift = (col_z / np.maximum(col_tot, 1e-12)) / np.maximum(zone_share, 1e-12)

    # -- the actionable list ---------------------------------------------------
    #
    # There is a hard ceiling on how many pieces may be excluded, and it is not a
    # statistical one. Rows 1..14 hold 14x14 = 196 inner cells and there are
    # exactly 196 inner pieces, so the puzzle has no spare inner pieces at all.
    # A beam to row R places R*14 of them; two more (the row-13 clues) are
    # reserved and never searched. So:
    #
    #     slack = 196 - 2 - R*14      R=10 -> 54,  R=11 -> 40,  R=12 -> 26
    #
    # Excluding K pieces cuts the slack to slack-K. Take it to zero and the beam
    # has exactly one way to fill the board and almost certainly cannot; take it
    # near zero and the run collapses for a reason that has nothing to do with
    # whether these statistics are right. Half the slack is the cap used here.
    inner_budget = 196 - 2 - args.ab_stop_row * 14
    hard_cap = max(1, inner_budget // 2)
    eff_top = min(args.exclude_top, hard_cap)
    sig_far = rankable & np.isfinite(far_lo) & (far_lo > 0)
    order = np.argsort(-np.where(sig_far, far, -np.inf))
    exclude = [int(p) for p in order[:eff_top] if sig_far[p]]

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out / "arrays.npz",
                        per_side_zone=np.stack([per_side_z[s_] for s_ in range(4)]),
                        lift=lift, lo=lo, hi=hi, obs=obs, kind=kind,
                        far=far, far_lo=far_lo, far_hi=far_hi,
                        cellc=cellc, sidec=sidec, cover_z=cover_z,
                        frac_z=frac_z, col_lift=col_lift, col_z=col_z,
                        rankable=rankable, sides=sides, n_boards=n_boards)

    import csv as _csv
    with (out / "zones.csv").open("w", newline="") as fh:
        w_ = _csv.writer(fh, lineterminator="\n")
        w_.writerow(["piece", "kind", "obs", "best_zone", "best_lift",
                     "far_score", "far_lo", "far_hi"]
                    + [f"lift_{z}" for z in ZONE_NAMES]
                    + [f"lo_{z}" for z in ZONE_NAMES]
                    + [f"hi_{z}" for z in ZONE_NAMES])
        for p in np.flatnonzero(rankable):
            row = lift[p]
            bz = int(np.nanargmax(row)) if np.isfinite(row).any() else 0
            w_.writerow([p, KIND_NAMES[kind[p]], round(float(obs[p]), 2),
                         ZONE_NAMES[bz], round(float(row[bz]), 3),
                         round(float(far[p]), 3), round(float(far_lo[p]), 3),
                         round(float(far_hi[p]), 3)]
                        + [("" if np.isnan(v) else round(float(v), 3)) for v in lift[p]]
                        + [("" if np.isnan(v) else round(float(v), 3)) for v in lo[p]]
                        + [("" if np.isnan(v) else round(float(v), 3)) for v in hi[p]])

    with (out / "replication.csv").open("w", newline="") as fh:
        w_ = _csv.writer(fh, lineterminator="\n")
        w_.writerow(["zone", "side_a", "side_b", "spearman_rho", "n_pieces"])
        for nm, ps in repl.items():
            for pr in ps:
                w_.writerow([nm, pr["sides"][0], pr["sides"][1], pr["rho"], pr["n"]])

    (out / "exclude_pieces.txt").write_text(
        (",".join(str(p) for p in exclude) if exclude else "") + "\n")

    top_by_zone = {}
    for z, nm in enumerate(ZONE_NAMES):
        cand = [p for p in np.flatnonzero(rankable)
                if np.isfinite(lift[p, z]) and lo[p, z] > 1.0]
        cand.sort(key=lambda p: -lift[p, z])
        top_by_zone[nm] = [{"piece": int(p), "lift": round(float(lift[p, z]), 2),
                            "lo": round(float(lo[p, z]), 2),
                            "hi": round(float(hi[p, z]), 2),
                            "kind": KIND_NAMES[kind[p]]} for p in cand[:15]]

    summary = {
        "corpus": str(args.corpus),
        "boards": int(n_boards.sum()), "boards_seen": n_rows,
        "off_frame": off_frame, "configs": n_cfg, "n_eff": round(float(n_eff), 1),
        "n_boot": args.n_boot,
        "per_side_configs": {str(s): int((sides == s).sum()) for s in range(4)},
        "per_side_boards": {str(s): int(n_boards[sides == s].sum()) for s in range(4)},
        "zone_names": ZONE_NAMES, "zone_label": ZONE_LABEL,
        "coverage": {ZONE_NAMES[z]: [s for s in range(4) if cover_z[s, z]]
                     for z in range(N_ZONES)},
        "control": control, "control_ok": ctrl_ok,
        "replication": repl, "corner_replication_rho": round(corner_rho, 4),
        "top_by_zone": top_by_zone,
        "n_significant": int(((lo > 1.0) & rankable[:, None]).any(1).sum()),
        "n_rankable": int(rankable.sum()),
        "exclude_suggestion": exclude,
        "far_significant": int(sig_far.sum()),
        "far_agreement": far_agree,
        "null_significant": null_sig,
        "null_significant_mean": round(float(np.mean(null_sig)), 1) if null_sig else None,
        "null_rho": null_rho,
        "null_rho_mean": round(float(np.mean(null_rho)), 4) if null_rho else None,
        "ab_stop_row": args.ab_stop_row,
        "inner_slack": inner_budget,
        "exclude_cap": hard_cap,
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=1))

    # -- report ----------------------------------------------------------------
    print()
    print(f"[check] pinned clue pieces recovered at their own zones: "
          f"{'all ok' if ctrl_ok else 'FAILED'}")
    print(f"[repl ] corner-zone cross-side agreement: rho = {corner_rho:+.3f}")
    for nm in CORNER_ZONES:
        for pr in repl.get(nm, []):
            print(f"[repl ]   {nm}  sides {pr['sides'][0]} vs {pr['sides'][1]}: "
                  f"rho {pr['rho']:+.3f}  (n={pr['n']})")
    print()
    print(f"[zones] {summary['n_significant']} of {summary['n_rankable']} pieces "
          f"have a zone preference whose 95% CI clears 1.00")
    if null_sig:
        print(f"[zones] label-shuffled control: {np.mean(null_sig):.0f} "
              f"(range {min(null_sig)}-{max(null_sig)}) over {len(null_sig)} "
              f"replicates, corner rho {np.mean(null_rho):+.3f}"
              if null_rho else "")
    for nm in CORNER_ZONES:
        t = top_by_zone[nm][:6]
        if t:
            print(f"[zones] {nm}: " + "  ".join(
                f"{d['piece']}({d['lift']:.1f}x)" for d in t))
    print()
    print(f"[far  ] {int(sig_far.sum())} pieces are significantly far-side "
          f"(rows 13-15 over rows 0-2, CI above 0)")
    print(f"[budget] a beam to row {args.ab_stop_row} needs {args.ab_stop_row*14} "
          f"of the 194 searchable inner pieces, so only {inner_budget} are spare; "
          f"excluding at most {hard_cap} keeps half that slack")
    for fa in far_agree:
        print(f"[far  ] sides {fa['sides'][0]} vs {fa['sides'][1]} agree on the "
              f"far-side score: rho {fa['rho']:+.3f}")
    if exclude:
        print(f"[far  ] excluding top {len(exclude)}: "
              f"{','.join(str(p) for p in exclude)}")
    print()
    print(f"[out  ] {out}/zones.csv  replication.csv  exclude_pieces.txt  "
          f"summary.json  arrays.npz")
    return 0


if __name__ == "__main__":
    sys.exit(main())
