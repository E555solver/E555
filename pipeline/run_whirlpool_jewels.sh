#!/bin/bash
# run_whirlpool_jewels.sh -- grow a board from all four sides under one border,
# keeping a band of it through every turn.
#
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db                  # an hour, all five clues, blocks frozen
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db WALL=600 DIVE=1  # 10 min, then dive
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db CLUES=center FREEZE=0 \
#       FIN_STOP_ROW=10 LAP_TIME=90 BANDS_GROWN=2 BOARD_TIME=180           # centre clue only, unfrozen
#   bash pipeline/run_whirlpool_jewels.sh INPUT=boards.csv RUN_DIR=w1      # whirl your boards
#
# The beamer grows a board upward from row 0, so its bottom rows were chosen
# first, by a beam that was still guessing, and are never searched again. The
# whirlpool turns the board a quarter turn and re-grows it, so a different side
# becomes the bottom. This runner looks for JEWELS:
#
#   s1   a beamer board, rows 0..BEAM_STOP_ROW complete; whatever it holds
#        above them in columns 0..K is cleared, and lap 1's band closes it
#   s2   s1 turned counter-clockwise and re-grown to row FIN_STOP_ROW
#   s3   s2 turned counter-clockwise and re-grown
#   s4   s3 turned counter-clockwise and re-grown
#
# all four holding the SAME complete 60-cell frame. Each turn keeps a band: the
# left columns 0..K of s_k become the bottom rows 0..K of s_k+1, which the
# finalizer locks (--finalize_from K). So consecutive boards share 14*K
# interior cells plus the frame, and a jewel's interior was re-grown from every
# side under one border. Its s2..s4 are the boards to finish (DIVE=1 dives
# them).
#
# One lap (s_k -> s_k+1):
#
#   close the band     E555_backtracker --stop_column K --with_frame: every exact
#                      filling of columns 0..K plus the frame. It closes the
#                      top-left corner block: on lap 1 rows above
#                      BEAM_STOP_ROW x columns 0..K with the frame still open,
#                      on laps 2-3 rows above FIN_STOP_ROW x columns 1..K
#                      under the fixed frame.
#                      Columns K+1..14 are cleared first and lend it their
#                      pieces; the finalizer frees them anyway. It runs on
#                      every board the batch or the previous lap wrote:
#                      a board whose corner cannot close writes no band, so
#                      the lap selects the boards that can.
#   turn               tools/E555_rotate.py 3 (counter-clockwise): the band
#                      becomes rows 0..K and the frame
#   re-grow            E555_finalizer --finalize_from K --keep_ring up to row
#                      FIN_STOP_ROW: the frame is held in place, so every lap
#                      keeps the ring lap 1 completed
#
# A lap picks BANDS_GROWN x LAP_TRIES of its bands, most different first
# (tools/E555_rank.py --diverse), and re-grows them BANDS_GROWN at a time. The
# search is depth-first: the boards a group re-grows are the next lap's pool,
# and the first chain to reach s4 ends that board.
#
# BORDERS: each produce call starts from a fresh border. anneal (the default)
# runs Stage A with --double_decker DD_SIDES, ANNEAL_RESTARTS restarts in
# parallel, and keeps the best by Score_dd (tools/E555_sort_rotations.py); the
# beamer then spends the TOP reserve last (--lambda_reserve RESERVE_F), so the
# pieces that fit row 14 stay free for the lap-1 band, which builds s1's row 14
# under columns 1..K, and the laps keep it. A path
# instead takes its next row from that rotations file each call; random uses
# the beamer's own --random_edges. Every border used is appended to
# borders.csv, so a later run can take them as a file.
#
# CLUES: every tool holds all five Eternity II clues (CLUES=all, the default),
# the centre one (center) or none. Clued boards keep each clue on its own cell
# through every lap; INPUT boards must carry the clues they are run with.
# FREEZE=1 (the default): the band also fills the five 2x2 clue blocks -- the
# centre and the four inner corners, rows/columns 1-2 and 13-14 -- and every
# lap holds them as it holds the frame (finalizer --freeze_clues, searched
# exhaustively from the lock, which needs K <= 6). The four boards of such a
# jewel share 80 pieces: the frame and the 20 block pieces. A frozen lap
# reaches row 9 rather than 10, hence FIN_STOP_ROW=9.
#
# NEEDS about 8 GB of RAM for the beamer, and DB_FILE: the beamer's 6.4 GB chain
# database is written there on the first call (~2 min) and loaded in seconds by
# every later one. A clued database leaves the clue pieces out, so the clue
# setting is appended to the name (E555.db.center, E555.db.all): one file per
# setting. Built with `make` (the finalizer needs --no_fanout_sort).
#
# WRITES into RUN_DIR:
#   jewels.csv    the jewels' four boards, ids b<board>_s1 .. b<board>_s4
#   jewels.log    one line per jewel: board, frame key, the interior cells
#                 each lap kept from its parent (shared=s1s2/s2s3/s3s4), time
#   frames.txt    the frame key of every jewel found (one frame, one jewel)
#   borders.csv   every border a produce call used (anneal or file mode)
#   run.log       one line per board; boards/<n>/ holds a board's tool logs
#                 until the next board starts
#   dived.csv     DIVE=1: the jewels' s2..s4 dived and polished by E555_diver,
#   best.csv      and its 20 best boards
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout with bin/ built
SEED=data/seed_Edge5.txt                # relative to REPO
RUN_DIR=whirlpool_jewels
THREADS=$(nproc)
DB_FILE=                # the beamer's chain database cache; strongly recommended
WALL=3600               # seconds to run
BOARD_TIME=1800         # seconds at most per board (its three laps)
RNG_SEED=0              # 0 = random, different for every copy started
INPUT=                  # whirl these boards instead of producing them
DIVE=0                  # 1 = dive the jewels at the end
CLUES=all               # none | center | all: the Eternity II clues every tool holds
FREEZE=1                # 1 = the band also fills the five 2x2 clue blocks and every
                        # lap holds them, like the frame; 0 = off
