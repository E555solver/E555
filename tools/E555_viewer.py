#!/usr/bin/env python3
"""
E555_viewer.py -- view (or diff) Eternity II boards from a solutions CSV.

For a chosen row of a partial/solution CSV this prints:

  1. an ASCII picture of the 16x16 board (piece id : rotation in each cell),
     with placement and edge-match statistics (including correct edges / 480
     and fully-connected "solid" pieces / 256), and
  2. a ready-to-open URL for the e2.bucas.name web viewer.

With --diff it instead compares two rows of the same CSV.

------------------------------------------------------------------------
CSV FORMAT (the whole "tailsolver" stack is accepted)
------------------------------------------------------------------------
Every row carries, at its END, the 256 piece positions followed by the
256 piece rotations:

        ... <meta fields> ... , pos[0..255] , rot[0..255]

so a row has 512 trailing numbers plus a few leading id/meta fields:

  * beam / partial input : config_id , sol_id ,                pos , rot   (514)
  * tailsolver output    : config_id_solid , correct_edges ,   pos , rot   (514)
  * older output variant : correct_edges , config_id_solid ,   pos , rot   (514)
  * any of the above with one extra leading meta integer                   (515)

Because we read pos/rot from the end, all of these parse correctly no
matter what the leading columns mean. `pos[p]` is the board cell of
piece p (0-based, 999 = unplaced); `rot[p]` is its rotation 0..3.

Board convention (same as seed_Edge5.txt and the solver):
  cell = row*16 + col ; row 0 = BOTTOM, row 15 = TOP ; col 0 = LEFT.
  A piece's seed line lists its edges as  N E S W  (top right bottom left);
  rotation `spin` sends side d to seed side (d+spin) mod 4.

------------------------------------------------------------------------
USAGE
------------------------------------------------------------------------
  python3 E555_viewer.py SOLUTIONS.csv                 # row 0, board + URL
  python3 E555_viewer.py SOLUTIONS.csv --row 5         # the 6th data row
  python3 E555_viewer.py SOLUTIONS.csv --diff 3 7      # compare rows 3 and 7
  python3 E555_viewer.py SOLUTIONS.csv --name my_try   # label in board + URL
  python3 E555_viewer.py SOLUTIONS.csv --all           # just a URL per row
  python3 E555_viewer.py SOLUTIONS.csv --no-url        # ASCII board only

The seed is taken from --seed PATH; without the option the viewer tries
./seed_Edge5.txt in the current directory, then data/seed_Edge5.txt next to
this repository's tools/ directory.
"""

import argparse
import csv
from pathlib import Path
from urllib.parse import quote

SIDE = 16
N_PIECES = SIDE * SIDE          # 256
N_TRAILING = 2 * N_PIECES       # 512 = pos[256] + rot[256]
N_EDGES = 2 * SIDE * (SIDE - 1) # 480 internal edges (240 H + 240 V)
UNPLACED = 999

# Fallback seed locations tried in order when --seed is not given: the
# current directory first (old behaviour), then the repository's data/ file
# resolved relative to this script, so the tool works from any cwd.
SEED_FALLBACKS = (
    Path("seed_Edge5.txt"),
    Path(__file__).resolve().parent.parent / "data" / "seed_Edge5.txt",
)


# -- seed ----------------------------------------------------------------

def find_seed(explicit):
    """Resolve the seed path: --seed wins, else the first existing fallback."""
    if explicit:
        p = Path(explicit)
        if not p.exists():
            raise SystemExit(f"Seed file '{explicit}' not found")
        return p
    for p in SEED_FALLBACKS:
        if p.exists():
            return p
    raise SystemExit("No seed file found: pass --seed PATH "
                     f"(tried {', '.join(str(p) for p in SEED_FALLBACKS)})")


