#!/bin/bash
# checkpoint.sh -- save a campaign to git, so that a cloud session that loses
# its container can resume it (agent/autonomous_agent_mode.md, section 11).
# Run it from the campaign's lab, agent/labs/<id>/. It publishes the lab's
# state files as agent/checkpoints/<id>/ on the shared results branch
# (agent_runs unless BRANCH= says otherwise, remembered in scoreboard.json),
# through agent/publish.sh: that branch holds results only, each campaign owns
# one folder of it, and the checkout and its branch are never touched.
# `bash agent/setup_lab.sh RESUME=<results branch> CAMPAIGN=<id>` restores it.
#
#   bash ../../checkpoint.sh                         # from the lab
#   bash ../../checkpoint.sh BRANCH=agent_runs       # the first call names the branch
#   bash ../../checkpoint.sh "Co-Authored-By: ..."   # each other argument: one more paragraph of the message
#   CHECKPOINT pushed=1a2b3c4 branch=agent_runs campaign=20261027-1930 best=465 runs=12
#
# Saved: scoreboard.json, run_log.csv, JOURNAL.md, STRATEGY.md, SUMMARY.md,
# frontier/*.csv, pool/distilled*.csv and pool/consumed.txt. Not saved: runs/,
# bin/ and the lab's sources, which setup_lab.sh rebuilds, and the chain
# database E555.db, which the next F4 run rebuilds. A file over 50 MB, or a
# folder whose history on the branch would pass 100 MB, is refused, so
# nothing that large reaches git. Exit status 0 = pushed (or nothing new),
# 1 = the push failed (the next call pushes it too), 2 = refused.
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
branch=""; paragraphs=()
for arg in "$@"; do
    case "$arg" in BRANCH=*) branch=${arg#BRANCH=} ;; *) paragraphs+=("$arg") ;; esac
done
read -r branch best runs < <(python3 - "$branch" <<'EOF'
import json, sys
s = json.load(open("scoreboard.json"))
s["results_branch"] = sys.argv[1] or s.get("results_branch") or "agent_runs"
json.dump(s, open("scoreboard.json", "w"), indent=1)
print(s["results_branch"], s["best_score"], s["total_runs"])
EOF
)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/frontier" "$TMP/pool"
for f in scoreboard.json run_log.csv JOURNAL.md STRATEGY.md SUMMARY.md \
         pool/distilled*.csv pool/consumed.txt frontier/*.csv; do
    if [ -f "$f" ]; then cp "$f" "$TMP/$f"; fi
done
rc=0
out=$(bash "$REPO/agent/publish.sh" BRANCH="$branch" SRC="$TMP" DEST="agent/checkpoints/$CAMPAIGN" \
          MESSAGE="Agent: campaign $CAMPAIGN checkpoint, best $best after $runs runs" \
          "${paragraphs[@]+"${paragraphs[@]}"}" 2>&1) || rc=$?
case "$rc" in
    0)  python3 - <<'EOF'
import json, time
s = json.load(open("scoreboard.json"))
s["last_checkpoint_unix"] = int(time.time())
json.dump(s, open("scoreboard.json", "w"), indent=1)
EOF
        sha=$(sed -n 's/^PUBLISH [a-z]*=\([0-9a-f]*\).*/\1/p' <<< "$out")
        echo "CHECKPOINT pushed=$sha branch=$branch campaign=$CAMPAIGN best=$best runs=$runs" ;;
    1)  echo "CHECKPOINT failed: the push to $branch failed; the next checkpoint pushes it too"
        sed 's/^/  /' <<< "$out" ;;
    *)  echo "CHECKPOINT refused:"
        sed 's/^/  /' <<< "$out" ;;
esac
exit "$rc"
