# Data-driven beam steering

A fork of the Stage B beamer that learns **where each piece tends to sit** on an
anchored board, then adds that positional evidence to the beam's score. It is
self-contained: one C file, its own `Makefile`, a viewer, and runners.
`src/B_beam/E555_database.c` is linked unmodified.

```bash
cd tests/datadriven && make ARCH=generic     # -> bin/E555_beamer_datadriven
```

The stock objective is local: a colour ledger plus a one-row fan-out lookahead.
It has no opinion about which piece belongs where on the board. With the centre
clue pinned (`--pin_clue`) and a rotations row fixing the corners, the puzzle is
anchored. "Piece *p* tends to sit at cell *x*" is then a geometric statement that
can be measured.

## Two phases

Learning and searching are separate invocations of the same binary.

| flag | meaning |
|---|---|
| `--learn PATH` | Grow the board from each of the four sides in turn. Count where each piece lands in the canonical frame, and write the raw counts to `PATH`. Emits no boards. |
| `--table PATH` | Search with the table: in the beam score, and in the ranking of bottom rows and left columns. |
| `--freq_model M` | The beam's spatial resolution. `segment` (default) pools the 5-5-5 A/B/C bins; `cell` uses exact cells. See *Choosing `--freq_model`*. |
| `--lambda_corners [F]` | Search only, with or without a table: keep the blocks around the two row-13 clues buildable. See *Corner supply*. |
| `--backtrack_row N` | Search only: stop the beam at row N, then search every row-N candidate exhaustively up to `--stop_row` and emit every board that completes it. See *Backtracking to the stop row*. |
| `--end_dive M` | Search only: complete every stop-row board to 256 pieces with M random dives that allow broken edges, and write the best completion per board, sorted by connected edges. See *Finishing boards with random dives*. |
| `--emit_score S` | With `--end_dive`: the connected edges (of 480) a dived board needs to be written. Default 450. |
| `--end_polish R` | With `--end_dive`: hill-climb each board's 16 best dives, then R kick-and-polish rounds. The largest single gain measured. See *Finishing boards with random dives*. |

Both phases need `--clue_center`, `--pin_clue 1..4`, a rotations file and
`--num_rows 1`. A table belongs to one seed, one clue frame and one border.

```bash
bash tests/datadriven/run_datadriven.sh                                   # learn, then search
bash tests/datadriven/run_datadriven.sh LEARN=0 \
     TABLE=tests/datadriven/example_run/table_rnd_s9_row2.txt             # search with a table
sbatch tests/datadriven/example_run/slurm_datadriven.sh                   # cluster, from the repo root

python3 tests/datadriven/freq_view.py TABLE --text                        # summary
python3 tests/datadriven/freq_view.py TABLE --out table.html              # HTML report
```

### Learning

**Defaults.** `--learn` changes two defaults, never a value you pass:
`--lambda_J 0 --lambda_Mahalanobis 0`. The table is meant to measure where
pieces land, so boards are grown on the chain database and the fan-out alone,
without the colour heuristics steering them. The `[cfg]` line marks each value
that came from these defaults, and the table records the objective it was
learned under as a `learn_objective lambda_J .. lambda_Mahalanobis ..` line.
`--stop_row` defaults to 11 here as everywhere else; `run_datadriven.sh` passes
its own `LEARN_STOP_ROW` (9), where the stop row is still reached often.

**Four passes.** Pass *j* turns the rotations row *j* quarter-turns clockwise and
pins the clue frame to match. That is the same anchored puzzle, relabelled.
Every board reaching `--stop_row S` is folded back to the canonical frame:
- Pass 0 covers canonical rows 1..S.
- Pass 2 covers rows 15−S..14.
- Passes 1 and 3 cover the columns.

So at `S ≥ 7` every free cell is measured, and the top rows are measured by
pass 2's first rows. Passes whose bottom pool is smaller than `--top_bottoms` go
round it again with fresh random streams, so every pass takes the same number of
samples.

**One configuration, one vote.** A configuration contributes up to 10 000 of its
distinct stop-row boards. When it has more, they are drawn uniformly without
replacement, by a key hashed from each board's frontier rather than by pool
order. Their counts are divided by their number, so each
configuration adds weight 1 to every cell it filled. The effective sample size
is reported in configurations. Treat it as an upper bound: configurations
sharing a bottom row are correlated. That is why `--top_bottoms` buys more
independent evidence than `--top_columns`.

The table stores raw counts together with its provenance: seed hash, clue mask
and pin, and a hash of the border.

