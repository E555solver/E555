# E555 Autonomous Solver — `/goal` Kit (v2)

**WARNING: this is untested experimental territory for learning about AI agent capabilities.** 
Providing a ready-to-paste `/goal` setup that turns one interactive Claude
Code session into a self-driving, multi-hour optimizer over the *real* E555
toolchain in this repo. Unlike v1 (a generic template with placeholders), this
version is wired to the actual tools, the actual scoring, and the actual current
frontier. Paste section 8, walk away, come back to a documented campaign.

Written to be driven by the economical **Sonnet** Claude LLM with PRO subscription (long run, 
tokens refill on a flat cadence). Sonnet is capable but literal, so this kit is
deliberately *prescriptive*: exact commands, explicit budgets, a fixed decision
procedure, and hard guardrails. When in doubt the agent follows the recipe rather
than improvising.

---

## 0. The one-paragraph mission

The puzzle is the 16×16, 256-piece Eternity-II-style board defined by
`data/seed_Edge5.txt`. A board scores **matched internal edges out of 480**;
`breaks = 480 − score`. **480 = solved.** This puzzle class is famously unsolved
in public — nobody should *expect* 480. The honest, measurable mission is:

> **Push the frontier past the current best of 463/480 (17 breaks), and drive the
> break count as low as the tools can reach — documenting every step so the run
> can be evaluated and resumed later.** 480 is the moonshot; every break removed
> below 17 is a genuine, publishable result.

Reaching 464 is a win. Reaching 470 is a big win. The point is real progress
under a trustworthy scorer, not a number that merely *looks* good.

---

## 1. Ground rules (containment + anti-cheating contract)

These are non-negotiable and belong verbatim inside the `/goal` block.

**Containment**
- Do **all** work inside `agent/lab/` (created in setup). Treat everything above
  `agent/` as **read-only reference**. Never modify the parent repo's `src/`,
  `bin/`, `data/`, `tools/`, `examples/`, or docs.
- Never write to any network path. The beamer's on-disk DB
  cache, if used, goes to `agent/lab/cache/` on **local** disk only.
- You **may** copy source into the lab, edit those copies, and write brand-new
  tools in `agent/lab/tools/`. You may rebuild the C binaries in the lab. Improve
  anything you like — parsers, samplers, allocators, heuristics.

**Anti-cheating contract** (the run is worthless if any of these is broken)
1. The puzzle is `agent/lab/data/seed_Edge5.txt`, perfect score **480**. Never
   swap in `synth_seed.txt` / `synth_solution_480.csv` as if they were the goal —
   the synthetic set is a *self-test fixture* (solvable by construction). Solving
   it is **not** progress and must never touch the scoreboard.
2. **The scorer is the referee.** The official score of any board is what
   `E555_rank.py` computes *from the untouched seed*. You
   may read the scorer and you may write independent cross-check scorers, but you
   may **never** edit the scorer, the seed, or the piece-validity check to inflate
   a number. If you write a new scorer, it must agree with the canonical one on
   known boards before you trust it.
3. Every board on the frontier must be **traceable to a logged command** that
   produced it (a `run_log.csv` row). No hand-authored or hard-coded boards.
4. A board only counts if it is a valid permutation of all 256 seed pieces with
   legal rotations — the scorer already enforces this; do not disable it.
5. **Report reality.** A run that did not improve is data — log it truthfully with
   its real score. Never round up, never claim an improvement you cannot
   reproduce by re-scoring the file from a clean read.
6. Before accepting any new best, **re-score the emitted file independently**
   (fresh `E555_rank.py` call). Accept only if it re-scores ≥ the old best.

---

## 2. The terrain — what each tool does and how to call it

Read this once; it is the map. Every command below assumes CWD = `agent/lab/`.
Canonical, battle-tested invocations live in `examples/` — **read the header of an
example script before first use**; they document every knob.

**Scoring convention.** Canonical board CSV row is
`config_id, score, pos[256], rot[256]` (514 fields). `pos[p]=row*16+col`, row 0 =
bottom; `999` = unplaced. Field 2 is *matched edges* — except Stage B writes a
solution index there, which is why you **always recompute** with the scorer.

