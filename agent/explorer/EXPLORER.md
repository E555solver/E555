# E555 explorer: new methods for Eternity II

An autonomous `/goal` agent that explores everything this repository's tools
can do, changes them, writes new ones, and looks for **methods** that get closer
to a solution than running the beamer harder, which has never beaten 465.

It runs in a Claude Code cloud session. Its deliverable is a set of measured,
documented methods that a much bigger workstation can run unchanged, plus any
good boards it finds on the way. Every search holds the Eternity II **centre
clue**, and every board is made from scratch.

The kit is three files:
- this one;
- `lab.sh`: setup, run, status, checkpoint;
- `score.py`: the measure, which the agent cannot change.

**Experimental:** the scripts are tested; a whole campaign under `/goal` is not.

## For you

### Launch

1. Start a **new** cloud session on this repository. Pick the latest **Opus**
   and **Auto** mode.
2. Send `/effort medium`. `/effort high` costs more usage but plans deeper
   experiments.
3. Paste this line, after editing the settings and the extra constraints:

```
/goal Run the E555 explorer campaign in agent/explorer/EXPLORER.md: read it now, again after any context compaction, and follow it. Settings: BUDGET=24h RUN_CAP=3600 CLUES=center CHECKPOINT=30m RESULTS_BRANCH=agent_runs CAMPAIGN=new RESUME=none. Extra constraints: none. A usage limit only pauses the campaign: continue it where it stopped once the limit resets. The goal is met when lab.sh setup printed SETUP failed twice in a row, or when the campaign's METHODS.md has been completed as EXPLORER.md's Done says, checkpointed, and its first ten lines printed, after one of these: (a) a STATUS line showed stop=480 or stop=budget; (b) a stop rule among the extra constraints held.
```

| setting | default | meaning |
|---|---|---|
| `BUDGET` | `24h` | **Run time**: the sum of the campaign's runs (`runs.csv`). Usage-limit waits, restores and the agent's thinking do not count, so 24h of runs can take several days of wall time |
| `RUN_CAP` | `3600` | The longest one run may take, in seconds. Runs over 540 s go to the background |
| `CLUES` | `center` | `center`, `all` or `none`: the Eternity II clue pieces every board must hold. Fixed when a campaign starts |
| `CHECKPOINT` | `30m` | Push the campaign folder to the results branch at least this often. `off`: never (a lost container then loses everything) |
| `RESULTS_BRANCH` | `agent_runs` | The results branch, shared with the other kits. Naming it here is your permission for the agent to push there, and nowhere else |
| `CAMPAIGN` | `new` | A new campaign, `agent/explorer/<id>/`, named by its UTC start time. An id, with `RESUME`, continues one |
| `RESUME` | `none` | `agent_runs` restores the campaign from its last checkpoint, in any session |

Anything in "Extra constraints" wins over this file. Examples:
- `THREADS=2`
- `no CP-SAT`
- `focus on frame-first methods`
- `stop after the first exact board of 400`

### While it runs

- **Every turn ends with a STATUS line:**
  ```
  STATUS best=458/480 bar=465 exact=387/480 runs=41 methods=3 options=96/413 run_time=7h20m/24h00m checkpoint=0h12m stop=none next=<move>
  ```
  - `best`: the best complete board, VALID and holding the clue.
  - `bar`: the repository's record, 465 (which lacks the clue; the best shipped board that holds it is a 464).
  - `exact`: the best board with no break at all, of any size (below).
  - `options`: options tried out of those inventoried.
- `/goal` shows the goal's state. To stop the campaign, send `/goal clear` and
  delete the scheduled task "E555 explorer <id> wake-up"; otherwise the task
  restarts it within the hour.
- **Everything is on the results branch** under `agent/explorer/<id>/` (browse
  `agent_runs` on GitHub):
  - `METHODS.md`: the deliverable, kept current;
  - `JOURNAL.md`: every experiment;
  - `OPTIONS.md`: what each option did;
  - `runs.csv`: every run, with its numbers;
  - `methods/`, `patches/` and `boards/`.

