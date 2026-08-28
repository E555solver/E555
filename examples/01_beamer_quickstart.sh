#!/bin/bash
# 01_beamer_quickstart.sh -- start here. Grow boards row by row from a border.
#
#   bash examples/01_beamer_quickstart.sh                    # random borders
#   bash examples/01_beamer_quickstart.sh ANNEAL=1           # searched borders
#   bash examples/01_beamer_quickstart.sh OUT_DIR=run1 THREADS=16
#
# Writes every board that survives to STOP_ROW; the files it wrote are listed
# in $OUT_DIR/outputs.txt. Needs ~8 GB RAM and a few quiet minutes on the first
# run, which builds the 6.4 GB chain database in memory.
#
# Border configurations that go extinct with no board are the search working,
# not failing. What every setting does: examples/README.md
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
OUT_DIR=beam_out
THREADS=8
BEAM_WIDTH=50000        # boards kept per row; the tool's own default is 250000
STOP_ROW=10             # last row filled. Higher = harder = slower
MAX_WALL=0              # seconds for the whole run, 0 = unlimited
DB_FILE=                # cache the 6.4 GB chain database here (~6.5 GB on
                        # disk) so later runs start in seconds; empty =
                        # build it in memory every time
RNG_SEED=1              # fixed so the run repeats; see the note in the README
CLUES=0                 # 1 = hold the published Eternity II clue pieces

ANNEAL=0                # 1 = search for good borders with Stage A first
BORDERS=4               # border rows handed to the beamer (ANNEAL=1)
STEPS=500000            # annealing steps per restart; 250000 is the floor
ROTATIONS=rotations.csv # Stage A writes here, Stage B reads it (ANNEAL=1)
N_BOTTOMS=2             # bottom rows tried per border
N_COLUMNS=2             # left columns tried per bottom row
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
[ -x bin/E555_beamer ] || make beamer

# The annealer needs at least 250000 steps to do its job. Below that it is not
# merely weaker -- it often fails to place a legal border at all: measured on
# the real seed, 8 restarts x 3000 steps found 2 feasible borders and 2 x 2000
# returned 1 on one run and 0 on the next, while every restart at 250000 steps
# succeeded (2/2, 4/4, 8/8). Warn rather than refuse: a deliberately tiny smoke
# run is a legitimate thing to ask for.
[ "$STEPS" -ge 250000 ] || echo "[warn] STEPS=$STEPS is below 250000, the point where"\
    " the annealer reliably finds a legal border at all. Expect few or none."

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)

if [ "$ANNEAL" = 1 ]; then
    # Anneal four times as many borders as we need, then keep the best quarter.
    # Stage A is cheap and the beamer's time is not, so it pays to be picky.
    echo "=== Stage A: annealing $((BORDERS * 4)) borders, keeping the best $BORDERS ==="
    rm -f "$ROTATIONS"          # the annealer APPENDS; start clean
    python3 src/A_border/E555_edge_annealer.py "$SEED" \
        --restarts $((BORDERS * 4)) --steps "$STEPS" --threads "$THREADS" \
        --w-bottom 0 --w-left 1 --w-right 3 --w-top 2 \
        --out "$ROTATIONS" --verbose
    python3 tools/E555_sort_rotations.py "$ROTATIONS" --top "$BORDERS" -o "$ROTATIONS.sorted"
    mv "$ROTATIONS.sorted" "$ROTATIONS"
    # An infeasible restart produces no border, so ask the beamer for the number
    # that actually survived rather than the number requested.
    BORDERS=$(grep -v '^ *#' "$ROTATIONS" | wc -l)
    [ "$BORDERS" -gt 0 ] || { echo "Stage A found no feasible border. Raise STEPS."; exit 1; }
    echo "=== Stage B: beaming from $BORDERS border(s) ==="
    BORDER_ARG=("$ROTATIONS" --border_row 0 --border_row_N "$BORDERS"
                --top_bottoms "$N_BOTTOMS" --top_columns "$N_COLUMNS")
else
    BORDER_ARG=(--random_edges --border_row_N "$N_BOTTOMS" --top_columns "$N_COLUMNS")
fi

DB_ARG=(); [ -n "$DB_FILE" ] && DB_ARG=(--db_file "$DB_FILE")

bin/E555_beamer "$SEED" "${BORDER_ARG[@]}" "${CLUE_ARG[@]}" "${DB_ARG[@]}" \
    --beam_width "$BEAM_WIDTH" --stop_row "$STOP_ROW" \
    --max_wall_sec "$MAX_WALL" --threads "$THREADS" --seed "$RNG_SEED" \
    --out_dir "$OUT_DIR" --print-cmd --verbose

# The beamer lists what it wrote; no need to rebuild the filenames here.
echo
if [ -s "$OUT_DIR/outputs.txt" ]; then
    echo "Boards written:"
    sed 's/^/  /' "$OUT_DIR/outputs.txt"
    echo
    echo "Look at the best one, then push it further:"
    echo "  python3 tools/E555_rank.py \$(head -1 $OUT_DIR/outputs.txt) --seed $SEED --top 5"
    echo "  bash examples/02_finalizer_regrow.sh BOARDS=\$(head -1 $OUT_DIR/outputs.txt)"
else
    echo "No board survived to row $STOP_ROW. Normal for a short run: raise"
    echo "BEAM_WIDTH, raise N_BOTTOMS, or lower STOP_ROW."
fi
