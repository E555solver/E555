#!/bin/bash
# run_cycles.sh -- fresh boards in parallel cycles: a beam that only grows
# partial boards over many border configurations, then the distiller, which
# spends its dives on the best of them. The best boards of every cycle collect
# in RUN_DIR/best.csv.
#
#   bash pipeline/run_cycles.sh
#   bash pipeline/run_cycles.sh TOTAL_TIME=2d THREADS=64 DB_FILE=/data/E555.db
#   bash pipeline/run_cycles.sh STOP_ROW=9 MIN_COL=4 RUN_DIR=cycles_r9
#
# One cycle, on THREADS_PER_SLOT threads; SLOTS cycles run at once:
#   1  E555_beamer --random_edges: SAMPLES bottoms x COLUMNS left columns, a beam
#      BEAM_WIDTH wide to BACKTRACK_ROW, exhaustive to STOP_ROW, NO end dives,
#      for BEAM_WALL seconds. Without the dives a configuration takes seconds,
#      so a cycle covers many borders: ~15 configurations and ~50000 row-11
#      partials in 300 s on 4 threads. MIN_COL > 0 keeps only the boards whose
#      column extension above STOP_ROW fills columns 1..MIN_COL whole.
#   2  tools/E555_distiller.py on those partials: a cheap screen of every one,
#      a full finish of the best, a probe (the ender's redive) of the best
#      finished, the TOP best kept.
#   3  RUN_DIR/best.csv is rebuilt from every cycle's distillate.
#
# Why: spending a run's dives on one border caps it at what that border allows.
# Measured on 4 cores (the agent kit's cloud campaign 20261028-2026, results on
# the agent_runs branch; it found this route): the stock route, a beam with
# --end_dive 10000 --end_polish 20000, gave 461-463 per 8-minute run (one 463 in
# 12 runs); these defaults gave a best of 464 in 4 of 40 cycles, 463 in 26, 462
# in 10, about 24 minutes a cycle. Five of those boards are in data/best_464.csv.
#
# What RUN_DIR holds afterwards:
#   best.csv      the BEST_KEEP best distinct boards of all cycles, best first
#   cycles.log    one line per finished cycle: seed, configurations, partials,
#                 best, minutes
#   cycles/c_N/   per cycle: distilled.csv (its TOP boards), the ender plan
#                 distilled.plan.sh, logs; the partials only with KEEP_CORPUS=1
#   chain.db      the chain database cache (DB_FILE), unless placed elsewhere
# A rerun with the same RUN_DIR carries on: a cycle stopped in its distil
# resumes there, one stopped in its beam starts over; to compare settings, give
# each its own RUN_DIR (best.csv pools every cycle in it). Ctrl-C is safe: the tools
# stop cleanly, the finished cycles are kept, and the run ends. A kill from
# outside (scancel, kill -TERM of the whole group) may kill a beamer outright;
# its cycle then starts over on the rerun.
#
# NEEDS ~7 GB of RAM for the chain database, which every slot maps from one
# page-cached file, plus ~1 GB per slot (measured: a 4-thread beam at the
# defaults peaks at 0.9 GB of its own); ~7 GB of disk for DB_FILE; bash 4.4+.
set -uo pipefail        # not -e: a cycle that fails must not kill the run

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # relative to REPO
RUN_DIR=cycles_out                      # relative to where you START it
THREADS=$(nproc)
THREADS_PER_SLOT=4      # threads of one cycle. 4 is the measured setting; more
                        # threads give a beam more boards in BEAM_WALL
SLOTS=0                 # cycles at once; 0 = THREADS / THREADS_PER_SLOT
TOTAL_TIME=24h          # seconds, or 90m, 4h, 2d. Cycles still running at the end
                        # are stopped and resume on a rerun.
MAX_CYCLES=0            # new cycles this run at most; 0 = until TOTAL_TIME. With
                        # 1, a run is one cycle: handy for sweeping settings
DB_FILE=chain.db        # the chain database cache, ~6.5 GB, shared by every
                        # slot. A bare name lands in RUN_DIR; give an absolute
                        # path to share it between runs. Must not be empty.
