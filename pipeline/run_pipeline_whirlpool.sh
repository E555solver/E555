#!/bin/bash
##SBATCH --job-name=E555_whirlpool
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=12G --time=24:00:00
##SBATCH --output=logs/whirlpool_%j.out
#
# =============================================================================
# run_pipeline_whirlpool.sh -- turn the board between every re-grow
# =============================================================================
# THIS IS NOT AN EXAMPLE. Like the other runners here it is meant to be started
# and left alone. Read ../examples/ first if you have not used the tools singly.
#
# THE IDEA
#   Stage B only ever grows ROWS UPWARD FROM THE BOTTOM. Every tool in it is
#   built that way: the beam advances a row at a time, and the finalizer locks
#   rows 0..N and frees everything above. So the rows a board is standing on
#   were chosen early, by a beam that was guessing, and are then never revisited
#   -- however many times you re-grow the top.
#
#   Turn the board 90 degrees and those buried rows become COLUMNS on one side,
#   where a re-grow can reach them. The obstacle was that a turned board has
#   complete columns and the finalizer can only start from complete ROWS, and
#   nothing could convert one into the other. E555_backtracker --stop_row can:
#   it searches rows 0..N only and emits every exact way to fill them.
#
#   That gives a lap:
#
#     rows 0..T full
#       -> rotate +-90    T+1 complete COLUMNS (new row 0 is an old column,
#                         so it is incomplete and no finalizer could start here)
#       -> backtracker    --stop_row 5 --order rowmajor: complete rows 0..5,
#                         clear everything above
#       -> finalizer      --finalize_from 5 --stop_row T: re-grow rows 6..T
#                         at full width over a database rebuilt without the
#                         locked pieces
#
#   and the lap ends where it started -- rows 0..T full -- but rebuilt from a
#   different direction. Four laps is one full turn of the board.
#
# WHY IT IS WORTH THE TIME
#   The lap keeps rows 0..5 of the turned board, which is a six-deep slab
#   against ONE side, and that side moves 90 degrees every lap. So 184 of the
#   256 pieces are freed and re-searched each lap; the four slabs hug four
#   different sides and their common intersection is empty, so no piece
#   survives a full circle untouched. The centre is re-searched every lap, a
#   corner twice in four. A board that comes out the far end is one that admits
#   an exact rows-0..T partial cut from every direction -- much stronger
#   evidence than surviving once from the bottom.
#
# WHAT THE LOOP DOES *NOT* DO
#   It does not climb. WHIRL_ROWS caps at 12 on purpose: above row 12 the beam
#   is spent and the Stage C tools are simply better, so the loop holds its
#   depth and spends its time on coverage instead. Stage C runs once, at the
#   end, on the survivors.
#
#   Nothing here ranks: every stage in the loop emits EXACTLY MATCHED boards, so
#   at equal depth they all score the same and there is nothing to sort on.
#   What thins the field is attrition -- a board whose turned band admits no
#   exact filling, or that will not re-grow to WHIRL_ROW, drops out. The
#   per-lap counts printed below are therefore the real diagnostic, and a lap
#   that returns what it was given means the neighbourhood is exhausted.
#
# Everything lands in $RUN_DIR. Override any setting from the environment:
#
#   RUN_DIR=my_whirl LAPS=4 THREADS=16 bash pipeline/run_pipeline_whirlpool.sh
# =============================================================================
set -euo pipefail

# Derived from this script's own location, so the runner works from anywhere.
# Overridable because a copy of this file kept outside the tree -- a snapshot
# pinned for a long run, say -- would otherwise resolve the repo to nonsense.
REPO="${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# ---- what to run on ---------------------------------------------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
RUN_DIR="${RUN_DIR:-$PWD/whirlpool_out}"
THREADS="${THREADS:-$(nproc 2>/dev/null || echo 4)}"
DB_FILE="${DB_FILE:-}"              # cache the 6.4 GB chain DB; empty = in memory
CLUES="${CLUES:-0}"                 # 1 = hold the Eternity II centre clue

