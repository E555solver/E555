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
| `--freq_model M` | The beam's spatial resolution. `segment` (default) pools the 5-5-5 A/B/C bins; `cell` uses exact cells. |

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
and pin, and a hash of the border. The learning summary reports a
**prequential lift**: each configuration is scored against the table built from
the configurations before it, in nats per cell above chance.

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
- `--verbose` adds per-row standard deviations and correlations of the score
  components.

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
| `table_rnd_s9_row2.txt` | Table learned on border **r16178**, which is row 4 of `borders_stageAx6.csv`. Its header names the file it was run from (`borders_stageAx6_best.csv`, row 2); the border check matches it by content. Learning used `--stop_row 9 --beam_width 250000 --top_bottoms 10000 --top_columns 10`, 24 threads, and the randomised settings of the slurm script. It covers 10 510 configurations, and its segment lift is +0.24 nats/cell. |
| `freq_view_table_rnd_s9_row2.txt` | `freq_view.py --text` of the table. |
| `beam_completions_2_11.csv` | 282 boards to `--stop_row 11` found with the table, from 174 configurations. All are edge-legal with 356 matched edges. |
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