WARM_DB=1               # 1 = read DB_FILE once before the slots start: a cold
                        # page cache makes every beam stall
RNG_SEED=0              # 0 = fresh seeds from the start time; S = S+1, S+2, ...
                        # per cycle. The seed names the cycle's boards (c<N>s<seed>_...).

# 1. beam (E555_beamer)
SAMPLES=4               # --samples: random bottom rows per cycle
COLUMNS=4               # --top_columns: left columns per bottom
BEAM_WIDTH=10000        # --beam_width
BACKTRACK_ROW=6         # --backtrack_row: the beam stops here, the exhaustive
                        # search goes on to STOP_ROW
STOP_ROW=11             # --stop_row: the last row the boards fill
MIN_COL=0               # --backtrack_min_col: > 0 extends every stop-row board
                        # column by column above STOP_ROW and keeps only those
                        # filling columns 1..MIN_COL whole; 0 = no extension,
                        # every stop-row board kept. The measured setting is
                        # STOP_ROW=11 MIN_COL=0. To try: STOP_ROW=9 MIN_COL=3..5
                        # (PROJECT_E555.md 3.1, 5.10). STOP_ROW below 11 needs
                        # MIN_COL > 0: a row-9 run writes millions of boards.
EXTEND_NODES=100000     # --extend_nodes: node cap of each extension (MIN_COL > 0)
LAMBDA_MAHA=10          # --lambda_Mahalanobis (beamer default 1; measured at 10)
LAMBDA_J=1              # --lambda_J (beamer default)
FRAC_RAND=0.1           # --frac_rand (beamer default)
TAU=0                   # --tau_bottoms and --tau_columns (beamer default)
BEAM_WALL=300           # --wall_time of the beam, seconds
MAX_EMITTED=200000      # --max_emitted: partials one beam may write; the screen
                        # costs ~0.15 thread-seconds a partial. 0 = no cap
BEAM_EXTRA=             # more beamer flags, e.g. BEAM_EXTRA="--clue_center"
                        # (a clue flag needs its own DB_FILE)

# 2. distil (tools/E555_distiller.py)
TOP=10                  # --top: boards kept per cycle
SCREEN_DIVES=300        # --screen_dives: dives per partial in the screen
FINISH_DIVES=20000      # --finish_dives  } the finish of the best partials
FINISH_POLISH=5000      # --finish_polish }
PROBE_PER_TOP=4         # --probe_per_top: finished boards probed per kept board.
                        # The campaign's later cycles used FINISH_DIVES=5000
                        # FINISH_POLISH=2000 PROBE_PER_TOP=12: shorter cycles
                        # (24 instead of ~35 min), the same best.
DISTIL_EXTRA=           # more distiller flags

# 3. the pool
BEST_KEEP=50            # boards in best.csv
MAX_AGREE=0.95          # best.csv drops a board agreeing with a better one on
                        # more than this share of its cells
KEEP_CORPUS=0           # 1 = keep every cycle's partials (~100 MB a cycle)
KEEP_LOGS=1             # 1 = keep each cycle's logs, gzipped
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done

die() { echo "$*" >&2; exit 1; }
[ "${BASH_VERSINFO[0]}" -gt 4 ] || { [ "${BASH_VERSINFO[0]}" -eq 4 ] && [ "${BASH_VERSINFO[1]}" -ge 4 ]; } ||
    die "bash 4.4 or newer is needed (wait -n, empty arrays under set -u)"
[[ $TOTAL_TIME =~ ^([0-9]+)([smhd]?)$ ]] ||
    die "TOTAL_TIME must be seconds, or a whole number with s, m, h or d: $TOTAL_TIME"
case "${BASH_REMATCH[2]}" in
    d) TOTAL_S=$(( BASH_REMATCH[1] * 86400 )) ;;
    h) TOTAL_S=$(( BASH_REMATCH[1] * 3600 )) ;;
    m) TOTAL_S=$(( BASH_REMATCH[1] * 60 )) ;;
    *) TOTAL_S=${BASH_REMATCH[1]} ;;
