# E555 cloud campaign launcher

Runs the `/goal` kit (`agent/autonomous_agent_mode.md`) in a Claude Code cloud
session (claude.ai/code or the Claude app), where compute is limited, a long
command gets cut off, and the container can be replaced, sometimes by a machine
with another CPU, and where a usage limit stops the turns until the 5-hour
window resets. The kit's section 11 explains the choices; this file is what the
agent follows. **Experimental:** the scripts are tested, a full cloud campaign
under `/goal` is not.

## For you: launch

1. Start a **new** cloud session on this repository. Pick the latest **Opus**
   in the model picker and **Auto** in the mode dropdown next to the prompt box.
2. Send `/effort medium`. Medium is already Opus 5.5's default; the command
   makes it explicit. If the picker has no Opus, send `/model opus` first.
3. Paste this, after editing the settings and the extra constraints:

```
/goal Run the E555 campaign in agent/launch_cloud_agent.md: read it now, again after any context compaction, and follow it. Settings: BUDGET=8h RUN_CAP=540 PLATEAU=1h/3 CHECKPOINT=30m RESUME=none. Extra constraints: none. A usage limit only pauses the campaign: continue it where it stopped once the limit resets. The goal is met when setup_lab.sh printed SETUP failed twice in a row, or when SUMMARY.md has been written and its first ten lines printed, after one of these: (a) a REFEREE line printed verdict=VALID score=480; (b) a STATUS line showed stop=budget or stop=plateau; (c) a stop rule among the extra constraints held.
```

| setting | default | meaning |
|---|---|---|
| `BUDGET` | `8h` | the campaign's **run time**: the `runtime_s` of the logged runs, since `goal_start_unix`. Usage-limit waits, restores and the agent's own thinking do not count |
| `RUN_CAP` | `540` | seconds per run. Up to 540 a run stays in the foreground; above it, it runs in the background (Timeouts, below) |
| `PLATEAU` | `1h/3` | stop after this much run time without a new best, once at least this many families ran since the last one |
| `CHECKPOINT` | `30m` | push the campaign state to this session's branch at least this often, and on every new best. `off`: never (a lost container then loses the campaign) |
| `RESUME` | `none` | the branch of an earlier session, to continue its campaign from its last checkpoint, with a fresh budget |

Anything in "Extra constraints" wins over this file, for example: `THREADS=2`,
`no Stage B`, `only F1 and F2`, `roots from elite.csv only`, `stop at the first
VALID board above 465`, `keep the clock` (with `RESUME`).

**While it runs:** each turn ends with a `STATUS` line. `/goal` alone shows the
goal's status; `/goal clear` stops it. With checkpoints on, the state is
readable on the session's branch under `agent/checkpoint/`.

**Usage limits:** Opus spends a 5-hour window in a few hours of campaign, and
that is fine. On its first turn the agent creates an hourly scheduled task, "E555
campaign wake-up", that messages this session. The first wake-up after the
window resets continues the campaign, from its checkpoint if the VM was
reclaimed meanwhile. The budget counts run time, so the wait costs nothing. If
the STATUS lines have not come back an hour after the reset, paste the line
again with `RESUME=<this session's branch>` and `keep the clock`. The agent
deletes the task when the campaign ends; you can also delete it from your list
of scheduled tasks.

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
5. KIT section 7, step 0: `cd <LAB> && bash ../status.sh BUDGET=<BUDGET>
   PLATEAU=<PLATEAU> CHECKPOINT=<CHECKPOINT>`. It stores the settings in
   `scoreboard.json` and starts the run-time clock when `goal_start_unix` is
   null. The setup script has already written `threads`, `stage_b_ok` and the
   other machine facts.
6. Unless `CHECKPOINT=off`: `cd <REPO> && bash agent/checkpoint.sh`. It must print
   `CHECKPOINT pushed=...`. If it fails twice, set `CHECKPOINT=off`, journal why,
   and go on.
7. **The usage-limit wake-up** (Usage limits, below).
8. Start the loop.

### Every turn

