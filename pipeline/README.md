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
| `run_whirlpool_jewels.sh` | the whirlpool as a jewel finder: an annealed border, then one beamer board s1 at a time, re-grown under one fixed frame from its left band (turned counter-clockwise), its right band (turned clockwise) and then its top side, each lap keeping a K+1-row band | an hour |
| `run_board_farm.sh` | runs a pipeline pass in a loop forever, keeps only the best boards, and periodically re-attacks them | days |
| `run_farm.py` | the same idea with a different chain, and boards that graduate: built, endered once, then deeply endered into `elite.csv` and never searched again | days |
| `run_cycles.sh` | fresh boards in parallel cycles: a beam that only grows row-11 partials over many border configurations (no end dives), then the distiller on them; every cycle's best boards collect in `best.csv`. The route that made the 464s of `data/best_464.csv` (rows 4-8) | 25 min a cycle on 4 threads, as many slots as the cores allow |
| `run_halo.sh` | L-band boards to row 12 (`--backtrack_unlock_right HALO`), the halo around their core lifted (`HALO=2`: two cells wide around 12 x 12; `HALO=3`: three wide around 10 x 10) and refilled by a frontier ratchet allowed 20 breaks; every complete board polished at length | hours |
| `topper_sweep.sh` | one plan of topper passes over a slice of a board file | minutes |
| `slurm_wrapper.sh` | runs any of the above as a batch job | -- |

