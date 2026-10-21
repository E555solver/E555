# E555 Autonomous Solver — `/goal` Kit (v3)

**WARNING: this is untested experimental territory for learning about AI agent capabilities.**
A ready-to-paste `/goal` setup that turns one interactive Claude Code session
into a self-driving, multi-hour optimizer over the *real* E555 toolchain in this
repo. Do the setup (sections 4 and 5), paste section 8, walk away, and come back
to a documented campaign.

Written to be driven by the economical **Sonnet** model on a Pro subscription
(a long run, with tokens that refill on a flat cadence). Sonnet is capable but
literal, so this kit is deliberately *prescriptive*: exact commands, explicit
budgets, a fixed decision procedure, and hard guardrails. When in doubt, the
agent follows the recipe rather than improvising.

v3 changes from v2: the current Stage C tools (the diver's `--reopen`, the
distiller, the ender's profiles), a referee script the agent cannot edit, a
lab that is a self-contained checkout, and a `/goal` condition that fits the
4,000-character limit.

---

## 0. The one-paragraph mission

The puzzle is the 16×16, 256-piece Eternity II board defined by
`data/seed_Edge5.txt`. A board scores **matched internal edges out of 480**, and
`breaks = 480 − score`. **480 = solved.** Nobody has solved it in public: the
best public board since 2007 matches 470. This repo's best is **465/480**
(`data/best_465.csv`, 15 breaks), followed by three 464s (`data/best_464.csv`)
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
- The session runs **in `agent/lab/`** (section 4.2 launches Claude Code there),
  so the permission system itself treats the rest of the repo as outside the
  working directory. Read anything in the repo; **write only inside
  `agent/lab/`**.
- No git command that changes the repository (no commit, push, checkout, reset,
  stash or clean). `agent/lab/` is gitignored.
- The beamer's 6.5 GB chain-database cache, if used, goes to `agent/lab/cache/`
  on **local** disk, never on a network drive.
- You **may** edit the lab's copies of the source, write new tools in
  `agent/lab/tools/`, and rebuild the lab's binaries. Improve anything you like:
  samplers, allocators, heuristics.

**Anti-cheating contract** (the run is worthless if any of these is broken)
1. The puzzle is `data/seed_Edge5.txt`. `synth_seed.txt` and
   `synth_solution_480.csv` are a *self-test fixture* (solvable by
   construction). Solving them is **not** progress and never touches the
   scoreboard.
2. **The referee decides.** The official score of a board file is the line
   `bash ../referee.sh FILE` prints. `agent/referee.sh` runs the repo's own
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

Every command below runs from `agent/lab/`, which is a small checkout of its
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
bash ../referee.sh FILE                       # official: REFEREE verdict=VALID score=465 breaks=15 ...
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
  `--lambda_Mahalanobis`, `--clue_center`/`--clue_corners`, `--rng_seed`. The
  database cache depends on the clue flags, so use one cache file per clue
  setting.
- topper: `--side {T,B,L,R,TR,TL,BR,BL,TB,LR}`, `--band_depth`, `--locked_rows`,
  `--top`, `--time_limit`, `--stall_time`.
- backtracker: `--break_mode any --breaks K`, `--holes FILE`,
  `--order {mrv,spiralout,frontier,…}`, `--time_limit`.

---

## 3. Measured heuristics (read the board before choosing a tool)

These come from measured runs on this seed (PROJECT_E555.md 9.2 and 9.3). Bake
them in; they save hours.

**H1: attack the breaks where they are.** On all 42 shipped boards (463–465) the
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

```bash
cd <repo root>                                   # the folder with agent/, src/, data/
mkdir -p agent/lab
cp -r Makefile src tools examples pipeline data PROJECT_E555.md README.md agent/lab/
cp agent/autonomous_agent_mode.md agent/lab/KIT.md   # the agent's manual
cd agent/lab
mkdir -p cache runs frontier pool
make                       # the lab's own bin/; tuned for this CPU (ARCH=generic only
                           # if the binaries must run on another machine)
pip install ortools        # the ender and the topper
python3 -c 'import ortools; print("ortools", ortools.__version__)'

# the bookkeeping
cp data/best_465.csv frontier/frontier.csv                       # start from the known best
python3 tools/E555_rank.py data/best_46[345].csv --out frontier/elite.csv --rescore --quiet
cat > scoreboard.json <<'JSON'
{ "puzzle": "seed_Edge5", "perfect_score": 480,
  "baseline_score": 465, "best_score": 465, "best_breaks": 15,
  "best_source": "data/best_465.csv (shipped baseline)",
  "best_file": "frontier/frontier.csv",
  "total_runs": 0, "goal_start_unix": null, "last_best_unix": null,
  "families_since_best": [], "threads": null, "stage_b_ok": null, "history": [] }
JSON
printf 'unix,iso,family,tool,params,input,output,score,breaks,verdict,runtime_s,note\n' > run_log.csv
: > JOURNAL.md
cat > STRATEGY.md <<'EOF'
# Strategy ledger

Measured before this campaign (PROJECT_E555.md 9.2, 9.3); re-test only with a new idea:
- CP-SAT windows around the breaks of polished boards (457-463): already optimal
  (78/78 4x4 windows); exact regions of 48-102 cells gained nothing.
- diver --reopen auto on the seven first 463s of data/best_463.csv, 120 s each:
  the first gained 2 (that board is also the root of data/best_465.csv), the
  other six gained nothing.
- Best measured reopen setting: --copies 8 --end_dive 1000 --end_polish 10000;
  it kept gaining from 15 s to 120 s per board.

## Families tried in this campaign
EOF
```

**Check the referee and the lab before trusting anything:**
```bash
bash ../referee.sh frontier/frontier.csv
# must print: REFEREE verdict=VALID score=465 breaks=15 placed=256 frame_violations=0 ...
python3 tools/E555_rank.py frontier/elite.csv --count     # 42
```
If either differs, stop and fix the setup; do not start the goal. Optionally,
run the repo's own gate from the repo root first (`bash tests/run_tests.sh`,
about 30 s), which proves the toolchain on this machine.