def load_seed(path):
    """Return a list of 256 (N, E, S, W) edge-colour tuples, piece p on line p."""
    p = Path(path)
    if not p.exists():
        raise SystemExit(f"Seed file '{path}' not found")
    pieces = []
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        vals = tuple(int(x) for x in line.split())
        if len(vals) != 4:
            raise ValueError(f"Bad seed line (need 4 ints): {line!r}")
        pieces.append(vals)
    if len(pieces) != N_PIECES:
        raise ValueError(f"Seed must list {N_PIECES} pieces, got {len(pieces)}")
    return pieces


def rotate_edges(edges, spin):
    """Edges (N,E,S,W) of a piece after rotating by `spin` quarter-turns."""
    return tuple(edges[(d + spin) & 3] for d in range(4))


# -- CSV reading (positions/rotations anchored at the end of the row) ---------

def parse_row(fields):
    """
    Turn one CSV row into (config_id, sol_id, pos, rot), or None if the row is
    a comment / header / too short to hold a board.
    """
    fields = [f.strip() for f in fields]
    if not fields or not fields[0] or fields[0].startswith("#"):
        return None
    if len(fields) < N_TRAILING:
        return None

    tail = fields[-N_TRAILING:]
    try:
        pos = [int(x) for x in tail[:N_PIECES]]
        rot = [int(x) for x in tail[N_PIECES:]]
    except ValueError:
        return None  # header row such as "...,pos_0,pos_1,..."

    meta = fields[:-N_TRAILING]
    config_id = meta[0] if len(meta) >= 1 else "?"
    sol_id = meta[1] if len(meta) >= 2 else "?"
    return config_id, sol_id, pos, rot


def iter_records(path):
    """Yield (data_row_index, config_id, sol_id, pos, rot) for each board row."""
    with Path(path).open(newline="") as f:
        idx = 0
        for raw in csv.reader(f):
            rec = parse_row(raw)
            if rec is None:
                continue
            yield (idx, *rec)
            idx += 1


def read_record(path, want_row):
    """Return the single record at 0-indexed data row `want_row`."""
    for idx, config_id, sol_id, pos, rot in iter_records(path):
        if idx == want_row:
            return config_id, sol_id, pos, rot
        if idx > want_row:
            break
    raise SystemExit(f"Row {want_row} not found in {path}")


def build_board(pos, rot):
    """Return board[row][col] = (piece_id, spin) or None, from pos/rot arrays."""
    board = [[None] * SIDE for _ in range(SIDE)]
    for pid in range(N_PIECES):
        cell = pos[pid]
        if cell == UNPLACED:
            continue
        if not (0 <= cell < N_PIECES):
            raise ValueError(f"Piece {pid} has invalid pos={cell}")
        r, c = divmod(cell, SIDE)
        if board[r][c] is not None:
            raise ValueError(f"Two pieces claim cell {cell} (row {r}, col {c})")
        if not (0 <= rot[pid] <= 3):
            raise ValueError(f"Piece {pid} has invalid rot={rot[pid]}")
        board[r][c] = (pid, rot[pid])
    return board


# -- statistics  ----------------------------------------------------------------

def piece_is_solid(board, seed, r, c):
    """True if the piece at (r,c) has all 4 sides satisfied: interior sides match
    the placed neighbour, exterior (frame) sides are grey (0). Unplaced neighbours
    leave the piece unsatisfied."""
    cell = board[r][c]
    if cell is None:
        return False
    n, e, s, w = rotate_edges(seed[cell[0]], cell[1])   # N, E, S, W
    # top side
    if r == SIDE - 1:
        if n != 0: return False
    else:
        nb = board[r + 1][c]
        if nb is None or rotate_edges(seed[nb[0]], nb[1])[2] != n: return False
    # right side
    if c == SIDE - 1:
        if e != 0: return False
    else:
        nb = board[r][c + 1]
        if nb is None or rotate_edges(seed[nb[0]], nb[1])[3] != e: return False
    # bottom side
    if r == 0:
        if s != 0: return False
    else:
        nb = board[r - 1][c]
        if nb is None or rotate_edges(seed[nb[0]], nb[1])[0] != s: return False
    # left side
    if c == 0:
        if w != 0: return False
    else:
        nb = board[r][c - 1]
        if nb is None or rotate_edges(seed[nb[0]], nb[1])[1] != w: return False
    return True


