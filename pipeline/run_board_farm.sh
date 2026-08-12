#!/bin/bash
##SBATCH --job-name=E555_farm
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=12G --time=7-00:00:00
##SBATCH --output=logs/farm_%j.out
#
# =============================================================================
# run_board_farm.sh -- run the pipeline forever, keep only the best boards
# =============================================================================
# THIS IS NOT AN EXAMPLE. It is meant to be started once and left running for
# days. Ctrl-C is safe at any time: the champions file is rewritten after every
# iteration and never depends on the run directory surviving.
#
# THE LOOP
#   1. run one full pipeline pass on a fresh random border (a new RNG seed each
#      time, so every iteration explores somewhere else)
#   2. harvest that pass's boards into champions.csv and keep the best KEEP
#   3. every RESAMPLE_EVERY iterations, stop producing and RESAMPLE instead:
#      re-attack the current champions with the finalizer and the roundhouse,
#      which pull pieces from deep in the board up to the frontier
#   4. throw the run directory away unless it set a new record
#
# WHY RESAMPLING IS A SEPARATE PHASE
#   Producing new boards and improving existing ones need opposite settings.
#   Production wants breadth: many borders, shallow. Resampling wants depth:
#   one board, many re-rolls of its top rows. Alternating is what makes the
#   farm climb instead of just accumulating.
#
# WHAT TO WATCH
#   farm.log        one line per iteration: score, whether it was a record
#   champions.csv   the KEEP best boards seen, best first, always current
#   records/        a copy of every board that set a new record, never deleted
#
# START IT
#   bash pipeline/run_board_farm.sh                 # defaults, runs forever
#   MAX_HOURS=48 THREADS=16 bash pipeline/run_board_farm.sh
# =============================================================================
set -uo pipefail          # not -e: one failing iteration must not kill the farm

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- settings ---------------------------------------------------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
FARM_DIR="${FARM_DIR:-$PWD/farm}"
THREADS="${THREADS:-$(nproc 2>/dev/null || echo 4)}"
MAX_HOURS="${MAX_HOURS:-0}"          # 0 = run until you stop it
KEEP="${KEEP:-200}"                  # champions retained
RESAMPLE_EVERY="${RESAMPLE_EVERY:-5}" # produce N times, then resample once
RESAMPLE_TOP="${RESAMPLE_TOP:-10}"   # champions re-attacked in a resample phase

# Per-iteration budgets. Smaller = more iterations = more borders tried.
PASS_BEAM_WALL="${PASS_BEAM_WALL:-600}"
PASS_FIN_WALL="${PASS_FIN_WALL:-420}"
PASS_CPSAT="${PASS_CPSAT:-45}"

# The chain database cache is the single biggest saving here: without it every
# iteration spends 2-3 minutes rebuilding the same 6.4 GB. Needs ~6.5 GB free
# disk. Set DB_FILE= (empty) to disable.
DB_FILE="${DB_FILE:-$FARM_DIR/chain.db}"
CLUES="${CLUES:-0}"                     # 1 = hold the Eternity II clue pieces in
                                        # place. Passed straight down to the
                                        # production pipeline, which also derives
                                        # the matching chain-database filename.

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
# -----------------------------------------------------------------------------

mkdir -p "$FARM_DIR/records"
cd "$FARM_DIR"
CHAMPS="$FARM_DIR/champions.csv"
LOG="$FARM_DIR/farm.log"
touch "$CHAMPS"

[ -x "$REPO/bin/E555_beamer" ] || make -C "$REPO"

# score FILE -- matched edges of the best board in a canonical CSV, or 0.
score() {
    if [ -s "$1" ]; then cut -d, -f2 "$1" | head -1 | tr -d ' '; else echo 0; fi
}