### 4.1 Starter pool (optional, recommended)

The repo ships two corpora of Stage B partials:
`data/E565_FixCorners23.csv.gz` (64,324 row-11 partials, 30,565 unique) and
`data/E565_lowB_baseline.csv` (255 row-12 partials). The distiller turned the big
one into five distinct 462s in 47 minutes on 4 threads. Your own Stage B files,
plain or gzipped, go in `pool/`.

Distil once, before or during the run (it resumes where a killed run stopped, so
`timeout` plus a rerun is safe):
```bash
python3 tools/E555_distiller.py data/E565_FixCorners23.csv.gz --top 5 --out pool/distilled.csv
bash ../referee.sh pool/distilled.csv
: > pool/consumed.txt          # ids the agent has already used as roots
```
Distilled boards land around 462, below the frontier. They are roots for F1/F2,
and their value is that they differ from the elites (H5).

### 4.2 Pre-flight and launch

- **Resources.** Stage B (F4) needs ~8 GB of free RAM and ~6.5 GB of local disk
  for `cache/`. Everything else is small. The agent records the answer as
  `stage_b_ok` in iteration 0 (`free -g`, `df -h .`).
- **A durable terminal:** `tmux new -s e555`.
- **Pick the permission mode** (section 5).
- **Launch from the lab**, in the mode you picked, with a Bash time limit long
  enough for one run. The Bash tool stops a foreground command after 2 minutes
  by default and 10 minutes at most; the kit's runs take up to 20:
  ```bash
  cd agent/lab
  BASH_DEFAULT_TIMEOUT_MS=1500000 BASH_MAX_TIMEOUT_MS=1500000 claude --permission-mode auto --model sonnet
  ```
