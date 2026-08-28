#!/bin/bash
# 05_backtracker_dives.sh -- greedy dives to triage, exhaustive DFS to prove.
#
#   bash examples/05_backtracker_dives.sh
#   bash examples/05_backtracker_dives.sh BOARDS=stage_c_out/3_patched.csv
#   bash examples/05_backtracker_dives.sh OUT=run1.csv MODE=any TIME_LIMIT=3600
#
# MODE=stuck fires many randomized dives and keeps the best -- fast triage, no
# proof. MODE=any or lds searches exhaustively: a level that exhausts is a PROOF
# that no better completion exists below it. Start with stuck to find out
# whether a board is worth the proof.
#
# HOLES reopens cells that are already filled, which is what makes a complete
# board improvable. Leave it empty to search a partial board's existing gaps.
# The run writes up to five CSVs, all listed in $OUT.outputs.txt.
# What the modes and orders do: examples/README.md
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_example_462.csv
OUT=backtracked.csv                     # sidecars are named after this
HOLES=data/holes_open_border_TR.csv     # empty = use the board's own gaps
MODE=stuck              # stuck = triage; any or lds = proof
ORDER=mrv               # cell order: mrv rowmajor colmajor snake spiral ...
MAX_MISMATCH=30         # break budget a dive may spend
RESTARTS=200000         # stuck mode: randomized dives per board
FIRST_LINE=0
N_LINES=1
THREADS=8
TIME_LIMIT=300          # seconds per input board, 0 = unlimited
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
cd "$REPO"
[ -d bin ] && [ -d tools ] ||
    { echo "REPO=$REPO is not an E555 checkout -- set REPO at the top" >&2; exit 1; }
[ -x bin/E555_backtracker ] || make backtracker

echo "=== before ==="
python3 tools/E555_rank.py "$BOARDS" --seed "$SEED" --top 3

HOLE_ARG=(); [ -n "$HOLES" ] && HOLE_ARG=(--holes "$HOLES")

bin/E555_backtracker "$SEED" "$BOARDS" "$OUT" "${HOLE_ARG[@]}" \
    --row "$FIRST_LINE" --count "$N_LINES" --threads "$THREADS" \
    --order "$ORDER" --break-mode "$MODE" \
    --max-mismatch "$MAX_MISMATCH" --stuck_restarts "$RESTARTS" \
    --time-limit "$TIME_LIMIT" --print-cmd --verbose

echo
echo "=== after ==="
python3 tools/E555_rank.py "$OUT" --seed "$SEED" --top 3
echo
echo "Files written:"
sed 's/^/  /' "$OUT.outputs.txt"
echo
echo "To turn a promising board into a proof, rerun with MODE=any and a long"
echo "TIME_LIMIT: an exhausted level says no better completion exists."
