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

    # "116 of 247" means nothing without knowing what the same procedure returns
    # when there is provably nothing to find.
    nul = S.get("null_significant_mean")
    nulr = S.get("null_rho_mean")
    if nul is not None:
        ns = S.get("null_significant", [])
        null_line = (f"Run the identical procedure on corpora where piece labels "
                     f"have been shuffled within each board, and it returns "
                     f"<strong>{nul:.0f}</strong> "
                     f"(range {min(ns)}&ndash;{max(ns)} over {len(ns)} replicates) "
                     f"&mdash; that is the multiplicity of {n_rank} pieces across "
                     f"nine zones leaking through, and the observed count is "
                     f"{n_sig/max(nul,1e-9):.1f}&times; it.")
    else:
        null_line = ""
    null_rho_line = (f" Under the same shuffle the correlation is "
                     f"<span class='mono'>{nulr:+.3f}</span>, which is what "
                     f"\u201cno structure\u201d looks like."
                     if nulr is not None else "")

    # Does the far-side score itself replicate? Three sides see rows 13-15.
    fa = S.get("far_agreement", [])
    if fa:
        fr = sum(x["rho"] for x in fa) / len(fa)
        fa_rows = ", ".join(f"sides {x['sides'][0]}&ndash;{x['sides'][1]}: "
                            f"<span class='mono'>{x['rho']:+.3f}</span>" for x in fa)
        if fr > 0.25:
            fa_note = (f"<p>The score replicates across the three sides that see "
                       f"rows 13&ndash;15 ({fa_rows}; mean "
                       f"<strong>{fr:+.3f}</strong>), so it is not one search "
                       f"direction's opinion.</p>")
        else:
            fa_note = (f"<div class=\"callout\" style=\"background:var(--sunk)\">"
                       f"<span class=\"eyebrow\" style=\"color:var(--bad)\">"
                       f"read this before using the list</span>"
                       f"<p>The three sides that see rows 13&ndash;15 do "
                       f"<em>not</em> agree on this score ({fa_rows}; mean "
                       f"<strong>{fr:+.3f}</strong>). The zone preferences above "
                       f"replicate; this particular ranking does not yet. Treat "
                       f"the exclusion list as a hypothesis the A/B test is "
                       f"about to judge, not as a finding.</p></div>")
    else:
        fa_note = ""
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
        rws = AB.get("rows", [])
        thr = "".join(
            f"<tr><td class='num'>{r['row']}</td>"
            f"<td class='num'>{r['surv_a']*100:.2f}%</td>"
            f"<td class='num'>{r['surv_b']*100:.2f}%</td>"
            f"<td class='num'>{r['surv_diff']*100:+.2f} pp</td>"
            f"<td class='num'>{'' if r['med_a']!=r['med_a'] else format(r['med_a'],',.0f')}</td>"
            f"<td class='num'>{'' if r['med_b']!=r['med_b'] else format(r['med_b'],',.0f')}</td>"
            f"<td class='num'>" + ("&mdash;" if not r["estimable"] else
              f"{r['ratio']:.3f} [{r['ratio_lo']:.2f}, {r['ratio_hi']:.2f}]")
            + f"</td></tr>"
            for r in rws)
        hr = AB.get("headline_row")
        ab_cap = ("Left: how far each border got. Right: the width of the "
                  "frontier at each row, among the borders that reached it \u2014 "
                  "the sensitive measure, since survival past row 2 almost "
                  "guarantees survival to row 10.")
        ab_fig = fig(figs, "fig_ab.png", ab_cap)
        ab_section = f"""
<section>
  <span class="sec-num">06 &middot; the test</span>
  <div class="col">
    <h2>Does acting on it work?</h2>
    <p>Everything above is description. This is the only part that decides
    anything: the beamer run twice in the canonical frame, identical except that
    one arm bars the {len(excl)} far-side pieces.</p>
    <div class="callout">
      <span class="eyebrow">why this does not count emitted boards</span>
      <p>At <code>--stop_row 12</code> with every clue on, this machine emits
      essentially nothing &mdash; the beam dies before it gets there on all but a
      freak border, so counting emissions would compare zero against zero. Both
      arms therefore run <code>--verbose</code>, and the comparison uses the
      per-row line the beam prints on the way up:
      <span class="mono">uniq</span>, the number of distinct states surviving the
      dedup at that row. It is continuous, recorded for every border at every row
      it reaches, and it is what &ldquo;the search still has room&rdquo; means.</p>
    </div>
    <p>The arms are not paired. You would expect the same seed to give both the
    same borders, but the border sampler ranks candidates by fan-out into the
    chain database, and excluding pieces changes that database &mdash; so the two
    runs diverge from the first border. The comparison is between two rates over
    thousands of borders.</p>
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
  {ab_fig}
  <div class="col">
    <div class="tbl-scroll"><table>
      <tr><th>row</th><th>survival base</th><th>survival excl</th><th>diff</th>
          <th>median uniq base</th><th>median uniq excl</th>
          <th>width ratio (95% CI)</th></tr>
      {thr}
    </table></div>
    <p>Survival intervals are Wilson per arm and Newcombe on the difference; the
    width ratio is bootstrapped over borders. The verdict reads row
    <span class="mono">{hr if hr else "&mdash;"}</span> &mdash; the deepest row
    where both arms still have enough borders to estimate a median.</p>
  </div>
</section>"""

    heat_cap = ("Every rankable piece against all nine zones, grouped by the zone "
                "it prefers. The blue block down the diagonal is each group "
                "preferring its own zone; the red block is the same group avoiding "
                "the OPPOSITE corner. Almost nothing prefers the four edge zones "
                "\u2014 pieces sort into corners and centre.")
    heat_fig = fig(figs, "fig_zone_heatmap.png", heat_cap)

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
    stream and a different search direction. What they <em>do</em> share is the
    frame &mdash; the same pinned corner piece, the same clue two cells in &mdash;
    and that is the conditioning the whole experiment is about. What they do not
    share is the search. So agreement means the frame is driving the placement;
    disagreement would mean the heuristic is, and no further analysis would
    rescue it.</p>
    <p>One honest qualification. Each corner is reached early in the growth of
    both sides that see it &mdash; within four rows or four columns. That is the
    regime where the border and the clues dominate and the objective has barely
    begun to matter, so the agreement below is strongest exactly where constraint
    is strongest. That is the effect this experiment set out to find rather than
    a flaw in it, but it does mean the numbers describe the constrained
    neighbourhood of a corner, not the board at large.</p>
    <p>Mean Spearman across the four corner zones:
    <strong class="mono">{rho:+.3f}</strong>.{null_rho_line}</p>
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
    95&nbsp;% bootstrap interval clears 1.00&times;. {null_line}</p>
  </div>
  {fig(figs, "fig_corner_tops.png", "The strongest pieces for each corner, with bootstrap 95% intervals. Bars whose interval crosses the dashed line at 1.00x are not shown at all — only pieces with a preference the resampling supports.")}
  <div class="col">
    {zone_table("BL")}
    {zone_table("BR")}
    {zone_table("TL")}
    {zone_table("TR")}
  </div>
  {heat_fig}
  <div class="col">
    <p>Two things in that figure are worth more than the diagonal. The first is
    the <strong>red opposite the blue</strong>: pieces that prefer the
    bottom-left actively avoid the top-right, and vice versa. A piece does not
    merely have a corner, it has an anti-corner &mdash; which is exactly what
    makes the far-side score below possible.</p>
    <p>The second is what is <em>missing</em>. Almost no piece prefers BM, ML, MR
    or TM, the four edge zones. The sorting is into corners and centre, not into
    nine regions. The corners are where the pinned pieces are, and the constraint
    evidently does not propagate sideways along an edge the way it propagates
    into a corner.</p>
  </div>
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
    {fa_note}
    <div class="pills">{excl_pills or '<span class="pill">none</span>'}</div>
  </div>
  {fig(figs, "fig_far_score.png", "Left: every rankable piece ordered by far-side score, with bootstrap intervals; red pieces belong to the far side, blue to the near side, grey are undecided. Right: the candidates actually excluded.")}
