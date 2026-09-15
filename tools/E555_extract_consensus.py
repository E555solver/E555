#!/usr/bin/env python3
"""
E555_extract_consensus.py -- rank clued partials by how well they match what
the whole corpus agrees on, and distil that agreement into a Stage A border.

WHY

    Above the entropy floor E555_rank.py runs out of discrimination: every
    board in a large Stage B pool carries nearly the same break count, and all
    of rank's measures read one board in isolation. The information that is
    actually there is BETWEEN boards. If a piece keeps landing on the same cell
    in board after board, that cell is saying something no single-board measure
    can see, and a board that agrees with a hundred siblings it never met is a
    better bet than one that does not.

    Pooling boards like that is only legal because of the clue symmetry. The
    five published clues are one rigid body in exactly four configurations
    (g_clue[4][CLUE_N] in src/B_beam/E555_database.c), so a clued board's
    orientation can be READ, not guessed. Turn every board to the same
    orientation and cell 119 means the same thing in all of them.

        a board of orientation o, turned k quarter-turns clockwise, has
        orientation (o + k) % 4

    so k = (4 - o) % 4 canonicalises any clued board to orientation 0 -- the
    centre clue on (7,7), the lower-left quadrant, which is `--pin_clue 1` to
    the beamer and the finalizer.

THE CONSENSUS

    One pass over the input builds a 256 x 256 table: how often each piece
    occupied each cell, counted in the canonical frame, over every board that
    carries the centre clue. Spin is not counted -- a piece sitting on a cell
    at the wrong spin is still that piece on that cell, and the spin of a
    border piece is already pinned by the frame.

    Boards WITHOUT the centre clue are dropped, counted, and reported. Only
    the centre clue is tested: the corner clues and the corner pieces are not
    checked, so a board whose Stage C pass moved a corner still counts.

SCORING A BOARD  (--metric, default `lift`)

    Every board is scored in the canonical frame the consensus was built in,
    and unplaced cells are skipped.

        lift   mean over the board's placed cells of

                   log2( P(this piece | this cell) x K(cell) )

               in BITS. K is the null model -- 4, 56 or 196, the number of
               pieces that could legally sit on a corner, border or interior
               cell -- so a cell where the board agrees with nobody reads 0
               and the three kinds of cell are on one scale. This is the
               default because it uses the whole frequency column, not just
               its mode, and because without the K a board with more of its
               frame placed outscores a better board with less of it.

        logp   mean log2 P(piece | cell): plain negative cross-entropy, the
               same quantity without the null model.
        rank   mean normalised position of the board's piece in that cell's
               frequency ordering, 0..1. Non-parametric: immune to the heavy
               tail of the counts and to --alpha, but blind to magnitude.
        top1   fraction of scored cells holding the cell's single most common
               piece. The readable one; too coarse to sort a large pool by.

    All four are computed and printed whichever one sorts.

    P is a LEAVE-ONE-OUT estimate -- the board's own vote is subtracted before
    it is scored, so a board is never rewarded for being in the corpus:

        P = (count[cell][piece] - 1 + a) / (placed[cell] - 1 + a * K)

    with a = --alpha (default 0.5) keeping it finite when that empties a cell.
    --no_loo turns the correction off; --consensus_in makes it unnecessary,
    since the table then comes from boards that are not being scored.

    Two guards keep the mean honest. --min_support drops cells too thin to
    mean anything (default 2 boards), and the `cells` column is always
    printed: a board scored on 140 cells and one scored on 250 are not making
    the same claim. --box restricts scoring to a rectangle, in CANONICAL
    coordinates.

THE BAG THAT IS LEFT  (--best_top, --best_bottom)

    A different question, and the one that matters when choosing which partial
    to hand to E555_finalizer: never mind where this board's pieces are, are
    the pieces it has LEFT the right pieces for the rows it has left?

        --best_top     the bag is every unplaced piece plus everything already
                       in the top --band_rows rows, in the board's OWN
                       un-rotated frame -- exactly the pieces that must fill
                       them. On a partial stopped below the band that is the
                       80 unplaced pieces.
        --best_bottom  the same rule at the other end: does this board's
                       foundation use the pieces the corpus puts there? The
                       bottom band of a bottom-up partial is already full, so
                       the bag is its own 80 pieces -- the unplaced ones are
                       reserved for the TOP and join a bottom bag only when
                       the bottom itself is holed, as after a --sink.

    Position inside the band is integrated out, so only band membership
    counts. From the same canonical table,

        q(p) = P(piece p lies in band B | p is placed anywhere)
        score = mean over the bag of  log2( q(p) / (band cells / 256) )

    in bits of enrichment, again leave-one-out -- and here the correction is
    not optional: a board's own placed bag pieces are, by construction,
    exactly its own contribution to band B.

    B is the canonical band the board's own band lands in, which is why this
    is done four times over. k clockwise turns carry the top rows to

        k = 0  TOP     k = 1  RIGHT     k = 2  BOTTOM     k = 3  LEFT

    so a board of orientation o is scored against band (B0 + (4-o)%4) % 4.

    THE CAVEAT, and the run prints it: a canonical band is only populated by
    boards whose own solved region landed there, so a corpus that is all one
    orientation leaves three bands empty and the measure vacuous. Per-band
    support is reported, and a band under --min_band_support is refused rather
    than scored. A pool mixing all four orientations is the case this works on.

OUTPUT

    The table, --top and --out follow E555_rank.py exactly, and --out writes
    the surviving input rows VERBATIM -- so the boards come back in their own
    original, un-rotated frame with their unplaced rows intact. The canonical
    copy exists only inside the scoring and is never written.

THE BORDER THE CORPUS IMPLIES  (--border_out FILE)

    The consensus knows which of the 56 edge pieces the corpus keeps putting
    on which side, and which corner piece it keeps putting in which corner.
    That is a Stage A answer, arrived at from Stage B evidence instead of from
    the annealer's cold start, and --border_out writes it as a rotations CSV
    the beamer reads directly.

    Affinity, in bits, is the same idea as `lift`: for edge piece p and side S,

        reward(p, S) = log2( 4 x P(S | p is on the frame) )

    The unconstrained best assignment -- 56 pieces to 4 sides x 14 slots -- is
    then exact, by Hungarian on a 56x56 matrix, with the 24 corner
    permutations brute-forced. That optimum is the ceiling, and it is printed.

    It is also, almost always, unusable. A side is a directed multigraph whose
    Euler trails ARE the orderings Stage B can enumerate, and an assignment
    picked for affinity alone routinely leaves a side with none at all, which
    would hand the beamer a border it cannot lay. So the optimum is only the
    SEED: from there this reuses the Stage A annealer's own move set and Euler
    machinery (src/A_border/E555_edge_annealer.py) to buy feasibility back,

        score = w_consensus x (affinity bits)
              + w_trails    x sum of log10(trail count)
              - 200 per decade any side sits below --min_trails
              - the annealer's own hard penalty

    so --min_trails (default 1000) is effectively a floor on every side while
    still pulling an infeasible start toward feasibility, and above the floor
    the affinity and the trail counts trade against each other openly.

    The search is capped by the clock, not by convergence: --border_time
    (default 120s) restarts until the budget is gone and then emits the best
    border it has, warning loudly if a side is still short of the floor. It
    cannot hang.

    The row is written in the annealer's own format -- a `#` comment carrying
    the four trail counts, then `id, spin[0..255]` -- and is verified with
    E555_rotate.classify_border, the same 14/14/14/14-plus-four-corners test
    E555_database.c's classify_deal_from_rotations applies, before the file is
    written at all. The border comes out in the canonical frame, so it pairs
    with `--pin_clue 1`; --border_pin N turns it to pair with any of the four.

USAGE

    python3 tools/E555_extract_consensus.py pool.csv --top 50
    python3 tools/E555_extract_consensus.py pool.csv --top 200 --out elite.csv
    python3 tools/E555_extract_consensus.py pool*.csv --metric rank --csv
    python3 tools/E555_extract_consensus.py pool.csv --best_top --top 50
    python3 tools/E555_extract_consensus.py pool.csv --best_bottom --band_rows 4
    python3 tools/E555_extract_consensus.py big.csv --consensus_out cons.txt --top 1
    python3 tools/E555_extract_consensus.py new.csv --consensus_in cons.txt --top 50
    python3 tools/E555_extract_consensus.py pool.csv --border_out border.csv --top 1
    python3 tools/E555_extract_consensus.py pool.csv --border_out b3.csv --border_pin 3

    Then, on a border it wrote:

    bin/E555_beamer data/seed_Edge5.txt border.csv --pin_clue 1 --stop_row 11 ...
"""
from __future__ import annotations
import argparse, csv, heapq, itertools, math, random, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import E555_viewer as V                  # seed loading, row parsing, the clue table
import E555_rank as R                    # FRAME_SIDES: which faces of a cell look out
import E555_rotate as RT                 # rotate_cell/rotate_board, classify_border

