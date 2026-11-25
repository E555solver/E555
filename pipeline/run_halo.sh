#!/bin/bash
# run_halo.sh -- grow L-band boards to row 12, keep every one, and fill the rest
# of each board: by diver dives (the default) or by a frontier ratchet allowed
# BREAKS broken edges, with or without the halo around the core lifted first.
# The best complete boards of each round are polished at length. Repeat.
#
#   bash pipeline/run_halo.sh
#   bash pipeline/run_halo.sh TOTAL_TIME=2d THREADS=32 DB_FILE=/data/E555.db
#   bash pipeline/run_halo.sh LIFT=both                  # dive both ways
#   bash pipeline/run_halo.sh FILL=ratchet BREAKS=64     # the ratchet, halo lifted
#   bash pipeline/run_halo.sh HALO=3 RUN_DIR=halo3
#
# HALO (2 or 3) is the halo's width: the beam leaves the right HALO columns
# open, and a lift empties data/holes_halo<HALO>.csv. One round:
#   A  E555_beamer --random_edges: up to SAMPLES bottoms x COLUMNS left columns,
#      a beam BEAM_WIDTH wide to BACKTRACK_ROW, then an exhaustive search to
#      STOP_ROW over columns 0..15-HALO (--backtrack_unlock_right HALO, no
#      extension): break-free blocks, rows 0-12. The beam stops once it has
#      written BOARDS boards. KEEP_BEAM=1 appends them, as the beam wrote them,
#      to RUN_DIR/beam.csv.gz.
#   B  Every board filled once per LIFT: none fills the open band as the beam
#      left it; halo first lifts the halo (HALO=2: rows 0-1 and 14-15, columns
#      0-1 and 14-15, 112 cells, around the 12 x 12 core; HALO=3: three wide,
#      around 10 x 10), and the fill rebuilds the frame, the band and the top
#      together around the core; both does each. Lifted boards are named
#      <id>_lift.
#        FILL=dive     E555_diver --end_dive DIVES --end_polish DIVE_POLISH:
#                      every board complete, in about 0.1 s a board.
#        FILL=ratchet  E555_backtracker --break_mode ratchet --order frontier:
#                      an exact search, a break where it is stuck, until the
#                      board is complete, BREAKS breaks are spent, or its cuts
#                      prove the rest cannot be finished within them.
#   C  The POLISH_TOP best complete boards of the round get a long E555_diver
#      --reopen auto polish of POLISH_TIME seconds each; complete.csv keeps the
#      COMPLETE_KEEP best complete boards, best.csv the BEST_KEEP best of every
#      fill, complete or not.
# Then back to A while TOTAL_TIME lasts.
#
# Measured on 4 threads, HALO=2, stop row 12 (pipeline/README.md):
#   - 405 boards dived (5000 dives + 2000 polish): unlifted 453.7 mean (max
#     458), halo lifted 450.2 (457); the lift lost on 371 of them. Lifting only
#     rows 0-1, columns 0-1 or the frame cost 0.4-0.8. Hence LIFT=none.
#   - The ratchet on 12 of them: BREAKS=16 or 20 completes none (the halo needs
#     32-41 breaks); BREAKS=64 completes all at 442.0 mean (~5 s a board on
#     one thread), 447.8 after a 25 s polish, where the lifted dives reach 451.5
#     and 453.3.
#     250 rollouts instead of 32: 8x the time, +1.4 raw, nothing after polish.
#   - Dive scores do not rank these boards (two runs of one dive screen agree
#     at Spearman +0.02 to +0.12), so the beam's boards are not screened.
#
# What RUN_DIR holds afterwards:
#   complete.csv  the COMPLETE_KEEP best complete boards, polished ones included
#   best.csv      the BEST_KEEP best boards of every fill, complete or not
#   beam.csv.gz   every board the beam wrote, unlifted and unfilled (KEEP_BEAM=1);
#                 zcat it to a .csv for the tools
#   halo.log      one line per round
#   logs/         each round's logs, gzipped (KEEP_LOGS=1)
# A rerun with the same RUN_DIR carries on: the round count and the files above.
# Ctrl-C is safe: the running tool stops cleanly, what it finished is kept (a
# board whose polish was stopped is kept as the fill wrote it), and the run ends.
# NEEDS ~8 GB RAM for the beamer, ~7 GB of disk for DB_FILE, bash 4.4+, GNU sed.
set -uo pipefail        # not -e: a round that fails must not kill the run

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # relative to REPO
RUN_DIR=halo_out                        # relative to where you START it
THREADS=$(nproc)
TOTAL_TIME=4h           # the whole loop: seconds, or 90m, 4h, 2d. A step still
                        # running at the end is stopped and harvested.
