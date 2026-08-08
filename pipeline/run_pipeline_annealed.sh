#!/bin/bash
# =============================================================================
# run_pipeline_annealed.sh -- the whole pipeline, starting from Stage A
# =============================================================================
# THIS IS NOT AN EXAMPLE. It runs for as long as you let it and is meant to be
# left alone. To learn the individual tools, read ../examples/ first.
#
# Seven stages, each feeding the next, with the board viewer called in between
# so you can watch the board grow:
#
#   1  annealer    border rotations                    -> 1_rotations.csv
#   2  beamer      rows 0..11 (complete + incomplete)  -> 2_beam_row11.csv
#   3  finalizer   rows 0..12                          -> 3_final_row12.csv
#   4  roundhouse  rotate, refill a border strip       -> 4_strip.csv
#   5  corners     breaks herded to the nearest corner -> 5_corners.csv
#   6  ender ring  border ring re-threaded             -> 6_rung.csv
#      ender patch LNS around whatever break remains   -> 6_patched.csv
#   7  backtracker greedy dives around the open ring   -> 7_backtracker.csv
#
# WHEN TO USE THIS ONE
#   Stage A sizes the four sides so the beamer's sweep actually covers its own
#   space. That pays off when you intend to drill a FEW borders hard. If you
#   would rather play many hands on fresh random borders, run
#   run_pipeline_random.sh instead -- it skips Stage A entirely.
#
# Everything lands in $RUN_DIR; intermediate scratch is deleted as we go.
# Every knob is a variable in the block below -- edit them, or override from
# the environment:
#
#   RUN_DIR=my_run BEAM_WIDTH=20000 bash pipeline/run_pipeline_annealed.sh
#
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
RUN_DIR="${RUN_DIR:-$PWD/pipeline_out}"
THREADS="${THREADS:-$(nproc 2>/dev/null || echo 4)}"
DB_FILE="${DB_FILE:-}"                  # empty = build the chain DB in memory

# -- stage 1: border annealer ------------------------------------------------
# Per-side Euler-trail targets. The annealer drives the search toward
# low bottom and left side combinations, and large right and top side
# combinations (many ways to finish).
ROUNDS="${ROUNDS:-50}"                  # annealing restarts = candidate borders
                                        # (they run in parallel over $THREADS,
                                        #  so raising this is cheap)

# -- stage 2: beamer ---------------------------------------------------------
BEAM_WIDTH="${BEAM_WIDTH:-200000}"
BEAM_STOP_ROW="${BEAM_STOP_ROW:-11}"
BEAM_MAX_PARTIALS="${BEAM_MAX_PARTIALS:-50}"
BEAM_MAX_WALL="${BEAM_MAX_WALL:-600}"
BEAM_COLUMNS="${BEAM_COLUMNS:-50}"

# -- stage 3: finalizer ------------------------------------------------------
FIN_WIDTH="${FIN_WIDTH:-150000}"
FIN_STOP_ROW="${FIN_STOP_ROW:-12}"
FIN_MAX_PARTIALS="${FIN_MAX_PARTIALS:-10}"
FIN_MAX_WALL="${FIN_MAX_WALL:-600}"
# Lock rows 0..FIN_FROM, and keep it a few rows BELOW the beamer's stop row.
# Two reasons: the incomplete boards of stage 2 have one 5-piece segment of their
# top row still empty and the finalizer skips any line with an unplaced cell at or
# below the lock; and locking right under the frontier only asks the finalizer
# to redo the very row the beamer already failed to fill, with the same pieces.
# Freeing four rows gives it a genuinely different way up.
FIN_FROM="${FIN_FROM:-$((BEAM_STOP_ROW - 5))}"

# -- stage 4: roundhouse ------------------------------------------------------
# Cheap (megabytes, seconds): it either closes a board -- which would solve the
# puzzle -- or proves it cannot be closed that way, often in milliseconds.
RH_WIDTH="${RH_WIDTH:-5}"               # 2..5; 5 frees the top five rows
RH_ROTATE="${RH_ROTATE:-1}"             # 1 attacks the unsolved top first
RH_LINES="${RH_LINES:-20}"              # input boards to try
RH_WALL="${RH_WALL:-300}"

# -- stages 5/6: CP-SAT tail tools (need: pip install ortools) ----------------
TOP_N="${TOP_N:-5}"                     # best boards carried into stage 5
CPSAT_TIME="${CPSAT_TIME:-45}"          # seconds per board per solve
CPSAT_STALL="${CPSAT_STALL:-15}"        # give up after this long with no gain

# -- stage 7: backtracker ----------------------------------------------------
# Without --holes a completed board has no empty cell to search and the dives
# are a no-op, so we reopen the top/right/left border ring and its neighbours.
HOLES="${HOLES:-$REPO/data/holes_open_border_TR.csv}"
BT_MISMATCH="${BT_MISMATCH:-30}"
BT_RESTARTS="${BT_RESTARTS:-200000}"
BT_TIME="${BT_TIME:-300}"

