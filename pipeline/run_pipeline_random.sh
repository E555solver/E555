#!/bin/bash
##SBATCH --job-name=E555_pipeline
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=12G --time=24:00:00
##SBATCH --output=logs/pipeline_%j.out
#
# =============================================================================
# run_pipeline_random.sh -- the whole pipeline, no border annealer
# =============================================================================
# THIS IS NOT AN EXAMPLE. It runs for as long as you let it and is meant to be
# left alone. To learn the individual tools, read ../examples/ first.
#
# Six stages, each feeding the next:
#
#   1  beamer       --random_edges, sampled borders     -> 1_beam.csv
#   2  finalizer    free the top rows and re-grow them  -> 2_final.csv
#   3  roundhouse   rotate, refill a border strip       -> 3_strip.csv
#   4  topper       herd breaks to the nearest corner   -> 4_topped.csv
#   5  ender        ring sweep, then patch              -> 5_ended.csv
#   6  backtracker  greedy dives on the best board      -> 6_dived.csv
#                                                          FINAL_best.csv
#
# WHY NO ANNEALER
#   Stage A sizes the four sides so the beamer's sweep covers its own space.
#   That is worth real time when you intend to drill ONE border hard. This
#   pipeline does the opposite -- it plays many hands with fresh random borders
#   -- so the annealer's guarantees buy little and cost minutes per run. Use
#   run_pipeline_annealed.sh when you want the other trade. The Gumbel
#   temperatures below are set to match: sample the borders rather than skim the
#   best-ranked, and spend the width on many frames instead of one.
#
# EVERY STAGE IS OPTIONAL AND RE-FEEDABLE
#   All six read and write the same canonical CSV. If a stage produces nothing
#   the script says so and carries the previous stage's boards forward, because
#   an empty result is usually a real answer about the board, not a crash.
#
# Everything lands in $RUN_DIR. Override any setting from the environment:
#
#   RUN_DIR=my_run THREADS=16 BEAM_WALL=1800 bash pipeline/run_pipeline_random.sh
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- what to run on ---------------------------------------------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
RUN_DIR="${RUN_DIR:-$PWD/pipeline_out}"
THREADS="${THREADS:-$(nproc 2>/dev/null || echo 4)}"
DB_FILE="${DB_FILE:-}"              # path to cache the 6.4 GB chain DB; empty = in memory
CLUES="${CLUES:-0}"                 # 1 = hold the Eternity II clue pieces in place

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
# A clue-built chain database has the same seed but different CONTENTS, and the
# beamer refuses a cache whose exclusion set does not match. Keep the two apart so
# toggling CLUES does not silently rebuild 6.4 GB each way.
[ "$CLUES" = 1 ] && [ -n "$DB_FILE" ] && DB_FILE="$DB_FILE.clue"
RNG_SEED="${RNG_SEED:-0}"           # 0 = clock+pid, so each run explores elsewhere

# ---- stage 1: beamer --------------------------------------------------------
BEAM_WIDTH="${BEAM_WIDTH:-200000}"
BEAM_STOP="${BEAM_STOP:-11}"        # last row the beamer fills
BEAM_BOARDS="${BEAM_BOARDS:-250}"   # stop once this many boards are written
BEAM_WALL="${BEAM_WALL:-900}"       # seconds
BEAM_COLUMNS="${BEAM_COLUMNS:-8}"   # random left columns per random bottom row
# Selection temperatures (0 = off). Yield turned out to be a property of the
# (bottom, column) PAIR, not of either ranking, so sampling the borders instead
# of taking the best-ranked costs nothing measurable and spreads the run over
# many more frames -- which is the whole point of the random-edges pipeline.
# The beam's own band is the one that pays: 0.25 beat the --frac_rand 0.75
# default by ~28% (a nonzero tau0/tau1 stands that band down).
BEAM_TAU_BOTTOMS="${BEAM_TAU_BOTTOMS:-2}"
BEAM_TAU_COLUMNS="${BEAM_TAU_COLUMNS:-4}"
BEAM_TAU0="${BEAM_TAU0:-0.25}"
BEAM_TAU1="${BEAM_TAU1:-0.25}"

