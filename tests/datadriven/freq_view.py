#!/usr/bin/env python3
"""freq_view.py -- inspect an E555 data-driven table.

    python3 freq_view.py TABLE [--out FILE.html] [--text]
    python3 freq_view.py TABLE --check DUMP

Inner pieces are pooled at the beamer's A/B/C resolution (cols 1-5, 6-10,
11-14) or kept per cell, smoothed by the Krichevsky-Trofimov half-count,
Sinkhorn-balanced, and conditioned on the rows still unfilled. Bottom-row and
left-column ranking uses the exact piece-by-cell location table.

The file stores raw counts. This script re-implements the C estimator
independently; --check compares the segment (fwseg), cell (fwcell) and border
location (fwb) weights against an E555_FREQ_DUMP file.
"""
import argparse
import html
import json
import math
import os
import sys
from collections import defaultdict

SIDE, NP, NUM_SEG = 16, 256, 3
KT_HALF = 0.5
SINK_ITERS, SINK_TOL = 400, 1e-13
SEG_NAME = ('A', 'B', 'C')


class Table:
    def __init__(self, path):
        self.meta = {}
        self.cnt, self.wcell, self.wsq = {}, {}, {}
        self.rowcnt, self.segcnt = {}, {}
        self.passes, self.pkind, self.pside = {}, {}, {}
        self.free_cells = []
        with open(path) as f:
            for line in f:
                if line.startswith('#') or not line.strip():
                    continue
                a = line.split()
                tag = a[0]
                if tag == 'cnt':
                    self.cnt[(int(a[1]), int(a[2]))] = float(a[3])
                elif tag == 'wcell':
                    self.wcell[int(a[1])] = float(a[2])
                    self.wsq[int(a[1])] = float(a[3])
                elif tag == 'rowcnt':
                    self.rowcnt[(int(a[1]), int(a[2]))] = float(a[3])
                elif tag == 'segcnt':
                    self.segcnt[(int(a[1]), int(a[2]), int(a[3]))] = float(a[4])
                elif tag == 'pass':
                    self.passes[int(a[1])] = int(a[3])
                elif tag == 'freep':
                    self.pkind[int(a[1])] = int(a[2])
                    self.pside[int(a[1])] = int(a[3])
                elif tag == 'freec':
                    self.free_cells.append(int(a[1]))
                elif tag == 'lift':
                    self.meta['lift'] = (float(a[1]), int(a[2]))
                else:
                    self.meta[tag] = ' '.join(a[1:])
        if 'version' not in self.meta:
            sys.exit(f'{path}: not an E555 data-driven table')
        self.version = int(str(self.meta['version']).split()[0])
        if self.version != 4:
            sys.exit(f'{path}: table version {self.version}; this viewer reads 4')
        if self.meta.get('lift_kind') != 'segment_kt_prequential':
            sys.exit(f"{path}: lift_kind is {self.meta.get('lift_kind', '(missing)')!r}; "
                     "expected 'segment_kt_prequential'")
        self.pieces = sorted(self.pkind)
        self.free_cells = sorted(self.free_cells)
        self.free_set = set(self.free_cells)
        if not self.pieces or not self.free_cells:
            sys.exit(f'{path}: no freep/freec lines')
        self.covered = [c for c in self.free_cells if self.wcell.get(c, 0.0) > 0.0]
        self._derive_and_validate()

    def _derive_and_validate(self):
        row = defaultdict(float)
        seg = defaultdict(float)
        for (piece, cell), value in self.cnt.items():
            if piece not in self.pkind or cell not in self.free_set:
                continue
            r = cell // SIDE
            row[(piece, r)] += value
            z = segment_of_cell(cell)
            if z is not None and self.pkind.get(piece) == 0:
                seg[(piece, z[0], z[1])] += value
        check_aggregate('rowcnt', self.rowcnt, row)
        check_aggregate('segcnt', self.segcnt, seg)

    def ess(self, cell):
        w, q = self.wcell.get(cell, 0.0), self.wsq.get(cell, 0.0)
        return w * w / q if q > 0.0 else 0.0


def check_aggregate(name, got, want):
    if not got:
        sys.exit(f'table declares {name} support but contains no {name} lines')
    for key in set(got) | set(want):
        a, b = got.get(key, 0.0), want.get(key, 0.0)
        if abs(a - b) > 1e-7 * (1.0 + abs(b)):
            sys.exit(f'{name} disagrees with cnt at {key}: {a} versus {b}')