- Threads: the number `nproc` prints (the agent records it as `threads` in
  iteration 0), never a guess.

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
Bash(bash ../referee.sh *)  Bash(make *)  Bash(date *)  Bash(nproc)
```
A rule also matches the command behind a `timeout` prefix. The kit writes run
directories and thread counts literally, never as shell variables, so these
rules can match. Be clear about what these rules mean: a rule that runs an
interpreter or a lab-built binary lets the agent run any code it writes there,
so C is a convenience, not a sandbox.

---

## 6. Bookkeeping the agent must maintain (this is how you evaluate progress)

Everything under `agent/lab/`:
- **`scoreboard.json`**: the current best, `goal_start_unix`, `last_best_unix`,
  `families_since_best` (the distinct families run since the last new best),
  `stage_b_ok`, `total_runs`, and `history[]` (one entry per accepted best:
  unix, score, breaks, family, tool, params, file).
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
read files. So every turn ends with this line, its values computed by commands
in that turn (elapsed = `date +%s` − `goal_start_unix`; since_best uses
`last_best_unix`, or `goal_start_unix` before the first new best):
```
STATUS best=465/480 breaks=15 runs=12 elapsed=2h05m since_best=0h40m families_since_best=2 next=<next move>
```

**`SUMMARY.md`** (written when a stop rule holds): the final referee line; the
winning family, tool, exact command and file; the five best runs from
`run_log.csv`; the score-over-time curve (`awk -F, 'NR>1{print $2, $8}'
run_log.csv`); the families exhausted (from `STRATEGY.md`); and three concrete,
specific things to try next with more time or a bigger machine.

---

## 7. The iteration loop (what the agent does each turn)

A fixed, prescriptive cycle.

0. **First turn only:** if `goal_start_unix` is null, set it to `date +%s`; if
   `threads` is null, set it to what `nproc` prints; if `stage_b_ok` is null,
   set it from `free -g` and `df -h .` (true only with ≥ 8 GB free RAM and
   ≥ 7 GB free disk).
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
4. **Referee the output** (`bash ../referee.sh <D>/out.csv`) and append a
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

| family | when | command (from `agent/lab`) |
|---|---|---|
| **F1 reopen** | any complete root | `timeout 1200 bash examples/11_diver_reopen.sh BOARDS=<root.csv> OUT=<D>/out.csv SECONDS_PER_BOARD=<1100 / boards> THREADS=<T> RNG_SEED=<S> REOPEN=<auto, auto+1, top:5 or top:6>` |
| **F2 ender** | a complete root after F1 stalls | `timeout 1200 python3 src/C_tail/E555_ender.py data/seed_Edge5.txt <root.csv> <D>/out.csv --profile deep --board_time_limit <1000 / boards> --threads <T> --rng_seed <S>` |
| **F3 distil** | a corpus not yet distilled (4.1) | `timeout 1200 python3 tools/E555_distiller.py <corpus> --top 5 --out pool/distilled_<k>.csv`; rerun the same command until it finishes; its boards then go to F1/F2 |
| **F4 beam** | `stage_b_ok` is true; new raw material | `timeout 1200 bash examples/01_beamer_quickstart.sh OUT_DIR=<D> THREADS=<T> BEAM_WIDTH=10000 BACKTRACK_ROW=6 STOP_ROW=11 END_DIVE=10000 END_POLISH=20000 MAX_WALL=1100 RNG_SEED=<S> DB_FILE=cache/E555.db` (with `CLUES=1`, use `DB_FILE=cache/E555_clues.db`); the files it wrote are listed in `<D>/outputs.txt`, and its boards go to F1 |
| **F5 rebuild** | an elite F1 cannot move | `timeout 1200 bin/E555_diver data/seed_Edge5.txt <root.csv> <D>/out.csv --holes top:6 --stop_row 11 --end_dive 10000 --end_polish 20000 --emit_score 455 --wall_time 1100 --threads <T> --rng_seed <S>` |
| **F6 exact** | an unpolished board, or a small hole to prove | `timeout 1200 bash pipeline/topper_sweep.sh INPUT=<root.csv> PRESET=safe OUT=<D>/out.csv OUT_DIR=<D> WORKERS=<T>`, or `timeout 1200 bin/E555_backtracker data/seed_Edge5.txt <root.csv> <D>/out.csv --holes <D>/mask.csv --break_mode any --breaks <K> --time_limit 1100 --threads <T>`, with a mask you write (16 lines of 16 `0`/`1`, first line = row 0, under ~30 open cells around the breaks). An exhausted level K proves that no filling of the hole has K or fewer breaks; a search that runs out of time writes the board with the hole still open, which the referee calls `INVALID reason=incomplete`: log it as a run that found nothing |
| **F7 tool** | an idea none of the above covers | write it in `agent/lab/tools/`, check it against the referee on `data/best_46*.csv` first, then run it like any other family |

The first F4 run builds the database cache (about 100 s on 4 cores); later runs
map it in seconds. F5 lifts cells, so its output can score below its input. It
is still a run: referee it and log it.

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

The working directory is agent/lab. KIT.md is your manual: read its sections 1-7 now, and again after any context compaction, and follow them exactly. Read an examples/*.sh header before first using its tool.

HARD RULES (breaking any voids the run):
- The official score of a board file is the line `bash ../referee.sh FILE` prints. Never edit anything outside agent/lab: not the referee, not the repository's tools, not its seed. Your lab copies may change; their numbers never count.
- Never treat synth_seed.txt or synth_solution_480.csv as the puzzle; they are self-test fixtures.
- Every frontier board comes from a command logged in run_log.csv. No hand-written boards. Log every run with its real referee result, improved or not.
- A new best needs verdict=VALID and a score above best_score, from a fresh referee call on the emitted file.
- Write only inside agent/lab. No git command that changes the repository. Keep parallelism to the tools' own --threads or plain background jobs.

EACH TURN: follow KIT.md section 7. Orient, choose a family by the heuristics of section 3, run it bounded (timeout 1200, output in a new runs/<ts>_<tool>/), referee the output, log it, update scoreboard.json and the frontier on a new best, chain the closers on it, and journal it. On the first turn, set goal_start_unix, threads and stage_b_ok as section 7 step 0 says.

END EVERY TURN with this line, its values computed by commands in that same turn:
STATUS best=<score>/480 breaks=<n> runs=<total_runs> elapsed=<h>h<mm>m since_best=<h>h<mm>m families_since_best=<k> next=<next move>

The goal is met only when SUMMARY.md has been written as KIT.md section 6 describes and its first ten lines are printed in the transcript, after one of these held in a STATUS or REFEREE line of this conversation:
(a) the referee printed verdict=VALID score=480;
(b) elapsed reached 8h00m;
(c) since_best reached 1h00m with families_since_best at least 3.
Until then, keep working.
```