# ---- stage 2: finalizer -----------------------------------------------------
FIN_STOP="${FIN_STOP:-12}"
FIN_WIDTH="${FIN_WIDTH:-100000}"
FIN_BOARDS="${FIN_BOARDS:-20}"
FIN_WALL="${FIN_WALL:-600}"
# Lock rows 0..FIN_FROM. Keep it several rows BELOW the beamer's stop row:
# locking just under the frontier only asks the finalizer to redo the very row
# that already failed, with the same pieces. Freeing five rows gives it a
# genuinely different way up.
FIN_FROM="${FIN_FROM:-$((BEAM_STOP - 5))}"

# ---- stage 3: roundhouse ----------------------------------------------------
# Cheap (megabytes, seconds). It either closes the board -- which would solve
# the puzzle -- or proves it cannot be closed that way, often in milliseconds.
# Its failures are still useful: break-free boards with the hole gathered in
# one corner, which is the shape Stage C closes best.
RH_WIDTH="${RH_WIDTH:-5}"           # 2..5; 5 frees the top five rows
RH_ROUNDS="${RH_ROUNDS:-1}"         # 1, 2 or 3 bands
RH_ROTATE="${RH_ROTATE:-1}"         # 1 attacks the unsolved top first
RH_LINES="${RH_LINES:-20}"          # input boards to try
RH_WALL="${RH_WALL:-300}"

# ---- stages 4/5: CP-SAT (needs: pip install ortools) ------------------------
TOP_N="${TOP_N:-5}"                 # best boards carried into Stage C
CPSAT_TIME="${CPSAT_TIME:-60}"      # seconds per board per solve
CPSAT_STALL="${CPSAT_STALL:-20}"    # give up after this long with no gain

# ---- stage 6: backtracker ---------------------------------------------------
# A complete board has no empty cell, so the dives would be a no-op: the mask
# reopens the top/right border ring and its neighbours first.
HOLES="${HOLES:-$REPO/data/holes_open_border_TR.csv}"
BT_MISMATCH="${BT_MISMATCH:-30}"
BT_RESTARTS="${BT_RESTARTS:-200000}"
BT_TIME="${BT_TIME:-300}"
# =============================================================================

# Five one-line helpers, used throughout.
banner()     { echo; echo "==============================================================="; echo "  $*"; echo "==============================================================="; echo; }
show_board() { echo; python3 "$REPO/tools/E555_viewer.py" "$1" --seed "$SEED" --no-url --row 0; echo; }
rows()       { grep -cv '^ *#' "$1" 2>/dev/null || echo 0; }
best()       { cut -d, -f2 "$1" | head -1 | tr -d ' '; }
# rank FILE: rewrite a board CSV in place, best board first. --rescore recomputes
# field 2 as matched edges (0..480) whatever tool wrote the line, which is what
# makes "best so far" comparable across stages; the tie-break is compactness, so
# of two equal boards the one with its breaks packed into fewer rows wins. The
# result is canonical, so every tool downstream can read it.
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
banner "STAGE 1/6  beamer --random_edges, beam $BEAM_WIDTH up to row $BEAM_STOP"
# -----------------------------------------------------------------------------
echo "Borders are sampled from the seed, so nothing has to be prepared. The"
echo "first run builds the 6.4 GB chain database: expect a few quiet minutes."
echo

BEAM_CMD=("$REPO/bin/E555_beamer" "$SEED" --random_edges
    --out_dir beam --threads "$THREADS"
    --beam_width "$BEAM_WIDTH" --stop_row "$BEAM_STOP"
    --border_row_N "$BEAM_BOARDS" --top_columns "$BEAM_COLUMNS"
    --gumbel_tau_bottoms "$BEAM_TAU_BOTTOMS" --gumbel_tau_columns "$BEAM_TAU_COLUMNS"
    --gumbel_tau0 "$BEAM_TAU0" --gumbel_tau1 "$BEAM_TAU1"
    --lambda_Mahalanobis 10 --incomplete_top
    --max_partials "$BEAM_BOARDS" --max_wall_sec "$BEAM_WALL" --verbose "${CLUE_ARG[@]}")
[ -n "$DB_FILE" ] && BEAM_CMD+=(--db_file "$DB_FILE")
[ "$RNG_SEED" != "0" ] && BEAM_CMD+=(--seed "$RNG_SEED")
"${BEAM_CMD[@]}"

