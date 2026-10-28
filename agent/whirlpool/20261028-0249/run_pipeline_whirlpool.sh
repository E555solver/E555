#!/bin/bash
# run_pipeline_whirlpool.sh -- the whirlpool as a jewel finder: beamer boards,
# each grown again from its three other sides under ONE fixed border.
#
#   bash run_pipeline_whirlpool.sh                              # an hour, all cores
#   bash run_pipeline_whirlpool.sh WALL=600 RUN_DIR=w1 DIVE=1   # ten minutes, then dive
#   bash run_pipeline_whirlpool.sh MODE=whirl INPUT=b.csv OUT=chains.csv RUN_DIR=tmp
#
# NEEDS the finalizer flag --no_fanout_sort: apply
# agent/whirlpool/20261028-0249/patches/src/B_beam/E555_finalizer.c (a full copy
# of the file) over src/B_beam/ and run make. It also needs ~8 GB of RAM for the
# beamer, and DB_FILE (6.4 GB on disk) to load the chain database in seconds
# instead of rebuilding it in two minutes on every produce call.
#
# A JEWEL is a chain s1..s4: a beamer board s1 (rows 0..10 complete) and three
# laps, each turning the board a quarter turn clockwise and re-growing rows
# 0..10, all four boards holding the same complete 60-cell frame. Grown from all
# four sides under one border. Campaign 20261028-0249 measured 82.9 jewels per
# hour with these settings on 4 cores (agent/whirlpool/20261028-0249/README.md).
#
# One lap:  turn clockwise (tools/E555_rotate.py)
#           -> keep a small block of the parent: rows 1..BR, columns 1..KC
#           -> E555_backtracker --stop_row BR --with_frame   (fills the rest of
#              rows 0..BR exactly, and on lap 1 the frame cells the board lacks)
#           -> E555_finalizer --finalize_from BR --stop_row 10 --keep_ring
#              --no_fanout_sort                               (re-grows rows BR+1..10)
# Lap 1 cuts at BR:KC = 3:4; laps 2 and 3 try 4:3 first, then 3:4. --keep_ring
# makes the ring lap 1's band cut chose the board's fixed border. Depth-first:
# a lap's first child goes straight into the next lap. Produce keeps one board
# per frame-cell signature and shuffles them, so consecutive boards do not
# rediscover the same ring.
#
# MODE=all (default) loops produce -> whirl for WALL seconds, each board capped
# at CAP seconds, and writes into RUN_DIR:
#   chains.csv   every chain written (ids b<board>_t<n>_s<k>)
#   jewels.csv   the jewels' four boards, judged by agent/whirlpool/judge.py
#   jewels.log   one CHAIN line per jewel
#   dived.csv    with DIVE=1: the jewels' s2..s4 dived and polished (E555_diver)
# MODE=produce and MODE=whirl are the whirlpool kit's candidate contract
# (agent/whirlpool_agent_mode.md, section 3).
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------------
MODE=all            # all | produce | whirl
REPO=                # E555 checkout with built bin/; default: the one this script is in
SEED=               # default: $REPO/data/seed_Edge5.txt
RUN_DIR=whirlpool_out
THREADS=$(nproc)
DB_FILE=            # cache of the 6.4 GB chain database; strongly recommended
WALL=3600           # MODE=all: seconds of produce + whirl
CAP=180             # MODE=all: seconds per board
DIVE=0              # MODE=all: 1 = dive the jewels at the end
JUDGE=              # default: $REPO/agent/whirlpool/judge.py
RNG_SEED=0          # 0 = from the clock
TIME_LIMIT=180      # produce/whirl: seconds this call may take
INPUT=              # whirl: one beamer board
OUT=                # produce: the queue; whirl: the chains file (append)
BEAM_WIDTH=10000
BEAM_BACKTRACK=6
BEAM_BOARDS=2000
BAND_ROW=3
KEEP_COLS=4         # lap 1 with MARGIN_LAP1=1: interior columns kept
LAP_CUTS="4:3 3:4"  # laps 2 and 3: band_row:keep_cols, tried in order
MARGIN_LAP1=1       # 1 = blank the margin on lap 1 too
BT_LIMIT=60         # bands enumerated per lap
BT_PICK=2           # lap 1: bands grown, farthest-first
BT_PICK2=2          # laps 2 and 3
BT_TIME=15
FIN_WIDTH=20000
FIN_COLUMNS=4
FIN_EMIT=1          # boards a lap's finalizer stops after
CHILDREN=2          # children of a lap taken deeper
LAP_TIME=90         # seconds at most per lap call
# -----------------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 2 ;;
    esac
