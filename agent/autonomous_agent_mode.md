# E555 Autonomous Solver — `/goal` Kit (v3)

**WARNING: this is untested experimental territory for learning about AI agent capabilities.**
A ready-to-paste `/goal` setup that turns one interactive Claude Code session
into a self-driving, multi-hour optimizer over the *real* E555 toolchain in this
repo. Do the setup (sections 4 and 5), paste section 8, walk away, and come back
to a documented campaign. In a **cloud session** (claude.ai/code, the Claude
app), paste the `/goal` line of `agent/launch_cloud_agent.md` instead: section 11.

Written for the latest **Opus** model (the `opus` alias; Opus 5.5 today) at
**medium** effort, on a claude.ai subscription. A campaign outlasts a 5-hour
usage window, and that is expected. When the limit hits, the session waits and
continues once the window resets (section 4.2, and for cloud sessions section
11). The campaign's clocks count run time, not wall time (section 6), so the
wait costs none of its budget. The kit stays deliberately *prescriptive*:
exact commands, explicit budgets, a fixed decision procedure, and hard
guardrails. A long unattended run should follow the recipe rather than
improvise.

v3 changes from v2: the current Stage C tools (the diver's `--reopen`, the
distiller, the ender's profiles), a referee script the agent cannot edit, a
lab that is a self-contained checkout, and a `/goal` condition that fits the
4,000-character limit. v3.1 adds `agent/setup_lab.sh`, which replaces the
hand-run setup block, and cloud sessions with checkpoints (section 11). v3.2
moves the kit to Opus at medium effort, measures budgets in run time
(`agent/status.sh`), and continues campaigns across usage-limit resets.

---

## 0. The one-paragraph mission

The puzzle is the 16×16, 256-piece Eternity II board defined by
`data/seed_Edge5.txt`. A board scores **matched internal edges out of 480**, and
`breaks = 480 − score`. **480 = solved.** Nobody has solved it in public: the
best public board since 2007 matches 470. This repo's best is **465/480**
(`data/best_465.csv`, 15 breaks), followed by eight 464s (`data/best_464.csv`)
and 38 463s (`data/best_463.csv`). The honest, measurable mission is:

> **Find a referee-valid board above 465/480 and drive the break count as low
> as the tools can reach, documenting every step so the run can be evaluated
> and resumed later.** 480 is the moonshot; every break removed below 15 is a
> real result.

Reaching 466 is a win. Reaching 470 ties the public record. The point is real
progress under a trustworthy referee, not a number that merely *looks* good.

---

## 1. Ground rules (containment + anti-cheating contract)

These are non-negotiable. Section 8 repeats them inside the `/goal` condition.

**Containment**
- **Every campaign has a lab of its own**, `agent/labs/<id>/`, named by the
  UTC date and time it started (`20261027-1930`), so the campaigns you run now
  and later never mix their results. "The lab" below always means this
  campaign's lab.
- The session runs **in the lab** (section 4.2 launches Claude Code there), so
  the permission system itself treats the rest of the repo as outside the
  working directory. Read anything in the repo, other campaigns included;
  **write only inside the lab**. A cloud session starts at the repository root
  instead, so there this rule holds by instruction alone.
- No git command that changes the repository (no commit, push, checkout, reset,
  stash or clean). `agent/labs/` is gitignored. The one exception is a cloud
  session's checkpoint (section 11): `agent/checkpoint.sh` publishes this
  campaign's `agent/checkpoints/<id>/` alone to the shared results branch
  (`agent_runs`), never to the code's branches.
- **The chain database is never committed.** The beamer builds its 6.4 GB
  chain database at the start of an F4 run. When `db_on_disk` is true, F4 also
  writes it to the lab as `E555.db` (6.8 GB, local disk only), so that later F4
  runs load it in seconds instead of rebuilding it. That file stays in the lab:
  it is never checkpointed or committed. `*.db` is gitignored, and
  `checkpoint.sh` refuses any file over 50 MB.
- You **may** edit the lab's copies of the source, write new tools in the
  lab's `tools/`, and rebuild the lab's binaries. Improve anything you like:
  samplers, allocators, heuristics.

**Anti-cheating contract** (the run is worthless if any of these is broken)
1. The puzzle is `data/seed_Edge5.txt`. `synth_seed.txt` and
   `synth_solution_480.csv` are a *self-test fixture* (solvable by
   construction). Solving them is **not** progress and never touches the
   scoreboard.
2. **The referee decides.** The official score of a board file is the line
   `bash ../../referee.sh FILE` prints. `agent/referee.sh` runs the repo's own
   `tools/E555_rank.py` and `tools/E555_viewer.py` on the repo's own seed, all
   outside the lab, so editing the lab's copies cannot change its verdict.
   Never edit the referee, the repo's tools or the seed. You may write your own
   cross-check scorers in the lab, but their numbers never count.
3. Every frontier board must be **traceable to a logged command** that produced
   it (a `run_log.csv` row). No hand-written or hard-coded boards.
4. A board counts only with `verdict=VALID`: all 256 pieces placed, each once,
   spins 0..3, and every frame cell grey on its outward side. The frame check
   matters because the score counts any two equal colours as a match, grey
   included, so a board with frame pieces moved inside could collect matches a
   legal board cannot.
5. **Report reality.** A run that did not improve is data. Log it truthfully
   with its real score. Never round up, and never claim an improvement the
   referee does not confirm.
6. A **new best** needs a fresh referee call on the emitted file that prints
   `verdict=VALID` with a score **above** `best_score`. A different board with an
   equal score is an alternative: add it to `frontier/elite.csv`, not the
   frontier.

---

## 2. The terrain: what each tool does and how to call it

Every command below runs from the lab, `agent/labs/<id>/`, which is a small checkout of its
own (`bin/ src/ tools/ examples/ pipeline/ data/ Makefile`). The example
scripts set `REPO` to their own parent directory, so `bash examples/X.sh` run
from the lab reads and writes in the lab. Always give them an `OUT`/`OUT_DIR`
under a fresh `runs/<ts>_<tool>/`. **Read the header of an example script before
first using it**; it documents every setting. `PROJECT_E555.md` (copied into the
lab) is the full reference: section 9 for Stage C, section 3.1 for the
recommended Stage B command.

**Board files.** A canonical row is `config_id, score, pos[256], rot[256]`
(514 fields). `pos[p] = row*16 + col` is the cell of piece `p` (row 0 = bottom,
`999` = unplaced). Field 2 is matched edges, except that Stage B writes a solution
index there without `--end_dive`. Never trust field 2: ask the referee.

**The tools, in the order they matter at 46x/480:**

| tool | job | use it for | vetted script |
|---|---|---|---|
| `bin/E555_diver --reopen` | lift the damaged band of a complete board, re-dive it in copies and rounds; **never returns a worse board** | the main lever on polished 46x boards | `examples/11_diver_reopen.sh` |
| `src/C_tail/E555_ender.py` | the closer: exchange cycles, the diver's redive, CP-SAT windows, corners, bands and the frame; never worse; needs OR-Tools | after reopen stalls; long attacks on elites | `examples/04b_CP-SAT_ender_overnight.sh`, `examples/04c_CP-SAT_ender_elite.sh` |
| `tools/E555_distiller.py` | corpus of partials → the N best, finished and probed, plus an ender plan | turning a pool into candidates | `examples/08_distiller_quickstart.sh` |
| `bin/E555_diver` (no `--reopen`) | finish partials with end dives and polish; `--holes top:K --stop_row S` lifts and rebuilds the top first | partials; rebuilding a top | `examples/10_diver_quickstart.sh` |
| `bin/E555_beamer` | Stage B: fresh boards from borders (beam, exhaustive tail, end dives); ~8 GB RAM | new raw material | `examples/01_beamer_quickstart.sh`, PROJECT_E555.md 3.1 |
| `bin/E555_finalizer` | re-grow a partial from a locked row | re-searching the upper rows | `examples/02_finalizer_regrow.sh` |
| `tools/E555_rotate.py` | turn boards (lossless); `--sink M` drops M rows for a fresh top | attacking from another side | — |
| `src/C_tail/E555_topper.py` | CP-SAT break minimizer over border bands | breaks against a border on *unpolished* boards | `pipeline/topper_sweep.sh` |
| `bin/E555_backtracker` | exact DFS (`--break_mode any`) that proves a bound; default `stuck` = greedy dives that prove nothing | small holes, proofs | `examples/05_backtracker_dives.sh` |
| `pipeline/run_farm.py` | the whole beam → topper → roundhouse → ender loop, for days | a model for chaining stages; too long for one 1200 s run | `pipeline/README.md` |

**The measure.** The referee gives the number; `rank` shows the shape:
```bash
bash ../../referee.sh FILE                       # official: REFEREE verdict=VALID score=465 breaks=15 ...
python3 tools/E555_rank.py FILE --top 5       # where the breaks are
python3 tools/E555_rank.py FILE --diverse 10 --out x.csv   # 10 mutually distinct boards
python3 tools/E555_viewer.py FILE --row 0 --no_url         # the picture ('#' = break)
```
The `rank` columns: `score`, `placed`, `break_rows`, `break_cols`, `span`,
`clean_b/clean_t/clean_l/clean_r` (clean complete rows/columns from each edge),
and `corner_d`. Its `border` measure (in `--csv`) is the clean *frame arc*
(frame seams included), not frame legality, so a legal board can show less
than 60.

**Knobs worth sweeping** (each tool's `--help` and its PROJECT_E555.md table):
- diver `--reopen`: the band `auto`, `auto+E` (E rows deeper), `top:K`,
  `box:R0-R1,C0-C1` or a mask file; `--copies`, `--rounds`, `--end_dive`,
  `--end_polish`, `--prior A --nogo B`, `--plateau`, `--orders`, `--wall_time`,
  `--rng_seed`, `--threads` (the output does not depend on the thread count).
- ender: `--profile {overnight,deep,superdeep}`, `--board_time_limit`,
  `--threads`, `--rng_seed`; advanced `--holes`. It accepts the old
  `--mode`/`--reach`/`--stall_time` flags only to ignore them.
- beamer: `--random_edges` or a rotations file, `--beam_width`,
  `--backtrack_row`, `--stop_row`, `--end_dive`/`--end_polish`,
  `--lambda_Mahalanobis`, `--clue_center`/`--clue_corners`, `--rng_seed`.
- topper: `--side {T,B,L,R,TR,TL,BR,BL,TB,LR}`, `--band_depth`, `--locked_rows`,
  `--top`, `--time_limit`, `--stall_time`.
- backtracker: `--break_mode any --breaks K`, `--holes FILE`,
  `--order {mrv,spiralout,frontier,…}`, `--time_limit`.

---

## 3. Measured heuristics (read the board before choosing a tool)

These come from measured runs on this seed (PROJECT_E555.md 9.2 and 9.3). Bake
them in; they save hours.

**H1: attack the breaks where they are.** On all 47 shipped boards (463–465) the
bottom 10–12 rows are clean and every break sits in the top 4–6 rows (`rank`:
`clean_b`, `break_rows`). Open only the region that holds breaks. Opening clean
sides wastes time and can spread a break into a clean row.

**H2: a gain needs the row under the damage lifted too.** The breaks a dive
leaves in the top rows are forced by the pieces left against the rows below. When
`--reopen auto` stops gaining, lift one row deeper (`auto+1`, or `top:K` with K
one more than the damaged rows).

**H3: on polished boards the redive wins, not CP-SAT windows.** On dived and
polished boards (457–463), every 4×4 window touching a break was already optimal
(78/78), and no exact region of 48–102 cells gained. The gains came from the
redive. The best measured reopen setting is `--copies 8 --end_dive 1000
--end_polish 10000`, and it kept gaining as the wall clock grew (120 s per board
beat 15 s). Spend CP-SAT windows (topper, ender windows) on unpolished boards.

**H4: live telemetry is not the score.** The topper's `[inc] breaks=N` counts
only the open band, and progress lines are per call. Only the referee counts.
Re-score every emitted file (contract rule 6).

**H5: diversity is fuel.** A plateaued board stays plateaued: 120 s of reopen
on each of the seven first 463s gained on one of them. The 463s and 464s came
from *new* starting boards (fresh Stage B runs, then dives and reopens). Rotate
between improving the elites and making new material, and pick distinct roots
with `rank --diverse K` or `--max_agree 0.9`.

**H6: match the tool to the board.**
- complete and polished (≥ 460) → diver `--reopen` (F1), then the ender `deep`
  (F2), then a longer reopen with `auto+1`;
- complete and unpolished (fresh from a beam without polish) → ender
  `overnight`, then reopen;
- partials (Stage B output, a corpus) → distiller (F3), then F1/F2 on its output;
- a small hole you want to *prove* → backtracker `--break_mode any` (F6).

---

## 4. One-time setup (do this by hand before `/goal`)

From the repository root:
```bash
bash agent/setup_lab.sh
```
It starts a new campaign in a lab of its own, `agent/labs/<id>/`, where `<id>`
is the UTC date and time (`20261027-1930`, with `-2` added if that name is
taken). It copies the toolchain into the lab (Makefile, src, tools, examples,
pipeline, data, the two docs, and this kit as `KIT.md`), builds the lab's
`bin/`, installs OR-Tools when Python lacks it (the ender and the topper), and
seeds the bookkeeping of section 6:
- `frontier/frontier.csv`, from `data/best_465.csv`;
- `frontier/elite.csv`, the 47 shipped boards;
- `scoreboard.json`, `run_log.csv`, `JOURNAL.md`;
- `STRATEGY.md`, with what was measured before the campaign.

It also writes this machine's facts into `scoreboard.json`: `threads` (what
`nproc` prints), `stage_b_ok` (8 GB of free RAM, for the chain database the
beamer builds), `db_on_disk` (`E555.db` is already in the lab, or 8 GB of free
disk can hold it), `ortools_ok`, the CPU model and the build's arch.
It ends with two lines:
```
REFEREE verdict=VALID score=465 breaks=15 placed=256 frame_violations=0 ...
SETUP campaign=20261027-1930 lab=fresh path=.../agent/labs/20261027-1930 arch=v3 cpu="..." ... ortools=yes stage_b_ok=true best=465 elite=47
```
If it fails, or `elite` is not 42, fix the setup before you start the goal.
`ortools=no` means no F2 and no topper: install OR-Tools and rerun, or accept
that. The script is safe to rerun: it repairs what is missing and never touches
the campaign's files, so it doubles as a health check. Optionally, run the
repo's own gate first (`bash tests/run_tests.sh`, about 30 s), which proves the
toolchain on this machine.

The build uses `ARCH=v3` (AVX2) where the CPU runs x86-64-v3, and `generic`
otherwise. Measured on a reopen run, v3 is as fast as `-march=native` and
generic is 45% slower, all three with byte-identical output. `ARCH=native` is
still available (`bash agent/setup_lab.sh ARCH=native`), but such a binary dies
with SIGILL on an older CPU.

### 4.1 Starter pool (optional, recommended)

The repo ships two corpora of Stage B partials:
`data/E565_FixCorners23.csv.gz` (64,324 row-11 partials, 30,565 unique) and
`data/E565_lowB_baseline.csv` (255 row-12 partials). The distiller turned the big
one into five distinct 462s in 47 minutes on 4 threads. Your own Stage B files,
plain or gzipped, go in `pool/`.

Distil once, before or during the run (it resumes where a killed run stopped, so
`timeout` plus a rerun is safe):
```bash
cd agent/labs/<id>             # the campaign's lab, as SETUP printed it
python3 tools/E555_distiller.py data/E565_FixCorners23.csv.gz --top 5 --out pool/distilled.csv
bash ../../referee.sh pool/distilled.csv
: > pool/consumed.txt          # ids the agent has already used as roots
```
Distilled boards land around 462, below the frontier. They are roots for F1/F2,
and their value is that they differ from the elites (H5).

### 4.2 Pre-flight and launch

- **Cloud session?** None of this section or the next applies: section 11.
- **Resources.** Stage B (F4) needs ~8 GB of free RAM for the chain database,
  plus ~7 GB of local disk to keep it in the lab between runs. Everything else
  is small. `setup_lab.sh` records the answers as `stage_b_ok` and
  `db_on_disk`.
- **A durable terminal:** `tmux new -s e555`.
- **Pick the permission mode** (section 5).
- **Launch from the lab** on the latest Opus at medium effort, in the mode you
  picked, with a Bash time limit long enough for one run. The Bash tool stops a
  foreground command after 2 minutes by default and 10 minutes at most; the
  kit's runs take up to 20:
  ```bash
  cd agent/labs/<id>             # the campaign's lab, as SETUP printed it
  BASH_DEFAULT_TIMEOUT_MS=1500000 BASH_MAX_TIMEOUT_MS=1500000 claude --permission-mode auto --model opus --effort medium
  ```
  On a laptop, keep the machine awake by putting `caffeinate -i` (macOS) or
  `systemd-inhibit --what=sleep:idle` (Linux) in front of `claude`.
- **Usage limits.** Opus at medium effort spends a 5-hour usage window in a few
  hours of campaign. When the limit stops a turn, Claude Code waits in the open
  session and continues the task on its own after the reset. It needs Claude
  Code v2.1.234 or later and a claude.ai sign-in, and it is on by default
  (`/config`: "Continue automatically at usage limit"). The campaign's clocks
  count run time, so the wait costs nothing. For the wait to work:
  - keep the session open: `tmux`, and no exit;
  - keep Auto mode, since a permission prompt would stop the continued turn;
  - after a sleep of more than about 30 minutes past the reset, press `Enter`.
  If continuations keep hitting the limit, Claude Code re-arms the wait twice
  in a row and then stops. It never waits on its own for a limit more than 24
  hours away, such as a weekly limit. In either case, run `/rate-limit-options`
  and pick **Wait here, then continue automatically**, or send `continue` after
  the reset.
- Threads: the number `nproc` prints (`setup_lab.sh` records it as `threads`),
  never a guess.

---

## 5. Permissions: pick one (before `/goal`)

`/goal` does not change the permission mode. In Manual mode each goal turn stops
at the first action your settings do not allow.

**A. Auto mode (recommended):** `claude --permission-mode auto`. In recent Claude
Code it is the default starting mode (`Shift+Tab` cycles modes). A classifier
reviews each action instead of you. It needs a model that supports auto mode,
and your organization can turn it off.

**B. Isolated container or VM:** `claude --dangerously-skip-permissions`, as a
non-root user. Use it only on a machine where nothing else matters.

**C. Accept-edits plus an allowlist:** `claude --permission-mode acceptEdits`
accepts file edits inside the working directory (the lab). Add Bash rules with
`/permissions`, for example:
```
Bash(bin/E555_diver *)  Bash(bin/E555_beamer *)  Bash(bin/E555_finalizer *)
Bash(bin/E555_backtracker *)  Bash(bin/E555_roundhouse *)
Bash(python3 tools/*)  Bash(python3 src/*)  Bash(bash examples/*)  Bash(bash pipeline/*)
Bash(bash ../../referee.sh *)  Bash(make *)  Bash(date *)  Bash(nproc)
```
A rule also matches the command behind a `timeout` prefix. The kit writes run
directories and thread counts literally, never as shell variables, so these
rules can match. Be clear about what these rules mean: a rule that runs an
interpreter or a lab-built binary lets the agent run any code it writes there,
so C is a convenience, not a sandbox.

---

## 6. Bookkeeping the agent must maintain (this is how you evaluate progress)

Everything under the lab, `agent/labs/<id>/`:
- **`scoreboard.json`**: the current best, `goal_start_unix`, `last_best_unix`,
  `families_since_best` (the distinct families run since the last new best),
  `total_runs`, and `history[]` (one entry per accepted best: unix, score,
  breaks, family, tool, params, file). `setup_lab.sh` keeps the machine's facts
  there (`threads`, `stage_b_ok`, `db_on_disk`, `ortools_ok`, `cpu`, `arch`,
  `ram_gb`, `disk_gb`), and `checkpoint.sh` keeps `last_checkpoint_unix`.
- **`run_log.csv`**: **every** run, improvement or not:
  `unix,iso,family,tool,params,input,output,score,breaks,verdict,runtime_s,note`,
  with score and verdict from the referee. No field may contain a comma (write
  params and notes with spaces or `;`), because the file is read with
  `awk -F,`.
- **`frontier/frontier.csv`**: the single current best board, always VALID.
  Keep a timestamped copy (`frontier/<ts>_<score>.csv`) whenever it advances.
- **`frontier/elite.csv`**: up to 50 distinct VALID complete boards, best first,
  the roots for F1/F2. Add each new best and each equal or near-best (≥ best − 2)
  alternative. Keep it distinct with
  `python3 tools/E555_rank.py frontier/elite.csv --max_agree 0.95 --top 50 --out frontier/elite.csv.new --rescore --quiet`,
  then move `elite.csv.new` over `elite.csv`.
- **`JOURNAL.md`**: append-only narrative, one short block per experiment:
  **hypothesis → exact command → referee line and shape → decision**.
- **`STRATEGY.md`**: the ledger of families and settings tried and exhausted,
  so neither the agent nor you re-plough the same ground.
- **`runs/<ts>_<tool>/`**: raw outputs. Route every tool's output to
  `log.txt` there and read it with `tail` or `grep`, never `cat`: logs are long,
  and tokens are the budget.

**The status line.** The `/goal` evaluator only sees the conversation; it cannot
read files. So every turn ends with the line `bash ../../status.sh` prints, run in
that turn, followed by ` next=<next move>`:
```
STATUS best=465/480 breaks=15 runs=12 run_time=2h05m/8h00m since_best=0h40m/1h00m families_since_best=2/3 stop=none next=<next move>
```
The first call passes the settings (`bash ../../status.sh BUDGET=8h PLATEAU=1h/3`),
and later calls reuse them from `scoreboard.json`; the first call also sets
`goal_start_unix` when it is null. The clocks are **run time**: `run_time` sums
`runtime_s` over the `run_log.csv` rows logged since `goal_start_unix`, and
`since_best` sums it over the rows logged after `last_best_unix`. A usage-limit
wait, a restart or the agent's own thinking never counts, so a campaign that
waits out a 5-hour window resumes with its budget and plateau intact. `stop=`
names the stop rule that holds: `480`, `budget`, or `plateau` (since_best and
families_since_best both at their limits); otherwise `none`. So `unix` in
`run_log.csv` is when the row was logged, after the run, and `runtime_s` is the
run's wall seconds.

**`SUMMARY.md`** (written when a stop rule holds): the final referee line; the
winning family, tool, exact command and file; the five best runs from
`run_log.csv`; the score-over-time curve (`awk -F, 'NR>1{print $2, $8}'
run_log.csv`); the families exhausted (from `STRATEGY.md`); and three concrete,
specific things to try next with more time or a bigger machine.

---

## 7. The iteration loop (what the agent does each turn)

A fixed, prescriptive cycle.

0. **First turn only:** run `bash ../../status.sh` with the goal's settings
   (`BUDGET=8h PLATEAU=1h/3` unless the goal says otherwise). It stores them
   and sets `goal_start_unix`. `threads` and `stage_b_ok` come from
   `setup_lab.sh` (section 4). If the machine may have changed, run
   `bash ../../setup_lab.sh` again (from the lab it checks this campaign): it
   rebuilds what the new CPU needs and rewrites them.
1. **Orient.** Read `scoreboard.json`, the last few `JOURNAL.md` blocks and
   `STRATEGY.md`. Run the referee on `frontier/frontier.csv` and `rank` on it
   for the shape.
2. **Choose a family** from the table below by H1–H6 and `STRATEGY.md`. Do not
   run the same family with the same settings twice; vary the band, the seed,
   the budget or the root. Rotate so that at least three different families run
   before you conclude a plateau.
3. **Run it, bounded.** Make a fresh run directory and write its name out
   literally in every command (shell variables do not survive from one Bash
   call to the next): `date +%Y%m%d_%H%M%S` gives the stamp, then
   `mkdir -p runs/20261021_140501_reopen`. Below, `<D>` stands for that
   directory and `<T>` for `threads`. Run `timeout 1200 <command> > <D>/log.txt
   2>&1`. Give the tool its own clock a little under that (`SECONDS_PER_BOARD`,
   `--board_time_limit`, `MAX_WALL`, `--wall_time`), so it stops cleanly and
   writes its output. Raise the 1200 s
   only with a one-line reason in the journal. If the Bash tool cannot wait
   that long, start the run in the background and end the turn; its result
   arrives as the next turn.
4. **Referee the output** (`bash ../../referee.sh <D>/out.csv`) and append a
   `run_log.csv` row **whether or not it improved**. Increment `total_runs`.
5. **On a new best** (VALID and above `best_score`): update `scoreboard.json`
   (best, `last_best_unix`, empty `families_since_best`, a `history` entry),
   copy the file to `frontier/frontier.csv` plus a timestamped copy, add it to
   `elite.csv`, and **chain the closers** on it before anything else: F1 with
   `REOPEN=auto`, then F1 with `REOPEN=auto+1`, then F2 `deep`. Otherwise, add
   the run's family to `families_since_best` if it is new there.
6. **Journal it** (hypothesis, command, referee line, decision), and update
   `STRATEGY.md` when a family or setting is exhausted.
7. **End the turn with the STATUS line** (section 6).

**The families.** `<S>` is a fresh `--rng_seed`/`RNG_SEED` for every run (log
it); `<root.csv>` is a file of complete boards: the frontier, `elite.csv`, a
distilled pool, or a slice of one (`python3 tools/E555_rank.py frontier/elite.csv
--diverse 4 --out <D>/roots.csv`). When you take roots from a distilled pool,
append their ids to `pool/consumed.txt` and skip the ids already there.

| family | when | command (from the lab) |
|---|---|---|
| **F1 reopen** | any complete root | `timeout 1200 bash examples/11_diver_reopen.sh BOARDS=<root.csv> OUT=<D>/out.csv SECONDS_PER_BOARD=<1100 / boards> THREADS=<T> RNG_SEED=<S> REOPEN=<auto, auto+1, top:5 or top:6>` |
| **F2 ender** | a complete root after F1 stalls | `timeout 1200 python3 src/C_tail/E555_ender.py data/seed_Edge5.txt <root.csv> <D>/out.csv --profile deep --board_time_limit <1000 / boards> --threads <T> --rng_seed <S>` |
| **F3 distil** | a corpus not yet distilled (4.1) | `timeout 1200 python3 tools/E555_distiller.py <corpus> --top 5 --out pool/distilled_<k>.csv`; rerun the same command until it finishes; its boards then go to F1/F2 |
| **F4 beam** | `stage_b_ok` is true; new raw material | `timeout 1200 bash examples/01_beamer_quickstart.sh OUT_DIR=<D> THREADS=<T> BEAM_WIDTH=10000 BACKTRACK_ROW=6 STOP_ROW=11 END_DIVE=10000 END_POLISH=20000 MAX_WALL=1100 RNG_SEED=<S> DB_FILE=E555.db`, leaving out `DB_FILE` when `db_on_disk` is false (with `CLUES=1`, use `DB_FILE=E555_clues.db`); the files it wrote are listed in `<D>/outputs.txt`, and its boards go to F1 |
| **F5 rebuild** | an elite F1 cannot move | `timeout 1200 bin/E555_diver data/seed_Edge5.txt <root.csv> <D>/out.csv --holes top:6 --stop_row 11 --end_dive 10000 --end_polish 20000 --emit_score 455 --wall_time 1100 --threads <T> --rng_seed <S>` |
| **F6 exact** | an unpolished board, or a small hole to prove | `timeout 1200 bash pipeline/topper_sweep.sh INPUT=<root.csv> PRESET=safe OUT=<D>/out.csv OUT_DIR=<D> WORKERS=<T>`, or `timeout 1200 bin/E555_backtracker data/seed_Edge5.txt <root.csv> <D>/out.csv --holes <D>/mask.csv --break_mode any --breaks <K> --time_limit 1100 --threads <T>`, with a mask you write (16 lines of 16 `0`/`1`, first line = row 0, under ~30 open cells around the breaks). An exhausted level K proves that no filling of the hole has K or fewer breaks; a search that runs out of time writes the board with the hole still open, which the referee calls `INVALID reason=incomplete`: log it as a run that found nothing |
| **F7 tool** | an idea none of the above covers | write it in the lab's `tools/`, check it against the referee on `data/best_46*.csv` first, then run it like any other family |

The first F4 run in a lab builds the chain database before it searches, inside
its cap, and writes `E555.db`; later runs load that file in seconds. Without
`DB_FILE`, every F4 run rebuilds the database in memory. Measured: one
5-minute F4 run on 4 cores wrote 3,077 boards, the best 462. F5 lifts cells, so
its output can score below its input. It is still a run: referee it and log it.

**Ideas the agent is encouraged to build** (all legitimate, all in the lab): a
portfolio allocator that gives more budget to the family that is gaining; a
diversity sampler over `elite.csv` and the distilled pools; a reopen schedule
that alternates bands per round (the diver's `--reopen` takes several specs).
Keep parallelism **simple**: the tools' own `--threads`, or plain background
jobs. Never hand-roll work-stealing, and never weaken the backtracker's exact
search to make it faster.

---

## 8. The `/goal` block (paste as one block, no edits needed)

```
/goal Run an unattended optimization campaign on the E555 Eternity II puzzle data/seed_Edge5.txt. A board scores matched internal edges out of 480; breaks = 480 - score. The best known board, 465/480, is frontier/frontier.csv. Mission: a referee-VALID board above 465, as close to 480 as the tools allow, with every step documented.

The working directory is this campaign's lab, agent/labs/<id>/: the folder Claude Code started in. Other campaigns' labs and checkpoints are not yours. KIT.md is your manual: read its sections 1-7 now, and again after any context compaction, and follow them exactly. Read an examples/*.sh header before first using its tool.

HARD RULES (breaking any voids the run):
- The official score of a board file is the line `bash ../../referee.sh FILE` prints. Never edit anything outside this lab: not the referee, not the repository's tools, not its seed. Your lab copies may change; their numbers never count.
- Never treat synth_seed.txt or synth_solution_480.csv as the puzzle; they are self-test fixtures.
- Every frontier board comes from a command logged in run_log.csv. No hand-written boards. Log every run with its real referee result, improved or not.
- A new best needs verdict=VALID and a score above best_score, from a fresh referee call on the emitted file.
- Write only inside this lab. No git command that changes the repository. Keep parallelism to the tools' own --threads or plain background jobs.

EACH TURN: follow KIT.md section 7. Orient, choose a family by the heuristics of section 3, run it bounded (timeout 1200, output in a new runs/<ts>_<tool>/), referee the output, log it, update scoreboard.json and the frontier on a new best, chain the closers on it, and journal it. On the first turn, set goal_start_unix as section 7 step 0 says; threads and stage_b_ok are in scoreboard.json.

END EVERY TURN with the line `bash ../../status.sh BUDGET=8h PLATEAU=1h/3` prints in that same turn, followed by next=<next move>. Its clocks count run time, so usage-limit waits do not count.

The goal is met only when SUMMARY.md has been written as KIT.md section 6 describes and its first ten lines are printed in the transcript, after one of these held in this conversation:
(a) the referee printed verdict=VALID score=480;
(b) a STATUS line showed stop=budget or stop=plateau.
If a usage limit stops a turn, continue the campaign where it stopped once the limit resets. Until then, keep working.
```

Tune before pasting only if you want to: the `1200` s per-run cap, and the
`BUDGET=8h` run-time budget and `PLATEAU=1h/3` plateau rule (one hour of run
time without a new best, after at least three families). Everything else is
repo-correct as written. The condition is under the 4,000-character limit of
`/goal`; keep it there if you edit it.

---

## 9. While it runs / after it stops

- `/goal` with no argument shows the condition, how long it has run, the turns
  evaluated, the token spend and the evaluator's last reason (`Ctrl+O` on a
  verdict shows the reason too). `/goal clear` stops it early.
- Follow the story without spending tokens: `tail -f agent/labs/<id>/JOURNAL.md`,
  `column -s, -t agent/labs/<id>/run_log.csv | tail`, and
  `bash agent/referee.sh agent/labs/<id>/frontier/frontier.csv` from the repo root.
- The frontier board is always `agent/labs/<id>/frontier/frontier.csv`. View it with
  `python3 tools/E555_viewer.py agent/labs/<id>/frontier/frontier.csv` from the repo
  root (the viewer's URL opens it on e2.bucas.name).
- **A usage limit** pauses the campaign until the window resets, then Claude
  Code continues it (section 4.2). Nothing is lost: the clocks count run time.
- **Resume another day:** `cd agent/labs/<id>` and relaunch with the same command
  plus `--continue`. Claude Code restores an active goal, but restarts its own
  timer and turn count, which is why the kit keeps its clocks in
  `scoreboard.json` and `run_log.csv`. They count run time, so downtime costs
  nothing. To give the run a fresh budget, set `goal_start_unix` and
  `last_best_unix` to null and empty `families_since_best`. In a new
  session, paste section 8 again; the scoreboard, frontier, elite file,
  `run_log.csv` and `STRATEGY.md` are all the state it needs. A cloud session
  resumes differently, from a checkpoint: section 11.
- **Keeping a result:** the agent never writes outside the lab. Copy a new
  best into `data/` yourself, after `bash agent/referee.sh` confirms it.

## 10. Realistic expectations

465 → 466 is the honest near-term target. The record boards came from long
reopens and redives of the right root: of the seven first 463s, given 120 s of
reopen each, only the first moved (by 2; that board is also the root of the
465). Expect long flat stretches; that *is* the shape of edge-matching search.
The run's deliverable is twofold: the best
board found, and a documented map of what worked and what didn't, so the next
run (or the next machine) starts smarter. That documentation is the win even if
480 stays out of reach.

---

## 11. Cloud sessions (claude.ai/code, the Claude app)

In a cloud session, Claude Code runs in a container on a machine you do not
choose. Do not do sections 4, 4.2, 5 and 8 by hand. Instead:
1. Start a new session on this repository.
2. Pick the latest Opus and Auto mode.
3. Send `/effort medium`.
4. Paste the `/goal` line of **`agent/launch_cloud_agent.md`**.

On its first turn the agent runs `setup_lab.sh` itself. Here is what differs
from a terminal, and how the launcher handles it:

| in a cloud session | the launcher's answer |
|---|---|
| No command-line flags: the model and the permission mode come from the session's pickers, so the launch line of 4.2 and section 5 do not apply. Bypass permissions is not offered | you pick Opus and Auto, and send `/effort medium` (`/model` and `/effort` take their value as an argument in a cloud session) |
| A usage limit stops the turns until the 5-hour window resets. The docs promise an automatic wait only for interactive terminal sessions, and an idle cloud VM is reclaimed | on its first turn the agent creates an hourly scheduled task that wakes this session. A wake-up after the reset continues the campaign, restoring the lab from its checkpoint if the VM was reclaimed. The clocks count run time (section 6), so the wait costs nothing. The agent deletes the task when the campaign ends |
| The Bash tool stops a foreground command after 2 minutes by default and 10 at most, and `BASH_*_TIMEOUT_MS` cannot be set at launch | `RUN_CAP` (default 540 s) keeps each run in the foreground. A larger cap sends runs to the background, where the tool allows up to 2 hours |
| Limited compute: the container this kit was measured in had 4 cores, 15 GB of RAM and a 30 GB disk allowance, and yours may have less | the health check at the start of every turn measures cores, free RAM and disk. F4 runs only with 8 GB of free RAM, and keeps its database in the lab only when the disk has room |
| The session can move to another machine, with another CPU, where a `-march=native` binary dies with SIGILL | the lab builds for x86-64-v3 (AVX2), which runs on any current server and here ran as fast as native, and falls back to generic below that or after a SIGILL. A new CPU shows as `cpu_changed=yes`, and the agent takes the new `threads` |
| The container can be replaced (idle, restarts). `agent/labs/` is gitignored, so the lab goes with the container, runs in flight included | `agent/checkpoint.sh` publishes the campaign's `agent/checkpoints/<id>/` (scoreboard, logs, journal, strategy, frontier, elite, distilled pools) to the shared results branch, `agent_runs`, on every new best and every `CHECKPOINT` (30 min). The health check restores a lost lab from it. The chain database is never checkpointed or committed: a new container rebuilds it in its first F4 run, and `checkpoint.sh` refuses any file over 50 MB |
| A fresh container has no OR-Tools | `setup_lab.sh` installs it. Without it, F2 and the topper are skipped |

**Settings** go in the pasted line: `BUDGET` (8h of run time), `RUN_CAP` (540),
`PLATEAU` (1h/3), `CHECKPOINT` (30m), `RESULTS_BRANCH` (agent_runs), `CAMPAIGN` (new)
and `RESUME` (none), plus free-text constraints such
as `no Stage B`, `THREADS=2` or `stop at the first VALID board above 465`. The
launcher's table explains each one.

**Each campaign is its own folder.** A new session starts a new campaign,
named by its UTC start time: its lab is `agent/labs/<id>/` and its checkpoint
`agent/checkpoints/<id>/`. Campaigns you run later never write into an earlier
one's folders.

**The results branch.** Checkpoints go to `RESULTS_BRANCH`, `agent_runs` by
default, through `agent/publish.sh`. That branch holds results only: it
shares no history with `main`, is never merged, and its `README.md` says so.
Each campaign of every kit owns one folder on it; several agents can push at
the same time, each replacing only its own folder; and each commit names the
code it ran (`Code: <sha> on <branch>`). The session's own branch gets no
commit, and code branches ignore `agent/checkpoints/`.

**Resume in any session:** paste the line with `CAMPAIGN=<id>
RESUME=agent_runs`. The campaign keeps its id and continues from its last
checkpoint with a fresh `BUDGET` (add `keep the clock` to keep the old one).
If the STATUS lines stop for more than an hour after a usage limit has reset,
the goal may have been lost with the VM: paste the same line again in the same
session, with `keep the clock`. A lab that is still there is kept, a lost one
is restored, and the old wake-up task is replaced.

**Keeping a result:** the best board is
`agent/checkpoints/<id>/frontier/frontier.csv` on the results branch
(`git fetch origin agent_runs`, then
`git show origin/agent_runs:agent/checkpoints/<id>/frontier/frontier.csv`).
Confirm it with `bash agent/referee.sh FILE` and copy it into `data/` on `main`
by hand. A campaign's checkpoint folder is also its record (journal, run log,
strategy, summary).

**Tested so far,** in a cloud container:
- `setup_lab.sh`: a fresh setup in 32 s, and a health check in under a second;
- `checkpoint.sh` through `publish.sh`: a refusal to publish to `main`, a
  results branch created with its README, two campaigns of the two kits
  pushing to it at the same time, and a restore from it;
- two campaigns started in the same minute (`-2` added to the second), each
  with its own lab and checkpoint on one branch; a restore that asks which
  campaign when the branch holds two, and picks the only one otherwise;
- a simulated CPU change, and a binary that dies with SIGILL;
- `status.sh` on a log with a 5-hour gap, which did not count.

A whole campaign under `/goal` in a cloud session has not been run, and neither
has a wake-up after a real usage limit.