# --incomplete_top also keeps boards that fill the stop row except for one of its
# three 5-piece segments -- the hole is at cols 11-15, 6-10 or 1-5; those land in
# a separate _partial.csv.
cat beam/beam_completions_random_"$BEAM_STOP".csv          > 1_beam.csv 2>/dev/null || true
cat beam/beam_completions_random_"$BEAM_STOP"_partial.csv >> 1_beam.csv 2>/dev/null || true
rm -rf beam
[ -s 1_beam.csv ] || { echo "!! no board reached row $BEAM_STOP -- raise BEAM_WALL or lower BEAM_STOP"; exit 1; }
rank 1_beam.csv
BEST_SO_FAR=1_beam.csv
echo
echo ">> $(rows 1_beam.csv) boards, best $(best 1_beam.csv)/480 -> 1_beam.csv"
show_board 1_beam.csv

# -----------------------------------------------------------------------------
banner "STAGE 2/6  finalizer -- rows 0..$FIN_FROM locked, re-grow up to row $FIN_STOP"
# -----------------------------------------------------------------------------
echo "Every piece above row $FIN_FROM goes back in the pool and rows"
echo "$((FIN_FROM+1))..$FIN_STOP are searched again over a database rebuilt without"
echo "the locked pieces."
echo

"$REPO/bin/E555_finalizer" "$SEED" 1_beam.csv \
    --out_dir final --threads "$THREADS" \
    --finalize_from "$FIN_FROM" --finalize_repeats 1 \
    --beam_width "$FIN_WIDTH" --stop_row "$FIN_STOP" \
    --border_row_N "$BEAM_BOARDS" --top_columns 0 \
    --lambda_Mahalanobis 10 --frac_rand 0.0 --incomplete_top \
    --max_partials "$FIN_BOARDS" --max_wall_sec "$FIN_WALL" --verbose "${CLUE_ARG[@]}"

cat final/beam_completions_finalized_"$FIN_STOP".csv          > 2_final.csv 2>/dev/null || true
cat final/beam_completions_finalized_"$FIN_STOP"_partial.csv >> 2_final.csv 2>/dev/null || true
rm -rf final

if [ -s 2_final.csv ]; then
    rank 2_final.csv
    BEST_SO_FAR=2_final.csv
    echo
    echo ">> $(rows 2_final.csv) boards, best $(best 2_final.csv)/480 -> 2_final.csv"
else
    rm -f 2_final.csv
    echo
    echo ">> nothing grew from row $FIN_FROM to $FIN_STOP. That is a real answer:"
    echo "   every left column went extinct. Carrying the stage-1 boards forward."
fi
show_board "$BEST_SO_FAR"

# -----------------------------------------------------------------------------
banner "STAGE 3/6  roundhouse -- refill a $RH_WIDTH-wide strip"
# -----------------------------------------------------------------------------
echo "Rotating each board and refilling one border band. A completion here would"
echo "be a solved puzzle; a refusal is a proof; a failure still yields break-free"
echo "boards with the hole gathered in one corner."
echo

"$REPO/bin/E555_roundhouse" "$SEED" "$BEST_SO_FAR" \
    --out_dir strip --threads "$THREADS" \
    --rounds "$RH_ROUNDS" --strip_width "$RH_WIDTH" --rotate "$RH_ROTATE" \
    --border_row_N "$RH_LINES" --max_wall_sec "$RH_WALL" --verbose "${CLUE_ARG[@]}"

# First existing match, or empty. NOT `$(ls GLOB 2>/dev/null | head -1)`: when
# the glob matches nothing `ls` exits 2, pipefail promotes that to the
# pipeline's status and `set -e` kills the run -- which is exactly the "emitted
# nothing" case the else branch below exists to report. An unmatched glob stays
# literal in bash, so `-e` is the test that works.
RH_OUT=""
for f in strip/roundhouse_*_miss0.csv; do [ -e "$f" ] && { RH_OUT="$f"; break; }; done
if [ -n "$RH_OUT" ] && [ -s "$RH_OUT" ]; then
    cp "$RH_OUT" 3_strip.csv
    rank 3_strip.csv
    echo
    echo ">> $(rows 3_strip.csv) boards, best $(best 3_strip.csv)/480 -> 3_strip.csv"
    # Keep both: the strip boards have their holes in one corner, the stage-2
    # boards have theirs spread across the top. Stage C can use either.
    cat "$BEST_SO_FAR" 3_strip.csv > 3_pool.csv
    rank 3_pool.csv
    BEST_SO_FAR=3_pool.csv
else
    echo
    echo ">> the roundhouse emitted nothing: no legal strip start survived its"
    echo "   oracle. Try RH_ROTATE=-1 or a larger RH_WIDTH next run."
