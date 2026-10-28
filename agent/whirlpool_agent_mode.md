# E555 whirlpool kit: make the whirlpool a practical jewel finder

The second `/goal` kit (the first is `agent/autonomous_agent_mode.md`). The
agent improves `pipeline/run_pipeline_whirlpool.sh` until it reliably grows
boards from all four sides under one fixed border, fast enough to be a
practical tool on a Claude Code cloud machine. It then runs the improved tool
for the rest of its budget to collect such boards, dives and polishes them,
and keeps the best. `agent/launch_whirlpool_agent.md` launches it in a cloud
session (Opus, medium effort) and holds the session mechanics: setup,
timeouts, checkpoints, usage limits, the STATUS line, and how to finish. This
file is the agent's manual. **Experimental:** the scripts and the judge are
tested; a full campaign under `/goal` is not.

## 1. The goal, in plain words

Stage B grows a board upward, one row at a time, from the bottom border. The
rows it chose first, at the bottom, are never searched again. The whirlpool
fixes that: it **turns the board a quarter turn**, keeps a slab of it against
the new bottom side, and **re-grows the rest** from there. One turn is a
**lap**. Three laps, turned the same way each time, grow the board from the
other three sides, so it has then been grown from all four.

The current runner rarely gets there. Depending on its start row and settings,
it either **floods**, enumerating far more bands than it can grow in time, or
**collapses** to no board at all. Section 5 shows why. A **stable** runner
keeps at least one live board at every lap, and never more than it can grow
in the time left.

What we want is rare. A **jewel** is one fixed border (all 60 frame cells, the
same piece and spin on each) with rows 0..10 or deeper reachable from **every**
side. Your job, on this machine:

1. **Optimize.** Find the best whirlpool runner, an improved
   `run_pipeline_whirlpool.sh` that uses whatever the repository's tools now
   offer. "Best" means the most jewels found per hour, where each beamer board
   gets at most **3 minutes** to complete its three laps. The beamer makes
   thousands of boards in minutes, so choosing which boards to whirl, and which
   to skip, is part of the job.
2. **Produce.** Once improvements level off, run the best runner for the rest
   of the budget, finding more jewels. Dive and polish them into complete
   boards, and keep the best-scoring ones.
3. **Report.** Explain every change that worked, in plain language, in the
   campaign's `README.md`, and push it all to the session's branch.

