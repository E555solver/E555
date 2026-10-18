#!/bin/bash
# run_halo.sh -- grow partial boards, lift the halo around their core, search
# the halo and the empty top together, keep the best few percent. Repeat.
#
#   bash pipeline/run_halo.sh
#   bash pipeline/run_halo.sh TOTAL_TIME=8h THREADS=16 DB_FILE=/tmp/E555.db
#   bash pipeline/run_halo.sh RUN_DIR=halo2 SAMPLES=20 BT_TIME=2 ELITE_PCT=2
#
# One round:
#   A  E555_beamer --random_edges: SAMPLES bottoms x COLUMNS left columns, a beam
#      BEAM_WIDTH wide to STOP_ROW, every clue held, no backtracking and no
#      extension above the stop row.
#   B  E555_backtracker on every board: HOLES lifts rows 0-2 and columns 0, 1,
#      14 and 15 (the frame and the ring inside it), the clues go back on, and
#      an exact search (--breaks 0, --order frontier) grows outward from the
#      kept core over the lifted halo and the empty top at once, BT_TIME seconds
#      a board. Each board comes back as the deepest prefix of that order the
#      search reached, break-free. It rarely keeps the beam's own halo: row 11
#      shares the first layer with the halo's inner ring, and the frame waits
#      for the second (pipeline/README.md, "Why the search does not keep ...").
#   C  The threshold N is the score at the best ELITE_PCT% of every board scored
#      so far, this round's included; E555_rank.py --split appends the round's
#      boards scoring N or more to elite.csv. The rest are deleted, with the
#      beam's boards.
#   D  Back to A while TOTAL_TIME lasts.
#
# Why: the bottom rows and the side columns may hold the pieces the top needs.
# Lifting them gives those pieces back, and the search decides where they go.
#
# What RUN_DIR holds afterwards:
#   elite.csv     the kept boards, round after round (canonical rows)
#   halo.log      one line per round: what each step made, the threshold
#   scores.txt    "score count" of every board scored so far: the threshold's base
#   logs/         each round's beamer and backtracker logs, gzipped (KEEP_LOGS=1)
# A rerun with the same RUN_DIR carries on: round numbers, scores and elite.
# Ctrl-C is safe: the running tool stops cleanly and the run ends. Stopped in
# B, the boards already searched are split as usual; stopped in A, the round's
# boards are dropped.
#
# The elite are break-free partial boards. Their score counts matched edges
# between placed pieces, before any dive, so it reads low. FINISH_DIVES=M dives
# the elite into elite_dived.csv once the loop is over. NEEDS ~8 GB RAM for the
# beamer. Measured settings and numbers: pipeline/README.md, "The halo".
set -uo pipefail        # not -e: a round that fails must not kill the run

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # SEED, HOLES, ROTATIONS: relative to REPO
RUN_DIR=halo_out                        # relative to where you START it
THREADS=$(nproc)
TOTAL_TIME=4h           # the whole loop: seconds, or 90m, 4h, 2d. A round still
                        # running at the end is cut short and harvested.
DB_FILE=chain.db        # the chain database cache, ~6 GB. A bare name lands in
                        # RUN_DIR; give an absolute path to share it between runs.
                        # The clue setting is appended (chain.db.all): a clued
                        # database leaves the clue pieces out. Empty = rebuild
                        # it in memory every round (~2 minutes each).
CLUES=all               # all | center | none -- held by the beam and put back
                        # by the backtracker

# A. beamer
ROTATIONS=              # empty = --random_edges. A Stage A rotations file
                        # instead: one border row per round, in turn, with
                        # SAMPLES bottoms each and LAMBDA_CORNERS
SAMPLES=10              # random bottoms per round (--samples; --top_bottoms
                        # with ROTATIONS). With BEAM_WIDTH, what sets a round's size
COLUMNS=5               # left columns per bottom (--top_columns)
BEAM_WIDTH=10000
STOP_ROW=10
BC_WINDOW=7,7
POOL=64                 # --pool_factor
TAU=0.1                 # --tau_bottoms and --tau_columns
EXHAUST_INNER=0         # --exhaust_inner_color N. Measured at stop row 10 with no
                        # backtracking: N=1 wrote 21 boards from 480 configurations
                        # in 7 minutes, where N=0 writes ~10000 from 50 in one
                        # (pipeline/README.md, "The halo")
LAMBDA_CORNERS=0.5      # --lambda_corners, with ROTATIONS only: the beamer
                        # refuses it with --random_edges. 0 = off
RNG_SEED=0              # 0 = a fresh seed every round; S = S, S+1, ... per round.
                        # The seed names the round's boards (h<seed>_rndb...).
