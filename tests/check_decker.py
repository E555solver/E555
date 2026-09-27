"""check_decker.py -- an independent check of E555_edge_annealer.py --double_decker.

usage (from the repository root, as tests/run_tests.sh runs it):
    python3 tests/check_decker.py ROTATIONS BOARDS SIDES [FINALIZE_FROM]

SIDES is the comma list the run was given (TOP,RIGHT,BOTTOM,LEFT for the bare
flag). For every rotations row and the witness board beside it, it checks:

  - the comment's TOP=.. counts are the classic counts of the row's own spins,
    so --input's cross-check and E555_sort_rotations.py keep reading it right;
  - the board is the beamer's 514-field line, one per row, in row order, and
    its id is the Board= name the comment gives;
  - every placed piece fits its cell and faces the frame the way the
    finalizer's fin_load_partial demands, every placed neighbour pair matches,
    and rows 0..FINALIZE_FROM are complete -- so the finalizer will load it;
  - the spin-1 flags are exactly the inner pieces the board places;
  - Decker= is the count of each two-tall side, recomputed from the board.

Everything is re-derived from the two files with nothing of the annealer's but
rotate_sides and count_euler_trails -- not its double-decker geometry -- so a
geometry bug there cannot vouch for itself."""
import re
import sys

sys.path.insert(0, "src/A_border")
import E555_edge_annealer as A

rot_path, board_path, sides_arg = sys.argv[1:4]
F = int(sys.argv[4]) if len(sys.argv) > 4 else None
DD = {A.Side[n] for n in sides_arg.split(",")} if sides_arg else set()
pieces = A.read_pieces("data/seed_Edge5.txt")
pbi = {p.id: p for p in pieces}
cap = A.build_inner_capacity(pieces)
cfg = A.AnnealingConfig()
T, R, B, L = A.Side.TOP, A.Side.RIGHT, A.Side.BOTTOM, A.Side.LEFT
CELLS = {T: [(15, c) for c in range(1, 15)], B: [(0, c) for c in range(1, 15)],
         L: [(r, 0) for r in range(14, 0, -1)], R: [(r, 15) for r in range(14, 0, -1)]}
INWARD = {T: (-1, 0), B: (1, 0), L: (0, 1), R: (0, -1)}
SRC = {T: L, B: L, L: T, R: T}
DST = {T: R, B: R, L: B, R: B}
ENDS = {T: ((15, 0), (15, 15)), B: ((0, 0), (0, 15)), L: ((15, 0), (0, 0)), R: ((15, 15), (0, 15))}
# a corner is blocked when either of its sides is two tall
CORNER_SIDES = {(15, 0): (T, L), (15, 15): (T, R), (0, 15): (B, R), (0, 0): (B, L)}
blocked = {cell: bool(DD & set(sd)) for cell, sd in CORNER_SIDES.items()}

rows = A.read_rotations(rot_path)
boards = [l.rstrip("\n") for l in open(board_path) if l.strip() and l.lstrip()[0] not in "#%"]
assert len(rows) == len(boards), f"{len(rows)} rotations rows but {len(boards)} boards"
assert rows, "no rotations row to check"
node = lambda f, i: (f << 5) | i

for k, ((spins, comment, lineno), line) in enumerate(zip(rows, boards)):
    # 1. the comment's classic counts are the spins' own
    state = A._build_run_state(pbi, *A.border_from_spins(pbi, spins), cap, cfg)
    got = {A.SIDE_NAMES[s]: state.evals[s].euler_count for s in A.Side}
    assert A.counts_in_comment(comment) == got, f"row {k}: classic counts {comment}"
    m_dd = re.search(r"\bDecker=(\S+)", comment)
    m_bd = re.search(r"\bBoard=(\S+)", comment)
    assert m_dd and m_bd, f"row {k}: no Decker=/Board= in {comment}"
    decker = m_dd.group(1).split("/")

    # 2. the board line: beamer format, the name the comment gives
    fields = [f.strip() for f in line.split(",")]
    assert len(fields) == 514, f"board {k}: {len(fields)} fields"
    assert fields[0] == m_bd.group(1), f"board {k}: id {fields[0]} but comment says {m_bd.group(1)}"
    pos = list(map(int, fields[2:258]))
    rot = list(map(int, fields[258:514]))
    grid = {}
    for i, c in enumerate(pos):
        if c != 999:
            assert c not in grid, f"board {k}: two pieces on cell {c}"
            grid[c] = A.rotate_sides(pbi[i + 1].sides, rot[i])
    cellof = lambda r, c: grid[r * 16 + c]

    # 3. piece type and frame orientation on every placed cell, as fin_load_partial
    for c, S in grid.items():
        r, col = divmod(c, 16)
        frame = [r == 15, col == 15, r == 0, col == 0]           # T R B L
        pid = pos.index(c) + 1
        assert pbi[pid].zero_count == sum(frame), f"board {k}: piece {pid} wrong type at {r},{col}"
        for d in range(4):
            assert (S[d] == 0) == frame[d], f"board {k}: piece {pid} misoriented at {r},{col}"
    # every placed neighbour pair matches
    assert A.board_mismatches(pbi, pos, rot) == 0, f"board {k}: broken edges"
    # the locked rows the finalizer would take are complete (and matched, above)
    if F is not None:
        for r in range(F + 1):
            for c in range(16):
                assert r * 16 + c in grid, f"board {k}: cell {r},{c} empty at finalize_from {F}"

    # 4. the flags are exactly the inner pieces the board places
    placed_inner = {pos.index(c) + 1 for c in grid if pbi[pos.index(c) + 1].zero_count == 0}
    flagged = {i + 1 for i in range(60, 256) if spins[i] == 1}
    assert flagged == placed_inner, f"row {k}: flags {len(flagged)} != placed {len(placed_inner)}"
    assert all(spins[i] in (0, 1) for i in range(60, 256))

    # 5. Decker= recomputed from the board alone
    for s in A.Side:
        cells = CELLS[s]
        b0, b1 = blocked[ENDS[s][0]], blocked[ENDS[s][1]]
        lo, hi = (1 if b0 else 0), (13 if b1 else 14)
        di, dj = INWARD[s]
        arcs = []
        for (r, c) in cells[lo:hi]:
            E = cellof(r, c)
            if s in DD:
                P = cellof(r + di, c + dj)
                arcs.append((node(E[SRC[s]], P[SRC[s]]), node(E[DST[s]], P[DST[s]])))
            else:
                arcs.append((E[SRC[s]], E[DST[s]]))
        def endpoint(i, face, blk, corner):
            r, c = cells[i]
            if not blk:
                return cellof(*corner)[face]
            E = cellof(r, c)
            if s in DD:
                P = cellof(r + di, c + dj)
                return node(E[face], P[face])
            return E[face]
        start = endpoint(0, DST[s], b0, ENDS[s][0])
        end = endpoint(13, SRC[s], b1, ENDS[s][1])
        n = A.count_euler_trails(arcs, start, end)
        want = decker[int(s)]
        if s in DD:
            assert want == str(n), f"row {k}: {A.SIDE_NAMES[s]} Decker={want}, board gives {n}"
        else:
            assert want == "-", f"row {k}: {A.SIDE_NAMES[s]} is classic but Decker={want}"
            assert n >= 1, f"row {k}: classic {A.SIDE_NAMES[s]} has no trail on the board"
print(f"ok: {len(rows)} rows -- classic counts, board ids, frame, matches, flags and "
      f"Decker= all agree ({sides_arg or 'none'} two tall)")
