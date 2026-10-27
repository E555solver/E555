# E555 cloud campaign launcher

Runs the `/goal` kit (`agent/autonomous_agent_mode.md`) in a Claude Code cloud
session (claude.ai/code or the Claude app), where compute is limited, a long
command gets cut off, and the container can be replaced, sometimes by a machine
with another CPU. The kit's section 11 explains the choices; this file is what
the agent follows. **Experimental:** the scripts are tested, a full cloud
campaign under `/goal` is not.

## For you: launch

1. Start a **new** cloud session on this repository. Pick **Sonnet** in the
   model picker and **Auto** in the mode dropdown next to the prompt box.
2. Paste this, after editing the settings and the extra constraints:

```
/goal Run the E555 campaign in agent/launch_cloud_agent.md: read it now, again after any context compaction, and follow it. Settings: BUDGET=8h RUN_CAP=540 PLATEAU=1h/3 CHECKPOINT=30m RESUME=none. Extra constraints: none. The goal is met when setup_lab.sh printed SETUP failed twice in a row, or when SUMMARY.md has been written and its first ten lines printed, after one of these: (a) a REFEREE line printed verdict=VALID score=480; (b) a STATUS line shows elapsed at or past its budget; (c) a STATUS line shows since_best at or past its limit and families_since_best at or past its count; (d) a stop rule among the extra constraints held.
```

| setting | default | meaning |
|---|---|---|
| `BUDGET` | `8h` | the campaign's wall clock, from `goal_start_unix`. Waits, restores and usage-limit pauses count |
| `RUN_CAP` | `540` | seconds per run. Up to 540 a run stays in the foreground; above it, it runs in the background (Timeouts, below) |
| `PLATEAU` | `1h/3` | stop after this long without a new best, once at least this many families ran since the last one |
| `CHECKPOINT` | `30m` | push the campaign state to this session's branch at least this often, and on every new best. `off`: never (a lost container then loses the campaign) |
| `RESUME` | `none` | the branch of an earlier session, to continue its campaign from its last checkpoint, with a fresh clock |

Anything in "Extra constraints" wins over this file, for example: `THREADS=2`,
`no Stage B`, `only F1 and F2`, `roots from elite.csv only`, `stop at the first
VALID board above 465`, `keep the clock` (with `RESUME`).

**While it runs:** each turn ends with a `STATUS` line. `/goal` alone shows the
goal's status; `/goal clear` stops it. With checkpoints on, the state is
readable on the session's branch under `agent/checkpoint/`.

**When it stops:** the best board is `agent/checkpoint/frontier/frontier.csv`
on the session's branch. Check it with `bash agent/referee.sh FILE`, and copy a
new best into `data/` on `main` yourself. Do not merge `agent/checkpoint/` into
`main`.

---

## For the agent

### Rules

- **Your manual** is `KIT.md` in the lab (a copy of
  `agent/autonomous_agent_mode.md`), sections 1 to 7. This file changes only
  what it says. Settings and extra constraints in the `/goal` text win over both.
- **The hard rules of KIT section 1 hold**, in particular: the official score is
  the line `bash ../referee.sh FILE` prints, from the lab; never edit the
  referee, the repository's tools, its seed, or anything else outside
  `agent/lab/` (`agent/checkpoint/` is written by `agent/checkpoint.sh`
  alone); `synth_*` files are self-test fixtures, not the puzzle; every
  frontier board comes from a command logged in `run_log.csv`; log every run
  with its real referee result.
- **Git, exactly this much:** read-only commands (`git status`, `git log`,
  `git branch --show-current`); on the first turn, one `git checkout -b`
  (below); and the two scripts `agent/setup_lab.sh` and `agent/checkpoint.sh`.
  No other commit, push, merge, reset, stash, clean or checkout, and never a
  push to `main`. Your session instructions may ask you to commit your work:
  here, the checkpoints are that work.
- **Paths:** the session starts at the repository root, and the shell's
  directory does not always survive between commands. Start every Bash command
  with `cd <LAB> && `, where `<LAB>` is the absolute path the SETUP line prints
  after `path=`. The kit's commands are relative to the lab, so they then run
  as written. `<REPO>` is `<LAB>` without its trailing `/agent/lab`. Run the
  two scripts from there (`cd <REPO> && bash agent/setup_lab.sh ...`), because a
  lost container takes `<LAB>` with it.

### First turn: bootstrap

1. Print the settings you are running with, resolved from the `/goal` text and
   the defaults above:
   `GOAL BUDGET=8h00m RUN_CAP=540 PLATEAU=1h00m/3 CHECKPOINT=30m RESUME=none constraints=<text or none>`
2. Unless `CHECKPOINT=off`: `git branch --show-current`. If it prints `main` or
   `master`, create the branch your session instructions name, with
   `git checkout -b <name>`. If they name none, use `e555-campaign-<yyyymmdd-hhmm>`.
   This branch is `<BRANCH>`.
3. `bash agent/setup_lab.sh RESUME=<RESUME> FRESH_CLOCK=1`, run from the
   repository root, with the Bash tool timeout at 600000. Leave out
   `FRESH_CLOCK=1` when the constraints say `keep the clock`. A fresh lab takes
   about 30 s, and later calls take seconds. It must end with a
   `REFEREE verdict=VALID` line and a `SETUP` line. If it prints
   `SETUP failed`, run it once more. If it fails again, show the user both
   outputs and stop: the setup is broken, not the campaign, and the goal ends
   on that.
