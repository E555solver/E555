# E555 -- a beam-search pipeline for Eternity II

Technical reference: algorithms, scoring, every tool's options, and file
formats. Measurements quoted here are from the current code unless stated.

## 1. The puzzle

Eternity II is a 16x16 edge-matching puzzle with 256 square pieces. A board is
legal when every one of the 480 interior junctions joins two equal colours and
every outward face carries the frame colour. No complete solution is published;
the best public boards match 470 of 480 junctions. The best board in this
repository is `data/best_465.csv` (465/480).

**Colours** (`data/seed_Edge5.txt`, one piece per line, `top right bottom left`):

| colour | role |
|---|---|
| 0 | frame: every outward face |
| 1-5 | frame-interface colours, between adjacent border pieces |
| 6-22 | the 17 inner colours: inner pieces and the inward face of border pieces |

**Pieces**: 4 corners (two frame faces), 56 edges (one), 196 inner pieces (none).

**Clues.** Five published hint pieces, in this repository's numbering (0-based
ids, rows bottom-up, spins counter-clockwise), at orientation 0:

| piece | cell | (row, col) | spin |
|---|---|---|---|
| 138 (centre) | 119 | (7, 7) | 0 |
| 180 | 34 | (2, 2) | 0 |
| 248 | 45 | (2, 13) | 3 |
| 207 | 210 | (13, 2) | 3 |
| 254 | 221 | (13, 13) | 1 |

A board built from our own border does not know which puzzle side is its
row 0, so the clue set is used at all four orientations (`g_clue[4][5]` in
`src/B_beam/E555_database.c`; a quarter-turn maps one orientation to the next).
The centre clue takes one of the cells 119, 120, 135, 136 by orientation; the
corner clues always occupy (2,2), (2,13), (13,2), (13,13), with different pieces.

## 2. Conventions and the board CSV

- Rows and columns are 0-indexed **bottom-up**: row 0 is the bottom border,
  col 0 the left border, cell = `row*16 + col`.
- Spins are counter-clockwise quarter-turns `s` in 0..3: side `d` of a placed
  piece (0 top, 1 right, 2 bottom, 3 left) reads seed side `(d+s) mod 4`.
- **One CSV dialect** connects every tool:

  ```
  config_id, score, pos[0..255], rot[0..255]          (514 fields)
  ```

  `pos[p]` is the cell of piece `p` (999 = unplaced), `rot[p]` its spin.
  Readers take the **last 512 fields** as `pos, rot` and treat anything before
  them as metadata, so every producer's rows parse everywhere (Stage B writes a
  solution index in field 2, or the matched-edge count under `--end_dive`;
  the backtracker's `--resume` rows put a resume identifier third, 515 fields).
  Lines starting with `#` or `%` are comments. `tools/E555_rank.py --out F
  --rescore` rewrites any file canonically with field 2 = matched edges.
- Scores are matched interior junctions, 0..480. A partial board's score counts
  only junctions between two placed pieces.

## 3. The pipeline

```
Stage A  E555_edge_annealer.py   assign the 60 border pieces to sides     -> rotations.csv
         (or beamer --random_edges: sample borders inside Stage B)
Stage B  E555_beamer             beam rows 1..N, exhaustive search to the stop row,
                                 column-major extension, end dives       -> boards
         E555_finalizer          the same machinery restarted from a partial
         E555_roundhouse         turn the board, refill W-wide strips exhaustively
Stage C  E555_distiller.py       screen a corpus with dives, keep the N best
         E555_diver              holes, clues, zero-break rebuild to a stop row, then
                                 end dives + polish on any board; --reopen re-dives
         E555_ender.py           exact CP-SAT regions + redive; never returns worse
         E555_topper.py          CP-SAT break minimizer over border bands
         E555_backtracker        exact / bounded-mismatch DFS; band enumeration
```

Stage B grows every board upward, one complete row at a time, from a fixed
border. It is not exhaustive over configurations: most (bottom, left column)
pairs die within a few rows, so it plays many of them cheaply and keeps the
survivors. Stage C spends real time on few boards.

### 3.1 Recommended workflow

The current production method is a narrow beam to row 6 followed by the
exhaustive backtracker to row 11 (§5.9), with the boards finished by end dives
(§5.11). The backtracker writes hundreds of times more row-11 boards per
core-hour than the beam alone, which rarely reaches row 11 at all (§5.13).

```bash
# Stage A: borders (or skip it: beamer --random_edges)
python3 src/A_border/E555_edge_annealer.py data/seed_Edge5.txt \
    --restarts 16 --steps 500000 --out rotations.csv

# Stage B: beam to row 6, exhaustive search to row 11, dive every board
bin/E555_beamer data/seed_Edge5.txt rotations.csv --start_row R --num_rows N \
    --clue_center --beam_width 10000 --backtrack_row 6 --stop_row 11 \
    --top_bottoms 6 --top_columns 12 --tau_bottoms 0.1 --tau_columns 0.1 \
    --extend_nodes 0 --end_dive 10000 --end_polish 20000 --emit_score 452 \
    --prefix run1 --db_file chain_center.db --out_dir beam_R

# Stage C: keep the best 25, then close them
python3 tools/E555_distiller.py beam_R/*.csv --top 25 --out best.csv
python3 src/C_tail/E555_ender.py data/seed_Edge5.txt best.csv closed.csv --profile deep
```

`--extend_nodes 0` because every stop-row board is dived (§5.10); to stop lower
and keep only boards that grow further, use `--stop_row 9 --backtrack_min_col 3`
instead. The database cache depends on the clue flags, hence its own file.

`examples/` holds one small script per tool (start with
`examples/01_beamer_quickstart.sh`); `pipeline/` holds long unattended runs
(`run_pipeline.sh`, the board farm `run_farm.py`, the whirlpool, the topper
sweeps and `slurm_wrapper.sh`). Scripts take their settings as `NAME=value`
arguments and pass `--print_cmd`, so logs record the exact commands.

---

## 4. Stage A -- `E555_edge_annealer.py`

`src/A_border/E555_edge_annealer.py`, standard library only.

### 4.1 Model

A border assigns the 56 edge pieces to the four sides (14 each) and the four
corners to the corners. Each side is a directed multigraph on the
frame-interface colours: every edge piece is one arc (the colour pair it
exposes along the border), and the corners fix the endpoints. A legal ordering
of a side's pieces is exactly an Euler trail of that graph, and the trail count
is exact by the BEST theorem:

```
#trails = t_w(G) * prod_v (outdeg(v) - 1)!
```

with the arborescence count `t_w(G)` a cofactor of the directed Laplacian,
computed by integer Bareiss elimination. The side's count is the number of
orderings Stage B enumerates for it: a border scored `BOTTOM=1152 LEFT=2880`
gives the beamer `bottoms=1152` and `left-cols=2880`.

Simulated annealing with a tabu list (`--tabu`, default 128) swaps edge pieces
between sides, and corner pieces between corners early in each restart
(`--fix_corners 0`). Hard constraints are dominating penalties: degree balance
per side, weak connectivity, the inner-colour inventory (inward faces of the
border cannot exceed the inner pieces' supply), and parity of the surplus.

### 4.2 Objectives

- **Log-sum** (default): maximize `sum_s w_s * log(count_s)`, weights
  `--w_top/--w_right/--w_bottom/--w_left` (default +1 each). A negative weight
  minimizes that side; a zero weight drops it from the objective.
- **Target balancing** (`--target_scale N`): each side is scored by how many
  decades its count sits from its own target `w_s * N`,
  `100 - 25 * sum_s log10(count_s / target_s)^2`; 100 = every side on target.
  The penalty depends only on the ratio, so no side drowns another.

Measured (8 seeds x 100k steps, targets 250/5000/5000/15000 bottom/left/right/top):

| objective | geometric-mean counts T/R/B/L | on-target score |
|---|---|---|
| log-sum, all `w = 1` | 4065 / 1977 / 3033 / 2574 | 42.7 |
| log-sum, `+9/-2/-5/-2` | 4825 / 550 / 449 / 643 | 39.6 |
| `--target_scale 250`, `w` 60/20/20/1 | 5465 / 3019 / 437 / 2720 | 85.8 |

Use log-sum to make every side rich; use targets when the size of each side's
search space matters (a bottom of 5000 with `--top_bottoms 300` searches 6% of
its own space).

### 4.3 Temperature schedule

`--T0/--Tf` are normally left unset and resolved at startup from the starting
state:

- **Cold start** (random border): no sampled move lands feasible, so the
  schedule is anchored to the feasibility cliff, the 45-point gap between the
  best infeasible and the worst feasible score: `T0 = cliff/4 = 11.25`,
  `Tf = cliff/5 = 9`. At 16 restarts x 500k steps this anchor scored a mean of
  8.69 against 8.18 for `T0 = 1000`.
- **Warm start** (`--input`): a probe samples the feasible neighbours and
  measures `sigma`, the standard deviation of their score change. If at most
  25% of them improve the border, it is near an optimum and is *polished*:
  `T0 = 0.5 sigma`, `Tf = T0/20`. Otherwise it is *searched* with the cold
  anchor. (Measured improving shares: 1.4-15.7% on rows that wanted polishing,
  30.6-59.1% on rows that wanted searching.) Polishing at the cold schedule is
  destructive: 0 of 12 restarts matched their input, against 12 of 12 at
  `0.5 sigma`.

### 4.4 Refining an existing border

`--input rotations.csv --row N` starts every restart from data row N of an
earlier output (rows counted from 0 over data lines, as the beamer's
`--start_row`). Without `--row`, every row is refined in parallel and written
back in input order, one output row per input row, id `r<N>`.

- The row is cross-checked against the seed: its trail counts are recomputed and
  must equal its comment's; a mismatch is fatal.
- Corners are kept; `--fix_corners` is refused with `--input`.
- The starting border is eligible as a restart's best, so a refinement never
  returns worse than its input.
- More `--steps` beats more `--restarts`: restarts share the start and diverge
  only by RNG.
- stdout gives one line per row with the before/after score and counts;
  Ctrl-C keeps every finished row.

### 4.5 Two-tall sides (`--double_decker [SIDES]`)

A classic side score says nothing about whether the inner ring under it can be
filled. `--double_decker` (sides: comma list of `TOP`, `RIGHT`, `BOTTOM`,
`LEFT`, or `ALL`; bare = `ALL`) scores each named side by its **two rows**: the
number of layouts of the 14 edge pieces plus an inner piece under each
(cols/rows 2..13 of the second ring), chaining legally, drawn from a per-side
**reserve** of `--decker_reserve K` inner pieces (default 16, 12..32) that the
search chooses. The corner cells of the second ring belong to fixed 2x2
**corner blocks** (corner, the two edges beside it, the inner piece diagonal),
between which each strip runs. Clue pieces are never reserved.

- **Counting** is exact: a memoized walk over (placed-edge mask, used-piece
  mask, frontier), ~1 ms at K = 24. The result is the side's `Decker=` count.
- **Objective** `Score_dd` = mean over the four sides of each side's log
  count (the `Decker=` count on two-tall sides, the trail count elsewhere).
- **Moves**: reserve swaps (85% biased to pieces that chain with the current
  reserve), trades between two-tall sides, block changes, edge swaps between
  sides. `--decker_keep_border` forbids every move that changes a side's edge
  set, so the output spins equal the input's.
- **Schedule.** A cold restart first runs the classic walk for
  `--decker_warmup` of its steps (default 0.5) to find a feasible border and
  fixes its corners; the double-decker phase then runs on a schedule probed per
  restart and printed (`restart N: double-decker schedule T0=... Tf=...`).
  `--T0/--Tf` set only that phase; a probe on a flat plateau gets
  `T0 = 0.1, Tf = 0.005`. Measured best range: `T0` 0.03-0.1; 0.3 and above is
  clearly worse. Run cold restarts at 250k steps or more.

The reserve is larger than the 12-cell strip on purpose: the 12 pieces of one
layout admit few other layouts (24 on a measured strip), while a searched set of
20 / 24 / 30 admits ~208 / ~256 / ~1,094. With all four sides two tall the
reserves compete for the same pieces (64 of 191 at K = 16), so each side's count
is lower than with one side alone.

Limits: each corner block holds one fixed inner piece, so only the strips
between blocks are flexible; K stops at 32 because the exact count grows with
it; clue orientation is not modelled (row 14, cols 2 and 13, sit on the row-13
clues in two orientations). Downstream, only the beamer's `--lambda_reserve`
reads a reserve (TOP), and the finalizer keeps a witness's complete rows.

**What each border carries:**

- the comment: the classic counts (`TOP=...`), `Score=` (the classic score under
  the weights in force), `Score_dd=`, `Decker=T/R/B/L` (exact counts, `-` for a
  classic side), `DeckerPool=` (the count if the whole free pool were allowed,
  with repeats: an upper bound), and `Board=<name>`, the witness board's id;
- the rotations row: every reserved piece and every block's inner piece carries
  its side code in place of spin 0: **1 = TOP** (row 14), **2 = BOTTOM**
  (row 1), **3 = LEFT or RIGHT** (cols 1 and 14);
- a **witness board** in `--decker_out` (default `<out stem>_decker.csv`), in
  the beamer's board format: the border ring plus one layout of each two-tall
  side's inner ring. It is a finalizer input (§6.6).

Rows with side codes are not turned by `E555_sort_rotations.py` and refused by
`E555_rotate.py --rotations`.

### 4.6 Output and parallelism

Each restart's best border (or each row's, when refining a file) is appended to
the rotations CSV: a `#` comment with the counts and `Score=`, then
`id, spin[0..255]` (60 border spins, 196 zeros or side codes). `--out FILE`
names it; the default is `<stem>_refined.csv` beside `--input`, or
`rotations.csv`. stdout has one line per restart; `--verbose` prints the whole
search, including `BEST,...` lines.

Restarts run as worker processes (`--threads`, 0 = one per core). Each restart
derives its seed from `--rng_seed` and its index, and the parent replays results
in restart order, so the thread count never changes the output.

### 4.7 Options