Tune before pasting only if you want to: the `1200` s per-run cap, the `8h00m`
total budget, and the `1h00m` / 3-family plateau rule. Everything else is
repo-correct as written. The condition is under the 4,000-character limit of
`/goal`; keep it there if you edit it.

---

## 9. While it runs / after it stops

- `/goal` with no argument shows the condition, how long it has run, the turns
  evaluated, the token spend and the evaluator's last reason (`Ctrl+O` on a
  verdict shows the reason too). `/goal clear` stops it early.
- Follow the story without spending tokens: `tail -f agent/lab/JOURNAL.md`,
  `column -s, -t agent/lab/run_log.csv | tail`, and
  `bash agent/referee.sh agent/lab/frontier/frontier.csv` from the repo root.
- The frontier board is always `agent/lab/frontier/frontier.csv`. View it with
  `python3 tools/E555_viewer.py agent/lab/frontier/frontier.csv` from the repo
  root (the viewer's URL opens it on e2.bucas.name).
- **Resume another day:** `cd agent/lab` and relaunch with the same command
  plus `--continue`. Claude Code restores an active goal, but restarts its own
  timer and turn count, which is why the kit keeps its clock in
  `scoreboard.json`. That clock includes the downtime: set `goal_start_unix`
  (and `last_best_unix`) to null to give the run a fresh 8 hours. In a new
  session, paste section 8 again; the scoreboard, frontier, elite file,
  `run_log.csv` and `STRATEGY.md` are all the state it needs.
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
