#!/usr/bin/env python3
"""
E555_distiller.py -- pick the few boards worth Stage C time, and write the
commands to attack them.

WHY

    Above ~450 the score stops discriminating: every board sits within a couple
    of breaks of the same entropy floor, so ranking by what a board IS spreads
    CP-SAT time evenly over thousands of boards, most of them finished.

    This tool ranks by what a board could BECOME. For each board it finds the
    cheapest repair neighbourhood covering that board's own breaks, measures the
    freedom left inside it, and writes the Stage C command to try -- masks and a
    runnable shell script, one block per kept board. There are no quality or
    speed knobs: everything that would be one is either derived from the corpus
    at runtime or a constant in this file.

THE MEASURES  (a "break" is an internal junction whose two cells disagree; a
               junction touching an unplaced cell counts as broken, exactly as
               everywhere else in Stage C)

    win       the cheapest repair window covering every break on THIS board: a
              T/B/L/R band at its minimal covering depth, or `hull`, a 1-cell
              padded outline of the break cells. Breaks that hug one border get
              a cheap band; sprawling ones get a hull, which is exactly when
              --holes earns its place. This decides which Stage C command gets
              written.

    cells     free cells in that window.

    J         junctions with at least one endpoint in it. THE cost measure: what
              the re-solve has to satisfy, and what `win` is chosen to minimise.
              Lower is better.

    closure   the colour ledger: how far the mix of interior colours still in
              the POOL has drifted from flat, weighted by how many pairings are
              left to make. Higher is better. Every other measure here reads the
              board's shape, which a corpus grown to one stop row makes constant
              -- closure reads what is left to place, so it still separates them.
              It is the beamer's own --lambda_J objective (closure_raw() in
              src/B_beam/E555_beamer.c); 0 on a complete board, which has no pool.

    fixers    piece-orientations that could sit on a BREAK cell and match
              strictly more of its junctions than the incumbent does: literal
              single-swap escape routes. Higher is better. It disagrees with the
              geometry -- measured 2..24 over seven boards that all score 463,
              the worst window carrying the most escape routes -- so it is real
              independent information.

    dive_min  the best break count randomized greedy dives reach over the window
              (E555_backtracker --break_mode stuck, ~12k dives). Lower is
              better. It lands far above a CP-SAT incumbent -- 26..30 against 17
              on data/best_463.csv -- so it ranks windows against each other and
              is never a bound on one, but it is the only measure here that
              samples the repair landscape rather than describing it. It is also
              the only one that moves between runs (see THE DIVE ENGINE). Blank
              without bin/E555_backtracker.

    rsum      the rank-sum; lower is better. See below.

    agree     cells on which this board matches the closest board already
              picked, 0..256 -- E555_rank.py's own unit, and its select_diverse
              is what does the picking. A property of the selection, not of the
              board, which is why `rank` is NOT sorted by `rsum`: the spread
              reaches past a slightly better board for a much more independent
              one. 0 for rank 1, which had nothing to agree with.

    score, solid, placed, border, break_rows, break_cols, span, clean_b/t/l/r,
    corner_d and clues come from E555_rank.py rather than being recomputed.

RANK-SUM  (a Borda count)

    The measures are in incompatible units and there is no data to fit weights
    with. So: sort the corpus by each measure separately, give each board its
    POSITION in each list, and add the positions. Lowest total wins.

        board     breaks    J    fixers  dive_min |  positions       sum
        0_1568      17     124     20        27   |  1 + 1 + 2 + 3  =  7
        a10_965     17     124     19        30   |  1 + 1 + 3 + 4  =  9
        a10_53      17     155     25        26   |  1 + 3 + 1 + 1  =  6   <- 1st

    Positions are unitless, so summing them needs no constants, it is robust to
    outliers, and it degrades cleanly to three measures when the dive engine is
    missing. The cost is that it discards MAGNITUDE -- slightly better and
    hugely better score the same -- acceptable only because the corpus is
    clustered tightly near the entropy floor.

    Two rules stop a tie becoming a ranking, and they matter more than they
    sound. A measure every board agrees on is skipped: a sort is stable, so a
    constant measure would otherwise hand out an arbitrary 0..N-1 by file
    position, and three of them together outweigh the measures that do vary. And
    boards tied WITHIN a measure share the first position they span, so a tie
    costs them all the same. Without both, a beamer dump ranks by file order.

STRATEGY

    Pass 1, every board: the rank.py measures, the window, and closure. Rank-sum
    on (closure, breaks, J), keep a generous shortlist. A tied measure is
    skipped rather than ranked on -- see RANK-SUM -- so closure carries this pass
    on a corpus that shares a stop row, which is what a beamer dump is.

    Pass 2, the shortlist only: mobility (bitsets) and dives, one subprocess per
    window shape rather than per board. Pass 1 has already cut the corpus to a
    few hundred, so the per-board dive budget is fixed whatever you fed in.

    Pass 3: rank-sum on (closure, breaks, J, fixers, dive_min), then
    E555_rank.py's select_diverse down to --top. The spread runs on whole-board
    agreement because what survives a Stage C run is the board OUTSIDE the
    window: comparing the pieces inside it compares exactly the ones about to be
    lifted, and on a corpus whose top row is unique per board that makes every
    pair maximally distant and the spread a no-op.

TRIAGE  (--triage)

    The pass-1 measures build a board per row and hold a record per row, and on
    a dump where every board stopped at the same row none of them says anything.
    --triage is the first cut for that case.

        python3 E555_distiller.py boards*.csv --triage --top 500 --out short.csv

    One streaming pass on closure alone -- no board built, no window, no
    mobility, no dives -- keeping a bounded heap of --top records, so the time
    is flat in the corpus and the memory is flat in --top. On 24,268 twelve-row
    partials: 4.9 s, 17 MB, and a top 500 drawing on all 23 Stage A borders in
    the file where pass 1 drew on one.

    It also drops boards repeating a Stage C job -- identical below the topmost
    full row, which the window frees anyway. That is a third of a typical beam
    dump (24,268 -> 16,502 here) and E555_clean_csv.py cannot see it, collapsing
    only the single-cell case. --no_dedup keeps them.

    Shard by file and concatenate the --out files: closure depends on the board
    alone, so a second pass over the concatenation gives the global top N.

THE DIVE ENGINE

    dive_min shells out to the project's tuned dive engine,

        bin/E555_backtracker SEED BATCH.csv OUT.csv \
            --break_mode stuck --restarts N --breaks B --holes MASK.csv

    which takes an exact fit where one exists and a minimal break where none
    does, never backtracks, and so always reaches 256 pieces in one pass at
    ~9k-18k dives/s on four cores. Five properties of that interface shape the
    code here.

    1. --holes applies ONE mask to every record, so boards batch together only
       when their windows are byte-identical -- and two boards both reading
       `hull` generally have different outlines. run_dives() groups on the mask
       itself and trades DIVE_RUNS against the number of groups
       (DIVE_CALL_BUDGET), so a corpus of all-distinct hulls cannot become
       thousands of subprocess calls.

    2. The RNG is not seedable: g_rng_master comes from the clock and the pid
       and there is no --rng_seed. Separate invocations are therefore
       independent samples, which is the only way to get more than one number
       out of a tool that reports one best per record, and why DIVE_RUNS short
       runs beat one long one. The price is that dive_min moves by a break or
       two between runs, and boards with close rsum can swap places.

    3. Records come back tagged `<input id>_<score>`, so feeding it `d7` returns
       `d7_463`; _read_dive_out takes the first "_"-separated segment.

    4. Neither obvious key maps results back: config_id repeats in real corpora
       (data/best_463.csv lines 1 and 4 share `lowB_free_Maha10_68` while
       agreeing on ZERO of 256 cells) and the row index restarts in every input
       file. The batch carries a synthetic `d<seq>` id counting across the run.

    5. --breaks is a ceiling the dive can hit, and a run whose budget is too
       small emits nothing rather than something worse. DIVE_BREAK_CEILING sits
       far above anything a real window needs.

    Without the binary the tool prints one notice and ranks on the three exact
    measures instead.

PATHS

    The tool lives in tools/, so --root defaults to its own parent's parent, the
    same convention E555_viewer.py uses to find its seed; bin/E555_backtracker
    for dives, src/C_tail/*.py for the commands --plan writes.
"""
from __future__ import annotations

