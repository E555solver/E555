# E555 whirlpool campaign launcher

Runs the whirlpool kit (`agent/whirlpool_agent_mode.md`) in a Claude Code
cloud session (claude.ai/code or the Claude app). The agent improves the
whirlpool runner until it finds **jewels**: boards with one fixed border that
reach row 10 or deeper from all four sides, each in under 3 minutes after the
beamer made it. It then keeps finding them, dives the jewels into complete
boards, and pushes the results to the session's branch.

Cloud sessions have their own constraints, and the campaign is built to
survive them:
- a few cores;
- commands that get cut off;
- a container that can be replaced, sometimes by a machine with another CPU;
- a usage limit that stops the turns until the 5-hour window resets.

This file is what the agent follows; the kit explains the goal.
**Experimental:** the scripts and the judge are tested; a full campaign under
`/goal` is not.

## For you: launch

1. Start a **new** cloud session on this repository. Pick the latest **Opus**
   in the model picker and **Auto** in the mode dropdown next to the prompt box.
2. Send `/effort medium`. If the picker has no Opus, send `/model opus` first.
3. Paste this line, after editing the settings and the extra constraints:

```
/goal Run the E555 whirlpool campaign in agent/launch_whirlpool_agent.md: read it now, again after any context compaction, and follow it. Settings: BUDGET=8h HARNESS_MINUTES=60 PLATEAU=2h CHECKPOINT=30m CAMPAIGN=new RESUME=none. Extra constraints: none. A usage limit only pauses the campaign: continue it where it stopped once the limit resets. The goal is met when setup.sh printed SETUP failed twice in a row, or when the campaign's README.md has been written and checkpointed and its first ten lines printed, after one of these: (a) a STATUS line showed stop=budget; (b) setup.sh printed stage_b_ok=false twice in a row; (c) a stop rule among the extra constraints held.
```

| setting | default | meaning |
|---|---|---|
| `BUDGET` | `8h` | the campaign's **run time**: harness runs plus logged experiments (`runs.csv`). Usage-limit waits, restores and the agent's own thinking do not count. The campaign never stops early: once it has a good runner, it keeps running it to find more jewels |
| `HARNESS_MINUTES` | `60` | the length of a scored harness run in the production phase (5..90). Screening runs are shorter |
| `PLATEAU` | `2h` | run time without a new best candidate, after which the agent spends most of its time producing jewels with the best one |
| `CHECKPOINT` | `30m` | push the campaign folder to this session's branch at least this often, after every harness run and on every record. `off`: never (a lost container then loses the campaign) |
| `CAMPAIGN` | `new` | a new campaign in its own folder, `agent/whirlpool/<id>/`, named by its UTC start time (`20261027-1930`). With `RESUME`, `new` picks the one campaign on that branch; name it (`CAMPAIGN=20261027-1930`) when the branch holds several |
| `RESUME` | `none` | the branch of an earlier session, to continue its campaign from its last checkpoint. The campaign keeps its id and its clock |

Anything in "Extra constraints" wins over this file. Examples:
- `THREADS=2`
- `no diving`
- `stop after the first jewel of depth 11`
- `start from candidates/v07_ring.sh` (with `RESUME`)

**One folder per campaign.** Each campaign writes only into its own
`agent/whirlpool/<id>/`, so the campaigns you run now and later never overlap.
A campaign never pushes more than 100 MB, and never pushes the 6.4 GB chain
database, which it rebuilds in about 2 minutes in a new container.

**While it runs:**
- Each turn ends with a `STATUS` line: the best runner and its jewels per hour,
  the jewels found, the best dived board, and the run time.
- `/goal` alone shows the goal's status; `/goal clear` stops it.
- With checkpoints on, everything is readable on the session's branch under
  `agent/whirlpool/<id>/`: `harness.csv` holds one row per scored run, and
  `JOURNAL.md` holds the agent's notes.

**Usage limits:** Opus spends a 5-hour window in a few hours of campaign, and
that is fine.
- On its first turn the agent creates an hourly scheduled task, "E555
  whirlpool <id> wake-up", that messages this session.
- The first wake-up after the window resets continues the campaign, from its
  checkpoint if the VM was reclaimed meanwhile.