BEAM_WALL=1800          # cap on one beamer call, seconds

# B. backtracker
HOLES=data/holes_halo.csv
BT_ORDER=frontier       # --order. mrv puts more pieces on the top (44 top cells
                        # against 20 on average), at the halo's expense
BT_TIME=0.5             # seconds per board (--time_limit). Half the searches
                        # finish their whole tree by then, and more boards beat
                        # longer searches: 2 s found 25% more boards scoring 298+
                        # in 3 times the time
BT_EXTRA=               # more backtracker flags, e.g. BT_EXTRA="--no_hall"

# C. elite
ELITE_PCT=5             # keep the best ELITE_PCT% of every board scored so far
ELITE_SCORE=0           # > 0: a fixed threshold instead of ELITE_PCT
KEEP_LOGS=1             # 1 = keep each round's logs, gzipped, in RUN_DIR/logs

# After the loop (not counted in TOTAL_TIME)
FINISH_DIVES=0          # > 0: dive every elite board (E555_diver --end_dive M)
FINISH_POLISH=2000      # --end_polish of those dives
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done

die() { echo "$*" >&2; exit 1; }
[[ $TOTAL_TIME =~ ^([0-9]+)([smhd]?)$ ]] ||
    die "TOTAL_TIME must be seconds, or a whole number with s, m, h or d: $TOTAL_TIME"
case "${BASH_REMATCH[2]}" in
    d) TOTAL_S=$(( BASH_REMATCH[1] * 86400 )) ;;
    h) TOTAL_S=$(( BASH_REMATCH[1] * 3600 )) ;;
    m) TOTAL_S=$(( BASH_REMATCH[1] * 60 )) ;;
    *) TOTAL_S=${BASH_REMATCH[1]} ;;
esac
case "$CLUES" in
    all|1)   CLUES=all;    CLUE_ARG=(--clue_center --clue_corners) ;;
    center)  CLUE_ARG=(--clue_center) ;;
    none|0)  CLUES=none;   CLUE_ARG=() ;;
    *) die "CLUES must be all, center or none (got: $CLUES)" ;;
esac
for v in SAMPLES COLUMNS BEAM_WIDTH STOP_ROW POOL EXHAUST_INNER BEAM_WALL \
         ELITE_SCORE KEEP_LOGS FINISH_DIVES FINISH_POLISH RNG_SEED; do
    [[ ${!v} =~ ^[0-9]+$ ]] || die "$v must be a whole number (got: ${!v})"
done
[[ $ELITE_PCT =~ ^[0-9]+([.][0-9]+)?$ ]] && awk -v p="$ELITE_PCT" 'BEGIN { exit !(p > 0 && p <= 100) }' ||
    die "ELITE_PCT must be a percentage above 0 and at most 100 (got: $ELITE_PCT)"
[ "$SAMPLES" -ge 1 ] || die "SAMPLES must be at least 1: --samples 0 never ends"