K=4                     # band width: columns 0..K of s_k become rows 0..K of s_k+1
                        # (the finalizer's --finalize_from); at most 6 with FREEZE

# borders: one per produce call
BORDERS=anneal          # anneal | random | a rotations file (its next row each call)
ANNEAL_RESTARTS=20      # Stage A restarts, run in parallel; the best by Score_dd is used
ANNEAL_STEPS=250000     # steps per restart
DD_SIDES=ALL            # --double_decker sides: the second rings the annealer scores
DD_RESERVE=16           # --decker_reserve: inner pieces reserved per two-tall side
RESERVE_F=2             # beamer --lambda_reserve: cost of placing a TOP reserve piece
                        # (0 = off; never with BORDERS=random)

# s1: the beamer, then one board per frame, shuffled
BEAM_WIDTH=10000
BEAM_STOP_ROW=10        # s1 is complete up to this row
BEAM_BACKTRACK=6        # exhaustive from this row (--backtrack_row)
BEAM_MIN_COL=0          # N > 0: the beamer writes only boards whose columns 1..N grow
                        # whole to row 14 (--backtrack_min_col, 0..K). The lap-1 band
                        # closes the corner above BEAM_STOP_ROW itself, so it is
                        # only a pre-filter, for BORDERS=random; a rotations border
                        # fixes the left column, and N = 2 wrote no board
BEAM_BOTTOMS=0          # bottom orderings tried per border, sampled with BEAM_TAU;
                        # 0 = all, until BEAM_TIME or BEAM_BOARDS (not with random)
BEAM_COLUMNS=16         # left columns tried per bottom
BEAM_TAU=0.1            # randomness in picking bottoms and columns; 0 = the best-ranked
BEAM_BOARDS=2000        # boards per beamer call
BEAM_TIME=570           # seconds per beamer call

# the band: E555_backtracker --stop_column K before each turn
BANDS=60                # fillings kept per board
BAND_TIME=15            # seconds per board

# the re-grow: E555_finalizer --finalize_from K
BANDS_GROWN=8           # bands per finalizer call
LAP_TRIES=3             # finalizer calls a lap may make before it gives up
FIN_STOP_ROW=9          # s2..s4 are re-grown up to this row (unfrozen: 10)
FIN_WIDTH=20000
LAP_TIME=480            # seconds per finalizer call (unfrozen: 90)
FIN_BOARDS=200          # boards a finalizer call writes: the next lap's pool
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 2 ;;
    esac
done
die() { echo "$*" >&2; exit 2; }
case "$CLUES" in
    none|0) CLUES=none; CLUE_ARG=() ;;
    center) CLUE_ARG=(--clue_center) ;;
    all|1)  CLUES=all;  CLUE_ARG=(--clue_center --clue_corners) ;;
    *) die "CLUES must be none, center or all (got: $CLUES)" ;;
esac
# Frozen blocks: the band fills them (--with_clue_regions) and the finalizer
# holds them (--freeze_clues). Only the finalizer's backtrack sees them, so it
# searches exhaustively from its lock (--backtrack_row K), each band with an
# equal share of LAP_TIME.
REGION_ARG=(); FREEZE_ARG=()
case "$FREEZE" in
    0) ;;
    1) REGION_ARG=(--with_clue_regions); FREEZE_ARG=(--freeze_clues) ;;
    *) die "FREEZE must be 0 or 1 (got: $FREEZE)" ;;
