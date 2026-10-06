#!/usr/bin/env python3
"""
E555_rank.py -- rank and sort board CSVs by more than the break count.

WHY

    `score` (matched edges / 480) is the only sort key the canonical CSV
    carries, and it cannot tell two very different boards apart. Eighteen
    breaks spread over seven rows is a mess; the same eighteen packed into
    rows 14-15 is nearly finished, and it is the second one you want to hand
    to the next window step, the ender or the backtracker.

    This tool derives the missing measures from the board itself, so it works
    on any CSV in the stack -- including files produced long before it existed
    -- and by default it never rewrites the format: --out re-orders the input
    rows byte for byte. Add --rescore when you want the file rewritten
    canonically instead; see CANONICAL OUTPUT below.

THE MEASURES

    breaks      480 - score: every internal junction not currently matched.
                Lower is better. It is sortable but omitted from the human
                table because score carries the same information.
    score       matched internal edges, 0..480. Higher is better.
    solid       pieces with all four sides satisfied (the viewer's "Solid
                pieces"), 0..256. Higher is better.
    placed      pieces on the board, 0..256.
    border      longest unbroken run of the external frame, walked from the
                bottom-left corner counter-clockwise, stopping at the first frame
                cell that is unplaced or touched by any break. 0..60; 60 = a fully
                placed, break-free frame. Higher is better. `--border_only` keeps
                just the border==60 boards -- the clean start E555_finalizer needs
                in fixed mode (a placed-but-broken seam would poison the search).

    break_rows  how many distinct ROWS hold a break. Breaks confined to rows
                14-15 give 2. THE compactness measure: lower is better.
    break_cols  the same for columns -- the one to watch after --side L/R.
    span        bounding box of the break cells, "HxW".

    clean_b     contiguous COMPLETE, mismatch-free rows from row 0 upward.
    clean_t     the same from row 15 downward.
    clean_l     contiguous complete, mismatch-free columns from col 0 rightward.
    clean_r     the same from col 15 leftward. An open frontier just beyond a
                complete line does not invalidate it. Higher is better.

    corner_d    total distance the breaks still have to travel to reach their
                nearest corner: the quantity E555_topper.py minimizes after
                the break count. Fine-grained, so it breaks ties that the
                integer measures leave. Lower is better.

    clues       how many of the five Eternity II clue pieces sit at their
                published cell and spin, 0..5, for whichever of the four
                board orientations the board matches. Higher is better. A
                board that never carried clues reads 0; one from a clued
                beam run reads 5 until some later stage moves a clue piece.

    agree       printed only under --diverse: how many cells this root shares
                with the roots chosen before it, 0..256. It is not a property
                of the board but of the selection, so it cannot be sorted on.
                Lower means more independent; the first root always reads 0.

STRUCTURAL GROUPING  (--group_box, --per_group, --unique)

    Quality-first `--top` can accidentally remove whole lineages before diversity
    selection sees them. `--group_box R0:R1,C0:C1` groups boards by the exact
    piece+spin contents of an inclusive board rectangle and keeps the best
    `--per_group N` from every group BEFORE the global top-K. Examples:

        --group_box 0:11,0:15       one representative per 12-row foundation
        --group_box 4:11,0:11       W4 rounds-3 CCW retained core
        --group_box 4:11,4:15       W4 rounds-3 CW retained core
        --unique                    exact whole-board deduplication

CHOOSING INDEPENDENT ROOTS  (--diverse, --max_agree, --diversity_box)

    A run that emits a thousand boards rarely emits a thousand ideas. The top
    of a ranking is usually one lineage -- the same board with three cells
    moved -- so post-processing the top five spends five budgets on one
    hypothesis. These two options answer the practical question instead: give
    me a few starting points that are not siblings.

    The measure is CELL AGREEMENT: two boards agree on a cell when both put
    the same piece there at the same spin, and `agree(A,B)` counts those cells.
    Literal, and a number you can check by eye -- "these two share 202 of 203
    placed cells" is a fact about the boards, not a coefficient.

        --diverse K   after ranking, keep K boards chosen farthest-first: the
                      best board is the first root, and each next root is the
                      one whose CLOSEST already-chosen root is furthest away.
                      Costs K x M comparisons, not the M^2 of a full matrix.
        --max_agree P drop any board agreeing with an already-kept board on
                      more than fraction P of its placed cells -- a plain
                      near-duplicate filter. Runs before --diverse.

    These run after structural grouping, ranking and --top. Use
    `--diversity_box 0:9,0:15` to measure agreement only in the lower ten rows,
    so superficial top-frontier changes do not count as diversity.

    On the 15 exact row-12 partials of a whirlpool run, `--diverse 4` returns
    one board from each of the four lineages the pool actually holds (within a
    lineage boards agree on 199-202 of 203 cells, across lineages on 0-14).

MEMORY

    Ranking holds one light record per board -- the input line and its
    measures, about 1.8x the row's size on disk. Measured: 51,000 boards of a
    96 MB CSV peak at 165 MB.

    `--top N` streams instead, keeping only the best N in a bounded heap, so
    peak memory does not depend on file size (14 MB for the same input). The
    exception is `--group_box`/`--unique`: grouping happens first and may retain
    one small heap per distinct group, so its worst case still grows with input.

    Without --top, an input projected to need more than `--max_mem` GB
    (default 8) is refused before it starts, rather than being OOM-killed
    half-way through. --diverse and --max_agree add a ~16 KB fingerprint per
    retained board, which the projection accounts for.

CANONICAL OUTPUT  (--out --rescore)

    Field 2 of a board row is NOT reliably the score. Stage B writes its
    solution index there, and older files carry other dialects still -- all of
    them keep pos+rot as the last 512 fields, which is why every reader here
    accepts them, but it means you cannot `sort -t, -k2,2nr` a mixed corpus and
    get anything meaningful.

    `--out FILE --rescore` rewrites each row in the one canonical form

        config_id , score , pos[0..255] , rot[0..255]           (514 fields)

    with `score` recomputed from the seed, so the column becomes trustworthy
    whatever wrote the input. The id keeps the input's identity: when the row
    carried two or more leading metadata fields they are joined as `meta1_meta2`
    so a Stage B solution index is not silently lost.

    Without --rescore the emitted rows are copied through byte for byte, which
    is what you want when a downstream tool reads a metadata field you would
    otherwise flatten.

USAGE

    python3 tools/E555_rank.py boards.csv
    python3 tools/E555_rank.py step*.csv --sort breaks,break_rows --top 20
    python3 tools/E555_rank.py boards.csv --sort solid --out best.csv
    python3 tools/E555_rank.py boards.csv --border_only --out clean.csv
    python3 tools/E555_rank.py mixed*.csv --out pool.csv --rescore
    python3 tools/E555_rank.py boards.csv --csv > metrics.csv
    python3 tools/E555_rank.py pool.csv --top 200 --diverse 5 --out roots.csv
    python3 tools/E555_rank.py huge.csv --top 100 --max_agree 0.9
    python3 tools/E555_rank.py raw.csv --group_box 0:11,0:15 --per_group 1 --out roots.csv
    python3 tools/E555_rank.py pool.csv --top 5000 --diverse 500 --diversity_box 0:9,0:15
    python3 tools/E555_rank.py boards.csv --split score 460 high.csv low.csv

    --sort takes a comma-separated list of measure names, applied in order,
    and always puts the BEST board first -- `--sort solid` gives the most
    solid board, `--sort breaks` the least broken one, no extra syntax
    needed. To invert one measure to worst-first write `--sort=-solid`,
    with the '=' (a bare `--sort -solid` looks like a flag to argparse).
    The default, `breaks,break_rows`, is "fewest breaks first, then most
    compact". Several files can be ranked together; a `file` column then
    appears and --out merges them into one ranked CSV.
"""
from __future__ import annotations
import argparse, csv, heapq, os, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import E555_viewer as V                      # seed loading, row parsing, board build

