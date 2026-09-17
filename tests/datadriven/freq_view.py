#!/usr/bin/env python3
"""freq_view.py -- look at a data-driven frequency table.

    python3 freq_view.py TABLE [--alpha A] [--out FILE.html] [--text]

Reads a table written by `E555_beamer_datadriven --learn`, applies the same
estimator the search applies (row-marginal backoff, Dirichlet shrinkage by
alpha, Sinkhorn balance, log weights) and writes ONE self-contained HTML file:
inline SVG, no libraries, no fonts, nothing to install. Open it in a browser.

Two reasons it re-implements the estimator rather than reading processed
weights out of the file. The table on disk holds RAW counts, so alpha can be
explored here without re-learning anything -- the alpha sweep panel is the point.
And an independent implementation is a check on the C one: --check compares the
two and prints the largest disagreement.

  --alpha A     shrinkage toward the piece-by-row prior (default 20, as the C).
  --text        terse terminal summary instead of HTML.
  --check DUMP  compare against the weights the C wrote via E555_FREQ_DUMP=DUMP.
"""
import argparse, html, json, math, os, sys
from collections import defaultdict

SIDE, NP = 16, 256
BETA = 1.0                      # must match FREQ_BETA in the C
SINK_ITERS, SINK_TOL = 400, 1e-13


# ---------------------------------------------------------------- the table

class Table:
    def __init__(self, path):
        self.meta, self.cnt, self.wcell, self.wsq = {}, {}, {}, {}
        self.passes, self.pkind, self.pside = {}, {}, {}
        self.free_cells = []
        with open(path) as f:
            for line in f:
                if line.startswith('#') or not line.strip():
                    continue
                f0 = line.split()
                if f0[0] == 'cnt':
                    self.cnt[(int(f0[1]), int(f0[2]))] = float(f0[3])
                elif f0[0] == 'wcell':
                    self.wcell[int(f0[1])] = float(f0[2])
                    self.wsq[int(f0[1])] = float(f0[3])
                elif f0[0] == 'pass':
                    self.passes[int(f0[1])] = int(f0[3])
                elif f0[0] == 'freep':
                    self.pkind[int(f0[1])] = int(f0[2])
                    self.pside[int(f0[1])] = int(f0[3])
                elif f0[0] == 'freec':
                    self.free_cells.append(int(f0[1]))
                elif f0[0] == 'lift':
                    self.meta['lift'] = (float(f0[1]), int(f0[2]))
                else:
                    self.meta[f0[0]] = ' '.join(f0[1:])
        if 'version' not in self.meta:
            sys.exit(f"{path}: not an E555 datadriven table")
        self.pieces = sorted(self.pkind)
        self.cells = sorted(self.free_cells)
        if not self.pieces or not self.cells:
            sys.exit(f"{path}: no freep/freec lines -- relearn with a build that "
                     f"writes them (the blocks cannot be reconstructed without them)")
        self.covered = sorted(c for c in self.free_cells if self.wcell.get(c, 0) > 0)

    def ess(self, cell):
        w, q = self.wcell.get(cell, 0.0), self.wsq.get(cell, 0.0)
        return (w * w / q) if q > 0 else 0.0


def cell_kind(cell):
    r, c = divmod(cell, SIDE)
    return (r == 0) + (r == SIDE - 1) + (c == 0) + (c == SIDE - 1)


def sinkhorn(m, n):
    """Doubly stochastic by iterative proportional fitting -- the same balance
    the C applies, and for the same reason: a row of the board is a permutation,
    so an unbalanced table lets globally popular pieces be spent on the rows the
    beam commits first."""
    ru, cu = [1.0] * n, [1.0] * n
    for _ in range(SINK_ITERS):
        worst = 0.0
        for i in range(n):
            base = i * n
            s = sum(m[base + k] * cu[k] for k in range(n)) * ru[i]
            if s > 0:
                ru[i] /= s
            worst = max(worst, abs(s - 1.0))
        for k in range(n):
            s = sum(m[i * n + k] * ru[i] for i in range(n)) * cu[k]
            if s > 0:
                cu[k] /= s
            worst = max(worst, abs(s - 1.0))
        if worst < SINK_TOL:
            break
    for i in range(n):
        for k in range(n):
            m[i * n + k] *= ru[i] * cu[k]
    return m


