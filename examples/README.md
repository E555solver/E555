# E555 examples

One script per tool, smallest first, then one that runs the whole chain. Each
runs with no arguments, is short enough to read in a minute, and keeps every
setting in a plain block at the top. The reference material that used to fill
their headers lives here instead, so the scripts stay editable and this file
stays readable.

**These are for learning the tools.** For long unattended runs -- the whole
pipeline, the whirlpool and the board farm -- see [`../pipeline/`](../pipeline/).

## Start here

```bash
make                                     # builds the four binaries
bash examples/01_beamer_quickstart.sh    # ~5 minutes, needs ~8 GB RAM
cd ~/runs && bash ~/E555/examples/07_barebones_chain.sh   # all four stages
```

## How every script works

`01` to `06` share one shape; `07` is deliberately the other extreme and is
described [in its own section](#07----the-whole-chain-barebones). One is for
changing settings, the other for reading commands.

**Settings are plain assignments.** Open the file, change the number, save. That
is the intended way to use them: copy a script into a folder of your own and let
the copy be the record of what you ran.

```bash
SEED=data/seed_Edge5.txt
OUT_DIR=final_out
FROM=7
```

**Overriding without editing is optional.** Any `NAME=value` argument is applied
over the block, so no arguments at all means the file's own values:

```bash
bash examples/02_finalizer_regrow.sh
bash examples/02_finalizer_regrow.sh OUT_DIR=run7 THREADS=16
bash examples/02_finalizer_regrow.sh BOARDS=beam_out/beam_completions_random_10.csv
```

**`REPO` says where the checkout is.** It defaults to the script's own parent,
so a fresh clone runs untouched. If you copy a script somewhere else, set `REPO`
at the top -- the script says so, by name, instead of failing obscurely:

```
REPO=/home/you/experiments is not an E555 checkout -- set REPO at the top
```

Every path below `REPO` in the settings block is relative to it. `OUT_DIR` is
too, unless you give an absolute path.

**They tell you where the output went.** Each tool writes `outputs.txt` in its
output directory listing the files it actually filled -- one path per line,
empty when it emitted nothing. No script guesses a filename, and neither should
you:

```bash
cat final_out/outputs.txt
python3 tools/E555_rank.py $(head -1 final_out/outputs.txt) --seed_file data/seed_Edge5.txt --top 10
```

**They print the command they ran.** Every tool call passes `--print_cmd`, so
the log carries a `[cmd]` line with every flag populated from its effective
value. Copy that line and you have the run, without this script in the middle.

## The scripts

| script | tool | what it teaches | needs |
|---|---|---|---|
| `01_beamer_quickstart.sh` | beamer (+ annealer) | Stage B from nothing: sample a border, grow the board row by row. `ANNEAL=1` runs Stage A first, so the borders are searched for rather than sampled | 8 GB RAM, ~5 min |
| `02_finalizer_regrow.sh` | finalizer | free the top rows of a board and re-grow them from a reduced database | seconds to minutes |
| `03_roundhouse_strip.sh` | roundhouse | rotate the board, refill a border strip; can prove a board dead in milliseconds | megabytes |
| `04_stage_c_close.sh` | topper + ender | the whole CP-SAT tail in the documented order: herd the breaks onto a band, sweep the ring, then patch what is left | `pip install ortools` |
| `05_backtracker_dives.sh` | backtracker | greedy dives to triage, exhaustive DFS to prove | minutes to overnight |
| `06_roundhouse_both_ways.sh` | roundhouse | chain two roundhouse passes per board, once each way round, so the two spirals cover all four sides | seconds to minutes |
| `07_barebones_chain.sh` | all four | the whole chain in six calls, no arguments and no indirection: what the tools are actually invoked with | 8 GB RAM, ~15 min |

## They all speak the same CSV

Every tool reads and writes the same canonical board row
(`config_id, score, pos[256], rot[256]`), so any output feeds any input --
including a tool's own output. That is what makes iteration possible:

```bash
bash examples/01_beamer_quickstart.sh
bash examples/02_finalizer_regrow.sh BOARDS=beam_out/beam_completions_random_10.csv
bash examples/04_stage_c_close.sh    BOARDS=final_out/beam_completions_finalized_12.csv
bash examples/05_backtracker_dives.sh BOARDS=stage_c_out/3_patched.csv
```

Between any two steps, look at what you have:

```bash
python3 tools/E555_rank.py   FILE --seed_file data/seed_Edge5.txt --top 10
python3 tools/E555_rank.py   FILE --seed_file data/seed_Edge5.txt --top 10 --no_id
python3 tools/E555_rank.py   FILE --count            # how many boards
python3 tools/E555_rank.py   FILE --field score      # the best board's score
python3 tools/E555_viewer.py FILE --seed_file data/seed_Edge5.txt
```

`--no_id` drops the board-id column, which is the widest one and the usual
reason the table wraps. `rank.py` is the one to trust: it recomputes the score
from the seed -- **field 2 of a Stage B row is a solution index, not a score** --
and it reports *where* the breaks are. Eighteen breaks spread over seven rows is
a mess; the same eighteen packed into rows 14-15 is nearly finished.

---

## 01 -- the beamer, and the borders it grows from

`ANNEAL=1` runs Stage A first. The annealer searches for borders whose sides
have many ways to be continued, and the beamer grows from those instead of from
sampled ones. Slower to start, much better material.

The script anneals **four times** as many borders as the beamer will use and
keeps the best quarter with `E555_sort_rotations.py --top`: Stage A is cheap and
the beam is not, so it pays to be picky.

**`STEPS` has a floor of 250000.** Below it the annealer is not merely weaker --
it often fails to place a legal border at all. Measured on the real seed: 8
restarts x 3000 steps found 2 feasible borders, and 2 x 2000 returned one border
on one run and none on the next, while *every* restart at 250000 steps succeeded
(2/2, 4/4, 8/8). The scripts warn below the floor rather than refusing, because
a deliberately tiny smoke run is a legitimate thing to ask for.

The weights say what a good border is. Without `--target_scale` a weight is a
maximize/minimize sign rather than a per-side target, which is what lets
`--w_bottom 0` mean "ignore the bottom". The two settings are coupled: putting
`--target_scale` back makes every weight a target that must be positive, and
`--w_bottom 0` stops meaning anything.

**Reproducibility.** The beam is reproducible from `--rng_seed` together with
`--threads`, not from the seed alone. The work partition follows the thread
count, and a beam that keeps a bounded number of candidates keeps a different
subset from a different partition. Record the thread count with the seed.

---

## 02 -- the finalizer, and how far down to lock

`FROM` is the one knob that matters. Rows `0..FROM` stay put; everything above
goes back in the pool. Lower `FROM` = more rows re-searched = deeper resampling
and much slower. Do not set it just below the input's top row: that asks the
search to redo the exact row that already failed, with the same pieces.

**The useful range depends on whether the board's border is complete**, and the
two cases pull opposite ways.

With a **complete** border -- an ordinary beamer partial -- the left column is
fixed and `--top_columns` samples orderings, so lower is better until the beam
stops filling. Measured on `board_partial_row12.csv` with 12 sampled columns:

| `--finalize_from` | configurations reaching row 11 | beam occupancy |
|---|---|---|
| 4 | 8 of 12 | 100% of cap at rows 6-9 |
| 5 | 6 of 12 | 31-48% |
| 6 | 0 of 12 | under 1% |
| 7, 8 | 0 of 12 | 0% |

Under 1% occupancy is an exhaustive walk wearing a beam's clothes, which is why
the tool's default is `5`.

With an **incomplete** border the finalizer falls back to `--free_edges`, and
`--top_columns 0` then enumerates *every* legal left column. That enumeration
grows explosively as rows are freed, so the same board wants a **higher** lock.
Measured on the shipped `board_partial_row12.csv`, `MAX_WALL=600`:

| `FROM` | columns enumerated | boards | wall |
|---|---|---|---|
| 7 | 3512 | 6 | 6.2 s |
| 6 | 40098 | 6 | 50.2 s |
| 5 | 8606 | 0 | budget exhausted |
| 4 | 127 | 0 | budget exhausted |

That is why the example ships `FROM=7` while the tool defaults to `5`. Raise
`MAX_WALL` before lowering `FROM`.

---

## 03 -- the roundhouse, and which band it tears up

It rotates the board 90 degrees and grows a `WIDTH`-wide vertical **strip**
instead of a row, so every level is one chain lookup and the frontier is only
`WIDTH` colours wide. Two things follow: it needs just the edge half of the
chain database (megabytes, seconds -- no 6.4 GB build, no `--db_file`), and the
relaxed problem is small enough to solve exactly, so the tool knows which
colourings can still finish the strip **before** trying any piece.

The search is **exhaustive and deterministic**: no beam, no sampling, no random
seed. Finishing without a complete board is therefore a proof that none exists
for this cut -- unless a budget stopped it first, which the run says.

### WIDTH is the dial that matters

`WIDTH=0` picks the narrowest width whose kept region is complete and
break-free -- on a board filled in whole rows that is 16 minus the filled rows.
A higher `WIDTH` frees already-solved rows on purpose:

| WIDTH | pieces kept at ROUNDS = 1 / 2 / 3 | needs rows filled |
|---|---|---|
| 3 | 208 / 169 / 130 | 0..12 |
| 4 | 192 / 144 / 96 | 0..11 |
| 5 | 176 / 121 / 66 | 0..10 |

### ROUNDS

Frees -- and refills -- that many `WIDTH`-wide bands: right, then top, then
left. The cuts nest, so a lower `ROUNDS` is a cheaper experiment, not a
truncated one. Work upward: 1, then 2, then 3.

**Cost.** Every cut keeping 96 pieces or more exhausts in seconds. The one wide
cut is `ROUNDS=3 WIDTH=5` (a 66-piece core): round 1 alone has hundreds of
thousands of break-free refills, so that one ends on `MAX_WALL` and the summary
marks it TRUNCATED rather than proved.

### ROTATE

Picks which side of the **original** board each round attacks. Only the *kept*
region is validated, so aim the strip at where the breaks and holes are and they
are simply freed. Negative turns the other way (`-1 == 3`):

| ROTATE | round 1 | round 2 | round 3 | ROUNDS=1 leaves its hole at |
|---|---|---|---|---|
| 0 | right | top | left | top-right |
| 1 | top | left | bottom | top-left |
| 2 | left | bottom | right | bottom-left |
| 3 / -1 | bottom | right | top | bottom-right |

`ROTATE=1` (the default) attacks a Stage B partial's unsolved top first, while
the piece pool is rich. `ROTATE=-1` attacks it last but re-cuts the bottom band
first -- the band the pipeline fixes by random sampling at row 0 and never
revisits.

### What you get back

**One board per input board**: the furthest the search got, measured in pieces
placed. Tagged `s` when it is complete and break-free (the puzzle solved) and
`d` otherwise. `TIES=N` widens that to N boards that reached the same depth.

`BREAKS=B` then greedily fills the rest of that deepest board, spending at most
B mismatches, and emits the complete result tagged `f`. That is a dive, not a
search: it does not backtrack and B is not proved minimal. It exists so Stage C
gets a full board to attack break by break instead of a hole. On the real seed,
expect roughly one break per two cells it has to fill.

---

## 04 -- Stage C, and why the three passes are one script

They only make sense together: the topper deliberately **piles** breaks onto a
border band, and the ender's two modes are what un-pile them.

1. **topper** opens a band along one border and re-solves it under a strictly
   lexicographic objective: fewest broken edges, then push the unavoidable ones
   to the nearest horizontal border, then slide them along it toward a corner.
   Measuring to the *nearest* border keeps the worst trip at 7+7 cells instead
   of 15+15 and lets all four corners share the load.
2. **ender ring** opens every cell within `REACH` BFS layers of a break plus the
   whole 60-cell border ring. A border break heals by an avalanche cascading
   around the frame, so this comes right after the topper piled them there.
3. **ender patch** opens a box around what is left and, besides minimising
   breaks, **compacts** them. The finishing pass.

Neither ender pass can return a board worse than its input, so running the chain
is always safe. `SKIP_TOPPER=1` starts at the ring sweep -- right when the
damage is already on the border, e.g. after a roundhouse run.

**The topper's two knobs.** `SIDE` is which band opens: `T B L R`, or the
L-shaped pairs `TR TL TB`. Open **only** where the breaks are: a pass over a
clean side wastes time and can spread a break into a clean row. `WORK_ROWS` is
how deep the band is -- deeper is stronger and much slower.

`HOLES` replaces the band with a 16x16 0/1 mask and the tool opens exactly those
cells; `SIDE` and `WORK_ROWS` stop applying. That is the only way to reach a
ragged region around a cluster of breaks, or the interior, which no band can
express. The ender and the backtracker read the same masks, so one mask drives
all three.

**Always re-score the output.** The topper's live `[inc] breaks=N` telemetry
counts only junctions touching the open band, so it is not the board score. The
script prints a rank table at each end.

---

## 05 -- the backtracker: triage or proof

The two modes are different engines, and the distinction matters more than any
other setting.

**`MODE=stuck` (default) -- greedy dives, for triage.** Each dive takes an exact
fit where one exists and a minimal break where none does, never backtracks, and
therefore always reaches 256 pieces. ~10k complete boards per second. Cheap
enough to run over a whole batch to see which partials deserve a long run. It
**proves nothing**: it never establishes that a board cannot be completed with
fewer breaks.

**`MODE=any` (or `lds`) -- exhaustive, for proof.** Iterative deepening over the
break count. An exhausted level is a theorem: no completion exists with that few
broken edges. Cost per level grows roughly exponentially -- run it overnight, on
the few boards triage picked out.

A **complete** board has no empty cell, so dives on it are a no-op. Give `HOLES`
a mask to reopen a region (`data/holes_open_border_*.csv`), or feed a partial.

**Expect the default run to score worse than its input.** Reopening the ring of
a well-optimized board and diving greedily lands around 28 breaks on a board
that came in with 18 -- documented behaviour, not a bug. Dives are for ranking a
hundred rough partials cheaply, not for improving a good one. Use `MODE=any`
when you want the board to actually get better, or proved.

`ORDER=mrv` (most-constrained cell first) is the right default for both modes.

The run writes up to five CSVs -- the best per record plus four sidecars named
after `OUT`. They are all listed in `$OUT.outputs.txt`.

---

## 06 -- does the spiral direction matter?

Each input board is run through two chains of two roundhouse passes: forward
then `--reverse`, and `--reverse` then forward. Two passes each way cover all
four sides. Both chains start from the same board, so the scores are comparable
and the tally at the end answers the question. `HOLD=1` makes the second pass
keep what the first left standing in the band instead of re-cutting it.

Each board is tagged with the pass that made it (`..._a1`, `..._b2`), so
provenance survives when the four passes are `cat`-ed into one file.

---

## 07 -- the whole chain, barebones

The other six scripts each teach one tool and share a skeleton: a settings
block, optional `NAME=value` overrides, `outputs.txt` reading, guard clauses.
`07` is the opposite extreme -- six calls, no arguments, no arrays, and one
guard in the whole file. Every flag is a literal in the line that uses it, so
any single call can be copied out of the file and pasted into a terminal
unchanged.

```bash
cd ~/runs && bash ~/E555/examples/07_barebones_chain.sh
```

**Output goes to the directory you run it from.** It is the one script that does
not `cd` into the checkout: `REPO` prefixes `bin/`, `data/` and `tools/`, and
every `--out_dir` is a bare relative path. Run it from a scratch folder and the
whole run stays there. `THREADS` and `PREFIX` are the only other variables.

**Flags absent from the file are the tool's tuned defaults**, not oversights.
`--beam_width` is the one to notice: the beamer and the finalizer both default
to 250000, and writing that number into an example would pin a stale value the
day it is retuned. `--print_cmd` reports every effective value at startup, so
leaving a flag out hides nothing.

**`--num_rows 0` means "every line of the input", and it is also the default.**
Stages 2 and 3 read whatever the stage before them produced, and that count is
not known when the script is written. The finalizer and the roundhouse both
take 0 as "from `--start_row` to the end of the file" and resolve it before the
banner, so `[cfg]` and `--print_cmd` report the real number rather than the
sentinel -- so does the beamer, in its fixed-border mode. The beamer's
`--random_edges` mode reads no file at all, so it counts random bottoms with a
separate flag, `--samples`, instead.

**`--max_emitted 25` counts both files.** The beamer's help is explicit that
the budget covers "both completions and `--incomplete_top` partials", so 25 is
25 boards of either kind. It is a stop condition, not a quota: the beam in
flight is always emitted in full, an overshoot of up to one beam width.

**Row 12 is the wall, and `--incomplete_top` is the only way through it.** No
configuration of any tool tested here has ever produced a *complete* row 12 --
every beamer and finalizer run ends `reached stop_row: 0`. What comes out
instead are boards that reach row 12 with 11 of its 16 pieces, banked while the
row dies, written to a separate `_partial.csv`. The five missing cells are work
the backtracker was doing anyway for rows 13..15.

**Why the beamer goes straight to row 12.** An earlier version grew to row 11
and had the finalizer carry it to 12. Given 880 s each to produce row-12
material:

| | row-12 boards | best after backtracker |
|---|---|---|
| beamer to row 11, then finalizer to row 12 | 13 | 454/480 |
| **beamer straight to row 12** | **29** | **457/480** |

Every one of the direct beamer's top five (457, 456, 456, 455, 455) beat the
two-stage best, and it passed 13 boards after six configurations -- about 262 s
including the database build, against 880 s.

**What the finalizer is for.** It takes a second, independent run at the same
boards, and it carries `--incomplete_top` for the same reason the beamer does.
Without that flag it reports only a complete row 12, which nothing has ever
produced, and it contributed exactly **zero** boards across two measured runs.
With it, it contributed **20 of the tail's 41** in 201 s. Note that at
`--finalize_from 4` it keeps only rows 0..4 of its input, so it does not build on
the beamer's row-12 work -- it re-derives from the same bottoms. That
independence is the point. `--finalize_from 11` would instead attack the five
missing cells directly, and is untested.

**Stage 3, the roundhouse, is conditional and usually sits out.** Only a
*complete* row 12 is worth spiralling, and nothing has yet produced one. The two
completions files are literally zero bytes when their stage emitted nothing --
the CSV header is written with the first board, not at open -- so `[ -s ]` on
their concatenation is an exact test rather than a heuristic. Give stage 1 a much
longer `--wall_time` and this stage starts working. `shopt -s nullglob` at the
top of the script is what lets the final concatenation succeed when the
roundhouse never created an output directory. `--clue_center` is *not* passed
here, unlike the two stages above: the roundhouse never frees the centre cell,
so the flag could only verify what is already true. `--strip_width 5` is passed
even though 5 is the default, because it is the dial worth turning -- it sets
how wide a band each round tears up, 2 to 5, and three rounds at width 5 keep
only the 66-piece core.

**Three limits bound stage 1, and the clock is the one that should win.**
`--wall_time 600`, `--max_emitted 25` and `--samples 50` are all live;
whichever comes first stops the run. Yield is 0.7 to 1.2 boards per
configuration at about 30 s each, so at these numbers the clock fires: one
measured run stopped at 600.2 s with 21 boards from 18 bottoms, under both other
limits. An earlier version set `--samples 30` and the bottoms ran out
first, at 22 boards, with the board limit never firing at all -- a bound you
cannot predict is not much of a bound.

**Where the lock goes, measured.** Four finalizer settings, 150 s each on one set
of 277 row-11 boards, back when the finalizer was the stage reaching row 12:

| `--finalize_from` | input lines seen | row-12 partials |
|---|---|---|
| 2 | 2 | 9 |
| **4** | **19** | **15** |
| 7 | 95 | 0 |

A higher lock races through lines and returns nothing, because from row 7 the
beam never widens enough to reach row 12 at all. A lower lock spends 75 s a line
and sees 2 boards of 277.

**`--breaks 20` is a filter, not a promise.** Closing the gap in row 12
plus rows 13..15 costs about 24-30 broken edges, so nothing passes a ceiling of
20. In stuck mode that changes nothing about the output: every dive completes,
the best board per input record is written regardless, and the ranking reads
those.

**Expect the yield to swing, and do not tune on one run.** Two runs of the
finalizer differing only in `--rng_seed` returned 15 partials and 1. That spread
swamps every parameter effect, so settings were chosen at a *matched* seed,
where `--finalize_repeats` earns its place:

| repeats | `--frac_rand` | configurations searched | partials |
|---|---|---|---|
| 1 | 0.30 | 70 | 1 |
| 1 | 0.50 | 73 | 1 |
| **3** | **0.30** | **174** | **3** |
| 3 | 0.75 | 173 | 3 |

Three passes spend the same wall budget on 174 configurations where one pass
reaches 70, and partials scale with configurations searched. `--frac_rand` moves
nothing, which fits the source's own note that its 0.30 default already assumes
repeated passes over one board. The board ids carry the pass index --
`p<line>r<repeat>l<column>` -- so the passes can be counted rather than assumed.

**`--clue_center` on the finalizer is load-bearing**, not decoration. Below
`--finalize_from 7` the centre cell is freed along with everything else above
the lock, and without the flag the beam quietly refills it with another piece.
`--free_edges` is the mirror image: it is never passed, because a beamer board
leaves the top border unplaced and the finalizer turns it on by itself.

Only `${PREFIX}_ranked.csv` survives; the beamer and finalizer directories and
the backtracker's CSVs are deleted once the ranking is written. Change `PREFIX`
between runs -- the C tools append to their CSVs rather than truncating them.

**Two dials set the run length**, since everything else is a default:
`--wall_time` on the beamer, which is two thirds of the total and decides
how many boards the rest of the chain is handed, and `--time_limit` on the
backtracker, which is spent once per board it receives. They are coupled --
doubling the first roughly doubles the fourth stage too. One measured
end-to-end run, four cores:

| stage | wall | out |
|---|---|---|
| beamer | 600 s (88 s of it the database), 18 configurations | 21 row-12 partials, 0 complete |
| finalizer | 201 s | 20 row-12 partials, 0 complete |
| roundhouse | 0 s | skipped: no complete row 12 to spiral |
| backtracker | ~100 s | 41 full boards |
| rank + view | seconds | best **455/480**, 25 breaks in 5 rows, 218/256 solid |
| **total** | **914 s** | `bb1_ranked.csv`, 41 rows |

Four idle cores; proportionally less on more threads. Stage 1 is two thirds of
it and is capped by its own clock, so `--wall_time` there is the dial that
moves the total. Measure on an idle machine: one stray solver left running from
an earlier experiment doubled every figure in an earlier version of this table.

Four chain shapes have now been measured end to end -- beamer to row 10, to row
11, to row 12 with a finalizer behind it, and this one. Every one scored between
454 and 457, which is inside the run-to-run noise, so none has separated itself
on board quality. What does differ is throughput: this shape produces 41 boards
in 914 s where the previous produced 22 in 1257.

The first edit worth making is a different one: add `--db_file chain.db` to the
beamer and later runs mmap the 6.4 GB chain database in seconds instead of
rebuilding it in memory every time.

---

## Fixing the Eternity II clue pieces

Scripts 01 to 04 take **`CLUES=1`**, which passes `--clue_center --clue_corners`
to every tool in that script. It is off by default and changes nothing when
unset. Script 07 is the exception: it holds the centre clue unconditionally,
because its `--finalize_from 4` frees that cell and the flag is what stops the
beam refilling it with something else.

```bash
bash examples/01_beamer_quickstart.sh CLUES=1
bash examples/02_finalizer_regrow.sh  CLUES=1 BOARDS=beam_out/beam_completions_random_10.csv
```

Set it on **every** stage or on none. A clue held by one stage and dropped by
the next is no better than never holding it, which is exactly what used to
happen: script 02 locks rows 0..7 and re-grows everything above, so without the
flag it frees the centre clue's cell and quietly refills it. The `clues` column
of `E555_rank.py` counts how many of the five a board still has:

```bash
python3 tools/E555_rank.py FILE --sort clues,score --no_id
```

## When a stage stalls, turn the board

Every stage here is direction-biased -- the finalizer frees rows from the top,
the roundhouse grows a strip against one border, the backtracker starts from a
fixed corner -- so a quarter-turn gives the same breaks to a different one. The
turn is lossless and re-scored to prove it:

```bash
python3 tools/E555_rotate.py FILE 1 --seed_file data/seed_Edge5.txt   # -> FILE_rot1.csv
```

## Three things that look like failures and are not

- **A configuration goes extinct and no board is emitted.** An empty candidate
  pool is an exact proof that that border is dead below the current row. The
  search is working: it abandons the configuration and plays another hand.
- **The roundhouse emits nothing and reports `REFUTED`.** Its oracle refuted
  every possible start before trying a single piece, and it ignores the piece
  supply entirely -- so no arrangement of any pieces can fill that band. That is
  a theorem about your board, delivered in milliseconds.
- **A roundhouse board scores far below the one you fed in.** Its output is
  break-free by construction; the score is 480 minus the junctions its *empty*
  cells leave open. A 191-piece board scoring 350 has no mismatch at all.

## Slurm

The scripts are plain bash and know nothing about schedulers. One wrapper runs
any of them as a batch job:

```bash
sbatch pipeline/slurm_wrapper.sh examples/02_finalizer_regrow.sh
sbatch --cpus-per-task=32 --mem=64G pipeline/slurm_wrapper.sh \
       examples/01_beamer_quickstart.sh THREADS=32 ANNEAL=1
```

Anything passed to `sbatch` overrides the wrapper's own defaults.
