#!/bin/bash
# referee.sh -- the official score of the best board in a board file, for the
# /goal kit (agent/autonomous_agent_mode.md). It runs the repository's own
# tools/E555_rank.py and tools/E555_viewer.py against the repository's own
# data/seed_Edge5.txt, wherever it is called from, so editing the copies in a
# campaign's lab (agent/labs/<id>/) cannot change its verdict.
#
#   bash ../../referee.sh frontier/frontier.csv       (from a lab, agent/labs/<id>/)
#   REFEREE verdict=VALID score=465 breaks=15 placed=256 frame_violations=0 id=... file=...
#
# The best board is the one tools/E555_rank.py ranks first (fewest breaks).
# VALID needs all 256 pieces placed, each once, at spins 0..3, and every frame
# cell grey on its outward side. The frame check matters because the score
# counts any two equal colours as a match, grey included: a board with frame
# pieces moved inside could collect matches a legal board cannot. Exit status
# 0 = VALID, 1 = INVALID, 2 = usage.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SEED="$REPO/data/seed_Edge5.txt"
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: bash referee.sh BOARDS.csv" >&2; exit 2; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# rank skips malformed rows (two pieces on a cell, a spin outside 0..3) on stderr
python3 "$REPO/tools/E555_rank.py" "$1" --seed_file "$SEED" --top 1 \
    --out "$TMP/best.csv" --rescore --quiet > /dev/null 2> "$TMP/rank.err" || true
if [ ! -s "$TMP/best.csv" ]; then
    echo "REFEREE verdict=INVALID reason=no_well_formed_board file=$1"
    exit 1
fi

# the same best board again, for its id and rank's own count (--csv takes no --out)
python3 "$REPO/tools/E555_rank.py" "$1" --seed_file "$SEED" --top 1 --csv \
    > "$TMP/rank.csv" 2>> "$TMP/rank.err"
id=$(awk -F, 'NR == 2 {print $2}' "$TMP/rank.csv")
ranked=$(awk -F, 'NR == 2 {print $4}' "$TMP/rank.csv")

python3 "$REPO/tools/E555_viewer.py" "$TMP/best.csv" --seed_file "$SEED" --no_url > "$TMP/view.txt"
placed=$(sed -n 's/^  Placed cells  : *\([0-9]*\) \/ 256.*/\1/p' "$TMP/view.txt")
score=$(sed -n 's/^  Correct edges : *\([0-9]*\) \/ 480.*/\1/p' "$TMP/view.txt")
viol=$(sed -n 's/^  Border check  : *\([0-9]*\) frame VIOLATIONS.*/\1/p' "$TMP/view.txt")
viol=${viol:-0}

reason=""
[ "$placed" = 256 ] || reason="incomplete"
[ "$viol" = 0 ] || reason="${reason:+$reason,}frame"
[ "$score" = "$ranked" ] || reason="${reason:+$reason,}scorers_disagree"

if [ -z "$reason" ]; then
    echo "REFEREE verdict=VALID score=$score breaks=$((480 - score)) placed=$placed" \
         "frame_violations=0 id=$id file=$1"
else
    echo "REFEREE verdict=INVALID reason=$reason score=$score placed=$placed" \
         "frame_violations=$viol id=$id file=$1"
    exit 1
fi
