# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

E555 is a pipeline of solvers for Eternity II (16x16 edge-matching puzzle, 256 pieces, 480 interior edges). Boards grow bottom-up via a wide beam search over a precomputed database of all legal 5-piece row chains (Stage B), then the top rows are attacked by tail solvers (Stage C). `PROJECT_E555.md` is the authoritative technical reference: algorithms, scoring math, every CLI flag, file formats. Consult its relevant section before changing a tool's behaviour.

## Build

```bash
make                      # bin/E555_beamer, E555_finalizer, E555_roundhouse, E555_backtracker, E555_diver
make ARCH=generic         # use this in containers / CI / cloud sandboxes (default is -march=native)
make beamer               # single target; also finalizer | roundhouse | backtracker | diver | clean
pip install ortools       # only needed for src/C_tail/E555_topper.py and E555_ender.py
```

- Flags are `-Wall -Wextra -O3 -fopenmp`. The test gate fails on **any** compiler warning.
- Changing `ARCH`/`OPT`/`CC` forces a rebuild through `bin/.buildflags`. A binary built with `-march=native` dies with SIGILL on an older CPU.
- beamer, finalizer and roundhouse all link `src/B_beam/E555_database.c`; the beamer, the finalizer and the diver also link `src/C_tail/E555_dive.c` (the end-dive engine, no argv of its own), and the diver links `E555_database.c` too. The backtracker is standalone.
- `tests/datadriven/` is a self-contained beamer fork with its own Makefile (`cd tests/datadriven && make ARCH=generic`).

## Tests

`tests/run_tests.sh` holds every check. It runs the selected ones in order, stops at the first failure, and writes everything to `tests/out/`, which is wiped at the start of each run. Run only what a change needs:

```bash
ARCH=generic bash tests/run_tests.sh                    # the core checks (~20 s): run for any change
ARCH=generic bash tests/run_tests.sh beamer             # every check of one tool (+ compile, no_stray_output)
ARCH=generic SKIP_BEAMER=1 bash tests/run_tests.sh --all   # the release gate without the 6.4 GB DB (~4 min)
bash tests/run_tests.sh --list                          # numbered list: tier, name, tags, label
bash tests/run_tests.sh 6 8-11 roundhouse_cache         # by number, range or name
```

- After a change, run the core set plus the tags of the tools you touched: `tools` (Python tools), `annealer`, `finalizer`, `roundhouse`, `backtracker`, `diver`, `beamer`, `cpsat`, `scripts`, `pipeline`. Shared sources: `E555_database.c` -> `beamer finalizer roundhouse diver`; `E555_dive.c` -> `beamer finalizer diver`. Run `--all` only before a release or when asked.
- Selection pitfalls: `core` is not a tag (the no-argument run is the core set), so run it as a separate invocation from a tag run. A word that is both a check name and a tag selects only the check: `diver` and `annealer` run one check each, not their tags. For those tags, pass the numbers that `--list` shows.
- The `ALL_STEPS` array at the top of `run_tests.sh` is the only list of checks. Each entry `name|tier|tags|label` has a matching `step_name` function. To add a check, add both; make it `core` only if it is fast and guards something no core check does.
- Each check runs on its own and never depends on an earlier check's artifacts. If check 1 (`compile`) isn't selected, the checks use whatever is already in `bin/`.
- The `beamer` checks and `pipeline_full` need the real 6.4 GB chain database (~8 GB RAM). The gate builds it once per run, on every core (~100 s on 4), before the first of them; `DB_FILE=path` keeps that cache across runs, after which each of those checks takes under a minute. `SKIP_BEAMER=1` skips them. `DB_IN_MEMORY=1` avoids writing it to disk but rebuilds it in every beamer run, so the beamer checks then take many minutes: use it only when the disk is full.
- Key regressions: the finalizer and roundhouse must rediscover `data/synth_solution_480.csv` (seed `data/synth_seed.txt`), and `viewer` must score it 480/480.
- Checks that need OR-Tools print `SKIPPED` and **pass** when it is missing. That includes the core regression `ender_repair`. Run `pip install ortools` before trusting a green run that touches the ender, topper or diver.
- `no_stray_output` fails if any check leaves a file in the repo root, so tools that write to the working directory must run from inside `tests/out`.
- `scripts_parse` (`tests/check_script_flags.py`) collects each binary's accepted flags from its `strcmp(argv[i], "--x")` sites. Every `--flag` that a script in `pipeline/`, `examples/` or `tests/` passes must be in that set. If you rename or remove a C flag, update every script that uses it. The check also fails when a tool's hand-written `print_cmd()` doesn't mention every flag its parser accepts, or when a comment sits between two backslash-continued lines.
- Bash idiom used in the gate (`set -euo pipefail`): don't write `$(ls GLOB | head -1)`; use `first_match`. Don't pipe a tool's long output into `grep -q`, because SIGPIPE plus pipefail causes false failures. Write to a file and grep that instead.

## Architecture

