#!/usr/bin/env python3
"""
E555_rotate.py -- rigid transforms of a board CSV: quarter-turns, and --sink.

WHY

    Every stage downstream is direction-biased: the finalizer frees rows from
    the top down, the roundhouse grows its strip against one border, the topper
    herds breaks toward the nearest corner, the backtracker orders cells from a
    fixed corner. A region that is awkward to attack from the top may be easy
    from the left, so turning a board and re-running the same stage is a real
    move. Run 0/1/2/3 and hand all four to the next stage.

    A turn is lossless: the frame rule is the same on all four sides, the board
    is square, and the piece set never changes. A sink is not, deliberately --
    it throws rows away.

WHAT A TURN PRESERVES  (measured with tools/E555_rank.py)

    identical    breaks, score, solid, placed, corner_d
    transposed   span ("HxW" becomes "WxH"), and break_rows / break_cols swap
    rotated      one clockwise turn sends clean_b -> clean_l -> clean_t ->
                 clean_r -> clean_b, following the board
    different    border, and only it. Its walk starts at a fixed corner, so an
                 incomplete frame reports a different arc from each of the four
                 starting points -- 28 / 13 / 0 / 0 on
                 data/board_partial_row12.csv. A complete, break-free frame
                 gives 60 whichever way you turn it.

GEOMETRY  (n quarter-turns CLOCKWISE, viewed the way E555_viewer prints:
           row 0 at the BOTTOM, col 0 at the LEFT)

    cell    (r, c) -> (SIDE-1-c, r), applied n times.
    spin    (spin + 3n) % 4, from the seed convention
            shown[d] = seed[(d + spin) % 4].

    The same convention as `bin/E555_roundhouse --rotate K`, which turns the
    board internally by exactly this map.

    Unplaced pieces (pos == 999) keep both fields untouched. Rows that are not
    boards -- comments, headers -- pass through verbatim, and the leading meta
    fields of a board row are copied unchanged, so a transformed row still names
    the board it came from.

SINKING  (--sink M)

    --sink M translates every piece M rows down. The turn is the positional N,
    applied first, which is how the sink is aimed: `FILE 2 --sink 3` turns the
    board 180 degrees and then drops what were the input's top three rows.

    Two rules define it.

    fall    a piece landing below row 0 is unset -- the M rows you asked to sink.
    frame   a piece is unset unless its grey sides are exactly the sides of its
            new cell that face out of the board. Grey is the frame colour and
            nothing else, so this single test places every piece.

    The frame rule fires in two places besides the M rows that fall:

    row 0        receives an interior row, whose pieces carry no grey south
                 face, so it empties -- a fresh bottom border to solve.
    row 15-M     receives the input's top frame row, its 14 border pieces and 2
                 corners now showing grey inward. They are unset, which is also
                 how all four corners come free.

    So --sink M opens row 0 and rows 15-M..15 -- 16(M+2) cells -- and frees
    exactly 16(M+2) pieces, matching the opened cells by kind as well as in
    total. Sinks compose: --sink 1 twice returns what --sink 2 returns, byte for
    byte, so M is a dial you can turn one notch at a time on the same file.

    Choose M by the breaks left among the surviving pieces, which the run
    reports. On data/best_463.csv (7 boards, 17 breaks each in the top rows) at
    `FILE 2 --sink M`:

        --sink 1   opens rows 0, 14..15    48 cells    3..12 breaks left
        --sink 2   opens rows 0, 13..15    64 cells    0..2
        --sink 3   opens rows 0, 12..15    80 cells    0..1

    A sunk board has holes in its frame, row 0 among them, so it is a partial
    however complete its input was: Stage C input, not Stage B input.

THE CENTRE CLUE  (--clue_center)

    The clue is piece 138 on one of the four centre cells, and each cell demands
    its own spin -- (7,7):0, (7,8):1, (8,8):2, (8,7):3, one per board
    orientation. Orientation is part of the clue, not a free choice.

    A turn carries a clue that is already in place round to the next centre
    cell, spin and all, so turning never breaks one. A translation moves the
    cell and leaves the spin alone, so a sink of any depth always does: the spin
    names the target cell, and only M = 0 lands on it. --clue_center therefore
    selects boards whose clue the transform puts RIGHT; it cannot preserve one.

    Which settings can is arithmetic, not a search. For turn k the spin becomes
    (spin + 3k) % 4, naming the target cell; the turned column must already
    equal the target's column, and then M = turned_row - target_row is forced.
    Turns k and k+2 give M and -M, the same move read from either end, so at
    most one of that pair is a sink. Few boards admit any (turn, M) at all, so
    an empty result is the normal outcome: the filter is off by default, and a
    run it empties exits non-zero.

VERIFICATION

    Every row's matched-edge count is recomputed after the turn and compared
    with the count before it. A turn that changes the score is a bug, and such a
    row is named on stderr, dropped, and makes the run exit non-zero.

    A sink destroys the junctions to the rows it drops, but a translation cannot
    alter a junction between two pieces that both survive it. So a sunk board is
    compared against the input holding exactly the surviving pieces on their old
    cells, and a row where the two disagree is named and dropped the same way.

MASKS

    A --holes mask is a separate 16x16 grid, so a turned board needs a turned
    mask or the next stage opens the wrong region. `--holes IN.csv` turns it by
    the same map and writes it alongside the board.

ANNEALED BORDERS

    A Stage A rotations CSV is a spin per piece, and for a border piece the spin
    IS its side: the grey face points at the edge of the frame it belongs to.
    Turn the board and those spins no longer describe it, so fin_rot_match stops
    recognising the row and the finalizer drops to --free_edges. `--rotations`
    turns the file instead of a board -- the same spin map, applied only to the
    pieces that have a grey side -- so the annealed side assignment follows the
    board round.

CAVEATS

    Anything that pins a cell -- a clue piece, a corner fixed with
    --BL/--BR/--TL/--TR -- moves with the board, and that stage has to be told
    the new position.

    This tool is a geometric transform and emits nothing but boards. Which cells
    the next stage should open, and which stage that is, are the next stage's
    business.

USAGE

    python3 E555_rotate.py best_463.csv 1     # 90 deg CW -> best_463_rot1.csv
    python3 E555_rotate.py best_463.csv 2     # 180 deg
    python3 E555_rotate.py best_463.csv 0     # copy, no rotation
    python3 E555_rotate.py best_463.csv --all # _rot0 .. _rot3 in one pass
    python3 E555_rotate.py best_463.csv 1 --out /tmp/left.csv
    python3 E555_rotate.py board.csv 1 --holes data/holes_open_border_TR.csv
    python3 E555_rotate.py best_463.csv 2 --sink 3  # -> best_463_rot2_sink3.csv
    python3 E555_rotate.py best_463.csv --sink 1    # no turn, sink one row
    python3 E555_rotate.py best_463.csv --all --sink 2
    python3 E555_rotate.py best_463.csv 0 --sink 4 --clue_center

    N is 0..4, with 0 and 4 both meaning no rotation, and may be omitted when
    --sink is given, so `--sink $DEPTH` scripts without a special case. The
    output is the input name with _rotN (plus _sinkM) before the extension
    unless --out overrides it; --all names its own four files and takes no --out
    or --holes_out. --holes and --rotations are both refused with --sink: a mask
    names cells in the geometry before the sink, and a rotations row is a spin
    per piece with no rows to move.
"""
from __future__ import annotations
import argparse, csv, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import E555_viewer as V                      # seed loading, row parsing, board build
import E555_rank as R                        # FRAME_SIDES: the frame rule, already written

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