done
if [ -z "$REPO" ]; then             # pipeline/ or agent/whirlpool/<id>/ of a checkout
    here=$(cd "$(dirname "$0")" && pwd)
    for REPO in "$here/.." "$here/../../.."; do [ -d "$REPO/tools" ] && [ -d "$REPO/src" ] && break; done
fi
REPO=$(cd "$REPO" && pwd)
SEED=${SEED:-$REPO/data/seed_Edge5.txt}; JUDGE=${JUDGE:-$REPO/agent/whirlpool/judge.py}
[ "$RNG_SEED" != 0 ] || RNG_SEED=$(( $(date +%s) % 1000000007 ))
BIN=$REPO/bin; TOOLS=$REPO/tools
[ -x "$BIN/E555_finalizer" ] || { echo "REPO=$REPO has no bin/E555_finalizer: run make there" >&2; exit 1; }
{ "$BIN/E555_finalizer" 2>&1 || true; } > "${TMPDIR:-/tmp}/fin_usage.$$"
grep -q -- --no_fanout_sort "${TMPDIR:-/tmp}/fin_usage.$$" && rm -f "${TMPDIR:-/tmp}/fin_usage.$$" ||
    { echo "bin/E555_finalizer lacks --no_fanout_sort: apply the patch named at the top" >&2; exit 1; }
mkdir -p "$RUN_DIR"; RUN_DIR=$(cd "$RUN_DIR" && pwd)