**The pipeline (each stage feeds the next):**

| stage | tool | one-line job | canonical example |
|---|---|---|---|
| A | `src/A_border/E555_edge_annealer.py` | anneal border piece rotations (the frame) | `ANNEAL=1 examples/01_beamer_quickstart.sh` |
| B | `bin/E555_beamer` | row-by-row beam search up to a stop row → deep partials | `ANNEAL=1 examples/01_beamer_quickstart.sh` |
| B′ | `bin/E555_finalizer` | re-grow the top rows of a partial with a fresh beam | `examples/02_finalizer_regrow.sh` |
| C1 | `src/C_tail/E555_topper.py` | CP-SAT: **herd** breaks toward the nearest corner/side | `pipeline/topper_sweep.sh` (`PRESET=window`, `safe`, `deep`, `close`) |
| C2 | `src/C_tail/E555_ender.py` | CP-SAT closer: `--mode ring` (re-thread whole border) or default patch (localized LNS) | `examples/04_stage_c_close.sh` |
| C3 | `bin/E555_backtracker` | exact / bounded-mismatch DFS dives to **close** a small tail | `examples/05_backtracker_dives.sh` |
| - | `pipeline/run_pipeline_annealed.sh` | **all six stages end to end** — the reference orchestration | `pipeline/run_pipeline_annealed.sh` |

**The measure (single source of truth), used after every run:**
```bash
python3 tools/E555_rank.py <FILE> --seed data/seed_Edge5.txt --top 5
```
Reads: `score` (=/480), `breaks`, and the *shape* — `break_rows`, `break_cols`,
`span`, `clean_b/clean_t/clean_l/clean_r` (clean rows from each edge),
`corner_d`. The top row is the best board. To pull the numeric best:
```bash
python3 tools/E555_rank.py <FILE> --seed data/seed_Edge5.txt \
    --emit /tmp/n.csv --rescore >/dev/null
head -1 /tmp/n.csv | cut -d, -f2                     # -> best score /480
```

**Visual check (occasional, for the journal):**
```bash
python3 tools/E555_viewer.py <FILE> --seed data/seed_Edge5.txt --no-url --row 0
```

**Key tool knobs worth sweeping** (see each `--help`):
- annealer: `--w-top/-right/-bottom/-left`, `--target_scale`, `--restarts`,
  `--fix-corners {0,1,2}`, `--steps`, `--seed`, `--threads` (restarts run in
  parallel; never changes the result), `--verbose` (per-step progress and the
  `BEST,` lines, off by default).
- beamer: `--beam_width`, `--stop_row` (1..13), `--lambda_Mahalanobis` (the 463
  boards came from a Mahalanobis campaign — this knob matters), `--incomplete_top`,
  `--random_edges` (border-free), `--db_file agent/lab/cache/E555.db`, `--threads`.
- finalizer: `--finalize_from` (keep it **~3–4 rows BELOW the beamer stop row** —
  locking right under the frontier just re-runs the row that already failed),
  `--finalize_repeats`, `--beam_width`.
- topper: `--side {T,B,L,R,TR,TL,TB}`, `--work-rows`, `--report_best` (real beam),
  `--max_time`, `--stall_time`, `--workers`.
- ender: `--mode {patch,ring}`, `--reach`, `--max-changes`, `--holes FILE`,
  `--max-time`, `--stall-time`, `--workers`. Never returns a board worse than input.
- backtracker: `--holes FILE`, `--order {mrv,spiralout,2sides,4sides,...}`,
  `--max-mismatch K` (0 = exact), `--stuck_restarts`, `--time-limit`, `--threads`.

---

## 3. The two hard-won heuristics (read the board before choosing a tool)

These come from real runs on this seed. Bake them in — they save hours.

