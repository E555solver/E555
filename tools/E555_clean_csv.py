#!/usr/bin/env python3
"""
E555_clean_csv.py -- drop boards that are an earlier board with one frontier
piece swapped.

WHY

    Stage B and the finalizer emit many boards that are identical below the
    frontier and differ only in the single piece sitting on it. They are
    distinct rows, so the file looks rich, but the moment a later stage frees
    the top -- a --holes mask, the finalizer's lock line, the roundhouse's
    strip -- they collapse into the same search. The file is large and its row
    count overstates how much it actually holds.

    This removes them. A board is dropped when it matches a board already kept
    everywhere except at ONE cell whose top face is exposed, meaning the cell
    directly above it is empty. Exact duplicates go too.

    Boards that differ at a cell buried inside the board are NOT touched: those
    are genuinely different arrangements. Only the frontier collapses.

WHAT COUNTS AS THE SAME

    Two placements match when the same piece sits in the same cell at the same
    rotation, so no seed is needed and no colour is read. The swapped piece may
    or may not happen to show the same top colour -- either way the two rows
    are one board plus one interchangeable frontier piece, and free the top row
    and they become the same board with the same pool.

    Cells in row 15 are never frontier cells: nothing sits above them, so their
    top face is the frame, not an exposed colour. A complete board therefore
    has no frontier at all, and only exact duplicates are dropped.

    Rows are copied out byte for byte. This tool only ever removes rows, it
    never rewrites one. Comments and headers pass through.

USAGE

    python3 E555_clean_csv.py boards.csv              # -> boards_clean.csv
    python3 E555_clean_csv.py boards.csv --out /tmp/small.csv

    The output name is the input with _clean before the extension unless --out
    overrides it.
"""
from __future__ import annotations
import argparse, csv, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import E555_viewer as V                      # row parsing only

SIDE, N_PIECES, UNPLACED = V.SIDE, V.N_PIECES, V.UNPLACED


def placements(pos, rot):
    """cell -> (piece, rotation) for every placed piece."""
    return {pos[p]: (p, rot[p]) for p in range(N_PIECES) if pos[p] != UNPLACED}


def cell_hash(cell, piece, spin):
    """What one placed cell contributes to its board's hash."""
    return hash((cell, piece, spin))


def board_hash(place):
    """Order-independent identity of a whole board, as one integer."""
    h = 0
    for cell, (piece, spin) in place.items():
        h ^= cell_hash(cell, piece, spin)
    return h


def frontier_cells(place):
    """Placed cells with an exposed top face: the cell above them is empty."""
    return [c for c in place
            if c // SIDE < SIDE - 1 and (c + SIDE) not in place]


def clean_file(path, out_path):
    """Copy `path` minus its near-duplicates; return (kept, same, twin, other)."""
    seen_boards = set()                # identities of the boards kept so far
    seen_blanked = set()               # ... each with one frontier cell removed
    kept = same = twin = other = 0

    with open(path, newline="") as fh, open(out_path, "w", newline="") as out:
        writer = csv.writer(out, lineterminator="\n")
        for raw in csv.reader(fh):
            rec = V.parse_row([f.strip() for f in raw])
            if rec is None:                # comment, header, short row
                writer.writerow(raw)
                other += 1
                continue
            _cid, _sol, pos, rot = rec
            place = placements(pos, rot)
            if len(place) != sum(1 for p in pos if p != UNPLACED):
                print(f"[warn] row {kept + same + twin}: two pieces claim one "
                      "cell; kept without comparing", file=sys.stderr)
                writer.writerow(raw)
                kept += 1
                continue

            full = board_hash(place)
            if full in seen_boards:
                same += 1
                continue
            # Blanking cell c gives the same value for two boards that agree
            # everywhere but c, which is exactly the match we are looking for.
            blanked = [full ^ cell_hash(c, *place[c]) for c in frontier_cells(place)]
            if any(b in seen_blanked for b in blanked):
                twin += 1
                continue

            seen_boards.add(full)
            seen_blanked.update(blanked)
            writer.writerow(raw)
            kept += 1
    return kept, same, twin, other


def main():
    ap = argparse.ArgumentParser(
        description="Drop boards that repeat an earlier board with one "
                    "frontier piece swapped.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="board CSV")
    ap.add_argument("--out", metavar="FILE",
                    help="output path (default: the input with _clean appended)")
    args = ap.parse_args()

    src = Path(args.input)
    if not src.exists():
        raise SystemExit(f"[ERROR] input '{src}' not found")
    dst = Path(args.out) if args.out else src.with_name(f"{src.stem}_clean{src.suffix}")
    if dst.resolve() == src.resolve():
        raise SystemExit(f"[ERROR] refusing to overwrite the input '{src}'")

    kept, same, twin, other = clean_file(src, dst)
    read = kept + same + twin
    if not read:
        raise SystemExit(f"[ERROR] no board rows found in {src}")

    print(f"[clean] {src.name}: {read} board row(s) read, {kept} kept"
          + (f", {other} other row(s) passed through" if other else ""))
    print(f"[clean] dropped {same + twin}: {twin} frontier twin(s), "
          f"{same} exact duplicate(s)")
    print(f"[out] {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