**Units: nats.** Every weight and score here is a natural logarithm, measured in
*nats*. A difference of 1 nat is a factor of $e \approx 2.72$ in probability
(1 nat = 1.44 bits). So a weight of +1.0 says the table rates a placement 2.7×
more likely than chance, −1.0 says 2.7× less likely, and 0 says it has no
opinion. Log-scores add, which is why a board's learned score is a sum over its
cells. They are in the same units as the fan-out lookahead, $\log$(number of
continuations), which is why the two can be summed.

**Lift.** The learning summary, and the table's `lift` line, report a
*prequential lift*. Each configuration's boards are scored, before its own
counts are added, against the table built from the configurations before it.
The score is the average over placed cells of $\log(K\,\hat P(\text{true piece}\mid\text{cell}))$,
with $\hat P$ smoothed as in step 1 below, and pooled over the A/B/C segment for
inner cells. $\log K$ is the most a cell can score, for a table that always
names the right piece: $\ln 191 \approx 5.25$ nats for inner cells. So
"segment lift +0.24 nats/cell" means the table gives the piece that actually
landed in a cell, on average, $e^{0.24} \approx 1.27$ times its chance
probability $1/K$. That is measured on configurations it had not yet seen, and
at segment resolution. Zero or below means the table predicts nothing; the
summary flags it. No cell-resolution lift is reported, so this figure does not
rank the two `--freq_model` modes.

### The estimator

Let $N_{px}$ be the configuration-weighted count of piece $p$ in free cell $x$,
and $W_x = \sum_p N_{px}$. Inner pieces and cells form one square block of
$K = 191$; each border side is its own $14 \times 14$ block.

1. **Smoothing.** Krichevsky–Trofimov:
   $\hat P(p\mid x) = (N_{px} + \tfrac12)/(W_x + \tfrac K2)$.
   It keeps every log finite and has no free parameter.
   - In `segment` mode, the counts of the cells of one A/B/C bin $z$ are summed
     first, then divided by the bin's free-slot count $|z|$ and smoothed per slot.
   - In `cell` mode, each cell is smoothed on its own.
2. **Balance.** Sinkhorn scales $\hat P$ to a doubly stochastic $q$. A board row
   is a permutation: without balance, a beam maximising $\sum \log \hat P$ spends
   globally popular pieces on the low rows it commits first. Balancing also
   removes the coverage bias between cells seen by different numbers of passes.
3. **Future conditioning (beam).** When row $r$ is committed, only rows
   $r..14$ remain. With $C_r$ the free slots in those rows and
   $R_p(r) = \sum_{x \in \text{rows } r..14} q_{px}$,

   $$w_r(p,x) = \log \frac{q_{px}\,C_r}{R_p(r)}$$

   This is the log-lift of placing $p$ at $x$ over a uniform remaining slot,
   given that $p$ is still unplaced. A top-loving piece spent low is penalised
   when it is spent. At $r=1$ it reduces to $\log(K q_{px})$.
   - Vertical-edge pieces use the same form within their side.
   - Horizontal edges carry no beam weight: the bottom row is fixed by the
     configuration, and the top row is not grown.
4. **Location (border ranking).** $\log(K q_{px})$ on exact cells. Bottom rows and
   left columns are ranked by the library's fan-out measure plus the sum of these
   weights over their cells. `--tau_bottoms`/`--tau_columns` keep their meaning.

**Beam score** = stock row-local score (fan-out lookahead + colour terms) +
$\sum$ of $w$ over every committed row. The learned sum is carried forward in
`BeamEntry.score`; the stock terms are recomputed per row as in the stock
beamer. At the stop row the fan-out term is dropped, as in the stock beamer.

`freq_view.py` implements the estimator independently. `--check` compares all
three weight sets against the binary's own:

```bash
E555_FREQ_DUMP=fw.txt bin/E555_beamer_datadriven SEED ROT --table T ...   # dumps, then continues
python3 freq_view.py T --check fw.txt                                     # AGREE / DISAGREE
```

### Choosing `--freq_model`

Both modes use one table and one estimator. Only the resolution at which inner
pieces are scored in the beam differs, so you choose at search time and never
re-learn.

- **`segment`** (default). The counts of the cells in one A/B/C bin of a row
  (cols 1–5, 6–10, 11–14) are summed, then shared equally over its slots. The
  table says "piece $p$ belongs in segment B of row 12", not in which column.
- **`cell`**. Each cell keeps its own counts. The table says "piece $p$ belongs
  at row 12, col 8".

| | `segment` | `cell` |
|---|---|---|
| evidence per estimate | about 5× more (pooled over the bin) | one cell's counts |
| noise on a thin table | low | high: ten configurations can put up to +3 nats on one pair |
| position inside a segment | lost | kept |
| near clues and corners | blurred: one sharp cell is averaged with four neutral ones | sharp |
| strength against the fan-out term | gentler | stronger (weights about 2.4× wider on the example table) |
| matches the beam's move | yes: the beam places a 5-piece chain at a time | finer than the move |