**H1 — Attack breaks where they actually are.** `rank.py` tells you *where* the
breaks sit (`break_rows`, `clean_b/t/l/r`, `span`). Open only the region that
contains breaks. On the 463 boards **every break is in the top ~4 rows** (12 rows
clean from the bottom). Running topper/ender on the Bottom/Left/Right there is
wasted effort — worse, it can *spread* a break into a previously-clean row and
inflate the span to 16×14 while the count stays 17. **Match the opened side/holes
mask to the break cluster.**

**H2 — Live "breaks=" telemetry is window-local, not the whole board.** The topper's
`[inc] breaks=N` counts only junctions touching the open band; the *whole-board*
number is what `rank.py`/`[best]` report. Never treat a low live number as a
result — **always re-score the emitted file** (contract rule 6). This is exactly
the trap that makes a run *look* like it hit 16 while the board is still 17.

**H3 — Match the closer to the tail size** (from `breaks` on the best board):
- **breaks > 30** → `ender --mode ring` first (re-thread the whole border), then re-measure.
- **10 ≤ breaks ≤ 30** → `topper` to herd breaks to the corner(s)/side(s) where they
  live (H1), then `ender` patch around them.
- **breaks < 10, clustered** → `backtracker` with a `--holes` mask covering the
  cluster + a slack ring, `--max-mismatch` small, `mrv` order. This is the endgame
  closer — the only exact engine that can actually *finish* a board.
- **the board is a partial** (unplaced cells / rows not full) → grow it first with
  `beamer`/`finalizer`, or `backtracker` using the empty cells as holes.

---

## 4. One-time setup (do this before `/goal`)

Run these by hand (or let the agent do them as iteration 0). They build the
self-contained lab.

```bash
cd <repo root>                      # the folder that contains agent/ and src/
mkdir -p agent/lab/{tools,src,data,cache,runs,frontier}

# working copies the agent may edit
cp -r src bin tools agent/lab/ 2>/dev/null || true
cp data/seed_Edge5.txt data/synth_seed.txt data/synth_solution_480.csv \
   data/best_463.csv data/holes_open_border_*.csv agent/lab/data/

# rebuild the C binaries FROM SOURCE on THIS machine (checked-in bins are WSL builds)
make -C agent/lab 2>/dev/null || make        # produces agent/lab/bin/E555_*

# ortools for the CP-SAT tools (topper/ender)
pip install ortools

# seed the bookkeeping
cd agent/lab
cp data/best_463.csv frontier/frontier.csv          # start from the known best
cat > scoreboard.json <<'JSON'
{ "puzzle": "seed_Edge5", "perfect_score": 480,
  "baseline_score": 463, "best_score": 463, "best_breaks": 17,
  "best_source": "data/best_463.csv (provided baseline)",
  "best_file": "frontier/frontier.csv",
  "total_runs": 0, "goal_start_unix": null, "history": [] }
JSON
printf 'unix,iso,stage,tool,params,input,output,score,breaks,runtime_s,note\n' > run_log.csv
: > JOURNAL.md
```

**Verify the scorer agrees with the baseline before trusting anything:**
```bash
python3 tools/E555_rank.py frontier/frontier.csv --seed data/seed_Edge5.txt --top 1
# must show score 463 / breaks 17. If not, STOP and fix setup — do not start the goal.
```

### 4.1 Starter pool — pre-computed Stage B partials (strongly recommended)

If you already have Stage B beam outputs (deep partials), seed them so the agent
skips slow Stage A/B generation at first and starts straight on the tail. This is
the biggest single speedup available.

```bash
mkdir -p agent/lab/pool
cp <your_big_stage_B_file(s)>.csv agent/lab/pool/     # 100k+ rows is fine
```

