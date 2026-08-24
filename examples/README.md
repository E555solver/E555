# E555 examples

One script per tool, smallest first. Each is short enough to read in a minute,
every setting is a variable at the top, and the CLI call underneath is written
out flag by flag so you can copy it and start changing things.

**These are for learning the tools.** For the long unattended runs -- the whole
pipeline, and the board farm that loops it for days -- see
[`../pipeline/`](../pipeline/).

## Start here

```bash
make                                   # builds the four binaries
bash examples/01_beamer_quickstart.sh  # ~5 minutes, needs ~8 GB RAM
```

That writes `beam_out/beam_completions_random_10.csv`. Every other script here
accepts it as input, so you can keep pushing the same board along.

## The scripts

| script | tool | what it teaches | needs |
|---|---|---|---|
| `01_beamer_quickstart.sh` | beamer (+ annealer) | Stage B from nothing: sample a border, grow the board row by row. `ANNEAL=1` runs Stage A first, so the borders are searched for rather than sampled | 8 GB RAM, ~5 min |
| `02_finalizer_regrow.sh` | finalizer | free the top rows of a board and re-grow them from a reduced database | seconds to minutes |
| `03_roundhouse_strip.sh` | roundhouse | rotate the board, refill a border strip; can prove a board dead in milliseconds | megabytes |
| `04_stage_c_close.sh` | topper + ender | the whole CP-SAT tail in the documented order: herd the breaks onto a band, sweep the ring, then patch what is left | `pip install ortools` |
| `05_backtracker_dives.sh` | backtracker | greedy dives to triage, exhaustive DFS to prove | minutes to overnight |
| `06_roundhouse_both_ways.sh` | roundhouse | chain two roundhouse passes per board, once each way round, so the two spirals cover all four sides; `--hold_band` lets the second keep what the first left | seconds to minutes |

## They all speak the same CSV

Every tool reads and writes the same canonical board row
(`config_id, score, pos[256], rot[256]`), so any output feeds any input --
including a tool's own output. That is what makes iteration possible:

```bash
bash examples/01_beamer_quickstart.sh
PARTIALS=beam_out/beam_completions_random_10.csv bash examples/02_finalizer_regrow.sh
BOARDS=final_out/beam_completions_finalized_12.csv bash examples/04_stage_c_close.sh
BOARDS=stage_c_out/3_patched.csv bash examples/05_backtracker_dives.sh
```

Between any two steps, look at what you have:

```bash
python3 tools/E555_rank.py   FILE --seed data/seed_Edge5.txt --top 10
python3 tools/E555_rank.py   FILE --seed data/seed_Edge5.txt --top 10 --no-id
python3 tools/E555_viewer.py FILE --seed data/seed_Edge5.txt
```

`--no-id` drops the board-id column, which is the widest one and the usual
reason the table wraps in a terminal; `row` still says which board is which.

## Fixing the Eternity II clue pieces

Scripts 01 to 04 take **`CLUES=1`**, which passes `--clue_center --clue_corners`
to every tool in that script. It is off by default and changes nothing when
unset.

```bash
CLUES=1 bash examples/01_beamer_quickstart.sh
CLUES=1 PARTIALS=beam_out/beam_completions_random_10.csv bash examples/02_finalizer_regrow.sh
CLUES=1 BOARDS=final_out/beam_completions_finalized_12.csv bash examples/04_stage_c_close.sh
```

Set it on **every** stage or on none. A clue held by one stage and dropped by
the next is no better than never holding it, which is exactly what used to
happen: script 02 locks rows 0..7 by default and re-grows everything above, so
without the flag it frees the centre clue's cell and quietly refills it. The
`clues` column of `E555_rank.py` counts how many of the five a board still has,
so you can check any stage's output at a glance:

```bash
python3 tools/E555_rank.py FILE --sort clues,score --no-id
```

And when a stage stalls on the same region twice, turn the board and hand it
back. Every stage here is direction-biased -- the finalizer frees rows from the
top, the roundhouse grows a strip against one border, the backtracker starts
from a fixed corner -- so a quarter-turn gives the same breaks to a different
one. The turn is lossless and re-scored to prove it:

```bash
python3 tools/E555_rotate.py FILE 1 --seed data/seed_Edge5.txt   # -> FILE_rot1.csv
```

`rank.py` is the one to trust. It recomputes the score from the seed -- field 2
of a Stage B row is a solution index, not a score -- and it also reports *where*
the breaks are. Eighteen breaks spread over seven rows is a mess; the same
eighteen packed into rows 14-15 is nearly finished.

## Two things that look like failures and are not

- **A configuration goes extinct and no board is emitted.** An empty candidate
  pool is an exact proof that that border is dead below the current row. The
  search is working: it abandons the configuration and plays another hand.
- **The roundhouse emits nothing and reports `REFUTED`.** Its oracle
  refuted every possible start before trying a single piece, and it ignores the
  piece supply entirely -- so no arrangement of any pieces can fill that band.
  That is a theorem about your board, delivered in milliseconds.
- **A roundhouse board scores far below the one you fed in.** Its output is
  break-free by construction; the score is 480 minus the junctions its *empty*
  cells leave open. A 191-piece board scoring 350 has no mismatch at all.

## Slurm

Every script carries commented-out `#SBATCH` headers and runs unchanged as a
batch job: uncomment them and `sbatch` the file. Nothing needs editing, because
all settings come from the environment.