esac
for v in THREADS THREADS_PER_SLOT SLOTS MAX_CYCLES RNG_SEED SAMPLES COLUMNS BEAM_WIDTH BACKTRACK_ROW \
         STOP_ROW MIN_COL EXTEND_NODES BEAM_WALL MAX_EMITTED TOP SCREEN_DIVES FINISH_DIVES \
         FINISH_POLISH PROBE_PER_TOP BEST_KEEP KEEP_CORPUS KEEP_LOGS WARM_DB; do
    [[ ${!v} =~ ^[0-9]+$ ]] || die "$v must be a whole number (got: ${!v})"
done
for v in LAMBDA_MAHA LAMBDA_J FRAC_RAND TAU MAX_AGREE; do
    [[ ${!v} =~ ^[0-9]+([.][0-9]+)?$ ]] || die "$v must be a number of 0 or more (got: ${!v})"
done
[ "$THREADS_PER_SLOT" -ge 1 ] || die "THREADS_PER_SLOT must be at least 1"
[ "$SLOTS" -gt 0 ] || SLOTS=$(( THREADS / THREADS_PER_SLOT ))
[ "$SLOTS" -ge 1 ] || die "THREADS=$THREADS is fewer than THREADS_PER_SLOT=$THREADS_PER_SLOT"
[ $(( SLOTS * THREADS_PER_SLOT )) -le "$THREADS" ] ||
    die "SLOTS x THREADS_PER_SLOT = $(( SLOTS * THREADS_PER_SLOT )) is more than THREADS=$THREADS"
[ "$SAMPLES" -ge 1 ] && [ "$COLUMNS" -ge 1 ] || die "SAMPLES and COLUMNS must be at least 1"
[ "$BACKTRACK_ROW" -ge 1 ] && [ "$BACKTRACK_ROW" -lt "$STOP_ROW" ] ||
    die "BACKTRACK_ROW must be at least 1 and below STOP_ROW"
[ "$MIN_COL" -le 14 ] || die "MIN_COL must be in 0..14"
[ "$STOP_ROW" -ge 11 ] || [ "$MIN_COL" -gt 0 ] ||
    die "STOP_ROW=$STOP_ROW needs MIN_COL > 0: without it every stop-row board is written (millions)"
[ "$MIN_COL" -eq 0 ] || [ "$EXTEND_NODES" -gt 0 ] || die "MIN_COL > 0 needs EXTEND_NODES > 0"
[ "$TOP" -ge 1 ] && [ "$BEST_KEEP" -ge 1 ] || die "TOP and BEST_KEEP must be at least 1"
[ -n "$DB_FILE" ] || die "DB_FILE must be set: without a cache every beam rebuilds the 6.4 GB database"