### Run the methods on a workstation

From a checkout of `main`:
```bash
bash agent/explorer/lab.sh setup CAMPAIGN=<id> RESUME=agent_runs ARCH=native   # fetches the campaign, builds its patched lab
bash agent/explorer/<id>/methods/<name>.sh THREADS=64 TIME=28800                # any method; settings as NAME=value
python3 agent/explorer/score.py <the boards it wrote>                            # the same measure as the campaign
```

Each method's header and its METHODS.md entry give:
- its settings;
- its memory (the chain database alone takes about 8 GB of RAM per beamer process);
- what it yielded per CPU-hour, and how that scaled with threads in the cloud.

`METHODS.md` ends with a workstation plan.

## The measure: score and exact

`agent/explorer/score.py FILE...` scores any board file on the real seed, the
same way everywhere. Both numbers are out of 480 internal edges.

- **score**: a complete board's matched edges, when the board is VALID and holds
  the clue. VALID means all 256 pieces once, with grey sides exactly where the
  cell faces out. It is the referee's number, on the scale of the 465 record.
- **exact**: a board of any size with **no break**. Every edge between placed
  neighbours matches, every piece is legal on its cell, and the clue is not
  contradicted. The value is the number of those matched edges, so scattered
  pieces earn nothing and a solution scores 480.

  Landmarks:

  | board | exact |
  |---|---|
  | rows 0..10 complete | 325 |
  | rows 0..12 | 387 |
  | rows 0..14 | 449 |
  | the 196 inner cells, frame open | 364 |

A solution is a board where both reach 480. Exact measures how far a search
gets without cheating, so it can rank methods long before they finish a
board; score ranks finished boards.

---

## For the agent

### Rules

**Containment.**
- Write only in `agent/explorer/<CAMPAIGN>/`.
- Change code only in its `lab/`: a full copy of the toolchain that is yours to
  change. Improve any source, write new tools (in the lab's `tools/` or `src/`,
  where checkpoints capture them), build what you like.
- Never edit this kit, `score.py`, `agent/publish.sh`, or anything else outside
  your folder. Read anything, including `lab/ref/` and other campaigns.

**Results come from `lab.sh run` alone.**
- Every run that spends compute (more than about 10 s) goes through it.
- Its `RUN` line and its row in `runs.csv` are the record.
- Never edit `runs.csv`, `scoreboard.json`'s bests or `boards/` by hand.
- Quick look-ups need no `run`: a usage text, `rank` or the viewer on a file,
  reading code.

**Fresh boards only.** The shipped boards and corpora (`data/best_*`,
`data/board_*`, `data/E565_*`) and the other campaigns' boards are for reading,
analysis and testing a tool. They are never the starting point of a counted
result. A run whose command names a shipped board file is logged with
`shipped=yes`, and its boards never become a best. Do not get around that.

**The clue.** Pass the campaign's clue setting to every tool that takes one:
- `--clue_center` in the beamer, finalizer, roundhouse, backtracker, ender and
  topper;
- `--pin_clue N` in the diver's rebuild;
- `CLUES=center` in the runners. `examples/01` and the whirlpool runners only
  know `CLUES=1`, which also pins the corners: patch them in the lab or pass the
  flags yourself.

A tool that can move a clue piece (the diver's `--reopen`, a free CP-SAT
window) may produce boards that `score.py` rejects. That is the measure working.

**Git.**
- Read-only commands only, plus `lab.sh setup` and `lab.sh checkpoint`.
- The checkpoint's push to `<RESULTS_BRANCH>` is the only push. The user named
  that branch, which is the permission.
- Never commit to the session's branch or `main`, even if your session
  instructions say to push your work: here the checkpoints are that work.
- Never commit a chain database (`*.db*`).

**One heavy job at a time.** A run's timing is a measurement, so start nothing
else heavy while it runs. Reading, writing code and journalling are fine.

### Names and paths

The SETUP line gives:
- `campaign=` (`<CAMPAIGN>`);
- `path=` (`<LAB>`, ending in `/agent/explorer/<CAMPAIGN>/lab`).

