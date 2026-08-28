#!/bin/bash
# 04_stage_c_close.sh -- Stage C: herd the breaks onto a border, then close them.
#
#   bash examples/04_stage_c_close.sh
#   bash examples/04_stage_c_close.sh BOARDS=round_out/roundhouse_round1_rot1_W5_miss0.csv
#   bash examples/04_stage_c_close.sh OUT_DIR=close1 SIDE=TR SKIP_TOPPER=1
#
# NEEDS: pip install ortools
#
# Three CP-SAT passes that only make sense together, each fed by the last:
#   1. topper       piles the unavoidable breaks onto one border band
#   2. ender ring   re-threads the 60-cell border ring around them
#   3. ender patch  a localized clean-up of whatever the sweep left
# Neither ender pass can return a board worse than its input, so the chain is
# always safe to run. It prints a rank table at each end -- always re-score,
# because the topper's live "[inc] breaks=N" counts only the open band.
#
# Which SIDE to open, how deep, and when to use a HOLES mask instead:
# examples/README.md
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_example_462.csv
OUT_DIR=stage_c_out     # one file per pass lands here
FIRST_LINE=0            # first input board
N_LINES=1               # how many input boards

SKIP_TOPPER=0           # 1 = start at the ring sweep, e.g. after a roundhouse
SIDE=T                  # T B L R TR TL TB -- open only where the breaks are
WORK_ROWS=4             # band depth. Deeper = stronger and much slower
HOLES=                  # a 16x16 0/1 mask opens exactly those cells instead

REACH=2                 # ender: interior BFS layers around a break
MAX_CHANGES=16          # ender: how many pieces may actually move
WORKERS=8               # CP-SAT workers (a worker is a whole search)
MAX_TIME=120            # seconds per board per pass
STALL_TIME=40           # give up after this long with no improvement
CLUES=0                 # 1 = hold the published Eternity II clue pieces
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
mkdir -p "$OUT_DIR"

# One switch for all three passes: a clue held by the topper and then dropped by
# the ender would be no better than never holding it.
CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
TOPPED="$OUT_DIR/1_topped.csv"
RINGED="$OUT_DIR/2_ringed.csv"
PATCHED="$OUT_DIR/3_patched.csv"

echo "=== before ==="
python3 tools/E555_rank.py "$BOARDS" --seed "$SEED" --top 3

CUR="$BOARDS"
if [ "$SKIP_TOPPER" != 1 ]; then
    echo
    echo "=== pass 1: topper, --side $SIDE ==="
    # HOLES and the band are alternatives, so only the differing arguments are
    # built conditionally -- the call itself is written once.
    if [ -n "$HOLES" ]; then REGION=(--holes "$HOLES")
    else                     REGION=(--side "$SIDE" --work-rows "$WORK_ROWS"); fi
    python3 src/C_tail/E555_topper.py "$SEED" "$CUR" "$TOPPED" "${CLUE_ARG[@]}" \
        --row "$FIRST_LINE" --count "$N_LINES" "${REGION[@]}" \
        --workers "$WORKERS" --max-time "$MAX_TIME" --stall-time "$STALL_TIME" \
        --verbose
    CUR="$TOPPED"
fi

echo
echo "=== pass 2: ender, ring sweep ==="
python3 src/C_tail/E555_ender.py "$SEED" "$CUR" "$RINGED" "${CLUE_ARG[@]}" \
    --mode ring --reach "$REACH" --max-changes "$MAX_CHANGES" \
    --workers "$WORKERS" --max-time "$MAX_TIME" --stall-time "$STALL_TIME" \
    --verbose

echo
echo "=== pass 3: ender, patch ==="
python3 src/C_tail/E555_ender.py "$SEED" "$RINGED" "$PATCHED" "${CLUE_ARG[@]}" \
    --mode patch --reach "$REACH" --max-changes "$MAX_CHANGES" \
    --workers "$WORKERS" --max-time "$MAX_TIME" --stall-time "$STALL_TIME" \
    --verbose

echo
echo "=== after ==="
python3 tools/E555_rank.py "$PATCHED" --seed "$SEED" --top 3
echo
echo "Boards -> $PATCHED"
echo "Still short? 05 proves whether the remaining breaks can be removed at all."
