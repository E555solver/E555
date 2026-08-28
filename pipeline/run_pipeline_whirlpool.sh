#!/bin/bash
# run_pipeline_whirlpool.sh -- turn the board, re-cut its base exactly, re-grow.
#
#   bash pipeline/run_pipeline_whirlpool.sh
#   bash pipeline/run_pipeline_whirlpool.sh INPUT=boards.csv RUN_DIR=whirl1
#   bash pipeline/run_pipeline_whirlpool.sh WHIRL_ROWS=10        # one lap only
#
# A lap turns every board a quarter turn, rebuilds rows 0..BAND_ROW EXACTLY with
# the backtracker, and re-grows above them with the finalizer. Turning is what
# makes it work: rows the beam fixed by sampling at row 0 and never revisited
# become the rows a later lap re-cuts. WHIRL_ROWS lists one target row per lap,
# so a single entry runs a single lap -- which is the way to try one by hand.
#
# NEEDS ~8 GB RAM for stage 0, and `pip install ortools` for the CP-SAT stages.
# Lap geometry, BAND_ROW, FIXED_BORDER and the measured evidence behind the
# defaults: pipeline/README.md
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
RUN_DIR=whirlpool_out
THREADS=8
DB_FILE=                                # cache the 6.4 GB chain DB here
INPUT=                                  # skip stage 0 and whirl these boards
CLUES=0                                 # 1 = hold the Eternity II clue pieces

# stage 0: beamer, one board supply
BEAM_WIDTH=200000
BEAM_STOP=10            # the whirlpool takes over here
BEAM_BOARDS=60
BEAM_WALL=900
BEAM_COLUMNS=8
BEAM_TAU_BOTTOMS=2      # sample the borders instead of taking the best-ranked:
BEAM_TAU_COLUMNS=4      # yield is a property of the PAIR, so this costs nothing
BEAM_FRAC_RAND=0.10     # and spreads the run over many more frames
BC_WINDOW=3,3           # score up to nB x nC completions per segment-A record

# the whirlpool: one target row per lap. Ten, not twelve -- twelve is not
# reachable from a five-row lock, measured against a perfect 480 board on its
# own true frame: 7491 completions at row 10, ten at row 11, NONE at row 12.
WHIRL_ROWS="10 10 10 10"
BAND_ROW=5              # backtracker --stop_row: rows 0..BAND_ROW rebuilt exactly
FIXED_BORDER=1          # 1 = --with_frame, so the band carries all 60 frame cells
BT_LIMIT=200            # bands ENUMERATED per turned board (--max_emitted)
BT_PICK=6               # bands actually GROWN, chosen farthest-first
BT_ORDER=rowmajor
BT_TIME=60              # seconds per turned board
POP=40                  # boards carried into the next lap
FIN_WIDTH=100000
FIN_BOARDS=60           # boards the finalizer may write per lap
FIN_WALL=900            # seconds per lap
FIN_COLUMNS=4           # left columns sampled per partial (0 = enumerate all)

# stage C: once, at the end
RH_WIDTH=4
RH_ROUNDS=3
RH_ROTATE=-1            # -1 cuts the bottom band first
RH_LINES=20
RH_WALL=600
BT_MISMATCH=30
BT_RESTARTS=200000
BT_FINAL_TIME=300
TOP_N=50                # boards kept in the final ranking
HOLES=data/holes_open_border_TRL.csv
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
[ -x bin/E555_beamer ] || make

# The beamer already refuses a stop row outside 1..13, so there is nothing to
# re-check here. What it cannot know is that a SHALLOW row floods the disk:
# nothing has gone extinct that early, so the whole stop-row beam is emitted and
# --incomplete_top multiplies it. Measured on the real seed, one config: row 6
# wrote 807042 boards and 1.5 GB where row 10 wrote 1136 and 2.2 MB, in the same
# 5 s. Worth warning about; not worth forbidding.
for t in $BEAM_STOP $WHIRL_ROWS; do
    [ "$t" -ge 10 ] || echo "[warn] stop row $t is below 10: nothing goes extinct that"\
        "shallow, so the whole beam is emitted and the output floods."
    [ "$t" -le 12 ] || echo "[warn] stop row $t is above 12: the beam is spent past 12"\
        "and the Stage C tools do better there."