# =============================================================================

banner() { echo; echo "==============================================================="; echo "  $*"; echo "==============================================================="; echo; }
show_board()   { echo; echo "--- first board of $(basename "$1") -----------------------------"; python3 "$REPO/tools/E555_viewer.py" "$1" --seed "$SEED" --no-url --row 0; echo; }
rows()   { grep -cv '^ *#' "$1" 2>/dev/null || echo 0; }
best()   { cut -d, -f2 "$1" | head -1 | tr -d ' '; }

# rank FILE -- rewrite a board CSV in place, best board first. --rescore
# recomputes field 2 as the number of matched edges (0..480) whatever tool wrote
# the line, which is what makes "the best board so far" comparable across
# stages; the tie-break is compactness, so of two equal boards the one with its
# breaks packed into fewer rows wins. Every tool downstream reads the canonical
# row, so the ranked file is re-feedable.
rank() {
    [ -s "$1" ] || return 0
    python3 "$REPO/tools/E555_rank.py" "$1" --seed "$SEED" \
        --sort breaks,break_rows --emit "$1.tmp" --rescore > /dev/null \
        && mv "$1.tmp" "$1"
    rm -f "$1.tmp"
}

[ -x "$REPO/bin/E555_beamer" ] || make -C "$REPO"
mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
echo "[cfg] repo=$REPO"
echo "[cfg] seed=$SEED"
echo "[cfg] run_dir=$RUN_DIR threads=$THREADS db_file=${DB_FILE:-<in memory>}"
START=$SECONDS

# -----------------------------------------------------------------------------
banner "STAGE 1/7  border annealer -- $ROUNDS rounds"
# -----------------------------------------------------------------------------
echo "Searching border rotations with per-side trail targets: low bottoms and left, large tops and right."
echo

python3 -u "$REPO/src/A_border/E555_edge_annealer.py" "$SEED" \
    --restarts "$ROUNDS" --steps 300000 --threads "$THREADS" \
    --w-bottom -3 --w-left -1 --w-right 4 --w-top 4 \
    --out raw_rotations.csv

# Stage B reads borders in file order, so the order decides which ones get
# searched. The score lives in the comment above each row, which is why this is
# a tool and not an inline awk pipeline.
python3 "$REPO/tools/E555_sort_rotations.py" raw_rotations.csv -o 1_rotations.csv
rm -f raw_rotations.csv

echo
echo ">> $(( $(rows 1_rotations.csv) )) border arrangements, best first -> 1_rotations.csv"
echo "   best: $(head -1 1_rotations.csv)"

# -----------------------------------------------------------------------------
banner "STAGE 2/7  beamer -- beam $BEAM_WIDTH up to row $BEAM_STOP_ROW"
# -----------------------------------------------------------------------------
echo "Growing boards row by row from every border. --incomplete_top also keeps"
echo "the boards that fill row $BEAM_STOP_ROW except for one of its three segments, and"
echo "--max_partials $BEAM_MAX_PARTIALS ends the sweep once that many boards have been written."
echo "First run rebuilds the 6.4 GB chain database: expect a few quiet minutes."
echo

BEAM_CMD=("$REPO/bin/E555_beamer" "$SEED" 1_rotations.csv
    --out_dir beam --threads "$THREADS"
    --beam_width "$BEAM_WIDTH" --stop_row "$BEAM_STOP_ROW"
    --border_row_N "$((ROUNDS/2))" --top_columns "$BEAM_COLUMNS" --lambda_Mahalanobis 10
    --incomplete_top --max_partials "$BEAM_MAX_PARTIALS"
    --max_wall_sec "$BEAM_MAX_WALL" --verbose)
[ -n "$DB_FILE" ] && BEAM_CMD+=(--db_file "$DB_FILE")
"${BEAM_CMD[@]}"

# One file pair per border row: complete boards first, then the incomplete ones.
cat beam/beam_completions_*_"$BEAM_STOP_ROW".csv          > 2_beam_row"$BEAM_STOP_ROW".csv 2>/dev/null || true
cat beam/beam_completions_*_"$BEAM_STOP_ROW"_partial.csv >> 2_beam_row"$BEAM_STOP_ROW".csv 2>/dev/null || true
rm -rf beam
BEAM_OUT="2_beam_row$BEAM_STOP_ROW.csv"
[ -s "$BEAM_OUT" ] || { echo "!! the beamer produced no board -- raise BEAM_MAX_WALL or lower BEAM_STOP_ROW"; exit 1; }
rank "$BEAM_OUT"          # best first: the finalizer below reads the first 100