SIDE, N_PIECES, UNPLACED = V.SIDE, V.N_PIECES, V.UNPLACED
LOG2 = math.log(2.0)

# Cell kinds, and the size of the piece pool each one draws from. That pool is
# the null model every `lift` is measured against: 4 corners, 56 edges, 196
# inner pieces, a partition of the 256 that the frame rule fixes and no board
# can change.
KIND_CORNER, KIND_BORDER, KIND_INNER = 0, 1, 2
KIND_K = (4, 56, 196)
KIND_NAME = ("corner", "border", "inner")

def cell_kind(cell):
    """Corner, border or interior, from the outward faces the frame rule gives
    the cell -- R.FRAME_SIDES already holds that rule, so it is not restated."""
    n = len(R.FRAME_SIDES.get(cell, ()))
    return KIND_CORNER if n == 2 else KIND_BORDER if n == 1 else KIND_INNER

CELL_KIND = tuple(cell_kind(c) for c in range(N_PIECES))
CELL_K = tuple(KIND_K[k] for k in CELL_KIND)

# The four board corners as cells, in the annealer's Corner order (TL TR BR BL).
CORNER_CELLS = ((SIDE - 1) * SIDE, N_PIECES - 1, SIDE - 1, 0)

# Bands, in the annealer's Side order: TOP RIGHT BOTTOM LEFT. One clockwise
# turn carries each to the next, which is the whole reason the four exist --
# verified against rotate_cell, not assumed.
BAND_NAMES = ("TOP", "RIGHT", "BOTTOM", "LEFT")
BAND_TOP, BAND_RIGHT, BAND_BOTTOM, BAND_LEFT = 0, 1, 2, 3

def band_cells(band, depth):
    """The cells of one band, `depth` rows or columns deep."""
    if band == BAND_TOP:
        return tuple(r * SIDE + c for r in range(SIDE - depth, SIDE) for c in range(SIDE))
    if band == BAND_RIGHT:
        return tuple(r * SIDE + c for r in range(SIDE) for c in range(SIDE - depth, SIDE))
    if band == BAND_BOTTOM:
        return tuple(r * SIDE + c for r in range(depth) for c in range(SIDE))
    return tuple(r * SIDE + c for r in range(SIDE) for c in range(depth))


def canon_turn(orient):
    """Quarter-turns clockwise that bring orientation `orient` to orientation 0.

    A board of orientation o turned k times has orientation (o + k) % 4, so
    this is (4 - o) % 4. Orientation 0 puts the centre clue on (7,7), the
    lower-left quadrant -- `--pin_clue 1`."""
    return (4 - orient) % 4


# =============================================================================
# The consensus table
# =============================================================================

