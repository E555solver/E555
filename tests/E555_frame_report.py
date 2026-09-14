#!/usr/bin/env python3
"""
E555_frame_report.py -- assemble the corner study into one readable report.

    python3 tests/E555_frame_report.py ff_out/stats --figs ff_out/figs \\
        --ab ab_out/ab.json --out report.html

Static figures with the explanation written beside them, in the order the
argument runs: what was measured, whether it replicates, what it says, and
whether acting on it worked. Every PNG is embedded, so the file stands alone.
"""
from __future__ import annotations

import argparse
import base64
import html
import json
import sys
from pathlib import Path

FIG_ORDER = [
    ("fig_coverage.png", "design"),
    ("fig_replication.png", "replication"),
    ("fig_corner_tops.png", "corners"),
    ("fig_zone_heatmap.png", "corners"),
    ("fig_colors.png", "mechanism"),
    ("fig_piece_maps.png", "mechanism"),
    ("fig_far_score.png", "action"),
    ("fig_ab.png", "test"),
]


def embed(path: Path) -> str:
    return ("data:image/png;base64,"
            + base64.b64encode(path.read_bytes()).decode())


def fig(figs: Path, name: str, caption: str, wide: bool = True) -> str:
    p = figs / name
    if not p.exists():
        return ""
    cls = "figure wide" if wide else "figure"
    return (f'<figure class="{cls}">\n'
            f'  <img src="{embed(p)}" alt="{html.escape(caption)}">\n'
            f'  <figcaption>{caption}</figcaption>\n</figure>\n')