`<DIR>` is `<LAB>` without `/lab`; `<REPO>` is `<LAB>` without
`/agent/explorer/<CAMPAIGN>/lab`.

The shell's directory does not always survive between commands, so start every
command with `cd <LAB> && `. From there:
- the kit is `bash ../../lab.sh ...`;
- the measure is `python3 ../../score.py ...`.

Run `lab.sh setup` from `<REPO>` (`cd <REPO> && bash agent/explorer/lab.sh setup ...`),
because a lost container takes `<LAB>` with it.

### First turn: bootstrap

1. Print the settings resolved from the `/goal` text and the defaults:
   `GOAL BUDGET=24h00m RUN_CAP=3600 CLUES=center CHECKPOINT=30m RESULTS_BRANCH=agent_runs CAMPAIGN=new RESUME=none constraints=<text or none>`.
2. Run `cd <REPO> && bash agent/explorer/lab.sh setup CAMPAIGN=<CAMPAIGN> RESUME=<RESUME> CLUES=<CLUES>`,
   with the Bash timeout at 600000.
   - It takes about a minute; later calls take a second.
   - It must end with a `SETUP` line. On `SETUP failed`, run it once more. If
     it fails again, show both outputs and stop: the goal ends on that.
   - Otherwise print `CAMPAIGN <CAMPAIGN> lab=<LAB> results=<RESULTS_BRANCH>`.
3. Read, in this order:
   - this file's "For the agent";
   - `<DIR>/OPTIONS.md` (the inventory);
   - `pipeline/README.md`;
   - PROJECT_E555.md §3.1, §5.7–5.13, §6, §8 and §9;
   - the READMEs in `<LAB>/ref/`. The whirlpool campaign's README explains its
     87-jewels-per-hour runner and its `--no_fanout_sort` patch.

   A `patches/` file in `ref/` is a whole source file from an older tree. Never
   copy one over the lab: port the change into the lab's own file, if the lab
   lacks it.

   Read more of PROJECT_E555.md, and each tool's usage text (run the binary
   with no arguments), whenever a lead needs it.
4. Run `cd <LAB> && bash ../../lab.sh status BUDGET=<BUDGET> CHECKPOINT=<CHECKPOINT> RUN_CAP=<RUN_CAP>`.
   It stores the settings and starts the clock.
5. Unless `CHECKPOINT=off`, run `cd <LAB> && bash ../../lab.sh checkpoint BRANCH=<RESULTS_BRANCH>`.
   It must print `CHECKPOINT pushed=...`. If it fails twice, set
   `CHECKPOINT=off`, journal why, and go on.
6. Create the **wake-up task** (below).
7. Start the playbook, step 1.

### Every turn

0. **Health check:** `cd <REPO> && bash agent/explorer/lab.sh setup CAMPAIGN=<CAMPAIGN> RESUME=<R>`.
   `<R>` is `<RESULTS_BRANCH>` once a checkpoint was pushed, `RESUME` before
   that. Read the SETUP line:
   - **`folder=restored`:** the container was replaced. The campaign came back
     from its last checkpoint, and the lab was rebuilt with `patches/` laid
     over it. Journal it, look at the tail of `runs.csv`, and go on. The chain
     database is gone: the next beamer run rebuilds it, in about 2.5 minutes.
   - **`lab=fresh`** after the first turn: the same, without the folder loss.
   - **`cpu_changed=yes`:** another machine. Rates from different CPUs do not
     compare: rerun the baseline before claiming a gain.
   - **`stage_b_ok=false`:** under 8 GB of free RAM, so no beamer or chain
     database. Explore what needs neither (backtracker, CP-SAT, diver, your own
     tools), and check again next turn.
1. Then the playbook, one experiment at a time.

### Runs

```bash
cd <LAB> && bash ../../lab.sh run "LABEL" [CAP=s] [THREADS=n] [OUT=glob,glob] -- COMMAND ARGS...
```

**What it does.**
- It makes the folder `runs/<stamp>_<label>/` and writes its path wherever
  `{D}` appears in the command or `OUT`. `{LAB}` becomes the lab's absolute
  path.