echo
echo ">> $(rows "$BEAM_OUT") boards at row $BEAM_STOP_ROW, best $(best "$BEAM_OUT")/480 edges -> $BEAM_OUT"
show_board "$BEAM_OUT"

# -----------------------------------------------------------------------------
banner "STAGE 3/7  finalizer -- beam $FIN_WIDTH, rows 0..$FIN_FROM locked, up to row $FIN_STOP_ROW"
# -----------------------------------------------------------------------------
echo "Restarting the beam from each stage-2 board: rows 0..$FIN_FROM stay put, every"
echo "piece above them goes back in the pool and the search redoes rows $((FIN_FROM+1))..$FIN_STOP_ROW."
echo "The randomized beam search is repeated 3 times for each configuration."
echo

"$REPO/bin/E555_finalizer" "$SEED" "$BEAM_OUT" \
    --out_dir final --threads "$THREADS" --finalize_repeats 1 --frac_rand 0.0 \
    --beam_width "$FIN_WIDTH" --stop_row "$FIN_STOP_ROW" --finalize_from "$FIN_FROM" \
    --border_row_N "$BEAM_MAX_PARTIALS" --top_columns 0 --lambda_Mahalanobis 10 \
    --incomplete_top --max_partials "$FIN_MAX_PARTIALS" \
    --max_wall_sec "$FIN_MAX_WALL" --verbose

FIN_OUT="3_final_row$FIN_STOP_ROW.csv"
cat final/beam_completions_finalized_"$FIN_STOP_ROW".csv          > "$FIN_OUT"
cat final/beam_completions_finalized_"$FIN_STOP_ROW"_partial.csv >> "$FIN_OUT" 2>/dev/null || true
rm -rf final

# A short stage 2 leaves boards whose locked rows are dead ends -- every left
# column goes extinct below the stop row and the finalizer emits nothing. That
# is a real answer, not an error: carry the stage-2 boards on to Stage C, which
# is perfectly happy with a shallower partial, and say so loudly.
if [ -s "$FIN_OUT" ]; then
    rank "$FIN_OUT"
    echo
    echo ">> $(rows "$FIN_OUT") boards at row $FIN_STOP_ROW, best $(best "$FIN_OUT")/480 edges -> $FIN_OUT"
else
    rm -f "$FIN_OUT"
    FIN_OUT="$BEAM_OUT"
    echo
    echo ">> no board was able to grow from row $FIN_FROM to $FIN_STOP_ROW."
    echo "   Stage C continues from the row-$BEAM_STOP_ROW boards instead."
    echo "   To actually climb here, give stage 2 more time (BEAM_MAX_WALL) so it"
    echo "   ends with a live beam, lower BEAM_STOP_ROW, or lower FIN_FROM."
fi
show_board "$FIN_OUT"

# -----------------------------------------------------------------------------
banner "STAGE 4/7  roundhouse -- refill a ${RH_WIDTH}-wide border strip"
# -----------------------------------------------------------------------------
echo "Rotating each board and refilling one border band from the chain database."
echo "A completion here would be a solved puzzle; a refusal is a proof; a failure"
echo "still yields break-free boards with the hole gathered in one corner."
echo

"$REPO/bin/E555_roundhouse" "$SEED" "$FIN_OUT" \
    --out_dir strip --threads "$THREADS" \
    --rounds 1 --strip_width "$RH_WIDTH" --rotate "$RH_ROTATE" \
    --border_row_N "$RH_LINES" --max_wall_sec "$RH_WALL" --verbose

# First existing match, or empty. NOT `$(ls GLOB 2>/dev/null | head -1)`: when
# the glob matches nothing `ls` exits 2, pipefail promotes that to the
# pipeline's status and `set -e` kills the run -- which is exactly the "emitted
# nothing" case the else branch below exists to report. An unmatched glob stays
# literal in bash, so `-e` is the test that works.
RH_OUT=""
for f in strip/roundhouse_*_miss0.csv; do [ -e "$f" ] && { RH_OUT="$f"; break; }; done
if [ -n "$RH_OUT" ] && [ -s "$RH_OUT" ]; then
    cp "$RH_OUT" 4_strip.csv
    rank 4_strip.csv
    echo
    echo ">> $(rows 4_strip.csv) boards, best $(best 4_strip.csv)/480 edges -> 4_strip.csv"
    # Keep both: strip boards have their holes in one corner, finalizer boards
    # have theirs spread across the top. Stage C can use either.
    cat "$FIN_OUT" 4_strip.csv > 4_pool.csv
    rank 4_pool.csv
    FIN_OUT=4_pool.csv
else
    echo
    echo ">> the roundhouse emitted nothing: its oracle refuted every strip start."
    echo "   Try RH_ROTATE=-1 or a larger RH_WIDTH next run."
fi
rm -rf strip