fi
rm -rf strip

# -----------------------------------------------------------------------------
banner "STAGE 4/6  topper -- herd breaks to the nearest corner, $TOP_N boards"
# -----------------------------------------------------------------------------
echo "Four upward passes over the top band, then one that folds the remainder"
echo "into the top-right corner. --unused_rows 0 throughout, so no pass can make"
echo "a board worse."
echo

head -n "$TOP_N" "$BEST_SO_FAR" > 4_topped.csv
for depth in 5 4 3 3; do
    python3 "$REPO/src/C_tail/E555_topper.py" "$SEED" 4_topped.csv 4_step.csv \
        --side T --work-rows "$depth" --unused_rows 0 --count "$TOP_N" \
        --workers "$THREADS" --max-time "$CPSAT_TIME" --stall-time "$CPSAT_STALL" "${CLUE_ARG[@]}"
    mv 4_step.csv 4_topped.csv
done
python3 "$REPO/src/C_tail/E555_topper.py" "$SEED" 4_topped.csv 4_step.csv \
    --side TR --work-rows 3 --unused_rows 0 --count "$TOP_N" \
    --workers "$THREADS" --max-time "$CPSAT_TIME" --stall-time "$CPSAT_STALL" "${CLUE_ARG[@]}"
mv 4_step.csv 4_topped.csv
rank 4_topped.csv
echo
echo ">> best $(best 4_topped.csv)/480 -> 4_topped.csv"
show_board 4_topped.csv

# -----------------------------------------------------------------------------
banner "STAGE 5/6  ender -- ring sweep, then patch"
# -----------------------------------------------------------------------------
echo "Stage 4 leaves the damage on the border bands, which is exactly what the"
echo "ring sweep re-threads. The patch pass then tidies whatever survives."
echo

python3 "$REPO/src/C_tail/E555_ender.py" "$SEED" 4_topped.csv 5_ring.csv \
    --mode ring --reach 2 --max-changes 16 --workers "$THREADS" \
    --max-time "$((CPSAT_TIME*2))" --stall-time "$CPSAT_STALL" --verbose "${CLUE_ARG[@]}"

python3 "$REPO/src/C_tail/E555_ender.py" "$SEED" 5_ring.csv 5_ended.csv \
    --mode patch --reach 2 --max-changes 16 --workers "$THREADS" \
    --max-time "$((CPSAT_TIME*3))" --stall-time "$((CPSAT_STALL*2))" --verbose "${CLUE_ARG[@]}"

rank 5_ended.csv
rm -f 5_ring.csv
echo
echo ">> best $(best 5_ended.csv)/480 -> 5_ended.csv"
show_board 5_ended.csv

# -----------------------------------------------------------------------------
banner "STAGE 6/6  backtracker -- greedy dives on the best board"
# -----------------------------------------------------------------------------
cat 4_topped.csv 5_ended.csv > 6_pool.csv
rank 6_pool.csv
head -1 6_pool.csv > 6_in.csv
rm -f 6_pool.csv
echo "Best of stages 4-5: $(best 6_in.csv)/480. Reopening the ring cells of"
echo "$(basename "$HOLES") and running $BT_RESTARTS randomized dives on them."
echo

"$REPO/bin/E555_backtracker" "$SEED" 6_in.csv 6_dived.csv \
    --row 0 --count 1 --threads "$THREADS" \
    --holes "$HOLES" --order mrv --break-mode stuck \
    --max-mismatch "$BT_MISMATCH" --stuck_restarts "$BT_RESTARTS" \
    --time-limit "$BT_TIME" --verbose

# -----------------------------------------------------------------------------
banner "PIPELINE COMPLETE  ($(( (SECONDS-START)/60 )) min $(( (SECONDS-START)%60 )) s)"
# -----------------------------------------------------------------------------
cat 6_in.csv 6_dived.csv > final_pool.csv
rank final_pool.csv
head -1 final_pool.csv > FINAL_best.csv
rm -f final_pool.csv 6_in.csv

echo "Files kept in $RUN_DIR:"
ls -1 [0-9]_*.csv FINAL_best.csv 2>/dev/null | sed 's/^/  /'
echo
echo "Best board of the whole run: $(best FINAL_best.csv)/480 correct edges."
show_board FINAL_best.csv
echo "Re-feed FINAL_best.csv to any tool, or let run_board_farm.sh loop this."
