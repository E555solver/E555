#!/bin/bash
# checkpoint.sh -- push a whirlpool campaign's folder to the shared results
# branch (agent/launch_whirlpool_agent.md), so that the work survives a lost
# container and the user can read it. Run it from the campaign's lab,
# agent/whirlpool/<id>/lab/. It publishes agent/whirlpool/<id>/, without its
# lab/, through agent/publish.sh: the results branch (agent_runs unless
# BRANCH= says otherwise, remembered in scoreboard.json) holds results only,
# and the checkout and its branch are never touched. setup.sh
# RESUME=<results branch> CAMPAIGN=<id> restores it.
#
#   bash ../../checkpoint.sh                              # from the lab
#   bash ../../checkpoint.sh BRANCH=agent_runs            # the first call names the branch
#   bash ../../checkpoint.sh "Co-Authored-By: ..."        # each other argument: one more paragraph of the message
#   CHECKPOINT pushed=1a2b3c4 branch=agent_runs campaign=20261027-1930 jewels=9 history_mb=1.2/100
#
# It refuses, and pushes nothing:
#   - when the working tree has a change outside the campaign's folder (the
#     campaign touches nothing else; the refusal lists the paths);
#   - a file over 50 MB;
#   - a push that would take the folder's history over 100 MB: the sum of
#     every distinct file version the folder has ever held on the branch,
#     which bounds what the campaign has pushed.
# Exit status 0 = pushed (or nothing new), 1 = the push failed (the next call
# pushes it too), 2 = refused.
set -euo pipefail

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
branch=""; paragraphs=()
for arg in "$@"; do
    case "$arg" in BRANCH=*) branch=${arg#BRANCH=} ;; *) paragraphs+=("$arg") ;; esac
done
branch=$(python3 - "$DIR/scoreboard.json" "$branch" <<'EOF'
import json, sys
path, branch = sys.argv[1:]
s = json.load(open(path))
s["results_branch"] = branch or s.get("results_branch") or "agent_runs"
json.dump(s, open(path, "w"), indent=1)
print(s["results_branch"])
EOF
)

outside=$(git -C "$REPO" status --porcelain --untracked-files=all | cut -c4- | grep -v "^$REL/" || true)
if [ -n "$outside" ]; then
    echo "CHECKPOINT refused: changes outside $REL/ (restore them, e.g. git checkout -- PATH, or delete them):"
    sed 's/^/  /' <<< "$outside"
    exit 2
fi

jewels=$(grep -c . "$DIR/frames_seen.txt" || true)
rc=0
out=$(bash "$REPO/agent/publish.sh" BRANCH="$branch" SRC="$DIR" DEST="$REL" EXCLUDE=lab \
          MESSAGE="Agent: whirlpool campaign $CAMPAIGN checkpoint, $jewels jewels" \
          "${paragraphs[@]+"${paragraphs[@]}"}" 2>&1) || rc=$?
case "$rc" in
    0)  python3 - "$DIR/scoreboard.json" <<'EOF'
import json, sys, time
s = json.load(open(sys.argv[1]))
s["last_checkpoint_unix"] = int(time.time())
json.dump(s, open(sys.argv[1], "w"), indent=1)
EOF
        sha=$(sed -n 's/^PUBLISH [a-z]*=\([0-9a-f]*\).*/\1/p' <<< "$out")
        mb=$(sed -n 's/.* history_mb=\([^ ]*\).*/\1/p' <<< "$out")
        echo "CHECKPOINT pushed=$sha branch=$branch campaign=$CAMPAIGN jewels=$jewels history_mb=${mb:-unchanged}" ;;
    1)  echo "CHECKPOINT failed: the push to $branch failed; the next checkpoint pushes it too"
        sed 's/^/  /' <<< "$out" ;;
    *)  echo "CHECKPOINT refused:"
        sed 's/^/  /' <<< "$out" ;;
esac
exit "$rc"
