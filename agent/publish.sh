#!/bin/bash
# publish.sh -- push one campaign's folder to the shared results branch, for
# the /goal kits' checkpoint scripts (agent/checkpoint.sh,
# agent/whirlpool/checkpoint.sh). The results branch (agent_runs by default)
# holds results only: it shares no history with main, is never merged, and
# each campaign owns one folder of it. Several agents may publish at once:
# each publish rebuilds its commit on the branch's latest commit, replacing
# its own folder only, so two campaigns never conflict.
#
#   bash agent/publish.sh BRANCH=agent_runs SRC=/abs/dir DEST=agent/whirlpool/<id> EXCLUDE=lab \
#        MESSAGE="Agent: ..." [PARAGRAPH ...]
#   PUBLISH pushed=1a2b3c4 branch=agent_runs path=agent/whirlpool/<id> history_mb=1.2/100
#
# The commit is built with git plumbing on a private index: the checkout, its
# branch and its staged changes are never touched. Its tree is the branch's
# latest tree with DEST replaced by SRC's files (EXCLUDE, a path inside SRC,
# left out). A missing branch is created with a README.md of its own. Each
# commit message names the code it came from (Code: <sha> on <branch>).
# Refused, nothing pushed: BRANCH main, master or the branch checked out here;
# a file over FILE_MB; a folder whose history on the branch would pass
# TOTAL_MB (every distinct version of every file it ever held: a bound on what
# the campaign has pushed). Exit status 0 = pushed (or nothing changed),
# 1 = the push failed 5 times (network), 2 = refused or bad usage.
# ---- settings: pass NAME=value on the command line ---------------------------
BRANCH=agent_runs   # the results branch
SRC=                # the folder to publish (absolute)
DEST=               # its path on the branch, e.g. agent/whirlpool/20261027-1930
EXCLUDE=            # a path inside SRC to leave out (e.g. lab)
MESSAGE=            # the commit's subject; further arguments are more paragraphs
FILE_MB=50
TOTAL_MB=100
# -----------------------------------------------------------------------------
SETTINGS=" BRANCH SRC DEST EXCLUDE MESSAGE FILE_MB TOTAL_MB "
paragraphs=()
for arg in "$@"; do
    name=${arg%%=*}
    if [[ "$arg" == *=* && "$SETTINGS" == *" $name "* ]]; then declare "$arg"; else paragraphs+=("$arg"); fi
done
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
refuse() { echo "PUBLISH refused: $*"; exit 2; }
[ -n "$SRC" ] && [ -d "$SRC" ] || refuse "SRC=$SRC is not a folder"
[[ "$DEST" =~ ^agent/[A-Za-z0-9._/-]+$ ]] || refuse "DEST=$DEST must be a path under agent/"
[ -n "$MESSAGE" ] || refuse "MESSAGE is empty"
[[ "$BRANCH" =~ ^[A-Za-z0-9._/-]+$ ]] || refuse "bad branch name '$BRANCH'"
current=$(git -C "$REPO" branch --show-current)
case "$BRANCH" in
    main|master|"$current")
        refuse "BRANCH=$BRANCH: results go to a branch of their own, never to main, master or the branch checked out here" ;;
esac

# the files, and the size limit per file
files=()
while IFS= read -r -d '' f; do files+=("${f#"$SRC/"}"); done < <(
    if [ -n "$EXCLUDE" ]; then find "$SRC" -path "$SRC/$EXCLUDE" -prune -o -type f -print0
    else find "$SRC" -type f -print0; fi)
[ "${#files[@]}" -gt 0 ] || refuse "nothing to publish in $SRC"
big=$(cd "$SRC" && find "${files[@]}" -size +"${FILE_MB}"M)
[ -z "$big" ] || refuse "too large for git (over $FILE_MB MB): $big"

code="Code: $(git -C "$REPO" rev-parse --short HEAD) on ${current:-a detached HEAD}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export GIT_INDEX_FILE="$TMP/index"
cd "$REPO"
git config user.email > /dev/null || export GIT_AUTHOR_NAME="E555 agent" GIT_AUTHOR_EMAIL="agent@localhost" \
    GIT_COMMITTER_NAME="E555 agent" GIT_COMMITTER_EMAIL="agent@localhost"

