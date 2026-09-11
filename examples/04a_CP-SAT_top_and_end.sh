#!/bin/bash
# 04a_CP-SAT_top_and_end.sh -- scout widely, promote diverse roots, then close.
#
# The ordinary controls are near the top.  The adaptive ender chooses its own
# focused and broad neighborhoods; do not pass a single attempt_time unless you
# are running a deliberate timing experiment.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SEED=data/seed_Edge5.txt
BOARDS=data/board_example_462.csv
OUT_DIR=stage_c_funnel
FIRST_LINE=0
N_LINES=0                         # 0 = every remaining board

SIDE=T
WORK_ROWS=5
THREADS=8
RNG_SEED=5552026
CLUES=0

# Topper breadth -> promotion -> polish.
SCOUT_TIME=30
SCOUT_STALL=12
PROMOTE_PERCENT=20
PROMOTE_POOL_FACTOR=5
MAX_AGREE=0.97
POLISH_TIME=180
POLISH_STALL=45

# The promoted set is small enough for a substantial adaptive close.
ENDER_PROFILE=deep
ENDER_BOARD_TIME=600             # true total per promoted board

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
TOPPER=src/C_tail/E555_topper.py
ENDER=src/C_tail/E555_ender.py
RANK=tools/E555_rank.py
for f in "$SEED" "$BOARDS" "$TOPPER" "$ENDER" "$RANK"; do
    [ -f "$f" ] || { echo "Missing $f" >&2; exit 1; }
done

CLUE_ARG=()
[ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)

SCOUT="$OUT_DIR/1_scout.csv"
PROMOTED="$OUT_DIR/2_promoted.csv"
POLISHED="$OUT_DIR/3_polished.csv"
FINAL="$OUT_DIR/4_closed.csv"
rm -f "$SCOUT" "$PROMOTED" "$POLISHED" "$FINAL"

printf '\n=== baseline ===\n'
python3 "$RANK" "$BOARDS" --seed_file "$SEED" --top 10

printf '\n=== topper scout: one inexpensive look at every root ===\n'
python3 "$TOPPER" "$SEED" "$BOARDS" "$SCOUT" "${CLUE_ARG[@]}" \
    --start_row "$FIRST_LINE" --num_rows "$N_LINES" \
    --side "$SIDE" --band_depth "$WORK_ROWS" --top 1 \
    --threads "$THREADS" --time_limit "$SCOUT_TIME" \
    --stall_time "$SCOUT_STALL" --rng_seed "$RNG_SEED" \
    --symmetry_level 2

N=$(python3 "$RANK" "$SCOUT" --count)
[ "$N" -gt 0 ] || { echo "Topper emitted no boards." >&2; exit 1; }
PROMOTE=$(( (N * PROMOTE_PERCENT + 99) / 100 ))
[ "$PROMOTE" -ge 1 ] || PROMOTE=1
POOL=$(( PROMOTE * PROMOTE_POOL_FACTOR ))
[ "$POOL" -le "$N" ] || POOL=$N
printf '[funnel] promoting %d of %d boards from a quality pool of %d\n' \
       "$PROMOTE" "$N" "$POOL"
python3 "$RANK" "$SCOUT" --seed_file "$SEED" \
    --sort breaks,break_rows,corner_d --top "$POOL" \
    --max_agree "$MAX_AGREE" --diverse "$PROMOTE" \
    --diversity_box 0:11,0:15 --out "$PROMOTED" --rescore

printf '\n=== topper polish: promoted roots only ===\n'
python3 "$TOPPER" "$SEED" "$PROMOTED" "$POLISHED" "${CLUE_ARG[@]}" \
    --side "$SIDE" --band_depth "$WORK_ROWS" --top 1 \
    --threads "$THREADS" --time_limit "$POLISH_TIME" \
    --stall_time "$POLISH_STALL" --rng_seed "$((RNG_SEED + 10000))" \
    --symmetry_level 2

printf '\n=== adaptive ender: focused exchanges, then broad escalation ===\n'
python3 "$ENDER" "$SEED" "$POLISHED" "$FINAL" "${CLUE_ARG[@]}" \
    --profile "$ENDER_PROFILE" --search_mode improve \
    --board_time_limit "$ENDER_BOARD_TIME" --threads "$THREADS" \
    --rng_seed "$((RNG_SEED + 20000))"

printf '\n=== final ranking ===\n'
python3 "$RANK" "$FINAL" --seed_file "$SEED" \
    --sort breaks,break_rows,corner_d --top 20
printf '\nBoards -> %s\n' "$FINAL"