# -----------------------------------------------------------------------------
banner "STAGE 5/7  corners -- sliding window + side passes, $TOP_N boards"
# -----------------------------------------------------------------------------
echo "Six CP-SAT passes with E555_topper.py: four over a window that slides"
echo "down from the top border, then two that open a corner and the left band,"
echo "so breaks stranded off the top are attacked where they are. The driver is"
echo "pipeline/topper_sweep.sh; PRESET=safe sweeps every side without ever"
echo "unsetting a piece, PRESET=deep spends breaks to reach into the core."
echo

head -n "$TOP_N" "$FIN_OUT" > corners_in.csv
INPUT="$PWD/corners_in.csv" NUM_ROWS="$TOP_N" WORKERS="$THREADS" \
MAX_TIME="$CPSAT_TIME" STALL_TIME="$CPSAT_STALL" PIECE_SEED="$SEED" \
OUT_DIR="$PWD/sweep" PRESET=window bash "$REPO/pipeline/topper_sweep.sh"

# topper_sweep.sh writes topped_<PRESET>_<first row>.csv into its OUT_DIR, and
# the row suffix follows SLURM_ARRAY_TASK_ID -- so match it rather than assume
# it is 0. Same literal-glob idiom as stage 4: no `ls` pipeline.
SWEEP_OUT=""
for f in sweep/topped_*.csv; do [ -e "$f" ] && { SWEEP_OUT="$f"; break; }; done
[ -n "$SWEEP_OUT" ] || { echo "[ERROR] topper_sweep.sh produced no board"; exit 1; }
mv "$SWEEP_OUT" 5_corners.csv
rm -rf sweep logs corners_in.csv
rank 5_corners.csv

echo
echo ">> $(rows 5_corners.csv) boards, best $(best 5_corners.csv)/480 edges -> 5_corners.csv"
show_board 5_corners.csv

# -----------------------------------------------------------------------------
banner "STAGE 6/7  ender -- ring sweep, then patch"
# -----------------------------------------------------------------------------
echo "Stage 4 leaves the damage on the border bands, i.e. in and around the"
echo "border ring -- which is exactly what ender's ring sweep re-threads. A final"
echo "patch pass then runs a localized LNS on whatever break the sweep left."
echo

python3 "$REPO/src/C_tail/E555_ender.py" "$SEED" 5_corners.csv 6_rung.csv \
    --mode ring --reach 2 --max-changes 16 --workers "$THREADS" \
    --max-time "$((CPSAT_TIME*2))" --stall-time "$CPSAT_STALL" --verbose

python3 "$REPO/src/C_tail/E555_ender.py" "$SEED" 6_rung.csv 6_patched.csv \
    --workers "$THREADS" \
    --max-time "$((CPSAT_TIME*3))" --stall-time "$((CPSAT_STALL*2))" --verbose

rank 6_rung.csv
rank 6_patched.csv

echo
echo ">> ring-sweep best $(best 6_rung.csv)/480, patch best $(best 6_patched.csv)/480 edges"
show_board 6_patched.csv

# -----------------------------------------------------------------------------
banner "STAGE 7/7  backtracker -- greedy dives on the best board"
# -----------------------------------------------------------------------------
cat 5_corners.csv 6_rung.csv 6_patched.csv > all_candidates.csv
rank all_candidates.csv
head -1 all_candidates.csv > 7_best_input.csv
rm -f all_candidates.csv

echo "Best board of stages 4-5: $(best 7_best_input.csv)/480 correct edges."
echo "Reopening the ring cells in $(basename "$HOLES") and diving on them"
echo "($BT_RESTARTS randomized greedy dives, keeping the best)."
echo

"$REPO/bin/E555_backtracker" "$SEED" 7_best_input.csv 7_backtracker.csv \
    --row 0 --count 1 --threads "$THREADS" \
    --holes "$HOLES" --order mrv --break-mode stuck \
    --max-mismatch "$BT_MISMATCH" --stuck_restarts "$BT_RESTARTS" \
    --time-limit "$BT_TIME" --verbose

# -----------------------------------------------------------------------------
banner "PIPELINE COMPLETE  ($(( (SECONDS-START)/60 )) min $(( (SECONDS-START)%60 )) s)"
# -----------------------------------------------------------------------------
cat 7_best_input.csv 7_backtracker.csv > final_candidates.csv
rank final_candidates.csv
head -1 final_candidates.csv > FINAL_best.csv
rm -f final_candidates.csv

echo "Files kept in $RUN_DIR:"
ls -1 [0-9]_*.csv FINAL_best.csv | sed 's/^/  /'
echo
echo "Best board of the whole run: $(best FINAL_best.csv)/480 correct edges."
show_board FINAL_best.csv
echo "Re-feed it to any Stage C tool, or open the bucas URL printed above."