# Clues are rotation-covariant, which is what makes the loop legal: g_clue in
# E555_database.c tabulates the published clues at ALL FOUR orientations (the
# centre piece 138 at (7,7) (8,7) (8,8) (7,8), spins 0 3 2 1) and g_clue_orients
# is 0xF, so a quarter-turn maps a satisfied configuration to another satisfied
# one. Only --clue_center is passed: the centre-only record is the higher one.
#
# CLUES=1 REQUIRES BAND_ROW >= 8. The finalizer reads a partial's orientation
# off the clue pieces it carries and skips any line carrying none, and the
# centre clue sits on row 7 or 8 -- so a rows-0..5 band strips it and every
# band is refused (measured: 26 bands in, 0 out, every lap). The guard below
# enforces it rather than letting a run burn hours emitting nothing.
CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center)
if [ "$CLUES" = 1 ] && [ "${BAND_ROW:-5}" -lt 8 ]; then
    echo "!! CLUES=1 needs BAND_ROW >= 8: the centre clue sits on row 7/8, so a"
    echo "   shallower band strips it and the finalizer refuses every partial"
    echo "   ('carries none of the enabled clue pieces'). Set BAND_ROW=8, or"
    echo "   run with CLUES=0."
    exit 1
fi
# A clued chain database has different CONTENTS for the same seed, and the
# beamer refuses a cache whose exclusion set does not match.
[ "$CLUES" = 1 ] && [ -n "$DB_FILE" ] && DB_FILE="$DB_FILE.clue"
RNG_SEED="${RNG_SEED:-0}"           # 0 = clock+pid

# ---- stage 0: beamer, one board supply --------------------------------------
# INPUT skips the beamer entirely and whirls boards you already have. They must
# have whole rows 0..N filled -- the loop turns rows into columns, so a board
# with a ragged top has nothing to turn.
INPUT="${INPUT:-}"
BEAM_WIDTH="${BEAM_WIDTH:-200000}"
BEAM_STOP="${BEAM_STOP:-10}"        # last row the beamer fills; the whirlpool takes over here
BEAM_BOARDS="${BEAM_BOARDS:-60}"
BEAM_WALL="${BEAM_WALL:-900}"
BEAM_COLUMNS="${BEAM_COLUMNS:-8}"
BEAM_TAU_BOTTOMS="${BEAM_TAU_BOTTOMS:-2}"
BEAM_TAU_COLUMNS="${BEAM_TAU_COLUMNS:-4}"
BEAM_TAU0="${BEAM_TAU0:-0.25}"
BEAM_TAU1="${BEAM_TAU1:-0.25}"

# ---- the whirlpool ----------------------------------------------------------
# One target row per lap. 11 first, then 12 -- and 12 is the ceiling, reached
# with --incomplete_top because a board that fills row 12 bar one 5-piece
# segment is still the best thing Stage C will be handed all day.
WHIRL_ROWS="${WHIRL_ROWS:-11 12 12 12}"
BAND_ROW="${BAND_ROW:-5}"           # backtracker --stop_row: rows 0..BAND_ROW are rebuilt exactly
# Bands per lap is 2 * POP * BT_LIMIT, and it is the setting that decides how
# long a lap takes: every distinct band is a distinct locked set, so the
# finalizer rebuilds its reduced database for each one. Measured at
# --finalize_from 5 on the synthetic seed: 337 M records, 0.82 GB, ~9 s a band.
# Locking only six rows is what makes that database big -- raise BAND_ROW and it
# shrinks fast, at the cost of freeing less of the board per lap.
BT_LIMIT="${BT_LIMIT:-3}"           # bands emitted per turned board (--solution-limit)
# --solution-limit is load-bearing, not a safety net. Measured on one turned
# synthetic board: 4 787 556 exact bands in 120 s (6.9 GB of CSV) and still
# running when the clock stopped. The cut is effectively free and the finalizer
# is the whole cost, so the loop consumes a vanishing fraction of what is on
# offer -- and BT_LIMIT takes the DFS's FIRST K, which share a long prefix.
# BT_ORDER is the lever on that: a static order starts from a different corner
# and so returns a different first K. rowmajor walks the empty columns row by
# row and closes the border row 0 first, which is the shape the lock needs.
BT_ORDER="${BT_ORDER:-rowmajor}"
BT_TIME="${BT_TIME:-60}"            # seconds per turned board
POP="${POP:-40}"                    # boards carried into the next lap
FIN_WIDTH="${FIN_WIDTH:-100000}"
FIN_BOARDS="${FIN_BOARDS:-60}"      # boards the finalizer may write per lap
FIN_WALL="${FIN_WALL:-900}"         # seconds per lap
FIN_COLUMNS="${FIN_COLUMNS:-4}"     # left columns sampled per partial (0 = enumerate all)

