#!/bin/bash
# 02_finalizer_regrow.sh -- lock the bottom rows of a board and re-grow the top.
#
#   bash examples/02_finalizer_regrow.sh
#   bash examples/02_finalizer_regrow.sh BOARDS=beam_out/beam_completions_random_10.csv
#   bash examples/02_finalizer_regrow.sh OUT_DIR=run7 THREADS=16 FROM=5
#
# Rows 0..FROM stay put; every piece above them goes back in the pool and a
# database rebuilt WITHOUT the locked pieces (small, seconds) re-searches the
# rows above. Output is the same canonical CSV as the input, so this script can
# be fed its own output with a different FROM or RNG_SEED.
#
# Emitting nothing is a real answer, not an error: no left column survived from
# FROM to STOP_ROW. Why FROM=7 and not the tool's default of 5, and what the
# other settings do: examples/README.md
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_partial_row12.csv     # boards to re-grow
ROTATIONS=                              # optional Stage A rotations CSV
OUT_DIR=final_out
THREADS=8
FROM=7                  # lock rows 0..FROM, re-search everything above
STOP_ROW=12             # up to 14; the top border is Stage C's job
REPEATS=3               # stochastic re-runs per input board
BEAM_WIDTH=150000
FIRST_LINE=0            # first input CSV line to use
N_LINES=20              # how many input lines to process
MAX_WALL=600            # seconds for the whole run, 0 = unlimited
COLUMNS=0               # left columns per board; 0 = enumerate every one
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
[ -x bin/E555_finalizer ] || make finalizer

# Passing the rotations CSV the boards came from is worth it: the finalizer
# recognizes which border row each board used and keeps that side assignment
# instead of treating all 56 edge pieces as candidates for every side.
ROT_ARG=(); [ -n "$ROTATIONS" ] && ROT_ARG=("$ROTATIONS")
CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)

bin/E555_finalizer "$SEED" "$BOARDS" "${ROT_ARG[@]}" "${CLUE_ARG[@]}" \
    --start_row "$FIRST_LINE" --num_rows "$N_LINES" \
    --finalize_from "$FROM" --finalize_repeats "$REPEATS" \
    --beam_width "$BEAM_WIDTH" --stop_row "$STOP_ROW" \
    --top_columns "$COLUMNS" --threads "$THREADS" \
    --wall_time "$MAX_WALL" --out_dir "$OUT_DIR" --print_cmd --verbose

echo
if [ -s "$OUT_DIR/outputs.txt" ]; then
    echo "Boards written:"
    sed 's/^/  /' "$OUT_DIR/outputs.txt"
    echo
    echo "  python3 tools/E555_rank.py \$(head -1 $OUT_DIR/outputs.txt) --seed_file $SEED --top 10"
else
    echo "Nothing emitted: no left column survived from row $FROM to row $STOP_ROW."
    echo "That is a real answer. Lower FROM, raise MAX_WALL, or take the input"
    echo "boards straight to Stage C (03, 04, 05)."
fi