esac
# Checked here, because a bad value would otherwise fail every lap quietly.
for v in K BOARD_TIME ANNEAL_RESTARTS ANNEAL_STEPS DD_RESERVE BEAM_MIN_COL BEAM_STOP_ROW BEAM_BACKTRACK \
         BEAM_BOTTOMS BEAM_COLUMNS BEAM_BOARDS BEAM_TIME BANDS BAND_TIME BANDS_GROWN \
         LAP_TRIES FIN_STOP_ROW LAP_TIME FIN_BOARDS; do
    [[ ${!v} =~ ^[0-9]+$ ]] || die "$v must be a whole number (got: ${!v})"
done
for v in BEAM_TAU RESERVE_F; do
    [[ ${!v} =~ ^[0-9]+([.][0-9]+)?$ ]] || die "$v must be a number, 0 or more (got: ${!v})"
done
[ "$BEAM_BACKTRACK" -lt "$BEAM_STOP_ROW" ] && [ "$BEAM_STOP_ROW" -le 14 ] ||
    die "need BEAM_BACKTRACK < BEAM_STOP_ROW <= 14"
[ "$FIN_STOP_ROW" -le 14 ] || die "FIN_STOP_ROW is at most 14"
[ "$K" -ge 1 ] && [ "$K" -lt "$FIN_STOP_ROW" ] || die "need 1 <= K < FIN_STOP_ROW (got K=$K)"
[ "$FREEZE" = 0 ] || [ "$K" -le 6 ] ||
    die "with FREEZE, K is at most 6 (--freeze_clues backtracks from row 6 or lower)"
[ "$BEAM_MIN_COL" -le "$K" ] || die "need BEAM_MIN_COL <= K (got BEAM_MIN_COL=$BEAM_MIN_COL, K=$K)"
[ "$BANDS_GROWN" -ge 1 ] && [ "$LAP_TRIES" -ge 1 ] && [ "$FIN_BOARDS" -ge 1 ] && [ "$BANDS" -ge 1 ] ||
    die "BANDS, BANDS_GROWN, LAP_TRIES and FIN_BOARDS must be 1 or more"
