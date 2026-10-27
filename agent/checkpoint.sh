#!/bin/bash
# checkpoint.sh -- save a campaign to git, so that a cloud session that loses
# its container can resume it (agent/autonomous_agent_mode.md, section 11).
# Run it from the campaign's lab, agent/labs/<id>/. It copies the lab's state
# files into agent/checkpoints/<id>/, commits that directory alone, and pushes
# the current branch. Each campaign has a directory of its own, so the
# checkpoints of several campaigns never overlap. It refuses main and master:
# a checkpoint is pushed to the session's own branch.
# `bash agent/setup_lab.sh RESUME=<branch>` restores it.
#
#   bash ../../checkpoint.sh                         # from the lab
#   bash ../../checkpoint.sh "Co-Authored-By: ..."   # each argument: one more paragraph of the message
#   CHECKPOINT pushed=1a2b3c4 branch=my-branch campaign=20261027-1930 best=465 runs=12
#
# Saved: scoreboard.json, run_log.csv, JOURNAL.md, STRATEGY.md, SUMMARY.md,
# frontier/*.csv, pool/distilled*.csv and pool/consumed.txt. Not saved: runs/,
# cache/, bin/ and the lab's sources, which setup_lab.sh rebuilds. Exit status
# 0 = pushed, 1 = committed but not pushed (the next call pushes it too),
# 2 = refused.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LAB=$PWD
case "$LAB" in
    "$REPO/agent/labs"/*) ;;
    *) echo "CHECKPOINT refused: run it from a campaign's lab, agent/labs/<id>/"; exit 2 ;;
esac
CAMPAIGN=${LAB#"$REPO/agent/labs/"}
case "$CAMPAIGN" in
    */*) echo "CHECKPOINT refused: run it from the lab's top folder, agent/labs/${CAMPAIGN%%/*}/"; exit 2 ;;
esac
[ -f scoreboard.json ] || { echo "CHECKPOINT refused: no campaign in $LAB"; exit 2; }
CK="$REPO/agent/checkpoints/$CAMPAIGN"
branch=$(git -C "$REPO" branch --show-current)
case "$branch" in
    ""|main|master)
        echo "CHECKPOINT refused: on ${branch:-a detached HEAD}; switch to this session's own branch first"
        exit 2 ;;
esac

rm -rf "$CK"
mkdir -p "$CK/frontier" "$CK/pool"
for f in scoreboard.json run_log.csv JOURNAL.md STRATEGY.md SUMMARY.md \
         pool/distilled*.csv pool/consumed.txt; do
    if [ -f "$f" ]; then cp "$f" "$CK/$f"; fi
done
cp frontier/*.csv "$CK/frontier/"
read -r best runs < <(python3 -c 'import json; s = json.load(open("scoreboard.json")); print(s["best_score"], s["total_runs"])')

cd "$REPO"
git add -A -- "agent/checkpoints/$CAMPAIGN"
if ! git diff --cached --quiet -- "agent/checkpoints/$CAMPAIGN"; then
    msg=(-m "Agent: campaign $CAMPAIGN checkpoint, best $best after $runs runs")
    for p in "$@"; do msg+=(-m "$p"); done
    git commit -q "${msg[@]}" -- "agent/checkpoints/$CAMPAIGN"
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
        echo "CHECKPOINT pushed=$sha branch=$branch campaign=$CAMPAIGN best=$best runs=$runs"
        exit 0
    fi
    [ "$wait" = 0 ] || sleep "$wait"
done
echo "CHECKPOINT committed=$sha branch=$branch campaign=$CAMPAIGN, but the push failed (see $LAB/runs/git.log); the next checkpoint pushes it too"
exit 1
