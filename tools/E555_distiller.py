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

    floor     the first-moment entropy floor for that window shape: the fewest
              breaks a TYPICAL board of that shape can be completed to. A
              property of the WINDOW, not the board, so it never ranks anything
              and is printed for context only; blank for hull masks, whose shape
              differs per board. (From the measured palette: an inner junction
              matches with p=0.0589, a ring junction with p=0.2004, both flat to
              within 0.2% on data/seed_Edge5.txt.)

    fixers    piece-orientations that could sit on a BREAK cell and match
              strictly more of its junctions than the incumbent does: literal
              single-swap escape routes. Higher is better. It disagrees with the
              geometry -- measured 2..24 over seven boards that all score 463,
              the worst window carrying the most escape routes -- so it is real
              independent information.

    subs      the same count relaxed to "no worse than the incumbent", over
              every cell of the window: how loose the neighbourhood is overall.
              Context; not ranked on.

    dive_min  the best break count randomized greedy dives reach over the window
              (E555_backtracker --break_mode stuck, ~12k dives). Lower is
              better. It lands far above a CP-SAT incumbent -- 26..30 against 17
              on data/best_463.csv -- so it ranks windows against each other and
              is never a bound on one, but it is the only measure here that
              samples the repair landscape rather than describing it. It is also
              the only one that moves between runs (see THE DIVE ENGINE). Blank
              without bin/E555_backtracker.

    rsum      the rank-sum; lower is better. See below.

    agree     how much of its repair window this board shares with the boards
              already picked, as a percentage. A property of the selection, not
              of the board, which is why `rank` is NOT sorted by `rsum`: the
              spread reaches past a slightly better board for a much more
              independent one. Blank for rank 1.

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
    clustered tightly near the floor.

STRATEGY

    Pass 1, every board: the rank.py measures plus the window, both cheap and
    linear, so this scales to a corpus of any size. Rank-sum on
    (breaks, J, corner_d), keep a generous shortlist.

    Pass 2, the shortlist only: mobility (bitsets) and dives, one subprocess per
    window shape rather than per board. Pass 1 has already cut the corpus to a
    few hundred, so the per-board dive budget is fixed whatever you fed in.

    Pass 3: rank-sum on (breaks, J, fixers, dive_min), then greedy max-min
    spread down to --top. The spread runs on the REPAIR SIGNATURE -- the window
    name plus the pieces inside it -- and not on E555_rank.py's whole-board
    agreement, which measures the wrong thing here: a cluster of beam siblings
    shares rows 0..11 and differs only in rows 12..15, so whole-board agreement
    sees one board and drops the rest -- but rows 12..15 ARE the repair problem,
    and those siblings are several genuinely different Stage C jobs.

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
import itertools
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
GREY = 0
UNPLACED = 999

# The entropy floor per window SHAPE, computed offline from the measured palette
# (see THE MEASURES). Board-independent by construction, hence context only.
# B/L/R equal T at the same depth by symmetry, so one entry serves all four.
FLOOR_BY_DEPTH = {2: 18, 3: 20, 4: 20, 5: 20, 6: 20, 7: 20}

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

def popcount(x):
    """Bits set. int.bit_count() is 3.10+, and requirements.txt allows 3.9."""
    try:
        return x.bit_count()
    except AttributeError:
        return bin(x).count("1")


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
    """The `depth`-deep band against one border, as a set of cell indices."""
    if side == "T":
        return {r * SIDE + c for r in range(SIDE - depth, SIDE) for c in range(SIDE)}
    if side == "B":
        return {r * SIDE + c for r in range(depth) for c in range(SIDE)}
    if side == "R":
        return {r * SIDE + c for r in range(SIDE) for c in range(SIDE - depth, SIDE)}
    if side == "L":
        return {r * SIDE + c for r in range(SIDE) for c in range(depth)}
    raise ValueError(f"unknown side {side!r}")


def hull(bad, pad=1):
    """The break cells grown by `pad` in every direction, clipped to the board.

    A mask of exactly the break cells would force the freed pieces to permute
    among themselves; the padding is what gives the re-solve somewhere to put
    them."""
    out = set()
    for cell in bad:
        r, c = divmod(cell, SIDE)
        for dr in range(-pad, pad + 1):
            for dc in range(-pad, pad + 1):
                rr, cc = r + dr, c + dc
                if 0 <= rr < SIDE and 0 <= cc < SIDE:
                    out.add(rr * SIDE + cc)
    return out


