#!/bin/bash
# run_pipeline.sh -- the whole chain, borders to a finished board.
#
#   bash pipeline/run_pipeline.sh                      # annealed borders
#   bash pipeline/run_pipeline.sh BORDERS=random       # sampled borders
#   bash pipeline/run_pipeline.sh RUN_DIR=run7 THREADS=16
#
# BORDERS=annealed searches for good borders with Stage A first and beams from
# the best of them. BORDERS=random samples borders straight from the seed:
# nothing to prepare, weaker material, far more frames per hour.
#
# Every stage feeds the next and writes a numbered CSV into RUN_DIR, so any
# stage's output can be picked up by hand afterwards. A stage that emits
# nothing says so and the run carries on with what it already had.
#
# NEEDS ~8 GB RAM for the beamer, and `pip install ortools` for the two CP-SAT
# stages. What each stage does and which knobs matter: pipeline/README.md
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
RUN_DIR=pipeline_out                    # everything is written in here
THREADS=8
DB_FILE=                                # cache the 6.4 GB chain DB here; empty
                                        # = build it in memory each run
BORDERS=annealed                        # annealed | random
CLUES=0                                 # 1 = hold the Eternity II clue pieces
HOLES=data/holes_open_border_TRL.csv    # cells the backtracker may reopen

# stage 1: border annealer (BORDERS=annealed only)
ROUNDS=50               # annealing restarts = candidate borders
STEPS=500000            # annealing steps per restart; 250000 is the floor

# stage 2: beamer
BEAM_WIDTH=200000
BEAM_STOP_ROW=11        # last row the beam fills
BEAM_MAX_PARTIALS=50    # stop once this many boards are written
BEAM_MAX_WALL=600       # seconds
BEAM_COLUMNS=50         # left columns per bottom row
BEAM_FRAC_RAND=0.10     # fraction of the beam kept at random, not by score
RNG_SEED=               # fix the beam's seed to repeat a run; empty = a fresh
                        # one from the clock, which is what a farm wants

# stage 3: finalizer. Keep FIN_FROM several rows BELOW the beam's stop row:
# locking just under the frontier only asks it to redo the row that just failed,
# with the same pieces.
FIN_WIDTH=150000
FIN_STOP_ROW=12
FIN_FROM=6
FIN_MAX_PARTIALS=10
FIN_MAX_WALL=600
# --frac_rand 0.0 overrides the finalizer's default of 0.30. It dates from when
# that default was 0.75 and plainly too high, and it has NOT been re-measured
# against 0.30. The default assumes repeated passes over one board, which is not
# what happens here (--finalize_repeats 1), so turning the band off is at least
# self-consistent -- but 0 is not a measured setting either: the beamer ships
# 0.10, and 0.25 beat 0.75 by ~28%.
FIN_FRAC_RAND=0.0

# stage 4: roundhouse -- cheap (megabytes, seconds). It either closes a board,
# which would solve the puzzle, or proves it cannot be closed that way.
RH_WIDTH=5              # 2..5; 5 frees the top five rows
RH_ROTATE=1
RH_LINES=20             # input boards to try
RH_WALL=600

# stages 5/6: CP-SAT tail (needs: pip install ortools)
TOP_N=20                # boards carried into the CP-SAT stages
CPSAT_TIME=120          # seconds per SOLVE, and the ender solves per break --
                        # measured, its two passes were 474 s of a 620 s run
CPSAT_STALL=40
ENDER_REACH=2           # interior BFS layers opened around each break
ENDER_CHANGES=16        # how many pieces the ender may actually move

# stage 7: backtracker
BT_MISMATCH=30
BT_RESTARTS=200000
BT_TIME=300
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
case "$BORDERS" in
    annealed|random) ;;
    *) echo "BORDERS must be 'annealed' or 'random', got: $BORDERS" >&2; exit 1 ;;
esac
cd "$REPO"
[ -d bin ] && [ -d tools ] ||
    { echo "REPO=$REPO is not an E555 checkout -- set REPO at the top" >&2; exit 1; }
[ -x bin/E555_beamer ] || make

# The annealer needs at least 250000 steps to do its job. Below that it is not
# merely weaker -- it often fails to place a legal border at all: measured on
# the real seed, 8 restarts x 3000 steps found 2 feasible borders and 2 x 2000
# returned 1 on one run and 0 on the next, while every restart at 250000 steps
# succeeded (2/2, 4/4, 8/8). Warn rather than refuse: a deliberately tiny smoke
# run is a legitimate thing to ask for.
[ "$STEPS" -ge 250000 ] || echo "[warn] STEPS=$STEPS is below 250000, the point where"\
    " the annealer reliably finds a legal border at all. Expect few or none."