DB_FILE=chain.db        # the chain database cache, ~6.5 GB. A bare name lands in
                        # RUN_DIR; give an absolute path to share it between runs.
                        # The clue setting is appended (chain.db.center): a clued
                        # database leaves the clue pieces out. Empty = rebuild
                        # it in memory every round (~2 minutes each on 4 threads).
CLUES=center            # center | all | none -- held by the beam and put back by
                        # the ratchet. FILL=dive needs center or none: a lift or
                        # the open band can hold a corner clue's cell.
RNG_SEED=0              # 0 = a fresh seed every round; S = S, S+1, ... per round.
                        # The seed names the round's boards (h<seed>_...).
HALO=2                  # 2 | 3: the halo's width. The beam leaves that many right
                        # columns open (--backtrack_unlock_right HALO), and a lift
                        # empties data/holes_halo<HALO>.csv: HALO=3 keeps a
                        # 10 x 10 core, HALO=2 keeps 12 x 12

# A. beam (E555_beamer): break-free blocks, rows 0..STOP_ROW x columns 0..15-HALO
SAMPLES=16              # --samples: random bottom rows per round, at most: the beam
COLUMNS=4               # --top_columns: left columns per bottom } stops as soon as
                        # it has written BOARDS boards
BEAM_WIDTH=10000
BACKTRACK_ROW=          # --backtrack_row: rows BACKTRACK_ROW+1..STOP_ROW exhaustive;
                        # empty = 10 for HALO=3, 8 for HALO=2. Measured boards per
                        # configuration at STOP_ROW=12: HALO=3 from row 10 ~80, from
                        # row 9 ~7000; HALO=2 from row 8 ~320, from 9 ~20, from 10 ~0
STOP_ROW=12             # 13 costs ~2000x more beam time per board (HALO=2, from
                        # row 6: 57 boards in 193G nodes on 24 threads, 20 min)
BC_WINDOW=7,7
POOL=64                 # --pool_factor
TAU=0.1                 # --tau_bottoms and --tau_columns
EXHAUST_INNER=0         # --exhaust_inner_color N: mark N colours per border, bias the beam, cut at S-2/S-1 (PROJECT 5.9)
BEAM_WALL=1800          # cap on one beamer call, seconds
BEAM_EXTRA=             # more beamer flags
BOARDS=                 # boards a round: the beam stops once it has written them
                        # (--max_emitted) and B takes them all; empty = 100 per
                        # thread
KEEP_BEAM=1             # 1 = append every beam board to RUN_DIR/beam.csv.gz

# B. fill
FILL=dive               # dive | ratchet
LIFT=                   # none | halo | both; empty = none for FILL=dive, halo
                        # for FILL=ratchet
DIVES=5000              # FILL=dive: --end_dive  } per board; 405 boards took
DIVE_POLISH=2000        # FILL=dive: --end_polish} 36 s unlifted, 69 s lifted (4 thr.)
DIVE_EXTRA=             # more diver flags for the fill
BREAKS=20               # FILL=ratchet: --breaks, the total break ceiling. A
                        # complete board then scores 480 - BREAKS or more; the
                        # lifted halo needs ~32-41, so 64 completes every board
RATCHET_ORDER=frontier  # --order: outward from the core, fewest fits first
RATCHET_NODES=1000000   # --ratchet_nodes: node cap of each exact search
ROLLOUTS=32             # --ratchet_rollouts: dives scoring each candidate break
RATCHET_TIME=120        # --time_limit per board, seconds: a guard only
RATCHET_EXTRA=          # more backtracker flags

# C. results
POLISH_TOP=2            # complete boards polished a round, the best first
POLISH_TIME=900         # seconds of E555_diver --reopen auto per polished board,
                        # on all threads; 0 = none
