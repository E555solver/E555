#!/bin/bash
##SBATCH --job-name=E555_finalizer
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=4G --time=12:00:00
##SBATCH --output=logs/finalizer_%j.out
#
# =============================================================================
# 02_finalizer_regrow.sh -- take a partial board and re-grow its top rows
# =============================================================================
# WHAT IT DOES
#   Restarts the beam FROM a board instead of from the bottom border. Rows
#   0..FROM stay locked; every piece above them goes back into the pool; a chain
#   database rebuilt WITHOUT the locked pieces (tiny, seconds, low memory)
#   searches the rows above at full beam width.
#
# WHY IT WORKS
#   A beamer board is one lineage out of billions. Freeing four or five rows and
#   re-searching them pulls pieces from deep in the board up to the frontier,
#   which the original run never had the chance to do.
#
# THE ONE KNOB THAT MATTERS: FROM
#   Lower FROM = more rows re-searched = deeper resampling and much slower.
#   Do NOT set it just below the input's top row: that asks the search to redo
#   the exact row that already failed, with the same pieces. Leave four or five
#   rows of daylight.
#
# CHAINS WITH ITSELF
#   Output is the same canonical CSV as the input, so you can feed this script
#   its own output with a different FROM or SEED_RNG.
#
# Near-identical input lines are deduplicated automatically: once the top rows
# are freed they would seed the same search.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- settings ---------------------------------------------------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
PARTIALS="${PARTIALS:-$REPO/data/board_partial_row12.csv}"
ROTATIONS="${ROTATIONS:-}"        # optional Stage A borders CSV; see the note below
OUT_DIR="${OUT_DIR:-final_out}"
FROM="${FROM:-7}"                 # lock rows 0..FROM, re-search everything above
STOP_ROW="${STOP_ROW:-12}"        # up to 14; the top border is Stage C's job
REPEATS="${REPEATS:-3}"           # re-runs per distinct input board
BEAM_WIDTH="${BEAM_WIDTH:-150000}"
FIRST_LINE="${FIRST_LINE:-0}"     # first input CSV line to use
N_LINES="${N_LINES:-20}"          # how many input lines to process
MAX_WALL="${MAX_WALL:-600}"       # seconds for the whole run, 0 = unlimited
# -----------------------------------------------------------------------------

[ -x "$REPO/bin/E555_finalizer" ] || make -C "$REPO" finalizer

# Passing the rotations CSV the boards came from is worth it: the finalizer
# recognizes which border row each board used and keeps that side assignment,
# instead of treating all 56 edge pieces as candidates for every side.
# The rotations file is an optional third positional, so build just that part
# conditionally rather than writing the whole call out twice.
ROT_ARG=(); [ -n "$ROTATIONS" ] && ROT_ARG=("$ROTATIONS")

"$REPO/bin/E555_finalizer" "$SEED" "$PARTIALS" "${ROT_ARG[@]}" \
    --border_row "$FIRST_LINE" --border_row_N "$N_LINES" \
    --finalize_from "$FROM" --finalize_repeats "$REPEATS" \
    --beam_width "$BEAM_WIDTH" --stop_row "$STOP_ROW" \
    --top_columns 0 --lambda_Mahalanobis 10 \
    --max_wall_sec "$MAX_WALL" --out_dir "$OUT_DIR" --verbose

RESULT="$OUT_DIR/beam_completions_finalized_$STOP_ROW.csv"
echo
echo "Boards -> $RESULT"
echo "  python3 $REPO/tools/E555_rank.py $RESULT --seed $SEED --top 10"
echo
echo "Emitting nothing here is a real answer, not an error: it means no left"
echo "column survived from row $FROM to row $STOP_ROW. Lower FROM, or take the"
echo "input boards straight to Stage C (05, 06, 07)."