class Consensus:
    """How often each piece occupied each cell, in the canonical frame.

    Sparse by cell: `count[cell]` is a dict piece -> occurrences, which is what
    the `rank` metric wants to walk and what keeps a table built from a small
    corpus small. Everything else here is a marginal of that table, kept
    alongside because recomputing them per board is what made a first draft
    quadratic.
    """

    def __init__(self, band_rows=5):
        self.band_rows = band_rows
        self.count = [dict() for _ in range(N_PIECES)]   # cell -> {piece: n}
        self.placed = [0] * N_PIECES                     # cell -> boards placing it
        self.tot = [0] * N_PIECES                        # piece -> placements anywhere
        self.band = [[0] * N_PIECES for _ in range(4)]   # band -> piece -> placements
        # Two different questions, and conflating them is a trap: `band_touch`
        # is how many boards put ANY piece in the band, `band_boards` how many
        # filled it completely. A pool of bottom-up partials touches the left
        # and right bands with every board while only half covering them, so
        # touch is not evidence the marginal is worth reading -- coverage is,
        # and it is what --min_band_support gates on.
        self.band_boards = [0] * 4
        self.band_touch = [0] * 4
        self.boards = 0
        self.sources = []
        self._modes = None
        self._ranks = None
        self._band_cells = None
        self._member = None

    # -- construction ------------------------------------------------------

    def band_of(self):
        if self._band_cells is None:
            self._band_cells = [band_cells(b, self.band_rows) for b in range(4)]
        return self._band_cells

    def add(self, canon_pos):
        """Fold one canonicalised board in. `canon_pos` is piece -> cell."""
        seen = [0] * 4
        member = self._band_member()
        for piece, cell in enumerate(canon_pos):
            if cell == UNPLACED:
                continue
            d = self.count[cell]
            d[piece] = d.get(piece, 0) + 1
            self.placed[cell] += 1
            self.tot[piece] += 1
            for b in member[cell]:
                self.band[b][piece] += 1
                seen[b] += 1
        for b in range(4):
            if seen[b]:
                self.band_touch[b] += 1
                if seen[b] == len(self.band_of()[b]):
                    self.band_boards[b] += 1
        self.boards += 1
        self._modes = self._ranks = None

    def _band_member(self):
        """cell -> the bands holding it. A cell in a corner belongs to two."""
        if self._member is None:
            mem = [[] for _ in range(N_PIECES)]
            for b, cells in enumerate(self.band_of()):
                for cell in cells:
                    mem[cell].append(b)
            self._member = [tuple(m) for m in mem]
        return self._member

    # -- derived views -----------------------------------------------------

    def modes(self):
        """cell -> the single most common piece there, or -1 for an empty cell.
        Ties go to the lower piece id, which is arbitrary but reproducible."""
        if self._modes is None:
            self._modes = [max(d.items(), key=lambda kv: (kv[1], -kv[0]))[0] if d else -1
                           for d in self.count]
        return self._modes

    def ranks(self):
        """cell -> {piece: 0-based position in that cell's frequency order}.

        Built once. The `rank` metric needs it per scored cell, and rebuilding
        it per board turned a 30-second run into an overnight one."""
        if self._ranks is None:
            out = []
            for d in self.count:
                order = sorted(d.items(), key=lambda kv: (-kv[1], kv[0]))
                out.append({p: i for i, (p, _) in enumerate(order)})
            self._ranks = out
        return self._ranks

    # -- serialisation -----------------------------------------------------

    def write(self, path):
        """A flat, greppable table. Not JSON: this is the artefact a later run
        scores against, and being able to read one line of it with `grep` is
        worth more than being able to load it in one call."""
        with open(path, "w") as fh:
            fh.write("# E555 consensus table v1 -- canonical frame, clue orientation 0 "
                     "(centre clue on (7,7), --pin_clue 1)\n")
            fh.write("# built from %d board(s): %s\n" % (self.boards, ", ".join(self.sources)))
            fh.write("version 1\n")
            fh.write("boards %d\n" % self.boards)
            fh.write("band_rows %d\n" % self.band_rows)
            for b in range(4):
                fh.write("band_boards %d %d\n" % (b, self.band_boards[b]))
                fh.write("band_touch %d %d\n" % (b, self.band_touch[b]))
            for cell in range(N_PIECES):
                if self.placed[cell]:
                    fh.write("placed %d %d\n" % (cell, self.placed[cell]))
            for piece in range(N_PIECES):
                if self.tot[piece]:
                    fh.write("tot %d %d\n" % (piece, self.tot[piece]))
            for b in range(4):
                for piece in range(N_PIECES):
                    if self.band[b][piece]:
                        fh.write("band %d %d %d\n" % (b, piece, self.band[b][piece]))
            for cell in range(N_PIECES):
                for piece, n in sorted(self.count[cell].items()):
                    fh.write("count %d %d %d\n" % (cell, piece, n))

    @classmethod
    def read(cls, path):
        self = cls()
        with open(path) as fh:
            for lineno, line in enumerate(fh, 1):
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                f = line.split()
                try:
                    if f[0] == "version":
                        if f[1] != "1":
                            raise SystemExit(f"[ERROR] {path}: consensus format v{f[1]}, "
                                             "this build reads v1")
                    elif f[0] == "boards":
                        self.boards = int(f[1])
                    elif f[0] == "band_rows":
                        self.band_rows = int(f[1])
                    elif f[0] == "band_boards":
                        self.band_boards[int(f[1])] = int(f[2])
                    elif f[0] == "band_touch":
                        self.band_touch[int(f[1])] = int(f[2])
                    elif f[0] == "placed":
                        self.placed[int(f[1])] = int(f[2])
                    elif f[0] == "tot":
                        self.tot[int(f[1])] = int(f[2])
                    elif f[0] == "band":
                        self.band[int(f[1])][int(f[2])] = int(f[3])
                    elif f[0] == "count":
                        self.count[int(f[1])][int(f[2])] = int(f[3])
                    else:
                        raise SystemExit(f"[ERROR] {path}:{lineno}: unknown record "
                                         f"'{f[0]}'")
                except (IndexError, ValueError):
                    raise SystemExit(f"[ERROR] {path}:{lineno}: malformed record: {line!r}")
        self.sources = [Path(path).name]
        return self


# =============================================================================
# Reading boards, and canonicalising them
# =============================================================================

def canonicalise(pos, rot):
    """(orientation, quarter-turns, canonical pos) for a board, or None.

    None means the board does not carry the centre clue -- piece 138 is not on
    one of the four centre cells at that cell's own spin -- and is the one
    filter this tool applies to its input. The corner clues and the corner
    pieces are deliberately not looked at.

    Only `pos` is turned. The consensus does not count spin, and the emitted
    rows are the input's own bytes, so the turned `rot` would never be read."""
    orient, n = V.clue_orient(pos, rot, V.CLUE_CENTER)
    if not n:
        return None
    k = canon_turn(orient)
    if k == 0:
        return orient, 0, list(pos)
    return orient, k, [p if p == UNPLACED else RT.rotate_cell(p, k) for p in pos]


def iter_boards(paths, stats, progress_every=0):
    """Yield (path, row, id, line, orient, turn, canon_pos, pos) for every clued row.

    `pos` is the board's OWN, un-turned placement. --best_top and --best_bottom
    name their band in that frame, so both have to travel together.

    `stats` accumulates what was skipped and why, so one report can be printed
    after the pass instead of a line per board. A row that fails to parse as a
    board is passed over exactly as E555_rank.py passes over it; a row that
    parses but carries no centre clue is counted separately, because that is a
    filter working, not an input problem."""
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
                    V.build_board(pos, rot)          # the strict board validator
                except ValueError as exc:
                    print(f"[skip] {path}:{idx}: {exc}", file=sys.stderr)
                    stats["bad"] += 1
                    idx += 1
                    continue
                stats["seen"] += 1
                c = canonicalise(pos, rot)
                if c is None:
                    stats["unclued"] += 1
                    idx += 1
                    continue
                orient, turn, canon = c
                stats["orient"][orient] += 1
                stats["kept"] += 1
                if progress_every and stats["kept"] % progress_every == 0:
                    print(f"[cons] {stats['kept']} clued boards", file=sys.stderr)
                yield (path, idx, cid, line, orient, turn, canon, pos)
                idx += 1


def new_stats():
    return {"seen": 0, "kept": 0, "unclued": 0, "bad": 0, "orient": [0, 0, 0, 0]}


# =============================================================================
# Scoring a board against the consensus
# =============================================================================

METRICS = ("lift", "logp", "rank", "top1")

def score_cells(cons, canon_pos, alpha, loo, min_support, box=None):
    """The four per-cell measures of one canonicalised board.

    Every measure is a MEAN over the cells actually scored, and `cells` is
    returned with them: a board scored on 140 cells and one scored on 250 are
    not making the same claim, and nothing downstream can tell them apart
    without that number.

    `loo` subtracts the board's own vote before reading the frequency, which
    is the whole of the self-bias a corpus scored against itself carries. It
    can empty a cell -- a piece only this board ever put there -- and --alpha
    is what keeps the logarithm finite when it does.
    """
    modes = cons.modes()
    ranks = cons.ranks()
    lift = logp = rankm = 0.0
    hits = 0
    n = 0
    for piece, cell in enumerate(canon_pos):
        if cell == UNPLACED:
            continue
        if box is not None and cell not in box:
            continue
        placed = cons.placed[cell]
        c = cons.count[cell].get(piece, 0)
        if loo:
            placed -= 1
            c -= 1
        if placed < min_support:
            continue
        k = CELL_K[cell]
        p = (c + alpha) / (placed + alpha * k)
        lp = math.log(p) / LOG2
        logp += lp
        lift += lp + math.log(k) / LOG2
        if piece == modes[cell]:
            hits += 1
        # Position in the cell's frequency order, 0 = the most common piece.
        # An unseen piece ranks behind everything observed, and the scale is
        # the cell's own pool, so all three kinds of cell stay comparable.
        pos = ranks[cell].get(piece, len(ranks[cell]))
        rankm += max(0.0, 1.0 - pos / max(1, k - 1))
        n += 1
    if not n:
        return dict(lift=0.0, logp=0.0, rank=0.0, top1=0.0, cells=0)
    return dict(lift=lift / n, logp=logp / n, rank=rankm / n,
                top1=hits / n, cells=n)