done

banner() { echo; echo "==============================================================="
           echo "  $*"; echo "==============================================================="; echo; }
rows()   { python3 "$TOOLS/E555_rank.py" "$1" --count; }
best()   { python3 "$TOOLS/E555_rank.py" "$1" --seed_file "$SEED" --field score; }
show_board() { echo; python3 "$TOOLS/E555_viewer.py" "$1" --seed_file "$SEED" --no_url --row 0; echo; }

# rank FILE -- rewrite a board CSV in place, best board first, field 2 rescored
# to the true matched-edge count so "best so far" is comparable across stages.
rank() {
    [ -s "$1" ] || return 0
    python3 "$TOOLS/E555_rank.py" "$1" --seed_file "$SEED" \
        --sort breaks,break_rows --out "$1.tmp" --rescore > /dev/null
    mv "$1.tmp" "$1"
}

# Paths are resolved against the repo before the cd into RUN_DIR below.
abs() { case "$1" in /*) printf '%s' "$1";; *) printf '%s' "$PWD/$1";; esac; }
SEED=$(abs "$SEED"); HOLES=$(abs "$HOLES")
[ -n "$INPUT" ] && INPUT=$(abs "$INPUT")
[ -n "$DB_FILE" ] && DB_FILE=$(abs "$DB_FILE")
TOOLS=$PWD/tools; SRC=$PWD/src; BIN=$PWD/bin
mkdir -p "$RUN_DIR"; cd "$RUN_DIR"

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
DB_ARG=();   [ -n "$DB_FILE" ] && DB_ARG=(--db_file "$DB_FILE")

echo "[cfg] repo=$REPO  seed=$SEED"
echo "[cfg] run_dir=$PWD threads=$THREADS db_file=${DB_FILE:-<in memory>} clues=$CLUES"
echo "[cfg] whirlpool: laps=[$WHIRL_ROWS] band_row=$BAND_ROW bt_limit=$BT_LIMIT bt_pick=$BT_PICK pop=$POP"
echo "[cfg] fixed_border=$FIXED_BORDER"
START=$SECONDS

# -----------------------------------------------------------------------------
if [ -n "$INPUT" ]; then
banner "STAGE 0  skipped -- whirling the boards in $INPUT"
cp "$INPUT" 0_beam.csv
rank 0_beam.csv
head -n "$POP" 0_beam.csv > 0_pop.csv; mv 0_pop.csv 0_beam.csv
CURRENT=0_beam.csv
echo ">> $(rows 0_beam.csv) boards -> 0_beam.csv"
else
banner "STAGE 0  beamer --random_edges, beam $BEAM_WIDTH up to row $BEAM_STOP"
# -----------------------------------------------------------------------------
echo "Borders are sampled from the seed, so nothing has to be prepared. The"
echo "first run builds the 6.4 GB chain database: expect a few quiet minutes."
echo "Completions only -- the whirlpool needs whole rows to turn."
echo

BEAM_CMD=("$BIN/E555_beamer" "$SEED" --random_edges
    --out_dir beam --threads "$THREADS"
    --beam_width "$BEAM_WIDTH" --stop_row "$BEAM_STOP"
    --samples "$BEAM_BOARDS" --top_columns "$BEAM_COLUMNS"
    --tau_bottoms "$BEAM_TAU_BOTTOMS" --tau_columns "$BEAM_TAU_COLUMNS"
    --frac_rand "$BEAM_FRAC_RAND"
    --bc_window "$BC_WINDOW"
    --max_emitted "$BEAM_BOARDS" --wall_time "$BEAM_WALL" --verbose "${CLUE_ARG[@]}")
[ -n "$DB_FILE" ] && BEAM_CMD+=(--db_file "$DB_FILE")
[ "$RNG_SEED" != "0" ] && BEAM_CMD+=(--rng_seed "$RNG_SEED")
"${BEAM_CMD[@]}"

xargs -r cat < beam/outputs.txt > 0_beam.csv
rm -rf beam
[ -s 0_beam.csv ] || { echo "!! no board reached row $BEAM_STOP -- raise BEAM_WALL or lower BEAM_STOP"; exit 1; }
rank 0_beam.csv
head -n "$POP" 0_beam.csv > 0_pop.csv; mv 0_pop.csv 0_beam.csv
CURRENT=0_beam.csv
echo
echo ">> $(rows 0_beam.csv) boards with rows 0..$BEAM_STOP full -> 0_beam.csv"
fi
show_board 0_beam.csv

# -----------------------------------------------------------------------------
# One lap. $1 = lap number, $2 = the row the finalizer must reach.
# Sets CURRENT to the lap's output, or leaves it alone if the lap emitted
# nothing -- an empty lap is usually a real answer about the boards, not a crash.
# -----------------------------------------------------------------------------
lap() {
    local k="$1" target="$2" inc=() frame_arg=() nrot nband nout nin
    [ "$FIXED_BORDER" = 1 ] && frame_arg=(--with_frame)
    # A lap that emits nothing leaves no file, and `rows` on a missing file is
    # 0 -- but only if a previous run into this same RUN_DIR did not leave one.
    rm -f "${k}_band.csv" "${k}_final.csv" "${k}_rot.csv"
    nin=$(rows "$CURRENT")
    [ "$target" -ge 12 ] && inc=(--incomplete_top)

    banner "LAP $k/$LAPS  turn, re-cut rows 0..$BAND_ROW, re-grow to row $target"

    # --- turn it both ways ---------------------------------------------------
    # 1 (CW) sends the old RIGHT column down to the new bottom row and leaves
    # the filled region on columns 0..T; 3 (CCW) takes the old LEFT column down
    # instead and fills columns 15-T..15. The two keep different halves of the
    # board, so this genuinely doubles the field rather than mirroring it.
    python3 "$TOOLS/E555_rotate.py" "$CURRENT" 1 --out "${k}_cw.csv"  --seed_file "$SEED" > /dev/null
    python3 "$TOOLS/E555_rotate.py" "$CURRENT" 3 --out "${k}_ccw.csv" --seed_file "$SEED" > /dev/null
    cat "${k}_cw.csv" "${k}_ccw.csv" > "${k}_rot.csv"
    rm -f "${k}_cw.csv" "${k}_ccw.csv"
    nrot=$(rows "${k}_rot.csv")
    echo "-- turned $(rows "$CURRENT") boards both ways -> $nrot"

    # --- re-cut rows 0..BAND_ROW exactly -------------------------------------
    # $BT_ORDER (rowmajor) because the empty cells are whole COLUMNS of rows
    # 0..BAND_ROW: rowmajor walks them row by row and closes the border row 0
    # first, which is exactly the shape the finalizer's lock needs. --break_mode
    # any because the default (stuck) takes a minimal break where no exact fit
    # exists, and a broken band is dropped at emission.
    "$BIN/E555_backtracker" "$SEED" "${k}_rot.csv" "${k}_bt.csv" \
        --stop_row "$BAND_ROW" --order "$BT_ORDER" --break_mode any \
        "${frame_arg[@]}" \
        --breaks 0 --max_emitted "$BT_LIMIT" \
        --time_limit "$BT_TIME" --threads "$THREADS" > "${k}_bt.log" 2>&1 ||
        echo "[warn] lap $k: the backtracker exited non-zero, see ${k}_bt.log"
    if [ -s "${k}_bt.csv.stop_row${BAND_ROW}.csv" ]; then
        mv "${k}_bt.csv.stop_row${BAND_ROW}.csv" "${k}_band.csv"
    fi
    # Thin the enumeration down to BT_PICK bands the finalizer will actually
    # grow. Farthest-first on cell agreement, because the DFS returns its first
    # K sharing a long prefix and exact bands are all the same score -- there is
    # nothing else to choose on. Skipped when the cut returned few enough anyway.
    if [ -s "${k}_band.csv" ] && [ "$(rows "${k}_band.csv")" -gt "$BT_PICK" ]; then
        python3 "$TOOLS/E555_rank.py" "${k}_band.csv" --seed_file "$SEED" \
            --diverse "$BT_PICK" --out "${k}_band.tmp" > /dev/null 2>&1 \
            && mv "${k}_band.tmp" "${k}_band.csv"
        rm -f "${k}_band.tmp"
    fi
    rm -f "${k}_bt.csv" "${k}_bt.csv".*
    nband=$(rows "${k}_band.csv")
    echo "-- backtracker emitted $nband exact bands (rows 0..$BAND_ROW)"
    if [ "$nband" = 0 ]; then
        echo ">> LAP $k emitted no band. Every turned board's rows 0..$BAND_ROW"
        echo "   are unfillable from the pieces above them -- a real answer."
        echo "   Carrying the previous boards forward."
        return 0
    fi

    # --- re-grow rows BAND_ROW+1 .. target -----------------------------------
    # --lambda_Mahalanobis 0 is deliberate and specific to this lap: the band
    # below was rebuilt exactly by the backtracker, so the closure objective is
    # the only one with anything left to say about it. --frac_rand 0.0, though,
    # is the same unmeasured holdover as in the other two pipelines -- see the
    # note in run_pipeline.sh.
    "$BIN/E555_finalizer" "$SEED" "${k}_band.csv" \
        --out_dir "lap$k" --threads "$THREADS" \
        --finalize_from "$BAND_ROW" --finalize_repeats 1 \
        --beam_width "$FIN_WIDTH" --stop_row "$target" \
        --num_rows "$nband" --top_columns "$FIN_COLUMNS" \
        --lambda_Mahalanobis 0 --frac_rand 0.0 \
        --max_emitted "$FIN_BOARDS" --wall_time "$FIN_WALL" \
        --print_cmd --verbose "${inc[@]}" "${CLUE_ARG[@]}" > "${k}_fin.log" 2>&1 ||
        echo "[warn] lap $k: the finalizer exited non-zero, see ${k}_fin.log"

    xargs -r cat < "lap$k/outputs.txt" > "${k}_final.csv"
    rm -rf "lap$k"

    nout=$(rows "${k}_final.csv")
    if [ "$nout" = 0 ]; then
        rm -f "${k}_final.csv"
        echo ">> LAP $k: $nband bands, none re-grew to row $target."
        echo "   Carrying the previous boards forward."
        return 0
    fi

    rank "${k}_final.csv"
    head -n "$POP" "${k}_final.csv" > "${k}_pop.csv"; mv "${k}_pop.csv" "${k}_final.csv"
    CURRENT="${k}_final.csv"
    LAP_LOG+=("$(printf 'lap %-2s -> row %-3s  in %-4s turned %-4s bands %-5s out %-4s kept %-4s  score %s' \
                 "$k" "$target" "$nin" "$nrot" "$nband" "$nout" "$(rows "${k}_final.csv")" \
                 "$(best "${k}_final.csv")")")
    echo
    echo ">> LAP $k: $nout boards with rows 0..$target full, best $(best "${k}_final.csv")/480 -> ${k}_final.csv"
    show_board "${k}_final.csv"
}

LAPS=$(echo "$WHIRL_ROWS" | wc -w)
LAP_LOG=()
k=0
for target in $WHIRL_ROWS; do
    k=$((k + 1))
    lap "$k" "$target"
done

banner "WHIRLPOOL DONE -- $LAPS laps, $(( (SECONDS-START)/60 )) min"
if [ "${#LAP_LOG[@]}" -gt 0 ]; then
    printf '[whirl] %s\n' "${LAP_LOG[@]}"
else
    echo "[whirl] no lap completed; Stage C runs on the beamer's boards"
fi
echo "[whirl] carrying $(rows "$CURRENT") boards into Stage C from $CURRENT"

# -----------------------------------------------------------------------------
banner "STAGE C 1/2  roundhouse -- rotate $RH_ROTATE, $RH_ROUNDS rounds, width $RH_WIDTH"
# -----------------------------------------------------------------------------
echo "Three rounds rebuild three sides and all four corners, which nothing in"
echo "the loop does. Raising the width past the default frees already-solved"
echo "rows on purpose. A completion here would be a solved puzzle; a refusal is"
echo "a proof, delivered in milliseconds."
echo

"$BIN/E555_roundhouse" "$SEED" "$CURRENT" \
    --out_dir strip --threads "$THREADS" \
    --rounds "$RH_ROUNDS" --strip_width "$RH_WIDTH" --rotate "$RH_ROTATE" \
    --num_rows "$RH_LINES" --wall_time "$RH_WALL" --print_cmd --verbose "${CLUE_ARG[@]}"

# First existing match, or empty. NOT `$(ls GLOB | head -1)`: an unmatched glob
# makes ls exit 2, pipefail promotes it, and set -e kills the run -- which is
# exactly the "emitted nothing" case the else branch exists to report.
RH_OUT=$(head -1 strip/outputs.txt)
if [ -n "$RH_OUT" ] && [ -s "$RH_OUT" ]; then
    cp "$RH_OUT" C1_strip.csv
    rank C1_strip.csv
    echo
    echo ">> $(rows C1_strip.csv) boards, best $(best C1_strip.csv)/480 -> C1_strip.csv"
    cat "$CURRENT" C1_strip.csv > C1_pool.csv
    rank C1_pool.csv
    CURRENT=C1_pool.csv
else
    echo
    echo ">> the roundhouse emitted nothing: no legal strip start survived its"
    echo "   oracle. Try another RH_ROTATE or a larger RH_WIDTH next run."
fi
rm -rf strip

# -----------------------------------------------------------------------------
banner "STAGE C 2/2  backtracker -- dives with up to $BT_MISMATCH mismatches"
# -----------------------------------------------------------------------------
head -1 "$CURRENT" > C2_in.csv
echo "Best so far: $(best C2_in.csv)/480. Reopening the cells of"
echo "$(basename "$HOLES") and running $BT_RESTARTS randomized dives on them."
echo "This is the first stage that may break an edge, and so the first point at"
echo "which the ranking means anything."
echo

"$BIN/E555_backtracker" "$SEED" C2_in.csv C2_dived.csv \
    --start_row 0 --num_rows 1 --threads "$THREADS" \
    --holes "$HOLES" --order mrv --break_mode stuck \
    --breaks "$BT_MISMATCH" --restarts "$BT_RESTARTS" \
    --time_limit "$BT_FINAL_TIME" --print_cmd --verbose

# -----------------------------------------------------------------------------
banner "PIPELINE COMPLETE  ($(( (SECONDS-START)/60 )) min $(( (SECONDS-START)%60 )) s)"
# -----------------------------------------------------------------------------
cat C2_in.csv C2_dived.csv "$CURRENT" > final_pool.csv 2>/dev/null || cp C2_in.csv final_pool.csv
rank final_pool.csv
python3 "$TOOLS/E555_rank.py" final_pool.csv --seed_file "$SEED" \
    --sort breaks,break_rows --top "$TOP_N" --out FINAL_top"$TOP_N".csv --rescore > /dev/null
head -1 final_pool.csv > FINAL_best.csv
rm -f final_pool.csv C2_in.csv

if [ "${#LAP_LOG[@]}" -gt 0 ]; then
    echo "Whirlpool yield:"
    printf '  %s\n' "${LAP_LOG[@]}"
    echo
fi
echo "Files kept in $RUN_DIR:"
ls -1 [0-9]_*.csv C[0-9]_*.csv FINAL_*.csv 2>/dev/null | sed 's/^/  /'
echo
echo "Best board of the whole run: $(best FINAL_best.csv)/480 correct edges."
show_board FINAL_best.csv
python3 "$TOOLS/E555_rank.py" FINAL_top"$TOP_N".csv --seed_file "$SEED" --top 20 --no_id