def frame_legal(cell, pid, spin, seed):
    """May piece `pid` at spin `spin` sit on `cell`?

    Exactly when its grey sides are the sides of the cell that face out of the
    board: an inner piece anywhere off the frame, a border piece with its one
    grey side pointing out, a corner with both. R.FRAME_SIDES holds the rule."""
    shown = V.rotate_edges(seed[pid], spin)
    out = R.FRAME_SIDES.get(cell, ())
    return all((shown[d] == 0) == (d in out) for d in range(4))


def sink_board(pos, rot, n, seed):
    """Move every piece n rows down; unset what cannot survive the move.

    Returns the new pos array only: a translation does not turn a tile, so rot
    is unchanged, and a freed piece keeps its old spin exactly as an
    already-unplaced one does."""
    new_pos = list(pos)
    for pid, cell in enumerate(pos):
        if cell == UNPLACED:
            continue
        r, c = divmod(cell, SIDE)
        dst = (r - n) * SIDE + c
        new_pos[pid] = dst if r >= n and frame_legal(dst, pid, rot[pid], seed) \
            else UNPLACED
    return new_pos


def matched(pos, rot, seed):
    """(matched junctions, junctions with a placed piece on both sides)."""
    st = V.board_stats(V.build_board(pos, rot), seed)
    return st["h_ok"] + st["v_ok"], st["h_tot"] + st["v_tot"]


