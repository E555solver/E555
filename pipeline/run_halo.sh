#!/bin/bash
# run_halo.sh -- grow partial boards, lift the halo around their core, and search
# the halo with the empty top for a board whose every inner piece is placed. The
# best few percent climb a ladder of ever longer searches. Repeat.
#
#   bash pipeline/run_halo.sh
#   bash pipeline/run_halo.sh TOTAL_TIME=8h THREADS=16 DB_FILE=/tmp/E555.db
#   bash pipeline/run_halo.sh RUN_DIR=halo2 MAX_BOARDS=20000 LADDER_TIME=1
#
# One round:
#   A  E555_beamer --random_edges: SAMPLES bottoms x COLUMNS left columns, a beam
#      BEAM_WIDTH wide, exhaustive from BACKTRACK_ROW to STOP_ROW, no extension.
#   B  E555_backtracker on up to MAX_BOARDS distinct boards: HOLES lifts rows
#      0-2 and columns 0, 1, 14, 15 (the frame and the ring inside it), and an
#      exact search (--breaks 0 --frame_last) fills the inner cells first and the
#      frame last, BT_TIME seconds a board. A board whose 196 inner cells are all
#      placed is released at once (the frame left open) and kept in jackpots.csv;
#      --edge_supply has kept the edge pieces left able to meet every inner edge
#      facing the frame.
#   C  The boards scoring at least the best ELITE_PCT% of every board scored so
#      far go to elite.csv and to the first rung of the ladder; the rest are
#      deleted with the beam's boards.
#   D  The ladder: every rung continues its boards' own searches (--resume) for
#      LADDER_TIME seconds, doubled on each rung up, and the better RUNG_KEEP%
#      move up; the top rung keeps its best TOP_RUNG_CAP and goes on searching
#      them round after round. A finished search leaves the ladder.
# Then back to A while TOTAL_TIME lasts.
#
# Why: the rows the beam laid first, and the side columns, may hold the pieces
# the top needs. Lifting them gives those pieces back to an exact search, which
# fails early on most boards and keeps a lucky one going.
#
# What RUN_DIR holds afterwards:
#   jackpots.csv  boards with every inner cell placed, break-free, frame open
#   top.csv       the TOP_KEEP deepest boards seen, best first
#   elite.csv     every board that entered the ladder, as first searched
#   ladder/       rung_K.csv, the boards on each rung, resumable
#   halo.log      one line per round; scores.txt the threshold's histogram
#   logs/         each round's logs, gzipped (KEEP_LOGS=1)
# A rerun with the same RUN_DIR carries on: rounds, histogram, ladder. Ctrl-C is
# safe: the running tool stops cleanly, its rows are kept, and the run ends.
# Rows from B and D carry a resume identifier as field 3 (515 fields); every
# reader takes the last 512 fields. NEEDS ~8 GB RAM for the beamer.
set -uo pipefail        # not -e: a round that fails must not kill the run

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # SEED, HOLES, ROTATIONS: relative to REPO
RUN_DIR=halo_out                        # relative to where you START it
THREADS=$(nproc)
TOTAL_TIME=4h           # the whole loop: seconds, or 90m, 4h, 2d. A step still
                        # running at the end is stopped and harvested.
DB_FILE=chain.db        # the chain database cache, ~6 GB. A bare name lands in
                        # RUN_DIR; give an absolute path to share it between runs.
                        # The clue setting is appended (chain.db.center): a clued
                        # database leaves the clue pieces out. Empty = rebuild
                        # it in memory every round (~2 minutes each).
CLUES=center            # center | all | none -- held by the beam and put back by
                        # the backtracker. The corner clues kill most beams on
                        # row 2 and most halo searches at once.

# A. beamer
ROTATIONS=              # empty = --random_edges. A Stage A rotations file
                        # instead: one border row per round, in turn, with
                        # SAMPLES bottoms each and LAMBDA_CORNERS
SAMPLES=10              # random bottoms per round (--samples; --top_bottoms
                        # with ROTATIONS)
COLUMNS=5               # left columns per bottom (--top_columns)
BEAM_WIDTH=10000
STOP_ROW=10
BACKTRACK_ROW=7         # --backtrack_row: exhaustive rows BACKTRACK_ROW+1..STOP_ROW;
                        # 0 = the beam alone. Row 6 writes millions of boards a round
