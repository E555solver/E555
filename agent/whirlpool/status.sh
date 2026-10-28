#!/bin/bash
# status.sh -- the STATUS line of a whirlpool campaign
# (agent/launch_whirlpool_agent.md), from its scoreboard.json, runs.csv,
# frames_seen.txt, whirlpool_index.csv and best_boards.csv. Run it from the
# campaign's lab, agent/whirlpool/<id>/lab/.
# The clocks count run time, the runtime_s column of runs.csv (harness.sh and
# timed.sh write it), not wall time: a usage-limit wait, a replaced container
# or the agent's own thinking costs nothing.
#
#   bash ../../status.sh BUDGET=8h PLATEAU=2h CHECKPOINT=30m    # the first call stores the settings
#   bash ../../status.sh                                        # later calls reuse them
#   STATUS best=v03_keep_ring@1a2b3c4d+none jewels/h=2.4(6/2.5h) jewels=9 depth_max=11 best_dived=455/480 run_time=3h10m/8h00m since_best=0h40m/2h00m checkpoint=0h12m stop=none
#
# best is the candidate harness.sh ranks first (scoreboard.json "best"), with
# its jewels per hour over all its standard harness runs; jewels counts the
# campaign's distinct jewel frames, and depth_max the deepest jewel. best_dived
# is agent/referee.sh's score of the best board in best_boards.csv. run_time
# sums runtime_s since goal_start_unix (the first call sets it when it is
# null), since_best since the best candidate last changed (best_unix). stop is
# budget once run_time reaches BUDGET, else none: the campaign never stops on a
# plateau, it switches to production (the kit, "Method"). checkpoint is the wall
# time since the last pushed checkpoint, marked (due) once it reaches
# CHECKPOINT; left out while CHECKPOINT is off.
# Durations are like 8h, 90m or 1h30m. Exit status 0; 1 or 2 on a bad setting.
# ---- settings: pass NAME=value on the command line ---------------------------
BUDGET=             # run time the campaign may spend (default 8h)
PLATEAU=            # run time without a new best candidate before production (default 2h)
CHECKPOINT=         # wall time between checkpoints, or off (default 30m)
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

KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
case "$PWD/" in
    "$KIT"/*/lab/*) ;;
    *) echo "status: run it from a campaign's lab, agent/whirlpool/<id>/lab/" >&2; exit 2 ;;
esac
DIR=${PWD#"$KIT/"}; DIR="$KIT/${DIR%%/*}"
[ -f "$DIR/scoreboard.json" ] && [ -f "$DIR/runs.csv" ] ||
    { echo "status: no campaign in $DIR; run setup.sh first" >&2; exit 2; }
dived=none
if [ -s "$DIR/best_boards.csv" ]; then
    ref=$(bash "$KIT/../referee.sh" "$DIR/best_boards.csv" 2>/dev/null || true)
    case "$ref" in *verdict=VALID*) dived=$(sed -n 's/.* score=\([0-9]*\) .*/\1/p' <<< "$ref")/480 ;; esac
fi
python3 - "$DIR" "$BUDGET" "$PLATEAU" "$CHECKPOINT" "$dived" <<'EOF'
import csv, json, re, sys, time
from pathlib import Path

d, budget_s, plateau_s, checkpoint_s, dived = sys.argv[1:]
d = Path(d)

def seconds(text, name):
    m = re.fullmatch(r"(?:(\d+)h)?(?:(\d+)m)?", text.strip())
    if not text.strip() or not m:
        sys.exit(f"status: {name}={text} is not a duration like 8h, 90m or 1h30m")
    return int(m.group(1) or 0) * 3600 + int(m.group(2) or 0) * 60

def hm(s):
    s = max(0, int(s))
    return f"{s // 3600}h{s % 3600 // 60:02d}m"

now = int(time.time())
board = json.load(open(d / "scoreboard.json"))
settings = board.setdefault("settings", {"BUDGET": "8h", "PLATEAU": "2h", "CHECKPOINT": "30m"})
for name, value in zip(("BUDGET", "PLATEAU", "CHECKPOINT"), (budget_s, plateau_s, checkpoint_s)):
    if value:
        settings[name] = value
budget = seconds(settings["BUDGET"], "BUDGET")
plateau = seconds(settings["PLATEAU"], "PLATEAU")
if board.get("goal_start_unix") is None:
    board["goal_start_unix"] = now
json.dump(board, open(d / "scoreboard.json", "w"), indent=1)

start = board["goal_start_unix"]
last_best = board.get("best_unix") or start
run_time = since_best = 0.0
with open(d / "runs.csv", newline="") as f:
    for row in csv.DictReader(f):
        try:
            logged, runtime = int(row["unix"]), float(row["runtime_s"])
        except (KeyError, TypeError, ValueError):
            continue
        if logged >= start:
            run_time += runtime
            if logged > last_best:
                since_best += runtime

best = board.get("best")
if best and best in board.get("candidates", {}):
    c = board["candidates"][best]
    hours = c.get("minutes", 0) / 60
    rate = c.get("jewels", 0) / hours if hours else 0.0
    best_txt = f"best={best} jewels/h={rate:.1f}({c.get('jewels', 0)}/{hours:.1f}h)"
else:
    best_txt = "best=none jewels/h=0.0"
frames = [l for l in (d / "frames_seen.txt").read_text().split() if l] \
    if (d / "frames_seen.txt").exists() else []
depth = 0
if (d / "whirlpool_index.csv").exists():
    with open(d / "whirlpool_index.csv", newline="") as f:
        for row in csv.DictReader(f):
            try:
                if row["jewel"] == "yes":
                    depth = max(depth, int(row["depth"]))
            except (KeyError, TypeError, ValueError):
                pass
stop = "budget" if run_time >= budget else "none"
fields = [best_txt, f"jewels={len(frames)}", f"depth_max={depth or '-'}", f"best_dived={dived}",
          f"run_time={hm(run_time)}/{hm(budget)}", f"since_best={hm(since_best)}/{hm(plateau)}"]
if settings["CHECKPOINT"] != "off":
    every = seconds(settings["CHECKPOINT"], "CHECKPOINT")
    done = board.get("last_checkpoint_unix")
    age = now - done if done else None
    fields.append("checkpoint=" + ("never" if age is None else hm(age))
                  + ("(due)" if age is None or age >= every else ""))
print("STATUS " + " ".join(fields) + f" stop={stop}")
EOF