A board above 465 (the repository's best, `data/best_465.csv`) would be a
record. It would be welcome, but it is not the goal.

## 2. The words this kit uses

`agent/whirlpool/judge.py` applies these definitions. Its verdict is the only
one that counts.

- **Beamer board.**
  - Exactly matched: no broken edge between two placed pieces.
  - Every piece legal on its cell: grey sides exactly where the cell faces out
    of the board.
  - Rows 0..T complete, with T ≥ 10.
  - The frame not complete. A board holding all 60 frame cells is a lap's
    board; the beamer writes only part of the frame.
- **Lap.** Turn a board a quarter turn, then re-grow it exactly until rows
  0..10 are complete. The lap must **keep** part of its parent: at least 10
  interior cells holding the same piece at the same spin. That is what makes a
  lap a re-grow and not a new board.
- **Chain.** A beamer board `s1` and the boards after each lap, `s2`, `s3`,
  `s4`.
  - The rows carry ids `<tag>_s1` .. `<tag>_s4`, where the tag names the
    lineage.
  - Each board is written the way it was grown, with rows 0..10 complete.
  - Every lap turns the same way: `d` = 1 (clockwise) or 3, as
    `tools/E555_rotate.py` counts.
- **Laps reached.** The number of valid laps in a row from `s1`.
- **Fixed border.** `s2`, `s3` and `s4` hold the same complete 60-cell frame,
  cell for cell, once turned back to `s1`'s orientation. `s1`'s own frame
  cells agree with it. The border is chosen at lap 1 and never changes after.
- **Jewel.** A chain with 3 laps reached and a fixed border.
  - Its **depth** is the lowest of its four boards' depths; a board's depth is
    the highest row r such that rows 0..r are complete. So a jewel of depth 11
    reaches row 11 from every side.
  - It is identified by its frame (`frame_id`, the same whichever way the board
    is turned). A frame counts once per campaign.
- **Attempt.** One `whirl` call on one beamer board. The harness kills it at
  **180 s** of wall time, with every core.
- **Jewels per hour.** New-frame jewels divided by the harness's wall time,
  which includes making the boards. This is the score of a candidate.

The judge also reports two diagnostics:
- the **funnel**: how many attempts reached lap 1, 2 and 3;
- the **frame**: held, or the first board where it broke, for example
  `incomplete:s2`.

`python3 agent/whirlpool/judge.py --self_test` builds chains from the
synthetic solution and shows each case.

## 3. The harness and the candidate contract

`agent/whirlpool/harness.sh` is the only source of results. It runs one
**candidate**, a bash script in your `candidates/` folder, for `MINUTES` of
wall time:

1. **produce.** When its queue is empty, it calls `MODE=produce`. The
   candidate runs the beamer and writes beamer boards to `OUT`, in the order
   it wants them whirled. The judge keeps the valid boards that this campaign
   has never whirled. A produce call is killed after `PRODUCE_CAP` (600 s).
2. **whirl.** It calls `MODE=whirl` on the next queued board. The candidate
   appends chain rows to `OUT` as each lap completes. A whirl is killed at
   `CAP` = 180 s. The judge then scores the chains, and each jewel with a new
   frame goes to `jewels.csv` and `jewel_index.csv`.
3. **Summary.** At the end it prints the `HARNESS` line, appends one row to
   `harness.csv`, and updates the candidate's totals and the campaign's best
   in `scoreboard.json`.

What it prints:
```
PRODUCE n=1 seconds=140 exit=0 queued=286 total=300 valid=286 unseen=286 invalid=14 reasons=depth:14
ATTEMPT n=1 board=r0b3l1 seconds=180 exit=cap chains=1 laps=2 frame=incomplete:s2 jewels=0 new=0
HARNESS run=r20261028-0106 candidate=v03_ring@1a2b3c4d+none minutes=60.4 standard=yes produced=812 attempted=19 funnel=lap1:15/lap2:9/lap3:6 frame_held=4 jewels=3 per_hour=3.0 depth_max=11 produce_share=8% cap=180 best=...
```

**The calls.** The harness calls the candidate with these settings:
```
MODE=produce REPO=<lab> SEED=<lab>/data/seed_Edge5.txt RUN_DIR=<fresh dir> THREADS=<cores>
             RNG_SEED=<new> DB_FILE=<lab>/E555.db (or empty) TIME_LIMIT=600 OUT=<dir>/queue.csv
MODE=whirl   REPO=<lab> SEED=<seed> RUN_DIR=<fresh dir> THREADS=<cores> RNG_SEED=<new>
             TIME_LIMIT=180 INPUT=<dir>/board.csv OUT=<dir>/chains.csv
```
`agent/whirlpool/example_candidate.sh` shows the mechanics in 90 lines. It is
naive on purpose and finds no jewel, because its frame breaks at `s2`.

**What a candidate must do:**
- **produce** runs the beamer in that call, with that `RNG_SEED`, and queues
  only boards it made there.
- **whirl** searches from `INPUT` alone. It writes `INPUT` itself as `s1`,
  writes each lap's board as soon as the lap is done (a killed call keeps what
  it wrote), and may stop early when a board looks hopeless.
- **Both** read only the lab's tools and data, the seed and the inputs given.
  They may also read data files in `candidates/` (a screening table you
  derived, for example). Those files are part of the candidate: never change
  them after its first run; make a new version instead. Never read earlier
  results such as `jewels.csv` or old attempts.

**Rules the harness enforces:**
- The candidate must live in `candidates/` (or be `run_pipeline_whirlpool.sh`).
- The lab's `src/`, `tools/` and `Makefile` must equal the repository's plus
  `patches/`.
- Each call gets a fresh `RUN_DIR`, deleted afterwards.
- When a call ends, every process it started is killed, however it was
  started.
- A whirl directory above 2 GB is killed.
- Only runs with `CAP=180` count towards a candidate's score.
- A candidate becomes the campaign's best only after 30 minutes of such runs.
  The best is the highest jewels per hour; ties go to the better funnel: lap 3,
  then lap 2, then lap 1, per attempt.
- `MINUTES` is 5..90. A run stops early if two produce calls in a row queue
  nothing.

## 4. Hard rules

