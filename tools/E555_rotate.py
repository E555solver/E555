#!/usr/bin/env python3
"""
E555_rotate.py -- turn every board in a CSV by a quarter-turn multiple.

WHY

    A board's open cells, and the breaks around them, sit wherever the search
    that produced them left off -- in practice all bunched against one edge.
    Everything downstream is direction-biased: the finalizer frees rows from
    the top down, the roundhouse grows its strip against one border, the
    topper herds breaks toward the nearest corner, and the backtracker's cell
    orders start from a fixed corner. A region that is awkward to attack from
    the top may be easy from the left.

    Rotating is free and lossless. The frame rule is the same on all four
    sides (every outward border side must be grey), the board is square, and
    the piece set never changes -- so a rotated board is the same board seen
    from a different corner. What moves is WHICH rows and columns hold the
    open cells, and therefore which direction the next stage gets to eat them
    from.

    Run the same board at 0/1/2/3 and hand all four to the next stage.

WHAT A TURN PRESERVES  (measured with tools/E555_rank.py)

    identical    breaks, score, solid, placed, corner_d
    transposed   span ("HxW" becomes "WxH"), and break_rows / break_cols swap
    rotated      one clockwise turn sends clean_b -> clean_l -> clean_t ->
                 clean_r -> clean_b, following the board
    different    border, and only it. Its walk starts at a fixed corner, so an
                 incomplete frame reports a different arc from each of the
                 four starting points -- 28 / 13 / 0 / 0 on
                 data/board_partial_row12.csv. A complete, break-free frame
                 gives 60 whichever way you turn it.

GEOMETRY  (n quarter-turns CLOCKWISE, viewed the way E555_viewer prints:
           row 0 at the BOTTOM, col 0 at the LEFT)

    cell    one turn sends (r, c) -> (SIDE-1-c, r), applied n times: the
            bottom-left corner goes to the top-left, top-left to top-right,
            and so on round.

    spin    turning a tile clockwise moves its north face to the east, so the
            colour now shown on side d is the one that used to be on side d-1.
            With the seed convention shown[d] = seed[(d + spin) % 4] that makes
            the new spin (spin + 3n) % 4.

    This is the same convention as `bin/E555_roundhouse --rotate K`, which
    turns the board internally by exactly this map, so the two agree on what
    "one turn clockwise" means.

    Unplaced pieces (pos == 999) keep both fields untouched; their rotation is
    meaningless and rewriting it would only obscure diffs.

    Rows that are not boards -- comments, headers -- pass through verbatim, and
    the leading meta fields of every board row are copied unchanged, so the id
    of a rotated row still names the board it came from.

VERIFICATION

    Every row's matched-edge count is recomputed after the turn and compared
    with the count before it. They must agree -- a rotation that changes the
    score is a bug, not a result -- and any row where they differ is reported
    loudly and makes the run exit non-zero. The seed is used for nothing else,
    so passing the wrong one weakens the check but cannot corrupt the output.

MASKS

    A --holes mask is a separate 16x16 grid, so a turned board needs a turned
    mask or the next stage opens the wrong region. Pass `--holes IN.csv` and
    the mask is turned by the same map and written alongside the board.

CAVEATS

    Anything that pins a specific cell -- a clue piece, a corner fixed with
    --BL/--BR/--TL/--TR -- moves with the board, and that stage has to be told
    the new position.

USAGE

    python3 E555_rotate.py best_463.csv 1       # 90 deg CW  -> best_463_rot1.csv
    python3 E555_rotate.py best_463.csv 2       # 180 deg    -> best_463_rot2.csv
    python3 E555_rotate.py best_463.csv 3       # 270 deg CW -> best_463_rot3.csv
    python3 E555_rotate.py best_463.csv 0       # copy, no rotation
    python3 E555_rotate.py best_463.csv 1 --out /tmp/left.csv

    N is 0..4; 0 and 4 both mean no rotation. The output name is the input
    with _rotN before the extension unless --out overrides it.

    python3 E555_rotate.py best_463.csv --all   # all four at once

    --all writes _rot0.._rot3 in one pass -- the "hand all four to the next
    stage" workflow above, without four commands. It takes no N, and no --out
    or --holes-out, because it names four files.

    python3 E555_rotate.py board.csv 1 --holes data/holes_open_border_TR.csv

    turns the mask with the board, to board_rot1.csv and
    holes_open_border_TR_rot1.csv. Works with --all too.
"""
from __future__ import annotations
import argparse, csv, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import E555_viewer as V                      # seed loading, row parsing, board build

