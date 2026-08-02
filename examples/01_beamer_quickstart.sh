#!/bin/bash
##SBATCH --job-name=E555_quickstart
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=10G --time=00:30:00
##SBATCH --output=logs/quickstart_%j.out
#
# =============================================================================
# 01_beamer_quickstart.sh -- start here. One command, one board, ~5 minutes.
# =============================================================================
# WHAT IT DOES
#   Runs the Stage B beamer. By default with --random_edges: it samples border
#   arrangements straight from the seed, so nothing has to be prepared first.
#   It grows boards row by row and writes every board that survives to STOP_ROW.
#
#   ANNEAL=1 runs Stage A first instead. The annealer searches for GOOD borders
#   -- ones whose sides have many ways to be continued -- and the beamer then
#   grows boards from those rather than from random ones. Slower to start, much
#   better material. Everything below the settings block is shared; the only
#   difference is where the borders come from.
#
#       bash examples/01_beamer_quickstart.sh              # random borders
#       ANNEAL=1 bash examples/01_beamer_quickstart.sh     # annealed borders
#
# WHAT TO EXPECT
#   The first run builds the 6.4 GB chain database in memory (a few quiet
#   minutes, ~8 GB RAM). Then rows scroll past. Some border configurations go
#   extinct with no board emitted -- that is normal and is the search working,
#   not failing.
#
#   The beam is reproducible only at --threads 1; runs at higher thread counts
#   will vary in row count and boards emitted from run to run.
#
# THE OUTPUT FEEDS EVERYTHING ELSE
#   The result is a canonical board CSV. Every other script in this folder
#   accepts it as input. Try 02 next.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- settings: edit here, or override from the environment ------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
OUT_DIR="${OUT_DIR:-beam_out}"
BEAM_WIDTH="${BEAM_WIDTH:-50000}"   # boards kept per row. Production: 262144
STOP_ROW="${STOP_ROW:-10}"          # last row filled. Higher = harder = slower
MAX_WALL="${MAX_WALL:-0}"           # seconds for the whole run, 0 = unlimited

ANNEAL="${ANNEAL:-0}"               # 1 = anneal the borders first (Stage A)
N_BOTTOMS="${N_BOTTOMS:-2}"         # bottom rows tried per border
N_COLUMNS="${N_COLUMNS:-2}"         # left columns tried per bottom row

# ANNEAL=1 only:
ROTATIONS="${ROTATIONS:-rotations.csv}"   # Stage A writes here, Stage B reads it
RESTARTS="${RESTARTS:-4}"                 # independent annealing runs = borders
STEPS="${STEPS:-60000}"                   # annealing steps per restart
TARGET="${TARGET:-250}"                   # per-side trail target scale
THREADS="${THREADS:-8}"                   # 0 = all cores
# -----------------------------------------------------------------------------

[ -x "$REPO/bin/E555_beamer" ] || make -C "$REPO" beamer

# The beamer call is the same either way apart from where the borders come
# from, so build the differing part as an array rather than writing the whole
# invocation out twice.
if [ "$ANNEAL" = 1 ]; then
    echo "=== Stage A: annealing $RESTARTS borders ==="
    python3 -u "$REPO/src/A_border/E555_edge_annealer.py" "$SEED" \
        --restarts "$RESTARTS" \
        --steps "$STEPS" \
        --target_scale "$TARGET" \
        --threads "$THREADS" \
        --w-bottom 60 --w-left 20 --w-right 20 --w-top 1 \
        --out "$ROTATIONS" \
        --verbose
    echo
    echo "=== Stage B: beaming from those borders ==="
    BORDERS=("$ROTATIONS" --border_row 0 --border_row_N "$RESTARTS"
             --top_bottoms "$N_BOTTOMS" --top_columns "$N_COLUMNS")
    RESULT="$OUT_DIR/beam_completions_0_$STOP_ROW.csv"
else
    BORDERS=(--random_edges --border_row_N "$N_BOTTOMS" --top_columns "$N_COLUMNS"
             --soft_center_139)
    RESULT="$OUT_DIR/beam_completions_random_$STOP_ROW.csv"
fi

"$REPO/bin/E555_beamer" "$SEED" "${BORDERS[@]}" \
    --beam_width "$BEAM_WIDTH" \
    --stop_row "$STOP_ROW" \
    --lambda_Mahalanobis 8 \
    --max_wall_sec "$MAX_WALL" \
    --seed 1 \
    --out_dir "$OUT_DIR" \
    --verbose

# Repeated runs: add  --db_file /some/path.db  to cache the chain database on
# disk (~6.5 GB) so later runs start in seconds instead of minutes.

echo
if [ -s "$RESULT" ]; then
    echo "Boards written to $RESULT. Look at the best one:"
    echo "  python3 $REPO/tools/E555_viewer.py $RESULT --seed $SEED"
    echo "  python3 $REPO/tools/E555_rank.py   $RESULT --seed $SEED --top 5"
    echo
    echo "Then push it further:"
    echo "  PARTIALS=$RESULT bash $REPO/examples/02_finalizer_regrow.sh"
    if [ "$ANNEAL" = 1 ]; then
        echo
        echo "Keep $ROTATIONS: the finalizer reads it too, and will then re-impose"
        echo "this side assignment instead of freeing all 56 edges to every side:"
        echo "  PARTIALS=$RESULT ROTATIONS=$ROTATIONS bash $REPO/examples/02_finalizer_regrow.sh"
    fi
else
    echo "No board survived to row $STOP_ROW. That is a normal outcome for a"
    echo "short run: raise BEAM_WIDTH, raise N_BOTTOMS, or lower STOP_ROW."
fi
