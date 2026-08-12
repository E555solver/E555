# E555 pipeline

Long, unattended runs. **These are not examples** -- they chain every stage
together and are meant to be started and left alone for hours or days. If you
have not run the tools individually yet, start with
[`../examples/`](../examples/) instead; it takes about ten minutes and the
settings here will make far more sense afterwards.

## The three runners

| script | what it is | typical run |
|---|---|---|
| `run_pipeline_random.sh` | the whole pipeline on fresh random borders, no Stage A. **Start here.** | 30-60 min per pass |
| `run_pipeline_annealed.sh` | the same, but Stage A picks the borders first | +minutes for Stage A |
| `run_board_farm.sh` | runs a pipeline pass in a loop forever, keeps only the best boards, and periodically re-attacks them | days |

```bash
bash pipeline/run_pipeline_random.sh          # one pass, ~30-60 min
MAX_HOURS=48 bash pipeline/run_board_farm.sh  # the farm, two days
```

### random or annealed?

Stage A sizes the four border sides so the beamer's sweep actually covers its
own space -- worth real time when you intend to drill a *few* borders hard.
The random pipeline does the opposite: many hands, fresh borders, no
guarantees per border. For a farm that runs for days, random wins on throughput;
for a targeted attack on one promising border, anneal first.

### The farm, in one paragraph

Produce, then improve, then repeat. Four iterations grow brand-new boards from
random borders; the fifth stops producing and *resamples* the current champions
instead -- freeing five rows with the finalizer and refilling a border strip
with the roundhouse, alternating the rotation so the region a strip never
touches moves around the board. Everything worth keeping ends up in
`champions.csv` (best first, deduplicated, capped at `KEEP`), every new record
is copied to `records/` and never deleted, and `farm.log` gets one line per
iteration. Ctrl-C is safe at any point.

The single most valuable setting is `DB_FILE`: without it, every iteration
spends 2-3 minutes rebuilding the same 6.4 GB chain database. It defaults to
`farm/chain.db` and needs about 6.5 GB of free disk.

## The topper sweep

One script, `topper_sweep.sh`, drives `E555_topper.py` through a list of passes.
A pass is written `SIDE:WINDOW:LOCKED` -- which border band opens, how deep it
is, and how many of its outermost rows to **unset and hold empty**. A `PLAN` is
a space-separated list of them, and `PRESET` names the four that used to be
separate scripts:

| `PRESET` | plan | when |
|---|---|---|
| `safe` | `T:5:0`, then `TR TL TB R L B` at `3:0` | a board filled to row 11. Nothing is ever unset, so no pass can make the board worse. Start here |
| `window` | `T:8:3 T:6:2 T:5:1 T:4:0`, then `TR:4:0 L:4:0` | a window sliding up the board, then two clean-ups on borders it never reached; what `run_pipeline_annealed.sh` stage 5 runs |
| `deep` | every side as a pair -- a wide pass with its outer band emptied, then a narrow pass that refills it | when the safe sweep has run out of moves and you will spend breaks to buy freedom |
| `closeT` `closeB` `closeR` `closeL` | `X:6:2 X:4:0` on the named side | closing a hole that sits against one border -- an `E555_roundhouse` `miss0` board, or any partial whose breaks are all on one side. `close` on its own means `closeT` |

```bash
INPUT=boards.csv PRESET=safe bash pipeline/topper_sweep.sh
INPUT=boards.csv PRESET=closeT bash pipeline/topper_sweep.sh
INPUT=boards.csv PLAN="T:8:3 T:5:1 T:4:0" MAX_TIME=3600 bash pipeline/topper_sweep.sh
```

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
INPUT=round_out/roundhouse_round1_rot1_W5_miss0.csv PRESET=closeB bash pipeline/topper_sweep.sh
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

All four runners take **`CLUES=1`**, which passes `--clue_center --clue_corners`
to every stage that supports them. It is off by default and changes nothing when
unset.

```bash
CLUES=1 bash pipeline/run_pipeline_random.sh
CLUES=1 INPUT=board.csv PRESET=window bash pipeline/topper_sweep.sh
```

It is one switch per run, not per stage, on purpose: a clue held by the beamer
and then dropped by the finalizer is no better than never holding it, and that
is exactly what happened before -- `FIN_FROM` is `BEAM_STOP_ROW - 5`, so the
finalizer frees the centre clue's row on every run. The farm passes `CLUES`
down to the production pipeline it drives.

Two consequences worth knowing:

* **The chain database differs.** A clued run excludes the clue pieces from the
  chains, so its cache is not interchangeable with a normal one -- the beamer
  refuses a cache whose exclusion set does not match. The runners therefore use
  `$DB_FILE.clue` when `CLUES=1`, so the two coexist and neither is rebuilt on
  every toggle.
* **The backtracker stage is not clue-aware.** It only moves pieces in cells
  named by `$HOLES`, but the shipped masks do free clue cells
  (`holes_open_border_TR.csv`, the default, frees the one at row 13 col 13), so
  the last stage can still displace a clue. Check with the ranker's `clues`
  column, which counts how many of the five a board still holds:

```bash
python3 tools/E555_rank.py pipeline_out/FINAL_best.csv --sort clues,score --no-id
```

## Reading the output

Every stage writes the same canonical CSV, so any file here can be fed back into
any tool -- including the one that produced it.

```bash
python3 tools/E555_rank.py pipeline_out/FINAL_best.csv --seed data/seed_Edge5.txt
python3 tools/E555_viewer.py pipeline_out/FINAL_best.csv --seed data/seed_Edge5.txt
```

A stage that emits nothing is usually telling you something true about the
board, not failing: the beamer going extinct proves that border is dead below
the current row, and the roundhouse refusing every strip start is a proof about
that board delivered in milliseconds. The runners detect both, say so, and carry
the previous stage's boards forward.