import argparse
import csv
import heapq
import itertools
import math
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import E555_viewer as V                     # seed loading, row parsing, board build
import E555_rank as R                       # every geometric measure already lives here

SIDE, N_PIECES, N_EDGES = V.SIDE, V.N_PIECES, V.N_EDGES
NORTH, EAST, SOUTH, WEST = R.NORTH, R.EAST, R.SOUTH, R.WEST
ALL_JUNCTIONS = R.ALL_JUNCTIONS
FRAME_SIDES = R.FRAME_SIDES
GREY, UNPLACED = R.GREY, V.UNPLACED

# Colour 0 is the frame and 1..5 face it from inside; 6..22 are the interior
# colours, and only those pair with each other, so only those enter closure.
INNER_MIN = 6
N_INNER = 17

# Pass-1 shortlist size: enough head-room that a board with unusual mobility is
# not cut before mobility is ever measured, small enough that pass 2 is quick.
SHORTLIST_MIN = 500
SHORTLIST_MULT = 20

# Dive budget for pass 2: several independent runs rather than one long one,
# because the backtracker's RNG is not seedable and separate invocations are the
# only way to get a spread out of it (see THE DIVE ENGINE).
DIVE_RUNS = 3
DIVE_RESTARTS = 4000
DIVE_BREAK_CEILING = 400            # generous: a dive must never hit the ceiling
DIVE_CALL_BUDGET = 400              # ceiling on total subprocess calls, runs traded for groups


# -- small helpers -----------------------------------------------------------

def mask_cells(mask):
    """The cells a 256-bit window mask names, ascending.

    Windows are bitmasks and not sets: a corpus is measured before it is cut, so
    every board carries one, and a set of up to 256 small ints costs about 8 KB
    against 60 bytes for the integer."""
    return [c for c in range(N_PIECES) if mask >> c & 1]