def board_stats(board, seed):
    """Counts of placed pieces, filled rows, and (if seed given) edge matches,
    correct edges out of 480, and solid (fully-connected) pieces out of 256."""
    placed = [(r, c, *board[r][c])
              for r in range(SIDE) for c in range(SIDE) if board[r][c] is not None]
    placed_ids = {pid for _, _, pid, _ in placed}
    unplaced = sorted(pid + 1 for pid in range(N_PIECES) if pid not in placed_ids)

    row_counts = [sum(1 for c in range(SIDE) if board[r][c] is not None)
                  for r in range(SIDE)]
    full_rows = [r for r in range(SIDE) if row_counts[r] == SIDE]
    partial_rows = [(r, row_counts[r]) for r in range(SIDE) if 0 < row_counts[r] < SIDE]

    stats = dict(n_placed=len(placed), unplaced=unplaced,
                 full_rows=full_rows, partial_rows=partial_rows)
    if seed is None:
        return stats

    h_ok = h_tot = v_ok = v_tot = border_ok = border_bad = 0
    for r, c, pid, spin in placed:
        n, e, s, w = rotate_edges(seed[pid], spin)
        # right neighbour: this E vs neighbour's W
        if c < SIDE - 1 and board[r][c + 1] is not None:
            pid2, sp2 = board[r][c + 1]
            h_tot += 1
            if e == rotate_edges(seed[pid2], sp2)[3]:
                h_ok += 1
        # top neighbour: this N vs neighbour's S
        if r < SIDE - 1 and board[r + 1][c] is not None:
            pid2, sp2 = board[r + 1][c]
            v_tot += 1
            if n == rotate_edges(seed[pid2], sp2)[2]:
                v_ok += 1
        # frame: the outward side of a border cell should be grey (0)
        for side_colour, on_border in ((s, r == 0), (n, r == SIDE - 1),
                                       (w, c == 0), (e, c == SIDE - 1)):
            if on_border:
                if side_colour == 0:
                    border_ok += 1
                else:
                    border_bad += 1

    solid = sum(1 for r, c, _, _ in placed if piece_is_solid(board, seed, r, c))

    stats.update(h_ok=h_ok, h_tot=h_tot, v_ok=v_ok, v_tot=v_tot,
                 correct_edges=h_ok + v_ok, solid=solid,
                 border_ok=border_ok, border_bad=border_bad)
    return stats


# -- display ----------------------------------------------------------------

def h_mismatch(board, seed, r, c):
    """True if cells (r,c-1) and (r,c) are both placed and their shared
    (left.E vs right.W) edge colours differ -- a horizontal junction mismatch."""
    left, right = board[r][c - 1], board[r][c]
    if left is None or right is None:
        return False
    return rotate_edges(seed[left[0]], left[1])[1] != rotate_edges(seed[right[0]], right[1])[3]


def v_mismatch(board, seed, r, c):
    """True if cells (r,c) and (r-1,c) are both placed and their shared
    (upper.S vs lower.N) edge colours differ -- a vertical junction mismatch."""
    upper, lower = board[r][c], board[r - 1][c]
    if upper is None or lower is None:
        return False
    return rotate_edges(seed[upper[0]], upper[1])[2] != rotate_edges(seed[lower[0]], lower[1])[0]


def print_board(board, seed):
    """Print the board as a grid, top row first, each cell `pid+1:rot`.

    Junctions where the two placed tiles disagree on the shared edge colour are
    drawn with `#` instead of the usual `-` (vertical) or `|` (horizontal)."""
    indent = "        "
    outer_rule = indent + "+-----" * SIDE + "+"   # outer border: no junctions
    print()
    print(indent + "".join(f" ({c + 1:2d}) " for c in range(SIDE)))
    print(outer_rule)
    for r in range(SIDE - 1, -1, -1):
        # cell line: each cell preceded by its left edge (`|`, or `#` if that
        # horizontal junction is a mismatch); outer left/right borders stay `|`.
        cells = ""
        for c in range(SIDE):
            sep = "#" if c > 0 and h_mismatch(board, seed, r, c) else "|"
            text = "  ???" if board[r][c] is None else f"{board[r][c][0] + 1:3d}:{board[r][c][1]}"
            cells += sep + text
        print(f"  ({r + 1:2d})  {cells}|")
        # rule below this row = the vertical junctions with the row beneath it.
        if r == 0:
            print(outer_rule)
        else:
            print(indent + "".join("+" + ("#####" if v_mismatch(board, seed, r, c) else "-----")
                                   for c in range(SIDE)) + "+")
    print()


