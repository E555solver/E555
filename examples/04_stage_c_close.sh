#!/bin/bash
##SBATCH --job-name=E555_stage_c
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=2G --time=06:00:00
##SBATCH --output=logs/stage_c_%j.out
#
# =============================================================================
# 04_stage_c_close.sh -- Stage C: herd the breaks, then close them
# =============================================================================
# NEEDS:  pip install ortools
#
# THE SEQUENCE, AND WHY IT IS ONE SCRIPT
#   These three CP-SAT passes are documented as one recipe because they only
#   make sense together: the topper deliberately PILES breaks onto a border
#   band, and the ender's two modes are what un-pile them.
#
#     1. topper       opens a band along one border and re-solves it under a
#                     strictly lexicographic objective: fewest broken edges,
#                     then push the unavoidable ones to the nearest horizontal
#                     border, then slide them along it toward a corner.
#                     Measuring to the NEAREST border keeps the worst trip at
#                     7+7 cells instead of 15+15 and lets all four corners
#                     share the load.
#     2. ender ring   opens every cell within REACH BFS layers of a break plus
#                     the whole 60-cell border ring. A border break heals by an
#                     avalanche cascading around the frame, so this comes right
#                     after the topper piled them there.
#     3. ender patch  opens a box around what is left and, besides minimising
#                     breaks, COMPACTS them. The finishing pass.
#
#   Neither ender pass can return a board worse than its input, so running the
#   chain is always safe. Set SKIP_TOPPER=1 to start at the ring sweep -- right
#   when the damage is already on the border, e.g. after a roundhouse run.
#
# THE TOPPER'S TWO KNOBS
#   SIDE       which band opens: T B L R, or the L-shaped pairs TR TL TB.
#              Open ONLY where the breaks are: a pass over a clean side wastes
#              time and can spread a break into a clean row.
#   WORK_ROWS  how deep the band is. Deeper = stronger and much slower.
#
#   HOLES replaces the band with a 16x16 0/1 mask and the tool opens exactly
#   those cells: SIDE and WORK_ROWS stop applying. That is the only way to
#   reach a ragged region around a cluster of breaks, or the interior, which no
#   band can express. The ender and the backtracker read the same masks, so one
#   mask drives all three.
#
# BEFORE AND AFTER
#   Always re-score the output. The topper's live "[inc] breaks=N" telemetry
#   counts only junctions touching the open band, so it is not the board score.
#   This script prints a rank table at each end.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- settings ---------------------------------------------------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
BOARDS="${BOARDS:-$REPO/data/board_example_462.csv}"
OUT_DIR="${OUT_DIR:-stage_c_out}" # directory: one file per pass lands here
FIRST_LINE="${FIRST_LINE:-0}"     # first input board
N_LINES="${N_LINES:-1}"           # how many input boards

SKIP_TOPPER="${SKIP_TOPPER:-0}"   # 1 = go straight to the ring sweep
SIDE="${SIDE:-T}"                 # T B L R TR TL TB
WORK_ROWS="${WORK_ROWS:-4}"       # band depth
HOLES="${HOLES:-}"                # a mask opens exactly those cells instead

REACH="${REACH:-2}"               # ender: interior BFS layers around a break
MAX_CHANGES="${MAX_CHANGES:-16}"  # ender: how many pieces may actually move

WORKERS="${WORKERS:-8}"           # CP-SAT workers (a worker is a whole search,
                                  # which is why this is not called threads)
MAX_TIME="${MAX_TIME:-120}"       # seconds per board per pass
STALL_TIME="${STALL_TIME:-40}"    # give up after this long with no improvement
CLUES="${CLUES:-0}"                # 1 = hold the published Eternity II clue
                                  # pieces on their cells and spins
# -----------------------------------------------------------------------------

mkdir -p "$OUT_DIR"
# One switch for all three passes: a clue held by the topper and then dropped by
# the ender would be no better than never holding it.
CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
TOPPED="$OUT_DIR/1_topped.csv"
RINGED="$OUT_DIR/2_ringed.csv"
PATCHED="$OUT_DIR/3_patched.csv"

echo "=== before ==="
python3 "$REPO/tools/E555_rank.py" "$BOARDS" --seed "$SEED" --top 3

CUR="$BOARDS"
if [ "$SKIP_TOPPER" != 1 ]; then
    echo
    echo "=== pass 1: topper, --side $SIDE ==="
    # HOLES and the band are alternatives, so only the differing arguments are
    # built conditionally -- the call itself is written once.
    if [ -n "$HOLES" ]; then REGION=(--holes "$HOLES")
    else                     REGION=(--side "$SIDE" --work-rows "$WORK_ROWS"); fi
    python3 "$REPO/src/C_tail/E555_topper.py" "$SEED" "$CUR" "$TOPPED" "${CLUE_ARG[@]}" \
        --row "$FIRST_LINE" --count "$N_LINES" "${REGION[@]}" \
        --workers "$WORKERS" --max-time "$MAX_TIME" --stall-time "$STALL_TIME" \
        --verbose
    CUR="$TOPPED"
fi

echo
echo "=== pass 2: ender, ring sweep ==="
python3 "$REPO/src/C_tail/E555_ender.py" "$SEED" "$CUR" "$RINGED" "${CLUE_ARG[@]}" \
    --mode ring --reach "$REACH" --max-changes "$MAX_CHANGES" \
    --workers "$WORKERS" --max-time "$MAX_TIME" --stall-time "$STALL_TIME" \
    --verbose

echo
echo "=== pass 3: ender, patch ==="
python3 "$REPO/src/C_tail/E555_ender.py" "$SEED" "$RINGED" "$PATCHED" "${CLUE_ARG[@]}" \
    --mode patch --reach "$REACH" --max-changes "$MAX_CHANGES" \
    --workers "$WORKERS" --max-time "$MAX_TIME" --stall-time "$STALL_TIME" \
    --verbose

echo
echo "=== after ==="
python3 "$REPO/tools/E555_rank.py" "$PATCHED" --seed "$SEED" --top 3
echo
echo "Boards -> $PATCHED"
echo "Still short? 05 proves whether the remaining breaks can be removed at all."