- If the STATUS lines have not come back an hour after the reset, paste the
  line again with `RESUME=<this session's branch>`.
- The agent deletes the task when the campaign ends. You can also delete it
  from your list of scheduled tasks.

**When it stops**, the campaign folder on the session's branch holds:
- `README.md`: what changed and why, in plain language, with the measurements;
- `run_pipeline_whirlpool.sh`: the improved runner;
- `patches/`: any source changes it needs;
- `whirlpools/*.csv.gz`: every board that completed the whirlpool, as it was
  grown and before any dive, compressed (`zcat` reads them), with one line per
  chain in `whirlpool_index.csv` (`jewel=yes` marks the jewels);
- `best_boards.csv`: the best complete boards. Check them with
  `bash agent/referee.sh FILE`.

Copying the runner into `pipeline/`, or a record into `data/`, is your call.

---

## For the agent

### Rules

- **Your manual** is `agent/whirlpool_agent_mode.md`, all of it. This file
  changes only what it says. Settings and extra constraints in the `/goal`
  text win over both.
- **The kit's hard rules (section 4) hold.** In particular:
  - write only in `agent/whirlpool/<CAMPAIGN>/`;
  - code changes go in its `patches/`;
  - results come from `harness.sh` alone;
  - never commit the database.
- **Git, exactly this much:**
  - read-only commands (`git status`, `git log`, `git branch --show-current`);
  - on the first turn, one `git checkout -b` (below);
  - the scripts `agent/whirlpool/setup.sh` and `agent/whirlpool/checkpoint.sh`.

  No other commit, push, merge, reset, stash, clean or checkout, and never a
  push to `main`. Your session instructions may ask you to commit your work:
  here, the checkpoints are that work.
- **Names and paths.** The SETUP line gives:
  - the campaign after `campaign=` (`<CAMPAIGN>`, such as `20261027-1930`);
  - its lab's absolute path after `path=` (`<LAB>`, ending in
    `/agent/whirlpool/<CAMPAIGN>/lab`).

  From that, `<DIR>` is `<LAB>` without `/lab` (the campaign folder), and
  `<REPO>` is `<LAB>` without `/agent/whirlpool/<CAMPAIGN>/lab`.

  The shell's directory does not always survive between commands, so start
  every Bash command with `cd <LAB> && `. The kit's commands are relative to
  the lab: `bash ../../harness.sh`, `../../status.sh`, `../../timed.sh`,
  `../../checkpoint.sh`, `python3 ../../judge.py`, `bash ../../../referee.sh`.
  Run `setup.sh` from `<REPO>` instead
  (`cd <REPO> && bash agent/whirlpool/setup.sh ...`), because a lost container
  takes `<LAB>` with it.

### First turn: bootstrap

1. Print the settings you are running with, resolved from the `/goal` text and
   the defaults above:
   `GOAL BUDGET=8h00m HARNESS_MINUTES=60 PLATEAU=2h00m CHECKPOINT=30m CAMPAIGN=new RESUME=none constraints=<text or none>`
2. Unless `CHECKPOINT=off`, run `git branch --show-current`. If it prints
   `main` or `master`, create the branch your session instructions name, with
   `git checkout -b <name>`. If they name none, use
   `e555-whirlpool-<yyyymmdd-hhmm>`. This branch is `<BRANCH>`.
3. Run `bash agent/whirlpool/setup.sh CAMPAIGN=<CAMPAIGN setting> RESUME=<RESUME>`
   from the repository root, with the Bash tool timeout at 600000.
   - A fresh campaign takes about a minute (most of it the build); later calls
     take seconds.
   - It must end with a `SETUP` line. If it prints `SETUP failed`, run it once
     more. If it fails again, show the user both outputs and stop: the setup is
     broken, not the campaign, and the goal ends on that.
   - Otherwise print `CAMPAIGN <CAMPAIGN> lab=<LAB> branch=<BRANCH>`, so that
     the names stay in the transcript.
4. Read `agent/whirlpool_agent_mode.md` in full. Then read what it points at:
   - `pipeline/run_pipeline_whirlpool.sh`;
   - `agent/whirlpool/example_candidate.sh`;
   - pipeline/README.md, "The whirlpool";
   - PROJECT_E555.md §6 and §8.