case "$RUN_DIR" in /*) ;; *) RUN_DIR="$PWD/$RUN_DIR" ;; esac
cd "$REPO" || die "REPO=$REPO does not exist"
[ -d bin ] && [ -d tools ] || die "REPO=$REPO is not an E555 checkout -- set REPO at the top"
[ -s "$SEED" ] || die "seed file not found: $SEED"
make -s beamer diver > /dev/null || die "the build failed: run make in $REPO"
mkdir -p "$RUN_DIR/cycles" || die "cannot create RUN_DIR=$RUN_DIR"
case "$DB_FILE" in /*) ;; *) DB_FILE="$RUN_DIR/$DB_FILE" ;; esac
mkdir -p "$(dirname "$DB_FILE")"

LOG=$RUN_DIR/cycles.log
BEST=$RUN_DIR/best.csv
STOPFILE=$RUN_DIR/STOP
touch "$LOG"
rm -f "$STOPFILE"

hms()  { local s=$1; if [ "$s" -ge 3600 ]; then printf '%dh%02dm' $((s/3600)) $((s%3600/60))
         elif [ "$s" -ge 60 ]; then printf '%dm%02ds' $((s/60)) $((s%60)); else printf '%ds' "$s"; fi; }
rows() { awk '!/^[ \t]*([#%]|$)/' "$@" 2>/dev/null | grep -c . ; }
best_of() { awk -F, '!/^[ \t]*([#%]|$)/ { s = $2 + 0; if (s > m) m = s } END { print m + 0 }' "$@" 2>/dev/null; }

# The seed of cycle N is SEED_BASE + N. SEED_BASE is kept in RUN_DIR so that a
# rerun goes on with new seeds rather than repeating old ones.
if [ -s "$RUN_DIR/seed_base" ]; then
    SEED_BASE=$(cat "$RUN_DIR/seed_base")
elif [ "$RNG_SEED" -gt 0 ]; then
    SEED_BASE=$RNG_SEED
else
    SEED_BASE=$(( $(date +%s) % 100000000 * 1000 ))
fi
echo "$SEED_BASE" > "$RUN_DIR/seed_base"

read -r -a BEAM_MORE <<< "$BEAM_EXTRA"
read -r -a DISTIL_MORE <<< "$DISTIL_EXTRA"
EXT=(--extend_nodes 0)
[ "$MIN_COL" -eq 0 ] || EXT=(--extend_nodes "$EXTEND_NODES" --backtrack_min_col "$MIN_COL")
BEAM=(--random_edges --samples "$SAMPLES" --top_columns "$COLUMNS"
      --beam_width "$BEAM_WIDTH" --backtrack_row "$BACKTRACK_ROW" --stop_row "$STOP_ROW" "${EXT[@]}"
      --lambda_Mahalanobis "$LAMBDA_MAHA" --lambda_J "$LAMBDA_J" --frac_rand "$FRAC_RAND"
      --tau_bottoms "$TAU" --tau_columns "$TAU" --max_emitted "$MAX_EMITTED"
      --db_file "$DB_FILE" --print_cmd "${BEAM_MORE[@]}")
DISTIL=(--top "$TOP" --screen_dives "$SCREEN_DIVES" --finish_dives "$FINISH_DIVES"
        --finish_polish "$FINISH_POLISH" --probe_per_top "$PROBE_PER_TOP" "${DISTIL_MORE[@]}")

# run_tool SECS CMD... -- run CMD and return its status; SECS > 0 sends it
# SIGTERM after SECS seconds. CMD runs in the background and is waited for, so
# a signal to this shell reaches its trap at once; TOOL holds CMD's pid for it.
TOOL=
run_tool() {
    local secs=$1 pid wd= rc; shift
    "$@" &
    pid=$!; TOOL=$pid
    if [ "$secs" -gt 0 ]; then
        ( end=$(( SECONDS + secs ))
          while [ "$SECONDS" -lt "$end" ] && kill -0 "$pid" 2>/dev/null; do sleep 1; done
          kill -TERM "$pid" 2>/dev/null ) &
        wd=$!
    fi
    while :; do
        wait "$pid"; rc=$?
        kill -0 "$pid" 2>/dev/null || break       # gone: rc is its status
    done
    [ -z "$wd" ] || kill "$wd" 2>/dev/null
    TOOL=
    return "$rc"
}

# cycle DIR SEED THREADS -- one cycle in DIR; resumes the distil when DIR holds a
# finished beam. Runs as a background job. Markers: beam.done, distilled.done.
# A cycle ends early when RUN_DIR/STOP exists or it gets SIGTERM (the TOTAL_TIME
# deadline), and passes the signal on to the tool it is running.
cycle() {
    local dir=$1 rs=$2 th=$3 n t0 rc
    local name; name=$(basename "$dir")
    trap '[ -n "$TOOL" ] && kill -TERM "$TOOL" 2>/dev/null; touch "$dir/stopped"' TERM
    if [ ! -e "$dir/beam.done" ]; then
        rm -rf "$dir/beam"
        t0=$SECONDS
        run_tool $(( BEAM_WALL + 120 )) bin/E555_beamer "$SEED" "${BEAM[@]}" \
            --rng_seed "$rs" --prefix "c${name#c_}s$rs" --wall_time "$BEAM_WALL" \
            --threads "$th" --out_dir "$dir/beam" > "$dir/beam.log" 2>&1
        rc=$?
        # A beam cut short by a stop is not a cycle's worth: it starts over.
        [ -e "$STOPFILE" ] || [ -e "$dir/stopped" ] && return 0
        n=0
        while read -r f; do
            case "$f" in *.csv) [ -s "$f" ] && n=$(( n + $(rows "$f") )) ;; esac
        done < "$dir/beam/outputs.txt" 2>/dev/null
        if [ "$n" -eq 0 ]; then
            echo "beamer exit $rc, no boards" > "$dir/failed"
            return 0
        fi
        echo "$n" > "$dir/partials"
        echo "$(( SECONDS - t0 ))" > "$dir/beam.seconds"
        touch "$dir/beam.done"
    fi
    local corpus=()
    while read -r f; do
        case "$f" in *.csv) [ -s "$f" ] && corpus+=("$f") ;; esac
    done < "$dir/beam/outputs.txt"
    t0=$SECONDS
    run_tool 0 python3 tools/E555_distiller.py "${corpus[@]}" "${DISTIL[@]}" \
        --threads "$th" --seed_file "$SEED" --out "$dir/distilled.csv" >> "$dir/distil.log" 2>&1
    rc=$?
    # Stopped (130, 143, or a stop asked for): the distil resumes on a rerun.
    [ "$rc" -eq 130 ] || [ "$rc" -eq 143 ] || [ -e "$STOPFILE" ] || [ -e "$dir/stopped" ] && return 0
    if [ "$rc" -ne 0 ] || [ ! -s "$dir/distilled.csv" ]; then
        echo "distiller exit $rc: $(tail -1 "$dir/distil.log")" > "$dir/failed"
        return 0
    fi
    echo "$(( SECONDS - t0 + $(cat "$dir/distil.seconds" 2>/dev/null || echo 0) ))" > "$dir/distil.seconds"
    if [ "$KEEP_CORPUS" = 0 ]; then
        rm -rf "$dir/beam"/*.csv "$dir/distilled_work"
    fi
    if [ "$KEEP_LOGS" = 1 ]; then
        gzip -f "$dir/beam.log" "$dir/distil.log" 2>/dev/null
    else
        rm -f "$dir/beam.log" "$dir/distil.log"
    fi
    touch "$dir/distilled.done"
}

# harvest DIR -- the parent's bookkeeping for a finished cycle: one line in
# cycles.log, best.csv rebuilt from every distillate (so a crash between the two
# loses nothing), the harvested marker.
harvest() {
    local dir=$1 name cfg parts b mins
    name=$(basename "$dir")
    cfg=$( { zcat "$dir/beam.log.gz" 2>/dev/null || cat "$dir/beam.log" 2>/dev/null; } |
           sed -n 's/^\[sum\] configs: \([0-9]*\),.*/\1/p' | tail -1)
    parts=$(cat "$dir/partials" 2>/dev/null || echo "?")
    b=$(best_of "$dir/distilled.csv")
    mins=$(( ( $(cat "$dir/beam.seconds" 2>/dev/null || echo 0) +
               $(cat "$dir/distil.seconds" 2>/dev/null || echo 0) ) / 60 ))
    rebuild_best
    echo "$name seed=$(cat "$dir/seed") configs=${cfg:-?} partials=$parts best=$b" \
         "minutes=$mins run_best=$(best_of "$BEST") $(date '+%F %T')" >> "$LOG"
    echo "[cycles] $name: best $b ($mins min), best.csv $(best_of "$BEST")"
    touch "$dir/harvested"
}
rebuild_best() {
    local files=("$RUN_DIR"/cycles/c_*/distilled.csv)
    [ -e "${files[0]}" ] || return 0
    python3 tools/E555_rank.py "${files[@]}" --seed_file "$SEED" --max_agree "$MAX_AGREE" \
        --top "$BEST_KEEP" --rescore --quiet --out "$BEST.tmp" > /dev/null 2> "$RUN_DIR/rank.log" &&
        mv "$BEST.tmp" "$BEST" || echo "[warn] best.csv not rebuilt: $(tail -1 "$RUN_DIR/rank.log")"
}