| option | default | meaning |
|---|---|---|
| `--out FILE` | see 4.6 | rotations CSV to append to |
| `--restarts N`, `--steps N` | 3, 250000 | restarts and steps per restart |
| `--rng_seed S` | 0 = random | master seed |
| `--threads N` | 0 = cores | worker processes |
| `--T0`, `--Tf` | resolved | temperature schedule (4.3) |
| `--input FILE`, `--row N` | -- | refine a row, or every row, of an existing file |
| `--w_top/--w_right/--w_bottom/--w_left` | 1 | objective weights |
| `--target_scale N` | off | target-balancing mode |
| `--tabu N` | 128 | tabu list length (0 = off) |
| `--fix_corners {0,1,2}` | 0 | 0 random corners, 1 edge-commutativity, 2 corner-commutativity |
| `--double_decker [SIDES]` | off | two-tall sides (4.5) |
| `--decker_reserve K` | 16 | reserve per two-tall side, 12..32 |
| `--decker_keep_border` | off | keep every side's edge set |
| `--decker_warmup F` | 0.5 | share of a cold restart spent in the classic walk |
| `--decker_out FILE` | `<stem>_decker.csv` | witness boards |
| `--verbose` | off | the whole search on stdout |

---

## 5. Stage B -- `E555_beamer`

`src/B_beam/E555_beamer.{c,h}` with the shared `src/B_beam/E555_database.{c,h}`
(seed, catalog, chain database, border enumeration and ranking, top-corner
catalog) and `src/C_tail/E555_dive.{c,h}` (end dives).

```
bin/E555_beamer seed.txt rotations.csv [options]
bin/E555_beamer seed.txt --random_edges [options]
```

### 5.1 The 5-5-5 row decomposition

Every inner row is the configuration's left-column piece plus three 5-cell
segments:

```
 col 0        cols 1-5          cols 6-10         cols 11-15
[left edge]  [A: 5 inner]      [B: 5 inner]      [C: 4 inner + right edge]
```

Segment A is keyed by the colour the left column exposes, B by A's rightmost
colour, C by B's. The right edge piece comes out of segment C's database record.

### 5.2 The chain database

`DB[left][b1][b2][b3][b4][b5]` is a direct-indexed array of all legal
**5-piece horizontal chains**, keyed by the colour to the chain's left and the
five colours it sits on (the exposed tops of the row below). When `b5` is an
inner colour (6-22) a record is five inner pieces; when `b5` is a
frame-interface colour (1-5) it is four inner pieces plus a right-edge
terminal. The colour ranges are disjoint, so one database serves all three
segments, and segment C, whose last bottom colour is always a frame-interface
colour, supplies the right edge automatically.

A record stores each piece as its index inside the (left, bottom)-colour bucket
of the oriented-piece catalog (buckets hold at most 7 orientations, 3 bits
each): about 2 bytes per record. For the official set: **3.12 x 10^9 chains,
6.4 GB**, built in two parallel passes and sorted within each cell by promise
(fan-out, below). The inner part depends only on the seed and the excluded
clue pieces and is cached with `--db_file` (mmapped read-only on later runs);
the border-dependent edge part (~0.2 GB) is rebuilt per border row. A cache
records its exclusion set and a run with different clue flags rebuilds it, so
keep one `--db_file` per clue setting. Record counts: 3.119e9 without clues,
3.042e9 with the centre clue excluded, 2.730e9 with all five.

`fanout[b1..b5]` (15 MB) caches, for every five-colour bottom signature, the
record count summed over the 17 possible left colours: how continuable a set of
exposed tops is, in one lookup.

### 5.3 Border configurations

A **configuration** is one bottom-row ordering and one left-column ordering of
a border row of the rotations file (enumerated as Euler trails of those sides),
plus the top-right corner. The sweep runs `--top_bottoms` bottoms per border
row and `--top_columns` columns per bottom, best-ranked first:

- **Bottom rank**: the log fan-out of the three segments it presents,
  `fanout(rt[1..5]) + fanout(rt[6..10]) + fanout(rt[11..15])`.
- **Column rank**, computed per bottom: (a) the column turned 90 degrees is a
  bottom row for vertical chains, so sliding 5-windows `r = 1..10` are counted
  in the same database (`db_seg_fanout(right[r+4], ..., right[r])`, read
  downward); (b) the exact number of row-1 segment-A chains for this
  (column, bottom) pair. A pair with (b) = 0 cannot complete row 1 and is never
  run.
- `--tau_bottoms/--tau_columns T > 0` replace the greedy head by a sample
  without replacement with probability proportional to `exp(rank/T)`.
- `--bail_columns N` abandons a bottom after N consecutive columns that wrote
  nothing.
