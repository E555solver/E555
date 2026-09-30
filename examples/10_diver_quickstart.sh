#!/bin/bash
# 10_diver_quickstart.sh -- finish partial boards with end dives and polish.
#
#   bash examples/10_diver_quickstart.sh
#   bash examples/10_diver_quickstart.sh BOARDS=beam_out/beam_completions_0_10.csv ROTATIONS=rotations.csv
#   bash examples/10_diver_quickstart.sh END_POLISH=-1 EMIT_SCORE=450
#   bash examples/10_diver_quickstart.sh HOLES=top:6 STOP_ROW=11     # rebuild rows 10-11 first
#
# The diver runs the beamer's --end_dive on boards you already have: every
# placed cell stays, the open cells are filled by greedy random dives that
# allow broken edges, the best boards learn from their own best dives, and
# END_POLISH hill-climbs and kick-polishes the winners. Rows with the same
# config id are dived as one batch, as the beamer does per configuration.
#
# ROTATIONS holds each beamer board's edge pieces to the sides its "r<N>b..."
# row dealt them (and lets CORNER_SEEDS seed alive top-corner blocks). Without
# it any edge piece may take any open border cell.
#
# HOLES lifts cells from every board first (a 0/1 mask file, top:K, box:...);
# STOP_ROW then rebuilds the open rows up to it with zero breaks and extends
# the board column by column above it, as the beamer's --backtrack_row does,
# before the dives. PIN_CLUE places the centre clue when HOLES opens its cell.
# What the stages do: PROJECT_E555.md, "Finishing boards"
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_partial_row12.csv     # any board CSV; open cells get dived
OUT=dived.csv
ROTATIONS=              # Stage A rotations of the beamer run; empty = free edges
END_DIVE=10000          # dives per board
END_POLISH=2000         # kick-and-polish rounds; -1 = no polish
EMIT_SCORE=0            # write boards whose best finish is >= this
CORNER_SEEDS=0          # seeded copies per board (needs ROTATIONS); 0 = off
HOLES=                  # cells to lift first: mask file, top:K, box:R0-R1,C0-C1; empty = none
STOP_ROW=               # zero-break rebuild up to this row, then columns; empty = off
PIN_CLUE=               # centre clue frame 1..4 (PROJECT_E555.md 9.2); empty = off
THREADS=8
RNG_SEED=1
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
[ -x bin/E555_diver ] || make diver

ROT_ARG=(); [ -n "$ROTATIONS" ] && ROT_ARG=(--rotations "$ROTATIONS" --corner_seeds "$CORNER_SEEDS")
POLISH_ARG=(); [ "$END_POLISH" -ge 0 ] && POLISH_ARG=(--end_polish "$END_POLISH")
PREP_ARG=()
[ -n "$HOLES" ]    && PREP_ARG+=(--holes "$HOLES")
[ -n "$STOP_ROW" ] && PREP_ARG+=(--stop_row "$STOP_ROW")
[ -n "$PIN_CLUE" ] && PREP_ARG+=(--pin_clue "$PIN_CLUE")

bin/E555_diver "$SEED" "$BOARDS" "$OUT" "${ROT_ARG[@]}" "${POLISH_ARG[@]}" "${PREP_ARG[@]}" \
    --end_dive "$END_DIVE" --emit_score "$EMIT_SCORE" \
    --threads "$THREADS" --rng_seed "$RNG_SEED" --print_cmd --verbose

echo
echo "=== best finished boards ==="
python3 tools/E555_rank.py "$OUT" --seed_file "$SEED" --top 3
echo
echo "Files written:"
sed 's/^/  /' "$OUT.outputs.txt"
echo
echo "A finished board is a complete 256-piece board with some broken edges: feed"
echo "it to the ender or the backtracker (with a holes mask) to repair them."