STOP=0
SLOT_PIDS=()
trap 'STOP=1; touch "$STOPFILE"' INT
trap 'STOP=1; touch "$STOPFILE"; for p in "${SLOT_PIDS[@]}"; do kill -TERM "$p" 2>/dev/null; done' TERM
DEADLINE=$(( SECONDS + TOTAL_S ))
left() { echo $(( DEADLINE - SECONDS )); }

# -- the chain database: one tiny beam builds or checks it before the slots
#    start, since two beamers building it at once would clash on DB_FILE.tmp.
[ -s "$DB_FILE" ] || echo "[cycles] building the chain database into $DB_FILE (~2 min on 4 threads)"
mkdir -p "$RUN_DIR/db_check"
run_tool 0 bin/E555_beamer "$SEED" --random_edges --samples 1 --top_columns 1 --stop_row 2 \
    --beam_width 16 --wall_time 1 --db_file "$DB_FILE" --threads "$THREADS" \
    --out_dir "$RUN_DIR/db_check" > "$RUN_DIR/db_check/log.txt" 2>&1
[ -s "$DB_FILE" ] || { tail -5 "$RUN_DIR/db_check/log.txt"; die "no chain database in $DB_FILE"; }
rm -rf "$RUN_DIR/db_check"
[ "$STOP" = 0 ] || exit 1
if [ "$WARM_DB" = 1 ]; then
    T0=$SECONDS
    cat "$DB_FILE" > /dev/null
    echo "[cycles] $DB_FILE read into the page cache in $(hms $(( SECONDS - T0 )))"
