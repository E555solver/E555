#!/bin/bash
# 04c_CP-SAT_ender_elite.sh -- repeated deep or super-deep attacks on a few elite roots.
#
# Examples:
#   bash examples/04c_CP-SAT_ender_elite.sh MODE=deep
#   bash examples/04c_CP-SAT_ender_elite.sh MODE=superdeep BOARDS=close/best.csv
#
# MODE=deep spends about one total hour per selected board.  MODE=superdeep
# spends about ten total hours per board.  The total is divided among independent
# passes with new random seeds; every pass starts from the best board retained by
# the previous pass.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SEED=data/seed_Edge5.txt
BOARDS=data/board_example_462.csv
OUT_DIR=ender_elite
MODE=deep                         # deep | superdeep

SELECT_ELITES=1                   # 0 = use every input row as supplied
ELITE_COUNT=5
CANDIDATE_POOL=100
MAX_AGREE=0.98
DIVERSITY_BOX=0:11,0:15

TOTAL_SECONDS=0                   # 0 = mode default: 3600 or 36000 per board
PASSES=0                          # 0 = mode default: 3 or 5
THREADS=0                         # 0 = mode default: 8 or 12 per process
JOBS=1                            # simultaneous elite boards; respect CPU count
RNG_SEED=5554001
CLUES=0
VERBOSE=0

for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done

case "$MODE" in
    deep)
        PROFILE=deep
        [ "$TOTAL_SECONDS" -gt 0 ] || TOTAL_SECONDS=3600
        [ "$PASSES" -gt 0 ] || PASSES=3
        [ "$THREADS" -gt 0 ] || THREADS=8
        ;;
    superdeep)
        PROFILE=superdeep
        [ "$TOTAL_SECONDS" -gt 0 ] || TOTAL_SECONDS=36000
        [ "$PASSES" -gt 0 ] || PASSES=5
        [ "$THREADS" -gt 0 ] || THREADS=12
        ;;
    *) echo "MODE must be deep or superdeep" >&2; exit 1 ;;
esac
[ "$PASSES" -ge 1 ] || { echo "PASSES must be positive" >&2; exit 1; }
[ "$JOBS" -ge 1 ] || { echo "JOBS must be positive" >&2; exit 1; }
BOARD_TIME=$(( (TOTAL_SECONDS + PASSES - 1) / PASSES ))

cd "$REPO"
[ -d bin ] && [ -d tools ] ||
    { echo "REPO=$REPO is not an E555 checkout -- set REPO at the top" >&2; exit 1; }
mkdir -p "$OUT_DIR"
ENDER=src/C_tail/E555_ender.py
RANK=tools/E555_rank.py
for f in "$SEED" "$BOARDS" "$ENDER" "$RANK"; do
    [ -f "$f" ] || { echo "Missing $f" >&2; exit 1; }
done

ROOTS="$OUT_DIR/00_elite_roots.csv"
if [ "$SELECT_ELITES" = 1 ]; then
    python3 "$RANK" "$BOARDS" --seed_file "$SEED" \
        --sort breaks,break_rows,corner_d --top "$CANDIDATE_POOL" \
        --max_agree "$MAX_AGREE" --diverse "$ELITE_COUNT" \
        --diversity_box "$DIVERSITY_BOX" --out "$ROOTS" --rescore
else
    python3 "$RANK" "$BOARDS" --seed_file "$SEED" \
        --sort breaks,break_rows,corner_d --out "$ROOTS" --rescore --quiet
fi
N=$(python3 "$RANK" "$ROOTS" --count)
[ "$N" -gt 0 ] || { echo "No elite rows selected." >&2; exit 1; }
[ "$JOBS" -le "$N" ] || JOBS=$N

CLUE_ARG=()
[ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
VERBOSE_ARG=()
[ "$VERBOSE" = 1 ] && VERBOSE_ARG=(--verbose)

printf '[elite] mode=%s boards=%d passes=%d seconds/pass=%d total/board=%d workers/job=%d jobs=%d\n' \
       "$MODE" "$N" "$PASSES" "$BOARD_TIME" "$TOTAL_SECONDS" "$THREADS" "$JOBS"
awk -v n="$N" -v j="$JOBS" -v s="$TOTAL_SECONDS" \
    'BEGIN { printf "[elite] nominal ceiling: %.2f wall-hours\n", n*s/j/3600 }'

CUR="$ROOTS"
for ((pass=1; pass<=PASSES; pass++)); do
    printf '\n=== %s pass %d/%d ===\n' "$MODE" "$pass" "$PASSES"
    PASS_DIR="$OUT_DIR/pass_$(printf '%02d' "$pass")"
    mkdir -p "$PASS_DIR"
    pids=()
    for ((shard=0; shard<JOBS; shard++)); do
        OUT="$PASS_DIR/shard_${shard}.csv"
        LOG="$PASS_DIR/shard_${shard}.log"
        rm -f "$OUT" "$LOG"
        PYTHONUNBUFFERED=1 python3 "$ENDER" "$SEED" "$CUR" "$OUT" \
            --profile "$PROFILE" --search_mode improve \
            --board_time_limit "$BOARD_TIME" --threads "$THREADS" \
            --duplicate_policy rerun \
            --shard_count "$JOBS" --shard_index "$shard" \
            --rng_seed "$((RNG_SEED + 1000003 * pass + 10007 * shard))" \
            "${CLUE_ARG[@]}" "${VERBOSE_ARG[@]}" >"$LOG" 2>&1 &
        pids+=("$!")
        printf '[elite] pass %d shard %d -> %s\n' "$pass" "$shard" "$LOG"
    done

    cleanup() {
        for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    }
    trap cleanup INT TERM
    fail=0
    for pid in "${pids[@]}"; do
        if ! wait "$pid"; then fail=1; fi
    done
    trap - INT TERM
    [ "$fail" -eq 0 ] || { echo "A shard failed in pass $pass" >&2; exit 1; }

    MERGED="$PASS_DIR/merged.csv"
    RANKED="$PASS_DIR/ranked.csv"
    : > "$MERGED"
    for ((shard=0; shard<JOBS; shard++)); do
        cat "$PASS_DIR/shard_${shard}.csv" >> "$MERGED"
    done
    M=$(python3 "$RANK" "$MERGED" --count)
    [ "$M" -eq "$N" ] || {
        echo "Pass $pass expected $N rows, found $M." >&2; exit 1;
    }
    python3 "$RANK" "$MERGED" --seed_file "$SEED" \
        --sort breaks,break_rows,corner_d --out "$RANKED" --rescore --quiet
    python3 "$RANK" "$RANKED" --seed_file "$SEED" \
        --sort breaks,break_rows,corner_d --top "$ELITE_COUNT"
    CUR="$RANKED"
done

FINAL="$OUT_DIR/final_${MODE}.csv"
cp "$CUR" "$FINAL"
printf '\n=== final %s ranking ===\n' "$MODE"
python3 "$RANK" "$FINAL" --seed_file "$SEED" \
    --sort breaks,break_rows,corner_d --top "$ELITE_COUNT"
printf '\nBoards -> %s\nLogs   -> %s/pass_*/shard_*.log\n' "$FINAL" "$OUT_DIR"