The example table shows both effects. Its segment beam weights span −4.4 to
+3.4 nats with an SD of 0.62, and its cell weights −6.3 to +4.5 with an SD of
1.52. Its strongest single-cell preferences, +3.8 to +4.6 nats, sit next to the
corners and clues (e.g. piece 73 at row 14 col 1). Segment pooling dilutes
exactly those.

**Which to use.** Decide by how much evidence the table has per piece-cell pair.
The `[freq] table …` line at startup prints `mean_ess`, in configurations per
cell; divide it by $K = 191$.

- **ESS/K of about 10 or more** (mean ESS ≳ 2 000): start with `cell`. The
  half-count prior is then at most 5 % of the data, so cell estimates are not
  dominated by noise, and the extra positional detail is real. The example table
  has ESS 4 374–10 510 per cell (mean 6 595, ESS/K ≈ 35), so `cell` is the first
  thing to try with it.
- **ESS/K of a few or less** (mean ESS of hundreds or fewer, e.g. a short or
  wall-limited learn): use `segment`. Cell estimates would be mostly prior and
  noise, and pooling is what makes such a table usable.
- **When unsure**, keep `segment`. It is the conservative choice and never
  worse on noise.

**Measuring it.** Run the search twice on the same table, border, `--rng_seed`,
`--threads` and budget, changing only `FREQ_MODEL`. Search with a table is
deterministic for fixed seed and threads, so every difference comes from the
model. Compare:
- the configurations reaching rows 11 and 12 (`[sum] extinctions by row`);
- the completions and two-segment partials at `--stop_row 12`.

If `cell` loses, look at a short `--verbose` run of each. The `[score] r… SD
fan=… table_acc=…` lines give the spread of the fan-out term and of the learned
sum per row. When `table_acc` grows far larger than `fan` by rows 9–12, the
table is overriding the lookahead that keeps rows continuable. A sharper table
makes that more likely.

## Search behaviour that differs from the stock beamer

- **Row-1 corner-clue prefilter.** With `--clue_corners`, each (bottom, column)
  pair is tested exactly for a row 1 that satisfies both lower corner-clue pins.
  The test uses the same database cells and piece masks as the beam, with no
  quotas. Columns that fail it are never run, so `--top_columns` counts only
  columns that can satisfy row 1's clue pins.
- **Stop row.** Complete stop-row boards skip frontier deduplication. They are
  emitted best-first with exact-board deduplication only, so distinct boards
  sharing a frontier (for example, differing by one top piece) are all kept.
- **`--incomplete_top`** writes stop-row boards that fill only part of the stop row:
  - two segments (A+B, A+C, B+C) go to `beam_completions_<row>_<stop>_partial.csv`;
  - segment B alone goes to `..._partial_B.csv`. B alone is useful with the top
    clues at `--stop_row 12`, which touch segments A and C of row 12 from above.

  A partial is dropped when an earlier one of the same kind has the same set of
  pieces below the stop row, and its stop-row pieces differ in at most one cell.
  Rotations are ignored. Completions and two-segment partials reset
  `--bail_columns`; B-only partials do not. `--max_emitted` counts everything
  written.
- **Emitted boards carry the fixed frame.** With a rotations file, each
  configuration fixes the whole left column (BL to TL) and the TR corner before
  the first row, and the beam never places those 17 pieces anywhere else. Every
  emitted board, completion or partial, writes them into the cells the search left
  empty: column 0 above the stop row, and cell 255. Cells the search placed are
  unchanged. The column is one matched trail, so a board gains matched edges (six
  at `--stop_row 9`) and never a break. Stage C tools treat these pieces as fixed;
  free them with a holes file. Not under `--random_edges`.
- **The rotations row is echoed.** At the start of each border row the log prints
  the row exactly as it appears in the file (unturned), after the comment line
  above it (the Stage A `TOP= RIGHT= BOTTOM= LEFT= Score=` line) when there is
  one. `--learn` writes the same two lines into the table as `rotations_comment`
  and `rotations_row`, so a lost rotations file can be rebuilt from either:
  `grep -E '^rotations_(comment|row) ' TABLE | cut -d' ' -f2-`. They are a record
  only; the border check uses the hash.
- `--verbose` adds per-row standard deviations and correlations of the score
  components.

## Corner supply (`--lambda_corners`)

A search-phase option that works with or without `--table`. With `--clue_corners`
the two top clues sit on row 13, at columns 2 and 13. Each closes a 2x3 block with
its corner:

```
row 15   corner  w1    w2              w2    w1    corner
row 14   side    in_a  in_b    ...     in_b  in_a  side
row 13   side    in_c  CLUE    ...     CLUE  in_c  side
         col 0   1     2               13    14    15
```

