# E555 pipeline

Long, unattended runs. **These are not examples** -- they chain every stage
together and are meant to be started and left alone for hours or days. If you
have not run the tools individually yet, start with
[`../examples/`](../examples/) instead; it takes about ten minutes and the
settings here will make far more sense afterwards.

## The runners

| script | what it is | typical run |
|---|---|---|
| `run_pipeline.sh` | the whole pipeline, borders to a finished board. `BORDERS=random` samples borders from the seed; `BORDERS=annealed` searches for them with Stage A first | 30-60 min per pass |
| `run_pipeline_whirlpool.sh` | turns the board 90 degrees between every re-grow, so the buried bottom rows get re-searched too | hours, or one lap in minutes |
| `run_board_farm.sh` | runs a pipeline pass in a loop forever, keeps only the best boards, and periodically re-attacks them | days |
| `run_farm.py` | the same idea with a different chain, and boards that graduate: produced, refined once, then deeply refined into `champions.csv` and never searched again | days |
| `topper_sweep.sh` | one plan of topper passes over a slice of a board file | minutes |
| `slurm_wrapper.sh` | runs any of the above as a batch job | -- |

```bash
bash pipeline/run_pipeline.sh                       # annealed borders
bash pipeline/run_pipeline.sh BORDERS=random        # sampled borders, start here
bash pipeline/run_pipeline_whirlpool.sh             # four laps around the board
bash pipeline/run_pipeline_whirlpool.sh WHIRL_ROWS=10   # just one lap
bash pipeline/run_board_farm.sh MAX_HOURS=48        # the farm, two days
python3 pipeline/run_farm.py DB_FILE=/tmp/E555.db  # the graduating farm
```

## How every runner works

The same shape as the examples: a settings block of plain assignments at the
top, edited in place or overridden by optional `NAME=value` arguments.

```bash
bash pipeline/run_pipeline.sh RUN_DIR=run7 THREADS=16 BEAM_STOP_ROW=10
```

`REPO` defaults to the script's own parent; set it if you copy a runner
somewhere else. Every stage reads the `outputs.txt` the previous tool wrote
rather than guessing filenames, and passes `--print_cmd`, so the log carries the
exact command each stage ran.

**The annealer needs at least 250000 steps.** Below that it is not merely weaker
-- it often fails to place a legal border at all. Measured on the real seed: 8
restarts x 3000 steps found 2 feasible borders, 2 x 2000 found one on one run
and none on the next, while every restart at 250000 succeeded (2/2, 4/4, 8/8).
The runners warn below the floor rather than refusing.

## The whirlpool

Every Stage B tool grows rows **upward from the bottom**: the beam advances a
row at a time and the finalizer locks rows `0..N` and frees everything above.
So the rows a board stands on were chosen early, by a beam that was guessing,
and are never revisited however often you re-grow the top.

Turn the board 90 degrees and those buried rows become *columns* on one side,
where a re-grow can reach them. What blocked that until now is that a turned
board has complete columns and the finalizer can only start from complete
**rows**. `E555_backtracker --stop_row` converts one into the other -- it
searches rows `0..N` only and emits every exact filling -- and that is the whole
reason this runner exists. One lap:

```
rows 0..T full -> rotate +-90 -> backtracker --stop_row 5 --with_frame --order rowmajor
               -> finalizer --finalize_from 5 --stop_row T -> rows 0..T full
```

`--with_frame` widens the cut to take in the outer frame, so the band carries all
60 border cells instead of the 26 a plain `--stop_row` leaves -- border cells the
turned board holds are kept, the ones it does not are searched, and a band whose
leftover border pool will not chain is dropped. That is what lets the finalizer
run its fixed-sides mode instead of falling back to `--free_edges`. It costs
population (a harder cut) and yield (fixed sides beat fewer completions out of
the same depth), so `FIXED_BORDER=0` keeps the old free-border lap for comparison.

The lap ends where it began, rebuilt from a different direction. It keeps rows
`0..5` of the turned board -- a six-deep slab against one side -- and that side
moves 90 degrees every lap, so **184 of the 256 pieces are freed and
re-searched each lap**, the four slabs hug four different sides, and no piece
survives a full circle untouched. Four laps is one full turn.