def closure(pos, rot, seed):
    """The beamer's --lambda_J objective, read off a board. Higher is better.

    Every free interior half-edge must eventually meet another of the SAME
    colour, so what matters is the colour mix the pool still holds against the
    demands the frontier has already committed to:

        R_c  interior half-edges of colour c left in the POOL
        D_c  interior faces of placed pieces that look at an empty cell
        S_c  R_c - D_c, the free half-edges that must pair with each other

        J = 0.5 * sum_c S_c (log S_c - log Sbar) + sum_c D_c (log R_c - log Rbar)

    which is closure_raw() in src/B_beam/E555_beamer.c, centred the same way.
    Unlike every other measure here it reads the POOL rather than the board, so
    it still separates boards a fixed stop row has made identical in shape.

    Two things keep it cheap enough to run on a whole corpus: a piece's colours
    are a multiset, so the pool sum needs no rotation at all, and the demands
    are found by walking the EMPTY cells rather than the placed ones."""
    placed = {}
    supply = [0] * N_INNER
    for pid, cell in enumerate(pos):
        if cell == UNPLACED:
            for col in seed[pid]:                 # a multiset: no rotation needed
                if col >= INNER_MIN:
                    supply[col - INNER_MIN] += 1
        else:
            placed[cell] = pid

    demand = [0] * N_INNER
    for r in range(SIDE):
        for c in range(SIDE):
            if r * SIDE + c in placed:
                continue
            # The face each placed neighbour turns towards this empty cell: the
            # cell above shows its south side, the one below its north, and so on.
            for nr, nc, side in ((r + 1, c, SOUTH), (r - 1, c, NORTH),
                                 (r, c + 1, WEST), (r, c - 1, EAST)):
                if not (0 <= nr < SIDE and 0 <= nc < SIDE):
                    continue
                pid = placed.get(nr * SIDE + nc)
                if pid is None:
                    continue
                col = V.rotate_edges(seed[pid], rot[pid])[side]
                if col >= INNER_MIN:
                    demand[col - INNER_MIN] += 1

    # Centring cannot change a ranking -- both sums are fixed at a given depth --
    # but it keeps the value near 0 instead of near a thousand nats.
    free = [supply[k] - demand[k] for k in range(N_INNER)]
    sum_s, sum_r = sum(free), sum(supply)
    if sum_s <= 0 or sum_r <= 0:
        return 0.0
    log_sbar, log_rbar = math.log(sum_s / N_INNER), math.log(sum_r / N_INNER)
    conc = sum(s * (math.log(s) - log_sbar) for s in free if s > 0)
    dem = sum(demand[k] * (math.log(supply[k]) - log_rbar)
              for k in range(N_INNER) if demand[k] > 0 and supply[k] > 0)
    return 0.5 * conc + dem


def board_colors(pos, rot, seed):
    """(colors, bad_cells) for one board: cell -> (N,E,S,W), and the cells that
    touch at least one broken junction. One pass, the same rule as R.measure."""
    board = V.build_board(pos, rot)              # the strict validator
    colors = {}
    for r, row in enumerate(board):
        for c, cell in enumerate(row):
            if cell is not None:
                colors[r * SIDE + c] = V.rotate_edges(seed[cell[0]], cell[1])
    bad = set()
    for a, b, da, db in ALL_JUNCTIONS:
        ca, cb = colors.get(a), colors.get(b)
        if ca is None or cb is None or ca[da] != cb[db]:
            bad.add(a)
            bad.add(b)
    return colors, bad


# -- repair windows ----------------------------------------------------------

def band(side, depth):
    """The `depth`-deep band against one border, as a 256-bit cell mask."""
    if side == "T":
        cells = [r * SIDE + c for r in range(SIDE - depth, SIDE) for c in range(SIDE)]
    elif side == "B":
        cells = [r * SIDE + c for r in range(depth) for c in range(SIDE)]
    elif side == "R":
        cells = [r * SIDE + c for r in range(SIDE) for c in range(SIDE - depth, SIDE)]
    elif side == "L":
        cells = [r * SIDE + c for r in range(SIDE) for c in range(depth)]
    else:
        raise ValueError(f"unknown side {side!r}")
    mask = 0
    for cell in cells:
        mask |= 1 << cell
    return mask


# There are only 64 of them and none depends on the board, so a corpus pays for
# each one once instead of once per row.
BANDS = {(side, depth): band(side, depth)
         for side in "TBLR" for depth in range(1, SIDE + 1)}


def hull(bad):
    """The break cells grown by one in every direction, clipped to the board.

    A mask of exactly the break cells would force the freed pieces to permute
    among themselves; the padding is what gives the re-solve somewhere to put
    them."""
    out = 0
    for cell in bad:
        r, c = divmod(cell, SIDE)
        for rr in range(max(0, r - 1), min(SIDE, r + 2)):
            for cc in range(max(0, c - 1), min(SIDE, c + 2)):
                out |= 1 << (rr * SIDE + cc)
    return out