The beam stops at row 12 or lower, so these blocks are Stage C's. Nothing in the
stock score or the learned table protects their pieces, and a piece-by-cell
statistic can't say "these three together". Measured on the reference run
(`example_run/beam_completions_2_11.csv`): of 282 boards at row 11, **none** can
still build a legal block at both top corners from its unused pieces.

**Without `--clue_corners`** the block is just the 3 cells next to each corner:
the side piece at (0,14), the inner piece at (1,14) and a top-border witness at
(1,15), mirrored for TR. These blocks number 10–93 per corner and usually
survive, so the term is a milder nudge there.

**The catalog is exact.** It is the shared `tc_*` code in
`src/B_beam/E555_database.c`, the same one the stock beamer and finalizer use.
For one rotations row the corner piece, the clue and the pieces on each side are
all fixed. So every legal filling of the two side
cells, the three inner cells and the two top-border cells `w1`, `w2` is
enumerated at the start of the border row, which takes milliseconds:

```
[corner] border row 4: TL 7 block(s) on 3 of 9 left pairs | TR 33 block(s) on 12 of 13 right pairs
```

A block is the five pieces the beam could spend: the two side pieces and the
three inner pieces, with their spins. `w1` and `w2` are only witnesses that the
top border can meet the block. Across 48 Stage A borders this is 0–55 TL blocks
and 1–75 TR blocks, so no learning and no truncation are needed.

Some borders have **no** legal TL block at all: rows 1 and 3 of
`borders_stageAx6.csv`, rows 6 and 8 of `data/borders_annealed_fix12.csv`, and
row 20 of the MaxSides file. No board from those borders can place the (13,2)
clue legally, so the whole border row is skipped.

**Left columns.** A column fixes (0,14) and (0,13). A column whose pair no TL
block uses can never close the corner, so it is never run. On r16178 that is
two columns in three. The count is in the summary:

```
[sum] corner filter: columns never run 4800, inputs skipped 0
```

**The score.** A block is *alive* while none of its pieces is on the board. At
`--stop_row 12` it must also meet row 12's exposed tops, because the row-13 clues
pin nothing and row 12 is built blind to them. TL keeps only the blocks on its
column's pair, and tests their three inner pieces. TR keeps every block, and
tests all five pieces, because the right column is chosen row by row. With `n`
alive per corner, capped at 3:

```
step(n) = -3, +1, +2, +3        for n = 0, 1, 2, 3+
term    = lambda_corners * u_row * (step(n_TL) + step(n_TR))      in [-6, +6] * lambda * u_row
```

`u_row` is the standard deviation of the rest of the child's score, measured
on the previous row. So λ is in score-SD units, like `--lambda_Mahalanobis`,
and means the same thing at every depth. At λ = 0.5 and the default
`--pool_factor 8`:
- about the top 12% of children survive (z ≈ +1.15);
- killing a corner's last block costs 2 SD, which is decisive;
- going from 3 to 2 blocks costs 0.5 SD, which is a nudge;
- a lineage dead at both corners loses 3 SD and is purged whenever healthy
  lineages exist.

The term depends only on the child's used set and frontier, which the dedup
signature already covers.

**Flag forms.** Absent → off, with outputs byte-identical to before. A bare
`--lambda_corners` → 0.5. `--lambda_corners F` → F. It needs a rotations file
(not `--random_edges` or `--free_edges`) and `--stop_row 12` or below. With
`--clue_corners` it uses the clue catalog of the frame being searched; this fork
searches one frame per pass, so that is `--pin_clue`'s, or frame 0 without it. It is refused under `--learn`, because steering the
learning would bias the table the search relies on, and in the turned passes
the frame's top corners are not the canonical ones.

**What it reports.**
- The `[sum]` block gives, over emitted stop-row boards, the histogram of alive
  blocks per corner, how many boards have both corners alive, and how many have
  a *piece-disjoint* TL+TR pair. The two corners are scored separately, but
  about a third of TL×TR block pairs share a piece or a top-border witness, so
  the joint number is the one that says both corners can really be closed.
- The block also prints `u_row` by row. This fork calibrates it per
  configuration (the previous row of the same configuration first, then run-pooled
  values), not with the shared run-level table.
- `--verbose` adds, per row, the corner term's SD, its mean, and its correlation
  with fan-out and closure.

**When it bites.** Only where the beam discards children: rows where the pool
exceeds the width, and the best-of-nB×nC window inside a parent. Once the beam
collapses and keeps every child, the term only orders the emitted boards. So it
does most at wide beams.

## Backtracking to the stop row (`--backtrack_row N`)

The beam's own collapse is not the limit it looks like. Past row 6 or so the
beam keeps a small fraction of the legal children, yet an exhaustive search over
the same boards is cheap, because the tree dies out within a few rows.
`--backtrack_row N` splits the work at row N:

