#!/bin/bash
#SBATCH --job-name=E555dd
#SBATCH --output=E555dd_%j.out
#SBATCH --cpus-per-task=24
#SBATCH --hint=nomultithread
#SBATCH --mem=25G
#SBATCH --time=24:00:00

# slurm_datadriven.sh -- learn a positional table from a de-biased beam, then
# search with it. Submit from the repository root:
#
#   sbatch tests/datadriven/example_run/slurm_datadriven.sh
#   sbatch tests/datadriven/example_run/slurm_datadriven.sh LEARN=0
#   sbatch --array=0-5 tests/datadriven/example_run/slurm_datadriven.sh   # one border row per task
#
# The defaults match the settings recorded in table_rnd_s9_row2.txt (border
# r16178 = borders_stageAx6.csv row 4). Runs are deterministic for a given
# RNG_SEED together with THREADS.
#
# Learning: J and Mahalanobis off, survivors chosen at random (--frac_rand 1),
# bottoms and columns sampled almost uniformly. Legal-chain generation, the
# one-row death gate, clue pins and frontier dedup stay active.
# Search: learned table + database fan-out, J and Mahalanobis off, border
# ranking greedy on the learned exact-location weights.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=${SLURM_SUBMIT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)}
SEED=data/seed_Edge5.txt
ROTATIONS=tests/datadriven/borders_stageAx6.csv
BORDER_ROW=${SLURM_ARRAY_TASK_ID:-4}
OUT_DIR=                # default: tests/datadriven/runs/rnd_s9_row$BORDER_ROW
TABLE=                  # default: $OUT_DIR/table_rnd_s9_row$BORDER_ROW.txt
DB_FILE=                # clued chain DB cache; empty = build in memory (~6 GB)
THREADS=${SLURM_CPUS_PER_TASK:-24}
RNG_SEED=20260920

LEARN=1
LEARN_BOTTOMS=10000     # distinct bottoms are independent evidence; columns
LEARN_COLUMNS=10        # sharing a bottom are correlated
LEARN_STOP_ROW=9
LEARN_BEAM=250000
LEARN_POOL_FACTOR=16
LEARN_WALL=0            # seconds, split evenly over the four passes; 0 = unlimited

FREQ_MODEL=segment      # segment (pooled A/B/C) or cell
SEARCH_BOTTOMS=500
SEARCH_COLUMNS=10
SEARCH_STOP_ROW=12
SEARCH_BEAM=500000
SEARCH_POOL_FACTOR=8
SEARCH_WALL=0
MAX_EMITTED=0
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
OUT_DIR=${OUT_DIR:-tests/datadriven/runs/rnd_s9_row$BORDER_ROW}
TABLE=${TABLE:-$OUT_DIR/table_rnd_s9_row$BORDER_ROW.txt}
cd "$REPO"
[ -f "$SEED" ] || { echo "no $SEED under $REPO: submit from the repository root" >&2; exit 1; }

BIN=tests/datadriven/bin/E555_beamer_datadriven
[ -x "$BIN" ] || { echo "build it first: (cd tests/datadriven && make)" >&2; exit 1; }
mkdir -p "$(dirname "$TABLE")" "$OUT_DIR"

CLUES=(--clue_center --clue_corners --pin_clue 1)
DB=()
[ -n "$DB_FILE" ] && DB=(--db_file "$DB_FILE")

if [ "$LEARN" = 1 ]; then
    echo "=== phase 1/2: randomized learning -> $TABLE ==="
    date
    "$BIN" "$SEED" "$ROTATIONS" \
        --start_row "$BORDER_ROW" --num_rows 1 \
        --top_bottoms "$LEARN_BOTTOMS" --top_columns "$LEARN_COLUMNS" \
        --stop_row "$LEARN_STOP_ROW" --beam_width "$LEARN_BEAM" \
        --threads "$THREADS" --rng_seed "$RNG_SEED" \
        --wall_time "$LEARN_WALL" \
        --learn "$TABLE" --out_dir "$OUT_DIR/learn" --print_cmd \
        "${CLUES[@]}" ${DB[@]+"${DB[@]}"} \
        --lambda_J 0 --lambda_Mahalanobis 0 \
        --frac_rand 1.0 --parent_cap 4 --pool_factor "$LEARN_POOL_FACTOR" \
        --bc_window 1,1 \
        --tau_bottoms 1000000 --tau_columns 1000000 \
        --beam_expand 1 --beam_expand_row 13
else
    echo "=== phase 1/2: skipped, reusing $TABLE ==="
    [ -f "$TABLE" ] || { echo "no table at $TABLE" >&2; exit 1; }
fi

echo "=== phase 2/2: table + fan-out search -> $OUT_DIR ==="
date
"$BIN" "$SEED" "$ROTATIONS" \
    --start_row "$BORDER_ROW" --num_rows 1 \
    --top_bottoms "$SEARCH_BOTTOMS" --top_columns "$SEARCH_COLUMNS" \
    --stop_row "$SEARCH_STOP_ROW" --beam_width "$SEARCH_BEAM" \
    --threads "$THREADS" --rng_seed "$RNG_SEED" \
    --wall_time "$SEARCH_WALL" --max_emitted "$MAX_EMITTED" \
    --table "$TABLE" --freq_model "$FREQ_MODEL" \
    --out_dir "$OUT_DIR" --print_cmd \
    --incomplete_top "${CLUES[@]}" ${DB[@]+"${DB[@]}"} \
    --lambda_J 0 --lambda_Mahalanobis 0 \
    --frac_rand 0.10 --parent_cap 4 --pool_factor "$SEARCH_POOL_FACTOR" \
    --bc_window 4,4 \
    --tau_bottoms 0 --tau_columns 0 \
    --beam_expand 2 --beam_expand_row 5

echo
echo "table   : $TABLE   (python3 tests/datadriven/freq_view.py $TABLE --text)"
echo "boards  : listed in $OUT_DIR/outputs.txt"
date