**Vet + curate ONCE (not every iteration):**
```bash
cd agent/lab
# 1) same-seed sanity check on a sample: cleanest partials must have clean BOTTOM
#    rows (a wrong-seed pool shows breaks scattered through every row).
head -50 pool/*.csv > pool/_sample.csv
python3 tools/E555_rank.py pool/_sample.csv --seed data/seed_Edge5.txt --top 10

# 2) curate a shortlist: most-placed first, then fewest breaks. Partials of
#    equal depth => fewest breaks wins; do NOT sort by breaks alone (an emptier
#    board trivially has fewer breaks). --rescore also rewrites field 2 as the
#    true matched-edge count, so the emitted file is sortable by score too.
python3 tools/E555_rank.py pool/<big>.csv --seed data/seed_Edge5.txt \
    --sort placed,breaks --top 5000 --emit pool/curated.csv --rescore
: > pool/consumed.txt          # ids the agent has already pulled
```
(If a single file is too big to rank in one pass, split it, rank each chunk with
`--top`, `cat` the winners, and rank once more.) Aim for a curated pool that is
both **good and diverse** — a few thousand distinct partials beat 100k near-clones,
because Stage C produces near-identical results from near-identical inputs.

The agent treats `pool/curated.csv` as its quarry: in the "need raw material"
branch it pulls the next batch of un-consumed rows (appending their ids to
`pool/consumed.txt`), grows/repairs them, and only falls back to running the
annealer+beamer once the promising pool rows are used up.

**Pre-flight for the long run**
- Start a durable session: `tmux new -s e555`, then launch `claude` inside it.
  (Avoid `claude -p` for multi-hour work.)
- Pick permissions (section 5). Apply **before** `/goal`.
- Set `threads = nproc`. This box is not the 4-core laptop — but still read
  `nproc`, don't hard-code.
- Optional big speedup on this stronger box: pass
  `--db_file agent/lab/cache/E555.db` to the beamer/finalizer so the ~6 GB chain
  DB is built once and mmapped thereafter. **Local disk only, never the shared drives.**

---

## 5. Permissions — pick one (apply before `/goal`)

**A. Isolated box, fast path:** `claude --dangerously-skip-permissions`. Fine on a
dedicated machine; you lose the safety net.

**B. Allowlist (recommended)** — add to `.claude/settings.json`, scoped to the lab:
```json
{ "permissions": { "allow": [
  "Bash(python3 agent/lab/*:*)", "Bash(python3 agent/lab/**:*)",
  "Bash(agent/lab/bin/*:*)", "Bash(bash agent/lab/*:*)",
  "Bash(make*:*)", "Bash(timeout *:*)", "Bash(nproc)", "Bash(date +%s)",
  "Bash(sort *:*)", "Bash(cut *:*)", "Bash(head *:*)", "Bash(grep *:*)",
  "Read(agent/**)", "Read(src/**)", "Read(examples/**)", "Read(data/**)",
  "Write(agent/lab/**)", "Edit(agent/lab/**)"
] } }
```

**C. Auto mode:** try `/permissions` in-session; availability varies by build.

---

## 6. Bookkeeping the agent must maintain (this is how you evaluate progress)

Everything under `agent/lab/`:
- **`scoreboard.json`** — current best + full `history[]` (one entry per accepted
  improvement: unix, score, breaks, tool, params, file).
- **`run_log.csv`** — **every** run, improvement or not:
  `unix,iso,stage,tool,params,input,output,score,breaks,runtime_s,note`.
- **`frontier/frontier.csv`** — the single current best board, always re-scorable.
  Keep a timestamped copy in `frontier/` whenever it advances.
- **`JOURNAL.md`** — append-only human narrative, one short block per experiment
  batch: **hypothesis → exact command → result (score/breaks/shape) → decision**.
  This is the "why" that makes the run reviewable.
- **`STRATEGY.md`** — a living list of parameter families **tried and exhausted**,
  so the agent (and you) never re-plough the same ground.
- **`runs/<timestamp>_<tool>/`** — raw outputs; keep stdout terse and route long
  solver logs to a file you `grep`, never `cat` (token discipline for Sonnet).

Logging style (repo convention): every experiment first echoes a `[cfg]` line
naming its input/output files and key params; counters/results are reported both
for the run **and** cumulatively against the frontier.

**Progress at a glance** (put this in each status reply and in `SUMMARY.md`):
```bash
awk -F, 'NR>1{print $2, $8}' run_log.csv     # iso, score over time -> the curve
```

---

## 7. The iteration loop (what the agent does each turn)