fi
MEM_GB=$(awk '/^MemTotal:/ { print int($2 / 1048576) }' /proc/meminfo 2>/dev/null)
NEED_GB=$(( 7 + SLOTS ))
[ -z "$MEM_GB" ] || [ "$MEM_GB" -ge "$NEED_GB" ] ||
    echo "[warn] ${MEM_GB} GB of RAM for $SLOTS slots; they need about ${NEED_GB} GB. Lower SLOTS."

# -- what a previous run left: finished cycles are harvested, a cycle with its
#    beam done resumes its distil, anything else starts over.
PENDING=()
for d in "$RUN_DIR"/cycles/c_*; do
    [ -d "$d" ] || continue
    if [ -e "$d/harvested" ]; then :
    elif [ -e "$d/distilled.done" ]; then harvest "$d"
    elif [ -e "$d/beam.done" ]; then rm -f "$d/stopped" "$d/failed"; PENDING+=("$d")
    else rm -rf "$d"
    fi
done
NEXT=$(ls -d "$RUN_DIR"/cycles/c_* 2>/dev/null | sed 's/.*c_0*//' | sort -n | tail -1)
NEXT=$(( ${NEXT:-0} + 1 ))
[ "${#PENDING[@]}" -eq 0 ] || echo "[cycles] resuming ${#PENDING[@]} cycle(s) stopped in their distil"
echo "[cycles] $(date '+%F %T') start: $SLOTS slot(s) x $THREADS_PER_SLOT threads," \
     "$(hms "$TOTAL_S") (TOTAL_TIME), STOP_ROW=$STOP_ROW MIN_COL=$MIN_COL, results in $RUN_DIR"
echo "# $(date '+%F %T') start: slots=$SLOTS threads_per_slot=$THREADS_PER_SLOT stop_row=$STOP_ROW" \
     "min_col=$MIN_COL samples=$SAMPLES columns=$COLUMNS beam_width=$BEAM_WIDTH maha=$LAMBDA_MAHA" \
     "beam_wall=$BEAM_WALL top=$TOP finish=$FINISH_DIVES/$FINISH_POLISH probe_per_top=$PROBE_PER_TOP" >> "$LOG"