```
Stage A  src/A_border/E555_edge_annealer.py   Euler-trail simulated annealing -> rotations.csv (border spins)
                                               (optional: beamer --random_edges samples borders itself)
Stage B  src/B_beam/E555_beamer.c             5-5-5 chain DB + wide beam + colour/Mahalanobis heuristic;
                                               --backtrack_row exhaustive tail, each board then extended
                                               column by column (--extend_nodes); --backtrack_unlock_right R
                                               leaves an L band (right R columns, top) and grows it in layers;
                                               --end_dive/--end_polish finish;
                                               --lambda_reserve keeps Stage A's TOP double-decker reserve for last
                                               boards to 256 pieces (engine: src/C_tail/E555_dive.c)
         src/B_beam/E555_finalizer.c          restart the beam from a partial, locked below a row (reduced DB);
                                               locks clean top rows (--keep_ring: the ring); --backtrack_row, --end_dive
         src/B_beam/E555_roundhouse.c         rotate 90 deg, grow a W-wide strip; exhaustive, DP oracle
Stage C  src/C_tail/E555_topper.py            CP-SAT break minimizer over bands / --holes masks
         src/C_tail/E555_backtracker.c        greedy dives + exhaustive DFS; --stop_row/--stop_column bands
         src/C_tail/E555_diver.c              the beamer's --end_dive/--end_polish on any board file; no DB;
                                               --holes/--pin_clue/--stop_row rebuild the top like the beamer first
         src/C_tail/E555_ender.py             CP-SAT closer, never returns a worse board
tools/   viewer, rank (--rescore), rotate (--sink), distiller (dive screen -> finish -> redive probe -> ender plan), extract_consensus, sort_rotations, clean_csv
```

- **One CSV dialect connects everything.** A canonical board row is `config_id, score, pos[256], rot[256]` (514 fields). `pos[p]` is the cell of piece `p`, with 999 meaning unplaced. Readers take the **last 512 fields** as pos+rot and treat any leading fields as metadata, so Stage B rows (which carry a solution index in slot 2, or the score under `--end_dive`) and the 515-field rows (legacy, or backtracker `--resume` output with a resume identifier third, which `--rescore` drops) parse everywhere. Lines starting with `#` or `%` are comments. Any stage's output can feed any other stage, or itself. `tools/E555_rank.py --out F --rescore` rewrites rows canonically.
- **Geometry conventions** (PROJECT_E555.md, "Conventions"): rows and columns are 0-indexed **bottom-up** (row 0 is the bottom border), and cell = `row*16 + col`. Rotations are CCW quarter-turns `s ∈ {0..3}`: side `d` of the rotated piece reads seed side `(d+s) mod 4`.
- **Seed files** (`data/seed_Edge5.txt` is the real puzzle, `data/synth_seed.txt` is synthetic) list 4 colours per piece. Most tools take `--seed_file`.
- **Stage B always grows upward.** The whirlpool (`pipeline/run_pipeline_whirlpool.sh`) turns the board 90° and uses `backtracker --stop_row N --with_frame` to convert complete columns back into complete rows. That lets the finalizer re-search buried rows.
- **Chain database:** all 3.1 B legal 5-piece row chains, 6.4 GB, built in `E555_database.c` and optionally cached to disk with `--db_file`. The roundhouse only uses the edge half of it (small and fast).
- **Top-corner supply (`--lambda_corners`)**, beamer and finalizer: the exact catalog of legal top-corner blocks, alive counting and the stop-row report are shared `tc_*` code in `E555_database.c/.h`; each tool only wires the score, the column filter and the CLI.
- **Script/runner convention** (`examples/`, `pipeline/`): each script opens with a block of plain settings, overridden by `NAME=value` **arguments** (not environment variables). Stages hand off through the `outputs.txt` each tool writes and pass `--print_cmd` so the log records the exact commands. `examples/` is for learning single tools. `pipeline/` holds long unattended runs, plus `slurm_wrapper.sh` for clusters.
- Run products (`beam_out/`, `final_out/`, `tests/out/`, `*.db_cache`, `rotations.csv`, …) are gitignored. Don't commit them.
- **End-dive engine** (`E555_dive.c`): its tuning constants are the `DV_*` defines at the top of the file (measured; PROJECT_E555.md 5.11). To expose one in a single tool, add a `DvParams` field whose 0 means the built-in default; tools that don't set it keep byte-identical output (example: the diver's `--polish_top`).
- `agent/` holds an experimental, untested autonomous `/goal` kit. It is not part of the toolchain.

## Making changes

- **A C flag lives in four places:** the `strcmp(argv[i], "--x")` parser, the usage text, `print_cmd()`, and that tool's Options table in `PROJECT_E555.md` (beamer §5.16, finalizer §6.10, roundhouse §7.5, Stage C §9.x). A behaviour change normally updates `PROJECT_E555.md` and adds or extends a check in `tests/run_tests.sh` in the same commit.
- **The backtracker's DFS order is part of a file format.** `--resume` identifiers (format `bt1`, documented at "Resume identifier" in `E555_backtracker.c`) record a path through the exact search. Changing the cell choice (`pick_next_cell`, `order_key`) or the candidate order (`collect_candidates`) invalidates them: bump the version there.
- **Determinism contract** (PROJECT_E555.md §5.15): the backtracker, the column extension, the dives and the diver write byte-identical output at any thread count, and checks compare 1 vs 4 threads. The beam is not like that: with `--rng_seed`, it repeats only on the same build and thread count. Keep work partitioning in the exact engines independent of the thread count.
- Python code (Stage A, `tools/`, runners, tests) targets Python ≥ 3.9 and uses only the standard library. The exception is `ortools`, in the two CP-SAT tools.
- Commit subjects name the area first: `Beamer: …`, `Beamer/finalizer: …`, `PROJECT_E555: …`, `Gate: …`.

## Working conventions

- **Git:** other sessions also push to `main`. Before pushing to it, `git fetch origin main` and rebase your branch onto it. Push `main` only as a fast-forward, never with force.
- **Docs:** PROJECT_E555.md documents behaviour and flags concisely. Small internal details stay in code comments.
- **Output:** every stdout line must be useful to the user running the tool. Diagnostics go behind `--verbose` or a test-only environment variable.
- **Before blaming a change for a failing check,** run that check against a binary built from the base commit.
