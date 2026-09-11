#!/bin/bash
# 04b_CP-SAT_ender_overnight.sh -- process a large full-board corpus overnight.
#
# Uses several independent processes with a modest CP-SAT worker portfolio in
# each.  This is usually a better breadth allocation than giving every board all
# machine threads.  The ender profile supplies the neighborhood schedule; the
# only routine budget here is the true per-board wall time.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SEED=data/seed_Edge5.txt
BOARDS=data/board_example_462.csv
OUT_DIR=ender_overnight
FIRST_LINE=0
N_LINES=0                         # 0 = every remaining board

BOARD_TIME=180                    # seconds per unique board, total over all calls
THREADS_PER_JOB=4
JOBS=0                            # 0 = auto from CPU count, capped at 8
MAX_JOBS=8
RNG_SEED=5553001
DEDUP=1                           # exact board dedup before spending solver time
RESUME=0                          # set 1 only when resuming the identical input
CLUES=0
VERBOSE=0

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
ENDER=src/C_tail/E555_ender.py
RANK=tools/E555_rank.py
for f in "$SEED" "$BOARDS" "$ENDER" "$RANK"; do
    [ -f "$f" ] || { echo "Missing $f" >&2; exit 1; }
done

SELECTED="$OUT_DIR/00_selected.csv"
ROOTS="$OUT_DIR/01_unique_roots.csv"
MERGED="$OUT_DIR/02_merged.csv"
FINAL="$OUT_DIR/03_ranked.csv"

# Materialize the requested input window once.  Shards then interleave this
# stable file, which makes --resume unambiguous.
python3 - "$BOARDS" "$SELECTED" "$FIRST_LINE" "$N_LINES" <<'PY'
import csv, sys
src, dst, first, count = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
stop = None if count == 0 else first + count
with open(src, newline="") as inp, open(dst, "w", newline="") as out:
    writer = csv.writer(out, lineterminator="\n")
    data = 0
    for row in csv.reader(inp):
        if not row or not row[0].strip() or row[0].lstrip().startswith(("#", "%")):
            continue
        if data >= first and (stop is None or data < stop):
            writer.writerow(row)
        data += 1
        if stop is not None and data >= stop:
            break
PY

if [ "$DEDUP" = 1 ]; then
    python3 "$RANK" "$SELECTED" --seed_file "$SEED" --unique \
        --out "$ROOTS" --rescore --quiet
else
    cp "$SELECTED" "$ROOTS"
fi
N=$(python3 "$RANK" "$ROOTS" --count)
[ "$N" -gt 0 ] || { echo "No board rows selected." >&2; exit 1; }

if [ "$JOBS" -eq 0 ]; then
    CORES=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    JOBS=$((CORES / THREADS_PER_JOB))
    [ "$JOBS" -ge 1 ] || JOBS=1
    [ "$JOBS" -le "$MAX_JOBS" ] || JOBS=$MAX_JOBS
fi
[ "$JOBS" -le "$N" ] || JOBS=$N
[ "$JOBS" -ge 1 ] || { echo "JOBS must be positive." >&2; exit 1; }

CLUE_ARG=()
[ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
VERBOSE_ARG=()
[ "$VERBOSE" = 1 ] && VERBOSE_ARG=(--verbose)
RESUME_ARG=()
[ "$RESUME" = 1 ] && RESUME_ARG=(--resume)

printf '[overnight] boards=%d jobs=%d workers/job=%d board_time=%ss\n' \
       "$N" "$JOBS" "$THREADS_PER_JOB" "$BOARD_TIME"
awk -v n="$N" -v j="$JOBS" -v s="$BOARD_TIME" \
    'BEGIN { printf "[overnight] nominal ceiling: %.2f wall-hours (early proofs/witnesses finish sooner)\n", n*s/j/3600 }'

pids=()
for ((shard=0; shard<JOBS; shard++)); do
    OUT="$OUT_DIR/shard_${shard}.csv"
    LOG="$OUT_DIR/shard_${shard}.log"
    if [ "$RESUME" != 1 ]; then
        rm -f "$OUT" "$LOG"
    fi
    PYTHONUNBUFFERED=1 python3 "$ENDER" "$SEED" "$ROOTS" "$OUT" \
        --profile overnight --search_mode improve \
        --board_time_limit "$BOARD_TIME" --threads "$THREADS_PER_JOB" \
        --shard_count "$JOBS" --shard_index "$shard" \
        --rng_seed "$((RNG_SEED + 100003 * shard))" \
        "${CLUE_ARG[@]}" "${VERBOSE_ARG[@]}" "${RESUME_ARG[@]}" \
        >"$LOG" 2>&1 &
    pids+=("$!")
    printf '[overnight] shard %d -> %s (log %s)\n' "$shard" "$OUT" "$LOG"
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
[ "$fail" -eq 0 ] || { echo "At least one shard failed; inspect shard_*.log" >&2; exit 1; }

: > "$MERGED"
for ((shard=0; shard<JOBS; shard++)); do
    OUT="$OUT_DIR/shard_${shard}.csv"
    [ -f "$OUT" ] && cat "$OUT" >> "$MERGED"
done
M=$(python3 "$RANK" "$MERGED" --count)
[ "$M" -eq "$N" ] || {
    echo "Expected $N output rows, found $M; inspect shard logs before trusting the merge." >&2
    exit 1
}

python3 "$RANK" "$MERGED" --seed_file "$SEED" \
    --sort breaks,break_rows,corner_d --out "$FINAL" --rescore --quiet
printf '\n=== overnight result ===\n'
python3 "$RANK" "$FINAL" --seed_file "$SEED" \
    --sort breaks,break_rows,corner_d --top 20
printf '\nBoards -> %s\nLogs   -> %s/shard_*.log\n' "$FINAL" "$OUT_DIR"