def build(tab, alpha):
    """Counts -> log weights, block by block. Returns {(piece,cell): nats}."""
    w = {}
    blocks = []
    inner_p = [p for p in tab.pieces if cell_kind_of_piece(tab, p) == 0]
    inner_c = [c for c in tab.free_cells if cell_kind(c) == 0]
    if inner_p and len(inner_p) != len(inner_c):
        sys.exit(f"interior block is {len(inner_p)} pieces x {len(inner_c)} cells "
                 f"-- not square; the table is inconsistent")
    if inner_p:
        blocks.append(('interior', inner_p, inner_c, True))
    for s in range(4):
        bp = [p for p in tab.pieces if cell_kind_of_piece(tab, p) == 1
              and piece_side(tab, p) == s]
        bc = [c for c in tab.free_cells if cell_kind(c) == 1 and cell_side(c) == s]
        if bp and len(bp) == len(bc):
            blocks.append((f'side{s}', bp, bc, False))

    for name, pl, cl, use_rowprior in blocks:
        n = len(pl)
        rowm = defaultdict(float)
        rowt = defaultdict(float)
        if use_rowprior:
            for i, p in enumerate(pl):
                for c in cl:
                    v = tab.cnt.get((p, c), 0.0)
                    if v:
                        r = c // SIDE
                        rowm[(i, r)] += v
                        rowt[r] += v
        m = [0.0] * (n * n)
        for i, p in enumerate(pl):
            for k, c in enumerate(cl):
                if use_rowprior:
                    r = c // SIDE
                    prior = (rowm[(i, r)] + BETA / n) / (rowt[r] + BETA)
                else:
                    prior = 1.0 / n
                m[i * n + k] = ((tab.cnt.get((p, c), 0.0) + alpha * prior)
                                / (tab.wcell.get(c, 0.0) + alpha))
        sinkhorn(m, n)
        for i, p in enumerate(pl):
            for k, c in enumerate(cl):
                w[(p, c)] = math.log(n * m[i * n + k])
    return w


def cell_kind_of_piece(tab, p):
    return tab.pkind.get(p, 0)


def cell_side(cell):
    r, c = divmod(cell, SIDE)
    if r == SIDE - 1: return 0
    if c == SIDE - 1: return 1
    if r == 0:        return 2
    if c == 0:        return 3
    return -1


def piece_side(tab, p):
    return tab.pside.get(p, -1)


# ---------------------------------------------------------------- rendering
# Colour follows the job, not taste. The weights are signed around zero (zero is
# chance), so they get a DIVERGING ramp: one cool hue for "favoured here", one
# warm for "avoided here", and a neutral grey midpoint that reads as "nothing" --
# equal step count per arm. Coverage and effective sample size are non-negative
# magnitudes, so they get a SEQUENTIAL single-hue ramp. No rainbow anywhere, and
# no hue at a diverging midpoint.
# Both ramps are CSS custom properties, so the light and dark steps swap in one
# place and the marks are written against roles. Dark is SELECTED, not flipped:
# on a dark surface "near zero" has to sit near the surface and high has to be
# bright, so each ramp is re-stepped from the same hue rather than inverted.
DIV_POS = [f'var(--pos-{i})' for i in range(1, 7)]
DIV_NEG = [f'var(--neg-{i})' for i in range(1, 7)]
SEQ     = [f'var(--seq-{i})' for i in range(1, 8)]

RAMP_CSS_LIGHT = {
    'pos': ['#dce9fb', '#b7d3f6', '#86b6ef', '#5598e7', '#2a78d6', '#184f95'],
    'neg': ['#fadedd', '#f4bfbe', '#eb9a99', '#e07170', '#cf4544', '#94302f'],
    'seq': ['#e8f0fd', '#cde2fb', '#9ec5f4', '#6da7ec', '#3987e5', '#256abf', '#0d366b'],
}
RAMP_CSS_DARK = {
    'pos': ['#15314f', '#184f95', '#256abf', '#3987e5', '#6da7ec', '#9ec5f4'],
    'neg': ['#4a201f', '#7d2f2e', '#a03b39', '#cf4544', '#e07170', '#eb9a99'],
    'seq': ['#14243a', '#0d366b', '#184f95', '#256abf', '#3987e5', '#6da7ec', '#9ec5f4'],
}