1. **Write only in your campaign's folder,** `agent/whirlpool/<CAMPAIGN>/`,
   and its `lab/`. Read anything; edit nothing else:
   - not this kit or its launcher;
   - not `agent/whirlpool/*.sh` or `judge.py`;
   - not the repository's `src/`, `tools/`, `data/`, `pipeline/` or `tests/`;
   - not another campaign's folder.

   `checkpoint.sh` refuses to commit while anything outside the folder has
   changed.
2. **Code changes go in `patches/` only.**
   - A patch is a full copy of a `src/` or `tools/` file, or of the
     `Makefile`, at its repository path. For example,
     `patches/src/B_beam/E555_finalizer.c`.
   - `setup.sh` lays the patches over the lab and rebuilds; the harness refuses
     a lab that differs from the repository plus `patches/`.
   - Follow the repository's conventions (`CLAUDE.md`, "Making changes"). New
     behaviour goes behind a new flag that is off by default, and a tool run
     without it writes byte-identical output. A patch can then go upstream as
     it is.
   - After a patch, run the lab's own checks for the tools it touches, for
     example `ARCH=<arch> bash tests/run_tests.sh finalizer` from the lab.
3. **Results come from the harness alone.** Never edit any of these by hand:
   - `jewels.csv`, `jewel_index.csv`, `attempts.csv`, `harness.csv`;
   - `seen_boards.txt`, `frames_seen.txt`;
   - `best`, `candidates` and `harness_runs` in `scoreboard.json`.

   A candidate file never changes after its first harness run; a new idea is a
   new file, `candidates/vNN_<name>.sh`. Every number in the README comes from
   these files.
4. **Size.** The chain database `lab/E555.db` (6.4 GB) stays in the lab and is
   never committed; `*.db` and the lab are gitignored. `checkpoint.sh` refuses
   any file over 50 MB, and any commit that takes the folder's history over
   100 MB.
5. **No clues.** Leave the clue flags off.
6. **Git:** see the launcher. Only `setup.sh` and `checkpoint.sh` write. Never
   push to `main`.
7. **Report honestly.** Report what failed as well as what worked. A setting
   you did not measure is labelled as such.

## 5. What is known

**About the current runner** (`pipeline/run_pipeline_whirlpool.sh`; read it,
and pipeline/README.md "The whirlpool", PROJECT_E555.md §8). It has three
structural problems:

- **It does not guarantee four sides.** It turns every board both ways every
  lap. A lineage that goes clockwise and then counter-clockwise is back on the
  side it started from.
- **It does not keep a border.** The band cut (`backtracker --stop_row 5
  --with_frame`) searches the frame cells the turned board lacks. Then, in
  fixed-sides mode, the finalizer (PROJECT_E555.md §6.3, §6.4, §6.8):
  - keeps the left column, and the top row while the ring is clean;
  - re-chooses the right column's order above the lock;
  - writes only rows 0..stop_row, the locked top rows, the left column and the
    top-right corner.

  So a lap board lacks the right column above row 10, and the frame can change
  from lap to lap. `--keep_ring` holds the whole ring in place. Its usage text
  warns that a `--with_frame` band's sides are one random order, which pinned
  often cannot be completed; a ring a board was grown against is the intended
  use.
- **Its population is not controlled.** A lap grows `2 x POP x BT_PICK` bands.
  The finalizer rebuilds its reduced chain database for every distinct locked
  set; consecutive input lines with the same locked set reuse it. Above a
  certain number of bands that cost decides the run (pipeline/README.md: about
  9 s per band at `BAND_ROW=5`, on a bigger machine). When none of a lap's
  bands re-grows, the lap carries nothing forward.

**About the stop rows** (pipeline/README.md):
- A target row below 10 floods: nothing has died out that early.
- Row 12 was out of reach from a 5-row lock even on a perfect board's own
  frame: 7491 completions at row 10, ten at row 11, none at row 12.
- A band cut with `--max_emitted 0` floods: rows 0..3 alone gave 7.6 M bands
  in five minutes.

**Measured on a 4-core cloud container** (Intel Xeon @ 2.80 GHz, 15 GB RAM,
arch v3), with `example_candidate.sh` in a 12-minute harness run:
- **Boards are cheap.** The first produce call builds the chain database and
  writes `lab/E555.db` (6.8 GB): 141 s in all, 628 boards. Later calls load it
  in 8 s; one call wrote 5096 boards in 9 s (width 10000, `--backtrack_row 6`,
  stop row 10).
