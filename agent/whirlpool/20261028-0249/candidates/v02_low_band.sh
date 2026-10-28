#!/bin/bash
# v02_low_band.sh -- v01 with a 3-row band on laps 2 and 3.
# v01's laps 2-3 (band rows 0..5, kept block 5 columns) found bands, but no band
# re-grew rows 6..10 between the two held ring sides: 0 of 16, exhaustively
# (journal 03:40). Here laps 2 and 3 lock only rows 0..BAND_ROW2 (3) with a
# KEEP_COLS2 (4) wide kept block (12 interior cells), so the finalizer's beam
# grows rows 4..10 itself from a wider start. Lap 1 is v01's.
# v01's changes against the baseline (v00):
#   * one turn direction (clockwise) every lap, depth-first: a lap's children
#     are tried in order, each taken to the next lap before the next child;
#   * every lap's finalizer runs --keep_ring, so the ring the lap-1 band cut
#     chose is written out with the board and held by laps 2 and 3;
#   * MARGIN: before the band cut, the turned parent keeps only the interior
#     cells of rows 1..BAND_ROW in columns 1..KEEP_COLS; the rest of that slab
#     is blanked, so the cut fills a (14-KEEP_COLS)-wide gap between the kept
#     block and the frame instead of the 4-wide wedge it faces otherwise;
#   * produce: the narrow beam to row 6 + exhaustive to row 10 of the example.
set -euo pipefail

# ---- the contract ----------------------------------------------------------------
MODE=; REPO=; SEED=; RUN_DIR=; THREADS=4; RNG_SEED=1; TIME_LIMIT=180; DB_FILE=; INPUT=; OUT=
# ---- settings ----------------------------------------------------------------------
BEAM_WIDTH=10000
BEAM_BACKTRACK=6
BEAM_BOARDS=2000
BAND_ROW=5
KEEP_COLS=5         # lap 1 with MARGIN_LAP1=1: interior columns kept
BAND_ROW2=3         # laps 2 and 3: band rows 0..BAND_ROW2
KEEP_COLS2=4        # laps 2 and 3: interior columns of the band kept from the parent
MARGIN_LAP1=0       # 1 = blank the margin on lap 1 too
BT_LIMIT=60         # bands enumerated per lap
BT_PICK=4           # lap 1: bands grown, farthest-first
BT_PICK2=2          # laps 2 and 3
BT_TIME=15
FIN_WIDTH=20000
FIN_COLUMNS=4
FIN_EMIT=1          # boards a lap's finalizer stops after
CHILDREN=2          # children of a lap taken deeper
LAP_TIME=90         # seconds at most per lap call
# ---------------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 2 ;;
    esac
done
BIN=$REPO/bin; TOOLS=$REPO/tools
cd "$RUN_DIR"
START=$SECONDS
left() { echo $(( TIME_LIMIT - (SECONDS - START) )); }
rows() { python3 "$TOOLS/E555_rank.py" "$1" --count 2>/dev/null || echo 0; }

if [ "$MODE" = produce ]; then
    DB_ARG=(); [ -n "$DB_FILE" ] && DB_ARG=(--db_file "$DB_FILE")
    "$BIN/E555_beamer" "$SEED" --random_edges --samples 0 --top_columns 4 \
        --out_dir beam --threads "$THREADS" --rng_seed "$RNG_SEED" \
        --beam_width "$BEAM_WIDTH" --backtrack_row "$BEAM_BACKTRACK" --stop_row 10 \
        --max_emitted "$BEAM_BOARDS" --wall_time $(( $(left) - 30 )) "${DB_ARG[@]}" > beam.log 2>&1 ||
        echo "[produce] the beamer exited non-zero"
    xargs -r cat < beam/outputs.txt > "$OUT" 2> /dev/null || : > "$OUT"
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
    local k=$1 board=$2; shift 2
    local d="l$k.$RANDOM"; mkdir -p "$d"
    local br=$BAND_ROW kc=$KEEP_COLS pick=$BT_PICK
    [ "$k" -ge 2 ] && { br=$BAND_ROW2; kc=$KEEP_COLS2; pick=$BT_PICK2; }
    python3 "$TOOLS/E555_rotate.py" "$board" 1 --out "$d/rot.csv" --seed_file "$SEED" > /dev/null
    if [ "$k" -ge 2 ] || [ "$MARGIN_LAP1" = 1 ]; then
        python3 blank.py "$d/rot.csv" "$d/cut.csv" "$br" "$kc"
    else
        cp "$d/rot.csv" "$d/cut.csv"
    fi
    local bt=$BT_TIME
    "$BIN/E555_backtracker" "$SEED" "$d/cut.csv" "$d/bt.csv" --stop_row "$br" --with_frame \
        --order rowmajor --break_mode any --breaks 0 --max_emitted "$BT_LIMIT" \
        --time_limit "$bt" --threads "$THREADS" > "$d/bt.log" 2>&1 || true
    local bf="$d/bt.csv.stop_row$br.csv"
    [ -n "$(first "$bf" 2>/dev/null)" ] || { echo "[whirl] lap $k: no band"; return 1; }
    python3 "$TOOLS/E555_rank.py" "$bf" --seed_file "$SEED" --diverse "$pick" \
        --out "$d/band.csv" > /dev/null 2>&1 || cp "$bf" "$d/band.csv"
    local budget=$(( $(left) - 3 )); [ "$budget" -le "$LAP_TIME" ] || budget=$LAP_TIME
    [ "$budget" -gt 5 ] || { echo "[whirl] lap $k: out of time"; return 1; }
    "$BIN/E555_finalizer" "$SEED" "$d/band.csv" --out_dir "$d/fin" --threads "$THREADS" \
        --finalize_from "$br" --stop_row 10 --keep_ring --beam_width "$FIN_WIDTH" \
        --top_columns "$FIN_COLUMNS" --lambda_Mahalanobis 0 --frac_rand 0.0 \
        --max_emitted "$FIN_EMIT" --wall_time "$budget" --rng_seed "$RNG_SEED" > "$d/fin.log" 2>&1 || true
    xargs -r cat < "$d/fin/outputs.txt" > "$d/next.csv" 2> /dev/null || : > "$d/next.csv"
    local n; n=$(rows "$d/next.csv")
    [ "$n" -gt 0 ] || { echo "[whirl] lap $k: $(rows "$d/band.csv") bands, none re-grew ($((SECONDS - START)) s)"; return 1; }
    echo "[whirl] lap $k: $n boards at $((SECONDS - START)) s"
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
