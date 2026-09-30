# E555 examples

One script per tool, numbered in the order you would meet them. Each runs with
no arguments and keeps its settings in a plain block at the top. For long
unattended runs (the full pipeline, the whirlpool, the board farm) see
[`../pipeline/`](../pipeline/).

## Setup

```bash
make ARCH=generic        # all binaries; plain `make` tunes for the build machine
pip install ortools      # only for the 04 scripts (topper, ender)
```

The beamer (01, 07) builds a 6.4 GB chain database in memory, so it needs about
8 GB of RAM. Set `DB_FILE` to cache the database on disk; later runs then map it
in seconds.

## How the scripts work

- **Settings** are plain assignments at the top: edit them, or override any of
  them on the command line, e.g. `bash examples/02_finalizer_regrow.sh FROM=6
  THREADS=16`.
- **Paths** in the settings block are relative to `REPO`, which defaults to the
  checkout the script sits in. Set `REPO` when you run a copy of a script
  elsewhere.
- **Outputs.** Every tool writes `outputs.txt` next to its output, listing the
  files it filled.
- **The command.** Every call passes `--print_cmd`, so the log's `[cmd]` line is
  the exact command, reusable without the script.
- **07** is the exception: literal commands, and output in the current directory.

## The scripts

| script | tool | what it does | in → out | needs |
|---|---|---|---|---|
| `01_beamer_quickstart.sh` | beamer (+ annealer) | grow boards row by row from sampled or annealed borders | seed → partial or complete boards | 8 GB RAM, ~5 min |
| `02_finalizer_regrow.sh` | finalizer | lock rows 0..FROM, re-grow everything above | partials → partials | minutes |
| `03_roundhouse_strip.sh` | roundhouse | turn the board and refill a strip, exhaustively | partial → deepest board | seconds to minutes |
| `04a_CP-SAT_top_and_end.sh` | topper + ender | scout, promote diverse boards, polish, close | boards → closed boards | OR-Tools |
| `04b_CP-SAT_ender_overnight.sh` | ender | de-duplicate a large corpus and shard it over processes | corpus → ranked boards | OR-Tools, hours |
| `04c_CP-SAT_ender_elite.sh` | ender | repeated `deep` or `superdeep` passes on a few elite boards | boards → best boards | OR-Tools, hours |
| `05_backtracker_dives.sh` | backtracker | greedy dives (triage) or exhaustive search (proof) | board (+ mask) → boards | minutes to overnight |
| `06_roundhouse_both_ways.sh` | roundhouse | two spiral chains per board, one each way | partial → boards | minutes |
| `07_barebones_chain.sh` | beamer, finalizer, roundhouse, backtracker | the whole chain in six literal calls | seed → ranked boards | 8 GB RAM, ~15 min |
| `08_distiller_quickstart.sh` | distiller (+ diver) | distil a corpus to the N boards worth the CP-SAT tail | corpus → N boards + ender command | minutes to hours |
| `09_backtracker_all_sides.sh` | backtracker | fill break-free as far as possible, from several directions | break-free partials → one board each | minutes |
| `10_diver_quickstart.sh` | diver | finish partial boards with end dives and polish | partials → complete boards | seconds to minutes |
| `11_diver_reopen.sh` | diver | improve complete boards by re-diving their damaged band, never worse | complete → complete | a minute a board, or more |

## Which script when

| goal | scripts |
|---|---|
| a first run from nothing | 01, then 10 on its partials |
| a large corpus of partials or finished boards | 08, then the ender command it writes (or 11) |
| improve finished boards | 11 (no OR-Tools) or 04c (the ender) |
| re-grow the top rows of partials | 02, 03, 06 |
| triage many partials, or prove a board dead | 05, 09 |
| see the whole chain as plain commands | 07 |

Every tool reads and writes the same board row (`config_id, score, pos[256],
rot[256]`; [`../data/README.md`](../data/README.md)), so any output feeds any
script. Between steps:

```bash
python3 tools/E555_rank.py   FILE --seed_file data/seed_Edge5.txt --top 10   # ranked, rescored
python3 tools/E555_viewer.py FILE --seed_file data/seed_Edge5.txt            # ASCII board + web URL
```

`E555_rank.py` recomputes the score from the seed and shows where the breaks
are. Field 2 of a Stage B row is a solution index, not a score, unless the run
used `--end_dive`.

---

## 01 -- beamer

Grows boards bottom-up over the database of all legal 5-piece row chains, with
a wide beam guided by the colour heuristic.

| setting | default | meaning |
|---|---|---|
| `STOP_ROW` | 10 | last row filled |
| `BEAM_WIDTH` | 50000 | boards kept per row (the tool's default is 250000) |
| `ANNEAL` | 0 | 1: run Stage A first; anneal 4 x `BORDERS` borders and keep the best quarter |
| `STEPS` | 500000 | annealing steps per restart; below 250000 the annealer often places no legal border |
| `BACKTRACK_ROW` | 0 | N: stop the beam at row N and search every row-N board exhaustively up to `STOP_ROW` |
| `END_DIVE`, `END_POLISH` | 0, -1 | finish every stop-row board with dives (and polish); field 2 is then the score |
| `DB_FILE` | none | cache the chain database here |
| `CLUES` | 0 | 1: hold the five published clue pieces |

- **Reproducibility.** `RNG_SEED` and `THREADS` together fix the run, since the
  beam's work partition follows the thread count.
- **Borders.** `tools/E555_sort_rotations.py` orders and turns a Stage A file.
  The annealer refines a chosen row with `--input FILE --row K`, and scores a
  two-row top with `--double_decker TOP`. The beamer's `--lambda_reserve` then
  keeps that row's reserve pieces for last.

## 02 -- finalizer

Keeps rows 0..`FROM` and re-grows everything above from a reduced database.

| setting | default | meaning |
|---|---|---|
| `FROM` | 7 | the lock row; lower = more rows re-searched, much slower |
| `STOP_ROW` | 12 | last row to reach |
| `REPEATS` | 3 | independent re-runs per board |
| `COLUMNS` | 0 | left columns tried per board; 0 enumerates them all |
| `MAX_WALL` | 600 | seconds for the run |

Where to put `FROM`:

- **A complete border** (the usual beamer partial): lower is better until the
  beam stops filling. The tool's default is 5; `FROM` 4 reached row 11 on 8 of
  12 sampled columns, `FROM` 6 on none.
- **An incomplete border:** the left columns are enumerated, and their number
  explodes as rows are freed. On `board_partial_row12.csv`, `FROM` 7 takes 6 s,
  `FROM` 6 takes 50 s, and `FROM` 5 exhausts 600 s.
- Raise `MAX_WALL` before lowering `FROM`. `--backtrack_row` equal to the lock
  searches everything above it exhaustively, with no beam.

## 03 -- roundhouse

Turns the board 90 degrees and grows a `WIDTH`-wide strip instead of a row. It
uses only the edge half of the database (megabytes), and a dynamic program
refutes hopeless starts before any piece is tried. Exhaustive and deterministic:
a run that ends without a complete board proves none exists for that cut,
unless it reports that a budget stopped it.

| setting | default | meaning |
|---|---|---|
| `WIDTH` | 5 | strip width 2..5; 0 = the narrowest that keeps the solved region. Pieces kept at `ROUNDS` 1/2/3: width 3 208/169/130, width 4 192/144/96, width 5 176/121/66 |
| `ROUNDS` | 1 | bands freed and refilled in turn (right, top, left for `ROTATE` 0); the cuts nest, so work upward |
| `ROTATE` | 1 | which side round 1 attacks: 0 right, 1 top, 2 left, 3 or -1 bottom |
| `TIES` | 1 | boards emitted at the deepest reach |
| `BREAKS` | 0 | B > 0: also fill the deepest board greedily with at most B mismatches |

Output is one board per input, tagged `s` (solved), `d` (deepest partial) or
`f` (filled with breaks). A cut that keeps 96 pieces or more exhausts in
seconds; `ROUNDS=3 WIDTH=5` (66 pieces kept) ends on `MAX_WALL`.

## 04 -- Stage C with CP-SAT (04a, 04b, 04c)

- **`04a`**, the funnel: the topper scouts the corpus widely, a diverse share is
  promoted, the topper polishes it, and the ender closes it. The topper's
  settings are `SIDE`, the band that opens (`T B L R` or `TR TL TB`); `WORK_ROWS`,
  its depth; and `HOLES`, a 16x16 mask instead of a band. Open only where the
  breaks are.
- **`04b`**: exact de-duplication of a large corpus, then sharding over
  several ender processes; `BOARD_TIME` seconds per board.
- **`04c`**: a few diverse elites, each given repeated passes with fresh seeds.
  `MODE=deep` (about 1 h a board) or `superdeep` (about 10 h); `ELITE_COUNT`,
  `TOTAL_SECONDS`.

The ender's two settings are `ENDER_PROFILE` (`overnight`, `deep`, `superdeep`)
and the per-board wall clock (`ENDER_BOARD_TIME`, `--board_time_limit`). It
never returns a worse board. On dived and polished boards its gains come from
the redive step, which needs `bin/E555_diver` (`make diver`). Re-score what the
topper writes: its live `[inc] breaks=N` counts only the open band.

## 05 -- backtracker

| setting | default | meaning |
|---|---|---|
| `MODE` | `stuck` | `stuck`: greedy dives that always complete, cheap triage that proves nothing. `any` / `lds`: iterative deepening over the break count, where an exhausted level proves no completion with that few breaks |
| `HOLES` | `holes_open_border_TR.csv` | cells to reopen; a complete board needs one |
| `ORDER` | `mrv` | cell order (most constrained first is the right default) |
| `MAX_MISMATCH` | 30 | break budget |
| `RESTARTS` | 100000 | dives per board (stuck) |
| `TIME_LIMIT` | 300 | seconds per board |

Stuck dives on a reopened, polished board land well below its score (about 28
breaks against 18): they rank rough partials, and they don't improve good
boards. The run writes up to five CSVs, listed in `$OUT.outputs.txt`.

## 06 -- roundhouse both ways

Runs each board through two chains of two roundhouse passes, `--ccw` then
`--cw` and `--cw` then `--ccw`, which together cover all four sides. The final
tally says which direction won. `HOLD=1` makes the second pass keep the half of
its final side the first left standing. Boards are tagged by pass (`_a1`,
`_b2`).

## 07 -- the whole chain, barebones

```bash
cd ~/runs && bash ~/E555/examples/07_barebones_chain.sh
```

Six literal calls, which can be copied into a terminal as they are:

1. the beamer to row 12 (`--incomplete_top` keeps the boards that reach it with
   11 of its 16 pieces);
2. the finalizer from row 4 (`--incomplete_top`, `--clue_center`);
3. the roundhouse (only when a complete row 12 exists; usually skipped);
4. backtracker dives to 256 pieces;
5. `E555_rank.py`, then the viewer.

- **Output** stays in the current directory, named by `PREFIX`. Only
  `PREFIX_ranked.csv` survives; change `PREFIX` between runs, because the C
  tools append.
- **Run length** is set by the beamer's `--wall_time` (two thirds of the total)
  and the backtracker's `--time_limit` per board.
- **Measured** on 4 cores: 914 s, 41 complete boards, best 455/480.
- **First edit to make:** add `--db_file chain.db` to the beamer.

## 08 -- distiller

Distils a corpus of any size to the N boards worth the CP-SAT tail, finished
and probed, and writes the ender command for them.

| setting | default | meaning |
|---|---|---|
| `BOARDS` | `data/E565_lowB_baseline.csv` | partial or complete boards, `.csv` or `.csv.gz` |
| `TOP` | 5 | boards to keep |
| `OUT` | `distilled.csv` | also writes `OUT.plan.sh` (the ender command) and `OUT_work/` |

The stages:

1. drop exact repeats;
2. screen every partial with 300 seeded dives;
3. finish the best half (at most 400 x `TOP`);
4. probe the best 4 x `TOP` with the ender's redive;
5. keep `TOP` boards with distinct foundations.

The run is reproducible, and a rerun resumes from `OUT_work/`. The run prints
its time estimate after reading; the default corpus takes about 4 min on 4
threads. Then:

```bash
bash distilled.plan.sh
```

## 09 -- backtracker from all sides

Fills each break-free partial as far as it goes without breaking an edge
(`--breaks 0`), from each direction in `DIRS` (a cell order plus a board turn).
It keeps the deepest result per board, so there is one board per input.

- **Input.** Feed it break-free partials: a board that already has a broken
  edge passes through unchanged.
- **`TIME_LIMIT`** is per call; set it, since an exact search need not finish.
- **`FOURSIDES=1`** adds `--order 4sides`, which can step over a dead cell.

## 10 -- diver: finish partials

| setting | default | meaning |
|---|---|---|
| `END_DIVE` | 10000 | dives per board |
| `END_POLISH` | 2000 | kick-and-polish steps; -1 = none |
| `EMIT_SCORE` | 0 | write boards whose best finish is >= this |
| `ROTATIONS` | none | Stage A file: hold beamer boards' edge pieces to their dealt sides |
| `CORNER_SEEDS` | 0 | extra copies with a top-corner block placed (needs `ROTATIONS`) |

Placed cells never move. The output does not depend on `THREADS`.

## 11 -- diver: improve complete boards

| setting | default | meaning |
|---|---|---|
| `REOPEN` | `auto` | cells to re-dive: `auto` (the damaged band, at most 5 rows), `auto+E`, `top:K`, `box:R0-R1,C0-C1`, or a mask file |
| `SECONDS_PER_BOARD` | 60 | wall clock per board; rounds run until it is spent |
| `COPIES` | 8 | copies per round, each on its own random streams |
| `END_DIVE`, `END_POLISH` | 3000, 1000 | per copy |
| `PRIOR`, `NOGO` | 1, 1 | start the dives pulled toward the board's clean placements and off its broken ones |
| `PLATEAU` | 0 | 1: a round may move to a different board of equal score |

- **Guarantee.** Every board comes back as one row, never worse; the run ends
  with a before → after table.
- **Defaults.** They are the best setting measured (PROJECT_E555.md,
  E555_diver). This is the ender's redive without OR-Tools.

---

## Clue pieces

Scripts 01-04 take `CLUES=1` (`--clue_center --clue_corners` on every tool).
Set it on every stage or on none: a stage that frees a clue cell without the
flag refills it with another piece. `E555_rank.py FILE --sort clues,score`
counts the clues a board still has.

## Turning and sinking a board

Every stage is direction-biased, so a quarter-turn hands the same breaks to a
different attack. Turns are lossless and re-scored:

```bash
python3 tools/E555_rotate.py FILE 1 --seed_file data/seed_Edge5.txt           # -> FILE_rot1.csv
python3 tools/E555_rotate.py FILE 2 --sink 3 --seed_file data/seed_Edge5.txt   # turn, drop 3 rows
```

`--sink N` moves every piece N rows down, so that the damaged rows (turned to
the bottom) fall out and their pieces return to the pool. The result is a
partial for Stage C.

## Results that are not failures

- **A configuration goes extinct.** An empty candidate pool proves that border
  dead below the current row; the beamer moves on to another.
- **The roundhouse reports `REFUTED`.** No arrangement of any pieces fills that
  band; this is a proof, in milliseconds.
- **A roundhouse board scores far below its input.** It is break-free by
  construction; the missing points are the junctions its empty cells leave open.

## Slurm

```bash
sbatch pipeline/slurm_wrapper.sh examples/02_finalizer_regrow.sh
sbatch --cpus-per-task=32 --mem=64G pipeline/slurm_wrapper.sh \
       examples/01_beamer_quickstart.sh THREADS=32 ANNEAL=1
```