def window_cost(mask):
    """Junctions with at least one endpoint free. What the re-solve must satisfy."""
    return sum(1 for a, b, _, _ in ALL_JUNCTIONS if mask >> a & 1 or mask >> b & 1)


def pick_window(bad):
    """The cheapest window covering every break cell, as a dict.

    Candidates are the four bands at their minimal covering depth plus the
    padded hull; cheapest means fewest junctions. A band that would have to
    reach deeper than the whole board is not a candidate."""
    rows = [c // SIDE for c in bad]
    cols = [c % SIDE for c in bad]
    depths = {"T": SIDE - min(rows), "B": max(rows) + 1,
              "L": max(cols) + 1, "R": SIDE - min(cols)}

    cands = [(f"{side}{depth}", BANDS[(side, depth)])
             for side, depth in depths.items() if depth <= SIDE]
    cands.append(("hull", hull(bad)))

    best = None
    for name, mask in cands:
        cost, cells = window_cost(mask), mask.bit_count()
        if best is None or cost < best["J"] or (cost == best["J"] and cells < best["cells"]):
            best = dict(win=name, mask=mask, cells=cells, J=cost)
    return best


# -- local mobility ----------------------------------------------------------

def orient_index(seed):
    """side_colour_bits[(side, colour)] = bitmask over (piece*4 + spin).

    The same static (side, colour) orientation index E555_backtracker.c builds,
    which is what turns "which pieces could sit here" into four big-int ANDs.
    The naive loop over placed pieces is far too slow for a real corpus."""
    idx = {}
    for pid in range(N_PIECES):
        for spin in range(4):
            cols = V.rotate_edges(seed[pid], spin)
            bit = 1 << (pid * 4 + spin)
            for d in range(4):
                key = (d, cols[d])
                idx[key] = idx.get(key, 0) | bit
    return idx


def _at_least(legal, masks, k):
    """Candidates in `legal` that match at least k of `masks`.

    A candidate matching k or more of them lies in the intersection of at least
    one k-subset, so the union over k-subsets is exactly the set wanted."""
    if k <= 0:
        return legal
    if k > len(masks):
        return 0
    acc = 0
    for combo in itertools.combinations(masks, k):
        got = legal
        for m in combo:
            got &= m
        acc |= got
    return acc


def mobility(colors, cells, idx, placed_bits):
    """(strictly_better, no_worse) piece-orientation counts summed over `cells`.

    For each cell: `legal` is the set of (piece, spin) that show grey on every
    outward frame side -- which also enforces the piece TYPE for free, since
    only a corner shows two greys. `masks` are the interior sides that have a
    placed neighbour to agree with. The incumbent matches `cur` of them, so the
    escape routes are the candidates matching more.

    The incumbent is itself legal and matches exactly `cur`, so it falls inside
    the "no worse" set and outside the "strictly better" one. It is subtracted,
    so both numbers count ALTERNATIVES to what is already there."""
    better = worse = 0
    for cell in cells:
        cols = colors.get(cell)
        if cols is None:
            continue
        legal = placed_bits
        for d in FRAME_SIDES.get(cell, ()):
            legal &= idx.get((d, GREY), 0)
        if not legal:
            continue

        masks, cur = [], 0
        r, c = divmod(cell, SIDE)
        for d, (nr, nc) in ((NORTH, (r + 1, c)), (EAST, (r, c + 1)),
                            (SOUTH, (r - 1, c)), (WEST, (r, c - 1))):
            if not (0 <= nr < SIDE and 0 <= nc < SIDE):
                continue
            other = colors.get(nr * SIDE + nc)
            if other is None:
                continue
            want = other[(d + 2) & 3]            # the neighbour's facing side
            masks.append(idx.get((d, want), 0))
            if cols[d] == want:
                cur += 1

        better += _at_least(legal, masks, cur + 1).bit_count()
        worse += _at_least(legal, masks, cur).bit_count() - 1
    return better, worse


# -- rank-sum and spread -----------------------------------------------------

def rank_sum(recs, keys):
    """Add each board's position in each measure's ordering. Lowest wins.

    `keys` is (name, high_is_better) pairs. Boards missing a measure share the
    worst position for it, so a corpus where dives did not run still ranks.
    Sorts `recs` in place and returns it; ties break on breaks, then J.

    Two rules keep a tie from becoming a ranking. A measure every board agrees
    on is skipped outright -- a beamer dump with one stop row makes `breaks`,
    `J` and the rest constants, and a stable sort would otherwise turn each of
    them into an arbitrary permutation that outweighs the measures that do vary.
    Boards that tie WITHIN a measure share the first of the positions they span,
    so a tie costs them all the same instead of ordering them by file position."""
    for rec in recs:
        rec["rsum"] = 0
    for name, high_is_better in keys:
        have = [r for r in recs if r.get(name) is not None]
        missing = [r for r in recs if r.get(name) is None]
        if len({r[name] for r in have}) <= 1:
            continue                          # a constant measure ranks nothing
        have.sort(key=lambda r: r[name], reverse=high_is_better)
        i = 0
        while i < len(have):
            j = i
            while j < len(have) and have[j][name] == have[i][name]:
                j += 1
            for rec in have[i:j]:
                rec["rsum"] += i              # competition ranking: ties share
            i = j
        for rec in missing:
            rec["rsum"] += len(have)
    recs.sort(key=lambda r: (r["rsum"], r["breaks"], r["J"]))
    return recs


# -- dives -------------------------------------------------------------------

def write_mask(path, mask, note):
    """A 16x16 0/1 mask in the dialect data/holes_*.csv uses: '#' comments, then
    16 lines of 16 values, FIRST DATA LINE = ROW 0 (BOTTOM)."""
    with open(path, "w") as fh:
        fh.write(f"# {note}\n")
        fh.write("# Row convention: first data line below = row 0 (BOTTOM), last = row 15 (TOP).\n")
        for r in range(SIDE):
            vals = ["1" if mask >> (r * SIDE + c) & 1 else "0" for c in range(SIDE)]
            fh.write(",".join(vals) + "\n")


def run_dives(recs, seed_path, backtracker, tmpdir):
    """Fill dive_min on `recs`, in one subprocess per distinct window mask.

    Records are re-tagged d<seq> in the batch CSV, where seq counts boards over
    the whole run. Neither of the obvious keys works: the corpus repeats
    config_id (data/best_463.csv lines 1 and 4 share one while agreeing on zero
    of 256 cells), and the row index restarts at 0 in every input file."""
    # Grouped by the MASK, not the window name: --holes applies one mask to every
    # record in the file, and two boards can share the name `hull` while having
    # different outlines. Runs are traded against group count so a corpus of all
    # distinct hulls cannot turn into thousands of subprocess calls.
    shapes = {}
    for rec in recs:
        shapes.setdefault(rec["mask"], []).append(rec)
    # The budget wins. A floor of 2 here would defeat it outright: 500 boards
    # with 500 distinct hulls give 400 // 500 == 0, and max(2, 0) would launch
    # 1000 subprocesses rather than the 400 the constant promises.
    runs = max(1, min(DIVE_RUNS, DIVE_CALL_BUDGET // max(1, len(shapes))))

    for gi, (mask, group) in enumerate(shapes.items()):
        batch = tmpdir / f"batch_{gi}.csv"
        with open(batch, "w", newline="") as fh:
            w = csv.writer(fh, lineterminator="\n")
            for rec in group:
                _, _, pos, rot = V.parse_row(next(csv.reader([rec["line"]])))
                w.writerow([f"d{rec['seq']}", rec["score"]] + pos + rot)
        mask_path = tmpdir / f"mask_{gi}.csv"
        write_mask(mask_path, mask, f"distiller window {group[0]['win']}")

        samples = {rec["seq"]: [] for rec in group}
        warned = set()
        for run in range(runs):
            out = tmpdir / f"dive_{gi}_{run}.csv"
            cmd = [str(backtracker), str(seed_path), str(batch), str(out),
                   "--break_mode", "stuck", "--restarts", str(DIVE_RESTARTS),
                   "--breaks", str(DIVE_BREAK_CEILING),
                   "--holes", str(mask_path)]
            try:
                subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, timeout=1800)
            except subprocess.TimeoutExpired:
                _dive_warn(warned, gi, "timed out")
                continue
            except subprocess.CalledProcessError as exc:
                # A negative code is a signal. SIGILL (-4) here almost always
                # means the binary was built with the Makefile's -march=native
                # on a different machine (or a migrated VM) from the one running
                # it; rebuilding in place fixes it, so name that rather than
                # reporting a bare CalledProcessError.
                rc = exc.returncode
                why = f"signal {-rc}" if rc < 0 else f"exit {rc}"
                if rc in (-4, 132):
                    why += (" (illegal instruction: the binary was built for a"
                            " different CPU. Rebuild -- `make backtracker`, or"
                            " `make backtracker ARCH=generic` if the machine"
                            " that runs it is not the one that builds it)")
                tail = (exc.stderr or b"").decode(errors="replace").strip().splitlines()
                if tail:
                    why += f": {tail[-1]}"
                _dive_warn(warned, gi, why)
                continue
            for cid, breaks in _read_dive_out(out):
                if cid in samples:
                    samples[cid].append(breaks)

        for rec in group:
            got = samples[rec["seq"]]
            if got:
                rec["dive_min"] = min(got)


def _dive_warn(warned, gi, why):
    """One line per failing group, not per failing run: a broken binary fails
    every run of every group, and DIVE_RUNS x groups lines of it buries the
    ranking that still came out fine."""
    if gi in warned:
        return
    warned.add(gi)
    print(f"[dive] window group {gi} failed -- {why}; ranking without its dives",
          file=sys.stderr)


def _read_dive_out(path):
    """(seq, breaks) per record from the backtracker's best-per-record CSV,
    whose rows are canonical `config_id,score,pos...,rot...`."""
    if not Path(path).exists():
        return
    for line in open(path, newline=""):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        rec = V.parse_row(next(csv.reader([line])))
        if rec is None:
            continue
        cid, sol, _, _ = rec
        # The backtracker re-tags each record `d<seq>_<score>`, so the seq is the
        # first segment -- not the whole id.
        if not cid.startswith("d"):
            continue
        head = cid[1:].split("_", 1)[0]
        if not head.isdigit():
            continue
        try:
            yield int(head), N_EDGES - int(sol)
        except ValueError:
            continue


# -- plan emission -----------------------------------------------------------

def stage_c_command(rec, mask_rel, clue_flags):
    """The tool and flags implied by this board's window.

    The ender is the purpose-built endgame tool, so a complete board goes there
    with this board's own mask as its neighbourhood. There is no mode choice to
    make any more: the ender picks its own neighbourhoods, and --holes pins the
    pool to exactly the window this distiller computed, so the profile supplies
    only the escalation over that fixed pool (m4, m8, ... with repeats) and the
    per-board budget. A board with empty cells is still the topper's job."""
    if rec["placed"] == N_PIECES:
        args = ["--holes", mask_rel,
                "--profile", "deep", "--board_time_limit", "900"]
        return "src/C_tail/E555_ender.py", args + clue_flags
    win = rec["win"]
    if win != "hull" and win[0] in "TBLR":
        args = ["--side", win[0], "--band_depth", win[1:]]
    else:
        args = ["--holes", mask_rel]
    return "src/C_tail/E555_topper.py", args + clue_flags


def write_plan(recs, root, seed_path, in_path, clue_flags):
    """plan_<stem>/ : one mask per board plus a runnable run_plan.sh.

    Blocks are named by the global board sequence, and each cites the file its
    own board came from: --start_row is an index into ONE csv, so a run over
    several inputs cannot share a single $IN."""
    # Named after the first input; a run over several files still writes one
    # plan, and each block inside it cites its own board's file.
    out_dir = Path.cwd() / f"plan_{Path(in_path).stem}"
    out_dir.mkdir(exist_ok=True)

    lines = ["#!/usr/bin/env bash",
             "set -euo pipefail",
             f'ROOT="{root}"                    # repo root -- edit if you move things',
             'PY="${PYTHON:-python3}"',
             f'SEED="{seed_path}"',
             'HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"',
             'OUT="$HERE/out"; mkdir -p "$OUT"',
             ""]

    for rank, rec in enumerate(recs, 1):
        tag = f"b{rec['seq']:04d}"
        mask_name = f"{tag}_{rec['win']}.holes.csv"
        write_mask(out_dir / mask_name, rec["mask"],
                   f"rank {rank}: {rec['path']} row {rec['row']}, window {rec['win']}")
        script, args = stage_c_command(rec, f'"$HERE/{mask_name}"', clue_flags)
        lines += [
            f"# rank {rank}  {rec['file']} row {rec['row']}  breaks={rec['breaks']}  "
            f"win={rec['win']} ({rec['cells']} cells, J={rec['J']})  "
            f"closure={rec['closure']:.2f}  fixers={rec['fixers']}  "
            f"dive_min={rec.get('dive_min', '-')}",
            f'"$PY" "$ROOT/{script}" "$SEED" "{rec["path"]}" "$OUT/{tag}.csv" \\',
            f'      --start_row {rec["row"]} --num_rows 1 ' + " ".join(args),
            "",
        ]

    sh = out_dir / "run_plan.sh"
    sh.write_text("\n".join(lines))
    sh.chmod(0o755)
    print(f"[plan] {len(recs)} board(s) -> {out_dir}/  (run_plan.sh + {len(recs)} mask(s))")


# -- output ------------------------------------------------------------------

# `seq` is the board's number over the whole run: it is what --explain takes and
# what --plan names its blocks and masks by, so it has to be on screen. `row` is
# the index inside its own file, which is what --start_row wants.
TABLE = (("rank", 5), ("seq", 8), ("file", 20), ("row", 8), ("id", 16),
         ("score", 6), ("win", 6), ("cells", 6), ("J", 5), ("closure", 8),
         ("fixers", 7), ("dive_min", 9), ("break_rows", 11), ("span", 6),
         ("clues", 6), ("rsum", 6), ("agree", 6))


def print_table(recs):
    print()
    print("".join(f"{name:>{w}}" for name, w in TABLE))
    print("-" * sum(w for _, w in TABLE))
    for rank, rec in enumerate(recs, 1):
        rec = dict(rec, rank=rank)
        cells = []
        for name, w in TABLE:
            val = rec.get(name)
            if val is None:
                val = "-"
            elif name == "closure":
                val = f"{val:.2f}"
            if name in ("id", "file"):
                val = str(val)[-(w - 1):]
            cells.append(f"{val:>{w}}")
        print("".join(cells))
    print()


def explain(rec, seed, colors, bad):
    """Everything known about one board, for reading rather than batching."""
    _, _, pos, rot = V.parse_row(next(csv.reader([rec["line"]])))
    print(f"\n=== {rec['file']} row {rec['row']}  id={rec['id']} ===")
    V.print_board(V.build_board(pos, rot), seed)

    print(f"  score {rec['score']}  breaks {rec['breaks']}  "
          f"placed {rec['placed']}  clues {rec['clues']}")
    print(f"  breaks touch rows {sorted({c // SIDE for c in bad})}")
    print(f"  breaks touch cols {sorted({c % SIDE for c in bad})}")
    print(f"  chosen window  {rec['win']}: {rec['cells']} cells, J={rec['J']}")
    print(f"  closure {rec['closure']:.2f}   fixers {rec['fixers']}   "
          f"dive_min {rec.get('dive_min', '-')}")
    print("\n  window (o = free, . = kept, # = touches a break):")
    for r in range(SIDE - 1, -1, -1):
        row = ""
        for c in range(SIDE):
            cell = r * SIDE + c
            row += "#" if cell in bad else ("o" if rec["mask"] >> cell & 1 else ".")
        print(f"    ({r + 1:2d})  {row}")
    print()


# -- triage ------------------------------------------------------------------

def job_key(pos, rot):
    """What Stage C will actually be handed, as one hashable value.

    The window frees the topmost placed row along with the empty ones, so two
    boards that agree BELOW that row are the same job however their top row
    differs -- E555_clean_csv.py only collapses the single-cell case. On a
    corpus of 12-row partials this is worth about a third of the rows.

    The first board of a group represents it. The group's members hold the same
    pieces and hand Stage C the same problem, but they arrange the top row
    differently, so their closure -- which reads the pool as the file leaves it,
    one row lower than the window will -- can differ by a nat or so. Which
    member is kept therefore moves a board slightly in the ranking, never in
    what it solves."""
    rows = [0] * SIDE
    for cell in pos:
        if cell != UNPLACED:
            rows[cell // SIDE] += 1
    full = [r for r in range(SIDE) if rows[r] == SIDE]
    cut = (max(full) if full else 0) * SIDE
    return hash(tuple(sorted((c, p, rot[p]) for p, c in enumerate(pos)
                             if c != UNPLACED and c < cut)))


def triage(paths, seed, top, dedup):
    """Rank a corpus by closure alone, in one streaming pass.

    No board is built and no record outlives the heap, so the cost is flat in
    the corpus size and the memory is flat in `top`. That is the whole point:
    the full pipeline measures every board before it can rank any, which a
    six-figure corpus cannot afford.

    Returns (kept records best first, boards read, boards dropped as repeats)."""
    heap, seen, seq, dropped = [], set(), 0, 0
    for path in paths:
        name = Path(path).name
        full = str(Path(path).resolve())
        with open(path, newline="") as fh:
            for row, raw in enumerate(csv.reader(fh)):
                rec = V.parse_row([f.strip() for f in raw])
                if rec is None:                       # comment, header, short row
                    continue
                cid, _sol, pos, rot = rec
                seq += 1
                if dedup:
                    key = job_key(pos, rot)
                    if key in seen:
                        dropped += 1
                        continue
                    seen.add(key)
                j = closure(pos, rot, seed)
                item = (j, -seq, dict(file=name, path=full, row=row, id=cid,
                                      seq=seq - 1, closure=j,
                                      line=",".join(raw) + "\n"))
                # A bounded min-heap on closure: the worst of the keepers is
                # always at the root, so a corpus of any size costs `top` records.
                if len(heap) < top:
                    heapq.heappush(heap, item)
                elif item[0] > heap[0][0]:
                    heapq.heapreplace(heap, item)
    kept = [rec for _, _, rec in sorted(heap, key=lambda t: (-t[0], -t[1]))]
    return kept, seq, dropped


# -- main --------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Distil a corpus of high-scoring boards down to the few "
                    "worth Stage C time, and write the commands to attack them.")
    ap.add_argument("inputs", nargs="+", help="one or more canonical board CSVs")
    ap.add_argument("--top", type=int, default=25,
                    help="how many boards to keep (default 25)")
    ap.add_argument("--out", metavar="FILE",
                    help="write the distilled boards, input rows re-ordered verbatim")
    ap.add_argument("--plan", action="store_true",
                    help="write plan_<stem>/: hole masks plus a runnable run_plan.sh")
    ap.add_argument("--triage", action="store_true",
                    help="first cut for a corpus too large to measure in full: "
                         "one streaming pass, rank by closure, no windows, no "
                         "mobility, no dives. Honours --top and --out")
    ap.add_argument("--no_dedup", action="store_true",
                    help="with --triage, keep boards that present the same Stage C "
                         "job (identical below their topmost full row)")
    ap.add_argument("--explain", type=int, metavar="N", default=None,
                    help="deep-dive board seq N instead of ranking (the seq "
                         "column, which counts boards over the whole run)")
    ap.add_argument("--seed_file", help="piece seed file (default: data/seed_Edge5.txt)")
    ap.add_argument("--root", help="repo root (default: this tool's parent directory)")
    args = ap.parse_args()

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    seed_path = V.find_seed(args.seed_file)
    seed = V.load_seed(seed_path)

    if args.triage:
        if args.explain is not None or args.plan:
            raise SystemExit("[ERROR] --triage ranks and nothing else; it has no "
                             "windows to explain and no masks to plan with")
        kept, total, dropped = triage(args.inputs, seed, args.top, not args.no_dedup)
        if not kept:
            print("[triage] no board rows in the input", file=sys.stderr)
            return 1
        print(f"[triage] {total} board(s) read"
              + (f", {dropped} repeat Stage C job(s) dropped" if dropped else "")
              + f", {total - dropped} ranked, top {len(kept)} kept", file=sys.stderr)
        print()
        print(f"{'rank':>5}{'seq':>7}{'file':>22}{'row':>7}{'id':>18}{'closure':>10}")
        print("-" * 69)
        for rank, rec in enumerate(kept, 1):
            print(f"{rank:>5}{rec['seq']:>7}{rec['file'][-21:]:>22}{rec['row']:>7}"
                  f"{str(rec['id'])[-17:]:>18}{rec['closure']:>10.2f}")
        print()
        if args.out:
            R.write_emit(args.out, kept, rescore=False)
        return 0

    idx = orient_index(seed)

    backtracker = root / "bin" / "E555_backtracker"
    if not backtracker.exists():
        print(f"[note] {backtracker} not found -- ranking without dives "
              f"(run `make` in {root} to enable them)", file=sys.stderr)
        backtracker = None

    # One file at a time so each record can carry the path it came from: the
    # row index R.read_boards reports restarts at 0 in every file, and
    # --start_row is an index into one csv.
    skipped = []
    recs = []
    for path in args.inputs:
        full = str(Path(path).resolve())
        for rec in R.read_boards([path], seed, skipped, progress_every=20000):
            if rec["breaks"] == 0:
                print(f"[note] {rec['file']} row {rec['row']} is already 480/480; skipping",
                      file=sys.stderr)
                continue
            _, _, pos, rot = V.parse_row(next(csv.reader([rec["line"]])))
            _, bad = board_colors(pos, rot, seed)
            rec.update(pick_window(bad))
            rec["closure"] = closure(pos, rot, seed)
            rec["path"] = full
            rec["seq"] = len(recs)
            recs.append(rec)

    if not recs:
        print("[distil] no rankable boards in the input", file=sys.stderr)
        return 1 if skipped else 0

    if args.explain is not None:
        for rec in recs:
            if rec["seq"] != args.explain:
                continue
            _, _, pos, rot = V.parse_row(next(csv.reader([rec["line"]])))
            colors, bad = board_colors(pos, rot, seed)
            rec["fixers"] = mobility(colors, bad, idx, _placed_bits(pos))[0]
            explain(rec, seed, colors, bad)
            return 1 if skipped else 0
        print(f"[distil] board {args.explain} not found in the input", file=sys.stderr)
        return 1

    # Pass 1 -- cheap measures on everything. closure leads because it is the
    # only one of these that still varies when a corpus shares a stop row.
    rank_sum(recs, (("closure", True), ("breaks", False), ("J", False)))
    short = recs[:max(SHORTLIST_MIN, SHORTLIST_MULT * args.top)]
    print(f"[distil] {len(recs)} board(s) measured, {len(short)} shortlisted", file=sys.stderr)
    # The window is the biggest thing on a record and only the shortlist needs
    # one from here on, so a large corpus does not carry the rest of them.
    cut = set(id(rec) for rec in short)
    for rec in recs:
        if id(rec) not in cut:
            rec.pop("mask", None)

    # Pass 2 -- mobility and dives on the shortlist only.
    for rec in short:
        _, _, pos, rot = V.parse_row(next(csv.reader([rec["line"]])))
        colors, bad = board_colors(pos, rot, seed)
        rec["fixers"] = mobility(colors, bad, idx, _placed_bits(pos))[0]

    if backtracker is not None:
        with tempfile.TemporaryDirectory(prefix="e555_distil_") as td:
            run_dives(short, seed_path, backtracker, Path(td))

    # Pass 3 -- rank, then spread. The spread runs on E555_rank.py's whole-board
    # agreement: what Stage C keeps is the board OUTSIDE the window, so comparing
    # the pieces inside it -- the ones about to be lifted -- separates nothing.
    rank_sum(short, (("closure", True), ("breaks", False), ("J", False),
                     ("fixers", True), ("dive_min", False)))
    kept = R.select_diverse(short, args.top)

    print_table(kept)
    if args.out:
        R.write_emit(args.out, kept, rescore=False)
    if args.plan:
        clue_flags = []
        if any(rec["clues"] >= 4 for rec in kept):
            clue_flags = ["--clue_center", "--clue_corners"]
            print("[plan] corpus carries clues; propagating --clue_center --clue_corners")
        write_plan(kept, root, seed_path.resolve(), args.inputs[0], clue_flags)

    return 1 if skipped else 0


def _placed_bits(pos):
    """Bitmask over (piece*4 + spin) of every piece actually on the board."""
    bits = 0
    for pid in range(N_PIECES):
        if pos[pid] != UNPLACED:
            bits |= 0xF << (pid * 4)
    return bits


if __name__ == "__main__":
    sys.exit(main())