1. **Rows 1..N-1** are the ordinary beam, with every heuristic, the table and the
   corner term.
2. **Row N** is expanded as a stop row: every conflict-free completion the
   per-parent quota allows, ranked raw with no frontier dedup. These candidates are
   the roots, exactly the boards `--stop_row N` would emit.
3. **Rows N+1..`--stop_row`** are searched exhaustively for every root, in the
   beam's rank order. The search goes cell by cell in row-major order:
   - column 0 is the configuration's fixed left column;
   - columns 1-14 take every unused inner piece orientation matching the left
     and bottom colours;
   - column 15 takes every unused right edge of the border's own terminal pool
     (all edges under `--free_edges`).

Nothing past row N is scored or selected. A path ends only when:

- a cell has no fitting piece;
- a completed row fails the beam's colour-parity test (`parity_ok`).

Nothing else is enforced. The row-13 corner clues and the top-corner blocks
(`--lambda_corners`) never end a path, so any board that reaches row 12 is
emitted; `--lambda_corners` only adds its tally to the report. Clue pins the
search passes through (the centre clue, when N is below its row) are enforced
as in the beam: the clue piece on its cell, and the colour it will sit on in
the row below. **Every** board that completes the stop row is
emitted, best root first. Only exact duplicate boards are dropped; there is no
per-configuration cap.

```bash
bin/E555_beamer_datadriven SEED ROT --clue_center --pin_clue 1 --start_row 4 --num_rows 1 \
    --beam_width 50000 --backtrack_row 5 --stop_row 11 --max_emitted 100000
```

- **Output volume.** The number of emitted boards can be very large when the
  stop row is close to N: 2,048 row-5 roots gave 123,355 row-8 boards. Use
  `--max_emitted`. It is checked after each tile of 32 × threads roots, so the
  final count can overshoot by one tile.
- **Measured** on border r16178, clued (`--pin_clue 1`), width 50,000, 4 threads,
  6 configurations:
  - The plain beam died at row 11 in all 6 (3.3 s each).
  - `--backtrack_row 5` searched about 2.17 million roots at about 110 M nodes/s,
    about 17 s per configuration.
  - It emitted 104 row-11 boards, from every configuration.
- **Log.** `--verbose` adds a `[dfs]` line per configuration: roots, roots that
  emitted, nodes, parity cuts, and boards completing each row. The run
  summary always carries the totals.
- **Stopping.** `--time_limit` and Ctrl-C stop the search mid-root. Boards found
  so far are still written, and the `[sweep]` reason says `time` or `interrupted`.
- **Deterministic.** Roots are searched in parallel a tile at a time and written in
  root order, so the output does not depend on the thread count.
- **Rejected with `--learn`.** Learning counts a beam's stop-row boards, not an
  exhaustive search's.
- **`--incomplete_top` is ignored, with a warning.** Only complete stop-row boards
  are emitted.
- **Clue frames.** Search mode runs a single clue frame per pass, so every root
  already belongs to one orientation.

## Finishing boards with random dives (`--end_dive M`)

A stop-row board covers rows 0..stop_row only, so its real quality is unknown
until the top is filled. With `--end_dive M`, each stop-row board is finished
to all 256 pieces, allowing broken edges, and the finished boards are what the
run writes. It works with or without `--backtrack_row`.

**The dive** is the stuck mode of `src/C_tail/E555_backtracker.c`
(`--break_mode stuck`), ported into the fork:

- Next cell: the one with the fewest exact fits.
- Piece: an exact fit where there is one, otherwise a placement from the
  minimal-break class, taken only when every open cell is stuck.
- Within a class: least-constraining value (fewest broken edges, then fewest
  stranded cells, then most room left around it). Ties go at random.
- A dive never backtracks and cannot fail, so it is cheap and every dive is a
  different board.

The dive starts from the board exactly as the stop row would have written it:
rows 0..stop_row, the whole left column, the top-right corner and any attached
corner clues. None of it moves. In rotations mode an edge piece stays on the
side its rotations-row spin gives it; under `--free_edges` or
`--random_edges` any edge piece may take any border cell. The score is
connected edges out of 480.

**Two stages per configuration** (one bottom × left column):

1. Every distinct stop-row board gets `M/10` dives and keeps its best.
2. A board goes on if its stage-1 best is at least `S-4`, **or** at or above the
   configuration's median stage-1 best. It gets the other `M - M/10` dives, and
   these **learn from the board's own best dives** (below).

**Polish** (`--end_polish R`, off by default). Every board whose best dive is
within 6 of `S` is then improved by local search over the cells the dives
filled:

- its 16 best distinct dives are each hill-climbed: the best re-rotation of one
  piece or swap of two (with their best frame-legal spins), repeated until no
  move adds an edge;
