#!/bin/bash
# run_board_farm.sh -- run the pipeline for days, keep only the best boards.
#
#   bash pipeline/run_board_farm.sh
#   bash pipeline/run_board_farm.sh THREADS=32 DB_FILE=/tmp/E555.db ANNEAL_EVERY=4
#   bash pipeline/run_board_farm.sh MAX_HOURS=48 FARM_DIR=/data/farm CLUES=center
#
# NOT an example: start it once and leave it running for days. Ctrl-C is safe at
# any time -- champions.csv is rewritten after every iteration and never depends
# on a run directory surviving.
#
# The loop alternates two phases, because producing new boards and improving
# existing ones want opposite settings. Production is broad and shallow: a whole
# new board from a fresh border. Every RESAMPLE_EVERY iterations it stops
# producing and resamples instead -- the finalizer and the roundhouse re-attack
# the current champions, pulling pieces from deep in the board up to the
# frontier. Alternating is what makes the farm climb rather than just accumulate.
#
# What it leaves behind, all under FARM_DIR:
#   champions.csv   the best KEEP boards, best first, rewritten every iteration
#   records/        every board that set a record, with the log that produced it
#   farm.log        one line per iteration, the whole history of the run
#   failed/         logs of iterations that died, capped at FAILED_KEEP files
# Iterations that neither set a record nor failed leave nothing: that is what
# keeps a multi-day run from filling the disk with output nobody will read.
set -uo pipefail        # not -e: one failing iteration must not kill the farm

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # relative to REPO
FARM_DIR=farm                           # relative to where you START it
THREADS=8
MAX_HOURS=0             # 0 = run until you stop it
KEEP=500                # champions retained in champions.csv
RESAMPLE_EVERY=5        # produce N times, then resample once
RESAMPLE_TOP=10         # champions re-attacked in a resample phase

# Border source for production iterations. Random borders cost nothing and give
# the sweep more frames per hour; annealed ones are stronger but spend minutes
# in Stage A first. ANNEAL_EVERY mixes them: 0 = always random, 4 = every fourth
# production iteration anneals. Sizes below apply only to those iterations.
ANNEAL_EVERY=0
ANNEAL_ROUNDS=50
ANNEAL_STEPS=500000

# Per-iteration budgets, in seconds. These are what you raise for a long run:
# the farm's whole climb rate is how deep one iteration gets before it stops.
PASS_BEAM_WALL=900
PASS_FIN_WALL=600
PASS_RH_WALL=300
PASS_CPSAT=180          # per CP-SAT solve; the tail runs several of them

# The chain database cache is the single biggest saving here: without it every
# production iteration spends 2-3 minutes rebuilding the same 6.4 GB. An
# absolute path puts it on a local disk, which is what you want on a cluster
# node -- DB_FILE=/tmp/E555.db. A bare name lands in FARM_DIR. Needs ~6.5 GB
# free. Set DB_FILE= (empty) to disable. Only the beamer can use it; the
# finalizer and the roundhouse build a database per board and cannot cache one.
DB_FILE=chain.db
CLUES=none              # none | center | all -- which Eternity II clues to hold
FAILED_KEEP=20          # failure logs kept before the farm stops saving them
GIVE_UP_AFTER=10        # consecutive FAILED iterations that end the run; 0 never
                        # gives up. A failed iteration is one whose tools exited
                        # non-zero -- not one that merely found nothing, which is
                        # an ordinary result and never counts here. Failures cost
                        # no time, so without this brake a broken setup spins for
                        # days; any single success resets the count to zero.

# Anything else run_pipeline.sh accepts, passed straight through to every
# production iteration. The settings above cover what a farm normally tunes;
# this reaches the rest without the farm having to name them one by one:
#   PIPE_EXTRA="BEAM_STOP_ROW=12 TOP_N=40 BT_TIME=600"
PIPE_EXTRA=
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done

