#!/usr/bin/env python3
"""
E555_frame_dashboard.py -- turn summary.json into one HTML page you can read.

    python3 tests/E555_frame_stats.py     ff_out/corpus.csv --out_dir ff_out/stats
    python3 tests/E555_frame_dashboard.py ff_out/stats/summary.json --out board.html

The page answers, in this order:

  1. Do the four sides agree?  If they do not, nothing else on the page matters,
     so the verdict sits at the top and says so in those words.
  2. How big is the corpus really?  Boards, border configs, and N_eff -- which is
     the only one of the three worth trusting.
  3. Which pieces prefer which ring?  One row per piece, a seven-cell strip
     coloured by lift (how much more often than an average inner piece), sortable
     and clickable.
  4. Which pieces belong in rows 12-15?  The --exclude_pieces candidates, with
     the command to test them.

No dependencies: it is one self-contained file with the data inlined.
"""
from __future__ import annotations

import argparse
import html
import json
import sys
from pathlib import Path

TEMPLATE_HEAD = """<title>Ring Affinity Readout</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Bricolage+Grotesque:opsz,wght@12..96,500;12..96,700&family=Archivo:wght@400;500;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
:root {
  color-scheme: light;
  --ground:#faf9f6; --raised:#ffffff; --sunk:#f2f0ea;
  --ink:#16150f; --ink-2:#57554a; --ink-3:#8a8779;
  --rule:#e2dfd4; --rule-2:#cfcbbd;
  --accent:#2a78d6;
  --pos-3:#1c5cab; --pos-2:#3987e5; --pos-1:#9ec5f4;
  --mid:#f0efec;
  --neg-1:#f4cccc; --neg-2:#e08585; --neg-3:#c23434;
  --good:#0ca30c; --warning:#fab219; --critical:#d03b3b;
  --shadow:0 1px 2px rgba(22,21,15,.06), 0 4px 14px rgba(22,21,15,.05);
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --ground:#15151a; --raised:#1e1e24; --sunk:#101014;
    --ink:#f3f2ee; --ink-2:#b4b2a8; --ink-3:#807d73;
    --rule:#2e2e36; --rule-2:#43434d;
    --accent:#3987e5;
    --pos-3:#86b6ef; --pos-2:#3987e5; --pos-1:#1c5cab;
    --mid:#383835;
    --neg-1:#7a2b2b; --neg-2:#c04b4b; --neg-3:#eb8f8f;
    --shadow:0 1px 2px rgba(0,0,0,.4), 0 4px 16px rgba(0,0,0,.3);
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --ground:#15151a; --raised:#1e1e24; --sunk:#101014;
  --ink:#f3f2ee; --ink-2:#b4b2a8; --ink-3:#807d73;
  --rule:#2e2e36; --rule-2:#43434d;
  --accent:#3987e5;
  --pos-3:#86b6ef; --pos-2:#3987e5; --pos-1:#1c5cab;
  --mid:#383835;
  --neg-1:#7a2b2b; --neg-2:#c04b4b; --neg-3:#eb8f8f;
  --shadow:0 1px 2px rgba(0,0,0,.4), 0 4px 16px rgba(0,0,0,.3);
}

* { box-sizing:border-box; }
body {
  margin:0; background:var(--ground); color:var(--ink);
  font:400 15px/1.6 Archivo, ui-sans-serif, system-ui, sans-serif;
  -webkit-font-smoothing:antialiased;
}
.wrap { max-width:1120px; margin:0 auto; padding-inline:20px; padding-block:40px 72px; }
h1, h2, h3 { font-family:"Bricolage Grotesque", Archivo, sans-serif; text-wrap:balance; margin:0; }
h1 { font-size:clamp(28px,4.2vw,42px); font-weight:700; letter-spacing:-.022em; line-height:1.08; }
h2 { font-size:19px; font-weight:700; letter-spacing:-.01em; }
h3 { font-size:14px; font-weight:700; letter-spacing:-.005em; }
p  { margin:0; }
a  { color:var(--accent); }
.mono { font-family:"IBM Plex Mono", ui-monospace, monospace; font-variant-numeric:tabular-nums; }
.eyebrow {
  font:500 11px/1 "IBM Plex Mono", monospace; letter-spacing:.13em;
  text-transform:uppercase; color:var(--ink-3);
}
.lede { color:var(--ink-2); max-width:64ch; margin-top:14px; font-size:16px; }
header { border-bottom:1px solid var(--rule); padding-bottom:28px; }
section { margin-top:44px; }
.sec-head { display:flex; align-items:baseline; gap:12px; flex-wrap:wrap; margin-bottom:16px; }
.sec-head p { color:var(--ink-2); font-size:14px; }

/* ---- verdict: the whole point of the page, so it gets the weight ---- */
.verdict {
  margin-top:28px; border-radius:3px; padding:22px 24px;
  background:var(--raised); border:1px solid var(--rule);
  border-left:5px solid var(--vc, var(--ink-3)); box-shadow:var(--shadow);
  display:grid; gap:10px;
}
.verdict[data-level="strong"] { --vc:var(--good); }
.verdict[data-level="weak"]   { --vc:var(--warning); }
.verdict[data-level="none"]   { --vc:var(--critical); }
.verdict[data-level="thin"]   { --vc:var(--ink-3); }
.verdict-top { display:flex; align-items:center; gap:10px; flex-wrap:wrap; }
.verdict-icon { width:18px; height:18px; flex:none; }
.verdict-title { font-family:"Bricolage Grotesque",sans-serif; font-weight:700; font-size:17px; }
.verdict p { color:var(--ink-2); font-size:14.5px; max-width:70ch; }
.rho-row { display:flex; gap:26px; flex-wrap:wrap; padding-top:4px; }
.rho { display:flex; align-items:baseline; gap:7px; }
.rho b { font:500 21px/1 "IBM Plex Mono",monospace; font-variant-numeric:tabular-nums; }
.rho span { font-size:12px; color:var(--ink-3); }

/* ---- corpus tiles ---- */
.tiles { display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:1px;
         background:var(--rule); border:1px solid var(--rule); border-radius:3px; overflow:hidden; }
.tile { background:var(--raised); padding:16px 18px; display:grid; gap:5px; }
.tile .v { font:500 25px/1 "IBM Plex Mono",monospace; font-variant-numeric:tabular-nums; }
.tile .k { font:500 10.5px/1.3 "IBM Plex Mono",monospace; letter-spacing:.1em;
           text-transform:uppercase; color:var(--ink-3); }
.tile .n { font-size:12px; color:var(--ink-2); line-height:1.45; }
.tile.flag .v { color:var(--accent); }

/* ---- board diagram ---- */
.board-row { display:grid; grid-template-columns:minmax(0,260px) minmax(0,1fr); gap:28px; align-items:start; }
@media (max-width:720px) { .board-row { grid-template-columns:1fr; } }
.board { width:100%; max-width:260px; aspect-ratio:1; display:grid;
         grid-template-columns:repeat(16,1fr); gap:1px; background:var(--rule);
         border:1px solid var(--rule-2); border-radius:2px; overflow:hidden; }
.cell { background:var(--sunk); position:relative; }
.legend-ring { display:flex; flex-wrap:wrap; gap:10px 16px; margin-top:14px; }
.lr { display:flex; align-items:center; gap:6px; font-size:12px; color:var(--ink-2); }
.sw { width:11px; height:11px; border-radius:2px; flex:none; border:1px solid rgba(128,128,128,.28); }

/* ---- the piece table ---- */
.tbl-tools { display:flex; gap:10px; flex-wrap:wrap; align-items:center; margin-bottom:12px; }
.tbl-tools input, .tbl-tools select {
  font:400 13px Archivo,sans-serif; padding:7px 10px; border-radius:3px;
  border:1px solid var(--rule-2); background:var(--raised); color:var(--ink);
}
.tbl-tools label { font:500 11px/1 "IBM Plex Mono",monospace; letter-spacing:.1em;
                   text-transform:uppercase; color:var(--ink-3); }
/* Capped, not page-length: ~200 pieces would otherwise be 8000px of table with
   the detail panel stranded below it, off every screen. Sticky headers make the
   pane readable at any scroll position. */
.tbl-scroll { overflow:auto; max-height:min(58vh,560px);
              border:1px solid var(--rule); border-radius:3px; background:var(--raised); }
table { border-collapse:collapse; width:100%; font-size:13px; }
thead th {
  position:sticky; top:0; background:var(--raised); z-index:1;
  font:500 10.5px/1.3 "IBM Plex Mono",monospace; letter-spacing:.09em; text-transform:uppercase;
  color:var(--ink-3); text-align:left; padding:11px 12px; border-bottom:1px solid var(--rule-2);
  white-space:nowrap;
}
tbody td { padding:0 12px; height:34px; border-bottom:1px solid var(--rule); vertical-align:middle; }
tbody tr:last-child td { border-bottom:0; }
tbody tr { cursor:pointer; }
tbody tr:hover { background:var(--sunk); }
tbody tr[aria-selected="true"] { background:var(--sunk); box-shadow:inset 3px 0 0 var(--accent); }
td.num { font-family:"IBM Plex Mono",monospace; font-variant-numeric:tabular-nums; }
.strip { display:flex; gap:2px; }
.sc { width:20px; height:15px; border-radius:2px; }
.sortable { cursor:pointer; user-select:none; }
.sortable:hover { color:var(--ink); }
.sortable[aria-sort]:not([aria-sort="none"]) { color:var(--ink); }
.sortable .arr { opacity:.5; }

/* ---- detail panel ---- */
.detail { display:grid; grid-template-columns:minmax(0,260px) minmax(0,1fr); gap:26px;
          margin-top:18px; padding:20px; border:1px solid var(--rule); border-radius:3px;
          background:var(--raised); box-shadow:var(--shadow); align-items:start; }
@media (max-width:720px) { .detail { grid-template-columns:1fr; } }
.detail h3 { margin-bottom:4px; }
.detail .sub { font-size:13px; color:var(--ink-2); margin-bottom:14px; }
.bars { display:grid; gap:7px; }
.bar-row { display:grid; grid-template-columns:58px 1fr 60px; gap:10px; align-items:center; font-size:12.5px; }
.bar-track { height:13px; background:var(--sunk); border-radius:2px; overflow:hidden; position:relative; }
.bar-mid { position:absolute; top:0; bottom:0; width:1px; background:var(--rule-2); }
.bar-fill { height:100%; border-radius:2px; }
.bar-lab { font-family:"IBM Plex Mono",monospace; color:var(--ink-3); font-size:11px; }
.bar-val { font-family:"IBM Plex Mono",monospace; font-variant-numeric:tabular-nums;
           text-align:right; font-size:12px; }

/* ---- exclude block ---- */
.cmd { background:var(--sunk); border:1px solid var(--rule); border-radius:3px;
       padding:14px 16px; overflow-x:auto; margin-top:12px; }
.cmd pre { margin:0; font:400 12.5px/1.7 "IBM Plex Mono",monospace; color:var(--ink-2); white-space:pre; }
.cmd b { color:var(--ink); font-weight:500; }
.pill-row { display:flex; flex-wrap:wrap; gap:6px; margin-top:12px; }
.pill { font:500 12px "IBM Plex Mono",monospace; padding:4px 9px; border-radius:2px;
        background:var(--sunk); border:1px solid var(--rule-2); color:var(--ink); }
.caveat { margin-top:14px; font-size:13.5px; color:var(--ink-2); max-width:72ch; }
footer { margin-top:56px; padding-top:22px; border-top:1px solid var(--rule);
         font-size:12.5px; color:var(--ink-3); display:grid; gap:6px; }
.tt { position:fixed; z-index:50; pointer-events:none; opacity:0; transition:opacity .1s;
      background:var(--ink); color:var(--ground); padding:7px 10px; border-radius:3px;
      font:400 12px/1.45 "IBM Plex Mono",monospace; box-shadow:var(--shadow); max-width:250px; }
:focus-visible { outline:2px solid var(--accent); outline-offset:2px; }
@media (prefers-reduced-motion:reduce) { * { transition:none !important; animation:none !important; } }
</style>
"""