- It runs the command from the lab under `timeout CAP` (default `RUN_CAP`),
  with all output in `{D}/run.log`, and kills whatever it left running.
- It scores the board files the run wrote: `OUT` (comma-separated globs), or by
  default every `*.csv` and `*.csv.gz` under `{D}`. Only files written during
  the run count.
- It prints a `RUN` line and the log's last lines, and keeps the bests:
  - `boards/best.csv` for score;
  - `boards/deepest.csv` for exact;
  - `boards/<run>.csv.gz` for the run's 50 best boards of each kind.

**Settings.**
- `THREADS`: what the command uses (default all). It feeds `cpu_h` and every
  per-CPU-hour comparison, so be honest about it.
- Wrap a pipeline of commands in `bash -c '...'`, or write a script in
  `methods/` and run that. A method becomes reproducible that way.

**Timeouts.**
- `CAP` up to 540: in the foreground, with the Bash timeout at 600000.
- Above 540: start the run with `run_in_background`, Bash timeout
  `(CAP + 180) × 1000` (at most 7200000). Do the bookkeeping, end the turn
  with `next=waiting for <label>`, and let the completion notice start the next
  turn. Never poll with `sleep`.

**The chain database.** The beamer's chain database takes about 6.6 GB of disk.
Building it takes about 8 GB of RAM and 2.5 min; a later beamer call maps the
file (a small run peaked at 2.4 GB). Keep one per lab:
- `--db_file E555.db.center` in direct beamer calls, and
  `DB_FILE={LAB}/E555.db.center` for `run_pipeline.sh`, `run_farm.py`, the
  examples and the whirlpool runner;
- `DB_FILE={LAB}/E555.db` for `run_board_farm.sh` and `run_halo.sh`, which
  append `.center` themselves.

With `CLUES=none` the name has no suffix. A clued database leaves the clue
pieces out, so it differs from an unclued one.

**Disk.** A run folder can hold millions of boards. Delete what a finished run
no longer needs: its best boards are already archived in `boards/`. The RUN
line prints `disk_free_gb`.

**Lost runs.** A container restart kills runs. A run folder with no `runs.csv`
row is lost: journal it and run it again.

### Checkpoints

Unless `CHECKPOINT=off`, run `cd <LAB> && bash ../../lab.sh checkpoint`:
- after a new best (`new_best=` in a RUN line);
- after a new method or patch;
- when STATUS shows `checkpoint=...(due)`;
- after the final METHODS.md.

It captures every lab file you changed or added (up to 1 MB) into
`patches/`. If your session instructions ask for attribution lines, pass them
as arguments; each becomes a paragraph of the commit message.
- **`CHECKPOINT failed`:** the next checkpoint pushes everything.
- **`CHECKPOINT refused`:** it names the problem (a change outside the folder,
  a file over 50 MB, more than 100 MB of history). Fix it first; never work
  around it.

### Usage limits and the wake-up task

Opus spends a 5-hour usage window in a few hours. When the limit hits, the turns
stop until the window resets.
- The clock counts run time, so the wait is free.
- Checkpoints keep the folder if the idle container is reclaimed.
- An hourly scheduled task wakes the session after the reset.

**Creating the task.**
1. If `scoreboard.json` already has a `wakeup_trigger_id`, delete that task
   with `delete_trigger` first; an error saying it is gone is fine.
2. Call `create_trigger` (claude-code-remote; load it with ToolSearch if it is
   deferred), firing into this session. Set neither `persistent_session_id`
   nor `create_new_session_on_fire`. Use:
   - `name`: `E555 explorer <CAMPAIGN> wake-up`
   - `cron_expression`: `0 * * * *`
   - `initiation`: `human_request`
   - `prompt`, with `<CAMPAIGN>` written out: `E555 explorer <CAMPAIGN> wake-up. Run the health check of agent/explorer/EXPLORER.md for campaign <CAMPAIGN> (it restores a lost folder). If scoreboard.json in agent/explorer/<CAMPAIGN>/ has "done": true, delete this scheduled task (its wakeup_trigger_id) and stop. Otherwise continue the campaign where it stopped, following EXPLORER.md; if a run is still in flight, let it finish.`