def print_info(name, row_idx, stats):
    """Print the header/statistics block above the board."""
    W = 62
    print("=" * W)
    print(f"  {name}   (CSV data row {row_idx})")
    print("-" * W)
    n_pl = stats["n_placed"]
    print(f"  Placed cells  : {n_pl:3d} / {N_PIECES}   ({n_pl * 100 / N_PIECES:.1f}%)")
    if stats["partial_rows"]:
        print("  Partial rows  : "
              + "  ".join(f"row{r + 1}:{n}" for r, n in stats["partial_rows"]))
    if stats["unplaced"]:
        ids = stats["unplaced"]
        shown = " ".join(str(p) for p in ids[:20])
        more = f"  ...+{len(ids) - 20} more" if len(ids) > 20 else ""
        print(f"  Unplaced IDs  : {shown}{more}")

    if "correct_edges" in stats:
        ok = stats["correct_edges"]
        print(f"  Correct edges : {ok:3d} / {N_EDGES}   ({ok * 100 / N_EDGES:.1f}%)")
        sp = stats["solid"]
        print(f"  Solid pieces  : {sp:3d} / {N_PIECES}   ({sp * 100 / N_PIECES:.1f}%)")
        if stats["border_bad"]:
            print(f"  Border check  : {stats['border_bad']} frame VIOLATIONS")
    else:
        print("  (seed not found -> no edge statistics)")
    print("=" * W)


# -- diff -----------------------------------------------------------------------

def stat_line(tag, cid, board, seed):
    """One-line summary of a board for the diff header."""
    s = board_stats(board, seed)
    return (f"  {tag} [{cid}] : {s['n_placed']:3d} placed, "
            f"{s['correct_edges']:3d}/{N_EDGES} edges, "
            f"{s['solid']:3d}/{N_PIECES} solid")


def print_diff(boardA, boardB, seed, ia, ib, recA, recB):
    """Compare two boards cell by cell and print an overlay grid + a list."""
    W = 62
    print("=" * W)
    print(f"  DIFF  row {ia} vs row {ib}")
    print("-" * W)
    print(stat_line(f"row {ia}", recA[0], boardA, seed))
    print(stat_line(f"row {ib}", recB[0], boardB, seed))

    diffs = [(r, c, boardA[r][c], boardB[r][c])
             for r in range(SIDE) for c in range(SIDE)
             if boardA[r][c] != boardB[r][c]]

    changed = added = removed = rot_only = 0
    for _, _, a, b in diffs:
        if a is None:
            added += 1
        elif b is None:
            removed += 1
        elif a[0] != b[0]:
            changed += 1
        else:
            rot_only += 1

    # identical bottom prefix: highest N with rows 0..N all identical
    agree = -1
    for r in range(SIDE):
        if all(boardA[r][c] == boardB[r][c] for c in range(SIDE)):
            agree = r
        else:
            break

    print("-" * W)
    if not diffs:
        print("  Boards are identical.")
        print("=" * W)
        return
    print(f"  {len(diffs)} cells differ: "
          f"piece-changed {changed}, rotation-only {rot_only}, "
          f"added-in-B {added}, only-in-A {removed}")
    print(f"  identical through row {agree + 1}" if agree >= 0
          else "  differ from row 1 up")
    print("=" * W)

    # overlay grid: '.' identical, '*' changed, '+' added in B, '-' only in A
    indent = "        "
    rule = indent + "+-----" * SIDE + "+"
    print()
    print(indent + "".join(f" ({c + 1:2d}) " for c in range(SIDE)))
    print(rule)
    for r in range(SIDE - 1, -1, -1):
        cells = ""
        for c in range(SIDE):
            a, b = boardA[r][c], boardB[r][c]
            if a == b:
                text = "  .  " if a is None else f" {a[0] + 1:3d} "
            elif a is None:
                text = f"+{b[0] + 1:3d} "
            elif b is None:
                text = f"-{a[0] + 1:3d} "
            else:
                text = f"*{a[0] + 1:3d} "
            cells += "|" + text
        print(f"  ({r + 1:2d})  {cells}|")
        print(rule if r == 0 else indent + ("+-----" * SIDE) + "+")
    print("  legend: . identical   * changed   + added in B   - only in A")
    print()

    # per-cell list
    def fmt(cell):
        return "----" if cell is None else f"{cell[0] + 1}:{cell[1]}"
    for r, c, a, b in diffs:
        print(f"  (row {r + 1:2d}, col {c + 1:2d})   A={fmt(a):>7}   B={fmt(b):>7}")
    print()