It does **not** climb: `WHIRL_ROWS` stays inside 10..12 because below 10 the
output floods (the whole stop-row beam is emitted before anything has gone
extinct) and above 12 the beam is spent and the Stage C tools are better. Twelve
is not merely expensive, it is out of reach from a five-row lock: measured
against a perfect board on its own frame, the beam found completions at row 10
and row 11 and **none at all** at row 12, fixed border or free. The default is
four laps at row 10; row 12 is Stage C's job, not a lap's. The
loop holds its depth and spends its time on coverage; Stage C runs once, at the
end. Both bounds are warnings, not refusals -- go shallow deliberately if you
want to; only a stop row outside 1..13 is rejected.

Nothing in the loop ranks, because there is nothing to rank -- every stage emits
exactly matched boards, so at equal depth they all score the same (a rows-`0..T`
board scores exactly `15(T+1) + 16T`). What thins the field is attrition, and
the per-lap counts the script prints are the real diagnostic: a lap that returns
what it was given means the neighbourhood is exhausted.

```bash
bash pipeline/run_pipeline_whirlpool.sh                  # beamer, then four laps
bash pipeline/run_pipeline_whirlpool.sh INPUT=champions.csv   # whirl boards you have
bash pipeline/run_pipeline_whirlpool.sh WHIRL_ROWS="10 10 10 10" BAND_ROW=5 POP=40
bash pipeline/run_pipeline_whirlpool.sh FIXED_BORDER=0        # the old free-border lap
```

`INPUT` skips the beamer and whirls boards you already have -- they need whole
rows `0..N` filled, since a board with a ragged top has nothing to turn. The
setting that decides how long a lap takes is `2 * POP * BT_LIMIT`, the number of
bands: each is a distinct locked set, so the finalizer rebuilds its reduced
database for every one (about 9 s at `BAND_ROW=5`). Raise `BAND_ROW` and that
database shrinks fast, at the cost of freeing less of the board per lap.

### random or annealed?

Stage A sizes the four border sides so the beamer's sweep actually covers its
own space -- worth real time when you intend to drill a *few* borders hard.
The random pipeline does the opposite: many hands, fresh borders, no
guarantees per border. For a farm that runs for days, random wins on throughput;
for a targeted attack on one promising border, anneal first.

### The farm, in one paragraph

Produce, then improve, then repeat. Four iterations grow brand-new boards from
a fresh border; the fifth stops producing and *resamples* the current champions
instead -- freeing five rows with the finalizer and refilling a border strip
with the roundhouse, alternating the rotation so the region a strip never
touches moves around the board. Everything worth keeping ends up in
`champions.csv` (best first, deduplicated, capped at `KEEP`), every new record
is copied to `records/` and never deleted, and `farm.log` gets one line per
iteration. Ctrl-C is safe at any point.

**Where the output goes.** Each iteration writes its whole output -- the tools'
own logs, `--verbose` telemetry and all -- to a log inside its run directory,
not to the farm's stdout. A record keeps that log next to the board, gzipped;
a failure keeps it under `failed/`, up to `FAILED_KEEP` of them; every other
iteration is deleted with its run directory. That is what makes a multi-day run
survivable: 117 test iterations left 100 KB behind, where the same output on
stdout runs to gigabytes a day. The farm's own stdout is one line per iteration,
carrying how many boards the phase produced -- a run that quietly produces
nothing, because every input contradicts the clue setting or a budget is too
small to reach the stop row, says `0 boards` instead of looking healthy.

**Borders.** `ANNEAL_EVERY` mixes the two sources: 0 runs every production
iteration from random borders, 4 anneals every fourth one. Random costs nothing
and gives the sweep more frames per hour; annealed spends minutes in Stage A
first and starts from stronger material. `ANNEAL_ROUNDS` and `ANNEAL_STEPS`
size those iterations only.

**Clues.** `CLUES=none|center|all`. Centre-only is the middle setting worth
having: the finalizer frees the centre cell on every resample, so holding it
costs nothing, while `all` also pins the four corners and constrains the
roundhouse's strips. Each setting gets its own database cache.

