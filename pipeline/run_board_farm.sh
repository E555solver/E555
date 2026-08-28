#!/bin/bash
# run_board_farm.sh -- run the pipeline for days, keep only the best boards.
#
#   bash pipeline/run_board_farm.sh
#   bash pipeline/run_board_farm.sh MAX_HOURS=48 THREADS=16 FARM_DIR=/data/farm
#
# NOT an example: start it once and leave it. Ctrl-C is safe at any time --
# champions.csv is rewritten after every iteration and never depends on the run
# directory surviving.
#
# The loop alternates two phases, because producing new boards and improving
# existing ones want opposite settings. Production is broad and shallow: a fresh
# random border every time. Every RESAMPLE_EVERY iterations it stops producing
# and resamples instead -- the finalizer and the roundhouse re-attack the
# current champions, pulling pieces from deep in the board up to the frontier.
# Alternating is what makes the farm climb rather than just accumulate.
#
# Watch:  farm.log (one line per iteration), champions.csv (the best KEEP, always
# current), records/ (a copy of every board that set a record, never deleted).
set -uo pipefail        # not -e: one failing iteration must not kill the farm

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # relative to REPO
FARM_DIR=farm                           # relative to where you START it
THREADS=8
MAX_HOURS=0             # 0 = run until you stop it
KEEP=200                # champions retained
RESAMPLE_EVERY=5        # produce N times, then resample once
RESAMPLE_TOP=10         # champions re-attacked in a resample phase

# Per-iteration budgets. Smaller = more iterations = more borders tried.
PASS_BEAM_WALL=600
PASS_FIN_WALL=420
PASS_CPSAT=45