# -- bucas URL ----------------------------------------------------------------

def make_bucas_url(seed, name, board):
    """Build an e2.bucas.name viewer URL for the given board.

    Only board_edges is sent. A board_custom override was tried before but it
    suppresses bucas's own colouring (the board renders dark), so it is omitted.
    """
    edges = []
    for y in range(SIDE):              # bucas lists cells top row first
        r = SIDE - 1 - y
        for c in range(SIDE):
            cell = board[r][c]
            if cell is None:
                edges.append("aaaa")   # empty placeholder
                continue
            pid, spin = cell
            n, e, s, w = rotate_edges(seed[pid], spin)
            edges.append("".join(chr(ord("a") + col) for col in (n, e, s, w)))

    params = [
        f"puzzle={quote(name)}",
        "board_w=16",
        "board_h=16",
        "board_edges=" + "".join(edges),
    ]
    return "https://e2.bucas.name/#" + "&".join(params)


# -- main  ----------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="solutions/partials CSV (tailsolver stack)")
    ap.add_argument("--seed", default=None, metavar="PATH",
                    help="piece seed file (default: ./seed_Edge5.txt, then the "
                         "repository's data/seed_Edge5.txt)")
    ap.add_argument("--row", type=int, default=0,
                    help="0-indexed data row to view (default 0)")
    ap.add_argument("--diff", type=int, nargs=2, metavar=("A", "B"),
                    help="compare data rows A and B of the CSV")
    ap.add_argument("--name", default=None,
                    help="label for the board title and URL (default: from CSV)")
    ap.add_argument("--all", action="store_true",
                    help="print one URL per row and exit (no ASCII board)")
    ap.add_argument("--no-board", action="store_true", help="skip the ASCII board")
    ap.add_argument("--no-url", action="store_true", help="skip the bucas URL")
    args = ap.parse_args(argv)

    seed = load_seed(find_seed(args.seed))

    if args.diff:
        ia, ib = args.diff
        recA = read_record(args.csv, ia)
        recB = read_record(args.csv, ib)
        boardA = build_board(recA[2], recA[3])
        boardB = build_board(recB[2], recB[3])
        print_diff(boardA, boardB, seed, ia, ib, recA, recB)
        return

    if args.all:
        for idx, cid, sid, pos, rot in iter_records(args.csv):
            name = args.name or f"{cid}_{sid}"
            print(f"# row {idx}: {name}")
            print(make_bucas_url(seed, name, build_board(pos, rot)))
        return

    cid, sid, pos, rot = read_record(args.csv, args.row)
    name = args.name or f"{cid}_{sid}"
    board = build_board(pos, rot)

    if not args.no_board:
        print_info(name, args.row, board_stats(board, seed))
        print_board(board, seed)

    if not args.no_url:
        print(f"# {name}")
        print(make_bucas_url(seed, name, board))


if __name__ == "__main__":
    main()