# -- the main loop: keep SLOTS cycles running; harvest each as it ends.
declare -A RUNNING=()          # pid -> cycle directory
STARTED=0 NEW=0 HARVESTED=0 SUM_S=0 FAILS=0
# The timer wakes the wait below at the deadline even while every slot is busy.
( sleep $(( $(left) > 1 ? $(left) : 1 )) ) &
TIMER=$!
# A new cycle starts while the time left exceeds the mean cycle so far; until a
# cycle has finished, only the deadline itself stops one.
mean_cycle() { [ "$HARVESTED" -gt 0 ] && echo $(( SUM_S / HARVESTED )) || echo 0; }
while :; do
    while [ "$STOP" = 0 ] && [ "${#RUNNING[@]}" -lt "$SLOTS" ]; do
        if [ "${#PENDING[@]}" -gt 0 ]; then
            dir=${PENDING[0]}; PENDING=("${PENDING[@]:1}")
        else
            [ "$(left)" -gt "$(mean_cycle)" ] || break
            [ "$MAX_CYCLES" -eq 0 ] || [ "$NEW" -lt "$MAX_CYCLES" ] || break
            NEW=$(( NEW + 1 ))
            dir=$(printf '%s/cycles/c_%04d' "$RUN_DIR" "$NEXT")
            mkdir "$dir" || die "cannot create $dir"
            echo $(( SEED_BASE + NEXT )) > "$dir/seed"
            NEXT=$(( NEXT + 1 ))
        fi
        cycle "$dir" "$(cat "$dir/seed")" "$THREADS_PER_SLOT" < /dev/null &
        RUNNING[$!]=$dir
        SLOT_PIDS=("${!RUNNING[@]}")
        STARTED=$(( STARTED + 1 ))
    done
    [ "${#RUNNING[@]}" -gt 0 ] || break
    wait -n
    if [ "$STOP" = 0 ] && [ "$(left)" -le 0 ]; then
        echo "[cycles] TOTAL_TIME reached: stopping the cycles in flight (a rerun resumes them)"
        STOP=1; touch "$STOPFILE"
        for p in "${!RUNNING[@]}"; do kill -TERM "$p" 2>/dev/null; done
    fi
    for p in "${!RUNNING[@]}"; do
        kill -0 "$p" 2>/dev/null && continue
        dir=${RUNNING[$p]}; unset "RUNNING[$p]"
        if [ -e "$dir/distilled.done" ]; then
            harvest "$dir"
            HARVESTED=$(( HARVESTED + 1 ))
            SUM_S=$(( SUM_S + $(cat "$dir/beam.seconds") + $(cat "$dir/distil.seconds") ))
            FAILS=0
        elif [ -e "$dir/failed" ]; then
            echo "[cycles] $(basename "$dir") FAILED: $(cat "$dir/failed"); logs in $dir"
            echo "$(basename "$dir") seed=$(cat "$dir/seed") FAILED: $(cat "$dir/failed")" >> "$LOG"
            FAILS=$(( FAILS + 1 ))
            if [ "$FAILS" -ge 3 ] && [ "$STOP" = 0 ]; then
                echo "[cycles] three cycles failed in a row: stopping (check the settings)"
                STOP=1; touch "$STOPFILE"
            fi
        fi
    done
    SLOT_PIDS=("${!RUNNING[@]}")
done
kill "$TIMER" 2>/dev/null
rm -f "$STOPFILE"

echo
[ "$STOP" = 1 ] && echo "[cycles] stopped; run the same command again to carry on."
if [ -s "$BEST" ]; then
    echo "[cycles] $HARVESTED cycle(s) finished this run, $(grep -c '^c_.* best=' "$LOG") in all;" \
         "best board $(best_of "$BEST") in $BEST"
else
    echo "[cycles] $HARVESTED cycle(s) finished this run; no board yet"
fi
echo "# $(date '+%F %T') end: $HARVESTED cycle(s) this run, best $(best_of "$BEST")" >> "$LOG"
[ -s "$BEST" ] || exit 0
python3 tools/E555_rank.py "$BEST" --seed_file "$SEED" --top 5
echo
echo "Push the best ones further:"
echo "  bash examples/11_diver_reopen.sh BOARDS=$BEST OUT=$RUN_DIR/reopened.csv SECONDS_PER_BOARD=600 THREADS=$THREADS"
echo "  python3 src/C_tail/E555_ender.py $SEED $BEST $RUN_DIR/closed.csv --profile deep --threads $THREADS"