case "$RUN_DIR" in /*) ;; *) RUN_DIR="$PWD/$RUN_DIR" ;; esac
cd "$REPO" || die "REPO=$REPO does not exist"
[ -d bin ] && [ -d tools ] || die "REPO=$REPO is not an E555 checkout -- set REPO at the top"
[ -x bin/E555_beamer ] && [ -x bin/E555_backtracker ] && [ -x bin/E555_diver ] || make ||
    die "the build failed; try make ARCH=generic"
[ -f "$SEED" ]  || die "seed file not found: $SEED"
[ -f "$HOLES" ] || die "holes mask not found: $HOLES"
SEED=$PWD/$SEED; HOLES=$PWD/$HOLES
NROT=0
if [ -n "$ROTATIONS" ]; then
    [ -s "$ROTATIONS" ] || die "rotations file not found: $ROTATIONS"
    ROTATIONS=$PWD/$ROTATIONS
    NROT=$(awk '!/^[ \t]*([#%]|$)/' "$ROTATIONS" | grep -c .)
    [ "$NROT" -ge 1 ] || die "no border rows in $ROTATIONS"
fi

mkdir -p "$RUN_DIR/logs" || die "cannot create RUN_DIR=$RUN_DIR"
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
else
    DB_ARG=()
fi

ELITE=$RUN_DIR/elite.csv
LOG=$RUN_DIR/halo.log
SCORES=$RUN_DIR/scores.txt
[ -s "$ELITE" ] || echo "# run_halo.sh elite: break-free halo boards, canonical rows," \
                        "score = matched edges before any dive" > "$ELITE"
touch "$SCORES"

# The round count goes on from the last run in this RUN_DIR.
ROUND=0
[ -s "$LOG" ] && ROUND=$(awk '$1 == "round" { n = $2 } END { print n + 0 }' "$LOG")
rm -rf "$RUN_DIR"/round_*       # a round a previous run was killed in

hms()  { local s=$1; if [ "$s" -ge 3600 ]; then printf '%dh%02dm' $((s/3600)) $((s%3600/60))
         elif [ "$s" -ge 60 ]; then printf '%dm%02ds' $((s/60)) $((s%60)); else printf '%ds' "$s"; fi; }
rows() { awk '!/^[ \t]*([#%]|$)/' "$@" 2>/dev/null | grep -c . ; }

# distinct FILE... -- the boards, one per distinct board after the HOLES lift,
# each renamed <config id>_<beam index>. The beam writes many boards that differ
# only in the lifted cells; the backtracker would write each repeat back as its
# bare lifted core, unsearched, and those would skew the elite threshold.
distinct() {
    awk -F, -v OFS=, -v mask="$HOLES" '
        BEGIN { r = 0
                while ((getline line < mask) > 0) {
                    if (line ~ /^[ \t]*[#%]/ || line !~ /[01]/) continue
                    gsub(/[ \t\r]/, "", line)
                    n = split(line, f, ",")
                    for (c = 1; c <= n; c++) lift[r * 16 + c - 1] = (f[c] == 1)
                    r++ } }
        /^[ \t]*[#%]/ || NF < 514 { next }
        { k = ""
          for (p = 0; p < 256; p++) {
              cell = $(NF - 511 + p) + 0
              if (cell != 999 && !lift[cell]) k = k p ":" cell ":" ($(NF - 255 + p) + 0) " " }
          if (k in seen) next
          seen[k] = 1
          id = $1; gsub(/[ \t]/, "", id); $1 = id "_" ($2 + 0)
          print }' "$@"
}

# add_scores FILE -- fold the scores (field 2) of FILE's boards into scores.txt,
# kept as "score count", best first.
add_scores() {
    { cat "$SCORES"
      awk -F, '!/^[ \t]*[#%]/ && NF >= 514 { print $2 + 0, 1 }' "$1"
    } | awk '{ n[$1] += $2 } END { for (s in n) print s, n[s] }' |
        sort -k1,1nr > "$SCORES.tmp" && mv "$SCORES.tmp" "$SCORES"
}
# threshold -- the score of the board at the best ELITE_PCT% of scores.txt, so a
# round only adds the boards that rank in the top ELITE_PCT% of the whole run.
threshold() {
    [ "$ELITE_SCORE" -gt 0 ] && { echo "$ELITE_SCORE"; return; }
    awk -v pct="$ELITE_PCT" '{ s[NR] = $1; c[NR] = $2; tot += $2 }
        END { k = tot * pct / 100; if (k < 1) k = 1
              for (i = 1; i <= NR; i++) { acc += c[i]; if (acc >= k) { print s[i]; exit } }
              print 0 }' "$SCORES"
}

STOP=0
trap 'STOP=1' INT TERM
DEADLINE=$(( SECONDS + TOTAL_S ))
left() { echo $(( DEADLINE - SECONDS )); }

# run_tool SECS CMD... -- run CMD and return its status; SECS > 0 sends it
# SIGTERM after SECS seconds. CMD runs in the background and is waited for:
# a Ctrl-C then reaches it once (the C tools stop cleanly on a first signal and
# die on a second, which timeout(1) would forward) and also interrupts the wait,
# so the trap above ends the run. A foreground tool that handles the signal and
# exits normally makes bash drop it, and the loop would go on.
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

BEAM=(--stop_row "$STOP_ROW" --beam_width "$BEAM_WIDTH" --bc_window "$BC_WINDOW"
      --pool_factor "$POOL" --top_columns "$COLUMNS" --tau_bottoms "$TAU"
      --tau_columns "$TAU" --exhaust_inner_color "$EXHAUST_INNER" "${CLUE_ARG[@]}"
      "${DB_ARG[@]}" --threads "$THREADS" --print_cmd)
read -r -a BT_MORE <<< "$BT_EXTRA"
BT=(--holes "$HOLES" "${CLUE_ARG[@]}" --breaks 0 --order "$BT_ORDER"
    --time_limit "$BT_TIME" --threads "$THREADS" --print_cmd "${BT_MORE[@]}")

echo "[halo] $RUN_DIR: $THREADS threads, $(hms "$TOTAL_S"), clues $CLUES, db ${DB_FILE:-<in memory>}"
if [ -n "$ROTATIONS" ]; then
    echo "[halo] A: $ROTATIONS, one of its $NROT border rows a round, $SAMPLES bottoms x $COLUMNS columns"
else
    echo "[halo] A: random borders, $SAMPLES bottoms x $COLUMNS columns"
    [ "$LAMBDA_CORNERS" = 0 ] ||
        echo "[halo]    (--lambda_corners left out: the beamer refuses it with --random_edges)"
fi
echo "[halo]    width $BEAM_WIDTH to row $STOP_ROW, bc_window $BC_WINDOW, pool $POOL, tau $TAU, exhaust_inner $EXHAUST_INNER"
echo "[halo] B: $(basename "$HOLES") lifted, --order $BT_ORDER --breaks 0, ${BT_TIME}s a board${BT_EXTRA:+, $BT_EXTRA}"
if [ "$ELITE_SCORE" -gt 0 ]; then echo "[halo] C: elite = boards scoring $ELITE_SCORE or more -> $ELITE"
else echo "[halo] C: elite = the best $ELITE_PCT% of every board scored -> $ELITE"; fi
[ -z "$DB_FILE" ] || [ -s "$DB_FILE" ] ||
    echo "[halo] the first round builds the chain database ($DB_FILE), about 2 minutes on 4 threads"
[ "$ROUND" -eq 0 ] || echo "[halo] going on from round $ROUND: $(rows "$ELITE") elite boards so far"
echo "[halo] $(date '+%F %T') start, $(hms "$TOTAL_S")" >> "$LOG"

SEARCHED=0; KEPT=0; DONE_ROUNDS=0; FAILS=0
while [ "$STOP" = 0 ] && [ "$(left)" -ge 60 ]; do
    ROUND=$(( ROUND + 1 ))
    RD=$RUN_DIR/round_$ROUND
    rm -rf "$RD"; mkdir -p "$RD"
    if [ "$RNG_SEED" = 0 ]; then RS=$(( ((RANDOM << 15) | RANDOM) + 1 ))
    else RS=$(( RNG_SEED + ROUND - 1 )); fi
    TAG="round $ROUND seed $RS"

    # -- A: the beam. It gets at most half of what is left, so B always runs.
    BW=$(( $(left) / 2 )); [ "$BW" -le "$BEAM_WALL" ] || BW=$BEAM_WALL
    if [ -n "$ROTATIONS" ]; then
        BORDER=("$ROTATIONS" --start_row $(( (ROUND - 1) % NROT )) --num_rows 1
                --top_bottoms "$SAMPLES")
        [ "$LAMBDA_CORNERS" = 0 ] || BORDER+=(--lambda_corners "$LAMBDA_CORNERS")
    else
        BORDER=(--random_edges --samples "$SAMPLES")
    fi
    T0=$SECONDS
    run_tool 0 bin/E555_beamer "$SEED" "${BORDER[@]}" "${BEAM[@]}" --rng_seed "$RS" \
        --prefix "h$RS" --wall_time "$BW" --out_dir "$RD/beam" > "$RD/beam.log" 2>&1
    RC=$?
    TA=$(( SECONDS - T0 ))
    BOARDS=()
    while read -r f; do
        case "$f" in *.csv) [ -s "$f" ] && BOARDS+=("$f") ;; esac
    done < "$RD/beam/outputs.txt" 2>/dev/null
    NB=0; [ "${#BOARDS[@]}" -eq 0 ] || NB=$(rows "${BOARDS[@]}")
    CFG=$(sed -n 's/^\[sum\] configs: \([0-9]*\),.*; \([0-9]*\) reached .*/\1 configs, \2 reached the stop row/p' "$RD/beam.log" | tail -1)
    A="A $(hms $TA): ${CFG:-? configs}, $NB boards"
    if [ "$RC" != 0 ] && [ "$STOP" = 0 ]; then
        FAILS=$(( FAILS + 1 ))
        cp "$RD/beam.log" "$RUN_DIR/logs/round_${ROUND}_beam_FAILED.log"
        echo "[halo] $TAG: the beamer FAILED (exit $RC); its log is in $RUN_DIR/logs"
        echo "round $ROUND seed $RS beamer_exit=$RC" >> "$LOG"
        rm -rf "$RD"
        [ "$FAILS" -lt 3 ] || { echo "[halo] three rounds failed in a row: stopping"; break; }
        continue
    fi
    FAILS=0

    # -- B: the halo search, cut short (cleanly, by SIGTERM) at the deadline
    B="B skipped"; NS=0
    if [ "$NB" -gt 0 ] && [ "$STOP" = 0 ] && [ "$(left)" -gt 30 ]; then
        distinct "${BOARDS[@]}" > "$RD/boards.csv"
        ND=$(rows "$RD/boards.csv")
        rm -rf "$RD/beam"/*.csv
        T0=$SECONDS
        run_tool $(( $(left) - 20 )) bin/E555_backtracker "$SEED" "$RD/boards.csv" \
            "$RD/halo.csv" "${BT[@]}" > "$RD/bt.log" 2>&1
        RC=$?
        TB=$(( SECONDS - T0 ))
        rm -f "$RD/boards.csv"
        NS=$(rows "$RD/halo.csv")
        LIM=$(grep -c '\] time_limit ' "$RD/bt.log")
        STAT=$(awk -F, '!/^[ \t]*[#%]/ && NF >= 514 { n++; s += $2; if ($2 + 0 > m) m = $2 + 0 }
                        END { if (n) printf "mean %.1f best %d", s / n, m }' "$RD/halo.csv" 2>/dev/null)
        B="B $(hms $TB): $NS of $ND distinct boards searched ($LIM to the limit)${STAT:+, $STAT}"
        [ "$RC" = 0 ] || [ "$STOP" = 1 ] || B="$B, backtracker exit $RC"
        [ "$(left)" -gt 20 ] || B="$B, cut at the deadline"
    fi

    # -- C: the elite
    C="C: nothing to split"
    if [ "$NS" -gt 0 ]; then
        add_scores "$RD/halo.csv"
        N=$(threshold)
        TOT=$(awk '{ t += $2 } END { print t + 0 }' "$SCORES")
        python3 tools/E555_rank.py "$RD/halo.csv" --seed_file "$SEED" --quiet \
            --split score "$N" "$RD/elite_new.csv" "$RD/below.csv"
        NE=$(rows "$RD/elite_new.csv")
        cat "$RD/elite_new.csv" >> "$ELITE" 2>/dev/null
        SEARCHED=$(( SEARCHED + NS )); KEPT=$(( KEPT + NE ))
        if [ "$ELITE_SCORE" -gt 0 ]; then C="C: N=$N"
        else C="C: N=$N (best $ELITE_PCT% of $TOT)"; fi
        C="$C -> +$NE elite ($(rows "$ELITE"))"
    fi
    DONE_ROUNDS=$(( DONE_ROUNDS + 1 ))
    echo "[halo] $TAG | $A | $B | $C | $(hms $(( $(left) > 0 ? $(left) : 0 ))) left"
    echo "round $ROUND seed $RS boards=$NB searched=$NS ${C#C: } $(date '+%F %T')" >> "$LOG"

    if [ "$KEEP_LOGS" = 1 ]; then
        for l in beam bt; do
            [ -s "$RD/$l.log" ] && gzip -c "$RD/$l.log" > "$RUN_DIR/logs/round_${ROUND}_$l.log.gz"
        done
    fi
    rm -rf "$RD"            # the beam's boards and those below N: deleted for good
done

echo
[ "$STOP" = 1 ] && echo "[halo] interrupted."
echo "[halo] $DONE_ROUNDS round(s), $SEARCHED boards searched, $KEPT kept; elite: $(rows "$ELITE") boards in $ELITE"
echo "[halo] $(date '+%F %T') end, $DONE_ROUNDS rounds, elite $(rows "$ELITE")" >> "$LOG"
[ "$(rows "$ELITE")" -gt 0 ] || exit 0
python3 tools/E555_rank.py "$ELITE" --seed_file "$SEED" --top 5

if [ "$FINISH_DIVES" -gt 0 ] && [ "$STOP" = 0 ]; then
    echo
    echo "[halo] finishing the elite: $FINISH_DIVES dives and $FINISH_POLISH polish rounds a board"
    T0=$SECONDS
    bin/E555_diver "$SEED" "$ELITE" "$RUN_DIR/elite_dived.csv" --end_dive "$FINISH_DIVES" \
        --end_polish "$FINISH_POLISH" --emit_score 0 --threads "$THREADS" --print_cmd \
        > "$RUN_DIR/logs/finish.log" 2>&1 ||
        { tail -5 "$RUN_DIR/logs/finish.log"; die "the diver failed; its log: $RUN_DIR/logs/finish.log"; }
    echo "[halo] dived in $(hms $(( SECONDS - T0 ))) -> $RUN_DIR/elite_dived.csv"
    python3 tools/E555_rank.py "$RUN_DIR/elite_dived.csv" --seed_file "$SEED" --top 5
fi