POLISH_DIVES=50000      # --end_dive    } per copy; 8 copies a round, rounds until
POLISH_STEPS=50000      # --end_polish  } POLISH_TIME (the 465's own re-dive)
POLISH_EXTRA=           # more diver flags, e.g. POLISH_EXTRA="--plateau"
RECORD=465              # a complete board above this is announced
COMPLETE_KEEP=1000      # boards in complete.csv
BEST_KEEP=100           # boards in best.csv
KEEP_LOGS=1             # 1 = keep each round's logs, gzipped, in RUN_DIR/logs
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) [[ -v ${arg%%=*} ]] ||
                          { echo "unknown setting ${arg%%=*}: the settings are at the top of $0" >&2; exit 1; }
                      declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done

die() { echo "$*" >&2; exit 1; }
[ "${BASH_VERSINFO[0]}" -gt 4 ] || { [ "${BASH_VERSINFO[0]}" -eq 4 ] && [ "${BASH_VERSINFO[1]}" -ge 4 ]; } ||
    die "bash 4.4 or newer is needed (empty arrays under set -u)"
[[ $THREADS =~ ^[0-9]+$ ]] && [ "$THREADS" -ge 1 ] || die "THREADS must be a whole number, at least 1 (got: $THREADS)"
: "${BOARDS:=$(( 100 * THREADS ))}"
[[ $TOTAL_TIME =~ ^([0-9]+)([smhd]?)$ ]] ||
    die "TOTAL_TIME must be seconds, or a whole number with s, m, h or d: $TOTAL_TIME"
case "${BASH_REMATCH[2]}" in
    d) TOTAL_S=$(( BASH_REMATCH[1] * 86400 )) ;;
    h) TOTAL_S=$(( BASH_REMATCH[1] * 3600 )) ;;
    m) TOTAL_S=$(( BASH_REMATCH[1] * 60 )) ;;
    *) TOTAL_S=${BASH_REMATCH[1]} ;;
esac
case "$CLUES" in
    all)    CLUE_ARG=(--clue_center --clue_corners) ;;
    center) CLUE_ARG=(--clue_center) ;;
    none)   CLUE_ARG=() ;;
    *) die "CLUES must be center, all or none (got: $CLUES)" ;;
esac
case "$HALO" in
    2|3) HOLES=data/holes_halo$HALO.csv; : "${BACKTRACK_ROW:=$(( HALO == 3 ? 10 : 8 ))}" ;;
    *) die "HALO must be 2 or 3 (got: $HALO)" ;;
esac
case "$FILL" in
    dive)    : "${LIFT:=none}"
             [ "$CLUES" != all ] ||
                 die "FILL=dive does not put corner clues back: use CLUES=center, or FILL=ratchet" ;;
    ratchet) : "${LIFT:=halo}" ;;
    *) die "FILL must be dive or ratchet (got: $FILL)" ;;
esac
case "$LIFT" in
    none|halo) LIFTS=("$LIFT") ;;
    both)      LIFTS=(none halo) ;;
    *) die "LIFT must be none, halo or both (got: $LIFT)" ;;
esac
for v in SAMPLES COLUMNS BEAM_WIDTH BACKTRACK_ROW STOP_ROW POOL EXHAUST_INNER BEAM_WALL BOARDS \
         KEEP_BEAM DIVES DIVE_POLISH BREAKS RATCHET_NODES ROLLOUTS RATCHET_TIME POLISH_TOP \
         POLISH_TIME POLISH_DIVES POLISH_STEPS RECORD COMPLETE_KEEP BEST_KEEP KEEP_LOGS RNG_SEED; do
    [[ ${!v} =~ ^[0-9]+$ ]] || die "$v must be a whole number (got: ${!v})"
done
[[ $TAU =~ ^[0-9]+([.][0-9]+)?$ ]] || die "TAU must be a number, 0 or more (got: $TAU)"
for v in SAMPLES COLUMNS BOARDS DIVES BREAKS RATCHET_TIME POLISH_DIVES COMPLETE_KEEP BEST_KEEP; do
    [ "${!v}" -ge 1 ] || die "$v must be at least 1"
done
[ "$BACKTRACK_ROW" -ge 1 ] && [ "$BACKTRACK_ROW" -lt "$STOP_ROW" ] ||
    die "BACKTRACK_ROW must be at least 1 and below STOP_ROW=$STOP_ROW"