**Running unattended.** Ten consecutive failures stop the farm (`GIVE_UP_AFTER`)
rather than spin on a broken setup -- a dead iteration costs no time, so without
that brake a bad setting burns a core for days. `ortools` is checked before the
first iteration, since without it every production iteration would die in the
CP-SAT stages. Stale `run_*` directories from a killed farm are cleared at
startup.

The single most valuable setting is `DB_FILE`: without it, every production
iteration spends 2-3 minutes rebuilding the same 6.4 GB chain database. It
defaults to `farm/chain.db` and needs about 6.5 GB of free disk. An absolute
path puts it on a node-local disk, which is what you want on a cluster:
`DB_FILE=/tmp/E555.db`. Only the beamer can use it -- the finalizer and the
roundhouse build a database around the pieces each board locks, so there is
nothing shareable to cache.

**Settings the farm does not name** reach the pipeline through `PIPE_EXTRA`:
`PIPE_EXTRA="BEAM_STOP_ROW=12 TOP_N=40 BT_TIME=600"` is passed verbatim to every
production iteration.

### The other farm: `run_farm.py`

Two runners now loop forever, and they answer different questions.
`run_board_farm.sh` drives `run_pipeline.sh`, so it inherits that chain and
re-attacks its champions indefinitely. `run_farm.py` runs **its own chain**,
built around the one measured fact that shapes everything here: **no
configuration of any tool in this repo has ever emitted a complete row 12**. So
it stops asking the beam for one and hands the job to CP-SAT.

    1 borders    random, or annealed every ANNEAL_EVERY-th iteration -- anneal
                 4x ANNEAL_BORDERS restarts, keep the best quarter
    2 beamer     to row 11, --top_columns 5, --max_emitted 0 (uncapped)
    3 finalizer  to row 12 from row 5, exhaustive columns, --incomplete_top
    4 topper     FOCUSED: rows (16-FOCUS_DEPTH)..12 open, to close row 12
    5 roundhouse forward then reverse at --strip_width 3
    6 topper     FINAL: the top band, filling rows 13..15
      refine     the best REFINE_TOP get one ender pass
      harvest    every HARVEST_EVERY iterations, the best HARVEST_TOP get a
                 deep ender and graduate

**Boards graduate rather than being re-attacked.** A board's search history is
simply which file it is in — `queue/produced.csv`, then `queue/refined.csv`,
then `champions.csv`. Because every stage takes the *top N* of a ranked file,
"remove what we just took" is a slice, so no attempt counter is stored
anywhere. `champions.csv` is the deliverable: deeply refined, ranked, and never
searched again, which is what makes it safe to hand to other tools while the
farm keeps running.

**Stage 4 is the one to understand.** `--band_depth D` counts inward from the
top edge, so the band is rows (16−D)…15, and `--locked_rows 3` locks 13–15
empty; the solver's free range is rows **(16−D)…12**. `FOCUS_DEPTH=8` opens rows
8–12. Depth is the whole point: the pieces needed to close row 12 are usually
already placed somewhere in the core, and a shallow band could only draw on
pieces still unplaced. Measured on one row-11 board at depth 8, the topper
closed row 12 completely, taking 15 pieces from the unplaced pool and **lifting
one out of row 11**. `FOCUS_DEPTH` is the dial to turn if row 12 will not close.

**Stage 5 filters itself.** `--strip_width 3` keeps *more* of the board than a
wider strip, and `core_usable()` requires every kept cell to be placed,
frame-legal and break-free — so a board that did not genuinely complete row 12
is skipped by name. No filtering code of ours is involved.

**Deduplication after the roundhouse is by id prefix.** The roundhouse keeps
the input board's `config_id` and appends `_<line><tag><n>`, so a board that
reached it is exactly one whose id prefixes some output's id. The final topper
gets the roundhouse's boards plus every stage-4 board with no descendant.

**Budgets.** `--wall_time` covers the beamer's whole run, database build
included, so `BEAM_WALL` under about four minutes with no `DB_FILE` is spent
entirely on the build — the farm warns rather than letting you discover it from
a day of `0 boards`. Set `DB_FILE` to an absolute path on node-local disk and
the build happens once.