- **They are not diverse.** All of those boards came from a single border
  configuration (id `rndb0l0`): one produce call explores one random bottom
  row and left column unless its settings ask for more (`--samples`,
  `--top_columns`, `--max_emitted`).
- **Some carry cells above row 10:** the beamer's column extension and its top
  cap, about one column and one top-border cell per board.
- **The reduced database costs about 9 s per band at `BAND_ROW=5`** on 4
  cores: 2.6 s to build and 5.2 s to sort. A lap whose 4 bands all die costs
  36 s; their beams die in milliseconds.
- **Lap 1 is easy, lap 2 is the wall.** Lap 1 (4 bands, width 20000,
  `--max_emitted 1`) reached row 10 on 41 of 48 boards, usually in 10 s. Lap 2
  reached it on none of the 41.

  On the board inspected, the lap-2 band cut proved that no band exists
  (`no_solution`). The lap-1 board lacks its right column above row 10. Turned,
  its empty band cells form a 6 x 4 block (rows 0..5 x columns 11..14), wedged
  between the lap-1 board's column 10 and the frame's column 15, both fixed. An
  exact fill must meet both sides at once.

  Lap 1 never faces this: its block touches nothing on the frame side, because
  the frame there is still being searched. Every lap after the first does,
  with a fixed border. That is the collapse to beat: give the band room (lift
  a margin of the parent beside the block before the cut), or grow each lap
  with the next band in mind.
- **Bash pitfall.** Under `set -o pipefail`, `grep ... | head -1` kills a
  script: `head` closes the pipe, `grep` dies of SIGPIPE, and `set -e` exits.
  The example uses `awk '... {print; exit}'`.

**This machine.** A Claude Code cloud container has a few cores (4 when this
kit was written) and about 15 GB of RAM. The beamer's database takes 6.4 GB of
it. Never run two beamers at once. Run nothing else heavy while a harness run
is timing a candidate. Several finalizers in parallel, with the threads split
between them, are allowed inside a candidate. `setup.sh` reports the machine;
measure on it.

## 6. Levers

Run each binary with no arguments for its current usage text, which is more
complete than this list. PROJECT_E555.md has the measurements behind the
options.

**Finalizer** (§6). The lap's re-grow.
- `--keep_ring`, `--free_sides`, `--free_top`: what the border does.
- `--backtrack_row N` and `--backtrack_row_factor M`: exhaustive from row N.
  With N = `--finalize_from`, the locked board itself is the single root.
- `--lambda_corners [F]`: top-corner supply; needs known sides.
- `--accept_best F`, `--frac_rand F`, `--tau_columns T`, `--top_columns N`
  (0 enumerates every left column), `--bail_columns N`.
- `--max_emitted N`, `--wall_time S`, `--finalize_repeats N`, `--rng_seed S`.

**Backtracker** (§9.5). The band cut.
- `--stop_row N` or `--stop_column N`, with `--with_frame`.
- `--order` (`rowmajor`, `mrv`, `frontier`, ...), `--reverse` (anchors the band
  at the far side), `--hall`.
- `--max_emitted N` (1 by default with a stop band), `--time_limit S`,
  `--all_for_one`.

**Beamer** (§5). What `produce` makes.
- `--random_edges --samples 0`, `--beam_width`, `--backtrack_row 6`. A narrow
  beam to row 6 and exhaustive from there is the cheapest way to deep rows
  (§5.13).
- `--extend_nodes`, `--exhaust_inner_color`, `--exhaust_border_color`,
  `--bc_window_accept`, `--tau_bottoms`, `--tau_columns`, `--max_emitted`.

**Other tools:**
- The roundhouse (§7): an exhaustive strip refill, with a DP oracle that
  refuses impossible strips in milliseconds. It could test whether a frame
  works from a side.
- `tools/E555_rank.py`: `--diverse N`, `--sort`, `--count`.
- `tools/E555_rotate.py`: quarter turns.
- The diver (§9.2): for the dives in section 8.

**Ideas worth testing.** These are hints, not orders.
- **Grow depth-first.** Take one lineage through all three laps. When a lap
  fails, back up to the next candidate of the last lap that worked. Do not
  carry a population breadth-first.