A fixed, prescriptive cycle. Prefer warm-starting from `frontier/frontier.csv`.

1. **Orient.** Read `scoreboard.json`, the last few `JOURNAL.md` blocks, and
   `STRATEGY.md`. Score the current frontier and note its **shape** (breaks,
   where they are) via `rank.py`.
2. **Choose the move** by the decision procedure:
   - If you need fresh raw material (frontier stalled, or few candidates in play):
     **first pull the next un-consumed batch from `pool/curated.csv`** (§4.1),
     append their ids to `pool/consumed.txt`, and grow/repair them. Only when the
     promising pool rows are exhausted do you **generate** new deep partials the
     slow way (Stage A annealer with a new weight/target family → Stage B beamer,
     varying `--beam_width`, `--lambda_Mahalanobis`, `--stop_row`; then finalizer).
     Diversity of partials is the fuel — prefer the pool, it is free fuel.
   - Else **repair the current best** using **H1 + H3**: pick the tool that matches
     the tail size, and open only the region where the breaks are.
   - Rotate families so at least a few genuinely different strategies are in flight
     before ever concluding a plateau (see stop rule c).
3. **Run it, bounded:** `timeout <MAX_SINGLE_RUN_SECONDS> <command>`, output into
   a fresh `runs/<ts>_<tool>/`. Default budget **1200 s**; raise only with a
   one-line justification in the journal.
4. **Score the output independently** with `rank.py` (contract rule 6). Append a
   `run_log.csv` row **whether or not it improved**.
5. **If it beats the frontier:** update `scoreboard.json`, copy the winning file to
   `frontier/frontier.csv` (+ a timestamped copy), and start the next iteration
   warm from it. **Chain the closers** on a new best (topper → ender ring → ender
   patch → backtracker) before declaring the new plateau.
6. **Journal it:** hypothesis, command, result, decision. Update `STRATEGY.md` if a
   family is now exhausted.
7. **Every turn, report:** `best_score/480` (and breaks), `total_runs`, elapsed
   wall-clock since `goal_start_unix`, and the last decision + next intended move.

**Ideas the agent is encouraged to build (all legitimate, all in the lab):**
a break-cluster analyzer that auto-generates a `--holes` mask around the actual
breaks; a diversity sampler that feeds many distinct partials into Stage C; a
small portfolio allocator that gives more budget to whichever family is gaining.
Keep any parallelism **simple** (the tools' own `--threads`, GNU `parallel`, or
plain background jobs) — do **not** hand-roll work-stealing, and never trade away
the backtracker's exhaustive correctness for speed.

---

## 8. The `/goal` block (paste as one block, no edits needed)