**Unattended.** ortools is checked before iteration 1; each tool's exit code is
checked separately, so a stage that dies does not discard what the stages
before it made; `GIVE_UP_AFTER` consecutive failures end the run (`0` never
gives up); pools are written atomically and `farm_state.json` lets a restart
resume; `SIGINT`/`SIGTERM` finish the current iteration and exit 0. Per-iteration
logs are kept only for records and failures, which is what keeps a multi-day run
to kilobytes.

## The topper sweep

One script, `topper_sweep.sh`, drives `E555_topper.py` through a list of passes.
A pass is `run_pass SIDE WINDOW LOCKED` -- which border band opens, how deep it
is, and how many of its outermost rows to **unset and hold empty**. `PRESET`
names a plan; each plan is a handful of literal `run_pass` lines near the bottom
of the script, so a new one is written by copying six lines rather than by
learning a syntax:

| `PRESET` | plan | when |
|---|---|---|
| `safe` | `T 5 0`, then `TR TL TB R L B` at `3 0` | a board filled to row 11. Nothing is ever unset, so no pass can make the board worse. Start here |
| `window` | `T 8 3`, `T 6 2`, `T 5 1`, `T 4 0`, then `TR 4 0` and `L 4 0` | a window sliding up the board, then two clean-ups on borders it never reached; what `run_pipeline.sh` stage 5 runs |
| `deep` | every side as a pair -- a wide pass with its outer band emptied, then a narrow pass that refills it | when the safe sweep has run out of moves and you will spend breaks to buy freedom |
| `closeT` `closeB` `closeR` `closeL` | `X 6 2` then `X 4 0` on the named side | closing a hole that sits against one border -- an `E555_roundhouse` `miss0` board, or any partial whose breaks are all on one side |

```bash
bash pipeline/topper_sweep.sh INPUT=boards.csv PRESET=safe
bash pipeline/topper_sweep.sh INPUT=boards.csv PRESET=closeT OUT=closed.csv
bash pipeline/topper_sweep.sh INPUT=boards.csv PRESET=deep MAX_TIME=3600
```

`FIRST_ROW`/`NUM_ROWS` cut this run's slice of `INPUT`, so several runs can
share one file. The script knows nothing about schedulers: under a Slurm array,
pass `FIRST_ROW=$((SLURM_ARRAY_TASK_ID * NUM_ROWS))` from the submit script.

`LOCKED > 0` is what makes the deep plans work, and it is worth being clear
about: it does not merely freeze those cells, it **unsets** them, lifting their
pieces back into the pool. The solver then rebuilds the inner part of the window
with more material than cells -- that is how you reach pieces buried in the core
-- and it hands back a board with a hole and a *higher* break count. A later
`LOCKED=0` pass fills the hole and the count comes down.

So the passes group themselves: everything up to and including the next
`LOCKED=0` pass is one group, and the sweep prunes only at the end of a group --
ranking what came out together with the board that went in, so a group that runs
out of time can never leave the board worse than it found it.

### Closing a roundhouse partial

The `close*` presets are the two-pass pair aimed at `E555_roundhouse` output:
boards that are break-free with one rectangular hole, which is the easiest thing
CP-SAT is ever handed. Pick the letter for the side the hole touches -- the
roundhouse leaves it in the band of its LAST round:

| roundhouse | hole | preset |
|---|---|---|
| `--rotate 1` (its default) | bottom | `closeB` |
| `--rotate 2` | right | `closeR` |
| `--rotate 3` (= `-1`) | top | `closeT` |
| `--rotate 0` | left | `closeL` |

```bash
bash pipeline/topper_sweep.sh INPUT=round_out/roundhouse_round1_rot1_W5_miss0.csv PRESET=closeB
```

Read the DEPTH off the roundhouse's `[emit]` line
(`60 empty in rows 12..15 x cols 0..14`): a run that stalls a round early leaves
part of a second band empty as well, and measured `--rotate 1` holes reached
rows 0..8, which needs a wider plan than the preset's, e.g.
`PLAN="B:9:3 B:7:0"`. The last `LOCKED=0` pass must open **every** empty cell:
unplaced pieces join the solver's pool whether or not their cell is open, so a
window that misses part of the hole wastes the pass.