3. Store the returned id as `wakeup_trigger_id` in `scoreboard.json`.

Without `create_trigger`, journal that the campaign has no wake-up.

### STATUS line

End every turn with the line that `cd <LAB> && bash ../../lab.sh status` prints
in that turn, followed by ` next=<next move>`. `stop=` is `480` (solved),
`budget` or `none`.

### The playbook

**The mission.** Find methods that get closer to a solution of Eternity II,
with the clue, than the repository's current paths do. Measure them per
CPU-hour, in exact and in score. Deliver them so that a machine 20–50 times
larger can run them. Boards above 465 are welcome, and so is a solution. The
lasting result, though, is a method that turns compute into progress better
than what exists.

**Be creative.**
- The tools have hundreds of options (`OPTIONS.md`), and most combinations have
  never been measured.
- The examples and pipelines are examples, not optimized; use them, rewrite
  them, or ignore them.
- Write new tools, patch the C, try ideas nobody has tried.
- An unexpected route that works is worth more than a tuned familiar one.

**What is known** (measured in a cloud container: 4 cores, 15 GB):

*Records.*
- The repository's best board is 465, without the clue.
- Clued, it is 464 (`data/best_464.csv`); 31 of the 38 shipped 463s also hold
  the centre clue.
- The best public board ever matches 470.

*Beamer.*
- The first clued beamer run builds the chain database (6.6 GB,
  `E555.db.center`).
- `--random_edges --samples 4 --top_columns 4 --clue_center --beam_width 10000 --backtrack_row 6 --stop_row 10 --end_dive 10000 --end_polish 20000`,
  in 480 s including the build, wrote 3,044 complete clued boards, the best
  456: exact 0, score 456.
- Beams of this kind make thousands of 450s quickly and have never passed 465.

*Halo.* `run_halo.sh` with its defaults (`CLUES=center`) ran 6 minutes and
scored 949 exact partials, the best exact 321 (174 placed), with no jackpot.

*Whirlpool* (`ref/`, campaign 20261028-0249).
- Its v08 runner makes 87 jewels per hour: boards whose fixed 60-cell frame
  re-grows to row 10 from all four sides. The jewels dived to 461.
- Its patch `--no_fanout_sort` makes a lap's finalizer call nearly 3 times
  faster (13.6 s against 37 s). The flag is now in the repository's finalizer,
  and so in your lab.
- It was run without clues.

*Fixed references.*
- Exact landmarks: 325 for rows 0..10, 364 for the full interior with the frame
  open.
- The backtracker is exact and deterministic. It resumes (`--resume`), takes
  `--holes` masks and `--order` choices, and can search the frame last
  (`--frame_last`) under an edge-supply bound (`--edge_supply`).
- The roundhouse is usually beaten by the plain backtracker.

**Steps.** A guide, not a cage.

1. **Baseline** (about 10% of the budget). Measure what the standard paths
   reach per CPU-hour in exact and score, with the clue. For example:
   - the beamer at a few widths, with and without end dives;
   - the halo;
   - the whirlpool v08 from `ref/` (changed to hold the clue; its finalizer
     flag is already in the lab);
   - one farm iteration.

   These numbers are the bar for "breakthrough". Write them into METHODS.md
   first.
2. **Coverage.** Work through `OPTIONS.md`: the options nobody measured, their
   sweeps, and their interactions. Update each row as you test it (`tested`,
   `promising` or `dead`, the run ids, one line learned). Coverage is part of
   the deliverable: humans had no time for it.
3. **Leads from the user:**
   - the beamer's column extension (`--extend_nodes`, §5.10) and its right
     and top band (`--backtrack_unlock_right`);
   - the backtracker's early release, frame-last and edge-supply search (the
     halo);
   - the finalizer and its whirlpool. Jewels are fixed frames that grow to
     row 10 from every side, a natural start for an exact interior search;
   - `experiments/datadriven` (a beamer that learns where pieces sit) and
     `experiments/soft` (needs NLopt), with care;
   - Stage A borders, the diver's rebuild, CP-SAT neighbourhoods.