def cell_kind(cell):
    r, c = divmod(cell, SIDE)
    return (r == 0) + (r == SIDE - 1) + (c == 0) + (c == SIDE - 1)


def segment_of_cell(cell):
    r, c = divmod(cell, SIDE)
    if not (1 <= r <= 14 and 1 <= c <= 14):
        return None
    return r, 0 if c <= 5 else 1 if c <= 10 else 2


def cell_side(cell):
    r, c = divmod(cell, SIDE)
    if r == SIDE - 1:
        return 0
    if c == SIDE - 1:
        return 1
    if r == 0:
        return 2
    if c == 0:
        return 3
    return -1


def sinkhorn(m, n):
    ru, cu = [1.0] * n, [1.0] * n
    for _ in range(SINK_ITERS):
        worst = 0.0
        for i in range(n):
            base = i * n
            total = sum(m[base + k] * cu[k] for k in range(n)) * ru[i]
            if total > 0.0:
                ru[i] /= total
            worst = max(worst, abs(total - 1.0))
        for k in range(n):
            total = sum(m[i * n + k] * ru[i] for i in range(n)) * cu[k]
            if total > 0.0:
                cu[k] /= total
            worst = max(worst, abs(total - 1.0))
        if worst < SINK_TOL:
            break
    for i in range(n):
        for k in range(n):
            m[i * n + k] *= ru[i] * cu[k]
    return m