[ "$KEEP_LOGS" -le 1 ] && [ "$KEEP_BEAM" -le 1 ] || die "KEEP_LOGS and KEEP_BEAM must be 0 or 1"

case "$RUN_DIR" in /*) ;; *) RUN_DIR="$PWD/$RUN_DIR" ;; esac
cd "$REPO" || die "REPO=$REPO does not exist"
[ -d bin ] && [ -d tools ] || die "REPO=$REPO is not an E555 checkout -- set REPO at the top"
make -s beamer backtracker diver > /dev/null || die "the build failed: run make in $REPO (make ARCH=generic in a container)"
[ -f "$SEED" ]  || die "seed file not found: $SEED"
[ -f "$HOLES" ] || die "holes mask not found: $HOLES"
SEED=$(readlink -f -- "$SEED"); HOLES=$(readlink -f -- "$HOLES")
mkdir -p "$RUN_DIR/logs" || die "cannot create RUN_DIR=$RUN_DIR"
DB_ARG=()
if [ -n "$DB_FILE" ]; then
    case "$DB_FILE" in /*) ;; *) DB_FILE="$RUN_DIR/$DB_FILE" ;; esac
    [ "$CLUES" = none ] || DB_FILE="$DB_FILE.$CLUES"
    mkdir -p "$(dirname "$DB_FILE")"
    if [ ! -s "$DB_FILE" ]; then
        FREE_GB=$(df -BG --output=avail "$(dirname "$DB_FILE")" 2>/dev/null | tail -1 | tr -dc 0-9)
        [ "${FREE_GB:-99}" -ge 7 ] ||
            echo "[warn] only ${FREE_GB}G free for $DB_FILE; the chain database needs about 6.5G"
    fi
    DB_ARG=(--db_file "$DB_FILE")
fi

COMPLETE=$RUN_DIR/complete.csv
BEST=$RUN_DIR/best.csv
BEAM_GZ=$RUN_DIR/beam.csv.gz
LOG=$RUN_DIR/halo.log
touch "$COMPLETE" "$BEST" "$LOG"
# The round count goes on from the last run in this RUN_DIR.
ROUND=$(sed -n 's/^round \([0-9][0-9]*\) .*/\1/p' "$LOG" | tail -1)
ROUND=${ROUND:-0}
rm -rf "$RUN_DIR"/round_*       # a round a previous run was killed in

hms()   { local s=$1; if [ "$s" -ge 3600 ]; then printf '%dh%02dm' $((s/3600)) $((s%3600/60))
          elif [ "$s" -ge 60 ]; then printf '%dm%02ds' $((s/60)) $((s%60)); else printf '%ds' "$s"; fi; }
count() { if [ -s "$1" ]; then python3 tools/E555_rank.py "$1" --count; else echo 0; fi; }
# best FILE MEASURE -- MEASURE (score, placed) of the best board by it; - for none
best()  { local v=; [ "$(count "$1")" -eq 0 ] ||
              v=$(python3 tools/E555_rank.py "$1" --seed_file "$SEED" --sort "$2" --field "$2"); echo "${v:--}"; }
# mean FILE -- the mean score of FILE's boards, one decimal; - for none
mean()  { local v=; [ "$(count "$1")" -eq 0 ] ||
              v=$(python3 tools/E555_rank.py "$1" --seed_file "$SEED" --csv 2>/dev/null |
                  awk -F, 'NR > 1 { s += $4; n++ } END { if (n) printf "%.1f", s / n }'); echo "${v:--}"; }
# merge OUT ARGS... FILE... -- rebuild OUT from itself and the FILEs that hold boards,
# best first, one row per distinct board
merge() {
    local out=$1 f in=() args=(); shift
    while [ "$#" -gt 0 ] && [ "${1#--}" != "$1" ]; do args+=("$1" "$2"); shift 2; done
    for f in "$out" "$@"; do [ "$(count "$f")" -eq 0 ] || in+=("$f"); done
    [ "${#in[@]}" -gt 0 ] || return 0
    python3 tools/E555_rank.py "${in[@]}" --seed_file "$SEED" --sort score --unique "${args[@]}" \
        --quiet --out "$out.tmp" > /dev/null 2> "$RUN_DIR/rank.log" && mv "$out.tmp" "$out" ||
        echo "[warn] $(basename "$out") not rebuilt: $(tail -1 "$RUN_DIR/rank.log")"
}

