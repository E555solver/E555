#!/bin/bash
# 03_roundhouse_strip.sh -- rotate the board and refill a border strip.
#
#   bash examples/03_roundhouse_strip.sh
#   bash examples/03_roundhouse_strip.sh BOARDS=final_out/beam_completions_finalized_12.csv
#   bash examples/03_roundhouse_strip.sh OUT_DIR=strip1 ROUNDS=2 WIDTH=4
#
# Grows a WIDTH-wide vertical strip instead of a row, so it needs only the edge
# half of the chain database: megabytes and seconds, no 6.4 GB build. The search
# is EXHAUSTIVE and deterministic -- there is no beam, no sampling and no random
# seed -- so finishing without a complete board PROVES none exists for this cut,
# unless a budget stopped it first, which the run says.
#
# Emitting nothing is also a result: "REFUTED" means colour alone rules the band
# out, for any arrangement of the pieces. WIDTH, ROUNDS and ROTATE each change
# what gets torn up; the tables are in examples/README.md
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_partial_row12.csv
OUT_DIR=round_out
THREADS=8
ROUNDS=1                # 1, 2 or 3 bands, freed and refilled in turn
WIDTH=5                 # 2..5 chain length; 0 = narrowest that keeps the board
ROTATE=1                # which side round 1 attacks; -3..3, see the README
TIES=1                  # boards to emit at the deepest reach
BREAKS=0                # >0: also emit a complete board bought with mismatches
FIRST_LINE=0
N_LINES=1
MAX_WALL=600            # seconds, 0 = unlimited
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
[ -x bin/E555_roundhouse ] || make roundhouse

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)

bin/E555_roundhouse "$SEED" "$BOARDS" "${CLUE_ARG[@]}" \
    --border_row "$FIRST_LINE" --border_row_N "$N_LINES" \
    --rounds "$ROUNDS" --strip_width "$WIDTH" --rotate "$ROTATE" \
    --ties "$TIES" --max_breaks "$BREAKS" --threads "$THREADS" \
    --max_wall_sec "$MAX_WALL" --out_dir "$OUT_DIR" --print-cmd --verbose

# The tool names its files after the geometry and splits break-free boards from
# break-bought ones, so read the list it wrote rather than guessing the names.
echo
if [ -s "$OUT_DIR/outputs.txt" ]; then
    echo "Boards written:"
    sed 's/^/  /' "$OUT_DIR/outputs.txt"
    echo
    echo "  python3 tools/E555_rank.py \$(head -1 $OUT_DIR/outputs.txt) --seed $SEED --top 10"
    echo
    echo "A break-free partial scores 480 minus the junctions its EMPTY cells"
    echo "leave open, never a mismatch. Compare it with another partial."
else
    echo "Nothing emitted. If the log says REFUTED, that is a PROOF that this"
    echo "board cannot be refilled this way without breaks: try a different"
    echo "ROTATE, a larger WIDTH to free more pieces, or BREAKS=N for a"
    echo "complete board bought with mismatches."
fi