0. **Health check:** `cd <REPO> && bash agent/setup_lab.sh RESUME=<R>`, where `<R>` is
   `<BRANCH>` once a checkpoint has been pushed, and the `RESUME` setting before
   that. When nothing has changed, it costs a second. Read its SETUP line:
   - `lab=restored` after the first turn: the container was replaced, and the
     campaign came back from the last checkpoint. Runs after that checkpoint
     are lost, so look at the tail of `run_log.csv`, journal the restore, and go
     on from there.
   - `lab=fresh` after the first turn: the container was replaced before any
     checkpoint, and the campaign restarts from 465. Run first-turn steps 5 and
     7 again, and journal it.
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
- when the STATUS line shows `checkpoint=...(due)`;
- after writing SUMMARY.md.

If your session instructions ask for attribution lines in commit messages, pass
them as arguments: each argument becomes one paragraph of the message. A
`CHECKPOINT committed=... the push failed` result is not fatal: go on, because
the next checkpoint pushes it too.

### Usage limits

Opus spends a 5-hour usage window in a few hours of campaign. When the limit
hits, the turn stops, and nothing runs until the window resets. The campaign
is built to survive that:
- the clocks count run time (`status.sh`), so the wait costs no budget;
- the checkpoints keep the state if the idle VM is reclaimed meanwhile;
- the wake-up task restarts the turns after the reset.

**The wake-up task** (first turn, step 7). Skip it if `scoreboard.json` already
has a `wakeup_trigger_id` whose `wakeup_branch` is `<BRANCH>`. If it has one
for another branch, from a `RESUME`d campaign, delete that task first with
`delete_trigger`: it would wake the old session. Then call `create_trigger`
(the claude-code-remote scheduled-task tool; load it with ToolSearch if it is
deferred). It fires into this session, so set neither `persistent_session_id`
nor `create_new_session_on_fire`, and use:
- `name`: `E555 campaign wake-up`
- `cron_expression`: `0 * * * *` (hourly)
- `initiation`: `human_request`
- `prompt`: `E555 campaign wake-up. Run the health check of agent/launch_cloud_agent.md (it restores a lost lab). If agent/lab/SUMMARY.md exists, the campaign is over: delete this scheduled task (wakeup_trigger_id in agent/lab/scoreboard.json) and stop. Otherwise continue the campaign where it stopped, following the launcher; if a run is still in flight, let it finish.`

Store the returned id as `wakeup_trigger_id` and `<BRANCH>` as `wakeup_branch`
in `scoreboard.json`, and journal it. Without `create_trigger` among your tools,
journal that the campaign has no wake-up and go on. A wake-up that arrives in
the middle of the campaign needs nothing but the next loop iteration.

### STATUS line

End every turn with the line `cd <LAB> && bash ../status.sh` prints in that
turn, followed by ` next=<next move>` (it replaces the kit's):
```
STATUS best=465/480 breaks=15 runs=12 run_time=2h05m/8h00m since_best=0h40m/1h00m families_since_best=2/3 checkpoint=0h12m stop=none next=<next move>
```
Each `a/b` is the value, then its stop limit, both in run time. `checkpoint` is
the wall time since the last pushed checkpoint, marked `(due)` when the next one
is due. `stop` names the stop rule that holds: `480`, `budget`, `plateau` or
`none`.

### Done

When a stop rule holds (`stop=` other than `none`, or a stop rule of the extra
constraints): if a background run is still going, wait for it and log it. Then:
1. Write SUMMARY.md as KIT section 6 says, plus a short **Machine** paragraph:
   the CPU models, arch and threads seen, the restores, the usage-limit pauses,
   and the checkpoints pushed.
2. Run `cd <REPO> && bash agent/checkpoint.sh` (unless `CHECKPOINT=off`).
3. Delete the wake-up task: `delete_trigger` with `wakeup_trigger_id`.
4. Print `head -10 SUMMARY.md` and a fresh `bash ../referee.sh frontier/frontier.csv` line.
5. Tell the user where the best board is: `agent/checkpoint/frontier/frontier.csv`
   on `<BRANCH>`, or `agent/lab/frontier/frontier.csv` in this container
   without checkpoints.