def ramp_vars(m):
    return ' '.join(f'--{k}-{i+1}:{v};' for k, a in m.items() for i, v in enumerate(a))


def robust_scale(vals, q=0.95):
    """The colour scale is set by the 95th percentile of |value|, not the max.
    A handful of extreme cells would otherwise compress everything else onto the
    two palest steps and hide exactly the mid-range structure these panels exist
    to show; values past the scale simply sit on the end step."""
    a = sorted(abs(v) for v in vals)
    if not a:
        return 1.0
    return a[min(len(a) - 1, int(q * len(a)))] or (a[-1] or 1.0)


def div_color(v, scale):
    """Diverging: sign picks the arm, magnitude picks the step."""
    if scale <= 0 or abs(v) < 1e-9:
        return 'var(--mid)'
    arm = DIV_POS if v > 0 else DIV_NEG
    i = min(len(arm) - 1, int(abs(v) / scale * len(arm)))
    return arm[i]


def seq_color(v, vmax):
    if vmax <= 0:
        return 'var(--mid)'
    return SEQ[min(len(SEQ) - 1, int(v / vmax * len(SEQ)))]


def grid(cells, cols, rows, cw, ch, title, note, legend, flip_y=True):
    """One heatmap. cells is a list of (col, row, fill, tooltip); every cell
    carries its own hover text, so nothing is readable by colour alone."""
    w, h = cols * cw, rows * ch
    out = [f'<figure class="panel"><figcaption><h3>{html.escape(title)}</h3>'
           f'<p class="note">{note}</p></figcaption>',
           f'<div class="scroll"><svg viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
           f'role="img" aria-label="{html.escape(title)}">']
    for (c, r, fill, tip) in cells:
        y = (rows - 1 - r) * ch if flip_y else r * ch
        out.append(f'<rect x="{c*cw}" y="{y}" width="{cw-0.6:.1f}" height="{ch-0.6:.1f}" '
                   f'rx="1" fill="{fill}" tabindex="0" data-t="{html.escape(tip)}"/>')
    out.append('</svg></div>')
    out.append(legend)
    out.append('</figure>')
    return '\n'.join(out)


def div_legend(scale, unit='nats vs chance'):
    sw = []
    for i, c in enumerate(reversed(DIV_NEG)):
        sw.append(f'<i style="background:{c}"></i>')
    sw.append('<i style="background:var(--mid)"></i>')
    for c in DIV_POS:
        sw.append(f'<i style="background:{c}"></i>')
    return (f'<div class="legend"><span>avoided</span>{"".join(sw)}'
            f'<span>preferred</span>'
            f'<em>&plusmn;{scale:.1f} {unit} (95th pct; beyond it sits on the end step)'
            f'</em></div>')


def seq_legend(vmax, unit):
    sw = ''.join(f'<i style="background:{c}"></i>' for c in SEQ)
    return (f'<div class="legend"><span>0</span>{sw}'
            f'<span>{vmax:,.0f}</span><em>{unit}</em></div>')


def tile(value, label, sub=''):
    return (f'<div class="tile"><b>{value}</b><span>{html.escape(label)}</span>'
            f'<em>{html.escape(sub)}</em></div>')