def build(data: dict) -> str:
    ag = data.get("agreement", {})
    core = (ag.get("core") or {}).get("mean_rho")
    alls = (ag.get("all") or {}).get("mean_rho")
    lead = core if core is not None else alls

    if lead is None:
        level, title = "thin", "Not enough boards yet"
        body = ("At least two sides need boards before they can be compared. "
                "Run the farm for longer, or with a lower stop row — four sides "
                "each filling 8 rows already cover the whole board.")
    elif lead > 0.5:
        level, title = "strong", "The four sides agree — the preference is real"
        body = ("Each side used different random borders, a different RNG stream and a "
                "different search direction, so they share almost no bias. Agreeing on "
                "the ranking this strongly means something about the puzzle is driving "
                "it, not something about the beam.")
    elif lead > 0.2:
        level, title = "weak", "A weak signal — present, but thin"
        body = ("The sides lean the same way without agreeing closely. That is what a "
                "real but small effect looks like, and also what an under-sampled one "
                "looks like. More borders will separate the two; more analysis of this "
                "corpus will not.")
    else:
        level, title = "none", "Nothing here — the sides disagree"
        body = ("Four independent views of the same frame rank the pieces differently, "
                "so the ranking below is noise or beam bias rather than a property of "
                "the puzzle. Acting on it would be fooling ourselves. More boards may "
                "change this; more analysis of these boards will not.")

    icons = {
        "strong": '<path d="M3 9.5 7 13.5 15 4.5" fill="none" stroke="var(--good)" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/>',
        "weak":   '<path d="M9 2.5 16.5 15.5H1.5Z" fill="none" stroke="var(--warning)" stroke-width="1.9" stroke-linejoin="round"/><path d="M9 7v3.6" stroke="var(--warning)" stroke-width="1.9" stroke-linecap="round"/><circle cx="9" cy="13.2" r="1" fill="var(--warning)"/>',
        "none":   '<circle cx="9" cy="9" r="7" fill="none" stroke="var(--critical)" stroke-width="2"/><path d="M6 12 12 6" stroke="var(--critical)" stroke-width="2" stroke-linecap="round"/>',
        "thin":   '<circle cx="9" cy="9" r="7" fill="none" stroke="var(--ink-3)" stroke-width="2"/><path d="M9 5.5v4.2" stroke="var(--ink-3)" stroke-width="2" stroke-linecap="round"/><circle cx="9" cy="12.8" r="1.05" fill="var(--ink-3)"/>',
    }

    def rho_txt(v):
        return "n/a" if v is None else f"{v:+.3f}"

    pieces = data.get("pieces", [])
    n_eff = data.get("n_eff", 0)
    configs = data.get("configs", 0)
    boards = data.get("boards_used", data.get("boards", 0))
    per_side = data.get("per_side_configs", {})
    excl = data.get("exclude_suggestion", [])
    control = data.get("control_clue_pieces", [])
    ctrl_ok = all(c.get("ok") for c in control) if control else None

    side_bits = " · ".join(
        f'<span class="mono">s{s}</span> {per_side.get(str(s), 0)}' for s in range(4))

    ctrl_line = ("control passed: all five pinned clue pieces recovered at their own cells"
                 if ctrl_ok else
                 "CONTROL FAILED — the corpus is not on the canonical frame"
                 if ctrl_ok is False else "control not run")

    pill_html = "".join(f'<span class="pill">{p}</span>' for p in excl)
    excl_csv = ",".join(str(p) for p in excl)

    payload = json.dumps({
        "pieces": pieces,
        "cell_map": data.get("cell_map", {}),
        "ring_share": data.get("ring_share", {}),
        "agreement": ag,
    }, separators=(",", ":"))

    corpus_name = html.escape(str(data.get("corpus", "")))

    excl_section = f"""
  <section>
    <div class="sec-head">
      <h2>Pieces that belong in rows 12&ndash;15</h2>
      <p>The band the beam never fills.</p>
    </div>
    <p class="lede" style="margin-top:0">
      The beamer builds bottom-up, fills row 11 and dies attempting row 12. A piece
      that concentrates in the top band is a piece it should not be spending low
      down &mdash; so bar it from the chain database and give the search back the
      budget. These {len(excl)} rank highest by lift, not by raw frequency.
    </p>
    <div class="pill-row">{pill_html or '<span class="pill">none above chance</span>'}</div>
    <div class="cmd"><pre><b>Test it.</b> Same frame, one row deeper, with and without:

  bin/E555_beamer_FixedFrame data/seed_Edge5.txt --clue_orient 0 --stop_row 12 \\
      --db_file excl.db --wall_time 1800 \\
      --exclude_pieces {excl_csv or "&lt;none&gt;"}

Then count <b>filled=12</b> lines in each log. That number decides this,
not anything on this page.</pre></div>
    <p class="caveat">
      This list is specific to <em>this</em> frame &mdash; orientation 0 with that
      corner assignment. It does not transfer to the production beamer, which hedges
      across all four orientations and samples its own corners.
    </p>
  </section>"""

    return (
        TEMPLATE_HEAD
        + f"""
<div class="wrap">
<header>
  <p class="eyebrow">E555 &middot; Stage B &middot; fixed-frame corpus</p>
  <h1>Where the pieces actually land</h1>
  <p class="lede">
    {boards:,} row-11 partial boards from {configs:,} independent border configurations,
    harvested from all four sides of one pinned frame and turned onto shared
    coordinates. None of them solves anything. The question is whether individual
    pieces still prefer a region of the board &mdash; and whether that can be handed
    back to the search.
  </p>
</header>

<div class="verdict" data-level="{level}">
  <div class="verdict-top">
    <svg class="verdict-icon" viewBox="0 0 18 18" aria-hidden="true">{icons[level]}</svg>
    <span class="verdict-title">{title}</span>
  </div>
  <p>{body}</p>
  <div class="rho-row">
    <div class="rho"><b>{rho_txt(core)}</b><span>core only &middot; all four sides see it</span></div>
    <div class="rho"><b>{rho_txt(alls)}</b><span>all observed cells</span></div>
  </div>
</div>

<section>
  <div class="sec-head">
    <h2>How big the corpus really is</h2>
    <p>Boards are not observations. Borders are.</p>
  </div>
  <div class="tiles">
    <div class="tile"><span class="k">Boards</span><span class="v">{boards:,}</span>
      <span class="n">Row-11 partials, 192 cells placed each.</span></div>
    <div class="tile"><span class="k">Border configs</span><span class="v">{configs:,}</span>
      <span class="n">{side_bits}</span></div>
    <div class="tile flag"><span class="k">N<sub>eff</sub></span><span class="v">{n_eff:,.0f}</span>
      <span class="n">Kish effective size after weighting each border once. This is the
      number to trust.</span></div>
    <div class="tile"><span class="k">Frame check</span><span class="v">{'PASS' if ctrl_ok else 'FAIL' if ctrl_ok is False else '&mdash;'}</span>
      <span class="n">{ctrl_line}</span></div>
  </div>
</section>

<section>
  <div class="sec-head">
    <h2>Rings</h2>
    <p>ring(r,c) = min(r, c, 15&minus;r, 15&minus;c)</p>
  </div>
  <div class="board-row">
    <div>
      <div class="board" id="board" aria-label="16 by 16 board coloured by ring index"></div>
      <div class="legend-ring" id="ringLegend"></div>
    </div>
    <div>
      <p class="lede" style="margin-top:0">
        Ring 0 is the frame itself, where the 60 corner and edge pieces go. The 196
        inner pieces live in rings 1&ndash;7 &mdash; which hold exactly 196 cells. Ring 7
        is the 2&times;2 centre, and the orientation-0 centre clue is pinned in it.
      </p>
      <p class="lede">
        Seven buckets instead of 256 cells is the whole trick: it keeps enough
        observations in each bucket to mean something at this corpus size. A piece's
        <em>lift</em> in a ring is how much more often it lands there than an average
        inner piece does &mdash; 1.00&times; is no preference at all.
      </p>
    </div>
  </div>
</section>

<section>
  <div class="sec-head">
    <h2>Every inner piece, by ring</h2>
    <p>Click a row for its map. The five pinned clue pieces are excluded.</p>
  </div>
  <div class="tbl-tools">
    <label for="q">Filter</label>
    <input id="q" type="search" placeholder="piece id…" size="10">
    <label for="minobs">Min obs</label>
    <select id="minobs">
      <option value="0">any</option><option value="10">10</option>
      <option value="25">25</option><option value="50">50</option>
    </select>
    <span class="mono" id="count" style="color:var(--ink-3);font-size:12px"></span>
  </div>
  <div class="tbl-scroll">
    <table>
      <thead><tr>
        <th class="sortable" data-k="piece">Piece <span class="arr"></span></th>
        <th>Ring 1 &rarr; 7 (lift)</th>
        <th class="sortable" data-k="mean_ring">Mean ring <span class="arr"></span></th>
        <th class="sortable" data-k="best_lift">Strongest <span class="arr"></span></th>
        <th class="sortable" data-k="top_lift">Top band <span class="arr"></span></th>
        <th class="sortable" data-k="obs">Obs <span class="arr"></span></th>
      </tr></thead>
      <tbody id="tbody"></tbody>
    </table>
  </div>
  <div class="detail" id="detail" hidden>
    <div>
      <div class="board" id="pieceBoard" aria-label="Where the selected piece landed"></div>
      <div class="legend-ring"><span class="lr">
        <span class="sw" style="background:var(--sunk)"></span>never</span>
        <span class="lr"><span class="sw" style="background:var(--pos-2)"></span>most often</span>
      </div>
    </div>
    <div>
      <h3 id="dTitle">&mdash;</h3>
      <p class="sub" id="dSub">&mdash;</p>
      <div class="bars" id="dBars"></div>
    </div>
  </div>
</section>
{excl_section}

<footer>
  <span>Corpus: <span class="mono">{corpus_name}</span></span>
  <span>Generated by <span class="mono">tests/E555_frame_dashboard.py</span> from
  <span class="mono">summary.json</span>. Every number weights each border config
  once, not each board.</span>
</footer>
</div>
<div class="tt" id="tt" role="tooltip"></div>
<script id="data" type="application/json">{payload}</script>
"""
        + SCRIPT
    )