case "$CLUES" in
    none|0)  CLUES=none;   CLUE_ARG=() ;;
    center)  CLUES=center; CLUE_ARG=(--clue_center) ;;
    all|1)   CLUES=all;    CLUE_ARG=(--clue_center --clue_corners) ;;
    *) echo "CLUES must be none, center or all (got: $CLUES)" >&2; exit 1 ;;
esac

case "$FARM_DIR" in /*) ;; *) FARM_DIR="$PWD/$FARM_DIR" ;; esac
cd "$REPO"
[ -d bin ] && [ -d tools ] ||
    { echo "REPO=$REPO is not an E555 checkout -- set REPO at the top" >&2; exit 1; }
[ -x bin/E555_beamer ] || make
SEED=$PWD/$SEED
[ -f "$SEED" ] || { echo "seed file not found: $SEED" >&2; exit 1; }

# Preflight, all of it before the first iteration: a farm is started once and
# looked at hours later, and every check below is for something that would
# otherwise fail silently in every single iteration until then.
python3 -c 'import ortools' 2>/dev/null || {
    echo "!! ortools is not installed, so the pipeline's two CP-SAT stages (topper"
    echo "   and ender) would fail in every production iteration. Install it with"
    echo "       pip install ortools"
    echo "   or run with RESAMPLE_EVERY=1, which never calls them." >&2
    exit 1; }

mkdir -p "$FARM_DIR/records"
if [ -n "$DB_FILE" ]; then
    case "$DB_FILE" in /*) ;; *) DB_FILE="$FARM_DIR/$DB_FILE" ;; esac
    # A clued run leaves the clue pieces out of the chains, so its cache is not
    # interchangeable with an unclued one -- the beamer checks the exclusion set
    # and rebuilds on a mismatch. One file per setting means switching CLUES
    # between runs does not throw the other cache away.
    [ "$CLUES" = none ] || DB_FILE="$DB_FILE.$CLUES"
    DB_DIR=$(dirname "$DB_FILE")
    mkdir -p "$DB_DIR" 2>/dev/null
    [ -w "$DB_DIR" ] || { echo "DB_FILE directory is not writable: $DB_DIR" >&2; exit 1; }
    FREE_GB=$(df -BG --output=avail "$DB_DIR" 2>/dev/null | tail -1 | tr -dc 0-9)
    [ "${FREE_GB:-99}" -ge 7 ] ||
        echo "[warn] only ${FREE_GB}G free on $DB_DIR; the chain database needs about 6.5G"
fi

rm -rf "$FARM_DIR"/run_*        # whatever a previous farm was killed in the
                               # middle of; champions.csv already has its boards
CHAMPS="$FARM_DIR/champions.csv"
LOG="$FARM_DIR/farm.log"
touch "$CHAMPS"

# score FILE -- matched edges of the best board, or 0 for an empty file.
# --field score reads the real board rather than trusting field 2, which the
# beam stages write a solution index into.
score() {
    local n
    [ -s "$1" ] || { echo 0; return; }
    n=$(python3 tools/E555_rank.py "$1" --seed_file "$SEED" --field score 2>/dev/null)
    case "$n" in ''|*[!0-9]*) echo 0 ;; *) echo "$n" ;; esac
}

# harvest FILE... -- merge boards into champions.csv, best first, keep KEEP.
harvest() {
    HARVESTED=0
    [ $# -gt 0 ] && HARVESTED=$(cat "$@" 2>/dev/null | grep -cv '^ *#')
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
# what they wrote, so harvest is handed exact paths and never a glob. Neither
# takes --db_file: each rebuilds a database around the pieces this board locks.
resample() {  # RUN_DIR ITER
    local run=$1 iter=$2 rc=0
    head -n "$RESAMPLE_TOP" "$CHAMPS" > "$run/in.csv"

    # Free five rows and re-grow them, three stochastic re-rolls each. This is
    # what the finalizer's --frac_rand default is for: repeated passes over one
    # board, where the random band is coverage rather than a tax. --num_rows
    # 0 means every line of in.csv, however many RESAMPLE_TOP actually yielded.
    bin/E555_finalizer "$SEED" "$run/in.csv" "${CLUE_ARG[@]}" \
        --out_dir "$run/fin" --threads "$THREADS" \
        --finalize_from 7 --finalize_repeats 3 --stop_row 12 \
        --top_columns 0 --num_rows 0 --incomplete_top \
        --wall_time "$PASS_FIN_WALL" || rc=$?

    # Rotate and refill a border strip. Alternate the direction between phases
    # so the retained core -- the one region a strip never touches -- moves
    # around the board instead of staying put.
    local rot=1
    [ $(( iter / RESAMPLE_EVERY % 2 )) -eq 0 ] || rot=-1
    bin/E555_roundhouse "$SEED" "$run/in.csv" "${CLUE_ARG[@]}" \
        --out_dir "$run/rh" --threads "$THREADS" \
        --rounds 1 --strip_width 5 --rotate "$rot" \
        --num_rows 0 --wall_time "$PASS_RH_WALL" || rc=$?

    # Harvest whatever did get written even when a tool died: half a phase of
    # boards is still boards. The status is reported separately, above.
    harvest $(cat "$run/fin/outputs.txt" "$run/rh/outputs.txt" 2>/dev/null)
    return $rc
}

# One production phase: a whole new board from a fresh border. BORDERS decides
# where that border comes from; ANNEAL_EVERY decides how often it is annealed.
produce() {  # RUN_DIR ITER BORDERS
    local run=$1 iter=$2 borders=$3 rc=0
    bash pipeline/run_pipeline.sh BORDERS="$borders" RUN_DIR="$run" \
        THREADS="$THREADS" DB_FILE="$DB_FILE" CLUES="$CLUES" \
        ROUNDS="$ANNEAL_ROUNDS" STEPS="$ANNEAL_STEPS" \
        RNG_SEED="$(( (RANDOM << 15) + RANDOM + iter ))" \
        BEAM_MAX_WALL="$PASS_BEAM_WALL" FIN_MAX_WALL="$PASS_FIN_WALL" \
        RH_WALL="$PASS_RH_WALL" \
        CPSAT_TIME="$PASS_CPSAT" CPSAT_STALL="$(( PASS_CPSAT / 3 ))" \
        $PIPE_EXTRA || rc=$?

    # Take everything the pass produced, not just its winner: a slightly worse
    # board with its breaks packed into one corner is often a better starting
    # point than a higher-scoring mess. A pipeline that died partway still leaves
    # the stages it finished, so this runs either way and the status is returned.
    harvest "$run"/[0-9]_*.csv "$run"/FINAL_best.csv
    return $rc
}

BEST=$(score "$CHAMPS")
HARVESTED=0
ITER=0
PRODUCED=0
FAILED=0
CONSEC=0
START=$SECONDS
echo "[farm] $FARM_DIR, $THREADS threads, clues=$CLUES, db=${DB_FILE:-<in memory>}"
echo "[farm] best so far: $BEST/480. One line per iteration below; the detail of"
echo "[farm] each one is in its own log, kept only for records and failures."
echo "[farm] $(date '+%F %T') start, champions=$BEST/480" >> "$LOG"

while true; do
    if [ "$MAX_HOURS" != 0 ] && [ $(( SECONDS - START )) -ge $(( MAX_HOURS * 3600 )) ]; then
        echo "[farm] MAX_HOURS reached, stopping."
        break
    fi
    ITER=$(( ITER + 1 ))
    RUN="$FARM_DIR/run_$ITER"
    rm -rf "$RUN"; mkdir -p "$RUN"
    STAMP=$(date '+%Y%m%d-%H%M%S')
    T0=$SECONDS

    # Every phase writes its whole output -- the tools' own logs included -- to
    # this one file instead of the farm's stdout. Days of --verbose beam and
    # CP-SAT telemetry is gigabytes; kept per iteration and thrown away with the
    # run directory, it costs nothing and is still there when a record needs
    # explaining.
    if [ $(( ITER % RESAMPLE_EVERY )) -eq 0 ] && [ -s "$CHAMPS" ]; then
        WHAT="resample top $RESAMPLE_TOP"
        resample "$RUN" "$ITER" > "$RUN/iter.log" 2>&1
    else
        PRODUCED=$(( PRODUCED + 1 ))
        BORDERS=random
        if [ "$ANNEAL_EVERY" != 0 ] && [ $(( PRODUCED % ANNEAL_EVERY )) -eq 0 ]; then
            BORDERS=annealed
        fi
        WHAT="produce $BORDERS"
        produce "$RUN" "$ITER" "$BORDERS" > "$RUN/iter.log" 2>&1
    fi
    STATUS=$?

    NEW=$(score "$CHAMPS")
    MINS=$(( (SECONDS - START) / 60 ))
    TOOK=$(( SECONDS - T0 ))

    if [ "$STATUS" != 0 ]; then
        # A dead iteration is the one whose log is worth keeping unread: it is
        # the same failure every time, and the farm carries on regardless.
        FAILED=$(( FAILED + 1 )); CONSEC=$(( CONSEC + 1 ))
        if [ "$FAILED" -le "$FAILED_KEEP" ]; then
            mkdir -p "$FARM_DIR/failed"
            cp "$RUN/iter.log" "$FARM_DIR/failed/iter${ITER}-${STAMP}.log"
        fi
        echo "[farm] iter $ITER: $WHAT FAILED (exit $STATUS) after ${TOOK}s, best still $BEST/480"
        echo "[farm] $(date '+%F %T') iter=$ITER $WHAT exit=$STATUS score=$NEW" >> "$LOG"
        if [ "$GIVE_UP_AFTER" != 0 ] && [ "$CONSEC" -ge "$GIVE_UP_AFTER" ]; then
            echo "[farm] $CONSEC iterations in a row failed -- stopping. The setup is"
            echo "[farm] broken, not unlucky: read $FARM_DIR/failed/ for the reason."
            echo "[farm] $(date '+%F %T') stop, $CONSEC consecutive failures" >> "$LOG"
            break
        fi
        sleep 5     # a failing iteration costs no time; without this the farm
                    # would retry the same broken setup thousands of times an hour
    elif [ "${NEW:-0}" -gt "${BEST:-0}" ]; then
        CONSEC=0
        head -1 "$CHAMPS" > "$FARM_DIR/records/best_${NEW}_${STAMP}.csv"
        if command -v gzip > /dev/null; then
            gzip -c "$RUN/iter.log" > "$FARM_DIR/records/best_${NEW}_${STAMP}.log.gz"
        else
            cp "$RUN/iter.log" "$FARM_DIR/records/best_${NEW}_${STAMP}.log"
        fi
        echo "[farm] iter $ITER: $WHAT -> NEW RECORD $NEW/480 (was $BEST), $HARVESTED boards in ${TOOK}s, ${MINS}m total"
        echo "[farm] $(date '+%F %T') iter=$ITER $WHAT boards=$HARVESTED score=$NEW RECORD" >> "$LOG"
        BEST="$NEW"
    else
        CONSEC=0
        echo "[farm] iter $ITER: $WHAT -> $HARVESTED boards, best still $BEST/480, ${TOOK}s, ${MINS}m total"
        echo "[farm] $(date '+%F %T') iter=$ITER $WHAT boards=$HARVESTED score=$NEW" >> "$LOG"
    fi
    rm -rf "$RUN"
done

echo
echo "[farm] $ITER iterations ($PRODUCED produced, $FAILED failed), best $BEST/480"
echo "[farm] champions in $CHAMPS, records in $FARM_DIR/records"
python3 tools/E555_rank.py "$CHAMPS" --seed_file "$SEED" --top 10