CSS = """
:root{color-scheme:light dark;
 --plane:#f9f9f7; --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781;
 --rule:#e1e0d9; --mid:#f0efec; --ring:rgba(11,11,11,.10); %LIGHT%}
@media (prefers-color-scheme:dark){:root:where(:not([data-theme=light])){
 --plane:#0d0d0d; --surface:#1a1a19; --ink:#fff; --ink2:#c3c2b7; --muted:#898781;
 --rule:#2c2c2a; --mid:#383835; --ring:rgba(255,255,255,.10); %DARK%}}
:root[data-theme=dark]{
 --plane:#0d0d0d; --surface:#1a1a19; --ink:#fff; --ink2:#c3c2b7; --muted:#898781;
 --rule:#2c2c2a; --mid:#383835; --ring:rgba(255,255,255,.10); %DARK%}
*{box-sizing:border-box}
body{margin:0;padding:24px 16px 72px;background:var(--plane);color:var(--ink);
 font:14px/1.55 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
 font-variant-numeric:tabular-nums}
.wrap{max-width:1180px;margin:0 auto}
h1{font-size:23px;margin:0 0 4px;letter-spacing:-.01em}
h2{font-size:15px;margin:36px 0 12px;color:var(--ink2);text-transform:uppercase;
 letter-spacing:.07em;font-weight:650}
h3{font-size:15px;margin:0 0 2px;font-weight:620}
.sub{color:var(--ink2);margin:0 0 18px}
.note{color:var(--muted);margin:0 0 10px;font-size:12.5px;max-width:74ch}
.tiles{display:flex;flex-wrap:wrap;gap:10px;margin:16px 0 8px}
.tile{background:var(--surface);border:1px solid var(--ring);border-radius:10px;
 padding:11px 15px;min-width:132px}
.tile b{display:block;font-size:25px;font-weight:660;letter-spacing:-.02em;
 white-space:nowrap}
.tile span{display:block;color:var(--ink2);font-size:12.5px}
.tile em{display:block;color:var(--muted);font-size:11.5px;font-style:normal}
.panel{background:var(--surface);border:1px solid var(--ring);border-radius:12px;
 padding:15px 16px 12px;margin:0 0 16px}
.row{display:flex;flex-wrap:wrap;gap:16px}
.row>.panel{flex:1 1 300px;margin:0}
.scroll{overflow-x:auto;padding-bottom:4px}
svg rect{shape-rendering:crispEdges}
svg rect:hover,svg rect:focus{stroke:var(--ink);stroke-width:1.2;outline:none;
 shape-rendering:geometricPrecision}
.legend{display:flex;align-items:center;gap:7px;margin-top:9px;color:var(--muted);
 font-size:11.5px;flex-wrap:wrap}
.legend i{width:16px;height:11px;border-radius:2px;display:inline-block;
 box-shadow:0 0 0 1px var(--ring) inset}
.legend em{font-style:normal;margin-left:auto;color:var(--ink2)}
#tip{position:fixed;pointer-events:none;background:var(--ink);color:var(--plane);
 padding:5px 9px;border-radius:6px;font-size:12px;opacity:0;transition:opacity .1s;
 z-index:9;max-width:280px}
table{border-collapse:collapse;width:100%;font-size:12.5px}
th,td{text-align:right;padding:4px 9px;border-bottom:1px solid var(--rule)}
th:first-child,td:first-child{text-align:left}
th{color:var(--ink2);font-weight:620}
details{margin-top:6px}summary{cursor:pointer;color:var(--ink2);font-size:13px}
.meta{font-size:12px;color:var(--muted);line-height:1.7;
 font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.warn{color:#d03b3b;font-weight:600}
button{font:inherit;background:var(--surface);color:var(--ink);cursor:pointer;
 border:1px solid var(--ring);border-radius:8px;padding:5px 11px}
input[type=number]{font:inherit;background:var(--surface);color:var(--ink);
 border:1px solid var(--ring);border-radius:8px;padding:5px 9px;width:92px}
.ctl{display:flex;gap:9px;align-items:center;margin-bottom:10px;color:var(--ink2)}
"""