SCRIPT = r"""<script>
const D = JSON.parse(document.getElementById('data').textContent);
const SIDE = 16;
const ringOf = c => Math.min((c/SIDE)|0, c%SIDE, SIDE-1-((c/SIDE)|0), SIDE-1-(c%SIDE));

/* Ring shading on the reference board: one hue, light to dark, ring 0 recessive. */
const RING_TINT = ['var(--sunk)','#cde2fb','#b7d3f6','#9ec5f4','#86b6ef','#6da7ec','#3987e5','#1c5cab'];

const tt = document.getElementById('tt');
function showTip(e, text){
  tt.textContent = text; tt.style.opacity = 1;
  const r = tt.getBoundingClientRect();
  let x = e.clientX + 14, y = e.clientY + 14;
  if (x + r.width  > innerWidth  - 8) x = e.clientX - r.width  - 12;
  if (y + r.height > innerHeight - 8) y = e.clientY - r.height - 12;
  tt.style.left = x + 'px'; tt.style.top = y + 'px';
}
const hideTip = () => { tt.style.opacity = 0; };

/* reference board */
const board = document.getElementById('board');
for (let c = 0; c < 256; c++){
  const r = (c/SIDE)|0, col = c%SIDE, k = ringOf(c);
  const d = document.createElement('div');
  d.className = 'cell';
  d.style.background = RING_TINT[k];
  /* Row 0 is the BOTTOM everywhere in this toolkit, so draw it bottom-up. */
  d.style.gridRow = (SIDE - r); d.style.gridColumn = (col + 1);
  d.addEventListener('pointerenter', e => showTip(e, `row ${r} col ${col} · ring ${k}`));
  d.addEventListener('pointerleave', hideTip);
  board.appendChild(d);
}
const RING_CELLS = [60,52,44,36,28,20,12,4];
document.getElementById('ringLegend').innerHTML = RING_TINT.map((t,i) =>
  `<span class="lr"><span class="sw" style="background:${t}"></span>ring ${i} · ${RING_CELLS[i]} cells</span>`).join('');

/* diverging scale around lift = 1: red = avoids, blue = prefers, grey = neither */
function liftColor(v){
  if (v == null) return 'var(--sunk)';
  if (v >= 2.5) return 'var(--pos-3)';
  if (v >= 1.6) return 'var(--pos-2)';
  if (v >= 1.15) return 'var(--pos-1)';
  if (v > 0.85) return 'var(--mid)';
  if (v > 0.5)  return 'var(--neg-1)';
  if (v > 0.25) return 'var(--neg-2)';
  return 'var(--neg-3)';
}

let sortKey = 'mean_ring', sortDir = 1, selected = null;
const tbody = document.getElementById('tbody');

function visible(){
  const q = document.getElementById('q').value.trim();
  const mo = +document.getElementById('minobs').value;
  return D.pieces
    .filter(p => p.obs >= mo && (!q || String(p.piece).includes(q)))
    .sort((a,b) => (a[sortKey] - b[sortKey]) * sortDir);
}

function render(){
  const rows = visible();
  document.getElementById('count').textContent =
    rows.length + ' of ' + D.pieces.length + ' pieces';
  tbody.innerHTML = rows.map(p => {
    const strip = [1,2,3,4,5,6,7].map(k => {
      const v = p['lift'+k];
      return `<span class="sc" data-p="${p.piece}" data-k="${k}" style="background:${liftColor(v)}"></span>`;
    }).join('');
    return `<tr data-p="${p.piece}" aria-selected="${selected === p.piece}">
      <td class="num">${p.piece}</td>
      <td><span class="strip">${strip}</span></td>
      <td class="num">${p.mean_ring.toFixed(2)}</td>
      <td class="num">${p.best_lift.toFixed(2)}&times; <span style="color:var(--ink-3)">r${p.best_ring}</span></td>
      <td class="num">${p.top_lift ? p.top_lift.toFixed(2)+'&times;' : '&mdash;'}</td>
      <td class="num">${Math.round(p.obs)}</td>
    </tr>`;
  }).join('');
}

tbody.addEventListener('pointerover', e => {
  const sc = e.target.closest('.sc'); if (!sc) return;
  const p = D.pieces.find(x => x.piece == sc.dataset.p), k = sc.dataset.k;
  showTip(e, `piece ${p.piece} · ring ${k}\\n${p['lift'+k].toFixed(2)}x expected\\n` +
             `${(p['p_ring'+k]*100).toFixed(1)}% of its placements`);
});
tbody.addEventListener('pointerout', e => { if (e.target.closest('.sc')) hideTip(); });
tbody.addEventListener('click', e => {
  const tr = e.target.closest('tr'); if (!tr) return;
  select(+tr.dataset.p);
});

document.querySelectorAll('.sortable').forEach(th => {
  th.addEventListener('click', () => {
    const k = th.dataset.k;
    sortDir = (k === sortKey) ? -sortDir : (k === 'piece' || k === 'mean_ring' ? 1 : -1);
    sortKey = k;
    document.querySelectorAll('.sortable').forEach(o => {
      o.setAttribute('aria-sort','none');
      o.querySelector('.arr').textContent = '';
    });
    th.setAttribute('aria-sort', sortDir > 0 ? 'ascending' : 'descending');
    th.querySelector('.arr').textContent = sortDir > 0 ? '\\u2191' : '\\u2193';
    render();
  });
});
document.getElementById('q').addEventListener('input', render);
document.getElementById('minobs').addEventListener('change', render);

/* per-piece detail */
const pBoard = document.getElementById('pieceBoard');
for (let c = 0; c < 256; c++){
  const r = (c/SIDE)|0, col = c%SIDE;
  const d = document.createElement('div');
  d.className = 'cell'; d.dataset.c = c;
  d.style.gridRow = (SIDE - r); d.style.gridColumn = (col + 1);
  pBoard.appendChild(d);
}

function select(pid){
  selected = pid;
  const p = D.pieces.find(x => x.piece === pid);
  const map = D.cell_map[String(pid)] || {};
  const max = Math.max(1, ...Object.values(map));
  pBoard.querySelectorAll('.cell').forEach(d => {
    const v = map[d.dataset.c] || 0;
    if (!v) { d.style.background = 'var(--sunk)'; d.title = ''; return; }
    const t = v / max;
    d.style.background = t > .66 ? 'var(--pos-3)' : t > .33 ? 'var(--pos-2)' : 'var(--pos-1)';
    const r = (d.dataset.c/SIDE)|0, col = d.dataset.c%SIDE;
    d.title = `row ${r} col ${col}: ${v.toFixed(1)}`;
  });
  document.getElementById('dTitle').textContent = 'Piece ' + pid;
  document.getElementById('dSub').textContent =
    `mean ring ${p.mean_ring.toFixed(2)} · strongest in ring ${p.best_ring} ` +
    `at ${p.best_lift.toFixed(2)}x · ${Math.round(p.obs)} weighted observations`;
  document.getElementById('dBars').innerHTML = [1,2,3,4,5,6,7].map(k => {
    const v = p['lift'+k];
    /* bar centred on 1.0: half the track is "below expected", half "above" */
    const HALF = 50, cap = 3;
    const w = Math.min(Math.abs(v - 1) / (cap - 1), 1) * HALF;
    const left = v >= 1 ? HALF : HALF - w;
    return `<div class="bar-row">
      <span class="bar-lab">ring ${k}</span>
      <span class="bar-track"><span class="bar-mid" style="left:${HALF}%"></span>
        <span class="bar-fill" style="margin-left:${left}%;width:${w}%;background:${liftColor(v)}"></span></span>
      <span class="bar-val">${v.toFixed(2)}&times;</span>
    </div>`;
  }).join('');
  document.getElementById('detail').hidden = false;
  render();
}

document.querySelector('.sortable[data-k="mean_ring"]').setAttribute('aria-sort','ascending');
document.querySelector('.sortable[data-k="mean_ring"] .arr').textContent = '\\u2191';
render();
if (D.pieces.length) select(visible()[0].piece);
</script>
</script>
"""



def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Render summary.json as a standalone HTML dashboard.")
    ap.add_argument("summary", help="stats summary.json")
    ap.add_argument("--out", default="frame_dashboard.html")
    args = ap.parse_args(argv)

    data = json.loads(Path(args.summary).read_text())
    Path(args.out).write_text(build(data))
    print(f"[out] {args.out}  ({Path(args.out).stat().st_size/1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