def score(pos, rot, seed):
    """Matched internal edges, 0..480: the invariant a turn must preserve."""
    return matched(pos, rot, seed)[0]


def lift(pos, keep):
    """`pos` with every piece the sink freed unset, and nothing moved.

    The board the sunk one is compared against: same pieces, same cells, so a
    difference in matched edges is the translation's doing, and a bug."""
    return [p if keep[pid] != UNPLACED else UNPLACED for pid, p in enumerate(pos)]


def rotate_file(path, n, out_path, seed, sink=0, clue_center=False):
    """Write the transformed copy of `path`.

    Returns (board rows written, other rows, bad rows, rows the clue filter
    dropped, the breaks left in each written board's surviving core)."""
    idx = boards = others = bad = dropped = 0   # idx counts board rows read, good or not
    cores = []
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
            core = None
            if sink:
                sunk = sink_board(new_pos, new_rot, sink, seed)
                kept, total = matched(sunk, new_rot, seed)
                # A translation may destroy junctions but never alter one it
                # keeps, so the surviving pieces score the same before the move.
                if kept != score(lift(new_pos, sunk), new_rot, seed):
                    print(f"[ERROR] row {idx - 1} ({cid}): the sink changed a "
                          "junction between two surviving pieces -- row dropped",
                          file=sys.stderr)
                    bad += 1
                    continue
                new_pos, core = sunk, total - kept
            # Applied to the board as written, so it means the same thing
            # whether the transform was a turn, a sink, or both.
            if clue_center and not V.clue_orient(new_pos, new_rot, V.CLUE_CENTER)[1]:
                dropped += 1
                continue
            if core is not None:
                cores.append(core)
            meta = fields[:-N_TRAILING]        # leading fields carried through
            writer.writerow(meta + [str(v) for v in new_pos + new_rot])
            boards += 1
    return boards, others, bad, dropped, cores


# Seed order is (N, E, S, W) and grey is colour 0. A border piece carries one
# grey side and a corner two, so once a spin is applied the grey side says which
# edge of the frame the piece belongs to -- the entire content of a rotations
# row. classify_deal_from_rotations() in E555_database.c, in Python;
# V.rotate_edges is the same formula the beamer applies.
SIDE_NAMES = ("top", "right", "bottom", "left")
EDGE_LEN = SIDE - 2                       # 14 non-corner pieces per side
# One clockwise turn carries the bottom row round to the left column.
CW_SIDE = {"bottom": "left", "left": "top", "top": "right", "right": "bottom"}


def classify_border(seed, spins):
    """{side: set of piece ids} for the four edges, plus 'corner', from spins alone."""
    out = {name: set() for name in SIDE_NAMES}
    out["corner"] = set()
    for pid, edges in enumerate(seed):
        shown = V.rotate_edges(edges, spins[pid])
        grey = [d for d in range(4) if shown[d] == 0]
        if len(grey) == 2:
            out["corner"].add(pid)
        elif len(grey) == 1:
            out[SIDE_NAMES[grey[0]]].add(pid)
    return out


def turned_sides(cls, n):
    """Where each side's pieces should end up after n quarter-turns clockwise."""
    out = {"corner": cls["corner"]}
    for name in SIDE_NAMES:
        dst = name
        for _ in range(n):
            dst = CW_SIDE[dst]
        out[dst] = cls[name]
    return out


def parse_rotations_row(fields):
    """Split a rotations row into (meta, spins), or None if it is not one.

    Mirrors read_one_border_row in E555_database.c: 256, 257 or 258 fields, the
    spins being the last 256, so the leading id and any second meta column ride
    through untouched."""
    if len(fields) not in (N_PIECES, N_PIECES + 1, N_PIECES + 2):
        return None
    spins = fields[-N_PIECES:]
    try:
        vals = [int(s) for s in spins]
    except ValueError:
        return None
    if any(v < 0 or v > 3 for v in vals):
        return None
    return fields[:-N_PIECES], vals