JS = """
const tip=document.getElementById('tip');
function show(e){const t=e.target.dataset.t;if(!t)return;
 tip.textContent=t;tip.style.opacity=1;
 const r=e.target.getBoundingClientRect();
 tip.style.left=Math.min(innerWidth-300,r.left)+'px';
 tip.style.top=Math.max(4,r.top-30)+'px';}
function hide(){tip.style.opacity=0;}
addEventListener('mouseover',show);addEventListener('mouseout',hide);
addEventListener('focusin',show);addEventListener('focusout',hide);
document.getElementById('theme').onclick=()=>{
 const d=document.documentElement;
 d.dataset.theme=d.dataset.theme==='dark'?'light':'dark';};
const PM=window.PIECEMAP||{}, PS=window.PSCALE||1, RAMP=window.RAMPS||{};
function col(v){if(Math.abs(v)<1e-9)return'var(--mid)';
 const a=v>0?RAMP.pos:RAMP.neg;
 return a[Math.min(a.length-1,Math.floor(Math.abs(v)/PS*a.length))];}
function drawPiece(){
 const p=+document.getElementById('pnum').value;
 const g=document.getElementById('pmap'), d=PM[p];
 document.getElementById('plabel').textContent=
   d?('piece '+p+' -- '+d.length+' cells it was measured on'):
     ('piece '+p+' is not a free piece in this table');
 if(!d){g.innerHTML='';return;}
 let h='';
 for(const [cell,v] of d){const r=cell>>4,c=cell&15;
  h+='<rect x="'+(c*22)+'" y="'+((15-r)*22)+'" width="21.4" height="21.4" rx="1" fill="'
    +col(v)+'" tabindex="0" data-t="cell '+cell+' (row '+r+', col '+c+') '
    +(v>=0?'+':'')+v.toFixed(2)+' nats"/>';}
 g.innerHTML=h;}
document.getElementById('pnum').oninput=drawPiece;drawPiece();
"""


# ---------------------------------------------------------------- the report

def piece_row_matrix(tab, w):
    """Mean weight of each piece over each interior row -- the piece-by-row view
    the estimator itself backs off to, and the one place the whole idea is
    visible or not: if low pieces prefer low rows and top pieces the top, the
    sorted matrix reads as a diagonal."""
    pr, rows = {}, range(1, SIDE - 1)
    for p in (q for q in tab.pieces if cell_kind_of_piece(tab, q) == 0):
        for r in rows:
            v = [w[(p, r * SIDE + c)] for c in rows if (p, r * SIDE + c) in w]
            pr[(p, r)] = sum(v) / len(v) if v else 0.0
    return pr