def build(tab):
    """Return (segment beam weights, cell beam weights, exact location weights)."""
    active, cellw, location = {}, {}, {}
    ip = [p for p in tab.pieces if tab.pkind[p] == 0]
    ic = [c for c in tab.free_cells if cell_kind(c) == 0]
    if len(ip) != len(ic):
        sys.exit(f'interior block is {len(ip)} pieces x {len(ic)} cells')
    n = len(ip)
    cap = defaultdict(int)
    for cell in ic:
        cap[segment_of_cell(cell)] += 1
    seg_total = defaultdict(float)
    for piece in ip:
        for r in range(1, 15):
            for sg in range(NUM_SEG):
                seg_total[(r, sg)] += tab.segcnt.get((piece, r, sg), 0.0)

    mloc = [0.0] * (n * n)
    mseg = [0.0] * (n * n)
    for i, piece in enumerate(ip):
        for k, cell in enumerate(ic):
            z = segment_of_cell(cell)
            c = cap[z]
            mloc[i * n + k] = ((tab.cnt.get((piece, cell), 0.0) + KT_HALF) /
                               (tab.wcell.get(cell, 0.0) + KT_HALF * n))
            mean_n = tab.segcnt.get((piece, z[0], z[1]), 0.0) / c
            mean_w = seg_total[z] / c
            mseg[i * n + k] = (mean_n + KT_HALF) / (mean_w + KT_HALF * n)
    sinkhorn(mloc, n)
    sinkhorn(mseg, n)

    row_cells = defaultdict(list)
    for k, cell in enumerate(ic):
        row_cells[cell // SIDE].append(k)
    rem_slots = {}
    total_slots = 0
    for r in range(14, 0, -1):
        total_slots += len(row_cells[r])
        rem_slots[r] = total_slots
    for i, piece in enumerate(ip):
        rem, reml = {}, {}
        mass = massl = 0.0
        for r in range(14, 0, -1):
            mass += sum(mseg[i * n + k] for k in row_cells[r])
            massl += sum(mloc[i * n + k] for k in row_cells[r])
            rem[r], reml[r] = mass, massl
        for k, cell in enumerate(ic):
            r = cell // SIDE
            location[(piece, cell)] = math.log(n * mloc[i * n + k])
            active[(piece, cell)] = math.log(mseg[i * n + k] * rem_slots[r] / rem[r])
            cellw[(piece, cell)] = math.log(mloc[i * n + k] * rem_slots[r] / reml[r])

    for side in range(4):
        bp = [p for p in tab.pieces if tab.pkind[p] == 1 and tab.pside[p] == side]
        bc = [c for c in tab.free_cells if cell_kind(c) == 1 and cell_side(c) == side]
        if not bp or len(bp) != len(bc):
            continue
        n1 = len(bp)
        mat = [0.0] * (n1 * n1)
        for i, piece in enumerate(bp):
            for k, cell in enumerate(bc):
                mat[i * n1 + k] = ((tab.cnt.get((piece, cell), 0.0) + KT_HALF) /
                                    (tab.wcell.get(cell, 0.0) + KT_HALF * n1))
        sinkhorn(mat, n1)
        rows = defaultdict(list)
        for k, cell in enumerate(bc):
            rows[cell // SIDE].append(k)
        rem_slots_side, total = {}, 0
        if side in (1, 3):
            for r in range(14, 0, -1):
                total += len(rows[r])
                rem_slots_side[r] = total
        for i, piece in enumerate(bp):
            rem = {}
            if side in (1, 3):
                mass = 0.0
                for r in range(14, 0, -1):
                    mass += sum(mat[i * n1 + k] for k in rows[r])
                    rem[r] = mass
            for k, cell in enumerate(bc):
                location[(piece, cell)] = math.log(n1 * mat[i * n1 + k])
                if side in (1, 3):
                    r = cell // SIDE
                    active[(piece, cell)] = math.log(
                        mat[i * n1 + k] * rem_slots_side[r] / rem[r])
                else:
                    active[(piece, cell)] = 0.0
                cellw[(piece, cell)] = active[(piece, cell)]
    return active, cellw, location


# ---------------------------------------------------------------- rendering
DIV_POS = ['#dce9fb', '#b7d3f6', '#86b6ef', '#5598e7', '#2a78d6', '#184f95']
DIV_NEG = ['#fadedd', '#f4bfbe', '#eb9a99', '#e07170', '#cf4544', '#94302f']
SEQ = ['#e8f0fd', '#cde2fb', '#9ec5f4', '#6da7ec', '#3987e5', '#256abf', '#0d366b']


def robust_scale(vals, q=.95):
    a = sorted(abs(v) for v in vals)
    return (a[min(len(a)-1, int(q * len(a)))] if a else 1.0) or 1.0


def div_color(v, scale):
    if abs(v) < 1e-9:
        return '#f0efec'
    ramp = DIV_POS if v > 0 else DIV_NEG
    return ramp[min(len(ramp)-1, int(abs(v) / scale * len(ramp)))]


def seq_color(v, vmax):
    return SEQ[min(len(SEQ)-1, int(v / vmax * len(SEQ)))] if vmax > 0 else '#f0efec'


def tile(value, label, sub=''):
    return f'<div class="tile"><b>{value}</b><span>{html.escape(label)}</span><em>{html.escape(sub)}</em></div>'


def grid(cells, cols, rows, cw, ch, title, note, flip_y=True):
    width, height = cols * cw, rows * ch
    out = [f'<figure class="panel"><figcaption><h3>{html.escape(title)}</h3>',
           f'<p class="note">{note}</p></figcaption><div class="scroll">',
           f'<svg viewBox="0 0 {width} {height}" width="{width}" height="{height}">']
    for col, row, fill, tip in cells:
        y = (rows - 1 - row) * ch if flip_y else row * ch
        out.append(f'<rect x="{col*cw}" y="{y}" width="{cw-.6:.1f}" height="{ch-.6:.1f}" '
                   f'rx="1" fill="{fill}" tabindex="0" data-t="{html.escape(tip)}"/>')
    out.append('</svg></div></figure>')
    return ''.join(out)


CSS = """
:root{color-scheme:light dark;--plane:#f9f9f7;--surface:#fff;--ink:#111;--muted:#6c6a64;--rule:#ddd}
@media(prefers-color-scheme:dark){:root{--plane:#101010;--surface:#1b1b1a;--ink:#eee;--muted:#aaa;--rule:#333}}
*{box-sizing:border-box}body{margin:0;padding:24px 16px 70px;background:var(--plane);color:var(--ink);font:14px/1.5 system-ui,sans-serif;font-variant-numeric:tabular-nums}.wrap{max-width:1200px;margin:auto}h1{font-size:24px;margin:0}.sub,.note{color:var(--muted)}h2{margin:34px 0 12px;font-size:15px;text-transform:uppercase;letter-spacing:.06em}.tiles{display:flex;flex-wrap:wrap;gap:10px;margin:18px 0}.tile,.panel{background:var(--surface);border:1px solid var(--rule);border-radius:11px}.tile{padding:10px 14px;min-width:150px}.tile b{display:block;font-size:24px}.tile span,.tile em{display:block;color:var(--muted);font-size:12px}.tile em{font-style:normal}.panel{padding:14px;margin:0 0 15px}.panel h3{margin:0}.note{margin:3px 0 10px;max-width:82ch;font-size:12.5px}.scroll{overflow-x:auto}.row{display:flex;flex-wrap:wrap;gap:15px}.row>.panel{flex:1 1 320px}svg rect{shape-rendering:crispEdges}svg rect:hover,svg rect:focus{stroke:var(--ink);stroke-width:1.2;outline:none}#tip{position:fixed;pointer-events:none;background:var(--ink);color:var(--plane);padding:5px 8px;border-radius:6px;opacity:0;z-index:3;font-size:12px}table{border-collapse:collapse;width:100%;font-size:12.5px}th,td{text-align:right;padding:4px 8px;border-bottom:1px solid var(--rule)}th:first-child,td:first-child{text-align:left}details{margin-top:14px}.meta{font:12px/1.65 ui-monospace,monospace;color:var(--muted)}
"""

JS = """
const tip=document.getElementById('tip');
addEventListener('mouseover',e=>{const t=e.target.dataset.t;if(!t)return;tip.textContent=t;tip.style.opacity=1;const r=e.target.getBoundingClientRect();tip.style.left=Math.min(innerWidth-310,r.left)+'px';tip.style.top=Math.max(4,r.top-29)+'px'});
addEventListener('mouseout',()=>tip.style.opacity=0);
"""


def render(tab, active, location, src):
    ip = [p for p in tab.pieces if tab.pkind[p] == 0]
    av = [v for (p, _), v in active.items() if tab.pkind.get(p) == 0]
    scale = robust_scale(av)
    border_scale = robust_scale(v for (p, _), v in location.items() if tab.pkind.get(p) == 1)
    lift, lift_n = tab.meta.get('lift', (0.0, 0))
    lift_label = 'prequential segment lift'
    lift_note = f'{lift_n:,} boards'
    ess = [tab.ess(c) for c in tab.covered]
    P = ['<div class="tiles">',
         tile(f'{lift:+.3f}', lift_label, lift_note),
         tile(f'{len(tab.covered)} / {len(tab.free_cells)}', 'free cells measured'),
         tile(f'{sum(ess)/len(ess):,.1f}' if ess else '0', 'mean cell ESS', 'configuration units'),
         tile('KT 1/2', 'smoothing', 'parameter-free'),
         tile('A / B / C', 'beam resolution', '5-5-4 inner pieces'),
         tile('exact cell', 'border resolution', 'bottom + left ranking'), '</div>']

    # Piece x segment heatmap, sorted by preferred segment centroid.
    seg_weight = {}
    for p in ip:
        for r in range(1, 15):
            for sg in range(3):
                vals = [active[(p, cell)] for cell in tab.free_cells
                        if tab.pkind.get(p) == 0 and segment_of_cell(cell) == (r, sg)
                        and (p, cell) in active]
                seg_weight[(p, r, sg)] = sum(vals) / len(vals) if vals else 0.0
    def pref_key(p):
        best = max(((seg_weight[p, r, sg], r, sg) for r in range(1, 15) for sg in range(3)),
                   key=lambda x: x[0])
        return best[1], best[2], -best[0]
    order = sorted(ip, key=pref_key)
    cells = []
    for y, p in enumerate(order):
        for r in range(1, 15):
            for sg in range(3):
                v = seg_weight[p, r, sg]
                x = (r - 1) * 3 + sg
                cells.append((x, y, div_color(v, scale),
                              f'piece {p}; row {r} segment {SEG_NAME[sg]}; {v:+.3f} nats'))
    P.append('<h2>5-5-5 preservation map</h2>')
    P.append(grid(cells, 42, len(order), 15, 5, 'Piece by row segment',
                  'Columns are row 1 A/B/C through row 14 A/B/C. A strong blue top segment makes lower placement red because the score conditions on all still-unfilled rows.'))

    # Board measurement maps.
    def board(fn, title, note):
        vals = {c: fn(c) for c in tab.free_cells}
        vmax = max(vals.values(), default=0.0)
        cells = [(c % 16, c // 16, seq_color(vals[c], vmax),
                  f'cell {c} row {c//16} col {c%16}: {vals[c]:,.1f}') for c in tab.free_cells]
        return grid(cells, 16, 16, 21, 21, title, note)
    P.append('<h2>Evidence map</h2><div class="row">')
    P.append(board(lambda c: tab.wcell.get(c, 0.0), 'Coverage', 'Weighted configurations contributing to each cell.'))
    P.append(board(tab.ess, 'Effective sample size', 'Configuration-level ESS; correlated bottoms make it an upper bound.'))
    P.append('</div>')

    # Border exact location panels.
    names = ('top', 'right', 'bottom', 'left')
    blocks = []
    for side in range(4):
        bp = [p for p in tab.pieces if tab.pkind[p] == 1 and tab.pside[p] == side]
        bc = [c for c in tab.free_cells if cell_kind(c) == 1 and cell_side(c) == side]
        if not bp or not bc:
            continue
        cs = [(k, i, div_color(location.get((p, c), 0.0), border_scale),
               f'piece {p}; cell {c}; {location.get((p,c),0.0):+.3f} nats')
              for i, p in enumerate(bp) for k, c in enumerate(bc)]
        blocks.append(grid(cs, len(bc), len(bp), 16, 16, f'{names[side]} side',
                           'Exact location weights used for border ranking.', flip_y=False))
    P.append('<h2>Exact border-location model</h2><div class="row">' + ''.join(blocks) + '</div>')

    # Most valuable top reservations.
    rows = []
    for p in ip:
        top = max((seg_weight[p, r, sg], r, sg) for r in range(12, 15) for sg in range(3))
        low = min(seg_weight[p, 1, sg] for sg in range(3))
        rows.append((top[0], p, top[1], top[2], low))
    trs = ''.join(f'<tr><td>piece {p}</td><td>{r}{SEG_NAME[sg]}</td><td>{v:+.3f}</td><td>{low:+.3f}</td></tr>'
                  for v, p, r, sg, low in sorted(rows, reverse=True)[:30])
    P.append('<h2>Pieces most worth preserving for the top</h2>')
    P.append('<figure class="panel"><table><thead><tr><th>piece</th><th>best top segment</th><th>top score</th><th>worst row-1 score</th></tr></thead><tbody>' + trs + '</tbody></table></figure>')

    # Exact interior cells remain in the table as a diagnostic even though the
    # beam deliberately pools them to A/B/C. This shows whether a segment signal
    # is broad or driven by one clue/corner-adjacent cell.
    exact = [((p, c), v) for (p, c), v in location.items()
             if tab.pkind.get(p) == 0 and cell_kind(c) == 0]
    trs = ''.join(
        f'<tr><td>piece {p}</td><td>{c}</td><td>{c//SIDE}</td><td>{c%SIDE}</td>'
        f'<td>{SEG_NAME[segment_of_cell(c)[1]]}</td><td>{v:+.3f}</td></tr>'
        for (p, c), v in sorted(exact, key=lambda kv: -kv[1])[:30])
    P.append('<h2>Exact-cell diagnostic</h2>')
    P.append('<figure class="panel"><p class="note">These are retained for inspection, '
             'not used by the interior beam. A sharp entry still contributes its full raw '
             'count to the corresponding A/B/C segment.</p><table><thead><tr><th>piece</th>'
             '<th>cell</th><th>row</th><th>col</th><th>segment</th><th>location nats</th>'
             '</tr></thead><tbody>' + trs + '</tbody></table></figure>')

    meta = '<br>'.join(f'{html.escape(k)} = {html.escape(str(v))}' for k, v in sorted(tab.meta.items()) if k != 'lift')
    return f'''<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>E555 frequency table</title><style>{CSS}</style></head><body><div class="wrap"><h1>E555 data-driven table</h1><p class="sub">{html.escape(os.path.basename(src))} — fixed 5-5-5 segment-preservation beam model; exact-location border model.</p>{''.join(P)}<details><summary>Provenance</summary><p class="meta">{meta}</p></details></div><div id="tip"></div><script>{JS}</script></body></html>'''


def text_summary(tab, active, location):
    lift, lift_n = tab.meta.get('lift', (0.0, 0))
    ess = [tab.ess(c) for c in tab.covered]
    print('beam model     5-5-5 segment preservation')
    print('border model   exact location')
    print('smoothing      Krichevsky-Trofimov 1/2 count')
    print(f'segment lift   {lift:+.4f} nats/cell over {lift_n:,} boards')
    print(f'free cells     {len(tab.free_cells)} ({len(tab.covered)} measured)')
    print(f'mean ESS       {sum(ess)/len(ess) if ess else 0:,.1f} configurations/cell')
    print('configs/pass   ' + '  '.join(f'p{j}={n:,}' for j, n in sorted(tab.passes.items())))
    vals = [v for (p, _), v in active.items() if tab.pkind.get(p) == 0]
    print(f'beam weights   {min(vals):+.2f} .. {max(vals):+.2f} nats' if vals else 'beam weights   none')
    bvals = [v for (p, _), v in location.items() if tab.pkind.get(p) == 1]
    print(f'border weights {min(bvals):+.2f} .. {max(bvals):+.2f} nats' if bvals else 'border weights none')

    ip = [p for p in tab.pieces if tab.pkind[p] == 0]
    def segv(p, r, sg):
        a = [active[p, c] for c in tab.free_cells if segment_of_cell(c) == (r, sg) and (p, c) in active]
        return sum(a) / len(a) if a else 0.0
    top = []
    for p in ip:
        best = max((segv(p, r, sg), r, sg) for r in range(12, 15) for sg in range(3))
        top.append((best[0], p, best[1], best[2], min(segv(p, 1, sg) for sg in range(3))))
    print('\nstrongest top reservations')
    for v, p, r, sg, low in sorted(top, reverse=True)[:15]:
        print(f'  piece {p:3d}  top {r:2d}{SEG_NAME[sg]} {v:+.3f}   worst row 1 {low:+.3f}')
    exact = [((p, c), v) for (p, c), v in location.items()
             if tab.pkind.get(p) == 0 and cell_kind(c) == 0]
    print('\nstrongest exact-cell preferences (diagnostic; beam pools to A/B/C)')
    for (p, c), v in sorted(exact, key=lambda kv: -kv[1])[:12]:
        r, col = divmod(c, SIDE)
        sg = segment_of_cell(c)[1]
        print(f'  piece {p:3d}  cell {c:3d} (row {r:2d}, col {col:2d}, '
              f'{SEG_NAME[sg]}) {v:+.3f}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('table')
    ap.add_argument('--out', help='HTML output path (default TABLE.html)')
    ap.add_argument('--text', action='store_true')
    ap.add_argument('--check', metavar='DUMP')
    a = ap.parse_args()
    tab = Table(a.table)
    active, cellw, location = build(tab)
    if a.check:
        got = {'fw': {}, 'fwseg': {}, 'fwcell': {}, 'fwb': {}}
        header = ''
        with open(a.check) as f:
            for line in f:
                if line.startswith('# beam_model '):
                    header = line.strip()
                    continue
                w = line.split()
                if len(w) == 4 and w[0] in got:
                    got[w[0]][int(w[1]), int(w[2])] = float(w[3])
        if not got['fw']:
            sys.exit(f'{a.check}: no fw lines')

        def cmp(label, observed, expected):
            expected = {k: v for k, v in expected.items() if v != 0.0}
            worst, at = 0.0, None
            for k, v in observed.items():
                d = abs(expected.get(k, 0.0) - v)
                if d > worst:
                    worst, at = d, k
            miss_o = [k for k in observed if k not in expected]
            miss_e = [k for k in expected if k not in observed]
            print(f'{label}: {len(observed):,} weights; max |delta|={worst:.3e}'
                  + (f' at {at}' if at else ''))
            if miss_o: print(f'  {len(miss_o)} C-only keys; first {miss_o[0]}')
            if miss_e: print(f'  {len(miss_e)} Python-only keys; first {miss_e[0]}')
            return worst < 1e-3 and not miss_o and not miss_e

        ok = cmp('segment', got['fwseg'], active)
        ok = cmp('cell', got['fwcell'], cellw) and ok
        ok = cmp('border', got['fwb'],
                 {k: v for k, v in location.items() if tab.pkind.get(k[0]) == 1}) and ok
        model = 'fwcell' if 'cell-preservation' in header else 'fwseg'
        if got['fw'] != got[model]:
            print(f'active fw differs from {model}')
            ok = False
        print('AGREE' if ok else 'DISAGREE')
        raise SystemExit(0 if ok else 1)
    if a.text:
        text_summary(tab, active, location)
        return
    out = a.out or os.path.splitext(a.table)[0] + '.html'
    with open(out, 'w') as f:
        f.write(render(tab, active, location, a.table))
    print(f'[out] {out} ({os.path.getsize(out)/1024:.0f} KB)')


if __name__ == '__main__':
    main()