```
/goal
You are running an autonomous, multi-hour optimization campaign on the E555
Eternity-II-style puzzle. The puzzle is agent/lab/data/seed_Edge5.txt; a board
scores matched internal edges out of 480 (breaks = 480 - score). Perfect = 480.
The current known best is 463/480 (17 breaks), already saved as
agent/lab/frontier/frontier.csv. Your mission: push the frontier BELOW 17 breaks
and as close to 480 as the tools allow, documenting everything for later review.

WORKING DIRECTORY: do everything inside agent/lab/. Everything above agent/ is
READ-ONLY reference. Read agent/autonomous_agent_mode.md sections 2, 3, 6, 7
now — it is your map (tools, exact invocations, heuristics, bookkeeping). Read the
header of an examples/*.sh script before first using the tool it drives.

HARD RULES (a run that violates any of these is void):
- The scorer is the referee: a board's official score is what
  tools/E555_rank.py computes from the UNTOUCHED seed_Edge5.txt. Never edit the scorer, the seed, or the piece-validity check to
  inflate a score. You may write independent cross-check scorers.
- Never substitute synth_seed.txt / synth_solution_480.csv for the real puzzle;
  the synthetic set is only a self-test fixture and never touches the scoreboard.
- Every frontier board must trace to a logged command in run_log.csv. No
  hand-authored or hard-coded boards. Report real numbers, including runs that did
  not improve. Before accepting a new best, RE-SCORE the emitted file from a clean
  read and accept only if it re-scores >= the old best.
- Write only inside agent/lab/. Never touch any network path; the
  beamer DB cache, if used, goes to agent/lab/cache/ on local disk.
- You MAY copy source into the lab, edit those copies, build new tools, and
  rebuild the binaries. Keep parallelism simple; never weaken the backtracker's
  exhaustive correctness.

EACH ITERATION (loop, following v2.md section 7):
1. Read scoreboard.json + recent JOURNAL.md + STRATEGY.md. Score frontier.csv with
   rank.py and note WHERE the breaks are.
2. Choose the next experiment by the decision procedure (v2.md section 3):
   - stalled or low on fresh material -> FIRST pull the next un-consumed batch
     from pool/curated.csv (record ids in pool/consumed.txt) and grow/repair them;
     only when those are exhausted, generate new partials the slow way (annealer
     -> beamer -> finalizer, varying beam_width, lambda_Mahalanobis, stop_row).
   - otherwise repair the current best: match the tool to the tail size (breaks>30
     -> ender ring; 10-30 -> topper then ender patch; <10 clustered -> backtracker
     with a holes mask on the cluster) and OPEN ONLY THE REGION THAT HAS BREAKS.
   Prefer warm-starting from frontier.csv. Rotate through at least a few distinct
   strategy families before calling a plateau.
3. Run it as: timeout 1200 <command>, outputs into agent/lab/runs/<ts>_<tool>/.
   Exceed 1200s only after stating why in the journal.
4. Score the output with rank.py. Append one run_log.csv row regardless of outcome.
5. If it beats best_score: update scoreboard.json, copy the winner to
   frontier/frontier.csv (+ timestamped copy), then chain the closers on it
   (topper -> ender ring -> ender patch -> backtracker) before the next plateau.
6. Journal it (hypothesis, command, result, decision); update STRATEGY.md when a
   family is exhausted.
7. On your FIRST turn, if scoreboard.json.goal_start_unix is null, set it to
   `date +%s`. Every turn, state: best_score/480 and breaks, total_runs, elapsed
   wall-clock, last decision, next intended move.

STOP and write SUMMARY.md when ANY holds:
(a) best_score == 480 (solved).
(b) 8 hours elapsed since goal_start_unix.
(c) best_score has not improved for 60 minutes of wall-clock AND you have tried at
    least 3 genuinely distinct strategy families since the last improvement.

SUMMARY.md must contain: final best_score/480 and breaks; the winning tool + exact
params + file; the top 5 attempts from run_log.csv; the score-over-time curve
(iso,score); which strategy families are exhausted (from STRATEGY.md); and three
concrete, specific things to try next with more time or a bigger machine.
```

Tune before pasting only if you want to: the `1200`s per-run cap, the `8`h total
budget, the `60`min / `3`-family plateau rule. Everything else is repo-correct as
written.

---

## 9. While it runs / after it stops

- Peek: `/goal` shows elapsed, turns, tokens, last reason. `/goal clear` stops early.
- Tail the story without spending tokens: `tail -f agent/lab/JOURNAL.md`,
  `column -s, -t agent/lab/run_log.csv | tail`.
- The frontier board is always at `agent/lab/frontier/frontier.csv`; re-feed it to
  any Stage C tool by hand, or view it:
  `python3 agent/lab/tools/E555_viewer.py agent/lab/frontier/frontier.csv --seed agent/lab/data/seed_Edge5.txt`.
- To resume a later day: the scoreboard, frontier, run_log, and STRATEGY.md are all
  the state you need — start a fresh session, point `/goal` at the same lab, and it
  picks up from the recorded frontier.

## 10. Realistic expectations

463 → 462 or lower is the honest near-term target; every break removed is real
progress on an open problem. Do not be discouraged by long flat stretches — that
*is* the shape of edge-matching search. The run's deliverable is twofold: the best
board found, and a documented map of what worked and what didn't, so the next run
(or the next machine) starts smarter. That documentation is the win even if 480
stays out of reach.