BC_WINDOW=7,7
POOL=64                 # --pool_factor
TAU=0.1                 # --tau_bottoms and --tau_columns
EXHAUST_INNER=0         # --exhaust_inner_color N; 1 leaves A almost empty at row 10
LAMBDA_CORNERS=0.5      # --lambda_corners, with ROTATIONS only: the beamer
                        # refuses it with --random_edges. 0 = off
RNG_SEED=0              # 0 = a fresh seed every round; S = S, S+1, ... per round.
                        # The seed names the round's boards (h<seed>_rndb...).
BEAM_WALL=1800          # cap on one beamer call, seconds
BEAM_MAX=300000         # --max_emitted: stop the beam after this many boards
                        # (about 2 KB each on disk); 0 = no cap
MAX_BOARDS=10000        # distinct boards searched per round, the same share from
                        # every configuration; 0 = all of them

# B. backtracker
HOLES=data/holes_halo.csv   # must lift the whole frame up to STOP_ROW
BT_ORDER=frontier       # --order, with --frame_last
BT_TIME=0.1             # seconds per board, the first look
BT_EXTRA=               # more backtracker flags, e.g. BT_EXTRA="--reverse"

# C, D. elite and ladder
ELITE_PCT=5             # the best ELITE_PCT% of every board scored so far
ELITE_SCORE=0           # > 0: a fixed threshold instead of ELITE_PCT
RUNGS=6                 # rungs of the ladder; 0 = no ladder
LADDER_TIME=0.5         # seconds per board on the first rung, doubled per rung
RUNG_KEEP=50            # % of a rung's boards that move up each round
TOP_RUNG_CAP=64         # boards the top rung keeps searching
TOP_KEEP=100            # boards in top.csv
KEEP_LOGS=1             # 1 = keep each round's logs, gzipped, in RUN_DIR/logs
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
    *) die "CLUES must be center, all or none (got: $CLUES)" ;;
esac
for v in SAMPLES COLUMNS BEAM_WIDTH STOP_ROW BACKTRACK_ROW POOL EXHAUST_INNER BEAM_WALL \
         BEAM_MAX MAX_BOARDS ELITE_SCORE RUNGS RUNG_KEEP TOP_RUNG_CAP TOP_KEEP KEEP_LOGS RNG_SEED; do
    [[ ${!v} =~ ^[0-9]+$ ]] || die "$v must be a whole number (got: ${!v})"
done
for v in ELITE_PCT BT_TIME LADDER_TIME; do
    [[ ${!v} =~ ^[0-9]+([.][0-9]+)?$ ]] && awk -v p="${!v}" 'BEGIN { exit !(p > 0) }' ||
        die "$v must be a number above 0 (got: ${!v})"
done
awk -v p="$ELITE_PCT" 'BEGIN { exit !(p <= 100) }' || die "ELITE_PCT is a percentage, at most 100"
[ "$RUNG_KEEP" -ge 1 ] && [ "$RUNG_KEEP" -le 100 ] || die "RUNG_KEEP is a percentage, 1 to 100"
[ "$SAMPLES" -ge 1 ] || die "SAMPLES must be at least 1: --samples 0 never ends"
[ "$BACKTRACK_ROW" -lt "$STOP_ROW" ] || die "BACKTRACK_ROW must be below STOP_ROW, or 0"