def rotate_rotations(path, n, out_path, seed):
    """Turn a Stage A rotations CSV; return (rows written, other rows, bad rows).

    Only pieces with a grey side are touched. Nothing downstream reads an inner
    piece's spin out of a rotations row -- classify_deal_from_rotations,
    build_top_border_demands and fin_rot_match all skip them -- so leaving them
    alone keeps the file's meaning identical and its diff to the frame."""
    greyed = [pid for pid, e in enumerate(seed) if 0 in e]
    rows = others = bad = 0
    label = "no rotation" if n == 0 else f"{n * 90} degrees clockwise"
    with open(path, newline="") as fh, open(out_path, "w", newline="") as out:
        writer = csv.writer(out, lineterminator="\n")
        out.write(f"# {label} from {Path(path).name} by E555_rotate.py --rotations\n")
        for raw in csv.reader(fh):
            fields = [f.strip() for f in raw]
            rec = parse_rotations_row([f for f in fields if f != ""])
            if rec is None:                       # comment, header, short row
                writer.writerow(raw)
                others += 1
                continue
            meta, spins = rec
            before = classify_border(seed, spins)
            new = list(spins)
            for pid in greyed:
                new[pid] = (new[pid] + 3 * n) % 4
            after = classify_border(seed, new)
            # A legal row partitions the frame 14/14/14/14 with one corner per
            # board corner -- the same test fin_rot_row_valid applies, and a row
            # failing it is dropped there too, so dropping it here keeps the row
            # numbering the two see identical.
            sizes = [len(before[s]) for s in SIDE_NAMES]
            if len(before["corner"]) != 4 or any(s != EDGE_LEN for s in sizes):
                print(f"[ERROR] row {rows + bad} is not a legal 14/14/14/14 border "
                      f"partition ({sizes}, {len(before['corner'])} corners) "
                      "-- row dropped", file=sys.stderr)
                bad += 1
                continue
            # The invariant a turn must preserve, and the reason this mode exists:
            # the side SETS have to follow the board round, or the finalizer would
            # be handed the wrong pool for each side.
            want = turned_sides(before, n)
            if any(after[s] != want[s] for s in SIDE_NAMES) or \
                    after["corner"] != want["corner"]:
                print(f"[ERROR] row {rows + bad}: the side sets did not follow the "
                      "turn -- row dropped", file=sys.stderr)
                bad += 1
                continue
            writer.writerow(meta + [str(v) for v in new])
            rows += 1
    return rows, others, bad


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