# ---- stage C: once, at the end ----------------------------------------------
RH_WIDTH="${RH_WIDTH:-4}"
RH_ROUNDS="${RH_ROUNDS:-3}"
RH_ROTATE="${RH_ROTATE:--1}"        # -1 cuts the bottom band first
RH_LINES="${RH_LINES:-20}"
RH_WALL="${RH_WALL:-600}"
# The backtracker is not clue-aware, so the mask matters when CLUES=1: this one
# frees the right and top border, never the centre cell, so the centre clue
# cannot be displaced by the close.
HOLES="${HOLES:-$REPO/data/holes_open_border_TR.csv}"
BT_MISMATCH="${BT_MISMATCH:-30}"
BT_RESTARTS="${BT_RESTARTS:-200000}"
BT_CLOSE_TIME="${BT_CLOSE_TIME:-300}"
TOP_N="${TOP_N:-100}"               # rows in the delivered ranking
# =============================================================================

banner()     { echo; echo "==============================================================="; echo "  $*"; echo "==============================================================="; echo; }
show_board() { echo; python3 "$REPO/tools/E555_viewer.py" "$1" --seed "$SEED" --no-url --row 0; echo; }
# grep -c exits 1 on a zero count, so the usual `|| echo 0` idiom can print 0
# TWICE -- once from grep, once from the fallback. The lap counts below are
# compared numerically, so this one has to yield exactly one number.
rows()       { local n; n=$(grep -cv '^ *#' "$1" 2>/dev/null) || n=0; echo "${n:-0}"; }
best()       { cut -d, -f2 "$1" | head -1 | tr -d ' '; }
rank() {
    [ -s "$1" ] || return 0
    python3 "$REPO/tools/E555_rank.py" "$1" --seed "$SEED" \
        --sort breaks,break_rows --emit "$1.tmp" --rescore > /dev/null \
        && mv "$1.tmp" "$1"
    rm -f "$1.tmp"
}

