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

**Defaults.** `--learn` changes three defaults, never a value you pass:
`--lambda_J 0 --lambda_Mahalanobis 0 --stop_row 9`. The table is meant to
measure where pieces land, so boards are grown on the chain database and the
fan-out alone, without the colour heuristics steering them. Row 9 is still
reached often, and learning writes no boards, so the larger number of stop-row
boards costs no disk. The `[cfg]` line marks each value that came from these
defaults, and the table records the objective it was learned under as a
`learn_objective lambda_J .. lambda_Mahalanobis ..` line.

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

**The catalog is exact.** For one rotations row the corner piece, the clue and
the pieces on each side are all fixed. So every legal filling of the two side
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
[sum] corner prefilter: clue-compatible columns whose (0,14),(0,13) pair no TL block can use, never run: 4800
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
`--lambda_corners` → 0.5. `--lambda_corners F` → F. It needs `--clue_corners`,
`--pin_clue 1..4`, a rotations file (not `--random_edges` or `--free_edges`) and
`--stop_row 12` or below. It is refused under `--learn`, because steering the
learning would bias the table the search relies on, and in the turned passes
the frame's top corners are not the canonical ones.

**What it reports.**
- The `[sum]` block gives, over emitted stop-row boards, the histogram of alive
  blocks per corner, how many boards have both corners alive, and how many have
  a *piece-disjoint* TL+TR pair. The two corners are scored separately, but
  about a third of TL×TR block pairs share a piece or a top-border witness, so
  the joint number is the one that says both corners can really be closed.
- The block also prints `u_row` by row.
- `--verbose` adds, per row, the corner term's SD, its mean, and its correlation
  with fan-out and closure.

**When it bites.** Only where the beam discards children: rows where the pool
exceeds the width, and the best-of-nB×nC window inside a parent. Once the beam
collapses and keeps every child, the term only orders the emitted boards. So it
does most at wide beams.

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