CSS = """<title>The Corner Preference Study</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Newsreader:ital,opsz,wght@0,6..72,400;0,6..72,600;1,6..72,400&family=Archivo:wght@400;500;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
:root{
  color-scheme:light;
  --paper:#faf9f6; --card:#ffffff; --sunk:#f2f0ea;
  --ink:#16150f; --ink2:#54524a; --ink3:#8a8779;
  --rule:#e3e0d6; --rule2:#cbc7b9;
  --accent:#2a78d6; --accent-soft:#e8f0fc;
  --good:#0ca30c; --warn:#fab219; --bad:#d03b3b;
}
@media (prefers-color-scheme:dark){
  :root:not([data-theme="light"]){
    color-scheme:dark;
    --paper:#14141a; --card:#1d1d24; --sunk:#101015;
    --ink:#f2f1ec; --ink2:#b3b1a7; --ink3:#7e7b72;
    --rule:#2c2c34; --rule2:#40404a;
    --accent:#6da7ec; --accent-soft:#182536;
  }
}
:root[data-theme="dark"]{
  color-scheme:dark;
  --paper:#14141a; --card:#1d1d24; --sunk:#101015;
  --ink:#f2f1ec; --ink2:#b3b1a7; --ink3:#7e7b72;
  --rule:#2c2c34; --rule2:#40404a;
  --accent:#6da7ec; --accent-soft:#182536;
}
*{box-sizing:border-box}
body{margin:0;background:var(--paper);color:var(--ink);
     font:400 16px/1.68 Archivo,ui-sans-serif,system-ui,sans-serif;
     -webkit-font-smoothing:antialiased}
.wrap{max-width:1000px;margin:0 auto;padding-inline:22px;padding-block:52px 88px}
.col{max-width:66ch}
h1,h2,h3{font-family:Newsreader,Georgia,serif;text-wrap:balance;margin:0}
h1{font-size:clamp(34px,5.4vw,54px);font-weight:600;line-height:1.04;letter-spacing:-.018em}
h2{font-size:clamp(23px,3vw,29px);font-weight:600;line-height:1.15;letter-spacing:-.012em}
h3{font-size:18px;font-weight:600;margin-top:26px}
p{margin:0 0 16px}
.lede{font-family:Newsreader,Georgia,serif;font-size:20.5px;line-height:1.55;
      color:var(--ink2);margin-top:18px}
.eyebrow{font:500 11px/1 "IBM Plex Mono",monospace;letter-spacing:.15em;
         text-transform:uppercase;color:var(--ink3)}
.mono{font-family:"IBM Plex Mono",ui-monospace,monospace;font-variant-numeric:tabular-nums}
code{font-family:"IBM Plex Mono",monospace;font-size:.88em;background:var(--sunk);
     padding:1px 5px;border-radius:3px}
header{padding-bottom:30px;border-bottom:2px solid var(--ink)}
section{margin-top:52px;scroll-margin-top:20px}
.sec-num{font:500 11px/1 "IBM Plex Mono",monospace;letter-spacing:.14em;
         color:var(--accent);display:block;margin-bottom:9px}
figure{margin:26px 0 8px}
figure.wide{max-width:none}
figure img{width:100%;height:auto;display:block;border:1px solid var(--rule);
           border-radius:3px;background:#fff}
figcaption{font-size:13.5px;line-height:1.5;color:var(--ink2);margin-top:9px;
           max-width:78ch}
.verdict{margin-top:30px;border:1px solid var(--rule);border-left:5px solid var(--vc,var(--ink3));
         border-radius:3px;background:var(--card);padding:22px 26px}
.verdict[data-v="good"]{--vc:var(--good)} .verdict[data-v="bad"]{--vc:var(--bad)}
.verdict[data-v="warn"]{--vc:var(--warn)} .verdict[data-v="none"]{--vc:var(--ink3)}
.verdict h3{margin:0 0 8px;font-size:19px}
.verdict p{margin:0;color:var(--ink2);font-size:15px}
.verdict p+p{margin-top:10px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:1px;
       background:var(--rule);border:1px solid var(--rule);border-radius:3px;
       overflow:hidden;margin:26px 0}
.tile{background:var(--card);padding:15px 17px;display:grid;gap:4px}
.tile .v{font:500 24px/1 "IBM Plex Mono",monospace;font-variant-numeric:tabular-nums}
.tile .k{font:500 10px/1.3 "IBM Plex Mono",monospace;letter-spacing:.1em;
         text-transform:uppercase;color:var(--ink3)}
.tile .n{font-size:12px;color:var(--ink2);line-height:1.4}
table{border-collapse:collapse;width:100%;font-size:14px;margin:20px 0}
th{font:500 10.5px/1.3 "IBM Plex Mono",monospace;letter-spacing:.08em;
   text-transform:uppercase;color:var(--ink3);text-align:left;padding:9px 11px;
   border-bottom:1px solid var(--rule2);white-space:nowrap}
td{padding:8px 11px;border-bottom:1px solid var(--rule);vertical-align:top}
td.num{font-family:"IBM Plex Mono",monospace;font-variant-numeric:tabular-nums;
       white-space:nowrap}
.tbl-scroll{overflow-x:auto}
.callout{background:var(--accent-soft);border-radius:3px;padding:18px 22px;margin:24px 0;
         border:1px solid var(--rule)}
.callout p:last-child{margin-bottom:0}
.callout .eyebrow{color:var(--accent);margin-bottom:8px;display:block}
pre{background:var(--sunk);border:1px solid var(--rule);border-radius:3px;
    padding:14px 16px;overflow-x:auto;margin:18px 0;
    font:400 12.8px/1.65 "IBM Plex Mono",monospace;color:var(--ink2)}
pre b{color:var(--ink);font-weight:500}
ul{margin:0 0 16px;padding-left:20px} li{margin-bottom:7px}
.pills{display:flex;flex-wrap:wrap;gap:6px;margin:14px 0}
.pill{font:500 12.5px "IBM Plex Mono",monospace;padding:4px 9px;border-radius:2px;
      background:var(--sunk);border:1px solid var(--rule2)}
footer{margin-top:64px;padding-top:24px;border-top:1px solid var(--rule);
       font-size:13px;color:var(--ink3)}
hr.thin{border:0;border-top:1px solid var(--rule);margin:40px 0}
@media (max-width:640px){ .wrap{padding-block:36px 60px} }
</style>
"""