SIDE, N_PIECES, N_TRAILING, UNPLACED = V.SIDE, V.N_PIECES, V.N_TRAILING, V.UNPLACED


def rotate_cell(cell, n):
    """Where `cell` lands after n quarter-turns clockwise."""
    r, c = divmod(cell, SIDE)
    for _ in range(n):
        r, c = SIDE - 1 - c, r
    return r * SIDE + c


def rotate_board(pos, rot, n):
    """Turn a whole board: new pos/rot arrays, unplaced pieces left alone."""
    new_pos = [p if p == UNPLACED else rotate_cell(p, n) for p in pos]
    new_rot = [r if p == UNPLACED else (r + 3 * n) % 4 for p, r in zip(pos, rot)]
    return new_pos, new_rot


def score(pos, rot, seed):
    """Matched internal edges, 0..480 -- the invariant a turn must preserve."""
    return V.board_stats(V.build_board(pos, rot), seed)["correct_edges"]


def rotate_file(path, n, out_path, seed):
    """Write the rotated copy of `path`; return (board rows, other rows, bad rows)."""
    idx = boards = others = bad = 0        # idx counts board rows read, good or not
    with open(path, newline="") as fh, open(out_path, "w", newline="") as out:
        writer = csv.writer(out, lineterminator="\n")   # LF, not the csv module's CRLF
        for raw in csv.reader(fh):
            fields = [f.strip() for f in raw]
            rec = V.parse_row(fields)
            if rec is None:                    # comment, header, short row
                writer.writerow(raw)
                others += 1
                continue
            cid, _sol, pos, rot = rec
            idx += 1
            new_pos, new_rot = rotate_board(pos, rot, n)
            # build_board rejects a board that puts two pieces on one cell, or a
            # piece outside 0..255; such a row is broken input, so name it and
            # drop it rather than writing out a rotated copy of the damage.
            try:
                before, after = score(pos, rot, seed), score(new_pos, new_rot, seed)
            except ValueError as exc:
                print(f"[ERROR] row {idx - 1} ({cid}): {exc} -- row dropped",
                      file=sys.stderr)
                bad += 1
                continue
            if before != after:
                print(f"[ERROR] row {idx - 1} ({cid}): score changed {before} -> "
                      f"{after} under the turn -- row dropped", file=sys.stderr)
                bad += 1
                continue
            meta = fields[:-N_TRAILING]        # leading fields carried through
            writer.writerow(meta + [str(v) for v in new_pos + new_rot])
            boards += 1
    return boards, others, bad


def rotate_holes(path, n, out_path):
    """Turn a 16x16 --holes mask by the same map; return its open-cell count."""
    grid, comments = [], []
    for line in Path(path).read_text().splitlines():
        s = line.strip()
        if not s or s[0] in "#%":
            comments.append(line.rstrip())
            continue
        row = [int(x) for x in s.replace(",", " ").split()]
        if len(row) != SIDE:
            raise SystemExit(f"[ERROR] {path}: mask row of {len(row)} entries, "
                             f"need {SIDE}")
        grid.append(row)
    if len(grid) != SIDE:
        raise SystemExit(f"[ERROR] {path}: mask of {len(grid)} rows, need {SIDE}")

    new = [[0] * SIDE for _ in range(SIDE)]
    for r in range(SIDE):
        for c in range(SIDE):
            nr, nc = divmod(rotate_cell(r * SIDE + c, n), SIDE)
            new[nr][nc] = grid[r][c]

    label = "no rotation" if n == 0 else f"{n * 90} degrees clockwise"
    with open(out_path, "w") as out:
        # The source's own comments are kept, but they describe the board BEFORE
        # the turn ("open the TOP border" names a different side afterwards), so
        # the generated line goes first to say what happened.
        out.write(f"# {label} from {Path(path).name} by E555_rotate.py\n")
        for line in comments:
            out.write(line + "\n")
        for row in new:                        # groups of four, as the fixtures
            out.write(", ".join(",".join(str(v) for v in row[i:i + 4])
                                for i in range(0, SIDE, 4)) + "\n")
    return sum(sum(row) for row in new)


