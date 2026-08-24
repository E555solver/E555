#!/bin/bash
##SBATCH --job-name=E555_bothways
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=4G --time=12:00:00
##SBATCH --output=logs/bothways_%j.out
#
# =============================================================================
# 06_roundhouse_both_ways.sh -- spiral one way, then the other, on one board
# =============================================================================
# WHAT IT DOES
#   Runs the roundhouse twice over each input board, feeding the first pass's
#   output to the second, and does it in both orders:
#
#       chain A    a1  CCW on the input    a2  CW  on a1's board
#       chain B    b1  CW  on the input    b2  CCW on b1's board
#
#   Every pass uses the same ROUNDS, WIDTH and ROTATE. Only the handedness
#   changes -- CCW is plain, CW is --reverse -- and at ROTATE=-1 that decides
#   which side each pass takes second:
#
#       CCW    bottom -> RIGHT -> top      core hugs left
#       CW     bottom -> LEFT  -> top      core hugs right
#
#   So the two passes of a chain cover all four sides between them, and the
#   script reports whether the ORDER changes what you end up with.
#
# WHY ROUNDS=3
#   At ROTATE=-1 only the third round frees the top band. A board whose top rows
#   are still empty needs that band freed, or the roundhouse refuses the cut.
#   Aim ROTATE elsewhere and a smaller ROUNDS will do -- see example 03.
#
# WHY WIDTH=4
#   It keeps a 96-piece core and exhausts in seconds, so pass 1 finishes and
#   pass 2 has a proved board to work from. WIDTH=5 keeps only 66 and ends on a
#   budget; WIDTH=3 never reaches row 12, leaving the empty top inside the core.
#
# HOLD -- the reason the two passes are worth chaining
#   Both passes rebuild the same top band, and they enter it from OPPOSITE ends.
#   Pass 1 stops having laid a few complete chain levels at the far end of pass
#   2's strip. With HOLD=1 pass 2 keeps them (--hold_band) and fills up to meet
#   them, so a chain only ever adds to what it has already proved. With HOLD=0
#   those pieces go back in the pool and pass 2 rebuilds the band from nothing:
#   less safe, but unconstrained. Both are worth running.
#
# READING THE SCORE
#   Roundhouse output is break-free, so a board's score is 480 minus the
#   junctions its EMPTY cells leave open -- it falls with every empty cell,
#   never with a mismatch. "in" is the input board's own score, on the same
#   scale. A pass showing "-" emitted nothing; its log says whether the cut was
#   refused or the oracle refuted every start.
#
#   Each board keeps the id it came in with -- the roundhouse appends its own
#   suffix, and this script adds the pass name -- so a merged CSV says where
#   every row came from. Expect duplicates across passes: two passes reaching
#   the same board is a real result, not a bug.
# =============================================================================
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# ---- settings ---------------------------------------------------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
PARTIALS="${PARTIALS:-$REPO/data/board_partial_row12.csv}"
OUT_DIR="${OUT_DIR:-bothways_out}"
ROUNDS="${ROUNDS:-3}"             # 3 to reach the top band at ROTATE=-1
WIDTH="${WIDTH:-4}"               # 2..5 (chain length)
ROTATE="${ROTATE:--1}"            # both spirals start on the input's bottom
HOLD="${HOLD:-1}"                 # 1 = pass 2 keeps what pass 1 left in the band
FIRST_LINE="${FIRST_LINE:-0}"
N_LINES="${N_LINES:-1}"
MAX_WALL="${MAX_WALL:-120}"       # seconds per pass, 0 = unlimited
THREADS="${THREADS:-0}"           # 0 = all cores
CLUES="${CLUES:-0}"               # 1 = hold the published Eternity II clue pieces
# -----------------------------------------------------------------------------

[ -x "$REPO/bin/E555_roundhouse" ] || make -C "$REPO" roundhouse

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
HOLD_ARG=(); [ "$HOLD"  = 1 ] && HOLD_ARG=(--hold_band)

# The break-free board a pass produced, if it produced one.
found() { ls "$1"/roundhouse_*_miss0.csv 2>/dev/null | head -1 || true; }