```bash
bash pipeline/run_pipeline.sh                       # annealed borders
bash pipeline/run_pipeline.sh BORDERS=random        # sampled borders, start here
bash pipeline/run_pipeline_whirlpool.sh             # four laps around the board
bash pipeline/run_pipeline_whirlpool.sh WHIRL_ROWS=10   # just one lap
bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db   # jewels for an hour, all five clues
bash pipeline/run_board_farm.sh MAX_HOURS=48        # the farm, two days
python3 pipeline/run_farm.py DB=/tmp/E555.db THREADS=10   # the graduating farm
bash pipeline/run_halo.sh TOTAL_TIME=4h             # the halo loop, four hours
bash pipeline/run_cycles.sh TOTAL_TIME=1d DB_FILE=/data/E555.db   # cycles on every core, a day
bash pipeline/run_cycles.sh INPUT=partials.csv.gz RUN_DIR=elite_out THREADS=$(nproc)   # distil one big file of partials
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

Two runners loop forever and answer different questions. `run_board_farm.sh`
drives `run_pipeline.sh`, so it inherits that chain and re-attacks its champions
indefinitely. `run_farm.py` runs **its own chain**, built around the measured
fact that shapes everything here: **no configuration of any tool in this repo
has ever emitted a complete row 12**. So it stops asking the beam for one and
hands the job to CP-SAT.

Eight stages, one tool call each, in `produce()` and `ender()`:

    0 annealer    every ANNEAL_EVERY-th iteration only; 0 = never (default)
    1 beamer      random or annealed borders, grow to row 11, emit all
    2 clean_csv   drop the near-duplicates the beam emits in hundreds
    3 topper      open rows 8..12 and close row 12
    4 roundhouse  width 3, forward then reverse, TIES refills each
    5 clean_csv   again, over the spirals and the boards they came from
    6 topper      open rows 10..15 and fill the top
    7 ender       the best ENDER_TOP, one pass                 -> good.csv
    8 ender       every ELITE_EVERY, the best of those, deep   -> elite.csv

**Boards graduate rather than being re-attacked.** A board's search history is
which file it is in — `boards.csv`, then `good.csv`, then `elite.csv`. Every
stage takes the top N of a ranked file, so "remove what we just took" is a
slice and no attempt counter is stored anywhere. `elite.csv` is the deliverable:
deeply searched, ranked, never touched again, safe to hand to other tools while
the farm keeps running.

**Stage 2 is there because siblings are not diversity.** Many of the beam's
boards are one board plus a different piece on the frontier: identical below
row 11. Stage 3 frees rows 8–12 outright, so a sibling pair presents the *same*
CP-SAT model — same locked floor, same free pool — and two slots buy one search.
Measured on a real sweep: **312 beamer boards carried only 126 distinct floors**,
and `clean_csv` dropped 79 of them as frontier twins.

**`spread()` picks stage 3's input, because ranking cannot.** Every row-11 board
scores *exactly* the same — 192 pieces, break-free, so 480 − 124 = 356 — and a
312-way tie means "the top 20" is really "the first config's first 20". Measured:

| picking 20 from the same 233 cleaned boards | configs | distinct floors |
|---|---|---|
| by rank, as a naive `[:20]` would | 1 | 13 |
| round-robin by config, one per floor | 4 | 20 |

Since rows 0–7 is the whole of stage 3's problem, that is 20 different searches
instead of 13, for the same money.

**Stage 3 is the one to understand.** `--band_depth D` counts inward from the top
edge, so the band is rows (16−D)…15, and `--locked_rows 3` locks 13–15 empty;
the solver's free range is rows **(16−D)…12**, and depth 8 opens rows 8–12.
Depth is the point: the pieces needed to close row 12 are usually already placed
in the core, and a shallow band could only draw on pieces still unplaced.
Measured on one row-11 board at depth 8, the topper closed row 12 completely,
taking 15 pieces from the unplaced pool and **lifting one out of row 11**.
Depth 10 was tried and is worse — 36% slower, slightly lower scores, and zero
optimal proofs where depth 8 gets 8 in 66.

**Stage 4 runs at width 3, to keep the row stage 3 built.** `--rounds 3
--rotate -1` keeps rows W…(15−W) × columns 0…(15−2W), so W=3 keeps rows 3–12 ×
columns 0–12 and frees the bottom three rows, the right three columns and rows
13–15. Wider cuts abandon row 12 entirely:

| W | keeps | row 12 |
|---|---|---|
| 3 | rows 3–12 × cols 0–12 | kept, except columns 13–15 |
| 4 | rows 4–11 × cols 0–11 | **freed** — the spiral tears out what stage 3 built |
| 5 | rows 5–10 × cols 0–10 | freed, and keeps two rows fewer still |

`--rotate -1` re-cuts the bottom band the beam fixed at row 0 and never
revisited, which is how a border piece buried low can come back up. `--ties N`
keeps N distinct refills per board instead of one; the tool drops a tie that
repeats another with a single frontier piece swapped, so they are real
alternatives.

**Two things about width 3 to know before reading its logs.** First, it keeps 13
of row 12's 16 cells, not all 16 — the right band is a vertical strip and
necessarily crosses row 12 at columns 13–15, and no width avoids that, since
freeing three sides always means two vertical strips. Second, `core_usable()`
requires the kept region to be placed **and break-free**, so a board is skipped
unless rows 3–12 are perfect. Measured on 22 real stage-3 outputs, every one
with a complete row 12: **20 were skipped**, the breaks sitting in row 12 (×17),
row 11 (×2) and row 9 (×1). The two that passed returned 196 and 169 placed
against parents of 208.

That is not a defect, it is the filter: stage 3 lands at 384–386 of 387, so the
stage only fires on a board that came out clean, and a skip costs a scan rather
than a search. `kept_spirals()` then drops any spiral that came back short, so
the pool only ever sees one that gained. Do **not** substitute
`--strip_width 0`: it picks the narrowest *usable* width per board, which on
these boards means W=4 or W=5, and two W=5 boards ran 600 s and 272 s
TRUNCATED — eating a 900 s budget for a worse result.

**Was the roundhouse worth moving earlier?** Measured, no. Run on all 312 row-11
boards *before* stage 3 it costs **1309 s** (1007 forward + 302 reverse, all
exhaustive proofs) and never loses a piece — but paired against the same boards
unspiralled, 16 lineages gave **1 win, 10 ties, 5 losses**. Stage 3 frees rows
8–12, so extra row-12 pieces go back in the pool and survive only as a hint.
It stays after stage 3, where its output is inherited.

**The two topper stages behave nothing alike, and their budgets say so.**
Measured over 24 solves of each at `--time_limit 90 --stall_time 30`:

| stage | time used | ended | mean gain |
|---|---|---|---|
| 3 (focused) | 35.8 s, **never** reached the limit | 29% `optimal` at 17.5 s, 71% stalled having last improved ~13 s | +9.3 |
| 6 (final) | 89.1 s of 90 | **88% cut off by the clock** | +61.6 |

Both stalls were then priced exactly, by running six boards with the watchdog
**off** and replaying the incumbent trace — the watchdog only stops the search,
it never changes it, so one trace prices every setting:

| stall | stage 3, 6 boards | breaks lost | stage 6, 6 boards | breaks lost |
|---|---|---|---|---|
| 10 s | 148 s | +8 | 444 s | +16 |
| 30 s | 337 s | +4 | 951 s | +8 |
| 45 s | 495 s | +3 | 1351 s | +3 |
| 60 s | 633 s | +2 | 1411 s | +3 |

Stage 3's improvements arrive in a burst — median gap **0.1 s**, all six first
incumbents at ~4.5 s — then stop; the late gains sit behind gaps of 22–85 s. So
`T1_STALL=10` buys the same result as 15, and it is not worth more because a
break won there does not survive: over 15 boards the stage-3 score and the final
score correlate at **r = −0.18**. Stage 6 is the opposite — median gap 0.7–2.2 s,
every board still improving when the 300 s cap cut it off, one at 298.4 s — so
`T2_STALL=45` is its knee, the same result as 60 for 60 s less, while 60 → 100
costs 130 s per break. (100 was also inert: no board had a gap over 96.7 s.)

**The CP-SAT stages are capped by board count, and must be.** The topper and the
ender take only `--time_limit`, which is per *solve* — unlike the C tools there
is no total `--wall_time` — so a stage costs (boards in) × (time each).
Measured: one 420 s beam emitted **425 boards**, and stage 3 was still on board
19 twenty minutes later, about **13 hours** for that one stage. The two caps cost
very differently: `TOP1` is ~25 s a board, `TOP2` ~250 s. Widen stage 3 freely;
widen stage 6 only if you have the hours.

**The ender's budget is per board, and it is a true wall clock.** An ender
stage costs (boards in) × `--board_time_limit` at worst -- every call it makes
for a board is inside that budget. It ends a board sooner only when the board
is solved, or -- with `--no-redive`, since a redive is random and never runs
out -- when every region of its plan is proven optimal.

**Budgets.** `--wall_time` covers the beamer's whole run, database build
included, so `BEAM` under about four minutes with no `DB` is spent entirely on
the build — the farm warns rather than letting you find out from a day of
`0 boards`. Set `DB` to an absolute path on node-local disk and the build happens
once. A slow disk is worse than none: measured at 5.9 MB/s the beamer sat in
`D` state at 98.8% iowait and never finished a config, where an in-memory build
ran fine.

**Unattended.** ortools is checked before iteration 1; `sh()` records every
tool's exit code, so a stage that dies does not discard what the stages before
it made; `GIVE_UP_AFTER` consecutive failures end the run (`0` never gives up);
files are written atomically; `SIGINT`/`SIGTERM` finish the current iteration and
exit 0. Per-iteration logs are kept only for records and failures, which is what
keeps a multi-day run to kilobytes.

## The jewel whirlpool: `run_whirlpool_jewels.sh`

The runner above carries a population of boards through its laps, and the
frame changes from lap to lap. This one follows a single beamer board s1
under **one fixed frame**, re-grows it from each of its other sides, and keeps
the boards that make it -- the jewels. The laps form a tree around s1:

```
s1  beamer board, rows 0..8, annealed bottom and left column
s2  lap 1: s1's left band (columns 0..K)       turned counter-clockwise, re-grown
s3  lap 2: s1's right band (columns 15-K..15)  turned clockwise, re-grown
s4  lap 3: s3's right band turned clockwise, or else s2's left band turned
           counter-clockwise: s1's top side is s4's bottom either way