banner() { echo; echo "==============================================================="
           echo "  $*"; echo "==============================================================="; echo; }
rows()   { python3 "$TOOLS/E555_rank.py" "$1" --count; }
best()   { python3 "$TOOLS/E555_rank.py" "$1" --seed_file "$SEED" --field score; }
show()   { echo; python3 "$TOOLS/E555_viewer.py" "$1" --seed_file "$SEED" --no_url --row 0; echo; }

# rank FILE -- rewrite a board CSV in place, best board first. --rescore
# recomputes field 2 as the number of matched edges (0..480) whatever tool wrote
# the line, which is what makes "the best board so far" comparable across
# stages; the tie-break is compactness, so of two equal boards the one with its
# breaks packed into fewer rows wins.
rank() {
    [ -s "$1" ] || return 0
    python3 "$TOOLS/E555_rank.py" "$1" --seed_file "$SEED" \
        --sort breaks,break_rows --out "$1.tmp" --rescore > /dev/null
    mv "$1.tmp" "$1"
}

# Paths are resolved against the repo before the cd into RUN_DIR below.
abs() { case "$1" in /*) printf '%s' "$1";; *) printf '%s' "$PWD/$1";; esac; }
SEED=$(abs "$SEED"); HOLES=$(abs "$HOLES")
[ -n "$DB_FILE" ] && DB_FILE=$(abs "$DB_FILE")
TOOLS=$PWD/tools; SRC=$PWD/src; BIN=$PWD/bin
mkdir -p "$RUN_DIR"; cd "$RUN_DIR"

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
DB_ARG=();   [ -n "$DB_FILE" ] && DB_ARG=(--db_file "$DB_FILE")
SEED_ARG=(); [ -n "$RNG_SEED" ] && SEED_ARG=(--rng_seed "$RNG_SEED")

[ "$BORDERS" = annealed ] && NSTAGE=7 || NSTAGE=6
STAGE=0; STAGE_T0=0
# Each banner closes the previous stage with its wall time, so a finished run
# says where the hours went without anyone having to instrument it afterwards.
next() {
    [ "$STAGE" -gt 0 ] && echo "[time] stage $STAGE took $(( SECONDS - STAGE_T0 ))s"
    STAGE=$((STAGE + 1)); STAGE_T0=$SECONDS
    banner "STAGE $STAGE/$NSTAGE  $*"
}

echo "[cfg] repo=$REPO  seed=$SEED"
echo "[cfg] run_dir=$PWD  threads=$THREADS  borders=$BORDERS"
echo "[cfg] db_file=${DB_FILE:-<in memory>}"
START=$SECONDS

# -----------------------------------------------------------------------------
if [ "$BORDERS" = annealed ]; then
    next "border annealer -- $ROUNDS restarts"
    echo "Searching border rotations: the bottom is ignored, and the top and"
    echo "right sides are weighted to leave the most ways to continue."
    echo
    rm -f raw_rotations.csv         # the annealer APPENDS; start clean
    python3 -u "$SRC/A_border/E555_edge_annealer.py" "$SEED" \
        --restarts "$ROUNDS" --steps "$STEPS" --threads "$THREADS" \
        --w_bottom 0 --w_left 1 --w_right 3 --w_top 2 \
        --out raw_rotations.csv

    # A restart that fails to place a legal border writes nothing, and whether
    # any of ROUNDS succeeds is stochastic at small budgets: measured on the
    # real seed, 8 restarts x 3000 steps found 2 feasible borders and the same
    # 8 at 10000 steps found 4, while 2 x 2000 returned 1 on one run and 0 on
    # the next. Check before the sort tool does, so the message names the fix.
    [ -s raw_rotations.csv ] && [ "$(grep -cv '^ *#' raw_rotations.csv)" -gt 0 ] || {
        echo "!! no restart found a feasible border. Raise STEPS or ROUNDS."; exit 1; }

    # Stage B reads borders in file order, so the order decides which get
    # searched. The score lives in the comment above each row, which is why
    # this is a tool and not an inline awk pipeline.
    python3 "$TOOLS/E555_sort_rotations.py" raw_rotations.csv -o 1_rotations.csv
    rm -f raw_rotations.csv
    NBORDER=$(grep -v '^ *#' 1_rotations.csv | wc -l)
    echo
    echo ">> $NBORDER border arrangements, best first -> 1_rotations.csv"
    BORDER_ARG=(1_rotations.csv --num_rows "$NBORDER" --top_columns "$BEAM_COLUMNS")
else
    BORDER_ARG=(--random_edges --samples "$BEAM_MAX_PARTIALS"
                --top_columns "$BEAM_COLUMNS" --frac_rand "$BEAM_FRAC_RAND")
fi

# -----------------------------------------------------------------------------
next "beamer -- beam $BEAM_WIDTH up to row $BEAM_STOP_ROW"
echo "Growing boards row by row. --incomplete_top also keeps boards that fill"
echo "row $BEAM_STOP_ROW except for one of its three segments, and --max_emitted"
echo "$BEAM_MAX_PARTIALS ends the sweep once that many boards have been written."
echo "The first run builds the 6.4 GB chain database: expect a few quiet minutes."
echo

"$BIN/E555_beamer" "$SEED" "${BORDER_ARG[@]}" "${CLUE_ARG[@]}" "${DB_ARG[@]}" \
    "${SEED_ARG[@]}" \
    --out_dir beam --threads "$THREADS" \
    --beam_width "$BEAM_WIDTH" --stop_row "$BEAM_STOP_ROW" \
    --incomplete_top --max_emitted "$BEAM_MAX_PARTIALS" \
    --wall_time "$BEAM_MAX_WALL" --print_cmd --verbose

# One file pair per border row. The beamer lists what it filled, so this reads
# that list instead of globbing for names it would have to know in advance.
BEAM_OUT="2_beam_row$BEAM_STOP_ROW.csv"
xargs -r cat < beam/outputs.txt > "$BEAM_OUT"
rm -rf beam
[ -s "$BEAM_OUT" ] ||
    { echo "!! the beamer produced no board -- raise BEAM_MAX_WALL or lower BEAM_STOP_ROW"; exit 1; }
rank "$BEAM_OUT"
echo
echo ">> $(rows "$BEAM_OUT") boards at row $BEAM_STOP_ROW, best $(best "$BEAM_OUT")/480 -> $BEAM_OUT"
show "$BEAM_OUT"

# -----------------------------------------------------------------------------
next "finalizer -- rows 0..$FIN_FROM locked, re-grow up to row $FIN_STOP_ROW"
echo "Every piece above row $FIN_FROM goes back in the pool and rows"
echo "$((FIN_FROM+1))..$FIN_STOP_ROW are searched again over a database rebuilt"
echo "without the locked pieces. One deterministic pass per board."
echo

"$BIN/E555_finalizer" "$SEED" "$BEAM_OUT" "${CLUE_ARG[@]}" \
    --out_dir final --threads "$THREADS" \
    --finalize_repeats 1 --frac_rand "$FIN_FRAC_RAND" \
    --beam_width "$FIN_WIDTH" --stop_row "$FIN_STOP_ROW" --finalize_from "$FIN_FROM" \
    --num_rows "$BEAM_MAX_PARTIALS" --top_columns 0 \
    --incomplete_top --max_emitted "$FIN_MAX_PARTIALS" \
    --wall_time "$FIN_MAX_WALL" --print_cmd --verbose

FIN_OUT="3_final_row$FIN_STOP_ROW.csv"
xargs -r cat < final/outputs.txt > "$FIN_OUT"
rm -rf final

# A short beam stage leaves boards whose locked rows are dead ends: every left
# column goes extinct below the stop row and the finalizer emits nothing. That
# is a real answer, not an error -- Stage C is perfectly happy with a shallower
# partial, so carry the beam boards on and say so loudly.
if [ -s "$FIN_OUT" ]; then
    rank "$FIN_OUT"
    echo
    echo ">> $(rows "$FIN_OUT") boards at row $FIN_STOP_ROW, best $(best "$FIN_OUT")/480 -> $FIN_OUT"
else
    rm -f "$FIN_OUT"
    FIN_OUT="$BEAM_OUT"
    echo
    echo ">> no board grew from row $FIN_FROM to $FIN_STOP_ROW. Stage C continues"
    echo "   from the row-$BEAM_STOP_ROW boards instead. To climb here, give the"
    echo "   beam more time (BEAM_MAX_WALL) so it ends live, or lower FIN_FROM."
fi
show "$FIN_OUT"

# -----------------------------------------------------------------------------
next "roundhouse -- refill a ${RH_WIDTH}-wide border strip"
echo "Rotating each board and refilling one border band from the chain database."
echo "A completion here would be a solved puzzle; a refusal is a proof; a failure"
echo "still yields break-free boards with the hole gathered in one corner."
echo

"$BIN/E555_roundhouse" "$SEED" "$FIN_OUT" "${CLUE_ARG[@]}" \
    --out_dir strip --threads "$THREADS" \
    --rounds 1 --strip_width "$RH_WIDTH" --rotate "$RH_ROTATE" \
    --num_rows "$RH_LINES" --wall_time "$RH_WALL" --print_cmd --verbose

RH_OUT=$(head -1 strip/outputs.txt)
if [ -n "$RH_OUT" ]; then
    cp "$RH_OUT" 4_strip.csv
    rank 4_strip.csv
    echo
    echo ">> $(rows 4_strip.csv) boards, best $(best 4_strip.csv)/480 -> 4_strip.csv"
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
next "topper -- herd breaks onto a border band, $TOP_N boards"
echo "Two passes over the top band: a deep one that may spend breaks, then a"
echo "shallow one that may not, so the second cleans up after the first."
echo

head -n "$TOP_N" "$FIN_OUT" > 5_in.csv
python3 "$SRC/C_tail/E555_topper.py" "$SEED" 5_in.csv 5_deep.csv "${CLUE_ARG[@]}" \
    --start_row 0 --num_rows "$TOP_N" --side T --band_depth 6 --locked_rows 2 \
    --threads "$THREADS" --time_limit "$CPSAT_TIME" --stall_time "$CPSAT_STALL" --verbose
python3 "$SRC/C_tail/E555_topper.py" "$SEED" 5_deep.csv 5_corners.csv "${CLUE_ARG[@]}" \
    --start_row 0 --num_rows "$TOP_N" --side T --band_depth 4 --locked_rows 0 \
    --threads "$THREADS" --time_limit "$CPSAT_TIME" --stall_time "$CPSAT_STALL" --verbose
rm -f 5_in.csv 5_deep.csv
rank 5_corners.csv
echo
echo ">> $(rows 5_corners.csv) boards, best $(best 5_corners.csv)/480 -> 5_corners.csv"
show 5_corners.csv

# -----------------------------------------------------------------------------
next "ender -- ring sweep, then patch"
echo "The stages above leave the damage in and around the border ring, which is"
echo "exactly what the ring sweep re-threads. The patch pass then runs a"
echo "localized LNS on whatever break the sweep left."
echo

python3 "$SRC/C_tail/E555_ender.py" "$SEED" 5_corners.csv 6_rung.csv "${CLUE_ARG[@]}" \
    --mode ring --reach "$ENDER_REACH" --max_changes "$ENDER_CHANGES" \
    --threads "$THREADS" \
    --time_limit "$((CPSAT_TIME*2))" --stall_time "$CPSAT_STALL" --verbose
python3 "$SRC/C_tail/E555_ender.py" "$SEED" 6_rung.csv 6_patched.csv "${CLUE_ARG[@]}" \
    --reach "$ENDER_REACH" --max_changes "$ENDER_CHANGES" --threads "$THREADS" \
    --time_limit "$((CPSAT_TIME*3))" --stall_time "$((CPSAT_STALL*2))" --verbose
rank 6_rung.csv; rank 6_patched.csv
echo
echo ">> ring sweep best $(best 6_rung.csv)/480, patch best $(best 6_patched.csv)/480"
show 6_patched.csv

# -----------------------------------------------------------------------------
next "backtracker -- greedy dives on the best board"
cat 5_corners.csv 6_rung.csv 6_patched.csv > all_candidates.csv
rank all_candidates.csv
head -1 all_candidates.csv > 7_best_input.csv
rm -f all_candidates.csv

echo "Best board so far: $(best 7_best_input.csv)/480 correct edges."
echo "Reopening the cells in $(basename "$HOLES") and diving on them"
echo "($BT_RESTARTS randomized greedy dives, keeping the best)."
echo

"$BIN/E555_backtracker" "$SEED" 7_best_input.csv 7_backtracker.csv \
    --start_row 0 --num_rows 1 --threads "$THREADS" \
    --holes "$HOLES" --order mrv --break_mode stuck \
    --breaks "$BT_MISMATCH" --restarts "$BT_RESTARTS" \
    --time_limit "$BT_TIME" --print_cmd --verbose

# -----------------------------------------------------------------------------
echo "[time] stage $STAGE took $(( SECONDS - STAGE_T0 ))s"
banner "PIPELINE COMPLETE  ($(( (SECONDS-START)/60 )) min $(( (SECONDS-START)%60 )) s)"
# -----------------------------------------------------------------------------
cat 7_best_input.csv 7_backtracker.csv > final_candidates.csv
rank final_candidates.csv
head -1 final_candidates.csv > FINAL_best.csv
rm -f final_candidates.csv

echo "Files kept in $PWD:"
ls -1 [0-9]_*.csv FINAL_best.csv | sed 's/^/  /'
echo
echo "Best board of the whole run: $(best FINAL_best.csv)/480 correct edges."
show FINAL_best.csv
echo "Re-feed it to any Stage C tool, or open the bucas URL printed above."
