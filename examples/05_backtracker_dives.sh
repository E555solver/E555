#!/bin/bash
##SBATCH --job-name=E555_backtracker
##SBATCH --ntasks=1 --cpus-per-task=12 --mem=2G --time=24:00:00
##SBATCH --output=logs/backtracker_%j.out
#
# =============================================================================
# 05_backtracker_dives.sh -- Stage C: DFS triage, and exhaustive proof
# =============================================================================
# WHAT IT DOES
#   Constrained depth-first search over the empty cells, with two very different
#   engines. The distinction matters more than any other setting here.
#
#   MODE=stuck (default) -- GREEDY DIVES, FOR TRIAGE.
#       Each dive takes an exact fit where one exists and a minimal break where
#       none does, never backtracks, and therefore always reaches 256 pieces.
#       ~10k complete boards per second. Cheap enough to run over a whole batch
#       to see which partials deserve a long run. It PROVES NOTHING: it never
#       establishes that a board cannot be completed with fewer breaks.
#
#   MODE=any (or lds) -- EXHAUSTIVE, FOR PROOF.
#       Iterative deepening over the break count. An exhausted level is a
#       theorem: no completion exists with that few broken edges. Cost per level
#       grows roughly exponentially -- run it overnight, on the few boards
#       triage picked out.
#
# A COMPLETE BOARD HAS NO EMPTY CELL, so dives on it are a no-op. Give HOLES a
# mask to reopen a region (data/holes_open_border_*.csv), or feed a partial.
#
# EXPECT THE DEFAULT RUN TO SCORE WORSE THAN ITS INPUT. Reopening the ring of a
# well-optimized board and diving greedily lands around 28 breaks on a board
# that came in with 18 -- that is the documented behaviour, not a bug. Dives are
# for ranking a hundred rough partials cheaply, not for improving a good one.
# Use MODE=any when you want the board to actually get better, or proved.
#
# --order mrv (most-constrained cell first) is the right default for both modes.
# --reverse runs the same search in the other direction and returns a different
# first closure, which is a cheap way to get two distinct results.
#
# Output is re-feedable: run the tool on its own output with a different HOLES
# or MODE to keep improving a record.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- settings ---------------------------------------------------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
BOARDS="${BOARDS:-$REPO/data/board_example_462.csv}"
OUT="${OUT:-backtracked.csv}"
HOLES="${HOLES:-$REPO/data/holes_open_border_TR.csv}"   # empty = use as-is
MODE="${MODE:-stuck}"             # stuck = triage, any/lds = proof
MAX_MISMATCH="${MAX_MISMATCH:-30}"
RESTARTS="${RESTARTS:-200000}"    # stuck mode: randomized dives per board
FIRST_LINE="${FIRST_LINE:-0}"
N_LINES="${N_LINES:-1}"
THREADS="${THREADS:-8}"
TIME_LIMIT="${TIME_LIMIT:-300}"   # seconds per input board, 0 = unlimited
# -----------------------------------------------------------------------------

[ -x "$REPO/bin/E555_backtracker" ] || make -C "$REPO" backtracker

echo "=== before ==="
python3 "$REPO/tools/E555_rank.py" "$BOARDS" --seed "$SEED" --top 3

# --holes only makes sense when there is something to reopen; drop it to search
# a partial board's existing empty cells instead.
# --holes is optional, so build just that part conditionally rather than
# writing the whole call out twice.
HOLE_ARG=(); [ -n "$HOLES" ] && HOLE_ARG=(--holes "$HOLES")

"$REPO/bin/E555_backtracker" "$SEED" "$BOARDS" "$OUT" \
    --row "$FIRST_LINE" --count "$N_LINES" --threads "$THREADS" \
    "${HOLE_ARG[@]}" --order mrv --break-mode "$MODE" \
    --max-mismatch "$MAX_MISMATCH" --stuck_restarts "$RESTARTS" \
    --time-limit "$TIME_LIMIT" --verbose

echo
echo "=== after ==="
python3 "$REPO/tools/E555_rank.py" "$OUT" --seed "$SEED" --top 3
echo
echo "Boards -> $OUT"
echo "To turn a promising board into a proof, rerun with MODE=any and a long"
echo "TIME_LIMIT: an exhausted level says no better completion exists."