```

s1..s4 hold the same complete 60-cell frame, and each of s1's four sides is
the bottom of one of them. So a jewel's interior was grown from all four sides
under one border, and none of its sides is a leftover of the beamer's early
guesses. s2 and s3 both lock a band of s1 itself, and s4 locks s3's band (or
s2's), which holds s1's top-right (top-left) corner. On the real jewels found,
s1 shared 46-53 interior cells with s2, 54-62 with s3 and 26-31 with s4.

```bash
bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db                  # an hour: all five clues, blocks frozen, K=4
bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db K=3              # 6 frozen jewels an hour on 4 cores (K=4: none in 45 min)
bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db WALL=600 DIVE=1  # 10 min, each jewel dived as it is found
bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db K=3 FREEZE=corners  # centre block re-grown
bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db BORDERS=random   # the beamer's sampled borders
bash pipeline/run_whirlpool_jewels.sh INPUT=boards.csv                 # whirl your own boards
```

**One band**, from a board to its child:

1. *Close.* `E555_backtracker --stop_column K --with_frame` writes every
   exact filling of the board's columns 0..K plus the frame (and, frozen, the
   five 2x2 clue blocks), up to `BANDS` per board; with `--reverse` it bands
   the right columns 15-K..15 instead. The other inner columns are cleared
   first and lend their pieces (the finalizer frees them anyway). The band
   closes a top corner block: s1's rows above `BEAM_STOP_ROW` (laps 1 and 2),
   or the rows above `FIN_STOP_ROW` under the fixed frame (lap 3). A board
   whose corner cannot close writes no band, so a lap selects the boards that
   can.
2. *Turn* (`E555_rotate.py`): counter-clockwise for a left band, clockwise for
   a right one. The band becomes rows 0..K.
3. *Re-grow* with `E555_finalizer --finalize_from K --keep_ring
   --no_fanout_sort` up to `FIN_STOP_ROW`, exhaustively (`--backtrack_row K`:
   a DFS from the lock, so no beam cuts an option). `--keep_ring` holds the
   frame in place. On 8 real K=4 bands the exhaustive search found the same
   20 boards as beams of width 20000 and 200000 (the narrower one was full on
   one band) in the same 24 s, nearly all of it the reduced database's rebuild
   (~3 s a band on 4 cores); the fan-out sort would add 7 s a band and, for an
   exhaustive search, only reorders the same boards (`--no_fanout_sort`).
   Every band of a call runs in its share of `LAP_TIME`, and each keeps
   `FIN_BOARDS`/bands of its boards.

**s1's two side bands** are cut separately and share only what a jewel must
share: the frame and, frozen, the clue blocks. Lap-1 bands are s1's left band
and the frame, closed against the annealed left column. Lap-2 bands are cut
from s1 with a lap-1 band's frame left of column K+1 and its blocks, and build
the frame right of it themselves: under a lap-1 band's whole frame no
top-right corner closed (0 of 435 real K=4 bands, 0 of 12 synthetic). A
lap-1 band and a lap-2 band of the same start therefore meet only on the
frame and the blocks, and inside each may use a piece the other uses (closing
both corners in one board worked on 0 of 11 real K=4 boards). Lap 1 grows a
lap-1 band under its lap-2 partner's frame, so that s2 and s3 carry one frame;
lap 2 grows the lap-2 bands whose partner re-grew; lap 3 cuts s3's right
band, or, when no s4 grows that way, the left band of the s2s that share a
start with an s3. The s1 written to `jewels.csv` is the beamer board with both
bands on it; a cell both bands took a piece for stays open.

Each lap picks `BANDS_GROWN x LAP_TRIES` of its bands, most different first
(`E555_rank.py --diverse`), and grows them `BANDS_GROWN` per finalizer call.
The search is depth-first: a call's boards feed the next lap, and the first
s4 ends the board. A band's id is the id of the board it was cut from plus
`.n`, and each lap's `parents.txt` maps a re-grown board to its band; that is
how the tree is traced from s4 back to s1.

**Borders.** Each produce call starts from a fresh border. `BORDERS=anneal`
(default) runs Stage A with `--double_decker DD_SIDES` (`ALL`),
`ANNEAL_RESTARTS` restarts in parallel, sorted by `Score_dd`. With
`CLUES=all` a 1 s beamer probe (`--lambda_corners`) reads, per clue
orientation, the exact catalog of top-left and top-right corner blocks the
border's sides allow around the corner clues; the border used is the best by
`Score_dd` whose two catalogs are both non-empty in some orientation (laps 1
and 2 close both corners of s1), and the beam runs one call per such
orientation (`--pin_clue`, with `--lambda_corners BEAM_CORNERS`), sharing
`BEAM_TIME` and `BEAM_BOARDS`. It sweeps all bottoms (`BEAM_BOTTOMS=0`,
sampled with `BEAM_TAU`) x `BEAM_COLUMNS` left columns, and spends the TOP
reserve last (`--lambda_reserve RESERVE_F`). A rotations file instead gives
its next row to each call, and `random` uses the beamer's `--random_edges`.
Every border used is appended to `RUN_DIR/borders.csv`, so a later run can
reuse them (`BORDERS=w1/borders.csv`).

**The annealed sides.** The bottom row, the whole left column (the top-left
corner included) and the right column up to `BEAM_STOP_ROW` keep the
annealed border's sides and the beamer's order. The side bands fill the top
row and the right column above the stop row from the edges left, and may
trade pieces between those two sides: the backtracker has no option to
restrict an edge to its side.

**Clues.** Every tool holds all five clues (`CLUES=all`, the default;
`center`, `none`), each on its own cell through every lap. A clued run keeps
its own database cache (`E555.db.center`, `E555.db.all`). **Frozen blocks**
(`FREEZE=1`, the default): the bands also fill the five 2x2 clue blocks and
every lap holds them (`--with_clue_regions`, `--freeze_clues`), so a jewel's
boards share the frame, the 20 block pieces and all five clues. Above a lock
at row K the rows beside the frozen centre block (rows 7-8) must wrap a fixed
2x2 block between the fixed ring ends, with 6-K free rows below it to adapt,
so a frozen lap stops lower the higher K is: `FIN_STOP_ROW=auto` is 11-K with
`FREEZE=1` (9 with `corners`, 10 unfrozen). With the earlier chain of laps
(below), frozen lap-1 bands of real boards reached row 9 on 0 of 10 boards at
K=4 and 0 of 18 at K=3, where K=2 gave 4 jewels from 8 boards.
`FREEZE=corners` holds only the four corner blocks and re-grows the centre
one every lap, its clue still on its cell.

**The beamer's settings** (measured with the earlier chain: beamer stop row
10, the probe asking for a top-left block only; one annealed border,
`CLUES=all`, frozen, 300 s beamer calls on 4 cores; frames with a lap-1 band,
of the frames written):

| beamer setting | K=4 | K=3 | K=2 |
|---|---|---|---|
| 16 columns, all orientations | 1 of 624 | 1 | 3 |
| 64 columns, tau 0.5 | 3 of 693 | 5 | 7 |
| 4 columns, tau 1 / `--bail_columns 8` | 0 of 544 / 0 of 599 | 0 / 0 | 1 / 1 |
| corner probe + pinned orientation, 16 columns | **6 of 879** | **15** | **28** |
| the same, 64 columns, tau 0.5 | 5 of 834 | 13 | 32 |

Keeping the annealed left column costs most of the yield: with it cleared
too, 44 of 628 frames got a K=4 band (194 at K=2), but then only 34% of the
left column's top four edges stayed on their side. The probe matters because
the annealer does not model the corner clues: on that border only one clue
orientation had a top-left block at all.

**The beamer's stop row.** s1's two side bands must close their corners
against s1's own rows, which the beamer filled with the pieces that fit them
best. Stopping it lower leaves the corners more pieces (real boards,
`CLUES=all`, frozen, cut back to the stop row; lap-2 bands cut as the runner
cuts them):

| K | stop row | boards | with a lap-1 band | with a lap-2 band |
|---|---|---|---|---|
| 4 | 10 | 11 | 10 | 0 |
| 4 | 9 | 11 | 9 | 1 (120 bands) |
| 4 | 8 | 11 | 9 | **4** (277) |
| 4 | 7 | 11 | 7 | 4 (9180) |
| 3 | 10 | 20 | 18 | 1 (56) |
| 3 | 8 | 20 | 17 | **12** (1112) |
| 4, unfrozen | 10 | 11 | 10 | 1 (60) |
| 4, unfrozen | 8 | 11 | 9 | 4 (240) |

So `BEAM_STOP_ROW=8`.

**Measured** (`CLUES=all`, frozen, 4 cores; `FIN_STOP_ROW=auto`: 8 at K=3, 7
at K=4). The first two rows whirl a fixed set of real beamer boards
(`LAP_TIME=240`, `LAP_TRIES=2`); the last two run end to end from fresh
annealed borders (the probe, `ANNEAL_RESTARTS=8`, `BEAM_TIME=300`, everything
else default). The earlier chain (below) ran the same boards and settings
with s1 to row 10:

| run | boards whirled | past lap 1 | past lap 2 | jewels | earlier chain |
|---|---|---|---|---|---|
| K=4, fixed boards | 11 | 1 | 0 | 0 | 11 / 4 / 0 / 0 |
| K=3, fixed boards | 20 | 5 | 1 | 1 | 20 / 12 / 7 / 5 |
| `K=3`, 60 min | 258 | 17 | 9 | **6** | 452 / 17 / 9 / 6 |
| defaults (K=4), 45 min | 633 | 6 | 0 | 0 | 753 / 8 / 3 / 1 |

Every jewel was checked: all placed edges matched, the five clues on their
cells, the frame and the blocks the same on s1..s4 under their turns. Every
s4 grew from s3; the s2 fallback never made a jewel. At K=3 the tree makes as
many jewels as the chain did, and each shares far more with s1: s1 and s4
share 26-31 interior cells, where the chain's s4 kept nothing of s1 but the
frame and the blocks. At K=4 lap 2 is the wall: of the 633 boards, 125 had a
lap-1 band, 74 of them no lap-2 band, and the 6 that passed lap 1 grew no s3.
Dived as `DIVE=1` dives them (`E555_diver --end_dive 10000 --end_polish
20000`, under a second a board on 4 cores, well inside `DIVE_TIME`), the six
K=3 jewels' s1 boards reached 430-439 and their s2..s4 449-454: the best of
each jewel 451-454.

`jewels.log` gives each jewel's frame key, `from=s3|s2` and
`shared=a/b/c/d`: the interior cells s1 shares with s2 and with s3, s4 with
its parent, and s1 with s4. On a big machine, run several copies, each with
its own `RUN_DIR`; every run draws its own seed. Several copies share one
`DB_FILE` through the page cache.

**What came before.** This runner's previous version (commit 6116fb9) made a
chain, s1 -> s2 -> s3 -> s4, each lap turning its parent counter-clockwise and
locking its left band. Consecutive boards shared 40-50 interior cells, but s3
kept only s1's K x K top-left corner and s4 nothing of s1 but the frame and
the blocks, and laps 2 and 3 each closed a corner against the fixed frame.
Before it, an earlier runner (campaign 20261028-0249 on the `agent_runs`
branch) turned clockwise and kept only the frame and a 12-cell block per lap:
about 90 jewels an hour on 4 vCPUs unclued, 15-30 with `CLUES=all` and frozen
blocks, whose s2..s4 dived to 455-461 without beating the best board. It had
measured keeping the parent's slab: on lap 1, 6 of 16 boards re-grew (against
14 of 16 for the small block); on lap 2 a 5-column kept block left a wedge
against the frame, and 0 of 16 bands re-grew, even exhaustively.

Before the first produce the script reads `DB_FILE` once into the page cache
(minutes on a slow disk, counted against `WALL`); from a cold disk the beamer's
memory-mapped reads can stall a produce call for many minutes.

## The halo: `run_halo.sh`

A loop until `TOTAL_TIME` is spent, looking for complete boards with at most
`BREAKS` (20) broken edges. `HALO` (2, the default, or 3) is the halo's width:
the beam leaves the right `HALO` columns open, and the ratchet lifts
`data/holes_halo<HALO>.csv`.

| step | tool | what it does |
|---|---|---|
| A | `E555_beamer --random_edges --backtrack_unlock_right HALO` | up to `SAMPLES` bottoms x `COLUMNS` left columns, a beam `BEAM_WIDTH` wide to `BACKTRACK_ROW` (8 for `HALO=2`, 10 for `HALO=3`), exhaustive to row 12: break-free blocks with the right band open, no extension. It stops once it has written `RATCHET_BOARDS` boards |
| B | `E555_backtracker --break_mode ratchet --order frontier` | every one of them: the mask lifts the halo (`HALO=2`: rows 0-1 and 14-15, columns 0-1 and 14-15, 50 cells of a block, 124 searched around a 12 x 12 core; `HALO=3`: three wide, 69 lifted, 156 searched around 10 x 10), and the ratchet refills it outward from the core within `BREAKS` breaks |
| C | `E555_diver --reopen auto`, `E555_rank.py` | each complete board polished for `POLISH_TIME` seconds (50000 dives and 50000 polish rounds per copy, 8 copies a round) into `complete.csv`; `best.csv` keeps the best boards of every ratchet, complete or not |

```bash
bash pipeline/run_halo.sh                                     # four hours, every core, HALO=2
bash pipeline/run_halo.sh HALO=3 RUN_DIR=halo3                # the three-wide halo
bash pipeline/run_halo.sh TOTAL_TIME=2d DB_FILE=/tmp/E555.db  # a shared database cache
```

The idea: the beam fills its first rows and the side columns long before the
pieces run short. Lifting them gives those pieces back, so one search can
rebuild the frame, the band and the top together around a core the beam built
well.

- **The ratchet.** An exact search outward from the core breaks where it is
  stuck, each break kept for good. It goes on until the board is complete,
  `BREAKS` breaks are spent, or its cuts prove the rest cannot be finished
  within them. It ends on its own, so no time ladder is needed. Under its node
  cap (`RATCHET_NODES`) a board's result is the same at any thread count.
- **The beam writes what the ratchet takes.** Its `--max_emitted` is
  `RATCHET_BOARDS`, so it stops after the configurations that fill it, and
  `SAMPLES` x `COLUMNS` is only an upper bound. The boards of a configuration
  come root by root in the beam's rank order. The backtracker skips repeats
  after the lift.
- **The polish.** One diver call per complete board, so each board gets its
  own clock. It never returns a worse board. It is off under `CLUES=all`,
  because an auto band can lift a corner clue.
- **Rounds.** The beam and the ratchet stop cleanly before `TOTAL_TIME` ends,
  and Ctrl-C does the same. A rerun with the same `RUN_DIR` goes on with its
  round count, `complete.csv` and `best.csv`.
- **The database.** Each round reads `DB_FILE` before its beam: ~1 s when it
  is in the page cache, ~20 s when it is not. Out of the cache, the beamer's
  memory-mapped lookups wait on the disk, and one configuration took 6 minutes
  instead of 8 s.

**Measured** (4 threads, `CLUES=center`, width 10000). Boards the beam writes
per configuration, at stop row 12 (~8 s a configuration):

| `HALO`, backtrack row | boards |
|---|---|
| 2, 8 | ~320 (217-419) |
| 2, 9 | ~20 |
| 2, 10 or 11 | 0-1 |
| 3, 9 | ~7000 |
| 3, 10 | ~80 (20-195) |
| 3, 11 | 0 |

At stop row 11 (`HALO=3`), backtrack row 10 wrote ~1000 boards a configuration
and row 9 36k-91k.

The frontier ratchet, 1e6 nodes, on ~34 boards spread over each beam's file:

| `HALO`, stop row, backtrack row | `--breaks 20`: placed of 256 (mean) | s a board, one thread | `--breaks 64`: complete at (mean) | s a board |
|---|---|---|---|---|
| 2, 12, 8 | 167-201 (180) | 6.6 | 435-444 (439.3) | 9.0 |
| 2, 12, 9 | 170-225 (184) | 6.9 | 434-446 (438.8) | 8.8 |
| 3, 12, 10 | 169-190 (180) | 6.9 | 431-442 (436.7) | 9.1 |
| 3, 12, 9 | 174-188 (181) | 5.5 | 432-441 (436.2) | 7.5 |
| 3, 11, 10 | 172-203 (183) | 4.3 | 435-442 (437.8) | 6.1 |

- **No board completes within 20 breaks.** Every one stops at "no candidate
  passes the cuts", its cuts having proved that the rest cannot finish within
  the breaks left. A complete board within `BREAKS` is a rare find; `best.csv`
  shows how close a run gets.
- **`HALO=2` is the default.** It completes about 2.6 edges better than
  `HALO=3`, and its beam fills a round from fewer configurations.
- **The lift costs edges.** At stop row 11 (`HALO=3`, 1e5 nodes), the same
  boards finished on their open band alone reached 442.5 with the ratchet
  against 436.4 lifted, and 451.9 against 442.4 with diver dives and polish
  (`--end_dive 10000 --end_polish 5000`, 8 boards).
- **No screen.** Dive scores do not rank these boards. Two runs of one screen
  (backtracker stuck dives or diver dives, 300-1000 a board) agree at Spearman
  +0.02 to +0.12. The median of a board's stuck dives repeats (+0.57 to +0.72)
  but does not predict the ratchet (+0.14, +0.05).
- The ratchet writes byte-identical boards at 1 and 4 threads.
- A round at the defaults on 4 threads:
  - `HALO=2`: the beam 26-30 s (2 configurations, ~400 boards), the ratchet
    9.6 min for 402 boards; deepest 229-230 of 256 placed, best score 409.
  - `HALO=3`: the beam 49-61 s (5-6 configurations), the ratchet 8.1 min for
    409 boards; deepest 189-192.
  - Before the beam was sized to the ratchet (stop row 11), it wrote 16k-21k
    boards a round in 2-3.5 min, of which the ratchet used 400.

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
bash pipeline/topper_sweep.sh INPUT=round_out/strip.csv PRESET=closeB
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
  file, `$DB.center` and `$DB.all` (`run_farm.py`) or `$DB_FILE.center` and
  `$DB_FILE.all` (the bash farm), so switching `CLUES` between runs does not
  throw the other cache away. The other runners take the path as you give it,
  so name it yourself if you toggle clues with one of those.
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