# The chain database cache is the single biggest saving here: without it every
# iteration spends 2-3 minutes rebuilding the same 6.4 GB. Needs ~6.5 GB free
# disk. Set DB_FILE= (empty) to disable.
DB_FILE=chain.db        # relative to FARM_DIR
CLUES=0                 # 1 = hold the Eternity II clue pieces in place
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
case "$FARM_DIR" in /*) ;; *) FARM_DIR="$PWD/$FARM_DIR" ;; esac
cd "$REPO"
[ -d bin ] && [ -d tools ] ||
    { echo "REPO=$REPO is not an E555 checkout -- set REPO at the top" >&2; exit 1; }
[ -x bin/E555_beamer ] || make
SEED=$PWD/$SEED
mkdir -p "$FARM_DIR/records"
[ -n "$DB_FILE" ] && case "$DB_FILE" in /*) ;; *) DB_FILE="$FARM_DIR/$DB_FILE" ;; esac
CHAMPS="$FARM_DIR/champions.csv"
LOG="$FARM_DIR/farm.log"
touch "$CHAMPS"

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)

# score FILE -- matched edges of the best board, or 0 for an empty file.
# --field score reads the real board rather than trusting field 2, which the
# beam stages write a solution index into.
score() {
    [ -s "$1" ] || { echo 0; return; }
    python3 tools/E555_rank.py "$1" --seed_file "$SEED" --field score
}

# harvest FILE... -- merge boards into champions.csv, best first, keep KEEP.
harvest() {
    cat "$CHAMPS" "$@" > "$FARM_DIR/merged.csv" 2>/dev/null
    [ -s "$FARM_DIR/merged.csv" ] || { rm -f "$FARM_DIR/merged.csv"; return; }
    # --rescore makes field 2 the true matched-edge count whatever tool wrote
    # the line, and ranks best first: fewest breaks, then most compact.
    python3 tools/E555_rank.py "$FARM_DIR/merged.csv" --seed_file "$SEED" \
        --sort breaks,break_rows --out "$FARM_DIR/scored.csv" --rescore > /dev/null
    # drop exact duplicate boards (everything after the id), keep the best KEEP
    awk '!seen[substr($0, index($0, ","))]++' "$FARM_DIR/scored.csv" \
        | head -n "$KEEP" > "$CHAMPS"
    rm -f "$FARM_DIR/merged.csv" "$FARM_DIR/scored.csv"
}

# One resample phase: re-attack the current champions two ways. Both tools list
# what they wrote, so harvest is handed exact paths and never a glob.
resample() {  # RUN_DIR ITER
    local run=$1 iter=$2
    mkdir -p "$run"
    head -n "$RESAMPLE_TOP" "$CHAMPS" > "$run/in.csv"

    # Free five rows and re-grow them, three stochastic re-rolls each. This is
    # what the finalizer's --frac_rand default is for: repeated passes over one
    # board, where the random band is coverage rather than a tax.
    bin/E555_finalizer "$SEED" "$run/in.csv" "${CLUE_ARG[@]}" \
        --out_dir "$run/fin" --threads "$THREADS" \
        --finalize_from 7 --finalize_repeats 3 --stop_row 12 \
        --beam_width 150000 --top_columns 0 \
        --num_rows "$RESAMPLE_TOP" --incomplete_top \
        --wall_time "$PASS_FIN_WALL"

    # Rotate and refill a border strip. Alternate the direction between phases
    # so the retained core -- the one region a strip never touches -- moves
    # around the board instead of staying put.
    local rot=1
    [ $(( iter / RESAMPLE_EVERY % 2 )) -eq 0 ] || rot=-1
    bin/E555_roundhouse "$SEED" "$run/in.csv" "${CLUE_ARG[@]}" \
        --out_dir "$run/rh" --threads "$THREADS" \
        --rounds 1 --strip_width 5 --rotate "$rot" \
        --num_rows "$RESAMPLE_TOP" --wall_time 300

    harvest $(cat "$run/fin/outputs.txt" "$run/rh/outputs.txt")
}

# One production phase: a whole new board from a fresh random border.
produce() {  # RUN_DIR ITER
    local run=$1 iter=$2
    bash pipeline/run_pipeline.sh BORDERS=random RUN_DIR="$run" \
        THREADS="$THREADS" DB_FILE="$DB_FILE" CLUES="$CLUES" \
        RNG_SEED="$(( (RANDOM << 15) + RANDOM + iter ))" \
        BEAM_MAX_WALL="$PASS_BEAM_WALL" FIN_MAX_WALL="$PASS_FIN_WALL" \
        CPSAT_TIME="$PASS_CPSAT" CPSAT_STALL="$(( PASS_CPSAT / 3 ))"

    # Take everything the pass produced, not just its winner: a slightly worse
    # board with its breaks packed into one corner is often a better starting
    # point than a higher-scoring mess.
    harvest "$run"/[0-9]_*.csv "$run"/FINAL_best.csv
}

BEST=$(score "$CHAMPS")
ITER=0
START=$SECONDS
echo "[farm] starting in $FARM_DIR with $THREADS threads; best so far: $BEST/480"
echo "[farm] $(date '+%F %T') start, champions=$BEST/480" >> "$LOG"

while true; do
    if [ "$MAX_HOURS" != 0 ] && [ $(( SECONDS - START )) -ge $(( MAX_HOURS * 3600 )) ]; then
        echo "[farm] MAX_HOURS reached, stopping."
        break
    fi
    ITER=$(( ITER + 1 ))
    RUN="$FARM_DIR/run_$ITER"
    rm -rf "$RUN"

    if [ $(( ITER % RESAMPLE_EVERY )) -eq 0 ] && [ -s "$CHAMPS" ]; then
        echo; echo "=== iteration $ITER: RESAMPLE the top $RESAMPLE_TOP champions ==="
        resample "$RUN" "$ITER"
    else
        echo; echo "=== iteration $ITER: PRODUCE a new board ==="
        produce "$RUN" "$ITER"
    fi

    NEW=$(score "$CHAMPS")
    ELAPSED=$(( (SECONDS - START) / 60 ))
    if [ "${NEW:-0}" -gt "${BEST:-0}" ]; then
        head -1 "$CHAMPS" > "$FARM_DIR/records/best_${NEW}_iter${ITER}.csv"
        echo "[farm] iter $ITER: NEW RECORD $NEW/480 (was $BEST) after ${ELAPSED}m"
        echo "[farm] $(date '+%F %T') iter=$ITER score=$NEW RECORD" >> "$LOG"
        BEST="$NEW"
    else
        echo "[farm] iter $ITER: best still $BEST/480 after ${ELAPSED}m"
        echo "[farm] $(date '+%F %T') iter=$ITER score=$NEW" >> "$LOG"
        rm -rf "$RUN"
    fi
done

echo
echo "[farm] $ITER iterations, best $BEST/480, champions in $CHAMPS"
python3 tools/E555_rank.py "$CHAMPS" --seed_file "$SEED" --top 10