def bag_pieces(pos, depth, top):
    """The pieces that still have to END UP in one band, in the board's OWN frame.

    Everything already sitting in the band, plus every unplaced piece -- but
    the unplaced ones only when the band actually has holes to put them in.
    That one condition is what makes the same rule right at both ends of a
    bottom-up partial, instead of two rules that happen to agree:

        --best_top     the top band is all holes, so the bag is the 80 unplaced
                       pieces: exactly what must fill those rows.
        --best_bottom  the bottom band is already full, so the bag is the 80
                       pieces in it -- the unplaced ones are reserved for the
                       TOP and have no business in a bottom bag.
        a complete board, either end: no holes anywhere, so the bag is the
                       band's own 80 pieces.
        a sunk board with holes low down: the unplaced pieces rejoin the bottom
                       bag, because now they really can land there.

    It is exact whenever the board's holes are all inside the band or all
    outside it, which covers every shape Stage B and the finalizer produce. A
    board holed at both ends over-counts, so `bag` is a printed column."""
    lo, hi = (SIDE - depth, SIDE) if top else (0, depth)
    inside = [p for p, cell in enumerate(pos)
              if cell != UNPLACED and lo <= cell // SIDE < hi]
    if len(inside) == depth * SIDE:                  # the band is full
        return inside
    return inside + [p for p, cell in enumerate(pos) if cell == UNPLACED]


def score_bag(cons, canon_pos, bag, band, alpha, loo):
    """Bits of enrichment of a bag against one canonical band.

    q(p) is how often the corpus put piece p in band B out of every time it
    placed p at all -- location inside the band integrated out, which is the
    point: the bag has no order and no cells, only membership.

    The leave-one-out subtraction is not optional here the way it is for the
    per-cell metrics. A board's own placed bag pieces ARE its contribution to
    band B, so scoring against a table it is inside would be reading back its
    own answer."""
    cells = len(band_cells(band, cons.band_rows))
    f = cells / N_PIECES
    own_tot, own_band = set(), set()
    if loo:
        member = cons._band_member()
        for piece, cell in enumerate(canon_pos):
            if cell == UNPLACED:
                continue
            own_tot.add(piece)
            if band in member[cell]:
                own_band.add(piece)
    bits = 0.0
    seen = 0
    for p in bag:
        t = cons.tot[p] - (1 if p in own_tot else 0)
        b = cons.band[band][p] - (1 if p in own_band else 0)
        if t > 0:
            seen += 1
        q = (b + alpha * f) / (t + alpha)
        bits += math.log(q / f) / LOG2
    return bits / len(bag) if bag else 0.0, seen


def band_for(orient, base):
    """The canonical band a board's own band lands in.

    k = (4-o)%4 clockwise turns canonicalise the board, and one clockwise turn
    carries TOP to RIGHT, RIGHT to BOTTOM, BOTTOM to LEFT -- which is why the
    band order in BAND_NAMES is exactly that cycle, and why the four
    orientations need four different marginals rather than one."""
    return (base + canon_turn(orient)) % 4


# =============================================================================
# --border_out: the Stage A border the corpus implies
# =============================================================================
#
# Side and Corner indices here are the Stage A annealer's own (Side TOP=0
# RIGHT=1 BOTTOM=2 LEFT=3; Corner TL=0 TR=1 BR=2 BL=3), and piece ids inside
# this section are the annealer's 1-based ids -- 0-based everywhere else in
# this file. The conversion happens at the two boundaries and nowhere in
# between, which is the only way it stays checkable.

FLOOR_DECADE = 200.0        # points charged per decade a side sits below the floor

# --pin_clue N (the beamer and finalizer flag) to clue orientation. The same
# table as ORIENT_OF_QUADRANT in src/B_beam/E555_database.c: N runs
# anticlockwise from the lower left, so N-1 is deliberately NOT the orientation.
ORIENT_OF_PIN = {1: 0, 2: 3, 3: 2, 4: 1}
PIN_NAMES = {1: "lower-left", 2: "lower-right", 3: "upper-right", 4: "upper-left"}


def load_annealer():
    """Import the Stage A annealer for its Euler machinery and its move set.

    Deliberately lazy: everything except --border_out works without it, and a
    checkout missing src/ should still rank a pool rather than fail at import."""
    path = Path(__file__).resolve().parent.parent / "src" / "A_border"
    if not (path / "E555_edge_annealer.py").exists():
        raise SystemExit(f"[ERROR] --border_out needs the Stage A annealer at "
                         f"{path / 'E555_edge_annealer.py'}, which is not there")
    sys.path.insert(0, str(path))
    import E555_edge_annealer as A                    # noqa: E402
    return A


def side_cells():
    """The 14 non-corner cells of each side, in Side order."""
    top = tuple((SIDE - 1) * SIDE + c for c in range(1, SIDE - 1))
    right = tuple(r * SIDE + (SIDE - 1) for r in range(1, SIDE - 1))
    bottom = tuple(c for c in range(1, SIDE - 1))
    left = tuple(r * SIDE for r in range(1, SIDE - 1))
    return (top, right, bottom, left)


def border_affinity(cons, alpha):
    """reward[piece][side] and reward[piece][corner] in bits, 0-based pieces.

    The same quantity `lift` measures, marginalised over a side: how much more
    often the corpus put this piece on this side than a flat split would, in
    bits. A piece the corpus never placed on the frame reads 0 on all four,
    which is the right answer -- no evidence, no preference."""
    sides = side_cells()
    rew_e = [[0.0] * 4 for _ in range(N_PIECES)]
    rew_c = [[0.0] * 4 for _ in range(N_PIECES)]
    seen_e = seen_c = 0
    for table, cellsets, rew in ((0, sides, rew_e), (1, tuple((c,) for c in CORNER_CELLS), rew_c)):
        raw = [[0] * 4 for _ in range(N_PIECES)]
        for s, cells in enumerate(cellsets):
            for cell in cells:
                for piece, n in cons.count[cell].items():
                    raw[piece][s] += n
        for piece in range(N_PIECES):
            tot = sum(raw[piece])
            if tot:
                if table == 0:
                    seen_e += 1
                else:
                    seen_c += 1
            for s in range(4):
                p = (raw[piece][s] + alpha / 4.0) / (tot + alpha)
                rew[piece][s] = math.log(4.0 * p) / LOG2
    return rew_e, rew_c, seen_e, seen_c


def hungarian(cost):
    """Minimum-cost perfect assignment of n rows to n columns (JV / e-maxx).

    56x56 here, so O(n^3) is instant and exactness is free -- which matters,
    because this number is reported as the ceiling the feasible border is
    measured against, and a heuristic ceiling would not be one."""
    n = len(cost)
    m = len(cost[0])
    INF = float("inf")
    u = [0.0] * (n + 1)
    v = [0.0] * (m + 1)
    p = [0] * (m + 1)
    way = [0] * (m + 1)
    for i in range(1, n + 1):
        p[0] = i
        j0 = 0
        minv = [INF] * (m + 1)
        used = [False] * (m + 1)
        while True:
            used[j0] = True
            i0 = p[j0]
            delta = INF
            j1 = -1
            row = cost[i0 - 1]
            for j in range(1, m + 1):
                if not used[j]:
                    cur = row[j - 1] - u[i0] - v[j]
                    if cur < minv[j]:
                        minv[j] = cur
                        way[j] = j0
                    if minv[j] < delta:
                        delta = minv[j]
                        j1 = j
            for j in range(m + 1):
                if used[j]:
                    u[p[j]] += delta
                    v[j] -= delta
                else:
                    minv[j] -= delta
            j0 = j1
            if p[j0] == 0:
                break
        while j0:
            j1 = way[j0]
            p[j0] = p[j1]
            j0 = j1
    out = [0] * n
    for j in range(1, m + 1):
        if p[j]:
            out[p[j] - 1] = j - 1
    return out


def assignment_optimum(rew_e, rew_c, edge_ids, corner_ids, A):
    """The affinity ceiling: the best side assignment ignoring Euler trails.

    Exact. 56 edges to four sides of 14 is a transportation problem, solved
    here by replicating each side 14 times into a square matrix; the four
    corners are 24 permutations, brute-forced. Neither result is usable on its
    own -- that is what the search below is for -- but both are printed, so
    the price feasibility charges is a number and not a feeling."""
    cols = [s for s in range(4) for _ in range(SIDE - 2)]
    cost = [[-rew_e[pid - 1][s] for s in cols] for pid in edge_ids]
    pick = hungarian(cost)
    edge_side = {pid: A.Side(cols[pick[i]]) for i, pid in enumerate(edge_ids)}
    edge_aff = sum(rew_e[pid - 1][int(s)] for pid, s in edge_side.items())

    best = None
    for perm in itertools.permutations(range(4)):
        val = sum(rew_c[corner_ids[i] - 1][perm[i]] for i in range(4))
        if best is None or val > best[0]:
            best = (val, perm)
    corner_pos = {corner_ids[i]: A.Corner(best[1][i]) for i in range(4)}
    return edge_side, corner_pos, edge_aff + best[0]


def _random_assignment(A, edge_ids, corner_ids, rng):
    ids = list(edge_ids)
    rng.shuffle(ids)
    per = SIDE - 2
    edge_side = {pid: A.Side(i // per) for i, pid in enumerate(ids)}
    perm = list(range(4))
    rng.shuffle(perm)
    corner_pos = {corner_ids[i]: A.Corner(perm[i]) for i in range(4)}
    return edge_side, corner_pos


def _perturb(edge_side, n, rng):
    out = dict(edge_side)
    ids = list(out)
    for _ in range(n):
        a, b = rng.sample(ids, 2)
        out[a], out[b] = out[b], out[a]
    return out


def search_border(A, rew_e, rew_c, seed_path, args, log):
    """Find a border that matches the consensus AND that Stage B can lay.

    Seeded from the exact affinity optimum, then walked with the annealer's own
    move set -- try_edge_swap does the arcs, the Euler recount and the
    inventory check; only its score is replaced, because the objective here is
    not the annealer's. Bounded by the clock: whatever exists when the budget
    runs out is what comes back, so this cannot hang."""
    pieces = A.read_pieces(str(seed_path))
    pieces_by_id = {p.id: p for p in pieces}
    corner_ids, edge_ids = A.classify_boundary_pieces(pieces)
    inner_cap = A.build_inner_capacity(pieces)
    cfg = A.AnnealingConfig()

    opt_edge, opt_corner, ceiling = assignment_optimum(rew_e, rew_c, edge_ids,
                                                       corner_ids, A)
    log(f"[border] affinity ceiling {ceiling:+.2f} bits over 60 pieces "
        f"({ceiling / 60:+.3f} a piece), ignoring Euler feasibility")

    w_c, w_t, floor_at = args.w_consensus, args.w_trails, args.min_trails

    def edge_aff(es):
        return sum(rew_e[pid - 1][int(s)] for pid, s in es.items())

    def corner_aff(cp):
        return sum(rew_c[pid - 1][int(c)] for pid, c in cp.items())

    def composite(aff, evals, hard):
        trails = floor_pen = 0.0
        for s in A.Side:
            ec = evals[s].euler_count
            if ec > 0:
                trails += math.log10(ec)
            if ec < floor_at:
                floor_pen += FLOOR_DECADE * math.log10(floor_at / max(1, ec))
        return w_c * aff + w_t * trails - floor_pen - hard

    def snapshot(state, aff, score, restart, step):
        counts = {int(s): state.evals[s].euler_count for s in A.Side}
        at_floor = all(v >= floor_at for v in counts.values())
        return dict(score=score, aff=aff, counts=counts, at_floor=at_floor,
                    feasible=all(state.evals[s].feasible for s in A.Side),
                    edge_side=dict(state.edge_side), corner_pos=dict(state.corner_pos),
                    restart=restart, step=step)

    def key(rec):
        return (rec["at_floor"], rec["score"])

    def signature(rec):
        return (tuple(sorted((p, int(s)) for p, s in rec["edge_side"].items())),
                tuple(sorted((p, int(c)) for p, c in rec["corner_pos"].items())))

    t0 = time.monotonic()
    deadline = t0 + args.border_time
    rng = random.Random(args.border_seed)
    found = {}                       # signature -> the best record carrying it
    cap = 32 * args.border_rows      # bound on `found`, so a long run cannot grow
    keep = (False, float("-inf"))    # the worst key still worth snapshotting
    best = None
    restart = 0
    steps_done = 0

    def harvest(state, aff, score, restart, step):
        """Keep a state worth emitting. Called for every state the walk
        occupies, its start included: a restart that ends worse than it peaked
        still walked past the peak, and the peak is what gets emitted.

        A snapshot costs O(60), so at the default --border_rows 1 one is taken
        only when the running best is beaten and `found` stays a handful of
        entries; asking for more borders is what makes it keep near-misses, and
        `cap` is what stops that growing with the budget."""
        nonlocal best, found, keep
        k = key_cheap(state, score, floor_at, A)
        if not (best is None or k > key(best) or
                (args.border_rows > 1 and k >= keep)):
            return
        rec = snapshot(state, aff, score, restart, step)
        sig = signature(rec)
        if sig not in found or k > key(found[sig]):
            found[sig] = rec
        if best is None or k > key(best):
            best = rec
        if len(found) > cap:
            order = sorted(found.items(), key=lambda kv: key(kv[1]),
                           reverse=True)[:cap]
            found = dict(order)
            keep = key(order[-1][1])

    while time.monotonic() < deadline:
        if args.border_restarts and restart >= args.border_restarts:
            break
        if restart == 0:
            es, cp = dict(opt_edge), dict(opt_corner)
        elif restart % 2:
            es, cp = _perturb(opt_edge, 6 + 2 * restart, rng), dict(opt_corner)
        else:
            es, cp = _random_assignment(A, edge_ids, corner_ids, rng)

        state = A._build_run_state(pieces_by_id, es, cp, inner_cap, cfg)
        aff = edge_aff(state.edge_side) + corner_aff(state.corner_pos)
        hard = A.hard_penalty(state.evals, state.inward_tally, inner_cap, cfg)
        cur = composite(aff, state.evals, hard)
        state.score = cur
        harvest(state, aff, cur, restart, -1)

        n = args.border_steps
        ratio = (args.border_Tf / args.border_T0) ** (1.0 / max(1, n - 1))
        temp = args.border_T0
        for step in range(n):
            if time.monotonic() >= deadline:
                break
            steps_done += 1
            temp *= ratio
            # Corner moves change every side's endpoints at once, so they are
            # the expensive move and the one that matters early: they are what
            # lets a state that cannot balance escape. Confined to the first
            # third of a restart, as in the annealer.
            if rng.random() < 0.05 and step < n // 3:
                a, b = rng.sample(corner_ids, 2)
                r = A.try_corner_swap(state, pieces_by_id, a, b, inner_cap, cfg)
                new_cp = dict(state.corner_pos)
                new_cp[a], new_cp[b] = new_cp[b], new_cp[a]
                new_aff = edge_aff(state.edge_side) + corner_aff(new_cp)
                h = A.hard_penalty(r.new_evals, state.inward_tally, inner_cap, cfg)
                new = composite(new_aff, r.new_evals, h)
                if new > cur or rng.random() < math.exp(min(0.0, (new - cur) / temp)):
                    A.commit_corner_swap(state, a, b, r)
                    state.score = cur = new
                    aff = new_aff
            else:
                a, b = rng.sample(edge_ids, 2)
                sa, sb = state.edge_side[a], state.edge_side[b]
                if sa == sb:
                    continue
                r = A.try_edge_swap(state, pieces_by_id, a, b, inner_cap, cfg)
                if r is None:
                    continue
                d = (rew_e[a - 1][int(sb)] + rew_e[b - 1][int(sa)]
                     - rew_e[a - 1][int(sa)] - rew_e[b - 1][int(sb)])
                new_aff = aff + d
                new_evals = {**state.evals, r.side_a: r.new_se_a, r.side_b: r.new_se_b}
                new = composite(new_aff, new_evals, r.hard)
                if new > cur or rng.random() < math.exp(min(0.0, (new - cur) / temp)):
                    A.commit_edge_swap(state, a, b, r)
                    state.score = cur = new
                    aff = new_aff

            harvest(state, aff, cur, restart, step)
        restart += 1

    log(f"[border] {restart} restart(s), {steps_done} step(s), "
        f"{time.monotonic() - t0:.1f}s of {args.border_time:g}s budget")
    ranked = sorted(found.values(), key=key, reverse=True)
    return ranked, ceiling, pieces_by_id, inner_cap, cfg


def key_cheap(state, score, floor_at, A):
    """The best-tracking key without building a snapshot: (at floor, score)."""
    return (all(state.evals[s].euler_count >= floor_at for s in A.Side), score)


def emit_border(path, records, args, A, pieces_by_id, inner_cap, cfg,
                seed, ceiling, cons, sources, log):
    """Write the rotations CSV, in the annealer's own format.

    Every row is rebuilt from its assignment with _build_run_state rather than
    carried out of the search, so the trail counts in the comment are recounted
    from the border actually being written, and then validated with
    E555_rotate.classify_border -- the 14/14/14/14-plus-four-corners test
    classify_deal_from_rotations applies in E555_database.c. A row that fails
    it is a bug here, and the file is not written at all rather than handed to
    the beamer to die on."""
    turn = ORIENT_OF_PIN[args.border_pin]
    rows = []
    for i, rec in enumerate(records[:args.border_rows]):
        state = A._build_run_state(pieces_by_id, dict(rec["edge_side"]),
                                   dict(rec["corner_pos"]), inner_cap, cfg)
        counts = {int(s): state.evals[s].euler_count for s in A.Side}
        if counts != rec["counts"]:
            raise SystemExit("[ERROR] a border's trail counts did not survive the "
                             f"rebuild: {rec['counts']} became {counts}")
        full = list(A.rotation_vector(pieces_by_id, state, 60)) + [0] * (N_PIECES - 60)
        if turn:
            # Only the 60 pieces with a grey side carry a meaningful spin; the
            # same map E555_rotate.py --rotations applies, for the same reason.
            for pid in range(60):
                full[pid] = (full[pid] + 3 * turn) % 4
        cls = RT.classify_border(seed, full)
        sizes = [len(cls[s]) for s in RT.SIDE_NAMES]
        if len(cls["corner"]) != 4 or any(n != SIDE - 2 for n in sizes):
            raise SystemExit(f"[ERROR] row {i} is not a legal 14/14/14/14 border "
                             f"partition ({sizes}, {len(cls['corner'])} corners); "
                             "nothing written")
        rows.append((rec, counts, full))

    with open(path, "w", newline="") as fh:
        fh.write("# E555_extract_consensus.py: the border implied by %d clued board(s)\n"
                 % cons.boards)
        fh.write("# from %s\n" % ", ".join(sources))
        fh.write("# frame: centre clue at the %s quadrant -- use with --pin_clue %d\n"
                 % (PIN_NAMES[args.border_pin], args.border_pin))
        fh.write("# affinity ceiling %+.2f bits (no Euler constraint); floor "
                 "--min_trails %d\n" % (ceiling, args.min_trails))
        w = csv.writer(fh, lineterminator="\n")
        for i, (rec, counts, full) in enumerate(rows):
            tag = "  ".join("%s=%d" % (A.SIDE_NAMES[A.Side(s)], counts[s])
                            for s in range(4))
            fh.write("#  %s  Affinity=%+.2f  Score=%.4f\n" % (tag, rec["aff"], rec["score"]))
            w.writerow(["c%d" % i] + [str(v) for v in full])

    for i, (rec, counts, _) in enumerate(rows):
        tag = "  ".join("%s=%d" % (A.SIDE_NAMES[A.Side(s)], counts[s]) for s in range(4))
        short = min(counts.values())
        mark = "" if short >= args.min_trails else "   <-- BELOW --min_trails"
        log("[border] row %d  %s  affinity %+.2f of %+.2f bits%s"
            % (i, tag, rec["aff"], ceiling, mark))
    worst = min((min(c.values()) for _, c, _ in rows), default=0)
    if worst < args.min_trails:
        # To stderr, and not through `log`: a border Stage B cannot lay well is
        # the one thing --quiet must not be able to hide.
        print("[border] WARNING: the budget ran out before every side reached "
              "--min_trails %d (worst side %d). Raise --border_time, or accept "
              "a less flexible border." % (args.min_trails, worst), file=sys.stderr)
    log("[emit] %d border row(s) -> %s" % (len(rows), path))


# =============================================================================
# Ranking, output and the command line
# =============================================================================

RECORD_OVERHEAD = 1.8       # measured in E555_rank.py; the record shape is the same

def collect(records, top):
    """Rank best-first, keeping only the best `top` when one is asked for.

    With --top N this is a bounded heap -- N records live at once whatever the
    corpus holds, which is what lets a multi-GB pool be ranked at all. The
    input position closes the key so ties keep input order and the heap never
    has to compare the records themselves."""
    if top <= 0:
        out = [((-r["sort"], i), r) for i, r in enumerate(records)]
        out.sort()
        return [r for _, r in out]
    heap = []
    for i, r in enumerate(records):
        k = (r["sort"], -i)
        if len(heap) < top:
            heapq.heappush(heap, (k, i, r))
        elif k > heap[0][0]:
            heapq.heapreplace(heap, (k, i, r))
    return [r for _, _, r in sorted(heap, key=lambda t: (-t[0][0], t[1]))]


def check_memory(paths, args):
    """Refuse a run projected to exceed --max_mem. A plain --top streams."""
    if args.top > 0:
        return
    size = sum(Path(p).stat().st_size for p in paths)
    want = size * RECORD_OVERHEAD
    if want > args.max_mem * (1 << 30):
        raise SystemExit(
            f"[ERROR] these inputs would need up to about {want / (1<<30):.2g} GB, "
            f"over the --max_mem limit of {args.max_mem:g} GB.\n"
            f"        Add --top N to stream with bounded memory, or raise --max_mem.")


def report_corpus(cons, stats, log):
    log(f"[cons] {stats['kept']} clued board(s) of {stats['seen']} read"
        + (f"; {stats['unclued']} carried no centre clue" if stats["unclued"] else "")
        + (f"; {stats['bad']} unreadable" if stats["bad"] else ""))
    if stats["kept"]:
        # Named by the --pin_clue quadrant rather than by the internal
        # orientation index, since that is what the user types at the beamer.
        pin_of = {o: n for n, o in ORIENT_OF_PIN.items()}
        log("[cons] clue quadrants: "
            + "  ".join(f"{PIN_NAMES[pin_of[o]]}={stats['orient'][o]}"
                        for o in range(4)))
    log("[cons] band coverage (boards that filled each canonical band, and "
        "boards that merely touched it):\n"
        + "       " + "  ".join(f"{BAND_NAMES[b]}={cons.band_boards[b]}"
                                f"/{cons.band_touch[b]}" for b in range(4)))


# The compact monitor table. `cells` is never dropped from it: it is the number
# that says whether two boards' means are comparable at all, and a ranking that
# hides it invites exactly the mistake of preferring a board scored on 40 cells.
COLUMNS = ("orient", "cells", "placed", "lift", "logp", "rank", "top1")
BAND_COLUMNS = ("orient", "band", "bag", "known", "bits", "cells", "lift", "top1")
CSV_COLUMNS = ("orient", "band", "bag", "known", "bits", "cells", "placed",
               "lift", "logp", "rank", "top1")
DECIMALS = {"lift": 3, "logp": 3, "rank": 3, "top1": 3, "bits": 3}


def cell_text(k, v):
    return f"{v:.{DECIMALS[k]}f}" if k in DECIMALS else str(v)


def print_table(records, args, band_mode):
    """The ranking, as E555_rank.py prints one: --csv for a machine, otherwise
    a fixed-width table that lines up without a separator row."""
    multi = len(args.inputs) > 1
    if args.csv:
        w = csv.writer(sys.stdout, lineterminator="\n")
        w.writerow((["file"] if multi else []) + ["row", "id"] + list(CSV_COLUMNS))
        for r in records:
            w.writerow(([r["file"]] if multi else []) + [r["row"], r["id"]]
                       + [cell_text(k, r[k]) for k in CSV_COLUMNS])
        return
    if args.quiet:
        return
    if not records:
        print("E555 consensus: no board was scored")
        return

    sort_name = ("bits, the bag against its band" if band_mode
                 else f"{args.metric}, per placed cell")
    print(f"E555 consensus: {len(records)} board(s), sort: {sort_name}")

    cols = BAND_COLUMNS if band_mode else COLUMNS
    idw = 0 if args.no_id else min(20, max(len(r["id"]) for r in records))
    idh = "" if args.no_id else f"{'id':<{idw}}  "
    fw = max((len(r["file"]) for r in records), default=4) if multi else 0
    head = (f"{'file':<{fw}} " if multi else "") + f"{'row':>5}  " + idh + \
           "  ".join(f"{k:>{max(6, len(k))}}" for k in cols)
    print(head)
    print("-" * len(head))
    for r in records:
        cid = "" if args.no_id else \
              (r["id"] if len(r["id"]) <= idw else r["id"][:idw - 1] + "~").ljust(idw) + "  "
        print((f"{r['file']:<{fw}} " if multi else "") + f"{r['row']:>5}  " + cid +
              "  ".join(f"{cell_text(k, r[k]):>{max(6, len(k))}}" for k in cols))

    b = records[0]
    if band_mode:
        print(f"best: {b['id']}  bits={b['bits']:+.3f} band={b['band']} "
              f"bag={b['bag']} placed={b['placed']}")
    else:
        print(f"best: {b['id']}  lift={b['lift']:+.3f} top1={b['top1']:.3f} "
              f"cells={b['cells']} placed={b['placed']}")


def main():
    ap = argparse.ArgumentParser(
        description="Rank clued partial boards by how well they match the "
                    "consensus of the whole corpus, and distil that consensus "
                    "into a Stage A border.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="metrics: " + ", ".join(METRICS))
    ap.add_argument("inputs", nargs="+", help="one or more board CSVs")

    ap.add_argument("--metric", choices=METRICS, default="lift",
                    help="which per-cell measure sorts the table (default lift, "
                         "bits above chance). All four are always printed.")
    ap.add_argument("--alpha", type=float, default=0.5, metavar="A",
                    help="additive smoothing on the frequency estimate "
                         "(default 0.5); keeps the logarithm finite when "
                         "leave-one-out empties a cell")
    ap.add_argument("--loo", choices=("auto", "on", "off"), default="auto",
                    help="subtract the board's own vote before scoring it "
                         "(default auto: on for a consensus built from these "
                         "inputs, off for --consensus_in)")
    ap.add_argument("--min_support", type=int, default=2, metavar="N",
                    help="skip cells fewer than N boards placed (default 2)")
    ap.add_argument("--box", metavar="R0:R1,C0:C1",
                    help="score only this inclusive rectangle, in CANONICAL "
                         "coordinates (centre clue at the lower left)")

    ap.add_argument("--best_top", action="store_true",
                    help="score the bag of pieces left for the top --band_rows "
                         "rows of each board's own frame, instead of its cells")
    ap.add_argument("--best_bottom", action="store_true",
                    help="the same for the bottom --band_rows rows")
    ap.add_argument("--band_rows", type=int, default=5, metavar="D",
                    help="depth of the band --best_top/--best_bottom score "
                         "(default 5)")
    ap.add_argument("--min_band_support", type=int, default=25, metavar="N",
                    help="refuse to score a board against a canonical band "
                         "fewer than N boards ever FILLED (default 25); such "
                         "boards are skipped and counted. Touching a band is "
                         "not enough -- a pool of bottom-up partials touches "
                         "the side bands with every board while covering half "
                         "of each. 0 disables the guard.")

    ap.add_argument("--top", type=int, default=0, help="show only the best N boards")
    ap.add_argument("--out", metavar="FILE",
                    help="write the surviving input rows, re-ordered, VERBATIM "
                         "to FILE -- so the boards keep their own un-rotated "
                         "frame and their unplaced rows")
    ap.add_argument("--csv", action="store_true",
                    help="print the measures as CSV instead of a table")
    ap.add_argument("--no_id", action="store_true", help="drop the board-id column")
    ap.add_argument("--quiet", action="store_true",
                    help="write the outputs without printing the table")
    ap.add_argument("--count", action="store_true",
                    help="print just the number of input rows carrying the "
                         "centre clue, one bare number, and exit")
    ap.add_argument("--max_mem", type=float, default=8.0, metavar="GB",
                    help="refuse an input projected to need more than this "
                         "much memory (default 8); --top streams instead")
    ap.add_argument("--progress_every", type=int, default=25000, metavar="N",
                    help="progress line every N clued boards (0 = silent)")
    ap.add_argument("--seed_file", help="piece seed file (default: data/seed_Edge5.txt)")

    ap.add_argument("--consensus_out", metavar="FILE",
                    help="write the frequency table for a later run to score against")
    ap.add_argument("--consensus_in", metavar="FILE",
                    help="score against a table built elsewhere instead of "
                         "building one from the inputs; removes the self-vote "
                         "outright")

    ap.add_argument("--border_out", metavar="FILE",
                    help="write the Stage A rotations CSV the consensus implies")
    ap.add_argument("--border_pin", type=int, default=1, metavar="N",
                    help="emit the border in the frame of --pin_clue N: "
                         "1 = lower-left (default, the canonical frame), "
                         "2 = lower-right, 3 = upper-right, 4 = upper-left")
    ap.add_argument("--border_rows", type=int, default=1, metavar="N",
                    help="emit the N best distinct borders (default 1)")
    ap.add_argument("--min_trails", type=int, default=1000, metavar="N",
                    help="floor on every side's Euler-trail count (default "
                         "1000), charged at 200 points a decade below it")
    ap.add_argument("--w_consensus", type=float, default=1.0, metavar="W",
                    help="weight on the affinity, in bits (default 1.0)")
    ap.add_argument("--w_trails", type=float, default=0.5, metavar="W",
                    help="weight on the sum of log10 trail counts above the "
                         "floor (default 0.5)")
    ap.add_argument("--border_time", type=float, default=120.0, metavar="S",
                    help="wall-clock budget for the border search (default "
                         "120). Whatever it has when the clock runs out is "
                         "emitted, so the search cannot hang.")
    ap.add_argument("--border_steps", type=int, default=100000, metavar="N",
                    help="steps in one restart (default 100000)")
    ap.add_argument("--border_restarts", type=int, default=0, metavar="N",
                    help="cap the restarts (default 0 = until the clock)")
    ap.add_argument("--border_T0", type=float, default=60.0, metavar="T")
    ap.add_argument("--border_Tf", type=float, default=0.5, metavar="T")
    ap.add_argument("--border_seed", type=int, default=0, metavar="N",
                    help="RNG seed for the border search (default 0)")
    args = ap.parse_args()

    if args.best_top and args.best_bottom:
        raise SystemExit("[ERROR] --best_top and --best_bottom score different "
                         "bags; ask for one or the other")
    if not 1 <= args.band_rows <= SIDE // 2:
        raise SystemExit(f"[ERROR] --band_rows must be 1..{SIDE // 2}")
    if args.border_pin not in ORIENT_OF_PIN:
        raise SystemExit("[ERROR] --border_pin takes 1..4 (1 = lower-left, "
                         "2 = lower-right, 3 = upper-right, 4 = upper-left)")
    if args.border_rows < 1:
        raise SystemExit("[ERROR] --border_rows must be at least 1")
    if args.alpha <= 0:
        raise SystemExit("[ERROR] --alpha must be positive")

    log = (lambda s: None) if args.quiet else (lambda s: print(s))
    band_mode = args.best_top or args.best_bottom
    box = frozenset(R.parse_box(args.box)) if args.box else None
    seed = V.load_seed(V.find_seed(args.seed_file))

    stats = new_stats()
    if args.count:
        for _ in iter_boards(args.inputs, stats, 0):
            pass
        print(stats["kept"])
        return 1 if stats["bad"] else 0

    check_memory(args.inputs, args)

    # -- pass 1: the consensus ------------------------------------------------
    if args.consensus_in:
        cons = Consensus.read(args.consensus_in)
        if cons.band_rows != args.band_rows and band_mode:
            raise SystemExit(f"[ERROR] {args.consensus_in} was built with "
                             f"--band_rows {cons.band_rows}, not {args.band_rows}; "
                             "the band marginals it carries are for that depth")
        log(f"[cons] read {cons.boards} board(s) of consensus from {args.consensus_in}")
        loo = args.loo == "on"
    else:
        cons = Consensus(band_rows=args.band_rows)
        for rec in iter_boards(args.inputs, stats, args.progress_every):
            cons.add(rec[6])
        cons.sources = [Path(p).name for p in args.inputs]
        report_corpus(cons, stats, log)
        if not cons.boards:
            raise SystemExit("[ERROR] no board in the input carries the centre "
                             "clue, so there is no consensus to build")
        loo = args.loo != "off"
    if args.consensus_out:
        cons.write(args.consensus_out)
        log(f"[emit] consensus of {cons.boards} board(s) -> {args.consensus_out}")

    # -- the border, which needs only the consensus ---------------------------
    if args.border_out:
        A = load_annealer()
        rew_e, rew_c, seen_e, seen_c = border_affinity(cons, args.alpha)
        log(f"[border] {seen_e} of 56 edge pieces and {seen_c} of 4 corner "
            f"pieces were observed on the frame; the rest read flat")
        ranked, ceiling, pieces_by_id, inner_cap, cfg = search_border(
            A, rew_e, rew_c, V.find_seed(args.seed_file), args, log)
        if not ranked:
            raise SystemExit("[ERROR] the border search produced nothing at all; "
                             "raise --border_time")
        emit_border(args.border_out, ranked, args, A, pieces_by_id, inner_cap,
                    cfg, seed, ceiling, cons, cons.sources, log)

    # -- pass 2: score ---------------------------------------------------------
    base = BAND_TOP if args.best_top else BAND_BOTTOM
    thin = [0]
    stats2 = new_stats()

    def stream():
        for path, idx, cid, line, orient, turn, canon, pos in iter_boards(
                args.inputs, stats2, args.progress_every):
            m = score_cells(cons, canon, args.alpha, loo, args.min_support, box)
            m["placed"] = sum(1 for c in canon if c != UNPLACED)
            m["orient"] = orient
            if band_mode:
                b = band_for(orient, base)
                if args.min_band_support and cons.band_boards[b] < args.min_band_support:
                    thin[0] += 1
                    continue
                # The bag is read off the board's OWN frame; the band it is
                # scored against is the canonical one that frame lands in.
                bag = bag_pieces(pos, args.band_rows, args.best_top)
                bits, known = score_bag(cons, canon, bag, b, args.alpha, loo)
                m.update(band=BAND_NAMES[b], bag=len(bag), known=known,
                         bits=bits, sort=bits)
            else:
                m.update(band="-", bag=0, known=0, bits=0.0, sort=m[args.metric])
            yield dict(file=Path(path).name, row=idx, id=cid, line=line, **m)

    records = collect(stream(), args.top)
    if band_mode and thin[0]:
        log(f"[band] {thin[0]} board(s) skipped: their band has fewer than "
            f"--min_band_support {args.min_band_support} boards behind it")

    if args.out:
        with open(args.out, "w", newline="") as out:
            for r in records:
                out.write(r["line"] if r["line"].endswith("\n") else r["line"] + "\n")
        log(f"[emit] {len(records)} verbatim row(s) -> {args.out}")

    print_table(records, args, band_mode)
    return 1 if (stats["bad"] or stats2["bad"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
