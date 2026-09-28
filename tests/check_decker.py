"""check_decker.py -- an independent check of E555_edge_annealer.py --double_decker.

usage (from the repository root, as tests/run_tests.sh runs it):
    python3 tests/check_decker.py ROTATIONS BOARDS SIDES [FINALIZE_FROM]

SIDES is the comma list the run was given (TOP,RIGHT,BOTTOM,LEFT for the bare
flag). For every rotations row and the witness board beside it, it checks:

  - the comment's TOP=.. counts are the classic counts of the row's own spins,
    so --input's cross-check and E555_sort_rotations.py keep reading it right;
  - the board is the beamer's 514-field line, one per row, in row order, its id
    is the Board= name the comment gives, and the `# <name> reserve ...` line
    above it names each two-tall side's reserve;
  - every placed piece fits its cell and faces the frame the way the
    finalizer's fin_load_partial demands, every placed neighbour pair matches,
    and rows 0..FINALIZE_FROM are complete -- so the finalizer will load it;
  - the spin-1 flags are exactly the reserves plus the corner blocks' inner
    pieces, and the board's second-ring pieces all come from the reserves;
  - Decker= is, for each two-tall side, the number of layouts of its strip
    from its reserve, recounted here by a search of its own; DeckerPool= is
    at least that.

Everything is re-derived from the two files with nothing of the annealer's but
rotate_sides and the file readers -- not its double-decker geometry or its
counter -- so a bug there cannot vouch for itself."""
import re
import sys
from functools import lru_cache

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
INWARD = {T: (-1, 0), B: (1, 0), L: (0, 1), R: (0, -1)}   # edge cell -> strip cell
FACE_IN = {T: B, B: T, L: R, R: L}                         # edge face toward the strip
SRC = {T: L, B: L, L: T, R: T}
DST = {T: R, B: R, L: B, R: B}
ENDS = {T: ((15, 0), (15, 15)), B: ((0, 0), (0, 15)), L: ((15, 0), (0, 0)), R: ((15, 15), (0, 15))}
CORNER_SIDES = {(15, 0): (T, L), (15, 15): (T, R), (0, 15): (B, R), (0, 0): (B, L)}
blocked = {cell: bool(DD & set(sd)) for cell, sd in CORNER_SIDES.items()}
Q_CELL = {(15, 0): (14, 1), (15, 15): (14, 14), (0, 15): (1, 14), (0, 0): (1, 1)}

rows = A.read_rotations(rot_path)
boards, reserves = [], []
pending = None
for line in open(board_path):
    line = line.rstrip("\n")
    if not line.strip():
        continue
    if line.lstrip().startswith("#"):
        m = re.match(r"#\s+(\S+)\s+reserve\s+(.*)$", line)
        if m:
            pending = (m.group(1), {A.Side[k]: [int(x) for x in v.split(",")]
                                    for k, v in re.findall(r"(\w+)=([\d,]+)", m.group(2))})
        continue
    boards.append(line)
    reserves.append(pending)
    pending = None
assert rows, "no rotations row to check"
assert len(rows) == len(boards), f"{len(rows)} rotations rows but {len(boards)} boards"


def layouts(s, edges, start, end, reserve):
    """The layouts of side s's strip from a reserve, by a plain memoized search
    over (placed edges, used pieces, the (frame, inner) pair at the frontier).
    edges are (frame in, frame out, inward colour). A piece in rotation r goes
    under an edge when its face toward the edge (face `s`: a top strip's piece
    touches its edge with its TOP face) shows the edge's inward colour and its
    face toward the previous cell shows the frontier's inner colour."""
    rots = {p: [A.rotate_sides(pbi[p].sides, r) for r in range(4)] for p in reserve}

    @lru_cache(maxsize=None)
    def go(done, used, frame, inner):
        if len(done) == len(edges):
            return 1 if (frame, inner) == end else 0
        total = 0
        for i, (fin, fout, inw) in enumerate(edges):
            if i in done or fin != frame:
                continue
            for p in reserve:
                if p in used:
                    continue
                for P in rots[p]:
                    if P[s] == inw and P[SRC[s]] == inner:
                        total += go(done | {i}, used | {p}, fout, P[DST[s]])
        return total

    return go(frozenset(), frozenset(), start[0], start[1])