- **Screen before spending a lap.** The beamer gives boards by the thousand,
  and the 3 minutes are per board. Cheap early signs may predict a finish:
  - a lap-1 band cut that finds bands within seconds;
  - how many bands it finds;
  - whether the frame closes;
  - whether a strip along each side passes the roundhouse's oracle;
  - features of the board itself.

  Measure what predicts a jewel, and use it to order the queue and to bail
  out early.
- **Fix the frame at lap 1, then hold it.** Use `--keep_ring` in laps 2 and 3.
  Lap 1's board must already hold all 60 frame cells. Ways to get there: put
  back the band's right column above the stop row, if it still closes; patch
  the finalizer to write the ring; or choose the band frame with the other
  sides in mind.
- **Make each lap cheaper.**
  - A higher `BAND_ROW` gives a much smaller database and faster laps, but
    frees less of the board.
  - `--max_emitted 1` stops at the first board that reaches the row.
  - Order the bands so that the same locked set comes in a row.
- **Deeper jewels.** Once laps to row 10 are reliable, try row 11 on some laps,
  watching jewels per hour.

## 7. Method

1. **Baseline.** Write `candidates/v00_baseline.sh`: the current runner's
   settings and commands, unchanged, under the contract.
   - `produce` is its stage 0, with `--wall_time` and `--max_emitted` fitted
     to the 600 s cap.
   - `whirl` is its lap loop with `WHIRL_ROWS="10 10 10"` and its values for
     `POP`, `BT_LIMIT`, `BT_PICK`, `BT_TIME` and `FIN_*`, turned both ways as
     it does. It writes one chain per lineage, tracking each board's parent
     and direction.

   Run it for 30 minutes, and `example_candidate.sh` (copied to `candidates/`)
   for 20. Journal the funnels: they are the starting point.
2. **Diagnose.** Time each step on this machine: produce, band cut, the
   finalizer's database build, the beam. Find where lineages die (the funnel
   and the `frame=` fields of `attempts.csv`). Journal a short findings table.
3. **Iterate.** One idea per candidate, `candidates/vNN_<short-name>.sh`, with
   a comment at its top saying what it changes.
   - Screen a candidate with a 20-30 minute harness run.
   - A candidate becomes the best only after 30 standard minutes.
   - To claim that A beats B, give both at least 60 minutes. With jewel counts
     a and b over equal time, the difference must exceed `2·sqrt(a + b)`.
   - While jewels are still 0, compare funnels instead: lap 3, then lap 2,
     then lap 1, per attempt.
   - Journal every result with its `harness.csv` run id.