SIDE, N_PIECES, N_EDGES, GREY = V.SIDE, V.N_PIECES, V.N_EDGES, 0
NORTH, EAST, SOUTH, WEST = 0, 1, 2, 3

# Every internal junction as (cell_a, cell_b, side_of_a, side_of_b), the same
# construction Stage C uses.
ALL_JUNCTIONS = []
for _cell in range(N_PIECES):
    _r, _c = divmod(_cell, SIDE)
    if _c + 1 < SIDE: ALL_JUNCTIONS.append((_cell, _cell + 1, EAST, WEST))
    if _r + 1 < SIDE: ALL_JUNCTIONS.append((_cell, _cell + SIDE, NORTH, SOUTH))

# Which outward sides a cell must show as grey, by position on the frame.
FRAME_SIDES = {}
for _cell in range(N_PIECES):
    _r, _c = divmod(_cell, SIDE)
    _s = []
    if _r == SIDE - 1: _s.append(NORTH)
    if _r == 0:        _s.append(SOUTH)
    if _c == SIDE - 1: _s.append(EAST)
    if _c == 0:        _s.append(WEST)
    if _s: FRAME_SIDES[_cell] = tuple(_s)

# Frame cells as a ring from the bottom-left corner, counter-clockwise: bottom
# row L->R, right column up, top row R->L, left column down. 60 cells; a board's
# clean-border arc is the longest unbroken prefix of this ring (see `border`).
BORDER_RING = (
    [c for c in range(SIDE)]                                     # bottom, BL..BR
    + [r * SIDE + (SIDE - 1) for r in range(1, SIDE)]           # right, up to TR
    + [(SIDE - 1) * SIDE + c for c in range(SIDE - 2, -1, -1)]  # top, TR..TL
    + [r * SIDE for r in range(SIDE - 2, 0, -1)]                # left, down to BL
)
N_BORDER = len(BORDER_RING)          # 60

# The two facing sides at each ring step i -> i+1 (wrapping at the last, closing
# seam), read off the cell-index delta. Lets the `border` walk test the seam
# between consecutive FRAME pieces only -- an unplaced interior behind the frame
# is the finalizer's job to fill, not a break in the border itself.
_DELTA_SIDES = {1: (EAST, WEST), -1: (WEST, EAST),
                SIDE: (NORTH, SOUTH), -SIDE: (SOUTH, NORTH)}
RING_SIDES = [_DELTA_SIDES[BORDER_RING[(i + 1) % N_BORDER] - BORDER_RING[i]]
              for i in range(N_BORDER)]