case "$RUN_DIR" in /*) ;; *) RUN_DIR="$PWD/$RUN_DIR" ;; esac
cd "$REPO" || die "REPO=$REPO does not exist"
[ -d bin ] && [ -d tools ] || die "REPO=$REPO is not an E555 checkout -- set REPO at the top"
[ -x bin/E555_beamer ] && [ -x bin/E555_backtracker ] || make ||
    die "the build failed; try make ARCH=generic"
[ -f "$SEED" ]  || die "seed file not found: $SEED"
[ -f "$HOLES" ] || die "holes mask not found: $HOLES"
SEED=$PWD/$SEED; HOLES=$PWD/$HOLES
# The jackpot release counts the 60 frame cells as the last ones open, so the
# mask must open the whole frame the beam fills (rows 0..STOP_ROW).
awk -v stop="$STOP_ROW" '!/^[ \t]*([#%]|$)/ { gsub(/[ \t\r]/, ""); n = split($0, f, ",")
        if (r <= stop) for (c = 1; c <= n; c++)
            if ((r == 0 || c == 1 || c == 16) && f[c] != 1) bad = 1
        r++ } END { exit bad }' "$HOLES" || die "HOLES must lift the whole frame up to row $STOP_ROW"
NROT=0
if [ -n "$ROTATIONS" ]; then
    [ -s "$ROTATIONS" ] || die "rotations file not found: $ROTATIONS"
    ROTATIONS=$PWD/$ROTATIONS
    NROT=$(awk '!/^[ \t]*([#%]|$)/' "$ROTATIONS" | grep -c .)
    [ "$NROT" -ge 1 ] || die "no border rows in $ROTATIONS"
fi

mkdir -p "$RUN_DIR/logs" "$RUN_DIR/ladder" || die "cannot create RUN_DIR=$RUN_DIR"
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
JACK=$RUN_DIR/jackpots.csv
TOP=$RUN_DIR/top.csv
LOG=$RUN_DIR/halo.log
SCORES=$RUN_DIR/scores.txt
LADDER=$RUN_DIR/ladder
touch "$ELITE" "$JACK" "$TOP" "$SCORES"

# The round count goes on from the last run in this RUN_DIR.
ROUND=0
[ -s "$LOG" ] && ROUND=$(awk '$1 == "round" { n = $2 } END { print n + 0 }' "$LOG")
rm -rf "$RUN_DIR"/round_*       # a round a previous run was killed in

hms()  { local s=$1; if [ "$s" -ge 3600 ]; then printf '%dh%02dm' $((s/3600)) $((s%3600/60))
         elif [ "$s" -ge 60 ]; then printf '%dm%02ds' $((s/60)) $((s%60)); else printf '%ds' "$s"; fi; }
rows() { awk '!/^[ \t]*([#%]|$)/' "$@" 2>/dev/null | grep -c . ; }

# distinct FILE... -- the boards, one per distinct board after the HOLES lift,
# each renamed <config id>_<beam index>. The beam writes many boards that differ
# only in the lifted cells, and those are one search.
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
# thin FILE MAX -- at most about MAX of FILE's boards, the same share from every
# configuration (the id before its _<index>), evenly spread within each.
thin() {
    awk -F, -v max="$2" '
        FNR == NR { id = $1; sub(/_[0-9]+$/, "", id); if (!(id in n)) nc++; n[id]++; next }
        { id = $1; sub(/_[0-9]+$/, "", id); k = ++i[id]; q = max / nc
          if (q >= n[id] || int(k * q / n[id]) != int((k - 1) * q / n[id])) print }' "$1" "$1"
}
# searched FILE -- the rows a --resume run wrote with an identifier (a row it
# never started comes back as it went in, without one).
searched() { awk -F, '$3 ~ /^bt1;/' "$1" 2>/dev/null; }
# inner_awk: n = the board's placed inner cells (rows and columns 1..14), 0..196.
INNER='n = 0; for (i = NF - 511; i <= NF - 256; i++) { c = $i + 0
       if (c != 999) { r = int(c / 16); k = c % 16; if (r >= 1 && r <= 14 && k >= 1 && k <= 14) n++ } }'
# jackpots FILE -- copy the boards with every inner cell placed to jackpots.csv,
# once each (a released board's search goes on to the next release).
jackpots() {
    awk -F, "{ $INNER; if (n == 196) print }" "$1" 2>/dev/null >> "$JACK"
    awk -F, '{ k = $0; for (i = 1; i <= NF - 512; i++) sub(/^[^,]*,/, "", k)
               if (!(k in seen)) { seen[k] = 1; print } }' "$JACK" > "$JACK.tmp" && mv "$JACK.tmp" "$JACK"
}
best_inner() { cat "$@" 2>/dev/null | awk -F, "NF >= 514 { $INNER; if (n > m) m = n } END { print m + 0 }"; }

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
# best_first -- rows on stdin, best score first
best_first() { awk -F, '{ print ($2 + 0) "\t" $0 }' | sort -t "$(printf '\t')" -k1,1nr -s | cut -f2-; }
# keep_top FILE... -- merge into top.csv: the TOP_KEEP best distinct boards
keep_top() {
    cat "$TOP" "$@" 2>/dev/null | awk -F, 'NF >= 514' | best_first |
        awk -F, -v keep="$TOP_KEEP" '{ k = $0; for (i = 1; i <= NF - 512; i++) sub(/^[^,]*,/, "", k)
                   if (!(k in seen) && kept < keep) { seen[k] = 1; kept++; print } }' > "$TOP.tmp" &&
        mv "$TOP.tmp" "$TOP"
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

BEAM=(--stop_row "$STOP_ROW" --beam_width "$BEAM_WIDTH" --bc_window "$BC_WINDOW"
      --pool_factor "$POOL" --top_columns "$COLUMNS" --tau_bottoms "$TAU"
      --tau_columns "$TAU" --exhaust_inner_color "$EXHAUST_INNER" --max_emitted "$BEAM_MAX"
      "${CLUE_ARG[@]}" "${DB_ARG[@]}" --threads "$THREADS" --print_cmd)
[ "$BACKTRACK_ROW" -eq 0 ] || BEAM+=(--backtrack_row "$BACKTRACK_ROW" --extend_nodes 0)
read -r -a BT_MORE <<< "$BT_EXTRA"
# The 60 frame cells are the last open ones, so --early_release 60 releases the
# first board with every inner cell placed. --edge_supply cuts a path once the
# inner cells ask the frame for more of a colour than the unused edge pieces
# carry inward. The colour count and Hall would also ask the rest of the frame
# to close; that is left for later.
BT=(--holes "$HOLES" "${CLUE_ARG[@]}" --breaks 0 --order "$BT_ORDER" --frame_last
    --edge_supply --early_release 60 --no_colour_count --no_hall --resume
    --threads "$THREADS" --print_cmd "${BT_MORE[@]}")

echo "[halo] $RUN_DIR: $THREADS threads, $(hms "$TOTAL_S"), clues $CLUES, db ${DB_FILE:-<in memory>}"
if [ -n "$ROTATIONS" ]; then
    echo "[halo] A: $ROTATIONS, one of its $NROT border rows a round, $SAMPLES bottoms x $COLUMNS columns"
else
    echo "[halo] A: random borders, $SAMPLES bottoms x $COLUMNS columns"
    [ "$LAMBDA_CORNERS" = 0 ] ||
        echo "[halo]    (--lambda_corners left out: the beamer refuses it with --random_edges)"
fi
echo "[halo]    width $BEAM_WIDTH to row $STOP_ROW, backtrack row $BACKTRACK_ROW, bc_window $BC_WINDOW, pool $POOL, tau $TAU, exhaust_inner $EXHAUST_INNER"
echo "[halo] B: up to $MAX_BOARDS boards a round, $(basename "$HOLES") lifted, --order $BT_ORDER --frame_last --edge_supply, ${BT_TIME}s a board${BT_EXTRA:+, $BT_EXTRA}"
if [ "$ELITE_SCORE" -gt 0 ]; then echo "[halo] C: boards scoring $ELITE_SCORE or more climb the ladder"
else echo "[halo] C: the best $ELITE_PCT% of every board scored climb the ladder"; fi
[ "$RUNGS" -eq 0 ] || echo "[halo] D: $RUNGS rungs from ${LADDER_TIME}s a board, doubling, $RUNG_KEEP% move up; top rung keeps $TOP_RUNG_CAP"
[ -z "$DB_FILE" ] || [ -s "$DB_FILE" ] ||
    echo "[halo] the first round builds the chain database ($DB_FILE), about 2 minutes on 4 threads"
[ "$ROUND" -eq 0 ] || echo "[halo] going on from round $ROUND: $(rows "$LADDER"/rung_*.csv) boards on the ladder, $(rows "$JACK") jackpots"
echo "[halo] $(date '+%F %T') start, $(hms "$TOTAL_S")" >> "$LOG"

# ladder RD -- one pass over the rungs, top first, so a board moves up one rung a
# round at most. Sets LAD to a summary of the rung sizes.
ladder() {
    local rd=$1 k t n out keep
    LAD=
    for (( k = RUNGS - 1; k >= 0; k-- )); do
        [ -s "$LADDER/rung_$k.csv" ] || continue
        if [ "$STOP" = 1 ] || [ "$(left)" -le 30 ]; then LAD="$LAD r$k:$(rows "$LADDER/rung_$k.csv")"; continue; fi
        t=$(awk -v a="$LADDER_TIME" -v k="$k" 'BEGIN { print a * 2 ^ k }')
        out=$rd/rung_$k.out.csv
        run_tool $(( $(left) - 20 )) bin/E555_backtracker "$SEED" "$LADDER/rung_$k.csv" \
            "$out" "${BT[@]}" --time_limit "$t" >> "$rd/ladder.log" 2>&1
        jackpots "$out"
        keep_top "$out"
        if [ "$STOP" = 1 ] || [ "$(left)" -le 20 ]; then
            # Stopped part way: the rows not reached come back unchanged, so the
            # rung keeps every board for the next run and nothing moves up.
            searched "$out" > "$LADDER/rung_$k.csv"
            LAD="$LAD r$k:$(rows "$LADDER/rung_$k.csv")"
            continue
        fi
        # A finished search is written once, as done; it leaves the ladder.
        searched "$out" | grep -v ';done' | best_first > "$rd/rung_$k.live"
        n=$(rows "$rd/rung_$k.live")
        if [ "$k" -eq $(( RUNGS - 1 )) ]; then
            head -n "$TOP_RUNG_CAP" "$rd/rung_$k.live" > "$LADDER/rung_$k.csv"
        else
            keep=$(( (n * RUNG_KEEP + 99) / 100 ))
            head -n "$keep" "$rd/rung_$k.live" >> "$LADDER/rung_$(( k + 1 )).csv"
            : > "$LADDER/rung_$k.csv"
        fi
        LAD="$LAD r$k:$n"
    done
}

SEARCHED=0; KEPT=0; DONE_ROUNDS=0; FAILS=0
while [ "$STOP" = 0 ] && [ "$(left)" -ge 60 ]; do
    ROUND=$(( ROUND + 1 ))
    RD=$RUN_DIR/round_$ROUND
    rm -rf "$RD"; mkdir -p "$RD"
    if [ "$RNG_SEED" = 0 ]; then RS=$(( ((RANDOM << 15) | RANDOM) + 1 ))
    else RS=$(( RNG_SEED + ROUND - 1 )); fi
    TAG="round $ROUND seed $RS"

    # -- A: the beam. It gets at most a third of what is left; the backtracking
    #    of the configuration in hand can run past --wall_time, hence the guard.
    BW=$(( $(left) / 3 )); [ "$BW" -le "$BEAM_WALL" ] || BW=$BEAM_WALL
    if [ -n "$ROTATIONS" ]; then
        BORDER=("$ROTATIONS" --start_row $(( (ROUND - 1) % NROT )) --num_rows 1
                --top_bottoms "$SAMPLES")
        [ "$LAMBDA_CORNERS" = 0 ] || BORDER+=(--lambda_corners "$LAMBDA_CORNERS")
    else
        BORDER=(--random_edges --samples "$SAMPLES")
    fi
    T0=$SECONDS
    run_tool $(( BW + 120 )) bin/E555_beamer "$SEED" "${BORDER[@]}" "${BEAM[@]}" --rng_seed "$RS" \
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

    # -- B: the first look, cut short (cleanly, by SIGTERM) at the deadline
    B="B skipped"; NS=0
    if [ "$NB" -gt 0 ] && [ "$STOP" = 0 ] && [ "$(left)" -gt 30 ]; then
        distinct "${BOARDS[@]}" > "$RD/distinct.csv"
        rm -rf "$RD/beam"/*.csv
        ND=$(rows "$RD/distinct.csv")
        if [ "$MAX_BOARDS" -gt 0 ] && [ "$ND" -gt "$MAX_BOARDS" ]; then
            thin "$RD/distinct.csv" "$MAX_BOARDS" > "$RD/boards.csv"
        else
            mv "$RD/distinct.csv" "$RD/boards.csv"
        fi
        rm -f "$RD/distinct.csv"
        NT=$(rows "$RD/boards.csv")
        T0=$SECONDS
        run_tool $(( $(left) - 20 )) bin/E555_backtracker "$SEED" "$RD/boards.csv" \
            "$RD/halo.raw.csv" "${BT[@]}" --time_limit "$BT_TIME" > "$RD/bt.log" 2>&1
        RC=$?
        TB=$(( SECONDS - T0 ))
        rm -f "$RD/boards.csv"
        searched "$RD/halo.raw.csv" > "$RD/halo.csv"
        rm -f "$RD/halo.raw.csv"
        NS=$(rows "$RD/halo.csv")
        jackpots "$RD/halo.csv"
        STAT=$(awk -F, "{ $INNER; s += n; if (n > m) m = n; rows++ }
                        END { if (rows) printf \"interior mean %.1f best %d\", s / rows, m }" "$RD/halo.csv")
        B="B $(hms $TB): $NS of $NT boards ($ND distinct)${STAT:+, $STAT}"
        [ "$RC" = 0 ] || [ "$STOP" = 1 ] || B="$B, backtracker exit $RC"
        [ "$(left)" -gt 20 ] || B="$B, cut at the deadline"
    fi

    # -- C: onto the ladder
    C="C: nothing to split"
    if [ "$NS" -gt 0 ]; then
        add_scores "$RD/halo.csv"
        N=$(threshold)
        python3 tools/E555_rank.py "$RD/halo.csv" --seed_file "$SEED" --quiet \
            --split score "$N" "$RD/elite_new.csv" "$RD/below.csv"
        NE=$(rows "$RD/elite_new.csv")
        cat "$RD/elite_new.csv" >> "$ELITE" 2>/dev/null
        [ "$RUNGS" -eq 0 ] || grep -v ';done' "$RD/elite_new.csv" >> "$LADDER/rung_0.csv"
        keep_top "$RD/elite_new.csv"
        SEARCHED=$(( SEARCHED + NS )); KEPT=$(( KEPT + NE ))
        C="C: N=$N -> +$NE"
    fi

    # -- D: the ladder
    D="D: no ladder"
    if [ "$RUNGS" -gt 0 ] && [ "$STOP" = 0 ]; then
        T0=$SECONDS
        ladder "$RD"
        D="D $(hms $(( SECONDS - T0 ))):${LAD:- empty}"
    fi
    DONE_ROUNDS=$(( DONE_ROUNDS + 1 ))
    BI=$(best_inner "$TOP")
    echo "[halo] $TAG | $A | $B | $C | $D | best interior $BI/196, jackpots $(rows "$JACK") | $(hms $(( $(left) > 0 ? $(left) : 0 ))) left"
    echo "round $ROUND seed $RS boards=$NB searched=$NS ${C#C: } ladder=${LAD:-} best_interior=$BI jackpots=$(rows "$JACK") $(date '+%F %T')" >> "$LOG"

    if [ "$KEEP_LOGS" = 1 ]; then
        for l in beam bt ladder; do
            [ -s "$RD/$l.log" ] && gzip -c "$RD/$l.log" > "$RUN_DIR/logs/round_${ROUND}_$l.log.gz"
        done
    fi
    rm -rf "$RD"            # the beam's boards and those below N: deleted for good
done

echo
[ "$STOP" = 1 ] && echo "[halo] interrupted."
echo "[halo] $DONE_ROUNDS round(s), $SEARCHED boards searched, $KEPT onto the ladder;" \
     "ladder $(rows "$LADDER"/rung_*.csv) boards, jackpots $(rows "$JACK") in $JACK"
echo "[halo] $(date '+%F %T') end, $DONE_ROUNDS rounds, jackpots $(rows "$JACK")" >> "$LOG"
[ "$(rows "$TOP")" -gt 0 ] || exit 0
echo "[halo] the deepest boards ($TOP), best interior $(best_inner "$TOP")/196:"
python3 tools/E555_rank.py "$TOP" --seed_file "$SEED" --top 5
