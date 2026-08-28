#!/bin/bash
# 06_roundhouse_both_ways.sh -- does the spiral direction matter?
#
#   bash examples/06_roundhouse_both_ways.sh
#   bash examples/06_roundhouse_both_ways.sh BOARDS=final_out/beam_completions_finalized_12.csv
#   bash examples/06_roundhouse_both_ways.sh OUT_DIR=bw1 N_LINES=5 ROUNDS=3
#
# Each input board is run through TWO chains of two roundhouse passes:
#   chain a:  forward, then --reverse      (CCW then CW)
#   chain b:  --reverse, then forward      (CW then CCW)
# Two passes each way cover all four sides. With HOLD=1 the second pass keeps
# what the first left standing in the band instead of re-cutting it.
#
# Both chains start from the same board, so the score they reach is a fair
# comparison and the tally at the end answers the question in the title.
# A pass that emits nothing simply ends its chain, which is a real answer.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_partial_row12.csv
OUT_DIR=bothways_out
THREADS=8
ROUNDS=3                # 3 reaches the top band at ROTATE=-1
WIDTH=4                 # 2..5 chain length
ROTATE=-1               # both spirals start on the input's bottom
HOLD=1                  # 1 = pass 2 keeps what pass 1 left in the band
FIRST_LINE=0
N_LINES=1
MAX_WALL=120            # seconds per pass, 0 = unlimited
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
HOLD_ARG=(); [ "$HOLD"  = 1 ] && HOLD_ARG=(--hold_band)

# One roundhouse pass. $1 output dir, $2 board CSV, $3 which line of it, then
# any extra flags. Prints the path of the break-free board it produced, or
# nothing. The tool lists what it wrote, so there is no filename to guess and
# no `ls` to parse.
pass() {
    local dir="$1" src="$2" line="$3"; shift 3
    mkdir -p "$dir"
    bin/E555_roundhouse "$SEED" "$src" \
        --start_row "$line" --num_rows 1 \
        --rounds "$ROUNDS" --strip_width "$WIDTH" --rotate "$ROTATE" \
        --ties 1 --wall_time "$MAX_WALL" --threads "$THREADS" \
        --out_dir "$dir" --print_cmd "${CLUE_ARG[@]}" "$@" > "$dir/log" 2>&1
    # Break-free boards go to the miss0 file; with --breaks 0 that is the
    # only file the tool can write, so the manifest holds it or is empty.
    local out; out=$(head -1 "$dir/outputs.txt")
    [ -n "$out" ] || return 0

    # Tag the board with the pass that made it, so provenance survives when the
    # four passes are cat-ed into one file. awk on field 1 rather than a sed
    # regex: the id is a CSV field, and treating it as one cannot corrupt a row.
    awk -F, -v tag="$(basename "$dir")" 'BEGIN { OFS = "," }
         !/^ *[#%]/ { $1 = $1 "_" tag } { print }' "$out" > "$out.tagged"
    mv "$out.tagged" "$out"
    echo "$out"
}

# A board's score, or "-" when the chain stopped before producing one.
score() {
    [ -n "$1" ] && [ -s "$1" ] || { echo -; return; }
    python3 tools/E555_rank.py "$1" --seed_file "$SEED" --field score
}

echo "=== both ways: rounds=$ROUNDS width=$WIDTH rotate=$ROTATE hold=$HOLD ==="
echo "    $BOARDS, lines $FIRST_LINE..$((FIRST_LINE + N_LINES - 1))"
echo

wins_a=0; wins_b=0; ties=0
for ((i = FIRST_LINE; i < FIRST_LINE + N_LINES; i++)); do
    cfg="$OUT_DIR/cfg$i"; rm -rf "$cfg"; mkdir -p "$cfg"

    # Chain a: forward, then reverse.   Chain b: reverse, then forward.
    # Pass 2 reads pass 1's single-board output, so its line index is 0.
    a1=$(pass "$cfg/a1" "$BOARDS" "$i")
    a2=""; [ -n "$a1" ] && a2=$(pass "$cfg/a2" "$a1" 0 --reverse "${HOLD_ARG[@]}")
    b1=$(pass "$cfg/b1" "$BOARDS" "$i" --reverse)
    b2=""; [ -n "$b1" ] && b2=$(pass "$cfg/b2" "$b1" 0 "${HOLD_ARG[@]}")

    # The chain's result is its last surviving pass.
    A=$(score "${a2:-$a1}"); B=$(score "${b2:-$b1}")
    na=$A; [ "$na" = - ] && na=0        # a chain that produced nothing scores 0
    nb=$B; [ "$nb" = - ] && nb=0
    if   [ "$na" -gt "$nb" ]; then verdict="CCW->CW"; wins_a=$((wins_a + 1))
    elif [ "$nb" -gt "$na" ]; then verdict="CW->CCW"; wins_b=$((wins_b + 1))
    else                          verdict="tie";     ties=$((ties + 1)); fi

    printf "line %-3d  a=%-4s b=%-4s  -> %s\n" "$i" "$A" "$B" "$verdict" \
        | tee -a "$OUT_DIR/summary.txt"
done

echo
echo "does the order matter?  CCW->CW $wins_a   CW->CCW $wins_b   tie $ties"
echo "boards and per-pass logs under $OUT_DIR/cfg<N>/"
echo "  cat $OUT_DIR/cfg*/[ab][12]/roundhouse_*_miss0.csv > all.csv"
echo "  python3 tools/E555_rank.py all.csv --seed_file $SEED --top 10"