5. Run `cd <LAB> && bash ../../status.sh BUDGET=<BUDGET> PLATEAU=<PLATEAU> CHECKPOINT=<CHECKPOINT>`.
   It stores the settings in `scoreboard.json` and starts the run-time clock.
6. Unless `CHECKPOINT=off`, run `cd <LAB> && bash ../../checkpoint.sh`. It must
   print `CHECKPOINT pushed=...`. If it fails twice, set `CHECKPOINT=off`,
   journal why, and go on.
7. Create **the usage-limit wake-up** (Usage limits, below).
8. Start the kit's Method (section 7), step 1.

### Every turn

0. **Health check:** `cd <REPO> && bash agent/whirlpool/setup.sh CAMPAIGN=<CAMPAIGN> RESUME=<R>`.
   `<R>` is `<BRANCH>` once a checkpoint has been pushed, and the `RESUME`
   setting before that. Run it again after every change to `patches/`. Read
   its SETUP line:
   - **`folder=restored`**: the container was replaced, and the campaign came
     back from its last checkpoint. Attempts after that checkpoint are lost:
     look at the tail of `attempts.csv` and `harness.csv`, journal the restore,
     and go on. The next produce call rebuilds `E555.db`, in about 2 minutes.
   - **`lab=fresh` after the first turn**: the lab was rebuilt, with
     `patches/` laid over it. The same note about `E555.db` applies.
   - **`cpu_changed=yes`**: the session moved to another machine, and the
     binaries were rebuilt if needed. Journal it. Jewels per hour from
     different CPUs do not compare. Before claiming that one candidate beats
     another, run the best again on the new machine.
   - **`stage_b_ok=false`**: the beamer cannot run here, because it needs 8 GB
     of free RAM. Journal it and check again next turn. On the second time in
     a row, finish (Done, below): write the README with what you have.
1. Then the kit's Method, with the changes below.

### Timeouts

The Bash tool stops a foreground command after 2 minutes by default and 10
minutes at most. Always pass it a timeout.

- **Harness runs go to the background.** Start
  `cd <LAB> && bash ../../harness.sh CANDIDATE=... MINUTES=<M> ...` with
  `run_in_background`, and a Bash tool timeout of `(M + 20) × 60000` (at most
  7200000). Do the bookkeeping, then end the turn with
  `next=waiting for harness <candidate>`. The run's completion notice starts
  the next turn. Never poll with `sleep`.
- **One heavy job at a time.** A harness run uses every core and its timing is
  the measurement, so start nothing else heavy while it runs: no diving, no
  builds, no tool experiments. Reading files, writing candidates and
  journalling are fine.
- **Other experiments:** run them through `bash ../../timed.sh "label" -- ...`,
  so that they count.
  - Up to 9 minutes: in the foreground, wrapped in `timeout 540`, with the Bash
    tool timeout at 600000.
  - Longer: in the background, as for a harness run.
- **Lost runs.** A container restart kills runs, background ones included. A
  harness run that died leaves its attempts in `attempts.csv` and its
  completed whirlpools in `whirlpools/`, but no `harness.csv` row; its minutes
  are not counted. Journal it and run again.

### Checkpoints

Unless `CHECKPOINT=off`, run `cd <LAB> && bash ../../checkpoint.sh`:
- after every harness run, so that its whirlpools are pushed;
- after a new best candidate, a new patch, or a record;
- when the STATUS line shows `checkpoint=...(due)`;
- after writing README.md.

If your session instructions ask for attribution lines in commit messages,
pass them as arguments: each argument becomes one paragraph of the message.
- `CHECKPOINT committed=... the push failed` is not fatal: the next checkpoint
  pushes it too.
- `CHECKPOINT refused` names what is wrong: a change outside the folder, a
  file over 50 MB, or more than 100 MB of history. Fix that before anything
  else: restore the outside file, or shrink or delete the large one. Never
  commit around it.
- Never delete or shrink anything in `whirlpools/`, not even to get a
  checkpoint through. If the archive itself is what is too large, set
  `CHECKPOINT=off`, journal it, and tell the user at once: the jewels are then
  only in this container.

