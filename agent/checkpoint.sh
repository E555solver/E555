#!/bin/bash
# checkpoint.sh -- save the campaign in agent/lab to git, so that a cloud
# session that loses its container can resume it (agent/autonomous_agent_mode.md,
# section 11). It copies the lab's state files into agent/checkpoint/, commits
# that directory alone, and pushes the current branch. It refuses main and
# master: a checkpoint lives on the session's own branch, and agent/checkpoint/
# stays out of main. `bash agent/setup_lab.sh RESUME=<branch>` restores it.
#
#   bash agent/checkpoint.sh                         # from anywhere
#   bash agent/checkpoint.sh "Co-Authored-By: ..."   # each argument: one more paragraph of the message
#   CHECKPOINT pushed=1a2b3c4 branch=my-branch best=465 runs=12
#
# Saved: scoreboard.json, run_log.csv, JOURNAL.md, STRATEGY.md, SUMMARY.md,
# frontier/*.csv, pool/distilled*.csv and pool/consumed.txt. Not saved: runs/,
# cache/, bin/ and the lab's sources, which setup_lab.sh rebuilds. Exit status
# 0 = pushed, 1 = committed but not pushed (the next call pushes it too),
# 2 = refused.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LAB="$REPO/agent/lab"
CK="$REPO/agent/checkpoint"
[ -f "$LAB/scoreboard.json" ] || { echo "CHECKPOINT refused: no campaign in $LAB"; exit 2; }
branch=$(git -C "$REPO" branch --show-current)
case "$branch" in
    ""|main|master)
        echo "CHECKPOINT refused: on ${branch:-a detached HEAD}; switch to this session's own branch first"
        exit 2 ;;
esac

rm -rf "$CK"
mkdir -p "$CK/frontier" "$CK/pool"
cd "$LAB"
for f in scoreboard.json run_log.csv JOURNAL.md STRATEGY.md SUMMARY.md \
         pool/distilled*.csv pool/consumed.txt; do
    if [ -f "$f" ]; then cp "$f" "$CK/$f"; fi
done
cp frontier/*.csv "$CK/frontier/"
read -r best runs < <(python3 -c 'import json; s = json.load(open("scoreboard.json")); print(s["best_score"], s["total_runs"])')

cd "$REPO"
git add -A -- agent/checkpoint
if ! git diff --cached --quiet -- agent/checkpoint; then
    msg=(-m "Agent: campaign checkpoint, best $best after $runs runs")
    for p in "$@"; do msg+=(-m "$p"); done
    git commit -q "${msg[@]}" -- agent/checkpoint
fi
sha=$(git rev-parse --short HEAD)

for wait in 2 4 8 16 0; do
    if git push -q -u origin "$branch" 2>> "$LAB/runs/git.log"; then
        python3 - "$LAB/scoreboard.json" <<'EOF'
import json, sys, time
s = json.load(open(sys.argv[1]))
s["last_checkpoint_unix"] = int(time.time())
json.dump(s, open(sys.argv[1], "w"), indent=1)
EOF
        echo "CHECKPOINT pushed=$sha branch=$branch best=$best runs=$runs"
        exit 0
    fi
    [ "$wait" = 0 ] || sleep "$wait"
done
echo "CHECKPOINT committed=$sha branch=$branch, but the push failed (see $LAB/runs/git.log); the next checkpoint pushes it too"
exit 1