for k, ((spins, comment, lineno), line, res_line) in enumerate(zip(rows, boards, reserves)):
    # 1. the comment's classic counts are the spins' own
    state = A._build_run_state(pbi, *A.border_from_spins(pbi, spins), cap, cfg)
    got = {A.SIDE_NAMES[s]: state.evals[s].euler_count for s in A.Side}
    assert A.counts_in_comment(comment) == got, f"row {k}: classic counts {comment}"
    m_dd = re.search(r"\bDecker=(\S+)", comment)
    m_pool = re.search(r"\bDeckerPool=(\S+)", comment)
    m_bd = re.search(r"\bBoard=(\S+)", comment)
    assert m_dd and m_pool and m_bd, f"row {k}: no Decker=/DeckerPool=/Board= in {comment}"
    decker = m_dd.group(1).split("/")
    pool = m_pool.group(1).split("/")

    # 2. the board line: beamer format, the name the comment gives, its reserve
    fields = [f.strip() for f in line.split(",")]
    assert len(fields) == 514, f"board {k}: {len(fields)} fields"
    assert fields[0] == m_bd.group(1), f"board {k}: id {fields[0]} but comment says {m_bd.group(1)}"
    assert res_line and res_line[0] == fields[0], f"board {k}: no reserve line for {fields[0]}"
    reserve = res_line[1]
    assert set(reserve) == DD, f"board {k}: reserves for {sorted(reserve)}, sides are {sorted(DD)}"
    pos = list(map(int, fields[2:258]))
    rot = list(map(int, fields[258:514]))
    grid, where = {}, {}
    for i, c in enumerate(pos):
        if c != 999:
            assert c not in grid, f"board {k}: two pieces on cell {c}"
            grid[c] = A.rotate_sides(pbi[i + 1].sides, rot[i])
            where[c] = i + 1
    cellof = lambda r, c: grid[r * 16 + c]

    # 3. piece type and frame orientation on every placed cell, as fin_load_partial
    for c, S in grid.items():
        r, col = divmod(c, 16)
        frame = [r == 15, col == 15, r == 0, col == 0]           # T R B L
        pid = where[c]
        assert pbi[pid].zero_count == sum(frame), f"board {k}: piece {pid} wrong type at {r},{col}"
        for d in range(4):
            assert (S[d] == 0) == frame[d], f"board {k}: piece {pid} misoriented at {r},{col}"
    assert A.board_mismatches(pbi, pos, rot) == 0, f"board {k}: broken edges"
    if F is not None:
        for r in range(F + 1):
            for c in range(16):
                assert r * 16 + c in grid, f"board {k}: cell {r},{c} empty at finalize_from {F}"

    # 4. flags = reserves + corner q's; the second ring comes from the reserves
    qs = {where[Q_CELL[cell][0] * 16 + Q_CELL[cell][1]] for cell, b in blocked.items() if b}
    flagged = {i + 1 for i in range(60, 256) if spins[i] == 1}
    assert all(spins[i] in (0, 1) for i in range(60, 256))
    all_res = {p for rs in reserve.values() for p in rs}
    assert flagged == all_res | qs, f"row {k}: flags {len(flagged)} != reserves+q {len(all_res | qs)}"
    for s in DD:
        di, dj = INWARD[s]
        for (r, c) in CELLS[s][1:13]:
            assert where[(r + di) * 16 + c + dj] in reserve[s], \
                f"row {k}: {A.SIDE_NAMES[s]} strip piece at {r + di},{c + dj} is not in its reserve"

    # 5. Decker= recounted from the board's frame and the reserve alone
    for s in A.Side:
        want = decker[int(s)]
        if s not in DD:
            assert want == "-" and pool[int(s)] == "-", f"row {k}: classic side has a Decker value"
            continue
        di, dj = INWARD[s]
        edges = []
        for (r, c) in CELLS[s][1:13]:
            E = cellof(r, c)
            edges.append((E[SRC[s]], E[DST[s]], E[FACE_IN[s]]))
        (r0, c0), (r1, c1) = CELLS[s][0], CELLS[s][13]
        start = (cellof(r0, c0)[DST[s]], cellof(r0 + di, c0 + dj)[DST[s]])
        end = (cellof(r1, c1)[SRC[s]], cellof(r1 + di, c1 + dj)[SRC[s]])
        n = layouts(s, edges, start, end, reserve[s])
        assert want == str(n), f"row {k}: {A.SIDE_NAMES[s]} Decker={want}, recount gives {n}"
        assert n >= 1 and float(pool[int(s)]) >= n * 0.999, \
            f"row {k}: {A.SIDE_NAMES[s]} DeckerPool={pool[int(s)]} below Decker={n}"
print(f"ok: {len(rows)} rows -- classic counts, board ids, reserves, frame, matches, flags "
      f"and Decker= all agree ({sides_arg or 'none'} two tall)")