### Usage limits

Opus spends a 5-hour usage window in a few hours of campaign. When the limit
hits, the turn stops, and nothing runs until the window resets. The campaign
survives that:
- the clocks count run time (`status.sh`), so the wait costs no budget;
- the checkpoints keep the folder if the idle VM is reclaimed meanwhile;
- the wake-up task restarts the turns after the reset.

**The wake-up task** (first turn, step 7).
- Skip it if `scoreboard.json` already has a `wakeup_trigger_id` whose
  `wakeup_branch` is `<BRANCH>`.
- If it has one for another branch, from a `RESUME`d campaign, delete that
  task first with `delete_trigger`: it would wake the old session.
- Then call `create_trigger` (the claude-code-remote scheduled-task tool; load
  it with ToolSearch if it is deferred). It fires into this session, so set
  neither `persistent_session_id` nor `create_new_session_on_fire`. Use:
  - `name`: `E555 whirlpool <CAMPAIGN> wake-up`
  - `cron_expression`: `0 * * * *` (hourly)
  - `initiation`: `human_request`
  - `prompt`, with `<CAMPAIGN>` written out:
    `E555 whirlpool <CAMPAIGN> wake-up. Run the health check of agent/launch_whirlpool_agent.md for campaign <CAMPAIGN> (it restores a lost folder). If agent/whirlpool/<CAMPAIGN>/README.md exists, the campaign is over: delete this scheduled task (wakeup_trigger_id in agent/whirlpool/<CAMPAIGN>/scoreboard.json) and stop. Otherwise continue the campaign where it stopped, following the launcher; if a harness run is still in flight, let it finish.`

Store the returned id as `wakeup_trigger_id`, and `<BRANCH>` as
`wakeup_branch`, in `scoreboard.json`, and journal it. Without `create_trigger`
among your tools, journal that the campaign has no wake-up and go on. A wake-up
that arrives mid-campaign needs nothing but the next loop iteration.

### STATUS line

End every turn with the line that `cd <LAB> && bash ../../status.sh` prints in
that turn, followed by ` next=<next move>`:
```
STATUS best=v03_ring@1a2b3c4d+none jewels/h=2.4(6/2.5h) jewels=9 depth_max=11 best_dived=455/480 run_time=3h10m/8h00m since_best=0h40m/2h00m checkpoint=0h12m stop=none next=<next move>
```
- **best**: the harness's best candidate, `name@script hash+patches hash`,
  with its jewels per hour over its standard runs.
- **jewels**: the campaign's distinct jewel frames. **depth_max**: the deepest
  jewel.
- **best_dived**: `agent/referee.sh`'s score of `best_boards.csv`.
- **run_time** and **since_best**: run time, each against its limit.
- **checkpoint**: the wall time since the last pushed checkpoint, marked
  `(due)` when the next one is due.
- **stop**: `budget` or `none`. The campaign has no other stop rule.

### Done

When `stop=budget` (or the second `stage_b_ok=false`, or a stop rule of the
extra constraints), start no new harness run. Wait for the one in flight to
finish, then:
1. **Write `<DIR>/run_pipeline_whirlpool.sh`** from the best candidate (kit
   section 9), and smoke-test it with `MODE=all WALL=600`. This is the one
   run allowed past the budget.
2. **Write `<DIR>/README.md`** (kit section 9), every number from
   `harness.csv`, plus a **Machine** paragraph: the CPU models, arch and
   threads seen, the restores, the usage-limit pauses, and the checkpoints
   pushed.
3. Run `cd <LAB> && bash ../../checkpoint.sh`, unless `CHECKPOINT=off`.
4. Delete the wake-up task: `delete_trigger` with `wakeup_trigger_id`.
5. Print `head -10 <DIR>/README.md` and a fresh `bash ../../status.sh` line.
6. Tell the user:
   - the campaign's id, and where its results are: `agent/whirlpool/<CAMPAIGN>/`
     on `<BRANCH>`, or only in this container without checkpoints;
   - the best runner's jewels per hour;
   - the number of jewels;
   - the best dived score, and whether it is a record.