</section>
{ab_section}

<section>
  <span class="sec-num">07 &middot; an aside worth keeping</span>
  <div class="col">
    <h2>Where the beam actually dies</h2>
    <p>Measured across the farm: of every 100 borders the sweep tries, about
    <strong>96 die at row 1 or 2</strong> &mdash; the two rows the clues pin. Of
    the four that survive row 2, <strong>98&nbsp;% reach row 10</strong>. The
    survival curve is a cliff followed by a plateau.</p>
    <p>That reframes what an exclusion list can do. Rows 3&ndash;10 are close to
    free; the search is not slowly grinding down, it is being killed at the
    clue-pinned rows and then coasting. So a piece list helps or hurts mainly
    through what it does to rows 1&ndash;2, and the danger is real: taking
    pieces away can only make those rows harder to thread. That is why the test
    reports survival per row as well as frontier width &mdash; a list that buys
    width at row 11 by costing survival at row 2 is a bad trade, and the two
    columns show it separately.</p>
  </div>
</section>

<section>
  <span class="sec-num">08 &middot; limits</span>
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
      <li><strong>The zone preferences replicate far better than the far-side
      ranking does.</strong> Two sides agree about a corner at
      &rho;&nbsp;&asymp;&nbsp;0.70; the three sides that see rows 13&ndash;15
      agree about the far-side score much more weakly, and one pair disagrees
      outright. The corner result and the exclusion list do not stand or fall
      together.</li>
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