def render(tab, w, alpha, src):
    rows = list(range(1, SIDE - 1))
    ip = [p for p in tab.pieces if cell_kind_of_piece(tab, p) == 0]
    pr = piece_row_matrix(tab, w)
    prs = robust_scale(pr.values())
    ws = robust_scale(w.values())

    # Sort pieces by the row they most prefer, so structure (if any) is a diagonal
    # rather than noise. Ties by the softmax centroid, which is smooth.
    def centroid(p):
        e = [(r, math.exp(pr[(p, r)])) for r in rows]
        tot = sum(x for _, x in e) or 1.0
        return (max(e, key=lambda t: t[1])[0], sum(r * x for r, x in e) / tot)
    order = sorted(ip, key=centroid)

    lift, lift_n = tab.meta.get('lift', (0.0, 0))
    ncov = len(tab.covered)
    ess = [tab.ess(c) for c in tab.covered]
    mean_ess = sum(ess) / len(ess) if ess else 0.0
    thin = sum(1 for e in ess if e < 30)
    P = []

    # --- stat tiles -------------------------------------------------------
    warn = ' class="warn"' if lift <= 0 else ''
    P.append('<div class="tiles">'
             + f'<div class="tile"><b{warn}>{lift:+.3f}</b>'
               f'<span>held-out lift, nats/cell</span>'
               f'<em>over {lift_n:,} boards</em></div>'
             + tile(f'{ncov}<span style="font-size:15px;color:var(--muted)"> / '
                    f'{len(tab.free_cells)}</span>', 'free cells measured',
                    f'{thin} with sample size < 30')
             + tile(f'{mean_ess:,.0f}', 'mean effective sample size', 'per measured cell')
             + tile('<span style="font-size:19px">'
                    + ' &middot; '.join(f'{n:,}' for _, n in sorted(tab.passes.items()))
                    + '</span>',
                    'configurations per pass (p0/p1/p2/p3)',
                    'growing from each of the four sides')
             + tile(f'{alpha:g}', 'alpha (shrinkage)', 'pseudo-configurations')
             + '</div>')
    if lift <= 0:
        P.append('<p class="note warn">The held-out lift is not positive: this table '
                 'carries no signal and searching with it is not worth the run.</p>')

    # --- headline: piece x row -------------------------------------------
    cells = []
    for i, p in enumerate(order):
        for r in rows:
            v = pr[(p, r)]
            cells.append((i, r - 1, div_color(v, prs),
                          f'piece {p} - row {r} - {v:+.2f} nats vs chance'))
    P.append('<h2>Where each piece wants to sit</h2>')
    P.append(grid(cells, len(order), len(rows), 6, 17,
                  'Piece by interior row',
                  'One column per free interior piece, sorted by the row it prefers; '
                  'rows run bottom-up as everywhere else in this repo. A diagonal band '
                  'means the table has learned height. A flat wash means it has not.',
                  div_legend(prs)))

    # --- board maps -------------------------------------------------------
    def board(fn, title, note, unit):
        vals = {c: fn(c) for c in tab.free_cells}
        vmax = max(vals.values(), default=0.0)
        cs = [(c % SIDE, c // SIDE, seq_color(vals[c], vmax),
               f'cell {c} (row {c//SIDE}, col {c%SIDE}) - {vals[c]:,.1f} {unit}')
              for c in tab.free_cells]
        return grid(cs, SIDE, SIDE, 21, 21, title, note, seq_legend(vmax, unit))

    P.append('<h2>What the board was measured from</h2>')
    P.append('<div class="row">'
             + board(lambda c: tab.wcell.get(c, 0.0), 'Coverage',
                     'Weighted configurations that placed anything here. Blank cells '
                     'were never reached; they hold pure backoff.', 'configurations')
             + board(tab.ess, 'Effective sample size',
                     'Sum of weights squared over sum of weights. Low means one '
                     'configuration is doing the talking.', 'boards')
             + board(lambda c: max((w[(p, c)] for p in tab.pieces if (p, c) in w),
                                   default=0.0),
                     'Decisiveness',
                     'The strongest preference any piece has for this cell. High means '
                     'the table has an opinion here.', 'nats')
             + '</div>')

    # --- border sides -----------------------------------------------------
    names = ['top', 'right', 'bottom', 'left']
    blocks = []
    for s in range(4):
        bp = [p for p in tab.pieces if cell_kind_of_piece(tab, p) == 1
              and piece_side(tab, p) == s]
        bc = [c for c in tab.free_cells if cell_kind(c) == 1 and cell_side(c) == s]
        if not bp or not bc:
            continue
        cs = [(k, i, div_color(w.get((p, c), 0.0), ws),
               f'piece {p} - cell {c} - {w.get((p,c),0.0):+.2f} nats')
              for i, p in enumerate(bp) for k, c in enumerate(bc)]
        blocks.append(grid(cs, len(bc), len(bp), 16, 16, f'{names[s]} side',
                           f'{len(bp)} edge pieces against the {len(bc)} cells of this '
                           f'side.', div_legend(ws), flip_y=False))
    if blocks:
        P.append('<h2>The border, side by side</h2>')
        P.append('<p class="note">A rotations row fixes which side each edge piece '
                 'belongs to, so the 56&times;56 border block is really four '
                 'independent assignment problems. These weights are what the '
                 'data-driven bottom-row and left-column ranking adds to the '
                 'library&rsquo;s fan-out measure &mdash; and they are only valid for '
                 'the rotations row they were learned on.</p>')
        P.append('<div class="row">' + ''.join(blocks) + '</div>')

    # --- distribution -----------------------------------------------------
    vals = sorted(w.values())
    if vals:
        lo, hi, nb = vals[0], vals[-1], 44
        span = (hi - lo) or 1.0
        hist = [0] * nb
        for v in vals:
            hist[min(nb - 1, int((v - lo) / span * nb))] += 1
        hmax = max(hist) or 1
        bw, bh = 24, 150
        bars = []
        for i, n in enumerate(hist):
            h = max(1, round(n / hmax * bh))
            c0 = lo + (i + .5) * span / nb
            bars.append(f'<rect x="{i*bw}" y="{bh-h}" width="{bw-2}" height="{h}" rx="2" '
                        f'fill="{div_color(c0, ws)}" tabindex="0" data-t="'
                        f'{c0:+.2f} nats - {n:,} piece-cell pairs"/>')
        P.append('<h2>How opinionated the table is</h2>')
        P.append('<figure class="panel"><figcaption><h3>Distribution of log weights</h3>'
                 '<p class="note">Every free piece-cell pair. Mass piled at zero means '
                 'alpha is shrinking the table to its prior; long tails mean it is '
                 'committing. This is the shape <code>--freq_alpha</code> moves.</p>'
                 '</figcaption><div class="scroll">'
                 f'<svg viewBox="0 0 {nb*bw} {bh}" width="{nb*bw}" height="{bh}">'
                 + ''.join(bars) + '</svg></div>'
                 f'<div class="legend"><span>{lo:+.1f}</span>'
                 f'<span style="margin-left:auto">{hi:+.1f} nats</span></div></figure>')

    # --- per-piece explorer ----------------------------------------------
    bypiece = defaultdict(list)
    for (q, c), v in w.items():
        bypiece[q].append([c, round(v, 3)])
    pmap = {str(q): sorted(v) for q, v in bypiece.items()}
    P.append('<h2>One piece at a time</h2>')
    P.append('<figure class="panel"><figcaption><h3 id="plabel">piece</h3>'
             '<p class="note">The whole board as one piece sees it. Blue where the '
             'table wants it, red where it does not.</p></figcaption>'
             '<div class="ctl"><label for="pnum">piece id</label>'
             f'<input type="number" id="pnum" min="0" max="255" value="{ip[0] if ip else 0}">'
             '</div><div class="scroll"><svg viewBox="0 0 352 352" width="352" '
             'height="352"><g id="pmap"></g></svg></div>'
             + div_legend(ws) + '</figure>')

    # --- table view -------------------------------------------------------
    top = sorted(w.items(), key=lambda kv: -kv[1])[:40]
    trs = ''.join(f'<tr><td>piece {p}</td><td>{c}</td><td>{c//SIDE}</td><td>{c%SIDE}</td>'
                  f'<td>{v:+.3f}</td><td>{tab.wcell.get(c,0):,.1f}</td></tr>'
                  for (p, c), v in top)
    P.append('<details><summary>Table view &mdash; the 40 strongest '
             'piece-cell preferences</summary><table><thead><tr><th>piece</th>'
             '<th>cell</th><th>row</th><th>col</th><th>nats</th><th>coverage</th>'
             f'</tr></thead><tbody>{trs}</tbody></table></details>')

    css = (CSS.replace('%LIGHT%', ramp_vars(RAMP_CSS_LIGHT))
              .replace('%DARK%', ramp_vars(RAMP_CSS_DARK)))
    meta = '<br>'.join(f'{html.escape(k)} = {html.escape(str(v))}'
                       for k, v in sorted(tab.meta.items()) if k != 'lift')
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Frequency table</title><style>{css}</style></head><body><div class="wrap">
<div style="display:flex;align-items:start;gap:12px">
<div style="flex:1"><h1>Data-driven frequency table</h1>
<p class="sub">{html.escape(os.path.basename(src))} &mdash; where each piece was
measured to sit, in the canonical clue frame.</p></div>
<button id="theme">theme</button></div>
{''.join(P)}
<details><summary>Provenance &mdash; what this table was learned from</summary>
<p class="meta">{meta}</p></details>
</div><div id="tip"></div>
<script>window.PIECEMAP={json.dumps(pmap)};window.PSCALE={ws:.6f};
window.RAMPS={json.dumps({"pos": DIV_POS, "neg": DIV_NEG})};</script>
<script>{JS}</script></body></html>"""


def text_summary(tab, w, alpha):
    lift, lift_n = tab.meta.get('lift', (0.0, 0))
    ess = [tab.ess(c) for c in tab.covered]
    print(f"alpha          {alpha:g}")
    print(f"held-out lift  {lift:+.4f} nats/cell over {lift_n:,} boards"
          + ("   <-- NO SIGNAL" if lift <= 0 else ""))
    print(f"free cells     {len(tab.free_cells)}  ({len(tab.covered)} measured, "
          f"{sum(1 for e in ess if e < 30)} with sample size < 30)")
    print(f"mean ESS       {sum(ess)/len(ess) if ess else 0:,.1f}")
    print("configs/pass   " + "  ".join(f"p{j}={n:,}" for j, n in sorted(tab.passes.items())))
    vals = sorted(w.values())
    if vals:
        print(f"weights        {vals[0]:+.2f} .. {vals[-1]:+.2f} nats over "
              f"{len(vals):,} piece-cell pairs")
    pr = piece_row_matrix(tab, w)
    print("\nstrongest piece-cell preferences")
    for (p, c), v in sorted(w.items(), key=lambda kv: -kv[1])[:12]:
        print(f"  piece {p:3d}  cell {c:3d} (row {c//SIDE:2d}, col {c%SIDE:2d})  {v:+.3f}")
    print("\nrow preference of the ten most opinionated pieces")
    ip = [p for p in tab.pieces if cell_kind_of_piece(tab, p) == 0]
    for p in sorted(ip, key=lambda q: -max(pr[(q, r)] for r in range(1, SIDE - 1)))[:10]:
        best = max(range(1, SIDE - 1), key=lambda r: pr[(p, r)])
        print(f"  piece {p:3d}  prefers row {best:2d}  ({pr[(p,best)]:+.2f} nats)")


def check_against_c(tab, w, dump_path):
    """Compare our weights against the ones the C wrote. Produce the dump with

        E555_FREQ_DUMP=/tmp/fw.txt bin/E555_beamer_datadriven SEED ROT \\
            --table TABLE --freq_alpha A ...

    and pass the same --alpha here, or the two are not computing the same thing."""
    got = {}
    with open(dump_path) as f:
        for line in f:
            if line.startswith('fw '):
                _, p_, c_, v_ = line.split()
                got[(int(p_), int(c_))] = float(v_)
    if not got:
        sys.exit(f"--check: {dump_path} holds no 'fw' lines")
    miss = [k for k in got if k not in w]
    worst, at = 0.0, None
    for k, v in got.items():
        d = abs(w.get(k, 0.0) - v)
        if d > worst:
            worst, at = d, k
    print(f"--check: {len(got):,} weights compared against {dump_path}")
    if miss:
        print(f"         {len(miss):,} the C has and this does not, e.g. {miss[0]}")
    print(f"         largest disagreement {worst:.3e} nats"
          + (f" at piece {at[0]} cell {at[1]}" if at else ""))
    ok = worst < 1e-3 and not miss
    print("         " + ("AGREE" if ok else "DISAGREE -- one of the two is wrong"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('table')
    ap.add_argument('--alpha', type=float, default=20.0,
                    help='shrinkage toward the piece-by-row prior (default 20)')
    ap.add_argument('--out', default=None,
                    help='HTML output path (default: TABLE with .html)')
    ap.add_argument('--text', action='store_true', help='terminal summary instead')
    ap.add_argument('--check', metavar='DUMP',
                    help='compare against weights the C wrote via E555_FREQ_DUMP')
    a = ap.parse_args()
    if a.alpha <= 0:
        sys.exit('--alpha must be > 0: it is what keeps every log finite')

    tab = Table(a.table)
    w = build(tab, a.alpha)
    if a.check:
        sys.exit(check_against_c(tab, w, a.check))
    if a.text:
        text_summary(tab, w, a.alpha)
        return
    out = a.out or (os.path.splitext(a.table)[0] + '.html')
    with open(out, 'w') as f:
        f.write(render(tab, w, a.alpha, a.table))
    print(f"[out] {out}  ({os.path.getsize(out)/1024:.0f} KB) -- open it in a browser")


if __name__ == '__main__':
    main()