- then `R` kick-and-polish rounds, split over 8 walks from the best polished
  boards: 3 random swaps, re-polish (only cells whose surroundings changed are
  re-examined), keep the result if it is not worse;
- `R = 0` polishes only.

Boards whose best is at least `S` (`--emit_score`, default 450) are kept in
memory. When the completions file closes (the end of a border row, or the end
of the run) they are written to it:

- sorted by score, best first (ties in the order the boards were found);
- exact duplicate boards dropped;
- at most `--max_emitted` dived boards in the whole run. The budget caps the
  written boards and no longer stops the search; `--incomplete_top` partials
  do not count against it;
- each line is `config_id, score, pos[256], rot[256]`, with connected edges in
  the score field, so rank, the viewer and the finalizer read it as it is. The
  stop-row board is still inside it, untouched.

```bash
bin/E555_beamer_datadriven SEED ROT --clue_center --clue_corners --pin_clue 1 \
    --start_row 4 --num_rows 1 --beam_width 250000 --backtrack_row 5 --stop_row 11 \
    --end_dive 10000 --end_polish 20000 --emit_score 452
```

**Learning dives.** The dive never overrides its safety order (fewest breaks,
fewest stranded cells). A weight `w(p,x)` per piece and open cell acts only
where the plain dive draws at random or ranks by room:

- **Cell.** Among cells tied for fewest exact fits, the one whose best fit has
  the largest `β·w` plus Gumbel noise goes first.
- **Piece.** The least-constraining value's third key becomes
  `log1p(room) + β·(w + Gumbel)`. A forced break with more than 8 candidates
  scores the 8 that a Gumbel-top-k draw on `β·w` picks.
- **Stage 2** runs 18 rounds of the cross-entropy method at `β = 2`, with `w`
  starting flat (0). After each round the top 5% of its dives vote on
  (piece, cell) placements, and `w += 0.3·log((votes + ½)/(expected + ½))`,
  with the vote and the total shift both clipped to ±2 so that nothing becomes
  certain. Each board learns what its own best completions agree on.
- **Stage 1** is the plain dive, or, with `--table --freq_model cell`, dives
  at `β = 0.5` and `1` on the table prior
  `w(p,x) = log(q[p,x] × n_open / Σ q[p,y])` over the board's open cells (`q`
  the Sinkhorn-balanced location mass; the beam's remaining-opportunity
  normalisation applied to the dive's region).

`E555_DIVE_PLAIN=1` turns the learning off (plain dives throughout).

### Measured: what each part buys

97 stop-row-11 boards (clued, `--pin_clue 1`, the example table, border r16178,
width 100,000, `--backtrack_row 5`, 40 bottoms × 15 columns), each dived with
every board taking all `M` dives (`--emit_score 0`), 4 threads. The same boards
were replayed for every setting, so differences are paired per board.

| setting | mean best | top 10 | best | boards ≥ 454 | time |
|---|---|---|---|---|---|
| plain dives, M = 50k | 449.34 | 452.8 | 454 | 2 | 188 s |
| first version (table prior, 4 β arms, 9 rounds), M = 50k | 450.36 | 453.8 | 455 | 6 | 202 s |
| learning from a flat start, 18 rounds, β 2 (**new default**), M = 50k | 450.98 | 454.3 | 457 | 8 | 206 s |
| + `--end_polish 0` (polish only) | 451.89 | 455.2 | 457 | 15 | 212 s |
| M = 10k + `--end_polish 5000` | 453.33 | 456.0 | 457 | 42 | 83 s |
| **M = 10k + `--end_polish 20000`** | **453.76** | **456.3** | **458** | **50** | **203 s** |
| M = 10k + `--end_polish 50000` | 453.79 | 456.5 | 458 | 54 | 434 s |

- **The learning is worth more than 10× the dives.** Plain dives gain about
  0.7-0.9 edges per 10× more dives (1k 447.4, 10k 448.7, 100k 449.6, extrapolated
  500k 449.9). The learning rounds reach 451.0 at 50k.
- **Without a table the learning helps just as much:** +1.52 ± 0.12 edges per
  board over plain dives (better on 78 of 97, worse on 2).
- **The table prior helps plain dives but hurts the learning.** Stage-1 dives on
  the prior gain about 0.2-0.5 edges over plain ones. Starting the stage-2
  rounds from it instead of flat loses 0.27 ± 0.11. So the prior now steers
  stage 1 only.
- **Polish and kicks are the biggest single gain:** about +2.8 edges per board
  at equal time. They never lower a board, and their extra over the dives alone
  was +2 to +4 edges on 90 of 97 boards (at most +5).
