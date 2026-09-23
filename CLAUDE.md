# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

E555 is a pipeline of solvers for Eternity II (16x16 edge-matching puzzle, 256 pieces, 480 interior edges). Boards grow bottom-up via a wide beam search over a precomputed database of all legal 5-piece row chains (Stage B), then the top rows are attacked by tail solvers (Stage C). `PROJECT_E555.md` is the authoritative technical reference: algorithms, scoring math, every CLI flag, file formats. Consult its relevant section before changing a tool's behaviour.

## Build

```bash
make                      # bin/E555_beamer, E555_finalizer, E555_roundhouse, E555_backtracker
make ARCH=generic         # use this in containers / CI / cloud sandboxes (default is -march=native)
make beamer               # single target; also finalizer | roundhouse | backtracker | clean
pip install ortools       # only needed for src/C_tail/E555_topper.py and E555_ender.py
```

- Flags are `-Wall -Wextra -O3 -fopenmp`. The test gate fails on **any** compiler warning.
- Changing `ARCH`/`OPT`/`CC` forces a rebuild through `bin/.buildflags`. A binary built with `-march=native` dies with SIGILL on an older CPU.
- beamer, finalizer and roundhouse all link `src/B_beam/E555_database.c`. The backtracker is standalone.
- `tests/datadriven/` is a self-contained beamer fork with its own Makefile (`cd tests/datadriven && make ARCH=generic`).

## Tests

`tests/run_tests.sh` is the release gate. It runs numbered checks in order, stops at the first failure, and writes everything to `tests/out/`, which is wiped at the start of each run.

```bash
ARCH=generic SKIP_BEAMER=1 bash tests/run_tests.sh      # full gate without the 6.4 GB DB (~4 min)
bash tests/run_tests.sh --list                          # numbered list of checks
bash tests/run_tests.sh 6                               # one check by number
bash tests/run_tests.sh 8-11 14                         # ranges
bash tests/run_tests.sh roundhouse_cache                # by name
```

- The `ALL_STEPS` array at the top of `run_tests.sh` is the only list of checks. Each entry `name|label` has a matching `step_name` function. To add a check, add both.
- Each check runs on its own and never depends on an earlier check's artifacts. If check 1 (`compile`) isn't selected, the checks use whatever is already in `bin/`.
- `beamer_micro`, `example_beamer` and `pipeline_full` need the real 6.4 GB chain database (~8 GB RAM). Set `SKIP_BEAMER=1` to skip them, `DB_FILE=path` to keep a persistent DB cache, or `DB_IN_MEMORY=1` to avoid writing it to disk.
- Key regressions: the finalizer and roundhouse must rediscover `data/synth_solution_480.csv` (seed `data/synth_seed.txt`), and `viewer` must score it 480/480.
- `no_stray_output` fails if any check leaves a file in the repo root, so tools that write to the working directory must run from inside `tests/out`.
- `scripts_parse` (`tests/check_script_flags.py`) collects each binary's accepted flags from its `strcmp(argv[i], "--x")` sites. Every `--flag` that a script in `pipeline/`, `examples/` or `tests/` passes must be in that set. If you rename or remove a C flag, update every script that uses it.
- Bash idiom used in the gate (`set -euo pipefail`): don't write `$(ls GLOB | head -1)`; use `first_match`. Don't pipe a tool's long output into `grep -q`, because SIGPIPE plus pipefail causes false failures. Write to a file and grep that instead.

## Architecture

```
Stage A  src/A_border/E555_edge_annealer.py   Euler-trail simulated annealing -> rotations.csv (border spins)
                                               (optional: beamer --random_edges samples borders itself)
Stage B  src/B_beam/E555_beamer.c             5-5-5 chain DB + wide beam + colour/Mahalanobis heuristic
         src/B_beam/E555_finalizer.c          restart the beam from a partial, locked below a row (reduced DB)
         src/B_beam/E555_roundhouse.c         rotate 90 deg, grow a W-wide strip; exhaustive, DP oracle
Stage C  src/C_tail/E555_topper.py            CP-SAT break minimizer over bands / --holes masks
         src/C_tail/E555_backtracker.c        greedy dives + exhaustive DFS; --stop_row/--stop_column bands
         src/C_tail/E555_ender.py             CP-SAT closer, never returns a worse board
tools/   viewer, rank (--rescore), rotate (--sink), distiller (--plan/--triage), extract_consensus, sort_rotations, clean_csv
```

- **One CSV dialect connects everything.** A canonical board row is `config_id, score, pos[256], rot[256]` (514 fields). `pos[p]` is the cell of piece `p`, with 999 meaning unplaced. Readers take the **last 512 fields** as pos+rot and treat any leading fields as metadata, so Stage B rows (which carry a solution index in slot 2) and the legacy 515-field rows parse everywhere. Lines starting with `#` or `%` are comments. Any stage's output can feed any other stage, or itself. `tools/E555_rank.py --out F --rescore` rewrites rows canonically.
- **Geometry conventions** (PROJECT_E555.md, "Conventions"): rows and columns are 0-indexed **bottom-up** (row 0 is the bottom border), and cell = `row*16 + col`. Rotations are CCW quarter-turns `s ∈ {0..3}`: side `d` of the rotated piece reads seed side `(d+s) mod 4`.
- **Seed files** (`data/seed_Edge5.txt` is the real puzzle, `data/synth_seed.txt` is synthetic) list 4 colours per piece. Most tools take `--seed_file`.
- **Stage B always grows upward.** The whirlpool (`pipeline/run_pipeline_whirlpool.sh`) turns the board 90° and uses `backtracker --stop_row N --with_frame` to convert complete columns back into complete rows. That lets the finalizer re-search buried rows.
- **Chain database:** all 3.1 B legal 5-piece row chains, 6.4 GB, built in `E555_database.c` and optionally cached to disk with `--db_file`. The roundhouse only uses the edge half of it (small and fast).
- **Script/runner convention** (`examples/`, `pipeline/`): each script opens with a block of plain settings, overridden by `NAME=value` **arguments** (not environment variables). Stages hand off through the `outputs.txt` each tool writes and pass `--print_cmd` so the log records the exact commands. `examples/` is for learning single tools. `pipeline/` holds long unattended runs, plus `slurm_wrapper.sh` for clusters.
- Run products (`beam_out/`, `final_out/`, `tests/out/`, `*.db_cache`, `rotations.csv`, …) are gitignored. Don't commit them.
- `agent/` holds an experimental, untested autonomous `/goal` kit. It is not part of the toolchain.