def turned_name(src, n):
    """The default output path for `src` under n quarter-turns: FILE_rotN.ext."""
    return src.with_name(f"{src.stem}_rot{n}{src.suffix}")


def main():
    ap = argparse.ArgumentParser(
        description="Rotate every board of an Eternity II CSV by a multiple of "
                    "90 degrees clockwise.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="0 or 4 = no rotation, 1 = 90 CW, 2 = 180, 3 = 270 CW")
    ap.add_argument("input", help="canonical board CSV")
    ap.add_argument("n", type=int, choices=[0, 1, 2, 3, 4], metavar="N",
                    nargs="?",
                    help="quarter-turns clockwise: 0 or 4 = none, 1 = 90, "
                         "2 = 180, 3 = 270. Omit it with --all.")
    ap.add_argument("--all", action="store_true",
                    help="write all four turns at once (_rot0 .. _rot3); takes "
                         "no N, --out or --holes-out")
    ap.add_argument("--out", metavar="FILE",
                    help="output path (default: the input with _rotN appended)")
    ap.add_argument("--holes", metavar="FILE",
                    help="a 16x16 --holes mask to turn with the board, so the "
                         "next stage opens the same cells it did before")
    ap.add_argument("--holes-out", metavar="FILE",
                    help="output path for the turned mask (default: the input "
                         "with _rotN appended)")
    ap.add_argument("--seed", help="piece seed file (default: data/seed_Edge5.txt)")
    args = ap.parse_args()

    if args.all and args.n is not None:
        raise SystemExit("[ERROR] --all turns the board every way; drop the N")
    if args.all and (args.out or args.holes_out):
        raise SystemExit("[ERROR] --all writes four files and names them itself; "
                         "drop --out / --holes-out")
    if args.n is None and not args.all:
        raise SystemExit("[ERROR] give N (0..4), or --all for every turn")
    if args.holes_out and not args.holes:
        raise SystemExit("[ERROR] --holes-out only means something with --holes")

    src = Path(args.input)
    if not src.exists():
        raise SystemExit(f"[ERROR] input '{src}' not found")
    holes = Path(args.holes) if args.holes else None
    if holes and not holes.exists():
        raise SystemExit(f"[ERROR] holes mask '{holes}' not found")
    # checked before anything is written, so a bad --holes-out cannot leave a
    # rotated board behind with no mask to go with it
    if args.holes_out and Path(args.holes_out).resolve() == holes.resolve():
        raise SystemExit(f"[ERROR] refusing to overwrite the mask '{holes}'")

    seed = V.load_seed(V.find_seed(args.seed))
    turns = [0, 1, 2, 3] if args.all else [args.n % 4]
    rc = 0

    for turn in turns:
        dst = Path(args.out) if args.out else turned_name(src, turn)
        if dst.resolve() == src.resolve():
            raise SystemExit(f"[ERROR] refusing to overwrite the input '{src}'")
        boards, others, bad = rotate_file(src, turn, dst, seed)
        if not boards:
            raise SystemExit(f"[ERROR] no board rows survived from {src}")

        label = "no rotation" if turn == 0 else f"{turn * 90} degrees clockwise"
        print(f"[rot] {src.name}: {label} (N={turn})")
        print(f"[rot] {boards} board row(s) turned"
              + (f", {others} other row(s) passed through" if others else ""))
        if bad:
            print(f"[rot] {bad} row(s) FAILED the score check and were dropped")
            rc = 1
        else:
            print("[rot] score preserved on every row")
        print(f"[out] {dst}")

        if holes:
            hdst = Path(args.holes_out) if args.holes_out else turned_name(holes, turn)
            open_cells = rotate_holes(holes, turn, hdst)
            print(f"[rot] mask {holes.name}: {open_cells} open cell(s), unchanged "
                  "in number by the turn")
            print(f"[out] {hdst}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