4. **Produce.** When the STATUS line shows `since_best` at its limit
   (`PLATEAU`, run time without a new best), spend at least 75% of the
   remaining run time on harness runs of the best candidate, each
   `HARNESS_MINUTES` long (the launcher's setting, 60 by default). Each run
   also adds to its measurement. Dive the new jewels
   (section 8). Try a new idea only between runs. A new best switches
   production to it.
5. **Finish** at `stop=budget` (the launcher, "Done"): the final runner, the
   README, a checkpoint.

## 8. Diving the jewels, and the best boards

Diving, polishing and scoring take at most 25% of the run time. Run them
through `timed.sh`. Do them between harness runs, never during one.

A jewel's `s2`, `s3` and `s4` boards each hold the full frame and 11 rows from
one side, so the diver fills the rest (PROJECT_E555.md §9.2). From the lab:
```
grep -E '_s[234],' ../jewels.csv > runs/to_dive.csv
bash ../../timed.sh "dive jewels" -- bin/E555_diver data/seed_Edge5.txt runs/to_dive.csv runs/dived.csv \
    --end_dive 10000 --end_polish 20000 --emit_score 0 --threads <T> --wall_time 900
python3 tools/E555_rank.py runs/dived.csv --seed_file data/seed_Edge5.txt --top 8 --out runs/top.csv --rescore
bash ../../timed.sh "reopen top 8" -- bin/E555_diver data/seed_Edge5.txt runs/top.csv runs/reopened.csv \
    --reopen auto --copies 8 --end_dive 1000 --end_polish 10000 --rounds 4 --threads <T> --wall_time 600
```
(`--reopen auto --copies 8 --end_dive 1000 --end_polish 10000` is the best
measured reopen setting, §9.2.)

Keep the 20 best complete boards in `../best_boards.csv`:
```
python3 tools/E555_rank.py ../best_boards.csv runs/dived.csv runs/reopened.csv --seed_file data/seed_Edge5.txt \
    --top 20 --out runs/best.tmp --rescore && mv runs/best.tmp ../best_boards.csv
bash ../../../referee.sh ../best_boards.csv
```
`agent/referee.sh` is the official score: `REFEREE verdict=VALID score=...`.
The STATUS line shows it as `best_dived`. A VALID board above **465** is a
record: checkpoint at once, journal it, and say so in your final message.
`data/` stays untouched: the user moves records there.

## 9. The campaign folder

`agent/whirlpool/<CAMPAIGN>/`, named by its UTC start time. `checkpoint.sh`
commits all of it except `lab/`.

| path | written by | what |
|---|---|---|
| `README.md` | you, at the end | the report (below). Its presence means the campaign is over |
| `run_pipeline_whirlpool.sh` | you, at the end | the improved runner (below) |
| `candidates/` | you | every candidate, never changed after its first run; its data files |
| `patches/` | you | full copies of changed `src/` and `tools/` files, and the `Makefile` |
| `JOURNAL.md` | you | dated notes: plans, findings, results with run ids, restores |
| `best_boards.csv` | you, from the diver | the 20 best complete boards (section 8) |
| `harness.csv`, `attempts.csv` | harness | one row per run, one per attempt |
| `jewels.csv`, `jewel_index.csv` | harness | every new-frame jewel: its four boards; its frame, depths and source |
| `seen_boards.txt`, `frames_seen.txt` | harness | hashes: boards already whirled, frames already counted |
| `runs.csv` | harness, `timed.sh` | the run-time log the budget counts |
| `scoreboard.json` | scripts | settings, clocks, machine, candidates' totals, the best |
| `lab/` | `setup.sh` | gitignored: the toolchain with the patches, `bin/`, `E555.db`, the harness's run directories |

**`run_pipeline_whirlpool.sh`** is the best candidate, made into a standalone
tool for the user, in the repository's script style: a settings block,
`NAME=value` arguments, `--print_cmd` on each tool.
- `MODE=all`, the default, loops produce → whirl, each board capped at 180 s,
  for `WALL` seconds. It writes the chains and the jewels (judged by
  `agent/whirlpool/judge.py`) into `RUN_DIR`, and dives them if `DIVE=1`.
- `MODE=produce` and `MODE=whirl` keep the contract, so the harness can still
  run it.
- It runs from any E555 checkout with built binaries (`REPO=`). A patch it
  needs is named in its header and in the README.

Before finishing, smoke-test it: `MODE=all WALL=600` in a scratch `RUN_DIR`
inside the lab.

**`README.md`** is written for the user, in plain language, with every number
traced to `harness.csv`:
```
# Whirlpool campaign <id>: <one line: the result>
## Result            baseline vs best: jewels/hour, funnel, depth, minutes measured; jewels found; best dived score
## How to run it     the command, the settings that matter, what it writes, patches to apply first
## What changed      one short paragraph per change that helped: what, why it helps, measured effect (run ids)
## Code changes      each patch: the file, the new flag, what it does, the checks it passed
## What did not work each idea that failed, with its numbers, so nobody tries it blind again
## Jewels and boards counts, depth, the best boards (best_boards.csv), any record
## Machine           CPU, cores, arch, restores, usage-limit pauses, checkpoints
```

## 10. Commands

From the lab, `agent/whirlpool/<CAMPAIGN>/lab/`:
```
bash ../../harness.sh CANDIDATE=../candidates/v03_ring.sh MINUTES=60 NOTE="..."   # a scored run
bash ../../timed.sh "label" -- COMMAND ...                                         # any other experiment, logged
bash ../../status.sh                                                               # the STATUS line
bash ../../checkpoint.sh                                                           # commit and push the folder
python3 ../../judge.py CHAINS --input BOARD                                        # judge a chains file by hand
python3 ../../judge.py --boards FILE                                               # check beamer boards
bash ../../../referee.sh FILE                                                      # score a complete board
```
From the repository root: `bash agent/whirlpool/setup.sh CAMPAIGN=<id>` (the
health check, and re-applying `patches/` after you change them).