- **Flat parameters** (paired against the old default, ±0.06-0.07): learning rate
  0.15/0.6, elite 2%/10%, clip 1/4 and LCV cap 16 changed nothing within about
  0.2. 5 rounds lost 0.27; LCV cap 4 lost 0.20. Kicks of 1 swap lost 0.8; 3
  beat 2 by 0.2; 4 matched 3 but slower. 8 walks ≈ 4 walks.

### How many dives, stage 1 and who goes to stage 2

- **M.** With `--end_polish`, dives beyond about 10k barely pay: with 5k kick
  rounds (4 walks), M = 2k, 5k, 10k, 20k, 50k gave 453.17, 453.18, 453.33,
  453.37, 453.38, in 50, 62, 83, 124 and 242 s. Spend the time on kick rounds
  instead. Without polish, M = 50k is reasonable: 20k gives 0.55 less, 100k 0.17
  more, 200k 0.40 more.
- **Speed** at stop row 11 (about 57 open cells): about 6,000 learning dives/s
  per thread; a kick-and-polish round costs about as much as 2-3 dives.
- **Stage-1 length.** With every board taking stage 2, stage 1 at 2%, 5%, 10%,
  20%, 30% of M ended at 450.95-450.99: the length does not matter for the
  boards that go on. 10% stays.
- **Who goes to stage 2.** The learning adds 1-4 edges to a board's stage-1 best
  (5-7 on about 3%), so the first rule's `S-2` was too tight. Offline, on the
  traces of every board's stage-1 and final best, a policy either lets a board
  reach `S` or loses it:

  | rule | S = 452: lost (of 36) | dives spent | S = 454: lost (of 8) | dives spent |
  |---|---|---|---|---|
  | median or `S-2` (first version) | 3 | 88% | 0 | 88% |
  | `S-2` only | 8 | 42% | 2 | 16% |
  | **median or `S-4` (new default)** | **0** | 93% | **0** | 88% |
  | `S-4` only | 2 | 76% | 0 | 42% |

  With few boards per configuration (typical at row 11) the median admits 85-92%
  of them, so stage 2 saves little. When `S` is high for the boards (454 here),
  `E555_DIVE_MEDIAN=0` (`S-4` alone) cut the dives to 42% with nothing lost; at
  452 it lost 2 of 36.

### Measured: complete runs

Border r16178, clued (`--clue_center --clue_corners --pin_clue 1`), width
250,000, `--backtrack_row 5 --stop_row 11`, 5 bottoms × 15 columns,
`--end_dive 10000 --end_polish 20000 --emit_score 450`, 4 threads:

| run | boards reaching row 11 | written (≥ 450) | best | ≥ 455 | dive + polish time |
|---|---|---|---|---|---|
| (a) `--table`, `--freq_model cell` | 20 | 20 | 456 | 9 | 33 s |
| (b) `--table`, `--freq_model segment` | 19 | 19 | 456 | 5 | 29 s |
| (c) no table | 6 | 6 | 454 | 0 | 8 s |
| (d) `--random_edges --samples 5 --top_columns 1`, `--end_dive 2000 --end_polish 2000 --emit_score 452` | 2,748 (1 of 5 borders) | 1,898 (≥ 452) | 458 | 495 | 445 s |

The beam and exhaustive search took about 2-3 minutes per surviving
configuration; most columns die at the corner-clue row. Random-edge borders let
any edge piece take any border cell, and one such border sent 2,748 boards
through row 11 (the rotations border above: 6-20 over 75 configurations). At
the (a)-(c) settings that border alone would take about 2 hours of dives, hence
the smaller settings for (d). `tools/E555_viewer.py` confirms the best boards
at 458/480 (221 solid pieces) and 456/480.

- **Log.** `--verbose` adds a `[dive]` line per configuration: roots, stage-1
  best and median, roots in stage 2, best, boards kept, dives, time, and the
  arm means when guided. The run summary always carries the totals and the
  scores written.
- **Time.** `--time_limit` bounds the beam and the exhaustive search; the dives
  of that configuration then run in full. `--wall_time` and Ctrl-C stop the
  dives, and every board that already has a score is still written.
- **Crash.** Kept boards live in memory until the file closes, so a killed run
  (SIGKILL) loses the current border row's dived boards, and `--resume` will
  not redo those configurations.
- **Deterministic.** Each dive's random stream is keyed by its board and its
  index, so the output does not depend on the thread count.
- **Not with `--learn`.** `--incomplete_top` partials are still written, not
  dived, to their own files.

## Border check and environment variables

`--table` compares the table's border with the one being searched by content, not
by file name: the table's `border` hash, or for tables without one, the side it
records for each edge piece. A different border is fatal.