STOP=0
trap 'STOP=1' INT TERM
DEADLINE=$(( SECONDS + TOTAL_S ))
left() { echo $(( DEADLINE - SECONDS )); }

# run_tool SECS CMD... -- run CMD and return its status; SECS > 0 sends it
# SIGTERM after SECS seconds. CMD runs in the background and is waited for:
# a Ctrl-C then reaches it once (the C tools stop cleanly on a first signal and
# die on a second, which timeout(1) would forward) and also interrupts the wait,
# so the trap above ends the run.
run_tool() {
    local secs=$1 pid wd= rc; shift
    "$@" &
    pid=$!
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
    return "$rc"
}

read -r -a BEAM_MORE <<< "$BEAM_EXTRA"
read -r -a DIVE_MORE <<< "$DIVE_EXTRA"
read -r -a RATCHET_MORE <<< "$RATCHET_EXTRA"
read -r -a POLISH_MORE <<< "$POLISH_EXTRA"
BEAM=(--random_edges --samples "$SAMPLES" --top_columns "$COLUMNS" --beam_width "$BEAM_WIDTH"
      --backtrack_row "$BACKTRACK_ROW" --stop_row "$STOP_ROW" --backtrack_unlock_right "$HALO"
      --extend_nodes 0 --bc_window "$BC_WINDOW" --pool_factor "$POOL" --tau_bottoms "$TAU"
      --tau_columns "$TAU" --exhaust_inner_color "$EXHAUST_INNER" --max_emitted "$BOARDS"
      "${CLUE_ARG[@]}" "${DB_ARG[@]}" --threads "$THREADS" --print_cmd "${BEAM_MORE[@]}")
DIVE=(--end_dive "$DIVES" --end_polish "$DIVE_POLISH" --emit_score 0 --threads "$THREADS"
      --print_cmd "${DIVE_MORE[@]}")
RATCHET=("${CLUE_ARG[@]}" --break_mode ratchet --breaks "$BREAKS"
         --order "$RATCHET_ORDER" --edge_supply --ratchet_nodes "$RATCHET_NODES"
         --ratchet_rollouts "$ROLLOUTS" --time_limit "$RATCHET_TIME" --threads "$THREADS"
         --print_cmd "${RATCHET_MORE[@]}")
POLISH=(--reopen auto --copies 8 --end_dive "$POLISH_DIVES" --end_polish "$POLISH_STEPS"
        --rounds 1000000 --emit_score 0 --threads "$THREADS" --print_cmd "${POLISH_MORE[@]}")
# An auto band is at most 5 deep: it never reaches the centre clue (7,7), but it
# can lift a corner clue, and the polish does not put clues back.
POLISHING=1
[ "$POLISH_TIME" -gt 0 ] && [ "$POLISH_TOP" -gt 0 ] && [ "$CLUES" != all ] || POLISHING=0

if [ "$FILL" = dive ]; then
    FILL_DESC="diver dives, $DIVES + $DIVE_POLISH polish a board${DIVE_EXTRA:+, $DIVE_EXTRA}"
else
    FILL_DESC="ratchet --breaks $BREAKS --order $RATCHET_ORDER, $RATCHET_NODES nodes a search,"
    FILL_DESC+=" $ROLLOUTS rollouts${RATCHET_EXTRA:+, $RATCHET_EXTRA}"
fi
echo "[halo] $RUN_DIR: $THREADS threads, $(hms "$TOTAL_S"), clues $CLUES, db ${DB_FILE:-<in memory>}"
echo "[halo] A: HALO=$HALO, random borders, up to $SAMPLES bottoms x $COLUMNS columns, width $BEAM_WIDTH," \
     "exhaustive from row $BACKTRACK_ROW to $STOP_ROW, the right $HALO columns left open;" \
     "the beam stops at $BOARDS boards$([ "$KEEP_BEAM" = 0 ] || echo ", all kept in $(basename "$BEAM_GZ")")"