[ -x "$REPO/bin/E555_beamer" ] || make -C "$REPO"
# Everything below runs from inside $RUN_DIR, so any path handed in relative to
# the invocation directory has to be resolved before the cd.
abspath() { case "$1" in /*) printf '%s' "$1";; *) printf '%s' "$PWD/$1";; esac; }
SEED="$(abspath "$SEED")"
HOLES="$(abspath "$HOLES")"
[ -n "$INPUT" ] && INPUT="$(abspath "$INPUT")"
[ -n "$DB_FILE" ] && DB_FILE="$(abspath "$DB_FILE")"
[ -r "$SEED" ] || { echo "!! seed not readable: $SEED"; exit 1; }
mkdir -p "$RUN_DIR"
cd "$RUN_DIR"
echo "[cfg] repo=$REPO"
echo "[cfg] seed=$SEED"
echo "[cfg] run_dir=$RUN_DIR threads=$THREADS db_file=${DB_FILE:-<in memory>} clues=$CLUES"
echo "[cfg] whirlpool: laps=[$WHIRL_ROWS] band_row=$BAND_ROW bt_limit=$BT_LIMIT pop=$POP"
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

BEAM_CMD=("$REPO/bin/E555_beamer" "$SEED" --random_edges
    --out_dir beam --threads "$THREADS"
    --beam_width "$BEAM_WIDTH" --stop_row "$BEAM_STOP"
    --border_row_N "$BEAM_BOARDS" --top_columns "$BEAM_COLUMNS"
    --gumbel_tau_bottoms "$BEAM_TAU_BOTTOMS" --gumbel_tau_columns "$BEAM_TAU_COLUMNS"
    --gumbel_tau0 "$BEAM_TAU0" --gumbel_tau1 "$BEAM_TAU1"
    --lambda_Mahalanobis 10
    --max_partials "$BEAM_BOARDS" --max_wall_sec "$BEAM_WALL" --verbose "${CLUE_ARG[@]}")
[ -n "$DB_FILE" ] && BEAM_CMD+=(--db_file "$DB_FILE")
[ "$RNG_SEED" != "0" ] && BEAM_CMD+=(--seed "$RNG_SEED")
"${BEAM_CMD[@]}"

cp beam/beam_completions_random_"$BEAM_STOP".csv 0_beam.csv 2>/dev/null || true
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
    local k="$1" target="$2" inc=() nrot nband nout nin
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
    python3 "$REPO/tools/E555_rotate.py" "$CURRENT" 1 --out "${k}_cw.csv"  --seed "$SEED" > /dev/null
    python3 "$REPO/tools/E555_rotate.py" "$CURRENT" 3 --out "${k}_ccw.csv" --seed "$SEED" > /dev/null
    cat "${k}_cw.csv" "${k}_ccw.csv" > "${k}_rot.csv"
    rm -f "${k}_cw.csv" "${k}_ccw.csv"
    nrot=$(rows "${k}_rot.csv")
    echo "-- turned $(rows "$CURRENT") boards both ways -> $nrot"

    # --- re-cut rows 0..BAND_ROW exactly -------------------------------------
    # $BT_ORDER (rowmajor) because the empty cells are whole COLUMNS of rows
    # 0..BAND_ROW: rowmajor walks them row by row and closes the border row 0
    # first, which is exactly the shape the finalizer's lock needs. --break-mode
    # any because the default (stuck) takes a minimal break where no exact fit
    # exists, and a broken band is dropped at emission.
    "$REPO/bin/E555_backtracker" "$SEED" "${k}_rot.csv" "${k}_bt.csv" \
        --stop_row "$BAND_ROW" --order "$BT_ORDER" --break-mode any \
        --max-mismatch 0 --solution-limit "$BT_LIMIT" \
        --time-limit "$BT_TIME" --threads "$THREADS" > "${k}_bt.log" 2>&1 || true
    if [ -s "${k}_bt.csv.stop_row${BAND_ROW}.csv" ]; then
        mv "${k}_bt.csv.stop_row${BAND_ROW}.csv" "${k}_band.csv"
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
    "$REPO/bin/E555_finalizer" "$SEED" "${k}_band.csv" \
        --out_dir "lap$k" --threads "$THREADS" \
        --finalize_from "$BAND_ROW" --finalize_repeats 1 \
        --beam_width "$FIN_WIDTH" --stop_row "$target" \
        --border_row_N "$nband" --top_columns "$FIN_COLUMNS" \
        --score_model J --frac_rand 0.0 \
        --max_partials "$FIN_BOARDS" --max_wall_sec "$FIN_WALL" \
        --verbose "${inc[@]}" "${CLUE_ARG[@]}" > "${k}_fin.log" 2>&1 || true

    cat "lap$k/beam_completions_finalized_$target.csv"          > "${k}_final.csv" 2>/dev/null || true
    cat "lap$k/beam_completions_finalized_${target}_partial.csv" >> "${k}_final.csv" 2>/dev/null || true
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

"$REPO/bin/E555_roundhouse" "$SEED" "$CURRENT" \
    --out_dir strip --threads "$THREADS" \
    --rounds "$RH_ROUNDS" --strip_width "$RH_WIDTH" --rotate "$RH_ROTATE" \
    --border_row_N "$RH_LINES" --max_wall_sec "$RH_WALL" --verbose "${CLUE_ARG[@]}" || true

# First existing match, or empty. NOT `$(ls GLOB | head -1)`: an unmatched glob
# makes ls exit 2, pipefail promotes it, and set -e kills the run -- which is
# exactly the "emitted nothing" case the else branch exists to report.
RH_OUT=""
for f in strip/roundhouse_*_miss0.csv; do [ -e "$f" ] && { RH_OUT="$f"; break; }; done
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

"$REPO/bin/E555_backtracker" "$SEED" C2_in.csv C2_dived.csv \
    --row 0 --count 1 --threads "$THREADS" \
    --holes "$HOLES" --order mrv --break-mode stuck \
    --max-mismatch "$BT_MISMATCH" --stuck_restarts "$BT_RESTARTS" \
    --time-limit "$BT_CLOSE_TIME" --verbose || true

# -----------------------------------------------------------------------------
banner "PIPELINE COMPLETE  ($(( (SECONDS-START)/60 )) min $(( (SECONDS-START)%60 )) s)"
# -----------------------------------------------------------------------------
cat C2_in.csv C2_dived.csv "$CURRENT" > final_pool.csv 2>/dev/null || cp C2_in.csv final_pool.csv
rank final_pool.csv
python3 "$REPO/tools/E555_rank.py" final_pool.csv --seed "$SEED" \
    --sort breaks,break_rows --top "$TOP_N" --emit FINAL_top"$TOP_N".csv --rescore > /dev/null
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
python3 "$REPO/tools/E555_rank.py" FINAL_top"$TOP_N".csv --seed "$SEED" --top 20 --no-id
