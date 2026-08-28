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

THE MEASURES  (a "break" is an internal junction whose two cells disagree;
               a junction touching an unplaced cell counts as broken, exactly
               as everywhere else in Stage C)

    breaks      480 - score. Lower is better. Measured and sortable, but NOT
                printed: it is exactly 480 - score, so the table shows score
                alone and you read closeness to a finished board off it.
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

    clean_b     contiguous break-free rows counting up from row 0: rows
    clean_t     0..clean_b-1 hold no break at all. clean_t counts down from
    clean_l     row 15, clean_l right from col 0, clean_r left from col 15.
    clean_r     Higher is better. clean_b is the old "completed rows".

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

CHOOSING INDEPENDENT ROOTS  (--diverse, --max_agree)

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

    Both run AFTER --sort and --top, so quality still decides which board
    represents a cluster and dissimilarity decides how many clusters you see.
    With --top N the choice is made among those N: raise it when the top of
    the ranking turns out to be one lineage.

    On the 15 exact row-12 partials of a whirlpool run, `--diverse 4` returns
    one board from each of the four lineages the pool actually holds (within a
    lineage boards agree on 199-202 of 203 cells, across lineages on 0-14).

MEMORY

    Ranking holds one light record per board -- the input line and its
    measures, about 1.8x the row's size on disk. Measured: 51,000 boards of a
    96 MB CSV peak at 165 MB.

    `--top N` streams instead, keeping only the best N in a bounded heap, so
    peak memory does not depend on the file size at all (14 MB for the same
    input). Use it on anything large; it is the only mode that scales.

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
import argparse, csv, heapq, sys
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

    # One pass over the junctions gives breaks, break cells and corner distance.
    # A cell touched by any break cannot be solid, which is precisely the
    # viewer's rule (an unplaced neighbour leaves the piece unsatisfied).
    breaks = 0
    corner_d = 0
    bad = set()
    rows, cols = set(), set()
    for a, b, da, db in ALL_JUNCTIONS:
        ca, cb = colors.get(a), colors.get(b)
        if ca is not None and cb is not None and ca[da] == cb[db]:
            continue
        breaks += 1
        corner_d += _v(a) + _v(b) + _h(a) + _h(b)
        bad.add(a); bad.add(b)
        rows.add(a // SIDE); rows.add(b // SIDE)
        cols.add(a % SIDE);  cols.add(b % SIDE)

    solid = 0
    for cell, col in colors.items():
        if cell in bad:
            continue
        if all(col[s] == GREY for s in FRAME_SIDES.get(cell, ())):
            solid += 1

    def leading_clean(order, marked):
        """How many border lines, in this order, hold no break before the first that does."""
        n = 0
        for k in order:
            if k in marked:
                break
            n += 1
        return n

    up, down = range(SIDE), range(SIDE - 1, -1, -1)
    clean_b = leading_clean(up, rows)      # rows, bottom upwards
    clean_t = leading_clean(down, rows)    # rows, top downwards
    clean_l = leading_clean(up, cols)      # columns, left to right
    clean_r = leading_clean(down, cols)    # columns, right to left

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

    return dict(breaks=breaks, score=N_EDGES - breaks, solid=solid,
                placed=len(colors), border=border,
                break_rows=len(rows), break_cols=len(cols),
                span=span, clean_b=clean_b, clean_t=clean_t,
                clean_l=clean_l, clean_r=clean_r, corner_d=corner_d,
                clues=n_clues)


# Every measure, in this order; `span` is text, the rest are integers. This is
# what --sort, --field and --csv see.
COLUMNS = ("breaks", "score", "solid", "placed", "border", "break_rows",
           "break_cols", "span", "clean_b", "clean_t", "clean_l", "clean_r",
           "corner_d", "clues")

# What the human table prints. `breaks` is left out because score = 480 - breaks
# EXACTLY, so the pair carried no information the other did not, and score is the
# one that says how close the board is to a finished 480. It stays a measure, so
# --sort breaks, --field breaks and --csv are unaffected.
SHOWN = tuple(k for k in COLUMNS if k != "breaks")

# Which way is "better" for each measure. Sorting is always best-first, so
# --sort solid puts the most solid board on top without any extra syntax; a
# '-' prefix (--sort=-solid) asks for worst-first instead.
HIGH_IS_BETTER = {"score", "solid", "placed", "border", "clues",
                  "clean_b", "clean_t", "clean_l", "clean_r"}
SORTABLE = tuple(k for k in COLUMNS if k != "span")


def fingerprint(line):
    """The board of a stored input line as a set of (cell, piece, spin) triples.

    Two boards agree on a cell when both put the same piece there at the same
    spin, so the agreement is just `len(a & b)` -- one C-speed set intersection
    instead of a 256-step Python loop. Built only for the records --diverse or
    --max_agree actually compare, which is why the record keeps the line rather
    than the parsed arrays: everything else in the file is measured once and
    never looked at again.
    """
    _, _, pos, rot = V.parse_row(next(csv.reader([line])))
    return frozenset(pos[p] * 1024 + p * 4 + rot[p]
                     for p in range(N_PIECES) if pos[p] != 999)


def read_boards(paths, seed, skipped, progress_every=0):
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
                # Canonical id: keep both leading metadata fields when the row
                # had them, so --rescore does not throw away a Stage B solution
                # index while collapsing everything else into one score column.
                canon_id = f"{cid}_{sol}" if sol not in ("", "?") else cid
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
                n += 1
                if progress_every and n % progress_every == 0:
                    print(f"[rank] measured {n} boards", file=sys.stderr)
                yield dict(file=Path(path).name, row=idx, id=cid,
                           canon_id=canon_id, line=line, **m)
                idx += 1


def write_emit(path, records, rescore):
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
    kind = "canonical" if rescore else "verbatim"
    print(f"[emit] {len(records)} {kind} row(s) -> {path}")


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


def _agree_prints(records):
    """Attach a fingerprint to each record, once, and return the list."""
    for r in records:
        if "fp" not in r:
            r["fp"] = fingerprint(r["line"])
    return records


def filter_max_agree(records, frac):
    """Drop boards agreeing with an already-kept board on more than `frac`.

    A plain near-duplicate filter, walked best-first so the board that survives
    a cluster is its best member. The denominator is the smaller placed count,
    so a 96-cell band and a 203-cell partial that share the band are correctly
    read as the same board carried forward, not as 47% different.
    """
    _agree_prints(records)
    kept = []
    for r in records:
        n = max((len(r["fp"] & k["fp"]) / max(1, min(r["placed"], k["placed"]))
                 for k in kept), default=0.0)
        if n <= frac:
            kept.append(r)
    print(f"[max-agree] kept {len(kept)} of {len(records)} boards at agreement "
          f"<= {frac:g}", file=sys.stderr)
    return kept


def select_diverse(records, k):
    """`k` boards spread as far apart as the pool allows, best board first.

    Farthest-first: the first root is the best-ranked board, and each next root
    is the one whose CLOSEST already-chosen root is furthest away. That is the
    practical question -- "give me a few starting points that are not siblings"
    -- answered in k x M comparisons rather than the M^2 of a full matrix, so a
    thousand-board pool costs nothing.

    Each root carries `agree`, its highest cell agreement with the roots before
    it, so the clustering is visible instead of asserted. Root 1 reads 0.
    """
    _agree_prints(records)
    if k >= len(records):
        # Nothing to choose: annotate anyway so the `agree` column is real for
        # every row it is printed beside.
        records[0]["agree"] = 0
        for i, r in enumerate(records[1:], 1):
            r["agree"] = max((len(r["fp"] & q["fp"]) for q in records[:i]), default=0)
        return records
    roots = [records[0]]
    records[0]["agree"] = 0
    rest = records[1:]
    best = [len(r["fp"] & roots[0]["fp"]) for r in rest]   # agreement, high = close
    while len(roots) < k and rest:
        i = min(range(len(rest)), key=lambda j: (best[j], j))
        pick = rest.pop(i)
        pick["agree"] = best.pop(i)
        roots.append(pick)
        for j, r in enumerate(rest):
            a = len(r["fp"] & pick["fp"])
            if a > best[j]:
                best[j] = a
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


def check_memory(paths, args):
    """Refuse a run that would not fit, instead of being OOM-killed halfway.

    Only the unbounded mode can grow with the file: --top N holds N records
    whatever the input is, which is the answer this message points at.
    """
    if args.top > 0:
        return
    size = sum(Path(p).stat().st_size for p in paths)
    want = size * RECORD_OVERHEAD
    rows = count_rows(paths)
    if args.diverse or args.max_agree is not None:
        want += rows * BYTES_PER_PRINT
    limit = args.max_mem * (1 << 30)
    if want > limit:
        raise SystemExit(
            f"[ERROR] {rows:,} boards would need about {want / (1<<30):.2g} GB, over "
            f"the --max_mem limit of {args.max_mem:g} GB.\n"
            f"        Add --top N to stream with bounded memory, or raise --max_mem.")


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
    ap.add_argument("--border_only", action="store_true",
                    help="keep only boards with a fully clean, complete external "
                         "border (border == 60)")
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
    ap.add_argument("--max_mem", type=float, default=8.0, metavar="GB",
                    help="refuse an input projected to need more than this much "
                         "memory (default 8). Only applies without --top, which "
                         "streams in bounded memory whatever the file size")
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
    args = ap.parse_args()
    if args.rescore and not args.out:
        raise SystemExit("[ERROR] --rescore only means something with --out FILE")
    if args.field and args.field not in SORTABLE:
        raise SystemExit(f"[ERROR] unknown measure '{args.field}'; "
                         f"choose from: {', '.join(SORTABLE)}")

    if args.diverse < 0:
        raise SystemExit("[ERROR] --diverse wants a positive count")
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

    check_memory(args.inputs, args)
    seed = V.load_seed(V.find_seed(args.seed_file))
    skipped = []
    keys = parse_sort_spec(args.sort)
    stream = read_boards(args.inputs, seed, skipped, progress_every=50000)
    # Counted as it streams, so --border_only can still report "N of M" without
    # a second pass and without holding the boards it rejects.
    seen = [0]
    def _count(it):
        for r in it:
            seen[0] += 1
            yield r
    stream = _count(stream)
    if args.border_only:
        stream = (r for r in stream if r["border"] == N_BORDER)
    # --field wants the single best board and returns before anything is
    # written, so it is exactly --top 1 with the printing left out; asking the
    # heap for it keeps that path bounded too.
    records = collect(stream, keys, 1 if args.field else args.top)
    if args.border_only and seen[0]:
        print(f"[border] {len(records)} of {seen[0]} boards have a complete, "
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
            write_emit(args.out, [], args.rescore)
        return _status(skipped)
    if args.field:
        print(records[0][args.field])
        return _status(skipped)
    # Ranking first, then the two dissimilarity passes: quality decides who
    # represents a cluster, dissimilarity decides how many clusters you see.
    if args.max_agree is not None:
        records = filter_max_agree(records, args.max_agree)
    if args.diverse:
        records = select_diverse(records, args.diverse)
    shown = records
    if not shown:
        if args.out:
            write_emit(args.out, [], args.rescore)
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

    order = " then ".join(f"{n}{' (worst first)' if d else ''}" for n, d in keys) \
            or "input order"
    print(f"\n=== E555 rank ===  {len(records)} boards from "
          f"{len(args.inputs)} file(s), sorted by {order}\n")

    # --no_id drops the widest column of all: board ids run to 44 characters and
    # are the main reason a row wraps. `row` still identifies the board, and it
    # is what --row of the other tools wants anyway.
    idw = 0 if args.no_id else min(44, max(len(r["id"]) for r in shown))
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
    print(f"\n[best] {b['id']}  breaks={b['breaks']} in {b['break_rows']} row(s) / "
          f"{b['break_cols']} col(s), span {b['span']}, solid {b['solid']}/256, "
          f"clean rows {b['clean_b']} from the bottom, {b['clean_t']} from the top")
    print("[note] lower is better: breaks break_rows break_cols corner_d   |   "
          "higher is better: score solid placed border clues clean_*")

    if args.out:
        write_emit(args.out, shown, args.rescore)
    return _status(skipped)


if __name__ == "__main__":
    raise SystemExit(main())