Measured on a real board (196/256 placed, 60-cell hole, from a 455/480 input):
one 4-deep pass reaches 26 breaks with a broken border and damage across five
rows; the pair reaches the same 26 breaks with the border intact and the damage
folded into three. The L-shaped bands are *worse* here -- `TL` at 4 deep opens
~112 cells, CP-SAT does not converge, and the same board came out at 49 breaks.
Open one side.

## Fixing the Eternity II clue pieces

Every runner takes **`CLUES=1`**, which passes `--clue_center --clue_corners`
to every stage that supports them. It is off by default and changes nothing when
unset. `run_pipeline.sh` and the farm also take **`CLUES=center`**, which holds
the centre clue alone -- the corners are what constrain the border search
hardest, so centre-only is the setting to reach for when `all` starves a run of
borders. `none` and `all` spell out `0` and `1`; all four are accepted.

```bash
bash pipeline/run_pipeline.sh BORDERS=random CLUES=1
bash pipeline/run_pipeline.sh BORDERS=random CLUES=center
bash pipeline/topper_sweep.sh INPUT=board.csv PRESET=window CLUES=1
```

It is one switch per run, not per stage, on purpose: a clue held by the beamer
and then dropped by the finalizer is no better than never holding it, and that
is exactly what happened before -- `FIN_FROM` sits five rows below the beam's
stop row, so the finalizer frees the centre clue's row on every run. The farm passes `CLUES`
down to the production pipeline it drives.

Two consequences worth knowing:

* **The chain database differs.** A clued run excludes the clue pieces from the
  chains, so its cache is not interchangeable with a normal one -- the beamer
  reads the exclusion set out of the cache header and rebuilds when it does not
  match, rather than returning wrong chains. The farm gives each setting its own
  file, `$DB_FILE.center` and `$DB_FILE.all`, so switching `CLUES` between runs
  does not throw the other cache away. The other runners take `DB_FILE` as you
  give it, so name it yourself if you toggle clues with one of those.
* **A clued whirlpool band costs up to 4x.** A band cut at `BAND_ROW=5` carries
  no clue -- the centre one sits on row 7 or 8 -- so the finalizer has no
  orientation to read and *chooses* instead, searching the band once per
  orientation the band does not already contradict. Four passes over one shared
  database; pass `--clue_orient N` to the finalizer to pin it to one.
* **The backtracker stage is not clue-aware.** It only moves pieces in cells
  named by `$HOLES`, but the shipped masks do free clue cells
  (`holes_open_border_TR.csv`, the default, frees the one at row 13 col 13), so
  the last stage can still displace a clue. Check with the ranker's `clues`
  column, which counts how many of the five a board still holds:

```bash
python3 tools/E555_rank.py pipeline_out/FINAL_best.csv --sort clues,score --no_id
```

## Reading the output

Every stage writes the same canonical CSV, so any file here can be fed back into
any tool -- including the one that produced it.

```bash
python3 tools/E555_rank.py pipeline_out/FINAL_best.csv --seed_file data/seed_Edge5.txt
python3 tools/E555_viewer.py pipeline_out/FINAL_best.csv --seed_file data/seed_Edge5.txt
```

A stage that emits nothing is usually telling you something true about the
board, not failing: the beamer going extinct proves that border is dead below
the current row, and the roundhouse refusing every strip start is a proof about
that board delivered in milliseconds. The runners detect both, say so, and carry
the previous stage's boards forward.

## Slurm

The runners are plain bash. One wrapper runs any of them as a batch job:

```bash
sbatch pipeline/slurm_wrapper.sh pipeline/run_pipeline.sh THREADS=8 RUN_DIR=run1
sbatch --cpus-per-task=32 --mem=64G pipeline/slurm_wrapper.sh \
       pipeline/run_pipeline_whirlpool.sh THREADS=32
sbatch --array=0-9 pipeline/slurm_wrapper.sh \
       pipeline/topper_sweep.sh INPUT=boards.csv NUM_ROWS=500
```

Anything passed to `sbatch` overrides the wrapper's own `#SBATCH` defaults. For
the array case the sweep needs its slice, so give it one in the submit script
rather than expecting it to read the scheduler's environment itself.