readme() {
    cat <<EOF
# $BRANCH: results of the E555 agents

This branch holds results, not code. It shares no history with main and is
never merged. Each campaign of an autonomous /goal agent owns one folder and
writes nothing else:

- \`agent/checkpoints/<id>/\`: campaigns of the beat-the-best kit
  (agent/autonomous_agent_mode.md), pushed by agent/checkpoint.sh;
- \`agent/whirlpool/<id>/\`: campaigns of the whirlpool kit
  (agent/whirlpool_agent_mode.md), pushed by agent/whirlpool/checkpoint.sh.

<id> is the campaign's UTC start time. Every commit names the code it ran
("Code: <sha> on <branch>"). Read a file with
\`git show origin/$BRANCH:agent/whirlpool/<id>/README.md\`; a campaign
resumes from here with its kit's setup script, RESUME=$BRANCH CAMPAIGN=<id>.
EOF
}

history_bytes() {   # every distinct file version DEST ever held, up to commit $1
    git rev-list "$1" -- "$DEST" | while read -r c; do git ls-tree -r -l "$c" -- "$DEST"; done |
        awk '{print $3, $4}' | sort -u | awk '{s += $2} END {print s + 0}'
}

for wait in 2 4 8 16 0; do
    tip=""
    if ! heads=$(git ls-remote --heads origin "refs/heads/$BRANCH" 2>> "$TMP/git.log"); then
        [ "$wait" = 0 ] || { sleep "$wait"; continue; }
        break
    fi
    if [ -n "$heads" ]; then
        git fetch -q origin "refs/heads/$BRANCH" 2>> "$TMP/git.log" || { [ "$wait" = 0 ] || sleep "$wait"; continue; }
        tip=$(git rev-parse FETCH_HEAD)
        git read-tree "$tip"
        git ls-files -z -- "$DEST" | git update-index -z --force-remove --stdin
    else
        git read-tree --empty
        blob=$(readme | git hash-object -w --stdin)
        git update-index --add --cacheinfo "100644,$blob,README.md"
    fi
    printf '%s\n' "${files[@]/#/$SRC/}" | git hash-object -w --no-filters --stdin-paths > "$TMP/blobs"
    paste -d ' ' "$TMP/blobs" <(printf '%s\n' "${files[@]}") | while read -r blob rel; do
        mode=100644; [ -x "$SRC/$rel" ] && mode=100755
        printf '%s %s\t%s\n' "$mode" "$blob" "$DEST/$rel"
    done | git update-index --index-info
    tree=$(git write-tree)
    if [ -n "$tip" ] && [ "$tree" = "$(git rev-parse "$tip^{tree}")" ]; then
        echo "PUBLISH unchanged=$(git rev-parse --short "$tip") branch=$BRANCH path=$DEST"
        exit 0
    fi
    msg=(-m "$MESSAGE")
    for p in "${paragraphs[@]+"${paragraphs[@]}"}"; do msg+=(-m "$p"); done
    msg+=(-m "$code")
    commit=$(git commit-tree "$tree" ${tip:+-p "$tip"} "${msg[@]}")
    bytes=$(history_bytes "$commit")
    mb=$(awk -v b="$bytes" 'BEGIN {printf "%.1f", b / 1048576}')
    if [ "$bytes" -gt $((TOTAL_MB * 1048576)) ]; then
        echo "PUBLISH refused: $DEST would hold $mb MB of history on $BRANCH, over $TOTAL_MB MB. Largest files:"
        (cd "$SRC" && find "${files[@]}" -printf '%s %p\n') | sort -rn | head -5 |
            awk '{printf "  %.1f MB %s\n", $1 / 1048576, $2}'
        exit 2
    fi
    if git push -q origin "$commit:refs/heads/$BRANCH" 2>> "$TMP/git.log"; then
        git update-ref "refs/remotes/origin/$BRANCH" "$commit"
        echo "PUBLISH pushed=$(git rev-parse --short "$commit") branch=$BRANCH path=$DEST history_mb=$mb/$TOTAL_MB"
        exit 0
    fi
    # rejected: another campaign pushed first, or the network failed. Rebuild on the new tip.
    [ "$wait" = 0 ] || sleep "$wait"
done
echo "PUBLISH failed: could not push to $BRANCH after 5 tries:"
tail -5 "$TMP/git.log" | sed 's/^/  /'
exit 1