# harvest FILE... -- merge boards into champions.csv, best first, keep KEEP.
harvest() {
    cat "$CHAMPS" "$@" > merged.csv 2>/dev/null
    [ -s merged.csv ] || { rm -f merged.csv; return; }
    # --rescore makes field 2 the true matched-edge count whatever tool wrote
    # the line, and ranks best first: fewest breaks, then most compact.
    python3 "$REPO/tools/E555_rank.py" merged.csv --seed "$SEED" \
        --sort breaks,break_rows --emit scored.csv --rescore > /dev/null
    # drop exact duplicate boards (everything after the id), keep the best KEEP
    awk '!seen[substr($0, index($0, ","))]++' scored.csv | head -n "$KEEP" > "$CHAMPS"
    rm -f merged.csv scored.csv
}

BEST=$(score "$CHAMPS")
ITER=0
START=$SECONDS
echo "[farm] starting in $FARM_DIR with $THREADS threads; best so far: $BEST/480"
echo "[farm] $(date '+%F %T') start, champions=$BEST/480" >> "$LOG"

while true; do
    if [ "$MAX_HOURS" != "0" ] && [ $(( SECONDS - START )) -ge $(( MAX_HOURS * 3600 )) ]; then
        echo "[farm] MAX_HOURS reached, stopping."
        break
    fi
    ITER=$(( ITER + 1 ))
    RUN="$FARM_DIR/run_$ITER"
    rm -rf "$RUN"

    if [ $(( ITER % RESAMPLE_EVERY )) -eq 0 ] && [ -s "$CHAMPS" ]; then
        # ---------------- resample phase: improve what we already have -------
        echo
        echo "=== iteration $ITER: RESAMPLE the top $RESAMPLE_TOP champions ==="
        mkdir -p "$RUN"
        head -n "$RESAMPLE_TOP" "$CHAMPS" > "$RUN/in.csv"

        # Free five rows and re-grow them, three stochastic re-rolls each.
        "$REPO/bin/E555_finalizer" "$SEED" "$RUN/in.csv" \
            --out_dir "$RUN/fin" --threads "$THREADS" \
            --finalize_from 7 --finalize_repeats 3 --stop_row 12 \
            --beam_width 150000 --top_columns 0 --lambda_Mahalanobis 10 \
            --border_row_N "$RESAMPLE_TOP" --incomplete_top \
            --max_wall_sec "$PASS_FIN_WALL" "${CLUE_ARG[@]}" 2>/dev/null

        # Rotate and refill a border strip. Alternate the direction between
        # phases so the retained core -- the one region a strip never touches --
        # moves around the board instead of staying put.
        if [ $(( ITER / RESAMPLE_EVERY % 2 )) -eq 0 ]; then ROT=1; else ROT=-1; fi
        "$REPO/bin/E555_roundhouse" "$SEED" "$RUN/in.csv" \
            --out_dir "$RUN/rh" --threads "$THREADS" \
            --rounds 1 --strip_width 5 --rotate "$ROT" \
            --border_row_N "$RESAMPLE_TOP" --max_wall_sec 300 "${CLUE_ARG[@]}" 2>/dev/null

        harvest "$RUN"/fin/*.csv "$RUN"/rh/*.csv
    else
        # ---------------- production phase: a whole new board ----------------
        echo
        echo "=== iteration $ITER: PRODUCE a new board ==="
        RUN_DIR="$RUN" THREADS="$THREADS" SEED="$SEED" DB_FILE="$DB_FILE" CLUES="$CLUES" \
        RNG_SEED="$(( (RANDOM << 15) + RANDOM + ITER ))" \
        BEAM_WALL="$PASS_BEAM_WALL" FIN_WALL="$PASS_FIN_WALL" \
        CPSAT_TIME="$PASS_CPSAT" CPSAT_STALL="$(( PASS_CPSAT / 3 ))" \
            bash "$REPO/pipeline/run_pipeline_random.sh"

        # Take everything the pass produced, not just its winner: a slightly
        # worse board with its breaks packed into one corner is often a better
        # starting point than a higher-scoring mess.
        harvest "$RUN"/[0-9]_*.csv "$RUN"/FINAL_best.csv
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
python3 "$REPO/tools/E555_rank.py" "$CHAMPS" --seed "$SEED" --top 10