def turned_name(src, n, sink=0):
    """The default output path: FILE_rotN.ext, plus _sinkM when sinking."""
    tag = f"_rot{n}" + (f"_sink{sink}" if sink else "")
    return src.with_name(f"{src.stem}{tag}{src.suffix}")


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
                         "no N, --out or --holes_out")
    ap.add_argument("--sink", type=int, default=0, metavar="M",
                    help="after the turn, move every piece M rows down: the "
                         "bottom M rows fall out of the board, row 0 and the "
                         "top M+1 rows come free. 0..14, 0 = no sink. N may be "
                         "omitted when this is given.")
    ap.add_argument("--clue_center", action="store_true",
                    help="keep only boards whose centre clue is in place after "
                         "the transform (piece 138 at one of its four cells and "
                         "spins). Very selective; off by default.")
    ap.add_argument("--out", metavar="FILE",
                    help="output path (default: the input with _rotN appended)")
    ap.add_argument("--rotations", action="store_true",
                    help="the input is a Stage A rotations CSV (a spin per piece, "
                         "not a board): turn the border's side assignment instead, "
                         "so an annealed border still matches after the board turns")
    ap.add_argument("--holes", metavar="FILE",
                    help="a 16x16 --holes mask to turn with the board, so the "
                         "next stage opens the same cells it did before")
    ap.add_argument("--holes_out", metavar="FILE",
                    help="output path for the turned mask (default: the input "
                         "with _rotN appended)")
    ap.add_argument("--seed_file", help="piece seed file (default: data/seed_Edge5.txt)")
    args = ap.parse_args()

    if args.all and args.n is not None:
        raise SystemExit("[ERROR] --all turns the board every way; drop the N")
    if args.all and (args.out or args.holes_out):
        raise SystemExit("[ERROR] --all writes four files and names them itself; "
                         "drop --out / --holes_out")
    if args.n is None and not (args.all or args.sink):
        raise SystemExit("[ERROR] give N (0..4), --all for every turn, or --sink M")
    if args.holes_out and not args.holes:
        raise SystemExit("[ERROR] --holes_out only means something with --holes")
    if args.rotations and args.holes:
        raise SystemExit("[ERROR] --rotations turns a border's side assignment, "
                         "which has no cells for a --holes mask to name")
    if not 0 <= args.sink <= SIDE - 2:
        raise SystemExit(f"[ERROR] --sink {args.sink} is outside 0..{SIDE - 2}; "
                         f"{SIDE - 1} rows down leaves an empty board")
    if args.sink and args.rotations:
        raise SystemExit("[ERROR] --rotations is a spin per piece, not a board: "
                         "it has no rows to sink")
    if args.sink and args.holes:
        raise SystemExit("[ERROR] --holes names cells in the geometry before the "
                         "sink; turn the mask in a separate run")

    src = Path(args.input)
    if not src.exists():
        raise SystemExit(f"[ERROR] input '{src}' not found")
    holes = Path(args.holes) if args.holes else None
    if holes and not holes.exists():
        raise SystemExit(f"[ERROR] holes mask '{holes}' not found")
    # checked before anything is written, so a bad --holes_out cannot leave a
    # rotated board behind with no mask to go with it
    if args.holes_out and Path(args.holes_out).resolve() == holes.resolve():
        raise SystemExit(f"[ERROR] refusing to overwrite the mask '{holes}'")

    seed = V.load_seed(V.find_seed(args.seed_file))
    turns = [0, 1, 2, 3] if args.all else [(args.n or 0) % 4]
    rc = 0

    for turn in turns:
        dst = Path(args.out) if args.out else turned_name(src, turn, args.sink)
        if dst.resolve() == src.resolve():
            raise SystemExit(f"[ERROR] refusing to overwrite the input '{src}'")
        dropped, cores = 0, []
        if args.rotations:
            boards, others, bad = rotate_rotations(src, turn, dst, seed)
        else:
            boards, others, bad, dropped, cores = rotate_file(
                src, turn, dst, seed, args.sink, args.clue_center)
        if not boards and dropped:
            raise SystemExit(f"[ERROR] --clue_center dropped all {dropped} board "
                             "row(s): none carries piece 138 at a centre cell "
                             "with the spin that cell wants. Try another --sink "
                             "depth or another turn")
        if not boards:
            raise SystemExit(f"[ERROR] no {'border' if args.rotations else 'board'} "
                             f"rows survived from {src}")

        label = "no rotation" if turn == 0 else f"{turn * 90} degrees clockwise"
        kind = "border" if args.rotations else "board"
        print(f"[rot] {src.name}: {label} (N={turn})")
        print(f"[rot] {boards} {kind} row(s) turned"
              + (f", {others} other row(s) passed through" if others else ""))
        check = "the side sets followed the turn on every row" if args.rotations \
            else "score preserved on every row"
        if bad:
            print(f"[rot] {bad} row(s) FAILED the check and were dropped")
            rc = 1
        else:
            print(f"[rot] {check}")
        if args.sink:
            opened = [0] + list(range(SIDE - 1 - args.sink, SIDE))
            # cells, not pieces: a partial input may already have had some of
            # them empty, so this is what the sink opens, not what it freed.
            print(f"[rot] sunk {args.sink} row(s): rows "
                  + ", ".join(str(r) for r in opened)
                  + f" now open -- {16 * (args.sink + 2)} cell(s)")
            print(f"[rot] breaks left in the surviving core: min {min(cores)}, "
                  f"max {max(cores)}, {cores.count(0)} of {len(cores)} clean")
        if dropped:
            print(f"[rot] --clue_center kept {boards} row(s) and dropped {dropped}")
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