- With `--clue_corners`, the two row-2 corner clues pin colours on row 1. An
  exact test (the beam's own database walk, without quotas) drops columns that
  cannot pass row 1 with the bottom; `[sum] border prefilter` reports the counts.

**`--random_edges`** replaces the rotations file: bottoms are sampled as random
legal chains of 14 edges between randomly assigned corners, left columns from
the edges the bottom left, each the best of 32 samples by the ranks above.
`--samples` is the number of bottoms (0 = until `--wall_time` or
`--max_emitted`); corners can be pinned with `--BL/--BR/--TL/--TR`. Free edges
are implied.

**`--exhaust_border_color`** makes every random border use up one frame colour
(the 5 colours on the joints between border pieces, 12 joints each), drawn per
bottom:
- In random mode only the bottom row and column 0 rows 1..S are written, so
  "used up" means every edge piece carrying the colour lies there, neither top
  corner carries it, and BR's top is not it. The right column, the top row and
  the left column above S then see only the other four colours (a plain walk
  uses up a colour once in 4000 borders).
- Why: by the BEST theorem a side's trail count is `t_w * prod_c (d_c - 1)!`
  over its joint colours, so concentrating the joints on fewer colours is what
  makes a frame flexible. Using a colour up gives the free frame (the right
  column from row 1, the top row, the left column above S) about 250 times
  more legal orderings, and the beam, which fills the right column row by row,
  writes about twice as many stop-row boards.
- How: pieces carrying the colour are drawn 16 times as often in both walks;
  a bottom that leaves more of them than the left can hold, a left that leaves
  any unused or puts one above S, and a corner draw that breaks the rule are
  resampled. Each bottom must also admit one such left before it is used.
  Best of 32 by the fan-out ranks as before, so borders stay distinct (1200
  configs: 299 distinct bottoms of 300, 1160 distinct lefts).
- Colour 3 can never be used up (three corners carry it), and corner pins can
  rule out others; the colours left are listed at startup (`[cfg] exhaust
  colours possible`), and the run stops at once only if none is. Colour 2 is
  accepted 1 time in 5: it allows just two corner placements, so its borders
  keep leaving the same edges for the top.
- A bottom that fails 4 colour draws is run as a plain random border, so a
  long run never stops on bad luck. The 20-21 edges carrying a colour must fit
  in 14 + S cells, so below S = 8 most borders are plain.
- `[sum] exhausted colour:` counts the configs by colour, `plain` the fallbacks.

**`--free_edges`** lets any unused edge piece end a row on the right (and so any
edge serve the top border later) instead of only the rotations row's right side.

### 5.4 The beam loop

Per configuration, one beam of up to `--beam_width` K boards advances one full
row at a time. The width grows late: K*E/2 at row R-1 and K*E from row R on
(E = `--beam_expand`, default 4; R = `--beam_expand_row`, default 7), and the
per-parent cap doubles there.

1. **Expand.** Each board fills its next row A, B, C from the database with
   exact 256-bit piece masks. A first phase scans each cell's records in promise
   order; a second visits the records the first did not reach in a random
   full-cycle order (random start, coprime stride), so no record is tried twice
   for one parent. Work per parent is bounded by `--pool_factor` (child quota,
   x K per row) and a decode budget. **`--bc_window nB,nC`** (default 3,3): while
   the beam is full, up to nB workable B chains x nC C completions are scored per
   A record and the best child kept; while it is below capacity every completion
   is kept, since selection would discard nothing anyway.
   **`--bc_window_accept F`** (default 1) keeps that best child with probability
   F and the window's second best otherwise, for variety between runs.
2. **Score** (5.5), after the feasibility test (5.6).
3. **Select.** Children are deduplicated by a 64-bit frontier signature (a hash
   of the used-piece set, the exposed tops and the clue orientation, which
   determine a board's entire future), keeping the best copy. If the pool
   exceeds the row width, it is cut to a score band with at most
   `--parent_cap` children per parent plus a random band of `--frac_rand` of the
   width (default 0.10, flat over rows). With clues on, each orientation first
   gets a floor of K/8 in its own score order.
4. **Materialize.** Moves go to an ancestry log from which emitted boards are
   rebuilt; a beam entry is 128 bytes.

An empty child pool ends the configuration (`extinct`). This is not a proof that
the configuration is dead: quotas and the pruned beam bound the search. Boards
completing `--stop_row` (1..13, default 11) are emitted best first with no
lookahead; `--backtrack_row` (5.9) replaces the last rows by an exhaustive
search.

### 5.5 Scoring

A child's score is a sum of terms in nats.

**One-row lookahead.** `log n(A') + log(1 + f_B') + log(1 + f_C')`: the exact
record count of the next row's segment-A cell (its left colour is known) and
the fan-out of the next B and C windows. A zero factor proves the child cannot
complete the next row and rejects it (below the stop row). The fan-out ignores
which pieces are used, so it overcounts, increasingly with depth.

**Closure** (`--lambda_J`, default 1.0). Let `S_c` be the free half-edges of
inner colour `c` (5.6) and `2A = sum_c S_c`. Every free half-edge must meet
another of its colour; of the `(2A-1)!!` pairings, `prod_c (S_c - 1)!!` are
colour-consistent:

```
P = prod_c (S_c - 1)!! / (2A - 1)!!
```

By Stirling, `log P` is, up to a constant at fixed depth, `-A * H(pi)` with
`pi_c = S_c / 2A`. The term is

```
J_conc = A_tot * KL(pi || uniform)          (rewards concentrated colour mixes)
J_dem  = sum_c D_c * log(R_c / Rbar)        (penalizes demand for a colour in short supply)
```

with `D_c` the colours the board owes and `R_c` the remaining supply. Both carry
their own depth dependence; there is no schedule.

**Mahalanobis correction** (`--lambda_Mahalanobis`, default 1.0). `x` is the
17-vector of inner-colour faces used by the `n` placed inner pieces, out of a
population of M = 196 with totals `t`. Under uniform sampling without
replacement, `E[x] = (n/M) t` and `Cov[x] = fn * (M sum_i f_i f_i^T - t t^T)`
with `fn = n(M-n) / (M^2 (M-1))`. The term is the normalized distance
`d2n = D^2 / E[D^2]` in the 16-dimensional Helmert contrast space, standardized
by the mean and SD of `d2n` over the previous row's children of the same
configuration (the run's pooled row spread when a row has fewer than 64
samples): `+lambda * (d2n - mu) / sigma`. A positive lambda rewards atypical
colour consumption. It correlates about 0.88 with closure; `--lambda_J 0` or
`--lambda_Mahalanobis 0` leaves the other term alone.

`--lambda_corners` (5.12) and `--lambda_reserve` (5.12) add further terms in
units of the row's score SD.

### 5.6 Colour parity

For every inner colour, `S_c = total_c - consumed_c - required_c`, where
`required` counts the exposed tops plus every committed future interface (left
column, right edges, top border). Every face of an unplaced piece is matched to
a requirement or paired inside the unfilled region, so `S_c >= 0` is necessary.
It is tested for every child and at every completed row of the backtracker.

The evenness half of the test never fires: each legal placement changes every
`S_c` by an even amount, so `S_c = total_c - B_c (mod 2)` (`B_c` = edge pieces of
inner colour `c`) is fixed by the seed. The `S_c >= 0` half rarely fires either
(634 cuts in 339 M backtracker nodes); the pruning comes from piece fit.

Under `--free_edges` the demands stay exact without knowing which edge ends up
on which side: an edge piece exposes its one inner colour inward in either role.
`--no_free_demand` turns this accounting off.

### 5.7 Clues in the beam

`--clue_center` forces piece 138 onto its cell; `--clue_corners` forces the two
row-2 corner clues and reserves the two on row 13 (never pinned). Clue pieces
are excluded from the database and placed by a pinned walk.

A board is uncommitted until it places its first clue, then owes that
orientation for good; the orientation joins the dedup signature. A clue also
pins the colour its bottom face needs on the row below, so it constrains the
search one row early (the pin is applied while generating, not as a filter).
The centre clue sits on row 7 (orientations 0, 3) or row 8 (1, 2), so it pins
row 6 or 7. `--pin_clue N` (1..4: centre at (7,7), (7,8), (8,8), (8,7)) searches
one frame instead of all four and implies `--clue_center`; 0 hedges over all.
`E555_CLUE_DEBUG=1` prints the pin schedule.

With `--clue_corners` every written board also carries its orientation's two
row-13 clue pieces at their cells, so later stages build around them (unless
`--stop_row 13` searched those cells).

**`--free_top_clue`** (with `--clue_corners` and `--backtrack_row`): the
row-13 clues pin two cells the column-major extension (5.10) must fill, and the
colours of the cells around them. With this flag the extension treats both
cells as ordinary cells, and:

- the **top-left** clue (13,2) is free: the extension may place it on any cell
  above the stop row, in any rotation. If it does not, the clue goes on its
  home cell only if that cell is empty and it matches every placed neighbour;
  otherwise it is left unplaced for the dives and the tail;
- the **top-right** clue (13,13) stays held through the extension, so it never
  lands in the left columns, and is then left off the board: the dives place it
  among the open cells, which the column-major extension leaves in the
  top-right corner, on its own cell or not.

Rows up to the stop row hold both clues in reserve. Corner-seeded dive copies
(5.11) find a board's clue frame from its (13,2) clue, so a board whose
top-left clue is not home gets none.

### 5.8 What a stop-row board carries

Rows `0..stop_row`; in rotations mode also the configuration's whole left
column and the top-right corner (the configuration fixed them, and the beam
never places them elsewhere); the row-13 clues under `--clue_corners`; and,
under `--backtrack_row`, the column-major extension above the stop row (5.10).
The right column and the top border above the stop row are left empty: the
rotations file fixes which pieces go there, not their order.

### 5.9 Exhaustive search to the stop row (`--backtrack_row N`)

Past row 6 or 7 the beam keeps a small share of the legal boards, while an
exhaustive search from those boards is cheap because its tree dies out within a
few rows. With `--backtrack_row N` (1..stop_row-1):

1. rows 1..N-1 are the ordinary beam;
2. row N is an ordinary beam row (lookahead, `--bc_window`, frontier dedup) and
   its best `--backtrack_row_factor` M x width boards, chosen by the beam's own
   selection (per-parent cap, random band), are the **roots**, in rank order
   (default M = 2). `--pool_factor` then sets how many candidates the roots are
   chosen from, not how many there are. With M = 0, the behaviour before the
   flag, row N is expanded like a stop row and every candidate is a root, kept
   raw without frontier dedup: up to `--pool_factor` x width roots, mostly
   siblings that differ only in the last segments of row N;
3. rows N+1..stop_row are searched exhaustively from every root, cell by cell in
   row-major order: col 0 is the fixed left column, cols 1-14 take every unused
   inner orientation matching left and bottom, col 15 every unused right edge of
   the border's pool (every edge under `--free_edges`).

A path ends when a cell has no fitting piece, a completed row fails the colour
test (5.6), or, under `--lambda_corners`, a completed row leaves neither top
corner buildable. Clue pins are enforced; an uncommitted board branches once
per orientation that owes a pin on the row. **Every** board completing the stop
row is written, root by root, exact duplicates dropped, with no cap per
configuration (bound the run with `--max_emitted`, checked after each root).
Nothing past row N is scored or selected, so the output of the search does not
depend on the thread count.

**Top-row near duplicates** (on by default; `--no_top_dedup` turns it off).
Consecutive stop-row boards of the search usually share all their lower rows
and differ in a piece or two of the top row. A board whose top row repeats the
previous board's columns 1..K (`K` = `--backtrack_min_col`, 0 without it) and
differs from it in at most one other cell (columns K+1..15) is dropped before
its extension and dives: if the previous board could not fill the columns,
its near twin will not either, and if it could, the twin would repeat its
extension and dives. The reference is the last stop-row board not itself
dropped, written or not, so a run of near twins is measured against its first
member. A board with a new piece in columns 1..K is never dropped. The
reference is cleared at the rows where a subtree may be handed to another
thread (below), so the output stays independent of the thread count; the first
board after such a row is always kept. Dropped boards count as found and are
reported as `near_dups`.

**Parallelism.** Threads claim roots in rank order within a window; a root's
boards are written once it and all earlier roots are done. A thread that finds
nothing to claim is *hungry*, and a search reaching a row boundary (rows
N+1..N+3) while a thread is hungry hands its subtree to a job queue, leaving a
placeholder in its output. Outputs are flattened placeholder by placeholder, so
file and counts equal the serial search's. On two-socket machines set
`OMP_PROC_BIND=close OMP_PLACES=cores`.

### 5.10 Column-major extension (`--extend_nodes`, `--backtrack_min_col`)

Under `--backtrack_row`, every board completing stop row S is continued before
it is written or dived: a second exhaustive, break-free search over rows
S+1..14, cols 1..14, in **column-major** order (column 1 bottom-up, then
column 2, ...). The board is written from the deepest prefix reached (the first
found at that depth, so thread-independent). Unfilled cells collect in the
top-right corner; the right column and the top border stay open.

Every cell is tested on its own: an unused piece fitting left and bottom; on
row 14, a top colour that an unplaced piece of the top-border pool still carries
(a counter per colour: the rotations row's top border, or all unused edges under
`--free_edges`); next to a clue cell, the clue's facing colour, and the clue
itself on its cell when the orientation is known (the extension stops at a clue
cell of unknown orientation).

- `--extend_nodes N` caps placements per board (default 100000; 0 = off). The
  search is usually tiny: 1.61 M row-9 boards were extended in a 16.6 s run,
  mean 2.4 cells, max 32 of 70, cap never hit.
- `--backtrack_min_col K` writes only boards whose extension fills columns
  1..K whole (exact up to the node cap). It selects, from a low stop row, the
  boards that can grow whole columns without a break.
- Under `--random_edges` the sampled left column is not written above the stop
  row, so the extension chooses column 0 there itself: the sampled edges above
  S are released, and columns 0 and 1 are filled together row by row (a
  frame-left edge on the column-0 top below, then the inner piece beside it)
  before columns 2..14. An edge placed in column 0 leaves the top-border pool.
  "Whole columns" still count from column 1.
- `--cap_top [N]` (on by default; `0` = off) closes the top-left border
  exactly over the whole columns. The extension's own row-14 test counts top
  colours only: it never asks whether the top edges chain by frame colour or a
  corner fits on column 0, so the break there was often decided before the
  dives. The cap is the TL corner (the fixed one; under `--random_edges` either
  corner off the board, its bottom on column 0's row-14 top), then top edges
  on (15,1), (15,2), ..., each unused, frame up, its inner colour the row-14
  top below and its left frame colour its neighbour's right one. It is only a
  tie-break: the search, its order and its node cap are unchanged and the most
  cells still win, so every board keeps its extension length and whole columns
  and `--backtrack_min_col` writes exactly the same boards; among the fillings
  of that length the one with the longest cap is kept and the cap is written
  on row 15. A cap is worked out once per whole column of a path that reaches
  the best length. `[sum] top cap:` gives the cap cells per board and the share
  of boards (with a whole column) closed over all of them.

**The column check** (always on with `--backtrack_min_col K >= 1`). Most
stop-row boards are otherwise found only to fail the extension. A board can be
written only if its column 1 runs whole up to row 14, and the row-major search
fills every column of the rows up to S, so each time a row's column-1 piece is
placed (rows N+1..S), the search looks for one way to stack column 1 from the
next row to row 14, as the board will have to build it:

- each piece fits the left column's colour beside it and the top of the piece
  below; no piece is used twice, and none the board already holds (the pieces
  the extension may release, 5.7 and below, count as free);
- under `--random_edges`, column 0 above the stop row is chosen too, as the
  extension does: a frame-left edge on the column-0 top below, its inner colour
  the left colour of column 1; rows up to S keep the sampled column;
- row 14's top must be a colour the top-border pool still carries, the stack's
  column-0 edges taken out of the pool;
- the column-1 cell next to a clue cell the extension will fill shows the clue
  the colour it needs.

Nothing else is asked, so every board the extension would keep has such a
stack: **the written boards are exactly those written without the check**,
while fewer boards are found, sooner. At the stop row, each of columns 2..K
must also leave a top some available piece can sit on (a clue cell's colours
around it, a border colour on row 14).

The search keeps two caches. A state (row, column-0 top, column-1 top) that
failed with some pieces taken fails with any more taken, so a dead state is
stored with the pieces taken then and recognised whenever all of them are
taken again; a stack found is stored with its pieces and stands again while
none of them is taken and its row-14 top is still in the pool. Both are emptied
at the start of every job and at the hand-off rows, so no answer depends on the
thread count. A check past 50,000 steps answers "unknown" and cuts nothing.

Measured, `data/borders_annealed_fix12.csv` row 0, `--backtrack_row 6`,
`--no_top_dedup` (backtrack time, nodes, boards found at the stop row, boards
written); "no check" and "reuse" are the earlier builds' no-check run and a
check that let pieces repeat between rows:

| stop row, K, threads | check | backtrack s | nodes | found | written |
|---|---|---|---|---|---|
| 9, 5, 4 | none | 2.02 | 341 M | 1,170,357 | 55 |
| 9, 5, 4 | reuse | 1.26 | 151 M | 387,105 | 55 |
| 9, 5, 4 | **exact** | **1.15** | 140 M | 354,622 | 55 |
| 10, 5, 4 | none | 4.29 | 740 M | 34,570 | 0 |
| 10, 5, 4 | **exact** | **2.23** | 309 M | 7,390 | 0 |
| 10, 2, 1 (2 configurations) | none | 33.2 | 1504 M | 70,352 | 823 |
| 10, 2, 1 (2 configurations) | reuse | 20.2 | 699 M | 16,901 | 823 |
| 10, 2, 1 (2 configurations) | **exact** | **18.0** | 650 M | 15,603 | 823 |

The written boards were byte for byte the same in every row. Under
`--random_edges` (two bottoms, stop row 10, K = 2) the check cut little --
6,597 boards found instead of 6,649 without, the same 4,746 written -- because
a free column 0 above the stop row lets almost any column 1 reach row 14; its
cost there is small (0.34 s against 0.28 s). Earlier builds also tried wider
strips (columns 1..W, W up to 5): they cut more boards but cost more than they
saved, and building strip rows from the chain database's records was slower
still; re-testing a row before its right edge cost more than it cut.

Growing by columns keeps many more boards alive than growing whole rows (it
does not have to chain the right-edge pieces): above row 9, 140 of 1.23 M boards
completed columns 1-6 (30 cells) break-free without the row-14 test, against 3
that completed rows 10-11. With the row-14 test, 1 did: the top border is the
binding constraint.

The extension does not by itself improve dives. The same 3,265 row-10 boards
dived with `E555_diver --end_dive 2000 --end_polish 200`:

| dived from | mean | best | >= 455 | >= 456 |
|---|---|---|---|---|
| the stop row (`--extend_nodes 0`) | 452.36 | 458 | 217 | 58 |
| the extension (mean 1.6 cells) | 452.28 | 458 | 215 | 37 |

Use it with `--backtrack_min_col` for selection; use `--extend_nodes 0` when
every stop-row board is dived anyway.

### 5.11 Finishing boards: end dives and polish (`--end_dive`, `--end_polish`)

With `--end_dive M` every stop-row board is completed to 256 pieces, allowing
broken edges, and the best completion is written instead. The engine,
`src/C_tail/E555_dive.{c,h}`, is shared with the finalizer and `E555_diver`.

**A dive** fills the open cell with the fewest exact fits first; it places an
exact fit where one exists, and otherwise a placement from the smallest break
class, only when every open cell is stuck. Within a class it takes the
least-constraining value (fewest broken edges, then fewest stranded cells, then
most room left, up to 8 candidates played out), ties at random. A dive never
backtracks and cannot fail (piece-type counts always balance). Placed cells
never move. Edge pieces stay on the side their rotations row deals them, or any
side under `--free_edges`/`--random_edges`.

**Per configuration:**

1. **Stage 1**: M/10 dives per board.
2. **Stage 2**: boards whose stage-1 best is `>= S-4` (`S` = `--emit_score`,
   default 450), or the configuration's top 10% when that is fewer than 20%, get
   the remaining dives in 18 **cross-entropy rounds**. A weight `w(piece, cell)`
   steers only choices that would otherwise be random or ranked by room; after
   each round the top 5% of dives vote and
   `w += 0.3 log((votes + 1/2) / (expected + 1/2))`, clipped to +-2.
3. **Polish** (`--end_polish R`) improves dives that are already finished; it
   does not dive again. Stages 1 and 2 keep each board's 32 best distinct
   dives, not just the best one (`E555_diver --polish_top K`: K, with K/2
   walks). A board whose best dive is within 6 of `S` is
   polished; a board further below rarely reaches `S`. Only pieces placed by
   the dives move.
   - **Climb:** each of the 32 dives is hill-climbed: turn one piece or swap
     two, with frame-legal spins, while any such move adds an edge.
   - **Walks:** up to 16 of the best distinct climbed boards each start a walk,
     and the walks share `R` rounds. A round makes 4 swaps, each the least
     damaging of 16 random pairs with one piece at a broken edge, climbs again
     around the swapped cells, and accepts a board d edges worse with
     probability `exp(-d/T)`, T cooling 3 -> 0.3 over the walk. After each
     quarter the worse half of the walks moves to the better half's boards;
     each walk returns the best board it met.
   - The best board found replaces the board's best dive. `R = 0` runs the
     climb only.

Boards reaching `S` are written after each configuration, best first, duplicates
dropped, with the matched-edge count in field 2; `--max_emitted` then caps
written boards instead of stopping the search.

**Corner-seeded copies** (`--corner_seeds N`, default 4, with
`--lambda_corners`): a board with an alive top-corner block (5.12) is also dived
in up to N copies with such a block and a free pair of its top-border witnesses
fixed, so that corner is clean by construction.

Every dive's random stream is keyed by its board and index, and results merge
with order-independent keys, so the output does not depend on the thread count.
Work is split into jobs (32-dive blocks, learning rounds, polish candidates,
walks) on one queue, so a configuration with few boards still uses every thread.

Measured on 35 row-10 boards (border row 0 of `data/borders_annealed_fix12.csv`,
4 threads):

| setting | mean best | best | boards >= 452 | time |
|---|---|---|---|---|
| `--end_dive 2000` | 449.3 | 453 | 4 | 2 s |
| `--end_dive 10000` | 450.5 | 455 | 9 | 8 s |
| `--end_dive 50000` | 451.1 | 453 | 12 | 46 s |
| `--end_dive 10000 --end_polish 5000` | 454.6 | 457 | 35 | 17 s |
| `--end_dive 10000 --end_polish 20000` | 454.8 | 457 | 35 | 29 s |

Polish is the largest gain: `10000/20000` beat `50000` dives on all 35 boards
by 3.7 edges on average, in less time. Past ~10000 dives, spend time on polish
rounds.

### 5.12 Protecting the top corners and the reserve

**`--lambda_corners [F]`** (bare = 0.5; needs a rotations file and
`--stop_row <= 12`). The top corners are finished by later stages, and nothing
else protects the pieces they need. Per border row every legal filling of a
small block at each top corner is enumerated exactly (shared `tc_*` code in
`E555_database.c`); top-border cells `w` only witness that the border can meet
the block:

```
with --clue_corners                  without
row 15  corner  w1    w2             corner  w1
row 14  side    in_a  in_b           side    in_a
row 13  side    in_c  CLUE
```

A child is scored by the blocks per corner still buildable from its unused
pieces, n capped at 3:

```
term = F * u_row * (step(n_TL) + step(n_TR)),   step(0..3+) = -3, +1, +2, +3
```

`u_row` is the SD of the rest of the score on the previous row, so F is in
score-SD units. Left columns that no TL block can use are not run, and border
rows that cannot close a corner are skipped. Under `--free_edges` the top and
right edge pieces are pooled for the TR block and the witnesses. The summary
reports alive blocks per corner at the stop row and how many boards keep a
piece-disjoint TL+TR pair. In the backtracker a path ends when neither corner
stays buildable.

**`--lambda_reserve F`** reads the TOP reserve that Stage A's `--double_decker`
marked with side code 1 (only on border rows whose `Decker=` shows TOP two
tall) and charges `F * u_row` per reserve piece a board has placed. Nothing is
held, and the backtracker and the dives ignore it. Measured (16
configurations, width 20000, 26 marked pieces): at F = 2 the boards reaching
row 10 left 12.0 reserve pieces free against 7.6 at F = 0, with the same reach
(16/16), but the reserve still could not lay out row 14 on any sampled board. It
is a mild bias; the summary line `[sum] reserve at stop row` shows its effect.

### 5.13 Measured settings for reaching row 11

`data/borders_annealed_fix12.csv`, `--clue_center`, 4 threads, database in the
page cache. "Reach" counts configurations with a row-11 board.

**Width and backtrack row** (32 configurations; the last two rows are rates
over 12 and 20):

| `--beam_width` | `--backtrack_row` | reach | row-11 boards | s/config | reach / core-hour | boards / core-hour |
|---|---|---|---|---|---|---|
| 10000 | off | 0/32 | 0 | 0.9 | 0 | 0 |
| 10000 | 8 | 0/32 | 0 | 0.8 | 0 | 0 |
| 10000 | 7 | 2/32 | 6 | 0.8 | ~74 | ~210 |
| **10000** | **6** | **16/32** | 47 | **1.0** | **~470** | **~1300** |
| 10000 | 5 | 12/12 | 238 | 23 | ~38 | ~760 |
| 250000 | 6 | 20/20 | 496 | 31 | ~29 | ~710 |

A narrow beam to row 6 and the exhaustive search from there is the cheapest way
to row 11 by an order of magnitude. The configurations it misses are not dead:
`--backtrack_row 5`, or a 250k beam to row 6, takes each of them to row 11 at
~25x the cost per configuration. A wide beam keeps every candidate from about
row 7 on, so width beyond ~100k mostly buys time.

**Centre-clue frame** (`--pin_clue`, 64 configurations, width 10000;
configurations reaching row 11, time):

| `--backtrack_row` | all frames | (7,7) | (7,8) | (8,8) | (8,7) |
|---|---|---|---|---|---|
| 6 | 35/64, 87 s | 19/64, 57 s | 22/64, 58 s | 6/64, 60 s | 2/64, 60 s |
| 7 | 6/64, 68 s | 7/64, 54 s | 8/64, 53 s | 0/64, 64 s | 0/64, 63 s |

A row-7 centre clue prunes one row earlier than a row-8 one; row-8 frames need
`--backtrack_row 5`. The unpinned run finds what the four pinned runs find
together at a quarter of the cost: leave `--pin_clue 0` unless the frame is
known.

**Breadth** (288 configurations, border rows 4-7, width 10000,
`--backtrack_row 6`): neither the bottom nor the column rank predicted yield
(bottoms 0-5 reached row 11 in 21-29 of 48 configurations each, with no trend),
while the border row did (19/72 on row 4, 67/72 on row 5). `--tau_bottoms/--tau_columns 0.1` on both
ranks cost nothing (168/288 against 159/288 at 0); tau 2 is near-uniform and
lost 10%. Spend the budget on border rows first.

These measurements predate `--backtrack_row_factor`: they used every row-N
candidate as a root, i.e. `--backtrack_row_factor 0`.

**Recommended pass** (the command of §3.1): width 10000, `--backtrack_row 6`,
`--stop_row 11`, 6 bottoms x 12 columns, tau 0.1; repeat promising borders with
`--backtrack_row 5`. Read the database once after boot (`cat chain.db >
/dev/null`): a cold page cache makes the first configurations 5-10x slower.

### 5.14 The run log

Each border row opens with a line carrying the run time so far,

```
========== border row 3   run 1234 s = 20.6 min ==========
```

then its rotations row as the file has it. `[cfg]` lines echo the settings (and
the run code under `--prefix`), `[init]` the database, `[rank]` (`--verbose`)
the ranking per bottom.

**Compact log** (default). Two words throughout: **found** = boards that
completed the stop row; **written** = rows appended to the CSV (under
`--end_dive`, dived boards kept at `>= --emit_score`; otherwise found boards
less near duplicates, exact repeats and `--backtrack_min_col` drops). A
`[sweep]` line appears for a configuration that found something or stopped for
an unusual reason; its counts are that configuration's. The line is written in
three steps: the id (when the beam starts under `--random_edges`, when it ends
otherwise), then what the beam hands on, then the counts once the backtracking
(and the dives) are done:

```
[sweep] r1b0l1 beam: 412 roots | found=1827 dups=212 repeats=64 written=1551 wall=4.7s
[sweep] r0b0l0 beam: 1549 boards | found=1549 written=1549 best=458 wall=104.1s
```

A configuration cut short also shows `stopped=<reason> at row R` (`time`,
`interrupted`). `dups` counts boards the near-duplicate filter dropped
(5.9), `repeats` boards the backtracker reached twice (clue frames cause it),
`below_min_col` appears with `--backtrack_min_col`, `best` the best dived
score. A random-edges configuration that dies in the beam ends its line with
`died rN`. In rotations mode the configurations that found nothing
collapse into one line per run of them under a bottom, with the rows they died
at:

```
[sweep] r0b3l0-l5 x6 found=0 died r9:4 r10:2 wall=245.1s
```

so every configuration appears, in order.

**Verbose log** adds, per configuration, `[beam]` lines per row
(`cands uniq beam=n/width smax t`), the `[dfs]` line of the backtracker (roots,
roots with boards, nodes, parity and corner cuts, `cut_col` partial rows the
min-col check cut and `unknown` checks over budget, `near_dups`, boards
completing each row, extension depth) and the `[dive]` line, and a `[sweep]`
line for every configuration in the older form:

```
[sweep] r0b0l0 filled=10 width=1549 reason=stop_row emitted=1549 sol_total=1549 wall=0.5s
[sweep] r0b0l0 died=1 width=1 reason=extinct(clue_row) wall=0.0s
```

`filled=R` is the last row completed with the width that completed it;
`died=R` the row that failed with the width carried into it. Consecutive
identical deaths under one bottom collapse to `l<first>-l<last> x<n>`.
`reason=extinct(clue_row)` marks a death on a row a clue constrains (a clue
pins the row under it too). `emitted` is the configuration's unique boards,
`sol_total` the run total.

**Summary.** One short line per topic, only those that apply:

```
================= run summary =================
[sum] time: init 45s, beam 11m08s, backtrack 12m10s, dives 38m12s
[sum] configs: 144, 25.5 s each; 61 reached row 11
[sum] extinct: r8:20 r9:41 r10:22
[sum] backtrack: 1.23G nodes, 180 M/s; r9:5120 r10:880 r11:12345
[sum] extension: mean 5.40 cols, max 9
[sum] top cap: 2.31 cells per board; 41% of the boards with a whole column closed over all of them
[sum] min_col 5: column check cut 3.4M partial rows
[sum] corners: TL 61%, TR 55%, both 40%, pair 31%
[sum] reserve: 12.0 of 26 TOP pieces free
[sum] boards: found 12345, near-dups 2345, repeats 12, below min_col 9000, dived 988
[sum] *** Output boards score: 458:1  457:3  456:12  455:40  454:101  453:255 ... (412 written)
[sum] best boards: 458 row 120, 457 row 7, 457 row 311, 457 row 390, 456 row 2 in beam_out/beam_completions_0_11.csv
[sum] *** Output boards extension (cells above row 11, top cap not counted): 32:1  31:4 ...
[time] run ended 2026-10-01 14:03:11 UTC (wall 1:02:03)
```

`backtrack` lists the boards completing each searched row; `corners` the share
of written boards with a TL / TR block alive, both, and a piece-disjoint pair
(5.12); `top cap` the row-15 closure `--cap_top` wrote (5.10); `reserve` the
TOP reserve left free at the stop row; `exhausted colour`
the configs by the frame colour their border used up (`--exhaust_border_color`,
5.3). `Output boards score` counts the written boards by score, highest first
(at most six, `...` if there are more): the dives' matched edges under
`--end_dive`, otherwise the matched edges of each written partial board, as
`E555_rank` scores it. `best boards` says where the five best are: the 0-based
data row of the CSV, as `tools/E555_viewer.py FILE --row N` takes it (the file
is appended to, so rows of earlier runs count). With the extension, the same
two lines follow for its length; `longest extensions` (the row of each) is left
out under `--end_dive`, whose rows are the dived boards.
`--verbose` first prints every detail line: time split, extinctions, row flow
(`attempts:candidates/retained/selected` per row), backtrack and extension
totals, end-dive totals (dives, polish gains, time and CPU use per phase, where
the written boards' breaks sit: TL and TR 4x4, the seam above the top full row,
the rest), the corner and reserve reports, the Mahalanobis spread by row.

### 5.15 Determinism

Without `--rng_seed` the master seed comes from the clock and PID and is
printed. With a seed, a run repeats on the same build and thread count; the beam
partitions work by thread, so a different thread count explores a different
beam. The beam can also rarely differ between identical runs at several threads
(observed: 1 run in 6 at 4 threads took a different row-7 beam). The
backtracker, the extension and the dives are exact: given the same stop-row
boards or roots they write the same file at any thread count.

### 5.16 Options

| option | default | meaning |
|---|---|---|
| `--out_dir DIR` | `beam_out` | output directory |
| `--prefix [NAME]` | -- | name every written board `NAME_<config>`; bare = a random 6-character run code, printed in `[cfg]` and `[sum]` |
| `--start_row N`, `--num_rows N` | 0, 0 = all | border rows of the rotations file (checked at startup: a missing file or a `--start_row` past its end stops the run at once) |
| `--db_file PATH` | -- | inner-database cache (~6.8 GB file) |
| `--free_edges` | off | any unused edge may end a row |
| `--random_edges`, `--samples N` | off, 1 | sample borders (5.3); 0 = unlimited bottoms |
| `--BL/--BR/--TL/--TR P` | -- | pin a corner piece (`--random_edges`) |
| `--exhaust_border_color` | off | every random border uses up one frame colour within the bottom row and column 0 rows 1..S (5.3) |
| `--incomplete_top` | off | also write stop-row boards with two of the three segments (`_partial.csv`) or segment B alone (`_partial_B.csv`); not with `--backtrack_row` |
| `--beam_width K` | 250000 | boards per row |
| `--stop_row R` | 11 | last row filled, 1..13 |
| `--backtrack_row N` | off | exhaustive search from row N (5.9) |
| `--backtrack_row_factor M` | 2 | roots = best M x width boards of row N; 0 = every candidate, raw (5.9) |
| `--extend_nodes N` | 100000 | column-major extension budget per board; 0 = off (5.10) |
| `--cap_top [N]` | 1 | close the top-left border exactly over the extension's whole columns; 0 = off (5.10) |
| `--backtrack_min_col K` | 0 | write only boards whose extension fills K columns; cuts partial rows whose column 1 cannot reach row 14 (5.10) |
| `--no_top_dedup` | -- | keep top-row near duplicates (5.9) |
| `--beam_expand E`, `--beam_expand_row R` | 4, 7 | late width multiplier and its row |
| `--lambda_J F` | 1.0 | closure weight |
| `--lambda_Mahalanobis F` | 1.0 | Mahalanobis correction, in its own SD units |
| `--lambda_corners [F]` | off; bare 0.5 | top-corner supply (5.12) |
| `--lambda_reserve F` | 0 | double-decker TOP reserve penalty (5.12) |
| `--clue_center`, `--clue_corners` | off | clues (5.7) |
| `--free_top_clue` | off | the extension may place the top-left row-13 clue anywhere; the top-right one is left for the dives (5.7) |
| `--pin_clue N` | 0 | one centre-clue frame, 1..4; implies `--clue_center` |
| `--end_dive [M]` | off; bare 10000 | finish every stop-row board (5.11) |
| `--end_polish R` | off | polish plus R kick rounds |
| `--emit_score S` | 450 | matched edges a finished board needs to be written |
| `--corner_seeds N` | 4 | corner-seeded copies per board (with `--lambda_corners`) |
| `--frac_rand F` | 0.10 | random selection band |
| `--parent_cap N` | 4 | children per parent in the score band; 0 = uncapped |
| `--pool_factor N` | 8 | candidate pool, x beam width; at least min(8, N) children per parent |
| `--bc_window nB,nC` | 3,3 | B/C completions scored per A record while the beam is full; each 1..128 |
| `--bc_window_accept F` | 1 | probability of keeping the window's best child, else its second best |
| `--no_free_demand` | -- | disable the free-edge demand accounting |
| `--top_bottoms N`, `--top_columns N` | 10, 12 | bottoms per border row, columns per bottom; < 1 = all |
| `--tau_bottoms T`, `--tau_columns T` | 0 | ranking temperatures |
| `--bail_columns N` | 0 | abandon a bottom after N barren columns |
| `--time_limit S` | 600 | per-row deadline within a configuration |
| `--wall_time S` | 0 | total budget; SIGINT/SIGTERM also stop cleanly (current configuration, summary), a second signal kills |
| `--max_emitted N` | 0 | stop after N boards written (with `--end_dive`: cap written boards, never stop the search) |
| `--resume` | off | continue from `sweep_checkpoint.txt`; give the original `--start_row/--num_rows` (and `--rng_seed` at tau > 0) |
| `--threads N`, `--rng_seed S` | all, random | |
| `--verbose`, `--print_cmd` | off | verbose log; echo the normalized command |

**`--bc_window` measured** (two seeds, `--random_edges`, width 200000,
`--stop_row 11`, ~16 min per arm; distinct rows-0..10 foundations per minute):

| window | foundations/min | borders/hour | reached stop row |
|---|---|---|---|
| 1,1 | 657 / 600 | 234 / 260 | 61% / 52% |
| 2,2 | 824 / 759 | 174 / 181 | 70% / 60% |
| 3,2 | 913 / 804 | 154 / 151 | 83% / 83% |
| 3,3 | 867 / 795 | 147 / 147 | 77% / 69% |

A wider window examines fewer borders per hour but more of them reach the stop
row, for more foundations per minute; 3,2 and 3,3 are close (the default is
3,3).

### 5.17 Performance and memory

| phase | cold | with a `--db_file` cache |
|---|---|---|
| inner database build (2 passes) | ~25-60 s | -- |
| promise sort | ~1-3 min | -- |
| startup to first configuration | ~2-5 min | ~1 s (then page-in) |

RAM: 6.4 GB database, the beam workspace (~9 KB per unit of
`beam_width x beam_expand`; ~2 GB at the defaults), ~0.4 GB tables.
`--backtrack_row` adds ~1 MB of search context per thread. The backtracker runs
at 100-200 million nodes per second on 4 threads.

---

## 6. `E555_finalizer` -- the beam restarted from a partial board

`src/B_beam/E555_finalizer.c` (shares the database module and the dive engine).

```
bin/E555_finalizer seed.txt partials.csv [rotations.csv] --finalize_from N --stop_row R [options]
```

For each input board, every piece at or below row `--finalize_from` (default 5)
is **locked**; pieces above return to the pool. The chain database is rebuilt
without the locked pieces, which shrinks it super-exponentially (a
`--finalize_from 10` database: 5.5 M records, ~2 s), and the beam grows the rows
above at full width, with the beamer's scoring, selection, parity test and
backtracker. Output and input are the same CSV dialect, so the finalizer chains
with itself and with the beamer.

### 6.1 Differences from the beamer

- **Beam rows enumerate every conflict-free (B, C) completion** of each A
  record (there is no `--bc_window`): the reduced database is sparse.
- **`--frac_rand` defaults to 0.30** and the first searched row always uses the
  full random band: the tool is meant to be re-run over the same partial
  (`--finalize_repeats N`), and the random band is what makes repeats differ.
  **`--accept_best F`** (default 1) also varies the score band: it takes each
  candidate with probability F and otherwise passes to the next.
- **`--beam_expand_row` defaults to 8** (the search starts at `finalize_from+1`).
- **The Mahalanobis spread** is taken from this row's own earlier measurement,
  then the row below, then the nearest measured row, since the rows below the
  lock were never searched.
- **Column rank** is `sum_r log1p(la_total[right[r]])` over the free rows; the
  beamer's rotation windows would count cells the lock already filled.
- The compact `[sweep]` line keeps the older fields (`filled/died`, `width`,
  `reason`, `emitted` per configuration, `sol_total` run total).

### 6.2 Input handling

- `--start_row/--num_rows` select input lines (0 = to the end). Each line is
  validated: piece types per cell, frame orientation, every match inside the
  locked region. A line with an unplaced cell at or below the lock is skipped.
- **Input dedup.** A line is hashed on what the search will see (the lock row,
  fixed sides above it, the free-piece set, the orientation); a repeat is
  skipped. `--finalize_repeats N` re-runs each line N times on purpose.
- Consecutive lines with the same locked set (plus clue pieces) reuse the
  reduced database.

### 6.3 Side modes

- **Fixed sides**: a line with all 60 border pieces placed keeps them; the left
  column is the input's, and each row's right edge is chosen from the right
  side's pieces.
- **Free edges** (automatic when the border is incomplete, as for every beamer
  partial, or `--free_edges`): all unused edges are candidates for the left
  column and the right terminals. The left column above the lock is sampled,
  `--top_columns` per repeat, each the best of 32 by rank (`--tau_columns` to
  sample instead), or **enumerated exhaustively** with `--top_columns 0`.
- **Rotations-matched**: with the optional third positional (normally the
  rotations file the beamer used), a free-mode line whose locked border matches
  a row of that file (same pieces on the same sides) gets that row's side sets
  back: the left column is enumerated from 14 edges instead of 56, and the right
  terminals and top-border demands come from the row. On the synthetic
  regression this cuts `--finalize_from 10 --stop_row 14` from 593 legal left
  columns to 2. Lines matching no row run in free mode, with a note.

### 6.4 Locked top rows

A line whose border is complete and whose ring is clean (every edge between
consecutive border pieces matched) keeps its clean top rows: `T` is the lowest
row above `--stop_row` with rows `T..15` complete and matched (`T = 15` for a
bare ring). Those rows are locked like the rows below the lock and written out.
The row under them (`T-1`, when it is the stop row) must meet row `T` exactly,
cell by cell; the backtracker then pins every cell's top colour, which makes
`--backtrack_row` the natural way to close onto a locked top. Rows between the
stop row and `T` are left open for `--end_dive`.

- `--keep_ring` also holds the right column in place (each row takes the
  input's own right edge). Use it for a ring the input built as part of a board,
  not for a witness's or a `--with_frame` band's sides, whose order is one
  random trail and often cannot be completed.
- `--free_sides` keeps the locked top rows but treats the sides as piece sets
  only (left column sampled, right edges from the side's pool).
- `--free_top` turns the top lock off.
- With `T <= 14` the corner blocks are already built, so `--lambda_corners` is
  off for that line.

### 6.5 Clues

`--clue_center/--clue_corners` hold the clues while rows above the lock are
rebuilt; without them a lock below row 7 frees the centre cell and the search
fills it with another piece (measured: 0 of 2372 boards kept all five clues
without the flags, 222 of 222 with them). A clue on a searched row is pinned
during generation; a clue on the row above pins a colour. A line whose lock
contradicts a clue is skipped.

The orientation is **read** from a line that carries a clue and **chosen** for
one that carries none: every partial locked below row 7 under `--clue_center`
has committed to nothing and is searched once per orientation its lock does not
contradict (up to 4 passes over one shared database; the clue piece set is the
same in all four). `--clue_orient LIST` (`auto` = all) or `--pin_clue N`
restricts the choice. The row-13 corner clues are reserved and attached to the
written board where their cells are empty.

### 6.6 From a double-decker witness

A Stage A witness board (4.5) with TOP two tall has complete rows 14-15, so the
finalizer locks them and grows the board from `--finalize_from 0` (1 when
BOTTOM is two tall too). Locking at row 0 rebuilds nearly the whole database
per witness (minutes, ~8 GB).

```bash
bin/E555_finalizer data/seed_Edge5.txt rotations_refined_decker.csv \
    --finalize_from 0 --beam_width 250000 --backtrack_row 6 --stop_row 11 \
    --end_dive 20000 --end_polish 50000 --emit_score 0
```

Measured on 8 witnesses (`--clue_center`, width 10000, backtrack from 6 to 11):
kept in place, the witnesses' classic sides killed half the witnesses at row 1
(one random trail order is often impossible), and the locked row 14 holds 14 of
the inner pieces that chain best with the top edges. `--free_top` reached row 11
on 1 of 12 (witness, frame) pairs, the locked variants on none. At
`--finalize_from 0` a witness is more useful for its sides than for its row 14.

### 6.7 Backtracking and end dives

`--backtrack_row N` (`--finalize_from <= N < --stop_row`) runs the beamer's
exhaustive search (5.9). With `N = --finalize_from` no beam row runs: the locked
board is the single root, split over the threads by the hand-off queue.
`--end_dive/--end_polish/--emit_score/--corner_seeds` finish every stop-row
board with the shared dive engine (5.11). `--stop_row` may be 14 (row 15 is
never searched). The finalizer has no column-major extension.

### 6.8 Output

Boards are appended to `<out_dir>/beam_completions_finalized_<stop_row>.csv`
with ids `p<line>r<repeat>l<column>`; each line is one atomic append, so several
processes may share the file. A board carries rows `0..stop_row`, the locked top
rows, the ring under `--keep_ring`, and (known sides) the left column and the
top-right corner. `--max_emitted` stops the run after N boards (the stop-row
beam in flight is written in full).

### 6.9 Choosing `--finalize_from`

On a beamer partial with a complete border (column fixed, `--top_columns`
sampling orderings), lower is better until the beam stops filling: on
`data/board_partial_row12.csv` with 12 columns, 4 reached row 11 on 8 of 12
configurations, 5 on 6, and 6 or higher on none, the beam staying below 1% of
its width (hence the default 5). With an incomplete border, `--top_columns 0`
enumeration grows explosively as rows are freed; 7 is a practical value.

### 6.10 Options

| option | default | meaning |
|---|---|---|
| `--out_dir DIR` | `beam_out` | output directory |
| `--start_row N`, `--num_rows N` | 0, 0 = all | input lines |
| `--finalize_from N` | 5 | lock rows 0..N |
| `--finalize_repeats N` | 1 | sweeps per input line |
| `--free_edges` | auto | free every edge above the lock |
| `--free_top`, `--keep_ring`, `--free_sides` | off | top-lock modes (6.4) |
| `--clue_center`, `--clue_corners` | off | hold the clues (6.5) |
| `--clue_orient LIST`, `--pin_clue N` | auto, 0 | orientations for a clue-less line |
| `--incomplete_top` | off | also write two-segment stop-row boards |
| `--beam_width K`, `--stop_row R` | 250000, 11 | width; last row, up to 14 |
| `--backtrack_row N` | off | exhaustive search (6.7) |
| `--backtrack_row_factor M` | 2 | roots of the search, as the beamer (5.9) |
| `--beam_expand E`, `--beam_expand_row R` | 4, 8 | late width |
| `--lambda_J`, `--lambda_Mahalanobis` | 1.0, 1.0 | scoring (5.5) |
| `--lambda_corners [F]` | off; bare 0.5 | top-corner supply; needs known sides |
| `--frac_rand F`, `--parent_cap N`, `--pool_factor N` | 0.30, 4, 8 | selection |
| `--accept_best F` | 1 | probability of taking each score-band candidate, else the next |
| `--end_dive [M]`, `--end_polish R`, `--emit_score S`, `--corner_seeds N` | off, off, 450, 4 | end dives (5.11) |
| `--no_free_demand` | -- | as the beamer |
| `--top_columns N` | 12 | sampled columns per repeat; <= 0 enumerates all |
| `--tau_columns T`, `--bail_columns N` | 0, 0 | column sampling temperature; abandon a line after N barren columns |
| `--time_limit S`, `--wall_time S`, `--max_emitted N` | 600, 0, 0 | budgets; SIGINT/SIGTERM stop cleanly, a second signal kills |
| `--threads N`, `--rng_seed S`, `--verbose`, `--print_cmd` | all, random, off, off | |

---

## 7. `E555_roundhouse` -- turning the board and refilling strips

`src/B_beam/E555_roundhouse.c` (shared database module; builds its own chains).

```
bin/E555_roundhouse seed.txt boards.csv output.csv [options]
```

A row-wise search has a 16-colour frontier: it cannot be enumerated or tested
exactly. The roundhouse turns the board so that a band of W columns (the
**strip**) lies against the right border, and fills it level by level, each
level one chain of `W-1` inner pieces plus a right-edge terminal. The frontier
is W colours wide, which makes exact search and an exact relaxation possible.

### 7.1 The width-W database and the oracle

Only edge-terminal chains of length W are needed:

| W | cells | records (no exclusions) | size | frontier states |
|---|---|---|---|---|
| 5 | 7.10 M | 228.7 M | 0.51 GB | 417,605 |
| 4 | 418 k | ~5.0 M | ~11 MB | 24,565 |
| 3 | 24.6 k | ~108 k | ~0.3 MB | 1,445 |
| 2 | 1.4 k | ~2.3 k | trivial | 85 |

Excluding the retained board shrinks it further (a W = 5 three-round core:
47.8 M records, ~2 s). No `--db_file` is needed.

Dropping the no-reuse rule turns a strip into a layered graph (nodes
`(level, signature)`, arcs database records). One backward sweep gives which
signatures can still finish the strip and in how many ways (0.4 s per strip at
W = 5). A branch whose signature is dead is cut by one bitset test, so a live
branch can only fail by piece reuse; an empty live set proves the band cannot
be filled by any pieces, reported as `[round] ... status=COLOR_DEAD level=L`.
`--verbose` prints the live count per level.

- **Earlier sides** of a multi-round run must finish (the next round needs their
  wall), so they use this endpoint oracle.
- **The final side** has no such obligation: with no `--stop_row/--stop_after`
  it uses a **depth oracle**, the maximum number of further levels reachable
  when pieces may repeat, and cuts a branch only when that upper bound cannot
  tie the deepest board found. The deepest exact prefix is kept.
- When the strip is the last unfilled region, a parity/supply test (every inner
  colour's surplus non-negative and even) prunes at every node.

### 7.2 Geometry

The board is mirrored by `--cw`, then turned `--rotate K` quarter-turns
clockwise, so the strip is always the rightmost W columns of the frame. Boards
are written back in the input's orientation. `--rounds` sets how many bands are
freed **and refilled** (right, then top, then left of the frame); the cuts nest,
so each round frees exactly what it will refill:

| `--rounds` | frees | kept at W = 5 |
|---|---|---|
| 1 | the right band, `16W` cells | 176 |
| 2 | + the top band, `W(16-W)` | 121 |
| 3 | + the left band, `W(16-W)`; all four corners and 54 of 60 border pieces re-searched | 66 |
| 4 | a centred core, the wall column and a W-piece bottom-right anchor kept; all four sides traversed | -- |

The last round's strip ends on a border chain closing row 15, so a successful run
ends on a complete board. `--rounds 1` covering the whole empty region either
returns a complete board or proves none exists for that core and pool.

| `--rotate` | round 1 | round 2 | round 3 | core hugs |
|---|---|---|---|---|
| 0 | right | top | left | bottom |
| 1 (default) | top | left | bottom | right |
| 2 | left | bottom | right | top |
| 3 or -1 | bottom | right | top | left |

`--cw` mirrors the board left-right (a placement `(p, (r,c), s)` becomes
`(p, (r,15-c), (4-s)&3)`) and gives the four spirals of the other handedness. It
frees no new region; it traverses each band the other way, a different search
over the same cells.

W defaults to 5; `--strip_width 0` picks the narrowest width whose kept region
is complete and break-free (`16 - rows filled` for a board filled in whole rows).
The centre cells are inside the core at every width, so a centre clue is
inherited, never placed.

### 7.3 What it writes

The search is exhaustive and deterministic. A complete board is a solution; an
exhausted search is a proof that this core admits no break-free refill of these
bands, unless a budget (`--max_nodes`, `--time_limit`, `--wall_time`,
`--max_emitted`) stopped it: each input's `[board]` line reports `status=DONE`,
`BUDGET` or `TARGET` (`--target_ties` reached).

Per input board it writes the deepest board reached (most pieces placed);
`--ties N` keeps up to N at that depth that differ at least `--tie_depth` levels
behind the newest placement; `--target_ties N` stops an input once N boards
reach the endpoint. `--breaks B` then fills the rest of the deepest board
greedily (the backtracker's stuck dive), with at most B mismatches; measured on
455-457 boards: 48 freed cells refilled for 27-40 breaks, 160 for 48-52.

Everything goes to the output CSV (replaced at startup), canonical rows with ids
`<input-id>_<line><tag><n>`: tag `s` solved, `d` deepest, `j` hold-join,
`f` break-filled. A break-free partial's score is `480 -` the junctions its
holes leave open, not a measure of damage. `outputs.txt` is written beside it.

`--clue_center` only verifies (the centre is in the core); `--clue_corners`
holds the four corner clues, which the rounds do free, by filtering records and
barring clue pieces from other cells. The oracle stays clue-blind.

### 7.4 Input requirements

Only the kept region is validated, completely: every cell placed, every piece
legal against the frame, every junction matched. Anything outside it is freed,
so broken top rows are fine; a break inside the core is refused with its
location. Inputs are deduplicated on the core. A mid-spiral board feeds the next
run directly if the chosen rotation's core lies in its filled part.

Expected branching per level, `mean_cell x (avail_inner/196)^(W-1) x
(avail_edge/56)`, is ~7.4 at W = 5 in round 1 and ~0.09 at the start of round 3:
the last round is subcritical at every width, and on the real seed strips die
well short of the top. The useful output is the deepest board, for Stage C.

### 7.5 Options

| option | default | meaning |
|---|---|---|
| `--rounds N` | 3 | 1..4 bands (7.2) |
| `--strip_width W` | 5 | 2..5; 0 = narrowest usable |
| `--rotate K` | 1 | -3..3 quarter-turns before the cut |
| `--ccw` / `--cw` | `--ccw` | spiral direction |
| `--hold_band` | off | rounds 1..3: keep the occupied cells in the half of the final side opposite the traversal and search the other half |
| `--stop_row R`, `--stop_after N` | -- | stop the final side at frame level R, or after N new levels |
| `--BL/--BR/--TL/--TR P` | -- | pin a corner piece by its role on the input board |
| `--ties N`, `--tie_depth N`, `--target_ties N` | 1, 2, 0 | output volume (7.3) |
| `--breaks B` | 0 | greedy fill with at most B mismatches |
| `--max_nodes N`, `--time_limit S` | 0, 600 | per input board |
| `--wall_time S`, `--max_emitted N` | 0, 0 | per run |
| `--clue_center`, `--clue_corners` | off | 7.3 |
| `--no_transition_cache` | cache on | decode successors on the fly (debug; must give identical output, checked by the gate) |
| `--start_row N`, `--num_rows N` | 0, 0 = all | input window |
| `--shard_count N`, `--shard_index I` | 1, 0 | every N-th row of the window, for parallel processes (use different outputs) |
| `--threads N`, `--verbose`, `--print_cmd` | all, off, off | threads for database and oracle work; the DFS is serial |

---

## 8. The whirlpool -- re-growing from every side

`pipeline/run_pipeline_whirlpool.sh` chains existing tools. Stage B only grows
rows upward, so the rows a board stands on are never revisited. A lap turns the
board a quarter-turn so they become columns, converts complete columns back into
complete rows, and re-grows:

```
rows 0..T full
  |- rotate +-90       tools/E555_rotate.py in.csv 1   (and 3)
  |- backtracker       --stop_row 5 --with_frame --order rowmajor --break_mode any
  |                    fills rows 0..5 and the 60 frame cells exactly
  '- finalizer         --finalize_from 5 --stop_row T  (fixed sides)
```

Four laps are one full turn. At `T = 11` a lap keeps 72 cells in place (rows
0..5 of the filled columns), rebuilds 24 in the band cut and re-grows 96, with
184 pieces free to the search; no piece survives a full turn untouched.

- `--with_frame` keeps the border through the cut (the finalizer needs all 60
  border cells for fixed sides); it drops boards whose leftover border pool
  cannot close, and it pins the side sets, not their order.
  `FIXED_BORDER=0` runs the free-border lap, which yields about ten times more
  boards at the same depth.
- Every stage writes exactly matched boards, so nothing inside the loop ranks;
  attrition (bands with no exact filling, boards that do not re-grow to T) thins
  the field, and the per-lap counts are the diagnostic.
- The cut is nearly free (one turned synthetic board gave 4.8 M exact bands in
  120 s) and the finalizer's per-band database rebuild is the whole cost, so
  `--max_emitted` (bands per lap = `2 x POP x BT_LIMIT`) defines the run.
  `BT_ORDER` changes which bands the DFS returns first; raising `BAND_ROW`
  shrinks the finalizer's database at the cost of freeing less per lap.
- `WHIRL_ROWS` should stay in 10..12: shallower floods the output, deeper leaves
  nothing for the beam.
- Clues: the finalizer needs the clue flags on every lap. A band cut below row 7
  carries no clue, and the finalizer then searches each orientation its lock
  allows (measured: a rows-0..5 band grown to row 8 gave 33,536 boards over
  three of the four orientations; grown to row 10, only one survived). The
  backtracker is not clue-aware at the cut; the closing dive's `--holes` mask
  must keep the centre cell shut (`data/holes_open_border_TR.csv` does).
- Stage C runs once at the end on the survivors: the roundhouse
  (`--rotate -1 --rounds 3 --strip_width 4`), then the backtracker's dives.

---

## 9. Stage C -- the tail

All Stage C tools read and write the canonical CSV. `--holes FILE` masks are
16x16 0/1 grids, first data line = row 0 (`data/holes_*.csv`).

### 9.1 `tools/E555_distiller.py` -- from a corpus to the boards worth closing

```bash
python3 tools/E555_distiller.py partials*.csv.gz --top 25 --out distilled.csv
```

Reads plain or gzip CSVs of partial or complete boards and keeps N = `--top`
(default 25); `--out` (default `distilled.csv`) and `--seed_file` are its only
other options. With P unique partials:

| stage | method | kept |
|---|---|---|
| read | drop exact repeats; group partials by placed-cell count | all unique |
| screen | best of 300 seeded dives per partial (`E555_diver --end_dive 300`), ties by closure | K = min(ceil(P/2), 400 N), split over groups |
| finish | `E555_diver --end_dive 20000 --end_polish 5000` | K |
| probe | the ender's redive at fixed work (`--reopen auto --rounds 8 --copies 8 --end_dive 3000 --end_polish 1000 --prior 1 --nogo 1`); complete inputs join here; clued boards skip it | 4 N |
| select | best by probed score, then probe gain, finished score, screen score, closure; a board sharing > 80% of its cells with a better one is skipped | N |

Writes `FILE` (canonical rows, best first), `FILE.plan.sh` (the
`E555_ender.py --profile deep` command for them) and `FILE_work/` (every stage's
results; a rerun resumes from it). Every diver call is seeded and keyed on the
board, so the output is independent of threads and interruptions. Cost per board
on 4 threads: screen 0.037 s, finish 0.75 s, probe ~7 s.

Calibration on 300 unique row-11 partials of `data/E565_FixCorners23.csv.gz`
(target: the top 10% by the mean of two full finishes; share of it held by each
predictor's top 25/33/50%):

| predictor | cost / board | Spearman | top 25% | top 33% | top 50% |
|---|---|---|---|---|---|
| closure | ~0 | +0.23 | 51% | 57% | 66% |
| best of 300 dives | 0.037 s | +0.55 | 57% | 69% | 89% |
| one full finish | 0.75 s | +0.9 | 97% | 97% | 100% |

Two full finishes of one board agree only at Spearman +0.62 (0.7 edges apart on
average), which is why the screen keeps half and the top boards are probed
before the final ranking. End to end on that corpus (`--top 5`, 4 threads,
47 min): 30,565 unique partials, 2,000 finished (best 462), output five
distinct 462s.

### 9.2 `E555_diver` -- rebuild the top and finish any board file

```bash
bin/E555_diver seed.txt boards.csv output.csv [options]
```

The diver finishes boards for score. It needs no chain database. Every board goes through the same fixed steps; the later ones are optional:

1. **Select.** `--start_row R --num_rows N` reads data rows R..R+N-1 (0-based, comments not counted). This is a record index, not a board row.
2. **Holes.** `--holes SPEC` lifts cells. SPEC is one of:
   - a 16x16 0/1 mask file, first data line = row 0 (`data/holes_*.csv`);
   - `top:K`, `bottom:K`, `left:K` or `right:K` (the outer K rows or columns);
   - `box:R0-R1,C0-C1`.

   The lifted board is an ordinary partial; nothing remembers what stood there.
3. **Clues.** `--pin_clue N` places the centre clue (piece 138) of clue frame N, using the beamer's numbering: 1 lower-left (7,7), 2 lower-right (7,8), 3 upper-right (8,8), 4 upper-left (8,7). `--clue_corners` also places that frame's four corner clues, including the two on row 13, since the diver fills every cell. A board fails if a clue cell holds another piece, or a clue piece stands elsewhere or at another spin. Such a board is dropped and counted.
4. **Backtrack** (`--backtrack`, or `--stop_row S`, which implies it). This is the beamer's trajectory (5.9, 5.10) run from the board itself. See the rules below.
5. **Dives.** The open cells are filled by the beamer's end-dive engine (5.11), with the same stages and tuning. Placed cells never move. Boards whose best reaches `--emit_score` are written as `config_id, score, pos[256], rot[256]`, best first per batch. `--end_dive 0` writes the prepared boards instead, scored by the matched edges between placed cells.

**Backtrack rules.** A cell takes an unused piece whose sides match every placed neighbour, with frame colour 0 facing exactly the board's edge.
- **Candidates.** Inner cells draw from the catalog bucket of their (left, bottom) colours, in the beamer's order. Border cells draw from the edge and corner orientations; in a framed batch (below) only the spin its rotations row deals is allowed.
- **Top supply.** While the top-border cell above is open, a row-14 piece's top colour must be the inner colour of an unused top-border candidate not yet promised to another row-14 cell. A candidate placed anywhere else spends one.
- **Without `--stop_row`.** The open cells of rows 0..14 are filled in row-major order, and the deepest zero-break prefix is kept (the first found at maximum depth).
- **With `--stop_row S`.** Rows up to S are searched exhaustively in the same order. Every board that completes them is extended column by column (column 0 from the bottom up, then column 1, and so on) over the open cells of rows S+1..14, cols 0..14. Each extension gets `--extend_nodes` nodes and keeps its deepest prefix. The board whose extension goes deepest is kept, the first found on ties, and a completely filled region ends the search.
  - A board that never completes row S keeps its deepest row prefix and is dived from there; the summary counts it as not reaching S.
- **Output and budget.** Each input gives at most one board. The budget is 10^7 nodes per board, extensions included, which is about 0.3 s. The top border and the right column above S are left to the dives, as in the beamer.
- **Parity with the beamer.** On a beamer stop-row board written with `--extend_nodes 0`, `--stop_row S` places exactly the cells the beamer's own extension places. The gate checks this board for board.

The diver deliberately has no break-tolerant search, no alternative cell orders and no enumeration of many solutions per board. For those (a band's every solution, bounded mismatches, Hall pruning), use `E555_backtracker` (9.5).

**Batches and frame.**
- **Batches.** Consecutive rows with the same config id form a batch, because stage 2 of the dives is selected within a batch. A beamer file therefore replays configuration by configuration.
- **Without `--rotations`.** Any unused edge piece may take any open border cell.
- **With `--rotations FILE`.** A beamer id `r<N>b...`, or `NAME_r<N>b...` from a `--prefix` run, holds edge pieces to the sides that row N deals them, in the backtrack and in the dives. A board whose edges do not sit where row N deals them is prepared and dived with free edges, in a run of its own under the same id. An id naming no row (finalizer, random-border or hand-made boards) is dived with free edges.
- **Determinism.** Boards are searched in parallel but each serially, and every dive's random stream is keyed by its board and `--rng_seed`. The output does not depend on `--threads`.

**Re-processing old boards.** Lift the rows an older run built, rebuild them with the current trajectory, and finish:

```bash
bin/E555_diver data/seed_Edge5.txt old_boards.csv redone.csv \
    --rotations rotations.csv --holes top:6 --stop_row 11 \
    --end_dive 10000 --end_polish 20000 --emit_score 455 --threads 16
```

- `--holes top:6` lifts rows 10-15, and rows 10-11 are rebuilt with zero breaks.
- Add `--pin_clue N` when the lifted rows include the centre clue's cell, and `--clue_corners` when they include row 13's clue cells. A clue below the lifted rows must already be on the board, or the board is dropped.
- Use `--end_dive 0` first to see how many boards reach S, and how deep their extensions go.

**Measured** on 40 row-12 partials from an older beam (`data/E565_lowB_baseline.csv`), 2000/200 dives per board:

| run | distinct boards | mean | max |
|---|---|---|---|
| `--holes top:6 --stop_row 11` | 16 | 453.8 | 457 |
| `--holes top:6`, no rebuild | 16 | 452.9 | 455 |
| the partials dived unchanged, rows 0–12 kept | 40 | 456.6 | 459 |

- Once rows 10–15 are lifted, only 16 of the 40 boards are distinct; the engine drops exact repeats. On those, the rebuild beats a plain dive of the same holed board, in less than half the time.
- Lifting rows that a beam built well costs more than rebuilding gains: dived unchanged, the original partials score higher. Lift the rows you don't trust (the top of the partial and anything above), not the rows the beam built well.

**Improving complete boards** (`--reopen SPEC`). Each complete board is cut and dived again with the complete board as its **incumbent**, so the board written is never worse than the input.
- **SPEC** is one of:
  - `auto`: the outer band of the side holding the most damaged cells, deep enough for 90% of them, at most 5 rows;
  - `auto+E`: E more rows;
  - `top:K`, `bottom:K`, `left:K` or `right:K`;
  - `box:R0-R1,C0-C1`;
  - a mask file.

  Given n times, round r uses the (r mod n)-th spec.
- `--reopen` does not combine with `--holes`, which makes every board a partial with no incumbent, nor with `--backtrack`/`--stop_row`.
- **Measured** on 24 dived-and-polished boards (459-461: `data/E565_lowB_baseline.csv` finished with 3000/10000), with `--reopen auto --copies 8` on 4 threads (`examples/11_diver_reopen.sh` runs the best):
  - 15 s per board: `--end_dive 1000 --end_polish 10000` gained +1.03 edges per board (81% of 72 runs). 500/7000, 4 copies of 2000/20000, `auto` and `auto+1` in turn, `--plateau`, and `--prior 1 --nogo 1` all came within 0.1 of it. 10000/10000 gained +0.88; 3000/1000 with `--prior 1 --nogo 1`, the best setting on the previous engine, +0.58.
  - 120 s per board, 12 of the boards: 1000/10000 +1.33 (11 improved), 10000/10000 +0.92, 3000/1000 +0.58.
  - The seven 463s of `data/best_463.csv`, 120 s each: the first gains 2 under both settings, the other six none.

A finished board turned 180 degrees, with its top K rows lifted so that a perfect refill exists, is refilled perfectly on:
- 15 of 15 boards at K = 3-4;
- 3 of 15 at K = 6;
- 0 at K = 8.

The breaks a dive leaves in the top rows of a Stage B board are forced by the pieces left against the rows below. A gain needs the row under the damage lifted too.

| option | default | meaning |
|---|---|---|
| `--start_row R`, `--num_rows N` | 0, 0 (all) | input records R..R+N-1 |
| `--holes SPEC` | off | lift cells first: mask file, `top:K`/`bottom:K`/`left:K`/`right:K`, `box:R0-R1,C0-C1` |
| `--pin_clue N` | off | place clue frame N's centre clue (1..4, as the beamer) |
| `--clue_corners` | off | with `--pin_clue`: also its four corner clues; shapes `--corner_seeds` blocks |
| `--backtrack` | off | zero-break row-major fill of rows 0..14, deepest prefix kept |
| `--stop_row S` | off | rows up to S, then the column extension; the deepest extension kept (implies `--backtrack`) |
| `--extend_nodes N` | 100000 | node budget of each extension (0 = the first board to reach S) |
| `--cap_top [N]` | 1 | as the beamer's: of the longest extensions keep the one whose top-left border closes over the most whole columns, and place that closure (0 = off) |
| `--end_dive M` | 10000 | dives per board; 0 = write the prepared boards |
| `--end_polish R` | off | polish plus R kick rounds |
| `--polish_top K` | 64 | dives polished per board, K/2 walks (1..256) |
| `--emit_score S` | 450 | write boards whose best is >= S |
| `--rotations FILE` | free edges | hold edge pieces to their dealt sides |
| `--corner_seeds N` | 0 | corner-seeded copies (needs `--rotations`) |
| `--reopen SPEC` | off | re-dive complete boards, never worse |
| `--rounds N` | 1 | rounds from the best board so far (continue until `--wall_time` if set) |
| `--copies K` | 1 | dive each board K times on separate streams; keep the best |
| `--prior A`, `--nogo B` | 0, 0 | starting weights: +A on the incumbent's clean placements, -B on its broken cells (0..2) |
| `--plateau` | off | a round may move to a different board of equal score |
| `--orders LIST`, `--order_weight W` | `mrv`, 6 | copy k breaks cell-choice ties toward `left`, `right`, `centre`, `ends`, `top`, `bottom`, with strength W |
| `--threads N`, `--rng_seed S` | all, 1 | output independent of threads |
| `--wall_time S`, `--max_emitted N` | -- | stop after the batch in flight |
| `--print_cmd`, `--verbose` | off | |

**Summary lines.**
- `[sum] holes`: cells lifted per board.
- `[sum] clues`: pieces placed, boards dropped.
- `[sum] backtrack`: boards, the highest full row, how many reached S, and node-cap hits.
- `[sum] extension`: extension cells and whole columns, mean and max.
- Then the dive engine's lines (5.14).

`E555_diver.c` is the front end: input, preparation, batches, frame and reopen. Policies and stages live in `E555_dive.c`, shared with the beamer and the finalizer.

### 9.3 `E555_ender.py` -- the closer

```bash
src/C_tail/E555_ender.py seed.txt boards.csv out.csv --profile deep [--threads N]   # python3 in front optional
```

Re-solves regions of a complete board exactly (the region's pieces may permute
within their class and re-rotate; everything else is fixed) and re-dives its
damaged band with `bin/E555_diver`. It never returns a worse board. Needs
OR-Tools.

**Model** (the Boolean encoding of Heule, *Solving edge-matching problems with
satisfiability solvers*, 2008): a literal `x[c,p,s]` per cell, piece and spin
(frame-legal only), exactly one per cell and per piece; a colour literal per cell
side and a match literal per junction and colour. The incumbent is kept feasible
(breaks <= current) and given as the hint, so CP-SAT's LNS workers start from it;
in `improve` mode a call stops at the first strict gain. Regions proved optimal or
infeasible are cached until a cell in or around them changes.

| step | region | method |
|---|---|---|
| `swap` | a maximal set of pairwise non-adjacent cells, damaged first | linear assignment: every exchange cycle over the set |
| `redive` | the band holding 90% of the damage | `E555_diver --reopen` with the band as a mask, 8 copies of 1000 dives + 10000 polish rounds |
| `window h x w` | every h x w and w x h rectangle touching a break | CP-SAT |
| `corner d` | a d x d corner block plus the frame arms beside it | CP-SAT |
| `band k` | the k rows or columns with the most damage | CP-SAT |
| `frame` | the 60 frame cells plus the damaged inner-ring cells | CP-SAT |

| profile | budget / board | threads | plan (cheapest first, restarting after every gain) |
|---|---|---|---|
| `overnight` | 180 s | 4 | swap, redive 90 s, windows 3x5 4x5 4x7, band 3 |
| `deep` | 900 s | 8 | swap, redive 180 s, windows 4x4 4x6 5x6, corner 5, redive 180 s, window 5x8, band 4, frame, corral 5x6 |
| `superdeep` | 7200 s | 12 | swap, redive 600 s (16 copies), windows 4x4 4x6 5x6, corner 6, redive 600 s one row deeper, windows 5x8 6x8, band 5, frame, corral 5x8 |

Step caps scale with `--board_time_limit`. On dived-and-polished Stage B boards
(457-463) every 4x4 window touching a break was already optimal (78/78), and no
exact region of 48-102 cells gained; the redive of the damaged rows plus the
clean row under them gained +1 on 2 of 8 boards and turned a 463 into
`data/best_465.csv`. The CP-SAT steps matter on unpolished boards. With the
redive at 1000/10000 instead of 3000/1000 `--prior 1 --nogo 1`, the overnight
plan at 30 s a board gained +0.96 edges per board on 24 such boards (459-461)
instead of +0.50: 9 boards better, none worse. Every gain there came from the
redive (none from 566 CP-SAT calls), and a re-dived band keeps gaining with
time (9.2, `--reopen`), so the redive calls get long walls.

**Output.** First the files, the number of boards, the threads, each plan step
with its real cap at this budget, and the latest finishing time; then per board
its score, every gain as it lands (the time into the board and the step that
found it), a progress line after two quiet minutes, and a closing line (calls,
proofs, redives, why it stopped); last, the scores before and after.

| option | default | meaning |
|---|---|---|
| `--profile` | `overnight` | plan, budget and threads above |
| `--board_time_limit S` | profile's | wall clock per input board, every call included |
| `--threads N`, `--jobs N` | profile's, threads/4 | CP-SAT workers; regions solved at once |
| `--corral` / `--no-corral` | on in deep/superdeep | when nothing gains, accept equal-break moves toward the nearest corner |
| `--redive` / `--no-redive` | on | the redive step (`--diver PATH` for another binary) |
| `--search_mode` | `improve` | `optimize` spends each call's cap |
| `--clue_center`, `--clue_corners`, `--clue_orient` | off, off, auto | pin clues in place; repair a displaced clue first |
| `--start_row`, `--num_rows`, `--shard_count`, `--shard_index`, `--resume` | 0, 0, 1, 0, off | input window and sharding |
| `--rng_seed S` | 0 = random (printed) | |
| `-v`, `--verbose` | off | also every CP-SAT call and redive; `-vv` draws each accepted region |

Advanced controls (`--show_advanced --help`):

| option | default | meaning |
|---|---|---|
| `--attempt_time S` | -- | cap every CP-SAT call |
| `--holes FILE` | -- | only these cells may move; the whole mask is also solved as one region |
| `--max_new_breaks N`, `--max_changes N` | no cap | junctions matched in the input a move may break; cells one move may change |
| `--preserve_clean` / `--no-preserve_clean`, `--preserve_side {auto,any,all,B,T,L,R}`, `--max_clean_loss N` | on, auto, -- | protect the board's clean foundation rows/columns |
| `--duplicate_policy {reuse,rerun}` | -- | exact duplicate inputs |
| `--max_passes N` | 0 = budget only | passes over the plan per board |
| `--diver PATH` | `bin/E555_diver` | the redive binary |
| `--symmetry_level`, `--linearization_level`, `--log_search` | solver defaults | OR-Tools controls |

### 9.4 `E555_topper.py` -- CP-SAT break minimizer over border bands

OR-Tools CP-SAT over the open cells of a band: per cell (piece, spin, four
exposed colours) with allowed-assignment tables filtered by the frame rule and
all-different per piece class. The objective is lexicographic by dominating
weights: (1) total breaks, (2) the distance of each break to its nearest
horizontal border `min(row, 15-row)`, (3) the distance along it to the nearest
corner `min(col, 15-col)`. Worst case 7 + 7, and the four corners share the
load.

- `--side` opens bands `--band_depth` (default 4) deep: `T` (default), `B`,
  `R`, `L`; L-shapes `TR`, `TL`, `BR`, `BL`; pairs `TB`, `LR`.
- `--locked_rows N` unsets the outermost N rows/cols of each band and keeps them
  empty for the run, so the next, narrower pass can refill them (a sliding
  window). A break cell is freed only if it lies in or touches the band.
- `--holes FILE` replaces the bands with an explicit mask, taken literally.
- `--top N` returns N distinct boards: each further rank is re-solved under a
  no-good cut requiring `--beam_diff` (default 4) differing cells from every
  earlier rank, with at most `--beam_slack` (default 1) extra breaks.
- `--clue_center/--clue_corners/--clue_orient` pin the clues (all four corner
  clues can be enforced here).
- Budgets: `--time_limit` (300 s per board, split over ranks), `--stall_time`
  (120 s), `--threads` (8).

| option | default | meaning |
|---|---|---|
| `--side`, `--band_depth N` | T, 4 | bands to open |
| `--locked_rows N` | 0 | outer rows/cols of each band unset and kept empty |
| `--holes FILE` | -- | explicit mask instead of bands |
| `--top N`, `--beam_diff N`, `--beam_slack N` | 1, 4, 1 | distinct ranks per board |
| `--beam_diff_mode {piece,placement}` | piece | whether a rotation-only change counts as a difference |
| `--rank1_fraction F` | 0.60 | share of the board's time reserved for rank 1 when `--top > 1` |
| `--relax_breaks` | off | let the break count trade against a longer corner pull |
| `--clue_center`, `--clue_corners`, `--clue_orient` | off, off, auto | clues |
| `--time_limit S`, `--stall_time S` | 300, 120 | per board (split over ranks); per rank without improvement |
| `--threads N`, `--rng_seed S` | 8, 0 = OS entropy | |
| `--symmetry_level`, `--linearization_level` | 2, 1 | OR-Tools levels |
| `--repair_hint` / `--no-repair_hint`, `--hint_conflicts N` | off, 1000 | repair the hint before search |
| `--start_row`, `--num_rows` | 0, 0 = all | input window |
| `--tag_id`, `--log_search`, `--verbose` | off | append `_<score>` to ids; solver logs; telemetry |

`pipeline/topper_sweep.sh PRESET=...` chains passes `SIDE DEPTH LOCKED`:

| `PRESET` | passes | use |
|---|---|---|
| `safe` | T 5 0; TR TL TB R L B at 3 0 | nothing is unset, so no pass makes a board worse; start here |
| `window` | T 8 3, T 6 2, T 5 1, T 4 0, TR 4 0, L 4 0 | the general sliding window |
| `deep` | each of the seven sides as a pair: a wide pass with the outer band unset (T 7 3, others 5 2), then a narrow refill (4 0, 3 0) | spend breaks to buy freedom; never prune between the two halves of a pair |
| `closeT/B/R/L` | X 6 2, X 4 0 | a hole against one border, e.g. a roundhouse `d` board |

### 9.5 `E555_backtracker` -- exact and bounded-mismatch DFS

```
bin/E555_backtracker seed.txt boards.csv output.csv [options]
```

A depth-first search over a board's empty cells (or a `--holes` region of a
complete board) with forward checking: a static (side, colour) orientation
bitset index gives every empty cell's exact-fit count in O(words), with
immediate cutoffs on an empty domain, plus a global empty-domain bound,
incremental colour/type accounting and a Hall/deficiency bound (`--hall`).
Candidates are enumerated by break class, so their break counts come from
counters rather than a scan.

`--break_mode` selects the engine:

- **`stuck`** (default): greedy dives for triage, the engine the end dives use.
  Each dive takes an exact fit where one exists and a minimal break where none
  does, never backtracks and always completes; `--restarts N` (default 50000)
  dives, best kept. Value ordering (off with `--no_lcv`) plays each candidate and
  prefers the one stranding fewest cells (~2.1x the cost per dive, 1-2 more
  matched edges per board at equal time). Best-of-N improves only
  logarithmically (26 -> 22 breaks from 2k to 2M dives on one board). It proves
  nothing.
- **`any` / `lds`**: exhaustive, iterative deepening over
  `k = input breaks .. --breaks`. An exhausted level proves no completion with
  at most k breaks exists. `--lds_max` caps voluntary mismatches per path.

`--order` (default `mrv`, the most constrained cell) also offers `rowmajor`,
`colmajor`, `snake`, `spiral`, `centerout`, `spiralout`, and the side modes
`2sides`/`4sides` (exact layers from the sides inward, `--breaks 0`).
`--reverse` flips a static order's direction (for `mrv`, a column-major
tie-break); it changes the visiting order, not the solution set. `--jump` skips a
dead cell to build a large break-free partial.

**Stop bands.** `--stop_row N` / `--stop_column N` search only rows (columns)
`0..N`, clearing the rest (their pieces return to the pool), and write every
filling of the band to `<out>.stop_row<N>.csv` as a finalizer input
(`--finalize_from N`). `--reverse` anchors the band at the far side.
`--max_emitted` defaults to 1 here (0 = all; rows 0..3 alone gave 7.6 M bands and
11 GB in five minutes). `--with_frame` adds the 60 frame cells to the band:
frame cells the input holds are kept, missing ones are searched, and a band only
counts once its frame closes. Requires `--breaks 0` and no `--jump`.

**Clues.** `--clue_center`, `--clue_corners` (all four corner clues) and
`--clue_orient N` put the clue pieces on their cells before the search, after
`--rotate`, `--holes` and dedup, so a hole that frees a clue cell makes it
placeable and a break the clues cause counts as an input break. The orientation
is read from a board that carries a clue; `--clue_orient` applies only to a board
carrying none. A board whose clue cell is taken, or whose clue piece sits
elsewhere, is reported and not written. Refused with a stop band.

**Early release.** `--early_release N` stops the exact search at the first
break-free board with only N of its searched cells still empty, the last N of
`--order` (so `--reverse` moves them), and writes it as the record's board.
`--holes H --order spiralout --early_release 60 --breaks 0`, with H opening the
frame and the rings to re-grow, leaves the border ring for the CP-SAT tools.
The search above the gate is unchanged. Requires `--breaks 0`; the usage text
(run without arguments) lists where the gate falls per order.
`--early_release R,C` (row, column, 0-based) names the last cell to place
instead: each record's N is the number of its searched cells after it in
`--order` (static orders only), so `--order spiralout --reverse
--early_release 14,14` leaves the border ring. A board on which the cell is
not open is reported and not written.
`--order frontier` puts every open cell in a layer, its distance (diagonal
steps count one) to the nearest placed piece, and fills the innermost
unfinished layer first, fewest fits first: it grows outward from the placed
pieces, and `--early_release R,C` then releases after that cell's whole layer.
The run prints the first row's layer map (`#` placed, digits the layers, `*` the
release cell) with the cells per layer.
`--no_colour_count` (like `--no_hall`) drops a start-of-record check that sees
the cells to be released too; neither is recorded by `--resume`.

**Resume.** `--resume` splits one exact search over many short runs. Every
output row carries a resume identifier as field 3 (515 fields): where the
record's search stopped, or `done`. Fed back with `--resume`, a row goes on
from there, so the runs together search exactly the tree of one long run.
After a released board, the next run releases the next one. The row's board is
the best over all runs, never worse.
- Rows without an identifier start fresh. `--holes` and the clue flags apply to
  them only, and dedup skips those identical after `--holes`; under `--resume`
  a skipped duplicate is not written.
- Each record runs on one thread; records still run in parallel.
- `--order`, `--reverse`, `--rotate` and `--early_release` must match the
  identifier; `--threads`, `--time_limit` and `--hall` may change between runs.
- Requires `--breaks 0` and `--max_emitted 1`. Refused with 2sides/4sides,
  `--jump`, a stop band and `--all_for_one`.
- `--resume_score S` skips, and does not write, the rows scoring below S.
- Every run ends with `output scores: ... median=M (--resume_score T keeps K)`.
  M is the true median (the mean of the middle two for an even count), T =
  ceil(M) keeps the K rows at or above it, about half. std is over n.
- A row already `done` is not written again: the file that first wrote it keeps
  it (collect the finished boards with `grep -h ';done,' s*.csv`).
- SIGINT/SIGTERM stops the running searches as their time limit would; their
  rows go out resumable, and the rows not started are copied unchanged.
- Without `--resume`, an identifier row is a plain board. Use that to finish
  the best boards with every thread.
- `rank.py --top/--out` keeps identifiers; `--rescore` drops them. The format
  is documented in `E555_backtracker.c` ("Resume identifier").

```bash
E555_backtracker seed beams.csv s1.csv --holes H --order spiralout --early_release 60 --resume --time_limit 120
E555_backtracker seed s1.csv s2.csv --order spiralout --early_release 60 --resume --resume_score M --time_limit 600
E555_backtracker seed elite.csv done.csv --breaks 0          # no --resume: all threads per board
```

**Parallelism.** One record per thread, or all threads on one record when there
are no more records than threads (`--all_for_one` forces it). The search is
memory-bandwidth bound (four independent single-thread runs reach 2.44x
aggregate on 4 cores). It uses `popcount` heavily: build with a non-generic
`ARCH` for real runs (`generic` costs ~12% of dive throughput and ~39% of DFS
node rate).

| option | default | meaning |
|---|---|---|
| `--holes PATH`, `--rotate K` | --, 0 | reopen a masked region; turn K quarter-turns CCW first |
| `--break_mode`, `--breaks K` | stuck, 0 | engine; break ceiling |
| `--restarts N`, `--no_lcv` | 50000, off | dives; value ordering off |
| `--order`, `--reverse`, `--jump` | mrv, off, off | cell order |
| `--no_colour_count` | off | skip the colour/type count (root and per placement); the no-fit cut stays |
| `--hall MODE`, `--hall_stride N`, `--hall_min N` | root, 8, 32 | Hall bound: `off`, `root`, `adaptive`, `always` (`--no_hall` = off) |
| `--lds_max N` | -- | voluntary mismatches per path (lds) |
| `--time_limit S` | unlimited exact, 30 s mismatch | per record; SIGINT/SIGTERM stop every running record the same way (rows not started: copied under `--resume`, else not written), a second signal kills |
| `--max_emitted N` | 1 | completions per record (0 = all) |
| `--stop_row N`, `--stop_column N`, `--with_frame` | -- | band enumeration |
| `--early_release N` or `R,C` | 0 | emit the break-free board N searched cells before the end, or right after cell (R,C) |
| `--resume` | off | continue each row's search from its identifier; write one on every row |
| `--resume_score S` | -- | with `--resume`: skip the rows scoring below S |
| `--clue_center`, `--clue_corners`, `--clue_orient N` | off | clues |
| `--start_row`, `--num_rows`, `--dedup`/`--no_dedup` | 0, all, dedup | input |
| `--best_n N`, `--status` | --, off | side files |
| `--threads N`, `--all_for_one`, `--verbose`, `--version`, `--print_cmd` | all | `--verbose`: statistics per row, and one example row drawn before and after its search |

Output: one best-board row per input record in `output.csv` (with `--resume`,
the identifier third; the run ends with the min/max/mean/std/median score), plus
`output.csv.checkpoint.csv` (crash recovery), `.status.csv`, `.best_pure.csv`,
`.best_mismatch.csv` and the stop-band files. The gate rebuilds the solver with
`-DVERIFY_BREAKCOUNT` to check the break counters against a full scan.

---

## 10. Tools

- **`tools/E555_viewer.py`** -- ASCII board (`#` marks broken junctions),
  placement and match statistics, frame check, an e2.bucas.name URL; `--diff A B`
  compares two rows; `--row N`, `--all` (one URL per row), `--name`, `--no_board`,
  `--no_url`, `--seed_file` (default `./seed_Edge5.txt`, then `data/`). It is also
  the shared Python module (clue table, board
  parsing) that the rank tool and the CP-SAT tools import.
- **`tools/E555_rank.py`** -- ranks board CSVs by measures the score cannot see:
  `breaks`, `score`, `solid`, `placed`, `border`, `break_rows`, `break_cols`
  (compactness), `clean_b/t/l/r` (break-free rows or columns from each border),
  `corner_d` (distance of the breaks to their nearest corner), `clues` (clue
  pieces in place, 0..5). `--sort` takes comma-separated keys (`--sort=-KEY`
  inverts). `--out F` writes the rows re-ordered verbatim; `--rescore` rewrites
  them canonically with the score recomputed. `--diverse K` picks K boards
  farthest-first on cell agreement; `--max_agree P` drops near-duplicates;
  `--diversity_box` and `--diversity_metric {cells,fraction}` set where and how
  agreement is measured. `--group_box R0:R1,C0:C1` / `--unique` group boards by a
  rectangle's (or the whole board's) contents and keep `--per_group` per group
  before ranking; `--border_only` keeps boards with a clean complete border.
  Output: a table (`--no_id`, `--quiet`) or CSV (`--csv`); `--count`,
  `--field NAME` and `--split KEY N A.csv B.csv` serve scripts; `--seed_file`.
  `--top N` streams with bounded
  memory; `--max_mem` (8 GB) refuses larger inputs up front.
- **`tools/E555_rotate.py`** -- turns every board by N quarter-turns clockwise
  (`--all` writes all four), losslessly and re-scored; `--holes` turns a mask
  with it (`--holes_out`; `--out` names the board file; `--seed_file`); `--rotations` turns a Stage A rotations file (a border piece's spin
  fixes its side). `--sink M` moves every piece M rows down, not losslessly: the
  bottom M rows leave the board, row 0 and rows `15-M..15` open (`16(M+2)` cells),
  giving a Stage C partial with a fresh top; `--clue_center` keeps only boards
  whose centre clue is right after the transform.
- **`tools/E555_sort_rotations.py`** -- orders a rotations file (the beamer reads
  borders in file order) by `score`, `score_dd`, a side's count, or the
  constraint-first keys `min_side`, `max_side`, `spread` (= ln max - ln min).
  `--max_top/--max_right/--max_bottom/--max_left` and
  `--min_top/--min_right/--min_bottom/--min_left` turn each row so its largest or smallest count lands
  on the named side, appending a `Turn=` note; `Score=` is not rewritten.
  `--top N` keeps the best N. Writes to stdout unless `-o`; `--seed_file` is
  read only for a turn.
- **`tools/E555_extract_consensus.py`** -- for a pool of clued partials: brings
  each board to orientation 0, builds the piece-by-cell frequency table of the
  pool, and scores each board by the mean over its cells of
  `log2(P(piece | cell) x K)` (K = 4, 56 or 196 by cell kind), leave-one-out, on
  the cells the whole pool placed (`--cells common`, so the stop row does not
  drive the ranking). `--best_top/--best_bottom` score the bag of pieces still
  to place. `--BL/--BR/--TL/--TR` filter by corner assignment. `--border_out F`
  distils the table into Stage A rows, one per corner class supported by at
  least `--min_corner_boards` boards: the best assignment (Hungarian over 56x56
  plus the 24 corner permutations) repaired by the annealer's moves until every
  side has `--min_trails` trails, within `--border_time`. It also writes
  `<stem>_frame.csv`, the 60 border pieces laid out as the maximum-consensus Euler
  trail per side (exact subset DP), a finalizer input in fixed-sides mode.
  Other options: `--metric {lift,logp,rank,top1}` (sort key; all four are
  printed), `--alpha` (smoothing, 0.5), `--loo {auto,on,off}`, `--min_support N`
  (2), `--common_frac F` (1.0), `--box R0:R1,C0:C1` (score a rectangle, canonical
  coordinates), `--band_rows D` (5) and `--min_band_support N` (25) for the bag
  scores, `--consensus_out/--consensus_in FILE` (score one corpus against
  another's table), `--top`, `--out` (rows re-ordered verbatim), `--csv`,
  `--no_id`, `--quiet`, `--count`, `--max_mem`, `--progress_every`,
  `--seed_file`; for the border search `--border_pin N` (the `--pin_clue` frame
  to emit in), `--border_rows N` (borders per corner class), `--w_consensus`
  (1.0), `--w_trails` (0.5), `--border_steps` (100000), `--border_restarts`,
  `--border_T0/--border_Tf`, `--border_seed`.
- **`tools/E555_clean_csv.py`** -- drops boards that repeat an earlier board
  with one frontier piece swapped.

---

## 11. File formats

| producer | file | layout |
|---|---|---|
| annealer | rotations CSV | `# comment` (counts, `Score=`, optional `Score_dd=`, `Decker=`, `DeckerPool=`, `Board=`, `From=`), then `id, spin[0..255]`: 60 border spins, inner pieces 0 or a side code 1/2/3 |
| annealer | `<stem>_decker.csv` | per border: `# <name> reserve SIDE=ids ...`, then `<name>, 0, pos[256], rot[256]` |
| beamer | `beam_completions_<border row>_<stop>.csv`, or `beam_completions_random_<stop>.csv` | `config_id, index, pos, rot`; under `--end_dive`, `config_id, matched, pos, rot`; ids `[prefix_]r<row>b<bottom>l<column>` (`rndb<b>l<l>` when random) |
| beamer | `..._partial.csv`, `..._partial_B.csv` | `--incomplete_top` boards |
| beamer, finalizer | `sweep_checkpoint.txt`, `outputs.txt` | resume state; files written by the run |
| finalizer | `beam_completions_finalized_<stop>.csv` | as the beamer; ids `p<line>r<repeat>l<column>` |
| roundhouse | the output CSV (third positional) | canonical; ids `<input>_<line><tag><n>`, tags `s d j f` |
| diver | the output CSV, `<out>.outputs.txt` | canonical, matched edges in field 2 |
| backtracker | output CSV, `.checkpoint.csv`, `.status.csv`, `.best_*.csv`, `.stop_row<N>.csv` / `.stop_col<N>.csv` (`_rev`) | canonical; with `--resume`, `config_id, score, identifier, pos, rot` |
| topper, ender, distiller | output CSV | canonical |

## 12. Build and test

```bash
make                  # bin/E555_beamer, E555_finalizer, E555_roundhouse, E555_backtracker, E555_diver
make ARCH=v3          # x86-64-v3 (AVX2): one binary for a mixed cluster
make ARCH=generic     # CI, containers, cloud sandboxes
pip install ortools   # topper and ender only
bash tests/run_tests.sh                       # the core checks (~20 s)
bash tests/run_tests.sh beamer                # the checks of one tool (see --list for tags)
bash tests/run_tests.sh --all                 # the release gate
ARCH=generic SKIP_BEAMER=1 bash tests/run_tests.sh --all   # without the 6.4 GB database
```

`make` compiles for the build machine (`-march=native`); such a binary dies with
`Illegal instruction` on an older CPU. Changing `ARCH`/`OPT`/`CC` forces a
rebuild. The core checks include the synthetic regression: the finalizer and
the roundhouse must rediscover `data/synth_solution_480.csv` from
`data/synth_seed.txt`.

## 13. Source files

| file | role |
|---|---|
| `src/A_border/E555_edge_annealer.py` | Stage A: BEST-theorem trail counts, annealing, double decker |
| `src/B_beam/E555_database.{c,h}` | seed and catalog, chain database and cache, fan-out table, border enumeration and ranking, clue table, top-corner catalog |
| `src/B_beam/E555_beamer.{c,h}` | beam, backtracker, column-major extension, sweep driver, CLI |
| `src/B_beam/E555_finalizer.c` | beam from a partial: locking, reduced database, side modes, locked top rows |
| `src/B_beam/E555_roundhouse.c` | strip search: width-W database, oracles, spiral geometry |
| `src/C_tail/E555_dive.{c,h}` | end-dive engine: dives, cross-entropy rounds, polish, corner seeds, copies and incumbents, job queue |
| `src/C_tail/E555_diver.c` | the dive engine on any board file; `--holes`, `--pin_clue`, `--stop_row`, `--reopen` |
| `src/C_tail/E555_ender.py` | exact region re-solves and redive |
| `src/C_tail/E555_topper.py` | CP-SAT band minimizer |
| `src/C_tail/E555_backtracker.c` | exact / mismatch DFS, stop bands |
| `tools/` | viewer, rank, rotate, sort_rotations, extract_consensus, distiller, clean_csv |
| `examples/`, `pipeline/` | one script per tool; long runs |
| `tests/run_tests.sh` | the release gate (`--list` for its checks) |
| `data/` | seeds, the synthetic solution, example boards, hole masks (`data/README.md`) |