def window_cost(mask):
    """Junctions with at least one endpoint free. What the re-solve must satisfy."""
    return sum(1 for a, b, _, _ in ALL_JUNCTIONS if a in mask or b in mask)


def pick_window(bad):
    """The cheapest window covering every break cell, as a dict.

    Candidates are the four bands at their minimal covering depth plus the
    padded hull; cheapest means fewest junctions. A band that would have to
    reach deeper than the whole board is not a candidate."""
    rows = [c // SIDE for c in bad]
    cols = [c % SIDE for c in bad]
    depths = {"T": SIDE - min(rows), "B": max(rows) + 1,
              "L": max(cols) + 1, "R": SIDE - min(cols)}

    cands = []
    for side, depth in depths.items():
        if depth <= SIDE:
            cands.append((f"{side}{depth}", band(side, depth), depth))
    cands.append(("hull", hull(bad), None))

    best = None
    for name, mask, depth in cands:
        cost = window_cost(mask)
        if best is None or cost < best["J"] or (cost == best["J"] and len(mask) < best["cells"]):
            best = dict(win=name, mask=mask, cells=len(mask), J=cost,
                        floor=FLOOR_BY_DEPTH.get(depth) if depth else None,
                        depths=depths)
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

        better += popcount(_at_least(legal, masks, cur + 1))
        worse += popcount(_at_least(legal, masks, cur)) - 1
    return better, worse


# -- rank-sum and spread -----------------------------------------------------

def rank_sum(recs, keys):
    """Add each board's position in each measure's ordering. Lowest wins.

    `keys` is (name, high_is_better) pairs. Boards missing a measure share the
    worst position for it, so a corpus where dives did not run still ranks.
    Sorts `recs` in place and returns it; ties break on breaks, then J."""
    for rec in recs:
        rec["rsum"] = 0
    for name, high_is_better in keys:
        have = [r for r in recs if r.get(name) is not None]
        missing = [r for r in recs if r.get(name) is None]
        have.sort(key=lambda r: r[name], reverse=high_is_better)
        for i, rec in enumerate(have):
            rec["rsum"] += i
        for rec in missing:
            rec["rsum"] += len(have)
    recs.sort(key=lambda r: (r["rsum"], r["breaks"], r["J"]))
    return recs


def repair_signature(rec):
    """What Stage C will actually see: the window, and the pieces inside it.

    Deliberately NOT E555_rank.py's whole-board agreement -- see STRATEGY."""
    _, _, pos, rot = V.parse_row(next(csv.reader([rec["line"]])))
    mask = rec["mask"]
    return frozenset(pos[p] * 1024 + p * 4 + rot[p]
                     for p in range(N_PIECES)
                     if pos[p] != UNPLACED and pos[p] in mask)


def select_spread(recs, k):
    """Greedy max-min over the repair signature: take the best, then repeatedly
    the highest-ranked board least like everything already taken."""
    if k <= 0 or len(recs) <= k:
        return recs
    for rec in recs:
        rec["_sig"] = repair_signature(rec)

    # Each candidate carries its distance to the CLOSEST chosen board, updated
    # against the one board just added rather than recomputed against all of
    # them: K x M comparisons, not K x M x K. Same shape as R.select_diverse.
    chosen = [recs[0]]
    rest = recs[1:]
    dmin = [_sig_distance(rec, chosen[0]) for rec in rest]
    while len(chosen) < k and rest:
        best_i = max(range(len(rest)), key=lambda i: dmin[i])
        rec = rest.pop(best_i)
        rec["agree"] = round((1.0 - dmin.pop(best_i)) * 100)
        chosen.append(rec)
        for i, cand in enumerate(rest):
            d = _sig_distance(cand, rec)
            if d < dmin[i]:
                dmin[i] = d
    for rec in recs:
        rec.pop("_sig", None)
    return chosen


def _sig_distance(a, b):
    """1 for boards whose windows differ at all, else 1 - overlap fraction."""
    if a["win"] != b["win"]:
        return 1.0
    span = max(len(a["_sig"]), len(b["_sig"]))
    if span == 0:
        return 0.0
    return 1.0 - len(a["_sig"] & b["_sig"]) / span


# -- dives -------------------------------------------------------------------

def write_mask(path, mask, note):
    """A 16x16 0/1 mask in the dialect data/holes_*.csv uses: '#' comments, then
    16 lines of 16 values, FIRST DATA LINE = ROW 0 (BOTTOM)."""
    with open(path, "w") as fh:
        fh.write(f"# {note}\n")
        fh.write("# Row convention: first data line below = row 0 (BOTTOM), last = row 15 (TOP).\n")
        for r in range(SIDE):
            vals = ["1" if r * SIDE + c in mask else "0" for c in range(SIDE)]
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
        shapes.setdefault(frozenset(rec["mask"]), []).append(rec)
    runs = max(2, min(DIVE_RUNS, DIVE_CALL_BUDGET // max(1, len(shapes))))

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

    The ender is the purpose-built endgame tool, so a complete board goes there;
    --mode ring when the window is mostly border, since a border break heals by
    cascading around the frame. A board with empty cells is the topper's job."""
    border_frac = sum(1 for c in rec["mask"] if c in FRAME_SIDES) / max(1, len(rec["mask"]))
    if rec["placed"] == N_PIECES:
        mode = "ring" if border_frac >= 0.60 else "patch"
        args = ["--mode", mode, "--holes", mask_rel,
                "--reach", "3", "--max_changes", "24", "--time_limit", "300"]
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
        floor = rec["floor"] if rec["floor"] is not None else "-"
        lines += [
            f"# rank {rank}  {rec['file']} row {rec['row']}  breaks={rec['breaks']}  "
            f"win={rec['win']} ({rec['cells']} cells, J={rec['J']}, floor {floor})  "
            f"fixers={rec['fixers']}  dive_min={rec.get('dive_min', '-')}",
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
TABLE = (("rank", 5), ("seq", 5), ("file", 20), ("row", 5), ("id", 16),
         ("score", 6), ("win", 6), ("cells", 6), ("J", 5), ("floor", 6),
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
    print(f"  minimal covering band depth   " +
          "  ".join(f"{s}={d}" for s, d in sorted(rec["depths"].items())))
    print(f"  chosen window  {rec['win']}: {rec['cells']} cells, J={rec['J']}, "
          f"floor {rec['floor'] if rec['floor'] is not None else '-'}")
    print(f"  fixers {rec['fixers']}   subs {rec['subs']}   "
          f"dive_min {rec.get('dive_min', '-')}")
    print("\n  window (o = free, . = kept, # = touches a break):")
    for r in range(SIDE - 1, -1, -1):
        row = ""
        for c in range(SIDE):
            cell = r * SIDE + c
            row += "#" if cell in bad else ("o" if cell in rec["mask"] else ".")
        print(f"    ({r + 1:2d})  {row}")
    print()


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
    ap.add_argument("--explain", type=int, metavar="N", default=None,
                    help="deep-dive board N instead of ranking (the row, for a "
                         "single input file; otherwise the Nth board overall)")
    ap.add_argument("--seed_file", help="piece seed file (default: data/seed_Edge5.txt)")
    ap.add_argument("--root", help="repo root (default: this tool's parent directory)")
    args = ap.parse_args()

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    seed_path = V.find_seed(args.seed_file)
    seed = V.load_seed(seed_path)
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
            placed_bits = _placed_bits(pos)
            rec["fixers"], rec["subs"] = _measure_mobility(rec, colors, bad, idx, placed_bits)
            explain(rec, seed, colors, bad)
            return 1 if skipped else 0
        print(f"[distil] board {args.explain} not found in the input", file=sys.stderr)
        return 1

    # Pass 1 -- cheap measures on everything.
    rank_sum(recs, (("breaks", False), ("J", False), ("corner_d", False)))
    short = recs[:max(SHORTLIST_MIN, SHORTLIST_MULT * args.top)]
    print(f"[distil] {len(recs)} board(s) measured, {len(short)} shortlisted", file=sys.stderr)

    # Pass 2 -- mobility and dives on the shortlist only.
    for rec in short:
        _, _, pos, rot = V.parse_row(next(csv.reader([rec["line"]])))
        colors, bad = board_colors(pos, rot, seed)
        rec["fixers"], rec["subs"] = _measure_mobility(rec, colors, bad, idx, _placed_bits(pos))

    if backtracker is not None:
        with tempfile.TemporaryDirectory(prefix="e555_distil_") as td:
            run_dives(short, seed_path, backtracker, Path(td))

    # Pass 3 -- rank, then spread.
    rank_sum(short, (("breaks", False), ("J", False),
                     ("fixers", True), ("dive_min", False)))
    kept = select_spread(short, args.top)

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


def _measure_mobility(rec, colors, bad, idx, placed_bits):
    """fixers over the break cells, subs over the whole window."""
    fixers, _ = mobility(colors, bad, idx, placed_bits)
    _, subs = mobility(colors, rec["mask"], idx, placed_bits)
    return fixers, subs


if __name__ == "__main__":
    sys.exit(main())
