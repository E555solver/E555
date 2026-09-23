#!/bin/bash
# run_datadriven.sh -- the two phases in one command.
#
#   bash tests/datadriven/run_datadriven.sh
#   bash tests/datadriven/run_datadriven.sh THREADS=16 SEARCH_STOP_ROW=12
#   bash tests/datadriven/run_datadriven.sh LEARN=0 \
#        TABLE=tests/datadriven/example_run/table_rnd_s9_row2.txt   # reuse a table
#
# The learning phase measures where each piece sits and writes a table; the
# search phase reads it and steers by it. Learning writes no boards; the search
# writes the usual completions CSV, listed in $OUT_DIR/outputs.txt. LEARN=0
# skips straight to the search with an existing table, which must have been
# learned on the same border (checked by the binary).
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/../.." && pwd)   # E555 checkout
SEED=data/seed_Edge5.txt                    # paths below are relative to REPO
ROTATIONS=tests/datadriven/borders_stageAx6.csv
BORDER_ROW=4            # which rotations row to use, for both phases (4 = r16178,
                        # the border of example_run/table_rnd_s9_row2.txt)
OUT_DIR=tests/datadriven/runs/pipeline
TABLE=tests/datadriven/runs/pipeline/table.txt
DB_FILE=                # chain DB cache file (~6 GB); empty = build in memory
THREADS=4
RNG_SEED=12345          # determinism is this AND --threads, never this alone

LEARN=1                 # 0 = keep the table at $TABLE and only search
LEARN_BOTTOMS=50000     # bottom samples per pass. A pass whose pool is smaller
                        # goes round it again with fresh random streams, so every
                        # pass takes the same number of samples.
LEARN_COLUMNS=5         # left columns per bottom. Keep this small: columns
                        # sharing a bottom are correlated, so they add votes
                        # faster than they add information.
LEARN_STOP_ROW=10       # only boards reaching this row are counted; 9 or more
LEARN_BEAM=20000
LEARN_WALL=0            # seconds for the learning phase, 0 = unlimited

FREQ_MODEL=segment      # beam statistic: segment (pooled A/B/C) or cell
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
DB=()
[ -n "$DB_FILE" ] && DB=(--db_file "$DB_FILE")

if [ "$LEARN" = 1 ]; then
    echo "=== phase 1/2: learning the table -> $TABLE ==="
    $BIN "$SEED" "$ROTATIONS" \
        --start_row "$BORDER_ROW" --num_rows 1 \
        --top_bottoms "$LEARN_BOTTOMS" --top_columns "$LEARN_COLUMNS" \
        --stop_row "$LEARN_STOP_ROW" --beam_width "$LEARN_BEAM" \
        --threads "$THREADS" --rng_seed "$RNG_SEED" \
        --wall_time "$LEARN_WALL" \
        $CLUES ${DB[@]+"${DB[@]}"} \
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
    $CLUES ${DB[@]+"${DB[@]}"} \
    --table "$TABLE" --freq_model "$FREQ_MODEL" \
    --out_dir "$OUT_DIR" --print_cmd

echo
echo "table   : $TABLE   (python3 tests/datadriven/freq_view.py $TABLE)"
echo "boards  : listed in $OUT_DIR/outputs.txt"