# A board's score, "-" when there is no board.
score() {
    [ -n "${1:-}" ] && [ -s "${1:-}" ] || { echo -; return; }
    python3 "$REPO/tools/E555_rank.py" "$1" --seed "$SEED" --field score 2>/dev/null || echo -
}

# The best of the scores given, ignoring passes that emitted nothing.
best() { printf '%s\n' "$@" | grep -E '^[0-9]+$' | sort -rn | head -1 || true; }

# One roundhouse pass: $1 output dir, $2 board to read, then any extra flags.
# Prints the score it reached. A missing input is not an error -- the chain
# simply stops there and says so.
pass() {
    local dir="$1" src="${2:-}"; shift 2
    mkdir -p "$dir"
    [ -n "$src" ] && [ -s "$src" ] || { echo -; return; }
    "$REPO/bin/E555_roundhouse" "$SEED" "$src" \
        --border_row 0 --border_row_N 1 \
        --rounds "$ROUNDS" --strip_width "$WIDTH" --rotate "$ROTATE" \
        --ties 1 --max_wall_sec "$MAX_WALL" --threads "$THREADS" \
        --out_dir "$dir" "${CLUE_ARG[@]}" "$@" > "$dir/log" 2>&1 || true

    # Label the board with the pass that made it. The roundhouse already keeps
    # the input's config_id and appends its own suffix, so this only adds the
    # pass name: a1 and b1 read the same board and are otherwise identical in
    # the id column once the two are merged.
    local out; out=$(found "$dir")
    [ -n "$out" ] && sed -i "1s|^\([^,]*\)|\1_$(basename "$dir")|" "$out"
    score "$out"
}

mkdir -p "$OUT_DIR"
echo "=== both ways: rounds=$ROUNDS width=$WIDTH rotate=$ROTATE hold=$HOLD clues=$CLUES ==="
echo "    $PARTIALS, lines $FIRST_LINE..$((FIRST_LINE + N_LINES - 1))"
echo

wins_a=0 wins_b=0 ties=0
for ((i = FIRST_LINE; i < FIRST_LINE + N_LINES; i++)); do
    cfg="$OUT_DIR/cfg$i"; rm -rf "$cfg"; mkdir -p "$cfg"

    # One board per config, so the baseline and both chains are about THIS line.
    grep -vE '^[[:space:]]*[#%]' "$PARTIALS" | sed -n "$((i + 1))p" > "$cfg/in.csv"
    [ -s "$cfg/in.csv" ] || { echo "cfg $i: no such data line"; break; }

    a1=$(pass "$cfg/a1" "$cfg/in.csv")
    a2=$(pass "$cfg/a2" "$(found "$cfg/a1")" --reverse "${HOLD_ARG[@]}")
    b1=$(pass "$cfg/b1" "$cfg/in.csv" --reverse)
    b2=$(pass "$cfg/b2" "$(found "$cfg/b1")" "${HOLD_ARG[@]}")

    A=$(best "$a1" "$a2"); B=$(best "$b1" "$b2")
    if   [ "${A:-0}" -gt "${B:-0}" ]; then verdict="CCW->CW"; wins_a=$((wins_a+1))
    elif [ "${B:-0}" -gt "${A:-0}" ]; then verdict="CW->CCW"; wins_b=$((wins_b+1))
    else verdict="tie"; ties=$((ties+1)); fi

    printf "cfg %-3d  in=%-4s  a1=%-4s a2=%-4s b1=%-4s b2=%-4s  chains %s/%s -> %s\n" \
           "$i" "$(score "$cfg/in.csv")" "$a1" "$a2" "$b1" "$b2" \
           "${A:--}" "${B:--}" "$verdict" | tee -a "$OUT_DIR/summary.txt"
done

echo
echo "does the order matter?  CCW->CW $wins_a   CW->CCW $wins_b   tie $ties"
echo "boards and per-pass logs under $OUT_DIR/cfg<N>/"
echo "  cat $OUT_DIR/cfg*/[ab][12]/roundhouse_*_miss0.csv > all.csv"
echo "  python3 $REPO/tools/E555_rank.py all.csv --seed $SEED --top 10"