def build(S, figs, AB, corpus_note):
    cov = S["coverage"]
    rho = S.get("corner_replication_rho", float("nan"))
    n_sig, n_rank = S.get("n_significant", 0), S.get("n_rankable", 0)
    excl = S.get("exclude_suggestion", [])

    # --- the headline verdict comes from the A/B, if it ran -------------------
    if AB:
        v = AB.get("verdict", "")
        lvl = ("good" if "HELPS" in v else "bad" if "HURTS" in v else "none")
        vtitle = ("Excluding the far-side pieces made the beam deeper" if lvl == "good"
                  else "Excluding the far-side pieces made the beam shallower"
                  if lvl == "bad" else "Excluding the far-side pieces changed nothing measurable")
        vbody = html.escape(v)
    else:
        lvl, vtitle = "none", "The practical test has not been run"
        vbody = ("The corpus says which pieces belong on the far side. Whether "
                 "removing them helps is a separate question and only the A/B run "
                 "answers it.")

    rrows = "".join(
        f"<tr><td class='num'>{nm}</td>"
        f"<td class='num'>{p['sides'][0]} vs {p['sides'][1]}</td>"
        f"<td class='num'>{p['rho']:+.3f}</td><td class='num'>{p['n']}</td></tr>"
        for nm in ["BL", "BR", "TL", "TR"] for p in S["replication"].get(nm, []))

    def zone_table(nm):
        rows = S["top_by_zone"].get(nm, [])[:8]
        if not rows:
            return "<p class='mono'>nothing significant</p>"
        body = "".join(
            f"<tr><td class='num'>{r['piece']}</td><td>{r['kind']}</td>"
            f"<td class='num'>{r['lift']:.2f}&times;</td>"
            f"<td class='num'>[{r['lo']:.2f}, {r['hi']:.2f}]</td></tr>"
            for r in rows)
        return (f"<h3>{nm} &mdash; {S['zone_label'][nm]} <span class='mono' "
                f"style='color:var(--ink3);font-size:13px;font-weight:400'>"
                f"(sides {cov[nm]})</span></h3>"
                f"<div class='tbl-scroll'><table><tr><th>piece</th><th>kind</th>"
                f"<th>lift</th><th>95% interval</th></tr>{body}</table></div>")

    ab_section = ""
    if AB:
        thr = "".join(
            f"<tr><td class='num'>&ge; {r['D']}</td>"
            f"<td class='num'>{r['pa']*100:.2f}%</td>"
            f"<td class='num'>{r['pb']*100:.2f}%</td>"
            f"<td class='num'>{r['diff']*100:+.2f} pp</td>"
            f"<td class='num'>[{r['lo']*100:+.2f}, {r['hi']*100:+.2f}]</td>"
            f"<td class='num'>{'yes' if r['significant'] else '&mdash;'}</td></tr>"
            for r in AB["thresholds"])
        ab_section = f"""
<section>
  <span class="sec-num">06 &middot; the test</span>
  <div class="col">
    <h2>Does acting on it work?</h2>
    <p>Everything above is description. This is the only part that decides
    anything: the beamer run twice in the canonical frame, identical except that
    one arm bars the {len(excl)} far-side pieces, and the depth each border
    reached compared between them.</p>
    <p>The arms are not paired. You would expect the same seed to give both arms
    the same borders, but the border sampler ranks candidates by fan-out into the
    chain database, and excluding pieces changes that database &mdash; so the two
    runs diverge from the first border. The comparison is therefore between two
    rates over hundreds of borders, not a per-border delta.</p>
  </div>
  <div class="tiles">
    <div class="tile"><span class="k">baseline</span>
      <span class="v">{AB['n_a']:,}</span><span class="n">borders swept</span></div>
    <div class="tile"><span class="k">excluded</span>
      <span class="v">{AB['n_b']:,}</span><span class="n">borders swept</span></div>
    <div class="tile"><span class="k">mean depth</span>
      <span class="v">{AB['mean_a']:.2f}</span><span class="n">baseline</span></div>
    <div class="tile"><span class="k">mean depth</span>
      <span class="v">{AB['mean_b']:.2f}</span><span class="n">excluded</span></div>
  </div>
  {fig(figs, "fig_ab.png", "Depth reached per border configuration. The left panel is the whole distribution, which is dominated by borders that die at row 1 or 2 in both arms; the right panel is the deep tail, which is what a practical run cares about.")}
  <div class="col">
    <div class="tbl-scroll"><table>
      <tr><th>reached depth</th><th>baseline</th><th>excluded</th>
          <th>difference</th><th>95% interval</th><th>significant</th></tr>
      {thr}
    </table></div>
    <p>Intervals on each rate are Wilson; on the difference, Newcombe. The
    secondary Mann&ndash;Whitney over the whole distribution gives
    <span class="mono">z = {AB['mw_z']:+.3f}</span>,
    <span class="mono">p = {AB['mw_p']:.3g}</span> &mdash; but it is dominated by
    the shallow bulk and can point the other way from the tail, so the threshold
    table above is the one to read.</p>
  </div>
</section>"""

    excl_pills = "".join(f'<span class="pill">{p}</span>' for p in excl)

    return CSS + f"""
<div class="wrap">
<header>
  <span class="eyebrow">E555 &middot; Stage B &middot; fixed-frame corpus</span>
  <h1>Which pieces belong near which corner</h1>
  <p class="lede">{S['boards']:,} partial boards from {S['configs']:,} independent
  border configurations, harvested from four sides of one pinned frame. None of
  them solves anything. The question is whether the pinned corners and clues bend
  the placement distribution hard enough to be worth exploiting &mdash; and
  whether exploiting it actually helps.</p>
</header>

<div class="verdict" data-v="{lvl}">
  <h3>{vtitle}</h3>
  <p>{vbody}</p>
</div>

<div class="tiles">
  <div class="tile"><span class="k">boards</span><span class="v">{S['boards']:,}</span>
    <span class="n">rows 0&ndash;10 filled</span></div>
  <div class="tile"><span class="k">borders</span><span class="v">{S['configs']:,}</span>
    <span class="n">the independent unit</span></div>
  <div class="tile"><span class="k">N<sub>eff</sub></span><span class="v">{S['n_eff']:,.0f}</span>
    <span class="n">Kish, after weighting each border once</span></div>
  <div class="tile"><span class="k">replication</span>
    <span class="v">{rho:+.3f}</span><span class="n">corner-zone cross-side &rho;</span></div>
  <div class="tile"><span class="k">with a preference</span>
    <span class="v">{n_sig}</span><span class="n">of {n_rank} pieces, 95% CI clear of 1.00&times;</span></div>
</div>

<section>
  <span class="sec-num">01 &middot; the design</span>
  <div class="col">
    <h2>Four sides, one frame</h2>
    <p>The frame pins four corner pieces and four corner clues. That is a lot of
    constraint: only a few pieces can sit beside a given corner piece and still
    leave room for the clue two cells in. So placement near a corner is nowhere
    near uniform &mdash; and it differs between the four corners, because they
    carry different pieces and different clues. That is the signal.</p>
    <p>The beamer grows bottom-up and fills eleven rows, so one run only ever
    sees a band. The same frame is therefore run from all four sides and each
    side's boards are turned back onto canonical coordinates. The board is
    partitioned into nine zones on the bands
    <span class="mono">[0&ndash;4] [5&ndash;10] [11&ndash;15]</span>, and at
    <code>--stop_row 10</code> those are exactly the cut points where the four
    bands land:</p>
    <pre>side 0  rows  0..10        side 2  rows  5..15
side 1  cols  5..15        side 3  cols  0..10</pre>
    <p>So every zone is wholly inside a side's band or wholly outside it, never
    half-covered &mdash; and each corner zone is seen by exactly two sides:
    <span class="mono">BL {cov['BL']}</span>,
    <span class="mono">BR {cov['BR']}</span>,
    <span class="mono">TL {cov['TL']}</span>,
    <span class="mono">TR {cov['TR']}</span>.</p>
  </div>
  {fig(figs, "fig_coverage.png", "Coverage, measured from the corpus rather than assumed. If the rotation step were wrong, or the farm ran at a different stop row, it would show up here instead of silently biasing every number downstream.")}
  <div class="col">
    <div class="callout">
      <span class="eyebrow">why exposure is the whole problem</span>
      <p>Side 0 fills rows 0&ndash;10 and therefore <em>cannot</em> place anything
      in the top-left or top-right zone. Count naively and every piece looks like
      it avoids the top &mdash; an artefact of where the beam was pointed, not a
      fact about the puzzle. So each lift is computed per side over only the zones
      that side covers, and pooled afterwards across the sides that cover it.</p>
    </div>
  </div>
</section>

<section>
  <span class="sec-num">02 &middot; replication</span>
  <div class="col">
    <h2>Do independent views agree?</h2>
    <p>Two sides see each corner, using different random borders, a different RNG
    stream and a different search direction. They share very little of the
    heuristic's bias. If they rank the pieces the same way, something about the
    puzzle is driving it; if they do not, the ranking is the search talking to
    itself and no further analysis rescues it.</p>
    <p>Mean Spearman across the four corner zones:
    <strong class="mono">{rho:+.3f}</strong>.</p>
    <div class="tbl-scroll"><table>
      <tr><th>zone</th><th>sides compared</th><th>Spearman &rho;</th><th>pieces</th></tr>
      {rrows}
    </table></div>
  </div>
  {fig(figs, "fig_replication.png", "One point per piece: its lift measured by one side against the same lift measured by the other. Points on the diagonal are pieces the two sides agree about. Pinned corner and clue pieces are excluded — they sit at the same cell in every board and would agree perfectly for a trivial reason.")}
</section>

<section>
  <span class="sec-num">03 &middot; what the corners want</span>
  <div class="col">
    <h2>The preferences themselves</h2>
    <p>A lift of 2.0&times; means the piece lands in that zone twice as often as
    an average piece of its kind does. Edge and inner pieces have separate
    baselines: an edge piece can only sit on the frame, so comparing it against
    inner pieces would measure the frame, not a preference.</p>
    <p><strong>{n_sig} of {n_rank}</strong> pieces have at least one zone whose
    95&nbsp;% bootstrap interval clears 1.00&times;.</p>
  </div>
  {fig(figs, "fig_corner_tops.png", "The strongest pieces for each corner, with bootstrap 95% intervals. Bars whose interval crosses the dashed line at 1.00x are not shown at all — only pieces with a preference the resampling supports.")}
  <div class="col">
    {zone_table("BL")}
    {zone_table("BR")}
    {zone_table("TL")}
    {zone_table("TR")}
  </div>
  {fig(figs, "fig_zone_heatmap.png", "Every rankable piece against all nine zones, grouped by the zone it prefers. Block structure down the diagonal is the finding: pieces do sort themselves by region.")}
</section>

<section>
  <span class="sec-num">04 &middot; the mechanism</span>
  <div class="col">
    <h2>Why a piece prefers a corner</h2>
    <p>A piece cannot prefer a corner for its own sake. It prefers it because the
    colours it carries are the ones that corner's pinned pieces demand, and
    because the pieces that fit beside those pieces demand others in turn. Colour
    is 22 buckets against 256, so it is far better estimated than the piece level
    &mdash; if the piece-level pattern is real, there should be a colour pattern
    under it.</p>
  </div>
  {fig(figs, "fig_colors.png", "Where each of the 22 edge colours ends up. The scale is much tighter than the piece-level one: colours are shared by many pieces, so their zone preferences are diluted — but a consistent bias here is the mechanism behind the piece-level effect.")}
  {fig(figs, "fig_piece_maps.png", "The most corner-specific pieces, showing every cell each one reached across the whole corpus.")}
</section>

<section>
  <span class="sec-num">05 &middot; the actionable part</span>
  <div class="col">
    <h2>Pieces the beam should not be spending</h2>
    <p>The beamer grows bottom-up. Run it in the canonical frame to
    <code>--stop_row {S.get('ab_stop_row', 12)}</code> and rows 13&ndash;15 are
    unreachable, so a piece that belongs up there is budget the search is
    wasting. The score is</p>
    <pre>far_score = log2( lift(rows 13-15) / lift(rows 0-2) )</pre>
    <p>bootstrapped over borders. Only pieces whose whole interval sits above
    zero qualify &mdash; evidence of a far-side preference, not a point estimate
    that happens to be positive. <strong>{S.get('far_significant', 0)}</strong>
    pieces clear that bar.</p>
    <div class="callout">
      <span class="eyebrow">the ceiling, and it is not statistical</span>
      <p>Rows 1&ndash;14 hold 14&times;14 = 196 inner cells and the puzzle has
      exactly 196 inner pieces. There are no spares. A beam to row
      {S.get('ab_stop_row', 12)} places {S.get('ab_stop_row', 12)*14} of them and
      two more are reserved for the row-13 clues, leaving
      <strong>{S.get('inner_slack', 26)}</strong> spare. Exclude K pieces and the
      slack drops to {S.get('inner_slack', 26)}&minus;K; push K toward the slack
      and the run collapses however good the statistics are. The list is capped
      at <strong>{S.get('exclude_cap', 13)}</strong> &mdash; half the slack
      &mdash; for that reason alone.</p>
    </div>
    <div class="pills">{excl_pills or '<span class="pill">none</span>'}</div>
  </div>
  {fig(figs, "fig_far_score.png", "Left: every rankable piece ordered by far-side score, with bootstrap intervals; red pieces belong to the far side, blue to the near side, grey are undecided. Right: the candidates actually excluded.")}
</section>
{ab_section}

<section>
  <span class="sec-num">07 &middot; limits</span>
  <div class="col">
    <h2>What this does not show</h2>
    <ul>
      <li><strong>These are the beam's habits, not the puzzle's truth.</strong>
      The heuristic has its own taste and the corpus inherits it. Cross-side
      replication is what separates the two, and it is a correlation, not a
      proof.</li>
      <li><strong>The corner assignment is a bet.</strong> No clue pins a corner,
      so the four corner pieces can fill the four corners 4! = 24 ways and only
      one is the solution's. Everything near the border is conditioned on that
      choice; the centre barely notices.</li>
      <li><strong>Nothing here transfers to the production beamer</strong>, which
      hedges across all four orientations and samples its own corners. The
      exclusion list is specific to this frame.</li>
      <li><strong>No board in the corpus is a solution.</strong> The statistics
      describe where a partial-board search puts pieces when it survives eleven
      rows, which is related to, but not the same as, where they belong.</li>
    </ul>
  </div>
</section>

<footer>
  <p>{corpus_note}</p>
  <p class="mono">tests/E555_frame_stats.py &rarr; tests/E555_frame_plots.py &rarr;
  tests/E555_frame_report.py &middot; bootstrap {S.get('n_boot', 0):,} resamples
  over border configurations</p>
</footer>
</div>
"""


def main(argv=None):
    ap = argparse.ArgumentParser(description="Assemble the corner-study report.")
    ap.add_argument("stats_dir")
    ap.add_argument("--figs", default=None)
    ap.add_argument("--ab", default=None, help="ab.json from E555_ab_analyze.py")
    ap.add_argument("--out", default="frame_report.html")
    args = ap.parse_args(argv)

    sd = Path(args.stats_dir)
    figs = Path(args.figs or (sd / "figs"))
    S = json.loads((sd / "summary.json").read_text())
    AB = json.loads(Path(args.ab).read_text()) if args.ab and Path(args.ab).exists() else None

    note = (f"Corpus: {html.escape(str(S.get('corpus','')))} &middot; "
            f"{S['boards']:,} boards, {S['configs']:,} borders, "
            f"sides {S.get('per_side_configs', {})}")
    Path(args.out).write_text(build(S, figs, AB, note))
    kb = Path(args.out).stat().st_size / 1024
    print(f"[out] {args.out}  ({kb:.0f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