if [ "$MODE" = all ]; then
    SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
    abs() { case "$1" in /*) printf '%s' "$1";; *) printf '%s' "$PWD/$1";; esac; }
    SEED=$(abs "$SEED"); [ -n "$DB_FILE" ] && DB_FILE=$(abs "$DB_FILE")
    cd "$RUN_DIR"; : > chains.csv; : > jewels.csv; : > jewels.log; : > frames_seen.txt
    echo "[cfg] repo=$REPO run_dir=$RUN_DIR threads=$THREADS wall=$WALL cap=$CAP rng_seed=$RNG_SEED db_file=${DB_FILE:-<in memory>}"
    T0=$SECONDS; nb=0; np=0; nj=0; empty=0
    while [ $(( SECONDS - T0 )) -lt "$WALL" ]; do
        np=$((np + 1)); rm -rf p; mkdir p
        timeout -k 10 600 bash "$SELF" MODE=produce REPO="$REPO" SEED="$SEED" RUN_DIR="$RUN_DIR/p" \
            THREADS="$THREADS" RNG_SEED=$((RNG_SEED + np)) DB_FILE="$DB_FILE" TIME_LIMIT=600 \
            OUT="$RUN_DIR/queue.csv" > "produce$np.log" 2>&1 || true
        rm -rf p
        nq=$(grep -vc '^[#%]' queue.csv 2>/dev/null || true); nq=${nq:-0}
        echo "[all] produce $np: $nq boards ($(( SECONDS - T0 )) s)"
        if [ "$nq" = 0 ]; then empty=$((empty + 1)); [ "$empty" -lt 2 ] && continue; echo "[all] two empty produce calls: stopping"; break; fi
        empty=0
        for ((i = 1; i <= nq; i++)); do
            [ $(( SECONDS - T0 )) -lt "$WALL" ] || break
            nb=$((nb + 1)); rm -rf w; mkdir w
            awk -v n="$i" '!/^[#%]/ && NF {k++; if (k == n) {print; exit}}' queue.csv > board.csv
            timeout -k 5 "$CAP" bash "$SELF" MODE=whirl REPO="$REPO" SEED="$SEED" RUN_DIR="$RUN_DIR/w" \
                THREADS="$THREADS" RNG_SEED=$((RNG_SEED + nb)) TIME_LIMIT="$CAP" INPUT="$RUN_DIR/board.csv" \
                OUT="$RUN_DIR/w.csv" > w.log 2>&1 || true
            rm -rf w; touch w.csv
            awk -F, -v p="b${nb}_" 'BEGIN {OFS = ","} !/^[#%]/ && NF {$1 = p $1; print}' w.csv >> chains.csv
            python3 "$JUDGE" w.csv --input board.csv --frames_seen frames_seen.txt > judge.log 2>&1 || true
            while read -r tag; do
                grep "^CHAIN tag=$tag " judge.log | sed "s/tag=/tag=b${nb}_/" >> jewels.log
                awk -F, -v t="$tag" -v p="b${nb}_" 'BEGIN {OFS = ","} index($1, t "_s") == 1 && length($1) == length(t) + 3 {$1 = p $1; print}' w.csv >> jewels.csv
                nj=$((nj + 1))
            done < <(awk '/^CHAIN / && / jewel=yes/ && / new=yes/ {sub(/^tag=/, "", $2); print $2}' judge.log)
            rm -f w.csv
            echo "[all] board $nb: $(grep -c '^CHAIN' judge.log || true) chains, jewels so far $nj ($(( SECONDS - T0 )) s)"
        done
        rm -f queue.csv
    done
    echo "[all] $nb boards whirled, $nj jewels in $(( SECONDS - T0 )) s -> $RUN_DIR/jewels.csv"
    if [ "$DIVE" = 1 ] && [ -s jewels.csv ]; then
        grep -E '_s[234],' jewels.csv > to_dive.csv || true
        "$BIN/E555_diver" "$SEED" to_dive.csv dived.csv --end_dive 10000 --end_polish 20000 \
            --emit_score 0 --threads "$THREADS" --wall_time 900 --print_cmd > dive.log 2>&1 || true
        python3 "$TOOLS/E555_rank.py" dived.csv --seed_file "$SEED" --top 5 --rescore || true
    fi
    exit 0
fi
cd "$RUN_DIR"
START=$SECONDS
left() { echo $(( TIME_LIMIT - (SECONDS - START) )); }
rows() { python3 "$TOOLS/E555_rank.py" "$1" --count 2>/dev/null || echo 0; }

if [ "$MODE" = produce ]; then
    DB_ARG=(); [ -n "$DB_FILE" ] && DB_ARG=(--db_file "$DB_FILE")
    "$BIN/E555_beamer" "$SEED" --random_edges --samples 0 --top_columns 4 \
        --out_dir beam --threads "$THREADS" --rng_seed "$RNG_SEED" \
        --beam_width "$BEAM_WIDTH" --backtrack_row "$BEAM_BACKTRACK" --stop_row 10 \
        --max_emitted "$BEAM_BOARDS" --wall_time $(( $(left) - 30 )) --print_cmd "${DB_ARG[@]}" > beam.log 2>&1 ||
        echo "[produce] the beamer exited non-zero"
    xargs -r cat < beam/outputs.txt > all.csv 2> /dev/null || : > all.csv
    python3 - all.csv "$OUT" "$RNG_SEED" <<'PYEOF'
import random, sys
frame = {c for c in range(256) if c // 16 in (0, 15) or c % 16 in (0, 15)}
seen, keep = set(), []
for line in open(sys.argv[1]):
    if not line.strip() or line[0] in "#%": continue
    x = line.rstrip("\n").split(","); n = len(x) - 512
    sig = tuple(sorted((int(x[n + p]), p, x[n + 256 + p]) for p in range(256) if int(x[n + p]) in frame))
    if sig not in seen:
        seen.add(sig); keep.append(line)
random.Random(int(sys.argv[3])).shuffle(keep)
open(sys.argv[2], "w").writelines(keep)
PYEOF
    echo "[produce] $(rows "$OUT") boards"
    exit 0
fi
[ "$MODE" = whirl ] || { echo "MODE must be produce or whirl" >&2; exit 2; }

# blank.py IN OUT BAND_ROW KEEP_COLS -- unplace the interior cells of rows
# 1..BAND_ROW in columns > KEEP_COLS (the band cut clears the rows above itself)
cat > blank.py <<'EOF'
import sys
src, dst, band, keep = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
with open(src) as f, open(dst, "w") as o:
    for line in f:
        if not line.strip() or line[0] in "#%": continue
        x = line.rstrip("\n").split(","); n = len(x) - 512
        for p in range(256):
            c = int(x[n + p])
            if c == 999: continue
            r, col = divmod(c, 16)
            if 1 <= r <= band and keep < col <= 14: x[n + p] = "999"
        o.write(",".join(x) + "\n")
EOF
first() { awk '!/^[#%]/ && NF {print; exit}' "$1"; }
nth()   { awk -v n="$2" '!/^[#%]/ && NF {i++; if (i == n) {print; exit}}' "$1"; }
TAGN=0
# chain FILE... -- append the boards (s1, s2, ...) as one new chain
chain() {
    TAGN=$((TAGN + 1)); local s=0 f
    for f in "$@"; do
        s=$((s + 1))
        first "$f" | awk -F, -v id="t${TAGN}_s$s" 'BEGIN {OFS = ","} {$1 = id; print}' >> "$OUT"
    done
}

# lap K BOARD PREFIX... -- one lap from BOARD (s_K); recurses into its children.
# PREFIX lists the chain's boards so far (s1..s_K). Returns 0 once a jewel-length
# chain (s4) has been written.
lap() {
    local k=$1 board=$2 cut; shift 2
    if [ "$k" -ge 2 ]; then
        for cut in $LAP_CUTS; do
            lap1cut "$k" "$board" "${cut%%:*}" "${cut##*:}" "$BT_PICK2" "$@"
            case $? in 0) return 0 ;; 2) ;; *) return 1 ;; esac
        done
        return 1
    fi
    lap1cut 1 "$board" "$BAND_ROW" "$KEEP_COLS" "$BT_PICK" "$@"
    [ $? = 0 ]
}

# lap1cut K BOARD BAND_ROW KEEP_COLS PICK PREFIX... -- one lap with one cut.
# 0 = a chain reached s4 below it; 2 = this cut re-grew nothing (try the next
# cut); 1 = it re-grew but no child went the distance, or no time is left.
lap1cut() {
    local k=$1 board=$2 br=$3 kc=$4 pick=$5; shift 5
    local d="l$k.$RANDOM"; mkdir -p "$d"
    python3 "$TOOLS/E555_rotate.py" "$board" 1 --out "$d/rot.csv" --seed_file "$SEED" > /dev/null
    if [ "$k" -ge 2 ] || [ "$MARGIN_LAP1" = 1 ]; then
        python3 blank.py "$d/rot.csv" "$d/cut.csv" "$br" "$kc"
    else
        cp "$d/rot.csv" "$d/cut.csv"
    fi
    local bt=$BT_TIME
    "$BIN/E555_backtracker" "$SEED" "$d/cut.csv" "$d/bt.csv" --stop_row "$br" --with_frame \
        --order rowmajor --break_mode any --breaks 0 --max_emitted "$BT_LIMIT" \
        --time_limit "$bt" --threads "$THREADS" --print_cmd > "$d/bt.log" 2>&1 || true
    local bf="$d/bt.csv.stop_row$br.csv"
    [ -n "$(first "$bf" 2>/dev/null)" ] || { echo "[whirl] lap $k cut $br:$kc: no band"; return 2; }
    python3 "$TOOLS/E555_rank.py" "$bf" --seed_file "$SEED" --diverse "$pick" \
        --out "$d/band.csv" > /dev/null 2>&1 || cp "$bf" "$d/band.csv"
    local budget=$(( $(left) - 3 )); [ "$budget" -le "$LAP_TIME" ] || budget=$LAP_TIME
    [ "$budget" -gt 5 ] || { echo "[whirl] lap $k: out of time"; return 1; }
    "$BIN/E555_finalizer" "$SEED" "$d/band.csv" --out_dir "$d/fin" --threads "$THREADS" \
        --finalize_from "$br" --stop_row 10 --keep_ring --no_fanout_sort --beam_width "$FIN_WIDTH" \
        --top_columns "$FIN_COLUMNS" --lambda_Mahalanobis 0 --frac_rand 0.0 \
        --max_emitted "$FIN_EMIT" --wall_time "$budget" --rng_seed "$RNG_SEED" --print_cmd > "$d/fin.log" 2>&1 || true
    xargs -r cat < "$d/fin/outputs.txt" > "$d/next.csv" 2> /dev/null || : > "$d/next.csv"
    local n; n=$(rows "$d/next.csv")
    [ "$n" -gt 0 ] || { echo "[whirl] lap $k cut $br:$kc: $(rows "$d/band.csv") bands, none re-grew ($((SECONDS - START)) s)"; return 2; }
    echo "[whirl] lap $k cut $br:$kc: $n boards at $((SECONDS - START)) s"
    local i c
    for ((i = 1; i <= n && i <= CHILDREN; i++)); do
        c="$d/c$i.csv"; nth "$d/next.csv" "$i" > "$c"
        chain "$@" "$board" "$c"
        if [ "$k" -lt 3 ]; then
            [ "$(left)" -gt 10 ] || return 1
            lap $((k + 1)) "$c" "$@" "$board" && return 0
        else
            return 0
        fi
    done
    return 1
}

first "$INPUT" > s1.csv
chain s1.csv
lap 1 s1.csv || true
