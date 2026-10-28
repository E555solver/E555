#!/bin/bash
# checkpoint.sh -- commit a whirlpool campaign's folder and push it to the
# session's branch (agent/launch_whirlpool_agent.md), so that the work survives
# a lost container and the user can read it. Run it from the campaign's lab,
# agent/whirlpool/<id>/lab/. It commits agent/whirlpool/<id>/ and nothing else;
# the lab inside it is gitignored. setup.sh RESUME=<branch> restores it.
#
#   bash ../../checkpoint.sh                         # from the lab
#   bash ../../checkpoint.sh "Co-Authored-By: ..."   # each argument: one more paragraph of the message
#   CHECKPOINT pushed=1a2b3c4 branch=my-branch campaign=20261027-1930 jewels=9 pushed_mb=1.2/100
#
# It refuses, and commits nothing:
#   - on main, master or a detached HEAD: a checkpoint goes to the session's branch;
#   - when the working tree has a change outside the campaign's folder (the
#     campaign touches nothing else; the refusal lists the paths);
#   - a file over 50 MB;
#   - a commit that would take the folder's history over 100 MB: the sum of
#     every distinct file version the folder has ever held, which bounds what
#     the campaign has pushed (the commit is undone, the changes stay staged).
# Exit status 0 = pushed, 1 = committed but not pushed (the next call pushes it
# too), 2 = refused.
set -euo pipefail

FILE_MB=50
TOTAL_MB=100
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$KIT/../.." && pwd)
case "$PWD/" in
    "$KIT"/*/lab/*) ;;
    *) echo "CHECKPOINT refused: run it from a campaign's lab, agent/whirlpool/<id>/lab/"; exit 2 ;;
esac
CAMPAIGN=${PWD#"$KIT/"}; CAMPAIGN=${CAMPAIGN%%/*}
DIR="$KIT/$CAMPAIGN"
REL="agent/whirlpool/$CAMPAIGN"
[ -f "$DIR/scoreboard.json" ] || { echo "CHECKPOINT refused: no campaign in $DIR"; exit 2; }
cd "$REPO"
branch=$(git branch --show-current)
case "$branch" in
    ""|main|master)
        echo "CHECKPOINT refused: on ${branch:-a detached HEAD}; switch to this session's own branch first"
        exit 2 ;;
esac

outside=$(git status --porcelain --untracked-files=all | cut -c4- | grep -v "^$REL/" || true)
if [ -n "$outside" ]; then
    echo "CHECKPOINT refused: changes outside $REL/ (restore them, e.g. git checkout -- PATH, or delete them):"
    sed 's/^/  /' <<< "$outside"
    exit 2
fi
big=$(find "$DIR" -path "$DIR/lab" -prune -o -type f -size +${FILE_MB}M -print)
if [ -n "$big" ]; then
    echo "CHECKPOINT refused: too large for git (over $FILE_MB MB):" $big
    exit 2
fi

history_bytes() {
    git rev-list HEAD -- "$REL" | while read -r c; do git ls-tree -r -l "$c" -- "$REL"; done |
        awk '{print $3, $4}' | sort -u | awk '{s += $2} END {print s + 0}'
}

git add -A -- "$REL"
committed=0
if ! git diff --cached --quiet -- "$REL"; then
    jewels=$(grep -c . "$DIR/frames_seen.txt" || true)
    msg=(-m "Agent: whirlpool campaign $CAMPAIGN checkpoint, $jewels jewels")
    for p in "$@"; do msg+=(-m "$p"); done
    git commit -q "${msg[@]}" -- "$REL"
    committed=1
fi
bytes=$(history_bytes)
mb=$(awk -v b="$bytes" 'BEGIN {printf "%.1f", b / 1048576}')
if [ "$bytes" -gt $((TOTAL_MB * 1048576)) ]; then
    if [ "$committed" = 1 ]; then git reset -q --soft HEAD~1; fi
    echo "CHECKPOINT refused: the folder's history would hold $mb MB, over $TOTAL_MB MB. Largest files:"
    find "$DIR" -path "$DIR/lab" -prune -o -type f -printf '%s %p\n' | sort -rn | head -5 |
        awk '{printf "  %.1f MB %s\n", $1 / 1048576, $2}'
    exit 2
fi
sha=$(git rev-parse --short HEAD)
jewels=$(grep -c . "$DIR/frames_seen.txt" || true)

for wait in 2 4 8 16 0; do
    if git push -q -u origin "$branch" 2>> "$DIR/lab/runs/git.log"; then
        python3 - "$DIR/scoreboard.json" <<'EOF'
import json, sys, time
s = json.load(open(sys.argv[1]))
s["last_checkpoint_unix"] = int(time.time())
json.dump(s, open(sys.argv[1], "w"), indent=1)
EOF
        echo "CHECKPOINT pushed=$sha branch=$branch campaign=$CAMPAIGN jewels=$jewels pushed_mb=$mb/$TOTAL_MB"
        exit 0
    fi
    [ "$wait" = 0 ] || sleep "$wait"
done
echo "CHECKPOINT committed=$sha branch=$branch campaign=$CAMPAIGN, but the push failed" \
     "(see $DIR/lab/runs/git.log); the next checkpoint pushes it too"
exit 1
