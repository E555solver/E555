#!/bin/bash
# status.sh -- the STATUS line of the /goal kit (agent/autonomous_agent_mode.md,
# sections 6 and 11), computed from agent/lab's scoreboard.json and run_log.csv.
# The campaign's clocks count run time, the runtime_s column of run_log.csv,
# not wall time: a usage-limit wait, a replaced container or the agent's own
# thinking costs nothing, so a campaign that waits out a 5-hour usage window
# resumes without tripping its budget or its plateau.
#
#   bash agent/status.sh BUDGET=8h PLATEAU=1h/3 CHECKPOINT=30m   # the first call stores the settings
#   bash agent/status.sh                                         # later calls reuse them
#   STATUS best=465/480 breaks=15 runs=12 run_time=2h05m/8h00m since_best=0h40m/1h00m families_since_best=2/3 checkpoint=0h12m stop=none
#
# run_time sums runtime_s over the runs logged since goal_start_unix (the first
# call sets it when it is null), and since_best over the runs logged after
# last_best_unix. stop= names the first stop rule that holds: 480, budget, or
# plateau (since_best and families_since_best both at their limits); else none.
# checkpoint= is the wall time since the last pushed checkpoint, marked (due)
# once it reaches CHECKPOINT; it is left out while CHECKPOINT is off.
# Durations are like 8h, 90m or 1h30m. Exit status 0; 1 or 2 on a bad setting.
# ---- settings: pass NAME=value on the command line ---------------------------
BUDGET=             # run time the campaign may spend (default 8h)
PLATEAU=            # run time without a new best / families tried since (default 1h/3)
CHECKPOINT=         # wall time between checkpoints, or off (default off)
# -----------------------------------------------------------------------------
SETTINGS=" BUDGET PLATEAU CHECKPOINT "
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) [[ "$SETTINGS" == *" ${arg%%=*} "* ]] ||
                          { echo "unknown setting ${arg%%=*}; the settings are:$SETTINGS" >&2; exit 2; }
                      declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 2 ;;
    esac
done
set -euo pipefail

LAB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lab"
[ -f "$LAB/scoreboard.json" ] || { echo "status: no campaign in $LAB" >&2; exit 2; }
cd "$LAB"
python3 - "$BUDGET" "$PLATEAU" "$CHECKPOINT" <<'EOF'
import csv, json, re, sys, time

def seconds(text, name):
    m = re.fullmatch(r"(?:(\d+)h)?(?:(\d+)m)?", text.strip())
    if not text.strip() or not m:
        sys.exit(f"status: {name}={text} is not a duration like 8h, 90m or 1h30m")
    return int(m.group(1) or 0) * 3600 + int(m.group(2) or 0) * 60

def hm(s):
    s = max(0, int(s))
    return f"{s // 3600}h{s % 3600 // 60:02d}m"

now = int(time.time())
board = json.load(open("scoreboard.json"))
settings = board.setdefault("settings", {"BUDGET": "8h", "PLATEAU": "1h/3", "CHECKPOINT": "off"})
for name, value in zip(("BUDGET", "PLATEAU", "CHECKPOINT"), sys.argv[1:]):
    if value:
        settings[name] = value
budget = seconds(settings["BUDGET"], "BUDGET")
plateau_time, _, plateau_families = settings["PLATEAU"].partition("/")
plateau_time = seconds(plateau_time, "PLATEAU")
if not plateau_families.isdigit():
    sys.exit(f"status: PLATEAU={settings['PLATEAU']} is not like 1h/3")
plateau_families = int(plateau_families)
if board.get("goal_start_unix") is None:
    board["goal_start_unix"] = now
json.dump(board, open("scoreboard.json", "w"), indent=1)

start = board["goal_start_unix"]
last_best = board.get("last_best_unix") or start
run_time = since_best = 0
with open("run_log.csv", newline="") as f:
    for row in csv.DictReader(f):
        try:
            logged, runtime = int(row["unix"]), float(row["runtime_s"])
        except (KeyError, TypeError, ValueError):
            continue
        if logged >= start:
            run_time += runtime
            if logged > last_best:
                since_best += runtime

best = board["best_score"]
families = len(board.get("families_since_best", []))
if best >= 480:
    stop = "480"
elif run_time >= budget:
    stop = "budget"
elif since_best >= plateau_time and families >= plateau_families:
    stop = "plateau"
else:
    stop = "none"
fields = [f"best={best}/480", f"breaks={480 - best}", f"runs={board.get('total_runs', 0)}",
          f"run_time={hm(run_time)}/{hm(budget)}", f"since_best={hm(since_best)}/{hm(plateau_time)}",
          f"families_since_best={families}/{plateau_families}"]
if settings["CHECKPOINT"] != "off":
    every = seconds(settings["CHECKPOINT"], "CHECKPOINT")
    done = board.get("last_checkpoint_unix")
    age = now - done if done else None
    fields.append("checkpoint=" + ("never" if age is None else hm(age))
                  + ("(due)" if age is None or age >= every else ""))
print("STATUS " + " ".join(fields) + f" stop={stop}")
EOF