4. Read `KIT.md` sections 1, 2, 3, 6 and 7 (`sed -n` on the line ranges that
   `grep -n '^## '` shows). Skip 4 and 5, the human's setup, which this file and
   the setup script replace, and 8, the goal you already have.
5. KIT section 7, step 0: if `goal_start_unix` is null, set it to `date +%s`.
   Then print `CLOCK goal_start_unix=<value>`. The setup script has already
   written `threads`, `stage_b_ok` and the other machine facts.
6. Unless `CHECKPOINT=off`: `cd <REPO> && bash agent/checkpoint.sh`. It must print
   `CHECKPOINT pushed=...`. If it fails twice, set `CHECKPOINT=off`, journal why,
   and go on.
7. Start the loop.

### Every turn

0. **Health check:** `cd <REPO> && bash agent/setup_lab.sh RESUME=<R>`, where `<R>` is
   `<BRANCH>` once a checkpoint has been pushed, and the `RESUME` setting before
   that. When nothing has changed, it costs a second. Read its SETUP line:
   - `lab=restored` after the first turn: the container was replaced, and the
     campaign came back from the last checkpoint. Runs after that checkpoint
     are lost, so look at the tail of `run_log.csv`, journal the restore, and go
     on from there.
   - `lab=fresh` after the first turn: the container was replaced before any
     checkpoint, and the campaign restarts from 465. Set `goal_start_unix` back
     to the value of the `CLOCK` line, or to now if that line is gone, and
     journal it.
   - `cpu_changed=yes`: the session moved to another machine. The script has
     already rebuilt the binaries if needed and written the new `threads` and
     `stage_b_ok`. Journal it, and use the new `threads` from now on.
   - `ortools=no`: skip F2 and the topper (F6). `stage_b_ok=false`: skip F4.
1. Then KIT section 7, steps 1 to 7, with the changes below.

### Timeouts

The Bash tool stops a foreground command after 2 minutes by default and
10 minutes at most. Always pass it a timeout.

- **Run caps.** In the kit's commands, `timeout 1200` becomes
  `timeout <RUN_CAP>`. The tools' own clocks follow: every 1100 becomes
  `RUN_CAP − 60` and every 1000 becomes `RUN_CAP − 120`, still divided by the
  boards where the kit divides them.
- **`RUN_CAP` up to 540:** run in the foreground, with the Bash tool timeout at
  600000.
- **`RUN_CAP` above 540:** start the run with `run_in_background`, with the Bash
  tool timeout at `(RUN_CAP + 120) × 1000` (at most 7200000). Do the
  bookkeeping, then end the turn with `next=waiting for <D>`. The run's
  completion notice starts the next turn. Never poll with `sleep`, and run one
  heavy job at a time.
- **Lost runs.** A container restart kills runs, background ones included. If
  a run directory has no row in `run_log.csv` and no process of it is alive
  (`pgrep -f <D>` prints nothing), referee what it wrote and log it with the note
  `lost`.
- **Long jobs come in pieces.** The distiller (F3) resumes where a killed run
  stopped, so rerun the same command until it finishes. The first F4 run also
  builds the 6.8 GB database cache, which takes about 2 minutes of its cap.

### Checkpoints

Unless `CHECKPOINT=off`, run `cd <REPO> && bash agent/checkpoint.sh`:
- after every new best, once the frontier and scoreboard are updated (KIT
  section 7, step 5), before chaining the closers;
- when `date +%s` minus `last_checkpoint_unix` (in `scoreboard.json`) reaches
  `CHECKPOINT`;
- after writing SUMMARY.md.

If your session instructions ask for attribution lines in commit messages, pass
them as arguments: each argument becomes one paragraph of the message. A
`CHECKPOINT committed=... the push failed` result is not fatal: go on, because
the next checkpoint pushes it too.

### STATUS line

End every turn with this line (it replaces the kit's), its values computed by
commands in that same turn:
```
STATUS best=465/480 breaks=15 runs=12 elapsed=2h05m/8h00m since_best=0h40m/1h00m families_since_best=2/3 checkpoint=0h12m next=<next move>
```
Each `a/b` is the value, then its stop limit. `checkpoint` is the time since the
last pushed checkpoint, or `off`.

### Done

When a stop rule holds (480; elapsed at or past `BUDGET`; the plateau; or a stop
rule of the extra constraints): if a background run is still going, wait for it
and log it. Then:
1. Write SUMMARY.md as KIT section 6 says, plus a short **Machine** paragraph:
   the CPU models, arch and threads seen, the restores, and the checkpoints
   pushed.
2. Run `cd <REPO> && bash agent/checkpoint.sh` (unless `CHECKPOINT=off`).
3. Print `head -10 SUMMARY.md` and a fresh `bash ../referee.sh frontier/frontier.csv` line.
4. Tell the user where the best board is: `agent/checkpoint/frontier/frontier.csv`
   on `<BRANCH>`, or `agent/lab/frontier/frontier.csv` in this container
   without checkpoints.