def _v(cell):
    """Rows between a cell and the nearest horizontal border (0..7)."""
    return min(cell // SIDE, SIDE - 1 - cell // SIDE)

def _h(cell):
    """Columns between a cell and the nearest vertical border (0..7)."""
    return min(cell % SIDE, SIDE - 1 - cell % SIDE)


def measure(pos, rot, seed):
    """All ranking measures of one board, from its pos/rot arrays.

    Raises ValueError if the board is not well formed -- a position outside
    0..255 (or the 999 sentinel), a rotation outside 0..3, or two pieces on one
    cell. Measuring such a board silently loses one of the colliding pieces, and
    the numbers that come out then drive --sort, --rescore and --field as if
    they meant something; read_boards turns the exception into a skipped row.
    """
    board = V.build_board(pos, rot)             # the one strict board validator
    colors = {}
    for r, row in enumerate(board):
        for c, cell in enumerate(row):
            if cell is not None:
                colors[r * SIDE + c] = V.rotate_edges(seed[cell[0]], cell[1])
    n_placed = len(colors)

    # One pass separates real color mismatches from unresolved junctions.
    # `bad` retains the viewer's solidity semantics: a piece beside a hole is not
    # solid. Deficit rows/columns still describe the whole unresolved region.
    breaks = mismatches = open_edges = 0
    corner_d = 0
    bad = set()
    rows, cols = set(), set()
    mismatch_rows, mismatch_cols = set(), set()
    for a, b, da, db in ALL_JUNCTIONS:
        ca, cb = colors.get(a), colors.get(b)
        if ca is not None and cb is not None:
            if ca[da] == cb[db]:
                continue
            mismatches += 1
            mismatch_rows.update((a // SIDE, b // SIDE))
            mismatch_cols.update((a % SIDE, b % SIDE))
        else:
            open_edges += 1
        breaks += 1
        corner_d += _v(a) + _v(b) + _h(a) + _h(b)
        bad.add(a); bad.add(b)
        rows.update((a // SIDE, b // SIDE))
        cols.update((a % SIDE, b % SIDE))

    # A mis-seated frame face is not an internal junction, but it still makes the
    # corresponding complete row/column unusable as a locked foundation.
    frame_bad_rows, frame_bad_cols = set(), set()
    for cell, col in colors.items():
        for side in FRAME_SIDES.get(cell, ()):
            if col[side] != GREY:
                frame_bad_rows.add(cell // SIDE)
                frame_bad_cols.add(cell % SIDE)
                bad.add(cell)

    solid = 0
    for cell, col in colors.items():
        if cell in bad:
            continue
        if all(col[s] == GREY for s in FRAME_SIDES.get(cell, ())):
            solid += 1

    occupied = set(colors)
    row_full = [all(r * SIDE + c in occupied for c in range(SIDE))
                for r in range(SIDE)]
    col_full = [all(r * SIDE + c in occupied for r in range(SIDE))
                for c in range(SIDE)]

    def leading_clean(order, full, mismatched, frame_bad):
        """Complete lines from one side before the first hole or true mismatch."""
        n = 0
        for k in order:
            if not full[k] or k in mismatched or k in frame_bad:
                break
            n += 1
        return n

    up, down = range(SIDE), range(SIDE - 1, -1, -1)
    clean_b = leading_clean(up, row_full, mismatch_rows, frame_bad_rows)
    clean_t = leading_clean(down, row_full, mismatch_rows, frame_bad_rows)
    clean_l = leading_clean(up, col_full, mismatch_cols, frame_bad_cols)
    clean_r = leading_clean(down, col_full, mismatch_cols, frame_bad_cols)

    # Clean-border arc: walk the frame ring from the bottom-left corner, counting
    # consecutive frame cells that are placed and whose seam to the previous frame
    # cell matches. Stops at the first gap or broken frame seam; only frame-to-
    # frame seams count, since an unplaced interior behind the frame is re-searched
    # by the finalizer, not a poison. border==60 means a fully placed frame with
    # every one of its 60 seams clean -- exactly the fixed-mode start it needs.
    border = 0
    for i, cell in enumerate(BORDER_RING):
        if cell not in colors:
            break
        if any(colors[cell][side] != GREY for side in FRAME_SIDES[cell]):
            break
        if i > 0:
            sa, sb = RING_SIDES[i - 1]
            if colors[BORDER_RING[i - 1]][sa] != colors[cell][sb]:
                break
        border += 1
    if border == N_BORDER:                     # reaching 60 also needs the closing seam
        sa, sb = RING_SIDES[-1]
        if colors[BORDER_RING[-1]][sa] != colors[BORDER_RING[0]][sb]:
            border = N_BORDER - 1

    if bad:
        rr = [c // SIDE for c in bad]; cc = [c % SIDE for c in bad]
        span = f"{max(rr) - min(rr) + 1}x{max(cc) - min(cc) + 1}"
    else:
        span = "-"

    # How many of the five Eternity II clue pieces sit at their published cell
    # and spin, for whichever orientation the board matches. Always measured --
    # it needs no flag, and a board that never had clues simply reads 0. Stage C
    # can move clue pieces unless told not to, so this is what says whether a
    # candidate still qualifies as a clue-satisfying solution.
    _clue_o, n_clues = V.clue_orient(pos, rot)

    return dict(breaks=breaks, score=N_EDGES - breaks,
                mismatches=mismatches, open=open_edges, solid=solid,
                placed=n_placed, border=border,
                break_rows=len(rows), break_cols=len(cols),
                mismatch_rows=len(mismatch_rows), mismatch_cols=len(mismatch_cols),
                span=span, clean_b=clean_b, clean_t=clean_t,
                clean_l=clean_l, clean_r=clean_r, corner_d=corner_d,
                clues=n_clues)


# Public measures. Real mismatch/open counts remain internal so clean-line
# calculations stay correct, but they are not separate ranking columns: the
# normal monitor table should fit on one line.
COLUMNS = ("breaks", "score", "solid", "placed", "border", "break_rows",
           "break_cols", "span", "clean_b", "clean_t", "clean_l",
           "clean_r", "corner_d", "clues")

# Compact monitor table. The omitted measures remain available to --sort,
# --field and --csv; they simply do not make every ordinary row wrap.
SHOWN = ("score", "solid", "placed", "break_rows", "break_cols", "span",
         "clean_b", "clean_t", "clean_l", "clean_r", "corner_d", "clues")

# Which way is "better" for each measure. Sorting is always best-first, so
# --sort solid puts the most solid board on top without any extra syntax; a
# '-' prefix (--sort=-solid) asks for worst-first instead.
HIGH_IS_BETTER = {"score", "solid", "placed", "border", "clues",
                  "clean_b", "clean_t", "clean_l", "clean_r"}
SORTABLE = tuple(k for k in COLUMNS if k != "span")


def parse_box(spec):
    """Inclusive `r0:r1,c0:c1` rectangle as an ordered tuple of cells."""
    try:
        rs, cs = spec.split(",", 1)
        r0, r1 = (int(x) for x in rs.split(":", 1))
        c0, c1 = (int(x) for x in cs.split(":", 1))
    except (AttributeError, ValueError):
        raise SystemExit("[ERROR] a box must be r0:r1,c0:c1, e.g. 4:11,0:11")
    if not (0 <= r0 <= r1 < SIDE and 0 <= c0 <= c1 < SIDE):
        raise SystemExit(f"[ERROR] box '{spec}' lies outside the 0..{SIDE-1} board")
    return tuple(r * SIDE + c for r in range(r0, r1 + 1)
                 for c in range(c0, c1 + 1))


def region_key(pos, rot, cells):
    """Compact exact piece+spin contents of `cells`, including empty cells."""
    inv = [0xFFFF] * N_PIECES
    for p, cell in enumerate(pos):
        if cell != 999:
            inv[cell] = (p << 2) | rot[p]
    out = bytearray(2 * len(cells))
    for i, cell in enumerate(cells):
        v = inv[cell]
        out[2*i] = v & 0xFF
        out[2*i+1] = v >> 8
    return bytes(out)


def fingerprint(line, cells=None):
    """Set of placed (cell,piece,spin) triples, optionally inside a rectangle."""
    _, _, pos, rot = V.parse_row(next(csv.reader([line])))
    allowed = None if cells is None else frozenset(cells)
    return frozenset(pos[p] * 1024 + p * 4 + rot[p]
                     for p in range(N_PIECES)
                     if pos[p] != 999 and (allowed is None or pos[p] in allowed))


def read_boards(paths, seed, skipped, progress_every=0, group_cells=None):
    """Yield one light record per board row of every input file.

    The record keeps the input LINE, not the parsed field list and not the
    pos/rot arrays: those cost ~38 KB a board against ~2 KB on disk, which is
    how a 1.5 GB corpus turned into a 30 GB process and an OOM kill. --rescore
    and the fingerprints re-parse the line, and they only ever touch the rows
    that survived the ranking.

    A row that fails validation is reported on stderr and appended to `skipped`
    rather than ranked: one malformed board in a large corpus must not cost the
    caller every other board, but it must not pass silently either, so main()
    exits nonzero when `skipped` is non-empty. This mirrors what the C tools do
    with a board they cannot use (see the [skip] lines in E555_roundhouse.c and
    E555_finalizer.c).
    """
    n = 0
    for path in paths:
        idx = 0
        with open(path, newline="") as fh:
            for line in fh:
                if not line.strip():
                    continue
                rec = V.parse_row(next(csv.reader([line])))
                if rec is None:
                    continue
                cid, sol, pos, rot = rec
                try:
                    m = measure(pos, rot, seed)
                except ValueError as exc:
                    # idx still advances: the number in the message is the row's
                    # real position in the file, which is what the reader needs
                    # to go and look at it.
                    print(f"[skip] {path}:{idx}: {exc}", file=sys.stderr)
                    skipped.append((path, idx))
                    idx += 1
                    continue
                # Preserve the second leading field in a canonicalized id. Stage B
                # writes its solution index there, and a mixed corpus cannot infer
                # reliably whether the number was metadata or an already-valid score.
                canon_id = f"{cid}_{sol}" if sol not in ("", "?") else cid
                n += 1
                if progress_every and n % progress_every == 0:
                    print(f"[rank] measured {n} boards", file=sys.stderr)
                extra = {"group": region_key(pos, rot, group_cells)} if group_cells else {}
                yield dict(file=Path(path).name, row=idx, id=cid,
                           canon_id=canon_id, line=line, **extra, **m)
                idx += 1


def write_emit(path, records, rescore, quiet=False):
    """Write the ranked rows: canonical when `rescore`, else byte for byte."""
    with open(path, "w", newline="") as out:
        w = csv.writer(out, lineterminator="\n")   # LF, not the csv module's CRLF
        for r in records:
            if rescore:
                _, _, pos, rot = V.parse_row(next(csv.reader([r["line"]])))
                w.writerow([r["canon_id"], r["score"]] + pos + rot)
            else:
                # The stored line, not a re-serialization of its fields: this is
                # the only way "verbatim" is literally true, quoting included.
                out.write(r["line"] if r["line"].endswith("\n") else r["line"] + "\n")
    if not quiet:
        kind = "canonical" if rescore else "verbatim"
        print(f"[emit] {len(records)} {kind} row(s) -> {path}")



def split_by_threshold(input_path, seed, key, threshold,
                       at_least_path, below_path, skipped, quiet=False):
    """Split one board file by an integer measure, preserving input order."""
    src = Path(input_path)
    high = Path(at_least_path)
    low = Path(below_path)

    src_abs = src.resolve()
    high_abs = high.resolve()
    low_abs = low.resolve()
    if high_abs == low_abs:
        raise SystemExit("[ERROR] the two split output paths must be different")
    if src_abs in (high_abs, low_abs):
        raise SystemExit("[ERROR] a split output must not overwrite the input file")

    high.parent.mkdir(parents=True, exist_ok=True)
    low.parent.mkdir(parents=True, exist_ok=True)
    # The outputs are NOT removed here. os.replace() below is atomic, so the
    # previous pair stays intact and readable until the new pair is complete --
    # and a run that ends up rejecting a row must not leave the caller with
    # neither the old files nor the new ones.

    tag = f".tmp.{os.getpid()}"
    high_tmp = Path(str(high) + tag)
    low_tmp = Path(str(low) + tag)
    high_tmp.unlink(missing_ok=True)
    low_tmp.unlink(missing_ok=True)

    n_high = n_low = 0
    try:
        with high_tmp.open("w", newline="") as out_high, \
             low_tmp.open("w", newline="") as out_low:
            for record in read_boards([str(src)], seed, skipped):
                line = record["line"]
                if not line.endswith("\n"):
                    line += "\n"
                if record[key] >= threshold:
                    out_high.write(line)
                    n_high += 1
                else:
                    out_low.write(line)
                    n_low += 1

        # A rejected row is reported through the exit status, but the rows that
        # DID parse are still a correct split of what could be read, and
        # throwing them away silently left the caller with nothing at all.
        os.replace(high_tmp, high)
        os.replace(low_tmp, low)
    except BaseException:
        high_tmp.unlink(missing_ok=True)
        low_tmp.unlink(missing_ok=True)
        raise

    if not quiet:
        print(f"[split] {key} >= {threshold}: {n_high} row(s) -> {high}")
        print(f"[split] {key} <  {threshold}: {n_low} row(s) -> {low}")
    return n_high, n_low


def parse_sort_spec(spec):
    """The `--sort` spec as [(measure, worst_first), ...], validated."""
    keys = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        worst_first = part.startswith("-")
        name = part[1:] if worst_first else part
        if name not in SORTABLE:
            raise SystemExit(f"[ERROR] unknown sort key '{name}'. "
                             f"Choose from: {', '.join(SORTABLE)}")
        keys.append((name, worst_first))
    return keys


def sort_key_of(keys, rec, seq):
    """One ascending-is-better tuple for `rec`, so best sorts first.

    Every sortable measure is an integer, so a descending key is just its
    negation, and `seq` -- the record's position in the input -- closes the
    tuple. That last term is what makes this identical to the stable
    least-significant-first sort it replaces: ties keep input order, and the
    tuple is unique, so a heap never has to compare the records themselves.
    """
    return tuple(-rec[n] if (n in HIGH_IS_BETTER) != worst_first else rec[n]
                 for n, worst_first in keys) + (seq,)


def collect(records, keys, top):
    """Rank the stream, keeping only the best `top` when one is asked for.

    With --top N this is a bounded heap: N records live at once whatever the
    file holds, which is the difference between ranking a multi-GB pool and
    being killed by the kernel. Without it every record is held, as before.
    """
    if top <= 0:
        out = [(sort_key_of(keys, r, i), r) for i, r in enumerate(records)]
        out.sort()
        return [r for _, r in out]
    heap = []                                   # min-heap on the NEGATED key,
    for i, r in enumerate(records):             # so heap[0] is the worst kept
        nk = tuple(-x for x in sort_key_of(keys, r, i))
        if len(heap) < top:
            heapq.heappush(heap, (nk, r))
        elif nk > heap[0][0]:
            heapq.heapreplace(heap, (nk, r))
    return [r for _, r in sorted(heap, reverse=True)]


def collect_grouped(records, keys, per_group):
    """Keep the best `per_group` records for every exact structural group."""
    groups = {}
    seen = 0
    for seq, r in enumerate(records):
        seen += 1
        nk = tuple(-x for x in sort_key_of(keys, r, seq))
        h = groups.setdefault(r["group"], [])
        if len(h) < per_group:
            heapq.heappush(h, (nk, r))
        elif nk > h[0][0]:
            heapq.heapreplace(h, (nk, r))
    kept = [r for h in groups.values() for _, r in h]
    kept.sort(key=lambda r: sort_key_of(keys, r, r["row"]))
    print(f"[group] kept {len(kept)} of {seen} boards from {len(groups)} exact "
          f"group(s), at most {per_group} per group", file=sys.stderr)
    return kept


def _agree_prints(records, cells=None):
    """Attach the requested-region fingerprint to each record once."""
    for r in records:
        if "fp" not in r:
            r["fp"] = fingerprint(r["line"], cells)
    return records


def filter_max_agree(records, frac, cells=None):
    """Drop boards agreeing with an already-kept board on more than `frac`.

    A plain near-duplicate filter, walked best-first so the board that survives
    a cluster is its best member. The denominator is the smaller number of placed
    cells in the selected diversity region.
    """
    _agree_prints(records, cells)
    kept = []
    for r in records:
        n = max((len(r["fp"] & k["fp"]) / max(1, min(len(r["fp"]), len(k["fp"])))
                 for k in kept), default=0.0)
        if n <= frac:
            kept.append(r)
    print(f"[max-agree] kept {len(kept)} of {len(records)} boards at agreement "
          f"<= {frac:g}", file=sys.stderr)
    return kept


def select_diverse(records, k, cells=None, metric="cells"):
    """Farthest-first roots using raw or placed-normalized cell agreement."""
    _agree_prints(records, cells)
    if not records:
        return records

    def sim(a, b):
        shared = len(a["fp"] & b["fp"])
        if metric == "fraction":
            return shared / max(1, min(len(a["fp"]), len(b["fp"])))
        return shared

    if k >= len(records):
        records[0]["agree"] = 0
        for i, r in enumerate(records[1:], 1):
            r["agree"] = max((len(r["fp"] & q["fp"]) for q in records[:i]), default=0)
        return records

    roots = [records[0]]
    records[0]["agree"] = 0
    rest = records[1:]
    closest = [sim(r, roots[0]) for r in rest]       # high = close
    while len(roots) < k and rest:
        i = min(range(len(rest)), key=lambda j: (closest[j], j))
        pick = rest.pop(i)
        closest.pop(i)
        pick["agree"] = max((len(pick["fp"] & q["fp"]) for q in roots), default=0)
        roots.append(pick)
        for j, r in enumerate(rest):
            a = sim(r, pick)
            if a > closest[j]:
                closest[j] = a
    return roots


def count_rows(paths):
    """Data lines across the inputs, counted by newline -- fast and close
    enough to project the memory the ranking will want."""
    n = 0
    for path in paths:
        with open(path, "rb") as fh:
            while True:
                buf = fh.read(1 << 22)
                if not buf:
                    break
                n += buf.count(b"\n")
    return n


# What holding the whole input costs, measured: 51,000 boards of a 96 MB CSV
# peaked at 165 MB, so the light record is about 1.8x its line on disk -- which
# is the right shape for a projection, since a record's size follows its row's
# width. A fingerprint set is another ~16 KB, and only --diverse/--max_agree
# build them.
RECORD_OVERHEAD = 1.8
BYTES_PER_PRINT = 16000
GROUP_KEY_BASE = 96


def check_memory(paths, args, group_cells=None):
    """Refuse a run projected to exceed --max_mem.

    A plain --top N is streaming. Structural grouping must precede the global
    top-K, however, and may retain one heap per distinct group; in the worst case
    every input row is its own group. Account for that rather than claiming the
    later --top makes the grouping pass bounded.
    """
    grouping = group_cells is not None
    if args.top > 0 and not grouping:
        return
    size = sum(Path(p).stat().st_size for p in paths)
    rows = count_rows(paths)
    want = size * RECORD_OVERHEAD
    if grouping:
        want += rows * (GROUP_KEY_BASE + 2 * len(group_cells))
    if args.diverse or args.max_agree is not None:
        compared = args.top if args.top > 0 else rows
        want += compared * BYTES_PER_PRINT
    limit = args.max_mem * (1 << 30)
    if want > limit:
        hint = ("Reduce the input in stages, use a smaller --group_box, or raise --max_mem."
                if grouping else
                "Add --top N to stream with bounded memory, or raise --max_mem.")
        raise SystemExit(
            f"[ERROR] {rows:,} boards would need up to about {want / (1<<30):.2g} GB, over "
            f"the --max_mem limit of {args.max_mem:g} GB.\n"
            f"        {hint}")


def _status(skipped):
    """The process exit status, reported once after all output has been written.

    Every return path in main() goes through this. Rejected boards have to leave
    a nonzero status behind -- a pipeline stage that silently ranked 900 of its
    1000 boards looks exactly like one that ranked all 1000 -- but the status is
    settled last, so --field still prints its bare number for the shell to
    capture before the run ends.
    """
    if not skipped:
        return 0
    print(f"[ERROR] {len(skipped)} board(s) failed validation and were left out "
          "of the ranking", file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(
        description="Rank and sort Eternity II board CSVs by compactness, "
                    "solidity and break count.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="measures: " + ", ".join(SORTABLE))
    ap.add_argument("inputs", nargs="+", help="one or more canonical board CSVs")
    ap.add_argument("--sort", default="breaks,break_rows",
                    help="comma-separated measures, best board first "
                         "(default: breaks,break_rows). Use --sort=-KEY "
                         "(with the '=') to invert one to worst-first.")
    ap.add_argument("--top", type=int, default=0, help="show only the best N rows")
    ap.add_argument("--out", metavar="FILE",
                    help="write the input rows, re-ordered, verbatim to FILE")
    ap.add_argument("--rescore", action="store_true",
                    help="with --out: rewrite each row canonically as "
                         "config_id,score,pos[256],rot[256] with the score "
                         "recomputed from the seed, instead of copying it "
                         "verbatim. Makes `sort -t, -k2,2nr` meaningful.")
    ap.add_argument("--csv", action="store_true",
                    help="print the measures as CSV instead of a table")
    ap.add_argument("--no_id", action="store_true",
                    help="drop the board-id column from the table, which is the "
                         "widest one and the usual reason a row wraps; `row` "
                         "still identifies the board. Ignored by --csv.")
    ap.add_argument("--quiet", action="store_true",
                    help="write --out without printing the human ranking table")
    ap.add_argument("--border_only", action="store_true",
                    help="keep only boards with a fully clean, complete external "
                         "border (border == 60)")
    ap.add_argument("--group_box", metavar="R0:R1,C0:C1",
                    help="before global --top, group by exact piece+spin contents "
                         "of this inclusive rectangle")
    ap.add_argument("--per_group", type=int, default=1, metavar="N",
                    help="with --group_box/--unique, retain the best N from each "
                         "group before global ranking (default 1)")
    ap.add_argument("--unique", action="store_true",
                    help="exact whole-board deduplication; shorthand for grouping "
                         "on 0:15,0:15 with --per_group")
    ap.add_argument("--diverse", type=int, default=0, metavar="K",
                    help="after ranking, keep K boards chosen farthest-first on "
                         "cell agreement -- independent roots to post-process, "
                         "instead of the K siblings the top of a ranking usually "
                         "holds. Adds an `agree` column: each root's highest "
                         "agreement with the roots before it")
    ap.add_argument("--max_agree", type=float, default=None, metavar="P",
                    help="drop any board agreeing with an already-kept board on "
                         "more than fraction P of its placed cells (0..1). A plain "
                         "near-duplicate filter; runs before --diverse")
    ap.add_argument("--diversity_box", metavar="R0:R1,C0:C1",
                    help="compute --max_agree/--diverse only in this rectangle; "
                         "use 0:9,0:15 to demand lower-board diversity")
    ap.add_argument("--diversity_metric", choices=("cells", "fraction"),
                    default="cells", help="farthest-first similarity: raw shared "
                         "cells (default) or fraction of the smaller placement")
    ap.add_argument("--max_mem", type=float, default=8.0, metavar="GB",
                    help="refuse an input projected to need more than this much "
                         "memory (default 8). A plain --top streams; --group_box "
                         "must run first and can still retain many groups")
    ap.add_argument("--seed_file", help="piece seed file (default: data/seed_Edge5.txt)")
    ap.add_argument("--count", action="store_true",
                    help="print just the number of board rows across the inputs "
                         "and exit, one bare number. For scripts: "
                         "N=$(E555_rank.py boards.csv --count). Comment and "
                         "blank lines do not count, and neither the seed nor "
                         "the boards are parsed, so it is instant on a large "
                         "file and works on one no tool has scored yet.")
    ap.add_argument("--field", metavar="NAME",
                    help="print just this measure for the best board and exit, "
                         "one bare number, nothing else. For scripts: "
                         "BEST=$(E555_rank.py boards.csv --field score). Reads "
                         "the real board instead of trusting field 2, which "
                         "Stage B writes its solution index into.")
    ap.add_argument("--split", nargs=4,
                    metavar=("KEY", "N", "AT_LEAST.csv", "BELOW.csv"),
                    help="stream one input file into two verbatim board files: "
                         "KEY >= integer N goes to AT_LEAST.csv and KEY < N "
                         "goes to BELOW.csv. Input order is preserved.")
    args = ap.parse_args()

    if args.split:
        if len(args.inputs) != 1:
            raise SystemExit("[ERROR] --split accepts exactly one input CSV")
        if (args.out or args.rescore or args.csv or args.count or args.field or
                args.border_only or args.group_box or args.unique or args.diverse or
                args.max_agree is not None or args.top):
            raise SystemExit("[ERROR] --split is a standalone mode; do not combine it "
                             "with ranking, grouping, diversity, or --out options")
        key, threshold_text, at_least_path, below_path = args.split
        if key not in SORTABLE:
            if key == "span":
                raise SystemExit("[ERROR] span is text (HxW), not an integer measure; "
                                 "use break_rows, break_cols, or another numeric key")
            raise SystemExit(f"[ERROR] unknown split key '{key}'; choose from: "
                             f"{', '.join(SORTABLE)}")
        try:
            threshold = int(threshold_text, 10)
        except ValueError:
            raise SystemExit(f"[ERROR] split threshold must be an integer, got "
                             f"'{threshold_text}'")
        seed = V.load_seed(V.find_seed(args.seed_file))
        skipped = []
        split_by_threshold(args.inputs[0], seed, key, threshold,
                           at_least_path, below_path, skipped, args.quiet)
        return _status(skipped)

    if args.rescore and not args.out:
        raise SystemExit("[ERROR] --rescore only means something with --out FILE")
    if args.field and args.field not in SORTABLE:
        raise SystemExit(f"[ERROR] unknown measure '{args.field}'; "
                         f"choose from: {', '.join(SORTABLE)}")

    if args.diverse < 0:
        raise SystemExit("[ERROR] --diverse wants a positive count")
    if args.per_group < 1:
        raise SystemExit("[ERROR] --per_group must be >= 1")
    if args.group_box and args.unique:
        raise SystemExit("[ERROR] use either --group_box or --unique, not both")
    if args.max_agree is not None and not 0.0 <= args.max_agree <= 1.0:
        raise SystemExit("[ERROR] --max_agree is a fraction of placed cells, 0..1")

    if args.count:
        n = 0
        for path in args.inputs:
            with open(path, encoding="utf-8", errors="replace") as fh:
                n += sum(1 for line in fh
                         if line.strip() and not line.lstrip()[:1] in "#%")
        print(n)
        return 0

    group_cells = (parse_box("0:15,0:15") if args.unique else
                   parse_box(args.group_box) if args.group_box else None)
    diversity_cells = parse_box(args.diversity_box) if args.diversity_box else None

    check_memory(args.inputs, args, group_cells)
    seed = V.load_seed(V.find_seed(args.seed_file))
    skipped = []
    keys = parse_sort_spec(args.sort)
    stream = read_boards(args.inputs, seed, skipped, progress_every=12500, group_cells=group_cells)
    # Counted as it streams, so --border_only can still report "N of M" without
    # a second pass and without holding the boards it rejects.
    seen = [0]
    def _count(it):
        for r in it:
            seen[0] += 1
            yield r
    stream = _count(stream)
    border_kept = [0]
    if args.border_only:
        def _border(it):
            for r in it:
                if r["border"] == N_BORDER:
                    border_kept[0] += 1
                    yield r
        stream = _border(stream)
    if group_cells is not None:
        stream = iter(collect_grouped(stream, keys, args.per_group))
    # --field is exactly --top 1 with printing suppressed.
    records = collect(stream, keys, 1 if args.field else args.top)
    if args.border_only and seen[0]:
        print(f"[border] {border_kept[0]} of {seen[0]} boards have a complete, "
              f"break-free external border", file=sys.stderr)
    if not records:
        # --field is meant to be captured in a shell variable, so an empty
        # input has to leave that variable empty rather than printing an error
        # into it. An input with no boards at all still fails loudly; a filter
        # that happened to keep none of them does not -- that is an answer.
        if args.field:
            return _status(skipped)
        if seen[0] == 0:
            raise SystemExit("[ERROR] no board rows found in the input")
        if args.out:
            write_emit(args.out, [], args.rescore, args.quiet)
        return _status(skipped)
    if args.field:
        print(records[0][args.field])
        return _status(skipped)
    # Ranking first, then the two dissimilarity passes: quality decides who
    # represents a cluster, dissimilarity decides how many clusters you see.
    if args.max_agree is not None:
        records = filter_max_agree(records, args.max_agree, diversity_cells)
    if args.diverse:
        records = select_diverse(records, args.diverse, diversity_cells,
                                 args.diversity_metric)
    shown = records
    if not shown:
        if args.out:
            write_emit(args.out, [], args.rescore, args.quiet)
        return _status(skipped)

    multi = len(args.inputs) > 1
    if args.csv:
        w = csv.writer(sys.stdout, lineterminator="\n")   # LF, not the csv module's CRLF
        cols = list(COLUMNS) + (["agree"] if args.diverse else [])
        head = (["file"] if multi else []) + ["row", "id"] + cols
        w.writerow(head)
        for r in shown:
            w.writerow(([r["file"]] if multi else []) + [r["row"], r["id"]]
                       + [r[k] for k in cols])
        return _status(skipped)

    if args.quiet:
        if args.out:
            write_emit(args.out, shown, args.rescore, True)
        return _status(skipped)

    order = " then ".join(f"{n}{' (worst first)' if d else ''}" for n, d in keys) \
            or "input order"
    print(f"E555 rank: {len(records)} board(s), sort: {order}")

    # --no_id drops the widest column of all: board ids run to 44 characters and
    # are the main reason a row wraps. `row` still identifies the board, and it
    # is what --row of the other tools wants anyway.
    idw = 0 if args.no_id else min(20, max(len(r["id"]) for r in shown))
    idh = "" if args.no_id else f"{'id':<{idw}}  "
    fw = max((len(r["file"]) for r in shown), default=4) if multi else 0
    cols = SHOWN + (("agree",) if args.diverse else ())
    head = (f"{'file':<{fw}} " if multi else "") + f"{'row':>5}  " + idh + \
           "  ".join(f"{k:>{max(6, len(k))}}" for k in cols)
    print(head)
    print("-" * len(head))
    for r in shown:
        cid = "" if args.no_id else \
              (r["id"] if len(r["id"]) <= idw else r["id"][:idw - 1] + "~").ljust(idw) + "  "
        line = (f"{r['file']:<{fw}} " if multi else "") + f"{r['row']:>5}  " + cid + \
               "  ".join(f"{r[k]:>{max(6, len(k))}}" for k in cols)
        print(line)

    b = shown[0]
    print(f"best: {b['id']}  score={b['score']} placed={b['placed']} "
          f"solid={b['solid']} clean_b={b['clean_b']} span={b['span']}")

    if args.out:
        write_emit(args.out, shown, args.rescore, args.quiet)
    return _status(skipped)


if __name__ == "__main__":
    raise SystemExit(main())