echo "[halo] B: every board, lift ${LIFTS[*]} ($(basename "$HOLES")): $FILL_DESC"
if [ "$POLISHING" = 1 ]; then
    echo "[halo] C: the best $POLISH_TOP complete board(s) a round polished $(hms "$POLISH_TIME") each:" \
         "E555_diver --reopen auto, $POLISH_DIVES dives + $POLISH_STEPS polish rounds x 8 copies a round${POLISH_EXTRA:+, $POLISH_EXTRA}"
elif [ "$CLUES" = all ]; then
    echo "[halo] C: no polish under CLUES=all (an auto band can lift a corner clue)"
else
    echo "[halo] C: no polish (POLISH_TIME=0 or POLISH_TOP=0)"
fi
[ -z "$DB_FILE" ] || [ -s "$DB_FILE" ] ||
    echo "[halo] the first round builds the chain database ($DB_FILE), about 2 minutes on 4 threads"
[ "$ROUND" -eq 0 ] || echo "[halo] going on from round $ROUND: complete.csv best $(best "$COMPLETE" score)," \
                           "best.csv best $(best "$BEST" score)"
echo "[halo] $(date '+%F %T') start, $(hms "$TOTAL_S"), FILL=$FILL LIFT=$LIFT" >> "$LOG"

DONE_ROUNDS=0; FAILS=0
while [ "$STOP" = 0 ] && [ "$(left)" -ge 120 ]; do
    ROUND=$(( ROUND + 1 ))
    RD=$RUN_DIR/round_$ROUND
    rm -rf "$RD"; mkdir -p "$RD"
    if [ "$RNG_SEED" = 0 ]; then RS=$(( ((RANDOM << 15) | RANDOM) + 1 ))
    else RS=$(( RNG_SEED + ROUND - 1 )); fi
    TAG="round $ROUND seed $RS"

    # -- A: the beam. It gets at most a quarter of what is left; the exhaustive
    #    search of the configuration in hand can run past --wall_time, hence the guard.
    BW=$(( $(left) / 4 )); [ "$BW" -le "$BEAM_WALL" ] || BW=$BEAM_WALL
    T0=$SECONDS
    # The beamer memory-maps the chain database. Out of the page cache, its
    # lookups wait on the disk: one configuration took 6 minutes instead of 8 s.
    # Reading the file first takes ~20 s cold and ~1 s cached.
    [ -z "$DB_FILE" ] || [ ! -s "$DB_FILE" ] || cat "$DB_FILE" > /dev/null
    run_tool $(( BW + 120 )) bin/E555_beamer "$SEED" "${BEAM[@]}" --rng_seed "$RS" --prefix "h$RS" \
        --wall_time "$BW" --out_dir "$RD/beam" > "$RD/beam.log" 2>&1
    RC=$?
    # boards.csv: the beam's boards, comments dropped, each named <config id>_<beam
    # index> (every board of a configuration shares the config id).
    : > "$RD/boards.csv"
    if [ -s "$RD/beam/outputs.txt" ]; then
        while read -r f; do
            case "$f" in *.csv) [ -s "$f" ] && sed -E '/^[[:space:]]*([#%]|$)/d
                s/^[[:space:]]*([^,[:space:]]+)[[:space:]]*,[[:space:]]*([0-9]+)[[:space:]]*,/\1_\2,\2,/' \
                "$f" >> "$RD/boards.csv" ;; esac
        done < "$RD/beam/outputs.txt"
    fi
    rm -rf "$RD/beam"
    NB=$(count "$RD/boards.csv")
    [ "$KEEP_BEAM" = 0 ] || [ "$NB" -eq 0 ] || gzip -c "$RD/boards.csv" >> "$BEAM_GZ"
    CFG=$(sed -n 's/^\[sum\] configs: \([0-9]*\),.*; \([0-9]*\) reached .*/\1 configs, \2 reached the stop row/p' \
          "$RD/beam.log" | tail -1)
    A="A $(hms $(( SECONDS - T0 ))): ${CFG:-? configs}, $NB boards"
    if [ "$RC" != 0 ] && [ "$STOP" = 0 ] && [ "$NB" -eq 0 ]; then
        FAILS=$(( FAILS + 1 ))
        cp "$RD/beam.log" "$RUN_DIR/logs/round_${ROUND}_beam_FAILED.log"
        echo "[halo] $TAG: the beamer FAILED (exit $RC); its log is in $RUN_DIR/logs"
        echo "round $ROUND seed $RS beamer_exit=$RC" >> "$LOG"
        rm -rf "$RD"
        [ "$FAILS" -lt 3 ] || { echo "[halo] three rounds failed in a row: stopping"; break; }
        continue
    fi
    FAILS=0

    # -- B: every board filled once per lift; stopped by SIGTERM at the deadline,
    #    when the boards in hand are written as far as they got.
    B="B skipped"; NF=0; NC=0; DEEP=-; ENDED=; PER_LIFT=; BLOG=
    if [ "$NB" -gt 0 ] && [ "$STOP" = 0 ] && [ "$(left)" -gt 90 ]; then
        T0=$SECONDS
        B_PARTS=
        for L in "${LIFTS[@]}"; do
            [ "$STOP" = 0 ] && [ "$(left)" -gt 60 ] || break
            IN=$RD/boards.csv; LIFT_ARG=()
            if [ "$L" = halo ]; then        # named <id>_lift: the fills keep the ids
                IN=$RD/boards_lift.csv; LIFT_ARG=(--holes "$HOLES")
                sed -E 's/^([^,]*),/\1_lift,/' "$RD/boards.csv" > "$IN"
            fi
            OUT=$RD/fill_$L.csv
            if [ "$FILL" = dive ]; then
                run_tool $(( $(left) - 30 )) bin/E555_diver "$SEED" "$IN" "$OUT" "${LIFT_ARG[@]}" \
                    "${DIVE[@]}" --rng_seed "$RS" > "$RD/fill_$L.log" 2>&1
            else
                run_tool $(( $(left) - 30 )) bin/E555_backtracker "$SEED" "$IN" "$OUT" "${LIFT_ARG[@]}" \
                    "${RATCHET[@]}" > "$RD/fill_$L.log" 2>&1
            fi
            RC=$?
            BLOG+=" $RD/fill_$L.log"
            N=$(count "$OUT")
            [ "$N" -eq 0 ] || grep -hv '^[[:space:]]*[#%]' "$OUT" >> "$RD/filled.csv"
            PER_LIFT+=" fill_${L}_mean=$(mean "$OUT") fill_${L}_best=$(best "$OUT" score)"
            P="$L $N boards, mean $(mean "$OUT"), best $(best "$OUT" score)"
            [ "$RC" = 0 ] || [ "$STOP" = 1 ] || P="$P, exit $RC"
            B_PARTS+="${B_PARTS:+;} $P"
            rm -f "$RD/boards_lift.csv" "$OUT.outputs.txt"
        done
        NF=$(count "$RD/filled.csv")
        if [ "$NF" -gt 0 ]; then
            python3 tools/E555_rank.py "$RD/filled.csv" --seed_file "$SEED" --quiet \
                --split placed 256 "$RD/new_complete.csv" "$RD/partial.csv" > /dev/null
            NC=$(count "$RD/new_complete.csv")
            DEEP=$(best "$RD/filled.csv" placed)
        fi
        if [ "$FILL" = ratchet ]; then
            # how each board's ratchet ended, e.g. "ended_budget_spent=12 ended_complete=1"
            # shellcheck disable=SC2086
            ENDED=$(cat $BLOG | grep -o 'ended: [a-z ]*[a-z]' | sed 's/^ended: //; s/ /_/g' | sort | uniq -c |
                    sed -E 's/^ *([0-9]+) (.*)/ended_\2=\1/' | tr '\n' ' ')
        fi
        B="B $(hms $(( SECONDS - T0 ))):$B_PARTS; $NC complete, deepest $DEEP placed"
    fi
    rm -f "$RD/boards.csv" "$RD/filled.csv"

    # -- C: the POLISH_TOP best complete boards polished, each on its own clock
    #    (one diver call each, as --wall_time covers a whole call); the rest go
    #    to complete.csv as the fill wrote them
    C=
    if [ "$NC" -gt 0 ]; then
        merge "$RD/new_complete.csv"            # best first, repeats dropped
        NC=$(count "$RD/new_complete.csv")
        NP=0; [ "$POLISHING" = 0 ] || NP=$(( NC < POLISH_TOP ? NC : POLISH_TOP ))
        : > "$RD/polished.csv"
        for (( i = 0; i < NP; i++ )); do
            t=$(( $(left) - 60 )); [ "$t" -le "$POLISH_TIME" ] || t=$POLISH_TIME
            rm -f "$RD/one.csv"
            if [ "$STOP" = 0 ] && [ "$t" -ge 10 ]; then
                run_tool $(( t + 120 )) bin/E555_diver "$SEED" "$RD/new_complete.csv" "$RD/one.csv" \
                    --start_row "$i" --num_rows 1 "${POLISH[@]}" --wall_time "$t" --rng_seed "$RS" \
                    >> "$RD/polish.log" 2>&1
            fi
            # never worse than the fill's board; a stopped polish keeps that board
            if [ "$(count "$RD/one.csv")" -gt 0 ]; then cat "$RD/one.csv" >> "$RD/polished.csv"
            else grep -v '^[[:space:]]*[#%]' "$RD/new_complete.csv" | sed -n "$(( i + 1 ))p" >> "$RD/polished.csv"; fi
        done
        grep -v '^[[:space:]]*[#%]' "$RD/new_complete.csv" | tail -n +$(( NP + 1 )) > "$RD/rest.csv"
        merge "$COMPLETE" --top "$COMPLETE_KEEP" "$RD/polished.csv" "$RD/rest.csv"
        BC=$(best "$RD/new_complete.csv" score)
        C=" | C: $NC complete, best $BC"
        if [ "$NP" -gt 0 ]; then
            BP=$(best "$RD/polished.csv" score)
            C="$C, $NP polished to best $BP"
            [ "$BP" = - ] || [ "$BP" -le "$BC" ] || BC=$BP
        fi
        [ "$BC" -le "$RECORD" ] ||
            echo "[halo] *** $TAG: a complete board scores $BC, above RECORD=$RECORD ($COMPLETE) ***"
    fi
    merge "$BEST" --top "$BEST_KEEP" "$RD/partial.csv" "$RD/polished.csv" "$RD/rest.csv"

    DONE_ROUNDS=$(( DONE_ROUNDS + 1 ))
    echo "[halo] $TAG | $A | $B$C | complete.csv best $(best "$COMPLETE" score)," \
         "best.csv best $(best "$BEST" score) | $(hms $(( $(left) > 0 ? $(left) : 0 ))) left"
    echo "round $ROUND seed $RS boards=$NB filled=$NF complete=$NC deepest=$DEEP${PER_LIFT} ${ENDED}best_complete=$(best "$COMPLETE" score)" \
         "$(date '+%F %T')" >> "$LOG"

    if [ "$KEEP_LOGS" = 1 ]; then
        for f in "$RD"/beam.log "$RD"/fill_*.log "$RD"/polish.log; do
            [ -s "$f" ] && gzip -c "$f" > "$RUN_DIR/logs/round_${ROUND}_$(basename "$f").gz"
        done
    fi
    rm -rf "$RD"            # the round's work: deleted for good (beam.csv.gz keeps the boards)
done

echo
[ "$STOP" = 1 ] && echo "[halo] interrupted."
echo "[halo] $DONE_ROUNDS round(s); complete.csv $(count "$COMPLETE") board(s), best $(best "$COMPLETE" score);" \
     "best.csv best $(best "$BEST" score)"
[ "$KEEP_BEAM" = 0 ] || [ ! -s "$BEAM_GZ" ] ||
    echo "[halo] every beam board, unlifted and unfilled: $BEAM_GZ (zcat it to a .csv for the tools)"
echo "[halo] $(date '+%F %T') end, $DONE_ROUNDS rounds, complete $(count "$COMPLETE")" >> "$LOG"
if [ "$(count "$COMPLETE")" -gt 0 ]; then
    echo "[halo] the best complete boards ($COMPLETE):"
    python3 tools/E555_rank.py "$COMPLETE" --seed_file "$SEED" --sort score --top 5
elif [ "$(count "$BEST")" -gt 0 ]; then
    echo "[halo] no complete board yet; the best boards ($BEST):"
    python3 tools/E555_rank.py "$BEST" --seed_file "$SEED" --sort score --top 5
fi
