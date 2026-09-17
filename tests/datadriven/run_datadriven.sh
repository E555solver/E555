#!/bin/bash
# run_datadriven.sh -- the two phases in one command.
#
#   bash tests/datadriven/run_datadriven.sh
#   bash tests/datadriven/run_datadriven.sh THREADS=16 SEARCH_STOP_ROW=12
#   bash tests/datadriven/run_datadriven.sh TABLE=runs/mine.txt LEARN=0   # reuse a table
#
# YES, IT IS TWO RUNS. The learning phase measures where each piece sits and
# writes a table; the search phase reads that table and steers by it. They are
# separate binaries invocations on purpose, so every flag keeps the meaning it
# already has in whichever phase it is passed to. This script just runs them in
# order. Learning writes no boards, so it is cheap on disk; the search writes
# the usual completions CSV, listed in $OUT_DIR/outputs.txt.
#
# LEARN=0 skips straight to the search, which is what you want when tuning
# ALPHA: the table holds raw counts, so re-reading it at a different ALPHA costs
# milliseconds and learning again costs hours.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/../.." && pwd)   # E555 checkout
SEED=data/seed_Edge5.txt                    # paths below are relative to REPO
ROTATIONS=data/borders_annealed_fix12.csv
BORDER_ROW=1            # which rotations row to use, for both phases
OUT_DIR=tests/datadriven/runs/pipeline
TABLE=tests/datadriven/runs/pipeline/table.txt
DB_FILE=tests/datadriven/runs/chain_clued.db   # 5.6 GB cache; empty = in memory
THREADS=4
RNG_SEED=12345          # determinism is this AND --threads, never this alone

LEARN=1                 # 0 = keep the table at $TABLE and only search
LEARN_BOTTOMS=2000      # bottom rows per pass. The table's sample size.
LEARN_COLUMNS=5         # left columns per bottom
LEARN_STOP_ROW=10       # how high learning grows. Higher = better top-row
                        # coverage, fewer surviving configurations.
LEARN_BEAM=20000
LEARN_WALL=0            # seconds for the learning phase, 0 = unlimited

ALPHA=20                # shrinkage toward the piece-by-row prior. The one knob.
SEARCH_BOTTOMS=40
SEARCH_COLUMNS=5
SEARCH_STOP_ROW=12
SEARCH_BEAM=200000
SEARCH_WALL=0           # seconds for the search phase, 0 = unlimited
MAX_EMITTED=0           # stop after this many boards, 0 = unlimited
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
cd "$REPO"

BIN=tests/datadriven/bin/E555_beamer_datadriven
[ -x "$BIN" ] || { echo "build it first: (cd tests/datadriven && make)" >&2; exit 1; }
mkdir -p "$(dirname "$TABLE")" "$OUT_DIR"

CLUES="--clue_center --clue_corners --pin_clue 1"

if [ "$LEARN" = 1 ]; then
    echo "=== phase 1/2: learning the table -> $TABLE ==="
    $BIN "$SEED" "$ROTATIONS" \
        --start_row "$BORDER_ROW" --num_rows 1 \
        --top_bottoms "$LEARN_BOTTOMS" --top_columns "$LEARN_COLUMNS" \
        --stop_row "$LEARN_STOP_ROW" --beam_width "$LEARN_BEAM" \
        --threads "$THREADS" --rng_seed "$RNG_SEED" \
        --wall_time "$LEARN_WALL" \
        $CLUES --db_file "$DB_FILE" \
        --learn "$TABLE" --out_dir "$OUT_DIR/learn" --print_cmd
else
    echo "=== phase 1/2: skipped, reusing $TABLE ==="
    [ -f "$TABLE" ] || { echo "no table at $TABLE" >&2; exit 1; }
fi

echo "=== phase 2/2: searching with it -> $OUT_DIR ==="
$BIN "$SEED" "$ROTATIONS" \
    --start_row "$BORDER_ROW" --num_rows 1 \
    --top_bottoms "$SEARCH_BOTTOMS" --top_columns "$SEARCH_COLUMNS" \
    --stop_row "$SEARCH_STOP_ROW" --beam_width "$SEARCH_BEAM" \
    --threads "$THREADS" --rng_seed "$RNG_SEED" \
    --wall_time "$SEARCH_WALL" --max_emitted "$MAX_EMITTED" \
    $CLUES --db_file "$DB_FILE" \
    --table "$TABLE" --freq_alpha "$ALPHA" \
    --out_dir "$OUT_DIR" --print_cmd

echo
echo "table   : $TABLE   (python3 tests/datadriven/freq_view.py $TABLE)"
echo "boards  : listed in $OUT_DIR/outputs.txt"