BIN=$REPO/bin; TOOLS=$REPO/tools
case "$SEED" in /*) ;; *) SEED=$REPO/$SEED ;; esac
case "$BORDERS" in
    anneal|random) ;;
    *) [ -s "$BORDERS" ] || die "BORDERS must be anneal, random or a rotations file (got: $BORDERS)"
       BORDERS=$(cd "$(dirname "$BORDERS")" && pwd)/$(basename "$BORDERS") ;;
esac
[ "$BORDERS" = anneal ] || [ "$BORDERS" = random ] ||
    [ "$(awk '!/^[#%]/ && NF {n++} END {print n + 0}' "$BORDERS")" -gt 0 ] ||
    die "BORDERS: $BORDERS holds no border row"
[ -z "$INPUT" ] || INPUT=$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")
[ -z "$DB_FILE" ] || DB_FILE=$(cd "$(dirname "$DB_FILE")" && pwd)/$(basename "$DB_FILE")
[ -z "$DB_FILE" ] || [ "$CLUES" = none ] || DB_FILE=$DB_FILE.$CLUES   # one cache per clue setting
# bash seeds RANDOM per process, so copies started together still differ
[ "$RNG_SEED" != 0 ] || RNG_SEED=$(( (RANDOM << 15 | RANDOM) + 1 ))
for t in beamer backtracker finalizer; do
    [ -x "$BIN/E555_$t" ] || { echo "no $BIN/E555_$t: run make in $REPO" >&2; exit 1; }
done
mkdir -p "$RUN_DIR"; RUN_DIR=$(cd "$RUN_DIR" && pwd); cd "$RUN_DIR"
: > jewels.csv; : > jewels.log; : > frames.txt; : > run.log
log() { echo "$*" | tee -a run.log; }

# ---- helpers ------------------------------------------------------------------
# Boards are CSV rows whose last 512 fields are pos[256] (the cell of each piece,
# 999 = unplaced) and rot[256]; cell = row*16 + col, row 0 at the bottom. Field
# 1 is the board's id: every board this runner handles gets one of its own, and
# the backtracker writes it on each band it cuts from that board, which is how
# a chain is traced back from s4 to s1.

# count_boards FILE / nth_board FILE N -- data lines only (# and % are comments)
count_boards() { awk '!/^[#%]/ && NF {n++} END {print n + 0}' "$1" 2> /dev/null || echo 0; }
nth_board()    { awk -v n="$2" '!/^[#%]/ && NF && ++k == n {print; exit}' "$1"; }
# board_by_id FILE ID -- the data line whose id is ID
board_by_id()  { awk -F, -v id="$2" '!/^[#%]/ && NF && $1 == id {print; exit}' "$1"; }
# tag_ids IN OUT PREFIX -- give every board the id PREFIX<n>
tag_ids() {
    awk -F, -v pre="$3" 'BEGIN {OFS = ","} !/^[#%]/ && NF {$1 = pre (++n); print}' "$1" > "$2"
}

# The frame as a ring: its 60 cells counter-clockwise from cell 0 (bottom row
# left to right, right column up, top row right to left, left column down).
# A counter-clockwise quarter turn moves ring position i to i+15, so one frame
# turned any way is the same ring shifted by a multiple of 15. RING prints, per
# board, the piece at each ring position ("-" where empty).
RING='
function ring_cells(   i, k) {
    k = 0
    for (i = 0;  i <= 15; i++) RC[k++] = i                 # bottom row
    for (i = 1;  i <= 15; i++) RC[k++] = i * 16 + 15       # right column
    for (i = 14; i >= 0;  i--) RC[k++] = 15 * 16 + i       # top row
    for (i = 14; i >= 1;  i--) RC[k++] = i * 16            # left column
}
function ring_of(line,   x, n, p, at, i, s) {               # 60 fields, space-separated
    n = split(line, x, ",") - 512; split("", at)
    for (p = 0; p < 256; p++) if (x[n + 1 + p] != 999) at[x[n + 1 + p] + 0] = p
    s = ""
    for (i = 0; i < 60; i++) s = s (i ? " " : "") ((RC[i] in at) ? at[RC[i]] : "-")
    return s
}
function shift(s, d,   f, i, t) {                          # position i takes i+d
    split(s, f, " "); t = ""
    for (i = 0; i < 60; i++) t = t (i ? " " : "") f[(i + d) % 60 + 1]
    return t
}
function key_of(s,   d, t, best) {                         # the same for all 4 turns
    best = s
    for (d = 15; d < 60; d += 15) { t = shift(s, d); if (t < best) best = t }
    gsub(/ /, ".", best); return best
}
# The pieces on the five frozen 2x2 clue blocks (rows and columns 1-2 and
# 13-14, and rows/columns 7-8), in piece order, or "" unless all their cells
# are placed. A turn moves the blocks onto each other, so a held set reads the
# same on every board of a chain.
function region_set(line,   x, n, p, c, r, k, s, m) {
    n = split(line, x, ",") - 512; s = ""; m = 0
    for (p = 0; p < 256; p++) {
        c = x[n + 1 + p]; if (c == 999) continue
        r = int(c / 16); k = c % 16
        if (((r == 1 || r == 2 || r == 13 || r == 14) && (k == 1 || k == 2 || k == 13 || k == 14)) ||
            ((r == 7 || r == 8) && (k == 7 || k == 8))) s = s (m++ ? "." : "") p
    }
    return m == 20 ? s : ""
}
BEGIN { ring_cells() }'

# open_corner IN OUT -- clear the top-left corner the lap-1 band closes: rows
# above BEAM_STOP_ROW x columns 0..K, the frame cells included. A rotations
# border fixes the left column up to row 14 and the top-left corner, and no
# band closed against them (0 of 628 beamer boards, CLUES=all, K=4); cleared,
# 44 of the 628 got a band. The band puts a cleared clue back on its own cell.
open_corner() {
    awk -F, -v s="$BEAM_STOP_ROW" -v kc="$K" 'BEGIN {OFS = ","} !/^[#%]/ && NF {
        n = NF - 512
        for (p = 1; p <= 256; p++) {
            c = $(n + p) + 0; if (c == 999) continue
            if (int(c / 16) > s && c % 16 <= kc) $(n + p) = 999
        }
        print }' "$1" > "$2"
}

# one_per_frame IN OUT -- keep the first board of each distinct frame
one_per_frame() {
    awk "$RING"' !/^[#%]/ && NF { r = ring_of($0); if (!(r in seen)) { seen[r] = 1; print } }' "$1" > "$2"
}

# jewel_key CHAIN -- the frame key if the four boards in CHAIN (s1..s4) make a
# jewel, else nothing. s2..s4 must each hold all 60 frame cells, the same frame
# turned counter-clockwise with the board (ring shift 45 per turn); s1 must
# agree with it wherever s1 has a frame piece. Under FREEZE, s2..s4 must also
# hold the same frozen clue-block pieces, and the key is the frame's and
# theirs. Not re-tested here, because each lap guarantees it: every board is
# exactly matched (no breaks), rows 0..FIN_STOP_ROW are complete, and each lap
# locks its parent's band.
jewel_key() {
    awk -v freeze="$FREEZE" "$RING"' !/^[#%]/ && NF { R[++k] = ring_of($0); Q[k] = region_set($0) }
        END {
            if (k != 4) exit
            for (j = 2; j <= 4; j++) if (R[j] ~ /-/) exit           # frame complete
            if (shift(R[2], 45) != R[3] || shift(R[2], 30) != R[4]) exit
            split(shift(R[1], 45), a, " "); split(R[2], b, " ")     # s1 turned once
            for (i = 1; i <= 60; i++) if (a[i] != "-" && a[i] != b[i]) exit
            if (!freeze) { print key_of(R[2]); exit }
            if (Q[2] == "" || Q[3] != Q[2] || Q[4] != Q[2]) exit    # blocks held
            print key_of(R[2]) "/" Q[2]
        }' "$1"
}

# held_blocks IN OUT -- the boards of IN that hold all five frozen clue blocks
held_blocks() {
    awk "$RING"' !/^[#%]/ && NF && region_set($0) != ""' "$1" > "$2"
}

# shared_cells A B -- interior cells where board A, turned counter-clockwise,
# and board B hold the same piece at the same spin
shared_cells() {
    python3 "$TOOLS/E555_rotate.py" "$1" 3 --out "$BOARD_DIR/turn.csv" --seed_file "$SEED" > /dev/null
    awk -F, '!/^[#%]/ && NF {
        n = NF - 512
        for (p = 1; p <= 256; p++) {
            c = $(n + p) + 0; if (c == 999) continue    # fields may carry a space
            r = int(c / 16); k = c % 16; if (r < 1 || r > 14 || k < 1 || k > 14) continue
            key = p "," c "," ($(n + 256 + p) + 0)
            if (FILENAME == ARGV[1]) A[key] = 1; else if (key in A) s++
        } }
        END { print s + 0 }' "$BOARD_DIR/turn.csv" "$2"
}

# ---- the band ---------------------------------------------------------------------
left() { echo $(( BOARD_END - SECONDS )); }
cap()  { local a=$1 b; b=$(left); echo $(( a < b ? a : b )); }   # min(a, time left)

# cut_bands POOL OUT LIMIT -- E555_backtracker --stop_column K --with_frame on
# every board of POOL: up to BANDS exact fillings of each board's columns 0..K
# and the frame (FREEZE: and the clue blocks), each carrying its board's id.
# Columns K+1..14 are cleared and lend their pieces. Most corners have no
# filling, and mrv proves that fastest. LIMIT caps the whole call, in seconds.
cut_bands() {
    local pool=$1 out=$2 limit=$3 bt=${2%.csv}.bt.csv f
    [ "$limit" -gt 2 ] || { : > "$out"; return; }
    timeout -k 5 "$limit" "$BIN/E555_backtracker" "$SEED" "$pool" "$bt" \
        --stop_column "$K" --with_frame "${REGION_ARG[@]}" "${CLUE_ARG[@]}" \
        --order mrv --break_mode any --breaks 0 \
        --max_emitted "$BANDS" --time_limit "$BAND_TIME" --threads "$THREADS" \
        --print_cmd > "${bt%.csv}.log" 2>&1 || true
    f=$bt.stop_col$K.csv
    if [ -s "$f" ]; then mv "$f" "$out"; else : > "$out"; fi
    rm -f "$bt" "$bt".*
}

# ---- produce: a border, beamer boards, one per frame, and their lap-1 bands -----
# Consecutive beamer boards differ in a few interior cells and share their ring,
# so lap 1 would find the same frame again; one board per frame avoids that.

# border N DIR -- sets BORDER_ARGS (the beamer's border arguments) and
# BORDER_NOTE for call N
border() {
    local n=$1 d=$2 rows row
    case "$BORDERS" in
        random)
            BORDER_ARGS=(--random_edges --samples 0); BORDER_NOTE="random edges" ;;
        anneal)
            python3 "$REPO/src/A_border/E555_edge_annealer.py" "$SEED" --out "$d/borders.csv" \
                --restarts "$ANNEAL_RESTARTS" --steps "$ANNEAL_STEPS" \
                --double_decker "$DD_SIDES" --decker_reserve "$DD_RESERVE" \
                --threads "$THREADS" --rng_seed "$((RNG_SEED + n))" > "$d/anneal.log" 2>&1 ||
                { BORDER_ARGS=(); BORDER_NOTE="the annealer failed (see $d/anneal.log)"; return; }
            python3 "$TOOLS/E555_sort_rotations.py" "$d/borders.csv" --sort score_dd --top 1 \
                -o "$d/border.csv" >> "$d/anneal.log" 2>&1 ||
                { BORDER_ARGS=(); BORDER_NOTE="sorting the borders failed (see $d/anneal.log)"; return; }
            { echo "# produce $n"; cat "$d/border.csv"; } >> "$RUN_DIR/borders.csv"
            BORDER_ARGS=("$d/border.csv" --start_row 0 --num_rows 1)
            BORDER_NOTE="annealed, best of $ANNEAL_RESTARTS: $(grep -o -m 1 'Score_dd=[^ ]*' "$d/border.csv" || :) $(grep -o -m 1 'Decker=[^ ]*' "$d/border.csv" || :)" ;;
        *)
            rows=$(count_boards "$BORDERS"); row=$(( (n - 1) % rows ))
            BORDER_ARGS=("$BORDERS" --start_row "$row" --num_rows 1)
            BORDER_NOTE="$(basename "$BORDERS") row $row" ;;
    esac
    [ "$BORDERS" = random ] || BORDER_ARGS+=(--top_bottoms "$BEAM_BOTTOMS" --tau_bottoms "$BEAM_TAU")
    [ "$BORDERS" = random ] || [ "$RESERVE_F" = 0 ] || BORDER_ARGS+=(--lambda_reserve "$RESERVE_F")
}

produce() {    # produce N -> queue.csv
    local d=produce$1 db=(); rm -rf "$d"; mkdir "$d"
    [ -z "$DB_FILE" ] || db=(--db_file "$DB_FILE")
    border "$1" "$d"
    PRODUCED=0; : > queue.csv
    [ "${#BORDER_ARGS[@]}" -gt 0 ] || return    # keeps $d/anneal.log
    timeout -k 10 $((BEAM_TIME + 60)) "$BIN/E555_beamer" "$SEED" "${BORDER_ARGS[@]}" \
        --top_columns "$BEAM_COLUMNS" --tau_columns "$BEAM_TAU" "${CLUE_ARG[@]}" \
        --beam_width "$BEAM_WIDTH" --backtrack_row "$BEAM_BACKTRACK" --stop_row "$BEAM_STOP_ROW" \
        --backtrack_min_col "$BEAM_MIN_COL" \
        --max_emitted "$BEAM_BOARDS" --wall_time "$BEAM_TIME" "${db[@]}" \
        --threads "$THREADS" --rng_seed "$((RNG_SEED + $1))" --out_dir "$d" --print_cmd \
        > "$d.log" 2>&1 || log "[produce] the beamer exited non-zero (see $d.log)"
    xargs -r cat < "$d/outputs.txt" > "$d/all.csv" 2> /dev/null || : > "$d/all.csv"
    open_corner "$d/all.csv" "$d/open.csv"
    one_per_frame "$d/open.csv" "$d/one.csv"
    PRODUCED=$(count_boards "$d/all.csv")
    shuf --random-source=<(yes "$RNG_SEED.$1") "$d/one.csv" > "$d/shuf.csv"
    tag_ids "$d/shuf.csv" queue.csv "q$1_"
    rm -rf "$d"
}

# lap1_bands -- the lap-1 bands of every board in queue.csv, in one backtracker
# call (one board per thread), split by board id into bands1/<id>.csv
lap1_bands() {
    local t=$(( WALL - (SECONDS - T0) ))
    rm -rf bands1; mkdir bands1
    cut_bands queue.csv bands1.csv "$t"
    LC_ALL=C sort -t, -k1,1 -s bands1.csv | awk -F, '!/^[#%]/ && NF {
        if ($1 != id) { if (f) close(f); id = $1; f = "bands1/" id ".csv" }
        print > f }'
    NBANDED=$(find bands1 -name '*.csv' | wc -l)
    rm -f bands1.csv
}

# ---- one lap ----------------------------------------------------------------------
# grow K G BANDS DIR -- turn the bands counter-clockwise and re-grow them to
# FIN_STOP_ROW: DIR/next.csv (ids L<K>g<G>_<n>, the next lap's pool) and
# DIR/parents.txt (each board's id, then the id of the board its band was cut
# from). Fails when nothing re-grew, with the reason in WHY.
grow() {
    local k=$1 g=$2 bands=$3 d=$4 t nb
    rm -rf "$d"; mkdir -p "$d"      # the finalizer appends to its output
    python3 "$TOOLS/E555_rotate.py" "$bands" 3 --out "$d/turned.csv" --seed_file "$SEED" > /dev/null
    nb=$(count_boards "$d/turned.csv")
    t=$(cap "$LAP_TIME"); [ "$t" -gt 5 ] || { WHY="out of time"; return 1; }
    local lock=()
    [ "$FREEZE" = 0 ] || lock=(--backtrack_row "$K" --time_limit "$(( t / nb > 1 ? t / nb : 1 ))")
    timeout -k 5 "$(left)" "$BIN/E555_finalizer" "$SEED" "$d/turned.csv" --out_dir "$d/fin" \
        --finalize_from "$K" --stop_row "$FIN_STOP_ROW" --keep_ring --no_fanout_sort \
        "${CLUE_ARG[@]}" "${FREEZE_ARG[@]}" "${lock[@]}" \
        --beam_width "$FIN_WIDTH" --lambda_Mahalanobis 0 \
        --frac_rand 0.0 --max_emitted "$FIN_BOARDS" --wall_time "$t" --rng_seed "$((RNG_SEED + NB))" \
        --threads "$THREADS" --print_cmd > "$d/fin.log" 2>&1 || true
    xargs -r cat < "$d/fin/outputs.txt" > "$d/raw.csv" 2> /dev/null || : > "$d/raw.csv"
    # The finalizer names a board p<line>r<repeat>l<column>, line counted from 0
    # over the data lines of its input: that line's id is the board's parent.
    awk -F, -v pre="L${k}g${g}_" -v max="$FIN_BOARDS" -v par="$d/parents.txt" 'BEGIN {OFS = ","}
        FNR == NR { if (!/^[#%]/ && NF) P[n++] = $1; next }
        !/^[#%]/ && NF && m < max && match($1, /^p[0-9]+/) {
            line = substr($1, 2, RLENGTH - 1) + 0; $1 = pre (++m)
            print $1 "," P[line] > par; print }' "$d/turned.csv" "$d/raw.csv" > "$d/next.csv"
    [ "$(count_boards "$d/next.csv")" -gt 0 ] || { WHY="lap $k re-grew nothing"; return 1; }
    # The finalizer re-grows a band whose blocks did not all freeze without
    # them (its [freeze] lines in fin.log say why); such boards are dropped.
    if [ "$FREEZE" = 1 ]; then
        held_blocks "$d/next.csv" "$d/held.csv"; mv "$d/held.csv" "$d/next.csv"
        [ "$(count_boards "$d/next.csv")" -gt 0 ] ||
            { WHY="lap $k: the clue blocks did not hold (see [freeze] in fin.log)"; return 1; }
    fi
}

# ---- one board: depth-first through three laps --------------------------------------
# whirl K POOL tries to extend the chain from lap K's pool (the s_K candidates)
# to s4: it cuts the pool's bands (lap 1: the batch's, for s1), picks the most
# different, and re-grows them BANDS_GROWN at a time; each group that re-grows
# is the pool of lap K+1. NEXT[K+1] is the group directory the chain runs
# through. Succeeds once lap 3 re-grows.
declare -a NEXT
DEEPEST=1
whirl() {
    local k=$1 pool=$2 d=$BOARD_DIR/lap$1 bands n g i
    rm -rf "$d"; mkdir -p "$d"
    if [ "$k" = 1 ]; then
        bands=$pool
    else
        cut_bands "$pool" "$d/bands.csv" "$(left)"; bands=$d/bands.csv
    fi
    [ "$(count_boards "$bands")" -gt 0 ] || { WHY="lap $k: no band (no corner closes)"; return 1; }
    python3 "$TOOLS/E555_rank.py" "$bands" --seed_file "$SEED" --diverse $(( BANDS_GROWN * LAP_TRIES )) \
        --out "$d/picked.csv" > /dev/null 2>&1 || cp "$bands" "$d/picked.csv"
    n=$(count_boards "$d/picked.csv")
    for ((g = 1; (g - 1) * BANDS_GROWN < n; g++)); do
        [ "$(left)" -gt 10 ] || { WHY="out of time"; return 1; }
        awk -v a=$(( (g - 1) * BANDS_GROWN )) -v b=$(( g * BANDS_GROWN )) \
            '!/^[#%]/ && NF { i++; if (i > a && i <= b) print }' "$d/picked.csv" > "$d/group$g.csv"
        grow "$k" "$g" "$d/group$g.csv" "$d/g$g" || continue
        NEXT[$((k + 1))]=$d/g$g
        [ "$((k + 1))" -le "$DEEPEST" ] || DEEPEST=$((k + 1))
        [ "$k" = 3 ] && return 0
        whirl $((k + 1)) "$d/g$g/next.csv" && return 0
    done
    return 1
}

# whirl_board N FILE -- whirl one board; on a jewel with a new frame, keep it
whirl_board() {
    local nb=$1 t0=$SECONDS key j id p shared
    BOARD_DIR=$RUN_DIR/boards/$nb; rm -rf "$RUN_DIR/boards"; mkdir -p "$BOARD_DIR"
    BOARD_END=$((SECONDS + BOARD_TIME)); DEEPEST=1; WHY=
    cp "$2" "$BOARD_DIR/s1.csv"; id=$(cut -d, -f1 "$2")
    if [ ! -s "bands1/$id.csv" ]; then
        log "[board $nb] reached s1; last: lap 1: no band (the frame and columns 0..$K do not close) (0 s)"; return
    fi
    if ! whirl 1 "bands1/$id.csv"; then
        log "[board $nb] reached s$DEEPEST; last: $WHY ($((SECONDS - t0)) s)"; return
    fi
    # The chain, from s4 back to s1: each board's parent is the board its band
    # was cut from.
    nth_board "${NEXT[4]}/next.csv" 1 > "$BOARD_DIR/s4.csv"
    for j in 3 2; do
        p=$(awk -F, -v id="$(cut -d, -f1 "$BOARD_DIR/s$((j + 1)).csv")" '$1 == id {print $2; exit}' \
            "${NEXT[$((j + 1))]}/parents.txt")
        board_by_id "${NEXT[$j]}/next.csv" "$p" > "$BOARD_DIR/s$j.csv"
    done
    cat "$BOARD_DIR"/s[1-4].csv > "$BOARD_DIR/chain.csv"
    key=$(jewel_key "$BOARD_DIR/chain.csv")
    if [ -z "$key" ]; then
        log "[board $nb] reached s4, but the frame was not held ($((SECONDS - t0)) s)"; return
    fi
    if grep -qxF "$key" frames.txt; then
        log "[board $nb] jewel, frame already found ($((SECONDS - t0)) s)"; return
    fi
    echo "$key" >> frames.txt; NJ=$((NJ + 1))
    for j in 1 2 3 4; do
        awk -F, -v id="b${nb}_s$j" 'BEGIN {OFS = ","} !/^[#%]/ && NF {$1 = id; print; exit}' \
            "$BOARD_DIR/s$j.csv" >> jewels.csv
    done
    shared=$(shared_cells "$BOARD_DIR/s1.csv" "$BOARD_DIR/s2.csv")/$(shared_cells "$BOARD_DIR/s2.csv" "$BOARD_DIR/s3.csv")/$(shared_cells "$BOARD_DIR/s3.csv" "$BOARD_DIR/s4.csv")
    echo "board=$nb jewel=$NJ frame=$key shared=$shared seconds=$((SECONDS - T0))" >> jewels.log
    log "[board $nb] JEWEL $NJ, new frame, shared $shared cells ($((SECONDS - t0)) s)"
}

# ---- main loop -----------------------------------------------------------------------
log "[cfg] run_dir=$RUN_DIR threads=$THREADS wall=$WALL board_time=$BOARD_TIME rng_seed=$RNG_SEED db_file=${DB_FILE:-<rebuilt each call>}"
log "[cfg] clues=$CLUES freeze=$FREEZE K=$K borders=$BORDERS beam: stop_row=$BEAM_STOP_ROW min_col=$BEAM_MIN_COL bottoms=$BEAM_BOTTOMS columns=$BEAM_COLUMNS tau=$BEAM_TAU; bands: $BANDS per board, $BANDS_GROWN x $LAP_TRIES grown; finalizer: stop_row=$FIN_STOP_ROW lap_time=$LAP_TIME pool=$FIN_BOARDS"
T0=$SECONDS; NB=0; NJ=0; NP=0; EMPTY=0
# The beamer maps the database file into memory and reads only the pages it
# touches, in random order. From a cold disk (after a reboot) that kept one
# produce call busy for 10 minutes with nothing emitted; one sequential read
# first (1.5 to 8 min for 6.8 GB, measured, depending on the disk; it counts
# against WALL) puts the whole file in the page cache.
if [ -z "$INPUT" ] && [ -s "$DB_FILE" ]; then
    cat "$DB_FILE" > /dev/null
    log "[init] database read into the page cache ($((SECONDS - T0)) s)"
fi
while [ $((SECONDS - T0)) -lt "$WALL" ]; do
    NBANDED=0
    if [ -n "$INPUT" ]; then
        [ "$NP" = 0 ] || break                  # INPUT: one pass over its boards
        NP=1; tag_ids "$INPUT" queue.csv "in_"
        nq=$(count_boards queue.csv)
        lap1_bands
        log "[input] $nq boards, $NBANDED with a lap-1 band ($((SECONDS - T0)) s)"
    else
        NP=$((NP + 1)); produce "$NP"
        nq=$(count_boards queue.csv)
        [ "$nq" = 0 ] || lap1_bands
        log "[produce $NP] border: $BORDER_NOTE"
        log "[produce $NP] $nq of the $PRODUCED boards written: one per frame; $NBANDED with a lap-1 band ($((SECONDS - T0)) s)"
    fi
    if [ "$nq" = 0 ]; then
        EMPTY=$((EMPTY + 1)); [ "$EMPTY" -lt 2 ] && continue
        log "two empty produce calls in a row: stopping"; break
    fi
    EMPTY=0
    for ((i = 1; i <= nq && SECONDS - T0 < WALL; i++)); do
        NB=$((NB + 1)); nth_board queue.csv "$i" > board.csv
        whirl_board "$NB" board.csv
    done
done
rm -rf boards bands1 board.csv queue.csv
log "[done] $NB boards, $NJ jewels in $((SECONDS - T0)) s -> $RUN_DIR/jewels.csv"

if [ "$DIVE" = 1 ] && [ "$NJ" -gt 0 ]; then
    grep -E '^b[0-9]+_s[234],' jewels.csv > to_dive.csv
    "$BIN/E555_diver" "$SEED" to_dive.csv dived.csv --end_dive 10000 --end_polish 20000 \
        --emit_score 0 --threads "$THREADS" --wall_time 900 --print_cmd > dive.log 2>&1 || true
    python3 "$TOOLS/E555_rank.py" dived.csv --seed_file "$SEED" --top 20 --out best.csv --rescore > /dev/null || true
    log "[dive] $(count_boards dived.csv) boards dived -> $RUN_DIR/best.csv"
fi