4. **Unexpected routes** (examples to break the habit, not a list to finish):
   - frame first, then the interior;
   - centre-out from the clue;
   - meet in the middle: two half-boards joined on a row of edges;
   - piece and colour statistics from deep exact boards turned into priors or
     pruning;
   - new search orders, restarts and diversity schemes;
   - a new solver in C, profiled first;
   - hybrids that hand a board from tool to tool.
5. **Discipline.**
   - Compare per CPU-hour, with the clue, on this machine.
   - Repeat a promising result with another seed before you believe it.
   - Measure scaling: the same method at `THREADS=1`, 2 and 4, as cpu_h
     against yield. Memory is the RUN line's `max_rss_mb`, the largest single
     process's peak.
   - A patch adds a flag that is off by default, so the original behaviour
     survives. Note it in METHODS.md's patch list.
6. **Methods.** A method that works becomes `<DIR>/methods/<name>.sh` at once,
   with a METHODS.md entry.
   - It opens with a settings block, overridden by `NAME=value` arguments:
     ```bash
     REPO=$(cd "$(dirname "$0")/../lab" && pwd)   # the campaign's lab, patched and built
     THREADS=$(nproc)
     TIME=3600                                    # seconds
     DB_FILE=$REPO/E555.db.center
     CLUES=center
     OUT_DIR=out_<name>
     ```
   - It writes its boards under `OUT_DIR` and prints their paths at the end.
     It must run unchanged on a 64-core machine.
   - Run it through `lab.sh run` for its numbers.
7. **Allocation** (guidance):
   - baseline about 10%;
   - breadth (coverage and leads) about 40%;
   - depth (the promising methods: longer runs, combinations, patches) about
     40%;
   - wild ideas 10–20% throughout;
   - polishing your own boards at most 10%.

Journal every experiment in `JOURNAL.md`: hypothesis, `RUN` id, result,
decision.

### METHODS.md

Keep it current; it is the deliverable. Its first ten lines are the headline,
and Done prints them.

1. **Headline:**
   - the best score and the best exact;
   - the best method and its yield per CPU-hour against the baseline's;
   - run time spent;
   - the clue setting.
2. **Ranked table:** method, the idea in a line, exact and score yields per
   CPU-hour, the best exact and score found, scaling (1/2/4 threads), memory,
   and the `methods/` script.
3. **Per method:** the idea, why it should work, the exact command, the
   numbers with run ids, and what limits it.
4. **Patches and new tools,** in plain language: what each changes, its flag,
   and how it was checked. The lab's `tests/run_tests.sh` runs the gate on
   your patched tree.
5. **Dead ends:** what did not work, with the numbers, so nobody repeats it.
6. **Workstation plan:**
   - for a given machine (cores, RAM), which methods to run, with which
     settings, how many at once, and the yields to expect;
   - what to try first if they disappoint.
7. **Reproduce:** the three commands of "Run the methods on a workstation",
   with this campaign's id.

### Done

When STATUS shows `stop=480` or `stop=budget`, or a stop rule of the extra
constraints holds, start no new run, and wait for the one in flight. Then:
1. Complete `METHODS.md` as above, plus a **Machine** paragraph: the CPUs seen,
   the restores, the usage-limit pauses.
2. Set `"done": true` in `scoreboard.json`, then run
   `cd <LAB> && bash ../../lab.sh checkpoint` (unless `CHECKPOINT=off`).
3. Delete the wake-up task (`delete_trigger` with `wakeup_trigger_id`).
4. Print `head -10 <DIR>/METHODS.md` and a fresh STATUS line.
5. Tell the user:
   - the campaign id and where it is: `agent/explorer/<CAMPAIGN>/` on
     `<RESULTS_BRANCH>`;
   - the best score and exact;
   - the best method and its gain over the baseline;
   - the first lines of the workstation plan.