| variable | effect |
|---|---|
| `E555_FREQ_ANY_BORDER=1` | Accept a table from another border. The interior table steers; the border block is dropped. |
| `E555_FREQ_NOBORDER=1` | Drop the border block. The bottom row and left column are ranked by the library alone. |
| `E555_FREQ_PURE=1` | Rank the beam by the learned sum alone. |
| `E555_FREQ_DEBUG=1` | With `PURE`: assert that the carried sum equals a from-scratch sum over the placed cells. |
| `E555_FREQ_DUMP=PATH` | Write the processed weights (`fw`, `fwseg`, `fwcell`, `fwb`) for `freq_view.py --check`. |
| `E555_DIVE_PLAIN=1` | With `--end_dive`: plain dives, no learning. For measuring what the learning buys. |
| `E555_DIVE_ROOTS=FILE` | With `--end_dive`: skip the beam and dive the saved stop-row boards of the border row from `FILE` (any completions CSV). Measures dive settings on identical boards, or re-dives an earlier run. Not with `--random_edges`. |
| `E555_DIVE_TRACE=FILE` | With `--end_dive`: one line per board and stage segment with the histogram of its dive scores. |
| `E555_DIVE_*` tuning | `STAGE1` (share of M, 0.1), `ROUNDS` (18), `S2BETA` (2), `BETAS` (stage-1 arms), `NOPRIOR` (1 flat, 2 prior in stage 1 only), `GAMMA` (0.3), `ELITE` (0.05), `CLIP` (2), `LCV` (8), `MARGIN` (4), `MEDIAN` (1), `POLISH` (dives polished), `ILS` (kick rounds), `KICK` (3), `STARTS` (8), `ILS_FULL` (full rescan after a kick). Defaults are the measured ones. |

## example_run/

A table and the boards found with it, kept as a reference and a test input.

| file | content |
|---|---|
| `table_rnd_s9_row2.txt` | Table learned on border **r16178**, which is row 4 of `borders_stageAx6.csv`. Its header names the file it was run from (`borders_stageAx6_best.csv`, row 2); the border check matches it by content. Learning used `--stop_row 9 --beam_width 250000 --top_bottoms 10000 --top_columns 10`, 24 threads, and the randomised settings of the slurm script. It covers 10 510 configurations, and its segment lift is +0.24 nats/cell. The slurm script passed `--lambda_J 0 --lambda_Mahalanobis 0`, so the table matches today's learn defaults; it predates the `learn_objective` line. |
| `freq_view_table_rnd_s9_row2.txt` | `freq_view.py --text` of the table. |
| `beam_completions_2_11.csv` | 282 boards to `--stop_row 11` found with the table, from 174 configurations. All are edge-legal with 356 matched edges. Written before emitted boards carried the frame, so rows 12 to 15 of the left column and the TR corner are empty. |
| `slurm_datadriven.sh` | The learn-then-search job. Its defaults match the settings recorded in the table header. |
| `E555_annealer_MaxSides.csv` | 30 Stage A borders maximising all four sides (100k restarts, 750k steps), sorted by top. |

To search with the table:

```bash
bash tests/datadriven/run_datadriven.sh LEARN=0 \
     TABLE=tests/datadriven/example_run/table_rnd_s9_row2.txt   # ROTATIONS/BORDER_ROW default to row 4
```

## borders_stageAx6.csv

Six Stage A rows, annealed with all four weights at +1 and `--target_scale 0`. The
header counts are the pass pools:
- pass 0 draws bottoms from BOTTOM;
- pass 1 from RIGHT;
- pass 2 from TOP;
- pass 3 from LEFT.

| row | id | TOP | RIGHT | BOTTOM | LEFT | note |
|---|---|---|---|---|---|---|
| 0 | r27182 | 483840 | 2880 | 5760 | 11232 | very flexible top |
| 1 | r7067 | 103680 | 17280 | 77760 | 14400 | loosest row |
| 2 | r5788 | 57600 | 120960 | 3744 | 2304 | |
| 3 | r14839 | 4320 | 77760 | 3744 | 69120 | tight opposite sides (top and bottom) |
| 4 | r16178 | 181440 | 4320 | 34560 | 2880 | the `example_run/` border |
| 5 | r11937 | 60480 | 25920 | 32 | 1152 | tightest: 32 bottoms |

## Files

| file | role |
|---|---|
| `E555_beamer_datadriven.c` | the fork: learning, estimator, guided search |
| `Makefile` | builds `bin/E555_beamer_datadriven`, linking `../../src/B_beam/E555_database.c` |
| `freq_view.py` | HTML/text report on a table; `--check` against the binary |
| `run_datadriven.sh` | both phases in order; `LEARN=0` reuses a table |
| `borders_stageAx6.csv` | six-row rotations file, above |
| `example_run/` | reference table, boards and slurm job |
| `runs/` | scratch output (gitignored) |
