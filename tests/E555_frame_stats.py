#!/usr/bin/env python3
"""
E555_frame_stats.py -- does any piece actually prefer a region of the board?

WHY

    tests/run_fixedframe_farm.sh produces a corpus of row-11 partials that all
    share one frame: the published clue set at orientation 0, and one fixed
    assignment of the four corner pieces. Four sides, each a 12-row band, turned
    onto that frame. Nothing in it is a solution. The question is whether the
    pieces nonetheless land somewhere systematic -- and if so, whether that can
    be fed back into the search.

    Two statistics, both chosen because they are estimable from a few thousand
    boards and because something can be DONE with them:

    ring affinity   ring(r,c) = min(r, c, 15-r, 15-c). Ring 0 is the frame, so
                    the 196 inner pieces live in rings 1..7 -- and rings 1..7
                    hold exactly 196 cells. "Outer three rows" is rings 1-2,
                    "the core" is rings 5-7. Far fewer buckets than 256 cells,
                    so the counts per bucket are large enough to mean something.

    top-band        P(piece lands in canonical rows 12..15). This is the one with
                    a use: the beamer fills rows 0..11 and dies attempting 12, so
                    a piece that belongs up there is a piece the search should
                    not be spending low down. --exclude_pieces bars it from the
                    database outright; this script writes the candidate list.

WHAT IT DOES NOT CLAIM

    These are the beam's habits, not the puzzle's truth. The heuristic has its
    own taste and the corpus inherits it. Two things are done about that, both
    cheap and neither a theory:

    per-config weighting    Boards from one border config share that border
                            EXACTLY -- they are one observation with variations,
                            not N observations. Each board is weighted
                            1/(boards from its config), so every border counts
                            once. The reported N_eff (Kish) is the honest size of
                            the corpus; it is much smaller than the board count
                            and that is the point.

    cross-side agreement    The four sides use different RNG, different borders
                            and attack from different directions, so they share
                            very little search bias. If the per-piece ranking
                            reproduces across them, something real is driving it.
                            If it does not, nothing is -- and no amount of
                            further analysis will change that. This is the
                            headline number, printed first. Spearman rank
                            correlation, so a per-side coverage difference (each
                            side sees a different band) shifts every piece the
                            same way and cancels out of the ranking.

                            It is reported twice: over all observed cells, and
                            over the 8x8 core alone, which is the only region all
                            four sides cover. If those two disagree, believe the
                            core.

USAGE

    python3 tests/E555_frame_stats.py ff_out/corpus.csv --out_dir ff_out/stats

    Writes pieces.csv (one row per piece, sortable), agreement.csv, the
    suggested --exclude_pieces list, and summary.json for the dashboard.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import E555_viewer as V                  # seed loading, row parsing  # noqa: E402

SIDE = V.SIDE                            # 16
N_PIECES = V.N_PIECES                    # 256
UNPLACED = 999

# The canonical frame this corpus is supposed to be in: the orientation-0 centre
# clue. Every board must agree, or it was never turned onto the frame and every
# number below would be averaging different coordinate systems.
CANON_ORIENT = 0
CANON_CLUE_CELL, CANON_CLUE_PIECE, CANON_CLUE_SPIN = V.clue_list(
    CANON_ORIENT, V.CLUE_CENTER)[0]

# The five clue pieces are PINNED by the frame, so their "preference" is the
# command line, not a finding. Two of them (the row-13 pair) are not even
# searched -- format_board_tail attaches them to the emitted board. They are
# dropped from every ranking and used instead as a positive control: the
# machinery must recover them at their own cells, or it is broken.
CLUE_CELL_OF = {p: cell for cell, p, _s in V.clue_list(CANON_ORIENT)}
CLUE_PIECES = set(CLUE_CELL_OF)

# The band the beam never fills. Four sides cover it between them; a piece that
# concentrates here is a candidate for --exclude_pieces.
TOP_BAND_ROW = 12

# The only region all four sides observe: rows 4..11 x cols 4..11, i.e. rings 4-7.
CORE_LO, CORE_HI = 4, 11


def ring_of(cell):
    r, c = divmod(cell, SIDE)
    return min(r, c, SIDE - 1 - r, SIDE - 1 - c)


def in_core(cell):
    r, c = divmod(cell, SIDE)
    return CORE_LO <= r <= CORE_HI and CORE_LO <= c <= CORE_HI


def piece_kind(edges):
    """corner / edge / inner, from how many frame-grey sides the piece carries."""
    greys = sum(1 for e in edges if e == 0)
    return "corner" if greys == 2 else "edge" if greys == 1 else "inner"


def side_of(config_id):
    """The side a board came from, off its config id: s<N>_<runtag>_b<i>l<j>."""
    if config_id.startswith("s") and "_" in config_id:
        head = config_id[1:config_id.index("_")]
        if head.isdigit():
            return int(head)
    return -1


def spearman(xs, ys):
    """Rank correlation, ties averaged. Returns None for fewer than 3 points."""
    n = len(xs)
    if n < 3:
        return None

    def ranks(vals):
        order = sorted(range(n), key=lambda i: vals[i])
        out = [0.0] * n
        i = 0
        while i < n:
            j = i
            while j + 1 < n and vals[order[j + 1]] == vals[order[i]]:
                j += 1
            avg = (i + j) / 2.0 + 1.0
            for k in range(i, j + 1):
                out[order[k]] = avg
            i = j + 1
        return out

    rx, ry = ranks(xs), ranks(ys)
    mx, my = sum(rx) / n, sum(ry) / n
    num = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    dx = math.sqrt(sum((a - mx) ** 2 for a in rx))
    dy = math.sqrt(sum((b - my) ** 2 for b in ry))
    return num / (dx * dy) if dx > 0 and dy > 0 else None


class Accum:
    """Weighted placement counts. One of these per side, plus one pooled."""

    def __init__(self):
        self.ring = defaultdict(float)       # (piece, ring) -> weight
        self.cell = defaultdict(float)       # (piece, cell) -> weight
        self.ring_total = defaultdict(float)  # ring -> weight over inner pieces
        self.core_ring = defaultdict(float)  # (piece, ring) -> weight, core only
        self.top_band = defaultdict(float)   # piece -> weight in rows >= 12
        self.seen = defaultdict(float)       # piece -> total weight placed
        self.top_band_total = 0.0
        self.placed_total = 0.0

    def add(self, piece, cell, w, is_inner):
        rg = ring_of(cell)
        self.cell[(piece, cell)] += w
        self.seen[piece] += w
        if not is_inner:
            return
        self.ring[(piece, rg)] += w
        self.ring_total[rg] += w
        self.placed_total += w
        if in_core(cell):
            self.core_ring[(piece, rg)] += w
        if cell // SIDE >= TOP_BAND_ROW:
            self.top_band[piece] += w
            self.top_band_total += w

    def mean_ring(self, piece, core_only=False):
        src = self.core_ring if core_only else self.ring
        tot = sum(src.get((piece, k), 0.0) for k in range(1, 8))
        if tot <= 0:
            return None
        return sum(k * src.get((piece, k), 0.0) for k in range(1, 8)) / tot


def load_corpus(path, strict):
    """Group boards by config id, checking every one is on the canonical frame."""
    by_config = defaultdict(list)
    off_frame = 0
    total = 0
    for _, cid, _sol, pos, rot in V.iter_records(path):
        total += 1
        if (pos[CANON_CLUE_PIECE] != CANON_CLUE_CELL
                or rot[CANON_CLUE_PIECE] != CANON_CLUE_SPIN):
            off_frame += 1
            continue
        by_config[cid].append(pos)
    if off_frame:
        msg = (f"{off_frame} of {total} board(s) are NOT on the canonical frame "
               f"(piece {CANON_CLUE_PIECE} not at row 7 col 7 spin 0). They were "
               f"never turned onto it -- check the rotation step in "
               f"run_fixedframe_farm.sh")
        if strict:
            sys.exit(f"[frame] FATAL: {msg}")
        print(f"[frame] WARNING: {msg}; skipped", file=sys.stderr)
    return by_config, total, off_frame


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Ring and top-band preferences of individual pieces, "
                    "measured over a canonical-frame corpus.")
    ap.add_argument("corpus", help="canonical board CSV (ff_out/corpus.csv)")
    ap.add_argument("--out_dir", default="frame_stats", help="where to write")
    ap.add_argument("--seed_file", default=None, help="piece seed (default: found)")
    ap.add_argument("--exclude_top", type=int, default=20,
                    help="how many pieces the suggested --exclude_pieces list "
                         "holds (default 20)")
    ap.add_argument("--min_obs", type=float, default=5.0,
                    help="weighted observations a piece needs before it is "
                         "ranked at all (default 5)")
    ap.add_argument("--no_strict_frame", action="store_true",
                    help="warn instead of dying when boards are off-frame")
    args = ap.parse_args(argv)

    seed = V.load_seed(V.find_seed(args.seed_file))
    kinds = [piece_kind(seed[p]) for p in range(N_PIECES)]

    by_config, n_rows, off_frame = load_corpus(args.corpus,
                                               strict=not args.no_strict_frame)
    if not by_config:
        sys.exit("[stats] no usable boards in the corpus")

    pooled = Accum()
    per_side = {s: Accum() for s in range(4)}
    weights = []

    for cid, boards in by_config.items():
        # Every border counts once, however many boards it emitted: boards from
        # one config share that border exactly.
        w = 1.0 / len(boards)
        s = side_of(cid)
        for pos in boards:
            weights.append(w)
            for piece, cell in enumerate(pos):
                if cell == UNPLACED:
                    continue
                is_inner = kinds[piece] == "inner"
                pooled.add(piece, cell, w, is_inner)
                if s in per_side:
                    per_side[s].add(piece, cell, w, is_inner)

    sum_w = sum(weights)
    n_eff = (sum_w ** 2) / sum(w * w for w in weights) if weights else 0.0

    # -- ring lift: how much more often this piece is in ring k than any inner
    # piece is. Both sides of the ratio come from the SAME observed cells, so the
    # four sides' different coverage cancels without any exposure model.
    ring_share = {k: (pooled.ring_total.get(k, 0.0) / pooled.placed_total
                      if pooled.placed_total else 0.0) for k in range(1, 8)}
    top_share = (pooled.top_band_total / pooled.placed_total
                 if pooled.placed_total else 0.0)

    # Positive control. Each clue piece is pinned at a known cell, so its
    # recovered mean ring must be that cell's ring, with a large lift. If this
    # does not hold, the corpus is not on the frame and nothing below means
    # anything -- so it is checked, not assumed.
    control = []
    for p in sorted(CLUE_PIECES):
        want = ring_of(CLUE_CELL_OF[p])
        got = pooled.mean_ring(p)
        control.append({"piece": p, "want_ring": want,
                        "got_mean_ring": None if got is None else round(got, 3),
                        "ok": got is not None and abs(got - want) < 1e-6})

    rows = []
    for p in range(N_PIECES):
        if kinds[p] != "inner" or p in CLUE_PIECES:
            continue
        obs = sum(pooled.ring.get((p, k), 0.0) for k in range(1, 8))
        if obs < args.min_obs:
            continue
        probs = {k: pooled.ring.get((p, k), 0.0) / obs for k in range(1, 8)}
        lift = {k: (probs[k] / ring_share[k]) if ring_share[k] > 0 else 0.0
                for k in range(1, 8)}
        p_top = pooled.top_band.get(p, 0.0) / obs
        best = max(range(1, 8), key=lambda k: lift[k])
        rows.append({
            "piece": p,
            "obs": round(obs, 2),
            "mean_ring": round(pooled.mean_ring(p), 3),
            "p_top_band": round(p_top, 4),
            "top_lift": round(p_top / top_share, 3) if top_share > 0 else 0.0,
            "best_ring": best,
            "best_lift": round(lift[best], 3),
            **{f"p_ring{k}": round(probs[k], 4) for k in range(1, 8)},
            **{f"lift{k}": round(lift[k], 3) for k in range(1, 8)},
        })

    # -- the headline: do the four sides agree on the ranking? -----------------
    agree = {}
    for label, core_only in (("all", False), ("core", True)):
        mat = {}
        for a in range(4):
            for b in range(a + 1, 4):
                xs, ys = [], []
                for p in range(N_PIECES):
                    if kinds[p] != "inner" or p in CLUE_PIECES:
                        continue
                    ma = per_side[a].mean_ring(p, core_only)
                    mb = per_side[b].mean_ring(p, core_only)
                    if ma is not None and mb is not None:
                        xs.append(ma)
                        ys.append(mb)
                rho = spearman(xs, ys)
                mat[f"{a}-{b}"] = {"rho": None if rho is None else round(rho, 4),
                                   "n": len(xs)}
        vals = [m["rho"] for m in mat.values() if m["rho"] is not None]
        agree[label] = {"pairs": mat,
                        "mean_rho": round(sum(vals) / len(vals), 4) if vals else None}

    # -- the actionable list ---------------------------------------------------
    # Pieces most concentrated in canonical rows 12..15 -- the band the beam never
    # fills. Ranked by lift so a piece that is merely common does not qualify.
    ranked_top = sorted((r for r in rows if r["top_lift"] > 1.0),
                        key=lambda r: (-r["top_lift"], -r["obs"]))
    exclude = [r["piece"] for r in ranked_top[:args.exclude_top]]

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    import csv as _csv
    with (out / "pieces.csv").open("w", newline="") as fh:
        if rows:
            w_ = _csv.DictWriter(fh, fieldnames=list(rows[0].keys()),
                                 lineterminator="\n")
            w_.writeheader()
            for r in sorted(rows, key=lambda r: r["mean_ring"]):
                w_.writerow(r)

    with (out / "agreement.csv").open("w", newline="") as fh:
        w_ = _csv.writer(fh, lineterminator="\n")
        w_.writerow(["scope", "side_pair", "spearman_rho", "n_pieces"])
        for label, blk in agree.items():
            for pair, m in blk["pairs"].items():
                w_.writerow([label, pair, m["rho"], m["n"]])

    (out / "exclude_pieces.txt").write_text(
        ",".join(str(p) for p in exclude) + "\n" if exclude else "\n")

    summary = {
        "corpus": str(args.corpus),
        "boards": n_rows,
        "boards_used": sum(len(b) for b in by_config.values()),
        "off_frame": off_frame,
        "configs": len(by_config),
        "n_eff": round(n_eff, 1),
        "ring_share": {str(k): round(v, 5) for k, v in ring_share.items()},
        "top_band_share": round(top_share, 5),
        "control_clue_pieces": control,
        "agreement": agree,
        "pieces": rows,
        "exclude_suggestion": exclude,
        "per_side_configs": {str(s): sum(1 for c in by_config if side_of(c) == s)
                             for s in range(4)},
        # 16x16 weighted occupancy per piece, sparse: only cells a piece actually
        # reached. Full dense would be 256x256 of mostly zeros.
        "cell_map": {str(p): {str(c): round(v, 3)
                              for (pp, c), v in pooled.cell.items() if pp == p}
                     for p in (r["piece"] for r in rows)},
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=1))

    # -- report ----------------------------------------------------------------
    print(f"[stats] {summary['boards_used']} board(s) from {len(by_config)} border "
          f"config(s); N_eff = {n_eff:.1f}")
    print(f"[stats] per-side configs: " +
          " ".join(f"s{s}={summary['per_side_configs'][str(s)]}" for s in range(4)))
    bad = [c for c in control if not c["ok"]]
    print("[check] pinned clue pieces recovered at their own cells: "
          + ("all %d ok" % len(control) if not bad
             else "FAILED for %s" % ", ".join(str(c["piece"]) for c in bad)))
    if bad:
        print("[check] the corpus is not on the canonical frame -- stop here",
              file=sys.stderr)
    print()
    print("[agree] Do the four sides rank pieces the same way? (Spearman, "
          "mean over the 6 side pairs)")
    for label in ("all", "core"):
        mr = agree[label]["mean_rho"]
        print(f"[agree]   {label:5s}: " + ("n/a (a side has too few boards)"
                                           if mr is None else f"rho = {mr:+.3f}"))
    mr = agree["core"]["mean_rho"] or agree["all"]["mean_rho"]
    if mr is None:
        verdict = "not enough data yet -- run the farm longer"
    elif mr > 0.5:
        verdict = "STRONG: the sides agree, the preference is not a search artefact"
    elif mr > 0.2:
        verdict = "WEAK but present -- worth more boards before acting on it"
    else:
        verdict = ("NOTHING THERE: the sides disagree, so the ranking is noise "
                   "or search bias. More analysis will not help; more boards might")
    print(f"[agree]   verdict: {verdict}")
    print()
    if rows:
        by_mean = sorted(rows, key=lambda r: r["mean_ring"])
        print("[rings] most border-loving pieces (low mean ring):")
        for r in by_mean[:8]:
            print(f"[rings]   piece {r['piece']:3d}  mean_ring {r['mean_ring']:.2f}"
                  f"  best ring {r['best_ring']} at {r['best_lift']:.2f}x"
                  f"  (obs {r['obs']:.0f})")
        print("[rings] most core-loving pieces (high mean ring):")
        for r in by_mean[-8:][::-1]:
            print(f"[rings]   piece {r['piece']:3d}  mean_ring {r['mean_ring']:.2f}"
                  f"  best ring {r['best_ring']} at {r['best_lift']:.2f}x"
                  f"  (obs {r['obs']:.0f})")
    print()
    if exclude:
        print(f"[top]   {len(exclude)} piece(s) concentrate in rows {TOP_BAND_ROW}..15 "
              f"-- the band the beam never fills:")
        print(f"[top]   {','.join(str(p) for p in exclude)}")
        print(f"[top]   Test it (NOT with the production beamer -- this list is "
              f"specific to THIS frame):")
        print(f"[top]     bin/E555_beamer_FixedFrame data/seed_Edge5.txt "
              f"--clue_orient 0 --stop_row 12 \\")
        print(f"[top]       --exclude_pieces $(cat {out}/exclude_pieces.txt) "
              f"--db_file excl.db --wall_time 1800")
        print(f"[top]   and compare the count of 'filled=12' sweep lines against "
              f"the same run without --exclude_pieces.")
    else:
        print(f"[top]   no piece concentrates in rows {TOP_BAND_ROW}..15 above "
              f"chance -- nothing to exclude")
    print()
    print(f"[out]   {out}/pieces.csv  {out}/agreement.csv  "
          f"{out}/exclude_pieces.txt  {out}/summary.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
