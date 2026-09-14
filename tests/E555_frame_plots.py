#!/usr/bin/env python3
"""
E555_frame_plots.py -- static figures for the fixed-frame corner study.

    python3 tests/E555_frame_plots.py ff_out/stats --out_dir ff_out/figs

Reads arrays.npz + summary.json written by E555_frame_stats.py and writes one
PNG per figure. Plain matplotlib, no interactivity: these are meant to be looked
at, printed, and pasted into a report.

Every "lift" figure uses the same diverging scale centred on 1.00x (log2 = 0):
red = the piece avoids that zone, grey = no preference, blue = it prefers it.
One scale across every figure, so panels can be compared by eye.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt                                  # noqa: E402
import numpy as np                                               # noqa: E402
from matplotlib.colors import LinearSegmentedColormap, TwoSlopeNorm  # noqa: E402
from matplotlib.patches import Rectangle                         # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import E555_viewer as V                                          # noqa: E402

SIDE = 16
ZONE_NAMES = ["BL", "BM", "BR", "ML", "CC", "MR", "TL", "TM", "TR"]
CORNER_ZONES = ["BL", "BR", "TL", "TR"]
BAND_EDGES = (0, 5, 11, 16)

# Diverging: red (avoids) -> grey (no preference) -> blue (prefers).
CMAP_LIFT = LinearSegmentedColormap.from_list("lift", [
    "#8f1f1f", "#c23434", "#e08585", "#f4cccc", "#f0efec",
    "#cde2fb", "#9ec5f4", "#3987e5", "#1c5cab"])
# Sequential, one hue, light -> dark.
CMAP_SEQ = LinearSegmentedColormap.from_list("seq", [
    "#f4f3f0", "#cde2fb", "#9ec5f4", "#5598e7", "#256abf", "#0d366b"])

INK, INK2, RULE = "#16150f", "#57554a", "#cfcbbd"

plt.rcParams.update({
    "figure.facecolor": "white", "axes.facecolor": "white",
    "savefig.facecolor": "white", "savefig.bbox": "tight", "savefig.dpi": 150,
    "font.size": 9, "axes.labelsize": 9, "axes.titlesize": 10.5,
    "axes.titleweight": "bold", "axes.edgecolor": RULE, "axes.labelcolor": INK2,
    "text.color": INK, "xtick.color": INK2, "ytick.color": INK2,
    "xtick.labelsize": 8, "ytick.labelsize": 8,
    "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False, "figure.autolayout": False,
})


def band_of(v):
    return 0 if v < BAND_EDGES[1] else (1 if v < BAND_EDGES[2] else 2)


def zone_of(cell):
    r, c = divmod(cell, SIDE)
    return band_of(r) * 3 + band_of(c)


def draw_board(ax, values, cmap, vmin=None, vmax=None, norm=None, zone_lines=True):
    """A 16x16 board with row 0 at the BOTTOM, as the whole toolkit draws it."""
    grid = np.asarray(values, float).reshape(SIDE, SIDE)
    im = ax.imshow(grid, origin="lower", cmap=cmap, vmin=vmin, vmax=vmax,
                   norm=norm, interpolation="nearest")
    if zone_lines:
        for e in BAND_EDGES[1:-1]:
            ax.axhline(e - 0.5, color=INK, lw=1.1, alpha=.55)
            ax.axvline(e - 0.5, color=INK, lw=1.1, alpha=.55)
    ax.set_xticks([2, 7.5, 13]); ax.set_xticklabels(["cols 0-4", "5-10", "11-15"])
    ax.set_yticks([2, 7.5, 13]); ax.set_yticklabels(["rows 0-4", "5-10", "11-15"])
    ax.tick_params(length=0)
    for s in ax.spines.values():
        s.set_visible(True)
        s.set_color(RULE)
    return im


def fig_coverage(D, S, out):
    """How many of the four sides ever placed a piece in each cell."""
    sidec = D["sidec"]
    cnt = (sidec > 0).sum(0).astype(float)
    fig, axes = plt.subplots(1, 2, figsize=(9.4, 4.3),
                             gridspec_kw={"width_ratios": [1, 1.15]})

    im = draw_board(axes[0], cnt, CMAP_SEQ, vmin=0, vmax=4)
    axes[0].set_title("Cells observed, by how many sides")
    cb = fig.colorbar(im, ax=axes[0], fraction=.046, pad=.04, ticks=[0, 1, 2, 3, 4])
    cb.outline.set_edgecolor(RULE)
    cb.set_label("sides", color=INK2)

    ax = axes[1]
    names = ["side 0\nrows 0-10", "side 1\ncols 5-15",
             "side 2\nrows 5-15", "side 3\ncols 0-10"]
    grid = np.zeros((4, 9))
    for s in range(4):
        for z in range(9):
            grid[s, z] = D["frac_z"][s, z]
    im = ax.imshow(grid, cmap=CMAP_SEQ, vmin=0, vmax=1, aspect="auto",
                   interpolation="nearest")
    ax.set_xticks(range(9)); ax.set_xticklabels(ZONE_NAMES)
    ax.set_yticks(range(4)); ax.set_yticklabels(names, fontsize=7.5)
    ax.set_title("Fraction of each zone a side reaches")
    for s in range(4):
        for z in range(9):
            v = grid[s, z]
            if v > 0.02:
                ax.text(z, s, f"{v:.0%}", ha="center", va="center", fontsize=7,
                        color="white" if v > .55 else INK)
    ax.tick_params(length=0)
    fig.colorbar(im, ax=ax, fraction=.046, pad=.04).outline.set_edgecolor(RULE)
    fig.suptitle("Each zone is wholly inside a side's band or wholly outside it",
                 fontsize=11, fontweight="bold", y=1.02)
    fig.savefig(out / "fig_coverage.png"); plt.close(fig)


def fig_zone_heatmap(D, S, out):
    """Every rankable piece x 9 zones, sorted by which zone it prefers."""
    lift, rank_m = D["lift"], D["rankable"]
    idx = np.flatnonzero(rank_m)
    L = np.log2(np.where(np.isfinite(lift[idx]) & (lift[idx] > 0), lift[idx], np.nan))
    best = np.nanargmax(np.where(np.isfinite(L), L, -np.inf), axis=1)
    strength = np.nanmax(np.where(np.isfinite(L), L, -np.inf), axis=1)
    order = np.lexsort((-strength, best))

    fig, ax = plt.subplots(figsize=(6.6, 9.2))
    norm = TwoSlopeNorm(vmin=-1.6, vcenter=0.0, vmax=1.6)
    im = ax.imshow(L[order], cmap=CMAP_LIFT, norm=norm, aspect="auto",
                   interpolation="nearest")
    ax.set_xticks(range(9)); ax.set_xticklabels(ZONE_NAMES)
    ax.set_yticks([]); ax.set_ylabel(f"{len(idx)} pieces, grouped by preferred zone")
    # A rule between groups makes the block structure legible without a legend.
    b = best[order]
    for i in range(1, len(b)):
        if b[i] != b[i - 1]:
            ax.axhline(i - 0.5, color=INK, lw=.7, alpha=.5)
            ax.text(9.05, i - 0.5, ZONE_NAMES[b[i]], va="center", fontsize=7.5,
                    color=INK2)
    ax.text(9.05, -0.5, ZONE_NAMES[b[0]], va="center", fontsize=7.5, color=INK2)
    ax.set_title("Each piece's zone preference\n(blue = prefers, red = avoids)")
    cb = fig.colorbar(im, ax=ax, fraction=.05, pad=.10,
                      ticks=[-1.585, -1, 0, 1, 1.585])
    cb.ax.set_yticklabels(["0.33x", "0.5x", "1x", "2x", "3x"])
    cb.outline.set_edgecolor(RULE)
    fig.savefig(out / "fig_zone_heatmap.png"); plt.close(fig)


def fig_corner_tops(D, S, out):
    """Top pieces for each corner, with bootstrap 95% intervals."""
    lift, lo, hi, kind = D["lift"], D["lo"], D["hi"], D["kind"]
    fig, axes = plt.subplots(2, 2, figsize=(10.2, 8.2))
    order = [("TL", axes[0, 0]), ("TR", axes[0, 1]),
             ("BL", axes[1, 0]), ("BR", axes[1, 1])]
    for nm, ax in order:
        z = ZONE_NAMES.index(nm)
        rows = S["top_by_zone"].get(nm, [])[:12][::-1]
        if not rows:
            ax.set_axis_off(); ax.set_title(f"{nm}: nothing significant"); continue
        y = np.arange(len(rows))
        val = [r["lift"] for r in rows]
        err = [[r["lift"] - r["lo"] for r in rows], [r["hi"] - r["lift"] for r in rows]]
        cols = ["#3987e5" if r["kind"] == "inner" else "#eb6834" for r in rows]
        ax.barh(y, val, color=cols, height=.62, zorder=2)
        ax.errorbar(val, y, xerr=err, fmt="none", ecolor=INK2, elinewidth=1,
                    capsize=2.5, zorder=3)
        ax.axvline(1.0, color=INK, lw=1, ls="--", alpha=.6, zorder=1)
        ax.set_yticks(y)
        ax.set_yticklabels([f"{r['piece']}" for r in rows], fontsize=8)
        ax.set_xlabel("times more often than an average piece of its kind")
        ax.set_xlim(0, max(3.0, max(r["hi"] for r in rows) * 1.05))
        ax.set_title(f"{nm} — {S['zone_label'][nm]}"
                     f"  (sides {S['coverage'][nm]})")
        ax.grid(axis="x", color=RULE, lw=.6, alpha=.7, zorder=0)
        ax.set_axisbelow(True)
    fig.legend(handles=[plt.Line2D([], [], marker="s", ls="", color="#3987e5",
                                   label="inner piece"),
                        plt.Line2D([], [], marker="s", ls="", color="#eb6834",
                                   label="edge piece")],
               loc="lower center", ncol=2, bbox_to_anchor=(.5, -.02))
    fig.suptitle("Which pieces the search puts near each corner",
                 fontsize=12, fontweight="bold")
    fig.tight_layout(rect=(0, .03, 1, .97))
    fig.savefig(out / "fig_corner_tops.png"); plt.close(fig)


def fig_replication(D, S, out):
    """Two independent sides see each corner. Do they agree, piece by piece?"""
    per = D["per_side_lift"]                      # [4, pieces, 9]
    rank_m, obs = D["rankable"], D["obs"]
    fig, axes = plt.subplots(1, 4, figsize=(13.2, 3.7))
    for ax, nm in zip(axes, CORNER_ZONES):
        z = ZONE_NAMES.index(nm)
        sides = S["coverage"][nm]
        a, b = per[sides[0], :, z], per[sides[1], :, z]
        m = rank_m & np.isfinite(a) & np.isfinite(b)
        ax.scatter(a[m], b[m], s=9, alpha=.45, color="#2a78d6", linewidths=0)
        hi = float(np.nanpercentile(np.concatenate([a[m], b[m]]), 99.5)) * 1.05
        ax.plot([0, hi], [0, hi], color=INK, lw=.9, ls="--", alpha=.55)
        ax.axhline(1, color=RULE, lw=.7); ax.axvline(1, color=RULE, lw=.7)
        rho = [p["rho"] for p in S["replication"].get(nm, [])]
        ax.set_title(f"{nm}   " + (f"$\\rho$ = {rho[0]:+.3f}" if rho else ""))
        ax.set_xlabel(f"lift from side {sides[0]}")
        ax.set_ylabel(f"lift from side {sides[1]}")
        ax.set_xlim(0, hi); ax.set_ylim(0, hi)
    fig.suptitle("Independent replication: the two sides that see each corner, "
                 "one point per piece", fontsize=11.5, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, .93))
    fig.savefig(out / "fig_replication.png"); plt.close(fig)


def fig_colors(D, S, out):
    """Colour composition per zone -- the mechanism under any piece preference."""
    cl = D["col_lift"][1:23]                       # colours 1..22
    fig, ax = plt.subplots(figsize=(7.6, 6.4))
    L = np.log2(np.where(cl > 0, cl, np.nan))
    norm = TwoSlopeNorm(vmin=-.45, vcenter=0.0, vmax=.45)
    im = ax.imshow(L, cmap=CMAP_LIFT, norm=norm, aspect="auto",
                   interpolation="nearest")
    ax.set_xticks(range(9)); ax.set_xticklabels(ZONE_NAMES)
    ax.set_yticks(range(22)); ax.set_yticklabels([str(c) for c in range(1, 23)],
                                                 fontsize=7.5)
    ax.set_ylabel("edge colour")
    ax.tick_params(length=0)
    for i in range(22):
        for j in range(9):
            if np.isfinite(L[i, j]) and abs(L[i, j]) > .18:
                ax.text(j, i, f"{cl[i, j]:.2f}", ha="center", va="center",
                        fontsize=6.2, color="white" if abs(L[i, j]) > .34 else INK)
    ax.set_title("Where each edge colour ends up\n"
                 "(a piece prefers a corner because of the colours it carries)")
    cb = fig.colorbar(im, ax=ax, fraction=.046, pad=.04,
                      ticks=[-.415, -.19, 0, .19, .415])
    cb.ax.set_yticklabels(["0.75x", "0.88x", "1x", "1.14x", "1.33x"])
    cb.outline.set_edgecolor(RULE)
    fig.savefig(out / "fig_colors.png"); plt.close(fig)


def fig_piece_maps(D, S, out, n=8):
    """Where the most corner-specific pieces actually landed."""
    lift, rank_m, cellc = D["lift"], D["rankable"], D["cellc"]
    zc = [ZONE_NAMES.index(c) for c in CORNER_ZONES]
    picks = []
    for z in zc:
        cand = [p for p in np.flatnonzero(rank_m)
                if np.isfinite(lift[p, z]) and D["lo"][p, z] > 1.0]
        cand.sort(key=lambda p: -lift[p, z])
        picks += [(p, z) for p in cand[:n // 4]]
    if not picks:
        return
    ncol = 4
    nrow = int(np.ceil(len(picks) / ncol))
    fig, axes = plt.subplots(nrow, ncol, figsize=(3.0 * ncol, 3.15 * nrow))
    axes = np.atleast_1d(axes).ravel()
    for ax, (p, z) in zip(axes, picks):
        m = cellc[p].astype(float)
        draw_board(ax, m / max(m.max(), 1e-9), CMAP_SEQ, vmin=0, vmax=1)
        ax.set_title(f"piece {p} — {ZONE_NAMES[z]} {lift[p, z]:.1f}x",
                     fontsize=9.5)
        ax.set_xticks([]); ax.set_yticks([])
        r, c = divmod(int(np.argmax(m)), SIDE)
        ax.add_patch(Rectangle((c - .5, r - .5), 1, 1, fill=False,
                               edgecolor="#eb6834", lw=1.6))
    for ax in axes[len(picks):]:
        ax.set_axis_off()
    fig.suptitle("Occupancy maps: darker = the piece landed there more often "
                 "(orange = its single most frequent cell)",
                 fontsize=11, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, .95))
    fig.savefig(out / "fig_piece_maps.png"); plt.close(fig)


def fig_far_score(D, S, out):
    """The actionable axis: rows 13-15 versus rows 0-2."""
    far, lo, hi, rank_m = D["far"], D["far_lo"], D["far_hi"], D["rankable"]
    m = rank_m & np.isfinite(far) & np.isfinite(lo)
    idx = np.flatnonzero(m)
    order = idx[np.argsort(far[idx])]
    sig_far = lo[order] > 0
    sig_near = hi[order] < 0

    fig, axes = plt.subplots(1, 2, figsize=(12.4, 4.6),
                             gridspec_kw={"width_ratios": [1.55, 1]})
    ax = axes[0]
    x = np.arange(len(order))
    colour = np.where(sig_far, "#c23434", np.where(sig_near, "#2a78d6", "#c9c6bb"))
    ax.vlines(x, lo[order], hi[order], color=colour, lw=1.1, alpha=.85)
    ax.scatter(x, far[order], s=7, color=colour, zorder=3, linewidths=0)
    ax.axhline(0, color=INK, lw=1, ls="--", alpha=.7)
    ax.set_xlabel("pieces, ordered by far-side score")
    ax.set_ylabel("log2  lift(rows 13-15) / lift(rows 0-2)")
    ax.set_title(f"{int(sig_far.sum())} pieces belong to the far side, "
                 f"{int(sig_near.sum())} to the near side\n"
                 f"(bars are bootstrap 95% intervals; grey = undecided)")
    ax.grid(axis="y", color=RULE, lw=.6, alpha=.7); ax.set_axisbelow(True)

    ax = axes[1]
    excl = S["exclude_suggestion"]
    if excl:
        y = np.arange(len(excl))[::-1]
        ax.barh(y, [far[p] for p in excl], color="#c23434", height=.68, zorder=2)
        ax.errorbar([far[p] for p in excl], y,
                    xerr=[[far[p] - lo[p] for p in excl],
                          [hi[p] - far[p] for p in excl]],
                    fmt="none", ecolor=INK2, elinewidth=1, capsize=2.5, zorder=3)
        ax.set_yticks(y); ax.set_yticklabels([str(p) for p in excl], fontsize=7.5)
        ax.axvline(0, color=INK, lw=1, alpha=.7)
        ax.set_xlabel("log2 far/near")
        ax.set_title("The --exclude_pieces candidates\n"
                     "(interval entirely above zero)")
        ax.grid(axis="x", color=RULE, lw=.6, alpha=.7); ax.set_axisbelow(True)
    else:
        ax.set_axis_off(); ax.set_title("nothing significant to exclude")
    fig.tight_layout()
    fig.savefig(out / "fig_far_score.png"); plt.close(fig)


def fig_ab(AB, out):
    """The practical test: survival per row, and frontier width per row."""
    la, lb = AB["label_a"], AB["label_b"]
    rows = [r for r in AB.get("rows", []) if r["ka"] or r["kb"]]
    if not rows:
        return
    x = np.array([r["row"] for r in rows])

    fig, axes = plt.subplots(1, 2, figsize=(12.8, 4.6))

    # Survival. Log scale: it falls from 100% to a few percent across rows 1-2
    # and is then almost flat, which a linear axis hides completely.
    ax = axes[0]
    sa = np.array([r["surv_a"] * 100 for r in rows])
    sb = np.array([r["surv_b"] * 100 for r in rows])
    ax.plot(x, sa, "o-", color="#8a8779", lw=2, ms=5, label=f"{la} (n={AB['n_a']:,})")
    ax.plot(x, sb, "s-", color="#2a78d6", lw=2, ms=5, label=f"{lb} (n={AB['n_b']:,})")
    for r, ya, yb in zip(rows, sa, sb):
        if r["surv_lo"] > 0 or r["surv_hi"] < 0:
            ax.annotate("*", (r["row"], max(ya, yb)), textcoords="offset points",
                        xytext=(0, 7), ha="center", fontsize=11, color="#d03b3b")
    ax.set_yscale("log")
    ax.set_xlabel("row"); ax.set_ylabel("% of borders still alive")
    ax.set_xticks(x)
    ax.set_title("Survival: almost all mortality is at rows 1-2\n"
                 "(* = 95% interval on the difference excludes zero)")
    ax.legend(); ax.grid(color=RULE, lw=.6, alpha=.7); ax.set_axisbelow(True)

    # Frontier width among the survivors -- the sensitive measure.
    ax = axes[1]
    est = [r for r in rows if r["n_uniq_a"] and r["n_uniq_b"]]
    if est:
        xe = np.array([r["row"] for r in est])
        ma = np.array([r["med_a"] for r in est])
        mb = np.array([r["med_b"] for r in est])
        ax.plot(xe, ma, "o-", color="#8a8779", lw=2, ms=5, label=la)
        ax.plot(xe, mb, "s-", color="#2a78d6", lw=2, ms=5, label=lb)
        for r in est:
            if not r["estimable"]:
                continue
            sig = r["ratio_lo"] > 1 or r["ratio_hi"] < 1
            ax.annotate(f"{r['ratio']:.2f}x" + ("*" if sig else ""),
                        (r["row"], max(r["med_a"], r["med_b"])),
                        textcoords="offset points", xytext=(0, 8), ha="center",
                        fontsize=7.5,
                        color="#0ca30c" if (sig and r["ratio"] > 1)
                        else "#d03b3b" if sig else INK2)
        ax.set_yscale("log")
        ax.set_xticks(xe)
    ax.set_xlabel("row")
    ax.set_ylabel("median distinct states at that row")
    ax.set_title("Frontier width among the borders that got there\n"
                 "(ratio excluded/baseline; * = interval excludes 1.00)")
    ax.legend(); ax.grid(color=RULE, lw=.6, alpha=.7); ax.set_axisbelow(True)
    fig.tight_layout()
    fig.savefig(out / "fig_ab.png"); plt.close(fig)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Static figures for the corner study.")
    ap.add_argument("stats_dir")
    ap.add_argument("--ab", default=None, help="ab.json from E555_ab_analyze.py")
    ap.add_argument("--out_dir", default=None)
    args = ap.parse_args(argv)

    sd = Path(args.stats_dir)
    out = Path(args.out_dir or (sd / "figs"))
    out.mkdir(parents=True, exist_ok=True)

    npz = np.load(sd / "arrays.npz")
    D = {k: npz[k] for k in npz.files}
    S = json.loads((sd / "summary.json").read_text())

    # Per-side lifts for the replication panel: recomputed here rather than
    # stored, since it is the only figure that needs them.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import E555_frame_stats as FS
    zone_per_side = D["per_side_zone"]
    per = np.stack([FS.lift_from_counts(zone_per_side[s][None], D["kind"])[0]
                    for s in range(4)])
    D["per_side_lift"] = per

    made = []
    for fn in (fig_coverage, fig_zone_heatmap, fig_corner_tops, fig_replication,
               fig_colors, fig_piece_maps, fig_far_score):
        try:
            fn(D, S, out)
            made.append(fn.__name__)
        except Exception as exc:                      # one bad panel is not fatal
            print(f"[warn] {fn.__name__}: {exc}", file=sys.stderr)
    if args.ab and Path(args.ab).exists():
        try:
            fig_ab(json.loads(Path(args.ab).read_text()), out)
            made.append("fig_ab")
        except Exception as exc:
            print(f"[warn] fig_ab: {exc}", file=sys.stderr)

    for f in sorted(out.glob("fig_*.png")):
        print(f"[out] {f}  ({f.stat().st_size/1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
