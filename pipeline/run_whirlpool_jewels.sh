#!/bin/bash
# run_whirlpool_jewels.sh -- grow a board from all four sides under one border.
#
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db                 # an hour
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db WALL=600 DIVE=1 # 10 min, then dive
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db CLUES=all       # all five clues, frozen
#   bash pipeline/run_whirlpool_jewels.sh INPUT=boards.csv RUN_DIR=w1     # whirl your boards
#
# The beamer grows a board upward from row 0, so its bottom rows were chosen
# first, by a beam that was still guessing, and are never searched again. The
# whirlpool turns the board a quarter turn and re-grows it, so a different side
# becomes the bottom. This runner looks for JEWELS:
#
#   s1   a beamer board, rows 0..BEAM_STOP_ROW complete
#   s2   s1 turned clockwise and re-grown to row FIN_STOP_ROW
#   s3   s2 turned clockwise and re-grown to row FIN_STOP_ROW
#   s4   s3 turned clockwise and re-grown to row FIN_STOP_ROW
#
# all four holding the SAME complete 60-cell frame. A jewel is a board whose
# interior was re-searched from every side under one fixed border, so no side
# of it is a leftover of an early guess. Its s2..s4 are the boards to finish
# (DIVE=1 dives them).
#
# CLUES: every tool holds the centre clue (CLUES=center, the default), all five
# Eternity II clues (all) or none. Clued boards keep each clue on its own cell
# through every lap; INPUT boards must carry the clues they are run with.
# FREEZE (on by default with CLUES=all): the band cut also fills the five 2x2
# clue blocks -- the centre and the four inner corners, rows/columns 1-2 and
# 13-14 -- and every later lap keeps them in place, as it keeps the frame. The
# four boards of such a jewel share 80 pieces: the frame and the 20 block
# pieces. Most boards fail at once (no cut closes their corners), and a frozen
# lap reaches row 9, not 10, so FREEZE switches the auto settings to cuts 2:4
# and 2:3 re-grown to row 9; each jewel is a strong target for a long tail
# search. FREEZE=corners freezes the four corner blocks only.
#
# One lap (s_k -> s_k+1):
#
#   turn clockwise          tools/E555_rotate.py
#   keep a small block      rows 1..BR, columns 1..KC of the old board; the rest
#                           of rows 1..BR is cleared, leaving the cut room
#   cut the band exactly    E555_backtracker --stop_row BR --with_frame: every
#                           exact filling of rows 0..BR plus the whole frame
#                           (FREEZE: --with_clue_regions, plus the clue blocks)
#   re-grow rows BR+1..     E555_finalizer --keep_ring, up to FIN_STOP_ROW: the
#                           frame is held in place, so every lap keeps the ring
#                           lap 1 completed (FREEZE: --freeze_clues holds the
#                           clue blocks the same way, searched exhaustively
#                           from the lock)
#
# A cut is written BR:KC. Lap 1 uses LAP1_CUT; laps 2 and 3 try each of
# LATER_CUTS in turn. The search is depth-first: the first boards a lap
# re-grows go straight into the next lap, and the first chain to reach s4 ends
# that board.
#
# NEEDS about 8 GB of RAM for the beamer, and DB_FILE: the beamer's 6.4 GB chain
# database is written there on the first call (~2 min) and loaded in seconds by
# every later one. A clued database leaves the clue pieces out, so the clue
# setting is appended to the name (E555.db.center, E555.db.all): one file per
# setting. Built with `make` (the finalizer needs --no_fanout_sort).
#
# WRITES into RUN_DIR:
#   jewels.csv    the jewels' four boards, ids b<board>_s1 .. b<board>_s4
#   jewels.log    one line per jewel: board, frame key, time
#   frames.txt    the frame key of every jewel found (one frame, one jewel)
#   run.log       one line per board; boards/<n>/ holds a board's tool logs
#                 until the next board starts
#   dived.csv     DIVE=1: the jewels' s2..s4 dived and polished by E555_diver,
#   best.csv      and its 20 best boards
#
# From the whirlpool campaign 20261028-0249 (candidate v08), which measured
# 87 jewels per hour on 4 vCPUs (Xeon, 2.1 GHz), unclued and before
# BEAM_MIN_COL and BEAM_TAU existed; its README, on the agent_runs branch, has
# the evidence behind the other settings below.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout with bin/ built
SEED=data/seed_Edge5.txt                # relative to REPO
RUN_DIR=whirlpool_jewels
THREADS=$(nproc)
DB_FILE=                # the beamer's chain database cache; strongly recommended
WALL=3600               # seconds to run
BOARD_TIME=auto         # seconds at most per board (its three laps); auto = 180,
                        # or 1800 with FREEZE
RNG_SEED=0              # 0 = random, different for every copy started
INPUT=                  # whirl these boards instead of producing them
DIVE=0                  # 1 = dive the jewels at the end
CLUES=center            # none | center | all: the Eternity II clues every tool
                        # holds (all = the centre and the four corner clues)
FREEZE=auto             # 1 = the band cut also fills the five 2x2 clue blocks and
                        # every lap keeps them, like the frame; corners = the four
                        # corner blocks only, the centre one is re-grown each lap;
                        # auto = 1 with CLUES=all, else 0

# board supply: the beamer, then one board per frame, shuffled
BEAM_WIDTH=10000
BEAM_STOP_ROW=10        # s1 is complete up to this row
BEAM_BACKTRACK=6        # exhaustive from this row (--backtrack_row)
BEAM_MIN_COL=4          # keep only boards whose columns 0..N then grow whole up to
                        # row 14 (0 = off); boards whose column 0 then cannot
                        # take a corner are dropped
BEAM_COLUMNS=auto       # left border columns tried per bottom; auto = 4, or 16 with
                        # CLUES=all (the corner clues leave few columns that work)
BEAM_TAU=0.1            # randomness in picking those columns; 0 = the best-ranked
BEAM_BOARDS=2000        # boards per beamer call
BEAM_TIME=570           # seconds per beamer call

# the band cut
LAP1_CUT=auto           # BR:KC for lap 1; auto = 3:4, or 2:4 with FREEZE
LATER_CUTS=auto         # laps 2 and 3, tried in this order; auto = "4:3 3:4",
                        # or "2:4 2:3" with FREEZE
BANDS_ENUMERATED=60     # each cut finds up to this many exact fillings (bands)
BANDS_GROWN=auto        # the finalizer re-grows the BANDS_GROWN most different
                        # ones; more gives a lap more chances, ~10 s per extra
                        # band; auto = 2, or 8 with FREEZE
BAND_TIME=15            # seconds per cut

# the re-grow
FIN_STOP_ROW=auto       # s2..s4 are re-grown up to this row; auto = 10, or 9
                        # with FREEZE
FIN_WIDTH=20000
LAP_TIME=auto           # seconds per finalizer call; auto = 90, or 480 with FREEZE
CHILDREN=2              # re-grown boards of a lap tried in the next one
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
[ "$FREEZE" != auto ] || { [ "$CLUES" = all ] && FREEZE=1 || FREEZE=0; }
[ "$BEAM_COLUMNS" != auto ] || { [ "$CLUES" = all ] && BEAM_COLUMNS=16 || BEAM_COLUMNS=4; }
FREEZE_MODE=$FREEZE     # FREEZE is 0 or 1 below; FROZEN_CENTRE says whether the
case "$FREEZE" in       # centre block is held too
    0|1)    FROZEN_CENTRE=$FREEZE ;;
    corners) FREEZE=1; FROZEN_CENTRE=0 ;;
    *) die "FREEZE must be 0, 1, corners or auto (got: $FREEZE)" ;;
esac
# auto: the measured settings of each mode. Frozen, a lock at row 2 took 33 of
# 56 lap-1 bands to row 9 and none to row 10, a lock at row 3 none past row 8;
# a lap then needs 1 to 8 bands of up to a minute each.
auto() { [ "${!1}" != auto ] || { [ "$FREEZE" = 1 ] && declare -g "$1=$3" || declare -g "$1=$2"; }; }
auto LAP1_CUT 3:4 2:4;      auto LATER_CUTS "4:3 3:4" "2:4 2:3"
auto FIN_STOP_ROW 10 9;     auto BANDS_GROWN 2 8
auto LAP_TIME 90 480;       auto BOARD_TIME 180 1800
# Frozen blocks: the cut fills them (--with_clue_regions) and the finalizer
# holds them (--freeze_clues). Only the finalizer's backtrack sees them, so it
# searches exhaustively from its lock (--backtrack_row BR, set per lap), each
# band with an equal share of LAP_TIME: a beam row below the centre block had to
# fit it by chance (on the synthetic solution's own band, from row 6, none did),
# and the search from the lock settles a band for good.
# The cut's order: rowmajor was measured for the plain cut; with the blocks, most
# cuts have no filling, and mrv proves that in a second where rowmajor ran out
# its BAND_TIME (the same 4 of 20 boards cut either way).
REGION_ARG=(); FREEZE_ARG=(); CUT_ORDER=rowmajor
if [ "$FREEZE" = 1 ]; then
    REGION_ARG=(--with_clue_regions); FREEZE_ARG=(--freeze_clues)
    CUT_ORDER=mrv
fi
# Checked here, because a bad value would otherwise fail every lap quietly.
for v in BEAM_STOP_ROW BEAM_BACKTRACK BEAM_MIN_COL BEAM_COLUMNS FIN_STOP_ROW BANDS_GROWN LAP_TIME BOARD_TIME; do
    [[ ${!v} =~ ^[0-9]+$ ]] || die "$v must be a whole number (got: ${!v})"
done
[[ $BEAM_TAU =~ ^[0-9]+([.][0-9]+)?$ ]] || die "BEAM_TAU must be a number, 0 or more (got: $BEAM_TAU)"
[ "$BEAM_BACKTRACK" -lt "$BEAM_STOP_ROW" ] && [ "$BEAM_STOP_ROW" -le 14 ] ||
    die "need BEAM_BACKTRACK < BEAM_STOP_ROW <= 14"
[ "$FIN_STOP_ROW" -le 14 ] || die "FIN_STOP_ROW is at most 14"
[ "$BEAM_MIN_COL" -le 14 ] || die "BEAM_MIN_COL is at most 14"
for cut in $LAP1_CUT $LATER_CUTS; do
    [[ $cut =~ ^([1-9][0-9]*):([1-9][0-9]*)$ ]] || die "a cut is BR:KC (got: $cut)"
    br=${BASH_REMATCH[1]}
    [ "$br" -lt "$FIN_STOP_ROW" ] || die "cut $cut: BR must be below FIN_STOP_ROW ($FIN_STOP_ROW)"
    [ "$FREEZE" = 0 ] || [ "$br" -le 6 ] || die "cut $cut: with FREEZE, BR is at most 6 (--freeze_clues backtracks from row 6 or lower)"
done
BIN=$REPO/bin; TOOLS=$REPO/tools
case "$SEED" in /*) ;; *) SEED=$REPO/$SEED ;; esac
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
# 999 = unplaced) and rot[256]; cell = row*16 + col, row 0 at the bottom.

# count_boards FILE / nth_board FILE N -- data lines only (# and % are comments)
count_boards() { awk '!/^[#%]/ && NF {n++} END {print n + 0}' "$1" 2> /dev/null || echo 0; }
nth_board()    { awk -v n="$2" '!/^[#%]/ && NF && ++k == n {print; exit}' "$1"; }

# keep_block IN OUT BR KC -- clear the interior cells of rows 1..BR right of
# column KC; the block in columns 1..KC stays as the band cut's anchor. Under
# FREEZE the 2x2 corner clue blocks are kept, rows 1-2 included (the turn
# brought the parent's top-right block to columns 13-14 there), but only where
# the five frame cells of their 3x3 corner are placed too: on s1, the column
# extension fills the top-left block, and where its frame cap stopped short no
# edge piece may fit beside it (160 of 170 boards in one batch); the cut then
# fills that block again, around the frame.
keep_block() {
    awk -F, -v br="$3" -v kc="$4" -v spare="$FREEZE" 'BEGIN {OFS = ","}
        function framed(r, c,   R, C, i, j) {
            R = r <= 2 ? 0 : 15; C = c <= 2 ? 0 : 15
            i = R ? -1 : 1; j = C ? -1 : 1
            return ((R * 16 + C) in at) && ((R * 16 + C + j) in at) && ((R * 16 + C + 2 * j) in at) &&
                   (((R + i) * 16 + C) in at) && (((R + 2 * i) * 16 + C) in at)
        }
        !/^[#%]/ && NF {
            n = NF - 512; split("", at)
            for (p = 1; p <= 256; p++) if ($(n + p) != 999) at[$(n + p) + 0] = 1
            for (p = 1; p <= 256; p++) {
                c = $(n + p); if (c == 999) continue
                r = int(c / 16); col = c % 16
                if (spare && (r == 1 || r == 2 || r == 13 || r == 14) &&
                    (col == 1 || col == 2 || col == 13 || col == 14)) {
                    if (!framed(r, col)) $(n + p) = 999
                    continue
                }
                if (r >= 1 && r <= br && col > kc && col <= 14) $(n + p) = 999
            }
            print
        }' "$1" > "$2"
}

# The frame as a ring: its 60 cells counter-clockwise from cell 0 (bottom row
# left to right, right column up, top row right to left, left column down).
# A clockwise quarter turn moves ring position i to i-15, so one frame turned
# any way is the same ring shifted by a multiple of 15. RING prints, per board,
# the piece at each ring position ("-" where empty).
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
# The pieces on the frozen 2x2 clue blocks (rows and columns 1-2 and 13-14,
# and, if the awk variable centre is set, rows/columns 7-8), in piece order, or
# "" unless all their cells are placed. A turn moves the blocks onto each
# other, so a held set reads the same on every board of a chain.
function region_set(line,   x, n, p, c, r, k, s, m) {
    n = split(line, x, ",") - 512; s = ""; m = 0
    for (p = 0; p < 256; p++) {
        c = x[n + 1 + p]; if (c == 999) continue
        r = int(c / 16); k = c % 16
        if (((r == 1 || r == 2 || r == 13 || r == 14) && (k == 1 || k == 2 || k == 13 || k == 14)) ||
            (centre && (r == 7 || r == 8) && (k == 7 || k == 8))) s = s (m++ ? "." : "") p
    }
    return m == (centre ? 20 : 16) ? s : ""
}
BEGIN { ring_cells() }'

# one_per_frame IN OUT -- keep the first board of each distinct frame, if its
# top-left corner can close: with BEAM_MIN_COL the column extension lays
# column 0 up to row 14, and where no unused corner fits on top of it (the
# extension's top cap stopped at once) no cut can ever close the frame there
# -- 110 of 170 boards in one batch
one_per_frame() {
    awk "$RING"' !/^[#%]/ && NF {
        n = split($0, x, ",") - 512; c14 = c15 = 0
        for (p = 1; p <= 256; p++) { c14 += x[n + p] + 0 == 224; c15 += x[n + p] + 0 == 240 }
        if (c14 && !c15) next
        r = ring_of($0); if (!(r in seen)) { seen[r] = 1; print } }' "$1" > "$2"
}

# jewel_key CHAIN -- the frame key if the four boards in CHAIN (s1..s4) make a
# jewel, else nothing. s2..s4 must each hold all 60 frame cells, the same frame
# turned with the board; s1 must agree with it wherever s1 has a frame piece.
# Under FREEZE, s2..s4 must also hold the same frozen clue-block pieces, and
# the key is the frame's and theirs. Not re-tested here, because each lap
# guarantees it: every board is exactly matched (no breaks), rows
# 0..FIN_STOP_ROW are complete, the turns are clockwise, and each lap keeps the
# parent's block of at least 12 interior cells.
jewel_key() {
    awk -v freeze="$FREEZE" -v centre="$FROZEN_CENTRE" "$RING"' !/^[#%]/ && NF { R[++k] = ring_of($0); Q[k] = region_set($0) }
        END {
            if (k != 4) exit
            for (j = 2; j <= 4; j++) if (R[j] ~ /-/) exit           # frame complete
            if (shift(R[2], 15) != R[3] || shift(R[2], 30) != R[4]) exit
            split(shift(R[1], 15), a, " "); split(R[2], b, " ")     # s1 turned once
            for (i = 1; i <= 60; i++) if (a[i] != "-" && a[i] != b[i]) exit
            if (!freeze) { print key_of(R[2]); exit }
            if (Q[2] == "" || Q[3] != Q[2] || Q[4] != Q[2]) exit    # blocks held
            print key_of(R[2]) "/" Q[2]
        }' "$1"
}

# held_blocks IN OUT -- the boards of IN that hold all the frozen clue blocks
held_blocks() {
    awk -v centre="$FROZEN_CENTRE" "$RING"' !/^[#%]/ && NF && region_set($0) != ""' "$1" > "$2"
}

# free_centre FILE -- FREEZE=corners: clear the centre 2x2 block of the bands,
# but for the centre clue (piece 138), so the finalizer re-grows the block
# instead of freezing it
free_centre() {
    awk -F, -v keep="$([ "$CLUES" = none ] && echo -1 || echo 138)" 'BEGIN {OFS = ","}
        !/^[#%]/ && NF {
            n = NF - 512
            for (p = 1; p <= 256; p++) {
                c = $(n + p); if (c == 999 || p - 1 == keep) continue
                r = int(c / 16); k = c % 16
                if ((r == 7 || r == 8) && (k == 7 || k == 8)) $(n + p) = 999
            }
        }
        { print }' "$1" > "$1.tmp" && mv "$1.tmp" "$1"
}

# ---- produce: beamer boards, one per frame, shuffled ------------------------------
# Consecutive beamer boards differ in a few interior cells and share their ring,
# so lap 1 would find the same frame again; one board per frame avoids that.
produce() {    # produce N -> queue.csv
    local d=produce$1 db=(); rm -rf "$d"; mkdir "$d"
    [ -z "$DB_FILE" ] || db=(--db_file "$DB_FILE")
    timeout -k 10 $((BEAM_TIME + 60)) "$BIN/E555_beamer" "$SEED" --random_edges --samples 0 \
        --top_columns "$BEAM_COLUMNS" --tau_columns "$BEAM_TAU" "${CLUE_ARG[@]}" \
        --beam_width "$BEAM_WIDTH" --backtrack_row "$BEAM_BACKTRACK" --stop_row "$BEAM_STOP_ROW" \
        --backtrack_min_col "$BEAM_MIN_COL" \
        --max_emitted "$BEAM_BOARDS" --wall_time "$BEAM_TIME" "${db[@]}" \
        --threads "$THREADS" --rng_seed "$((RNG_SEED + $1))" --out_dir "$d" --print_cmd \
        > "$d.log" 2>&1 || log "[produce] the beamer exited non-zero (see $d.log)"
    xargs -r cat < "$d/outputs.txt" > "$d/all.csv" 2> /dev/null || : > "$d/all.csv"
    one_per_frame "$d/all.csv" "$d/one.csv"
    PRODUCED=$(count_boards "$d/all.csv")
    shuf --random-source=<(yes "$RNG_SEED.$1") "$d/one.csv" > queue.csv
    rm -rf "$d"
}

# ---- one lap ----------------------------------------------------------------------
left() { echo $(( BOARD_END - SECONDS )); }
cap()  { local a=$1 b; b=$(left); echo $(( a < b ? a : b )); }   # min(a, time left)

# run_lap K BOARD BR KC DIR -- turn BOARD, cut rows 0..BR, re-grow to row
# FIN_STOP_ROW. Leaves the re-grown boards in DIR/next.csv; fails when there
# are none, with the reason in WHY. The cut and the finalizer get the clue flags
# (the clues keep their own cells) and, under FREEZE, fill and hold the 2x2
# clue blocks.
run_lap() {
    local k=$1 board=$2 br=$3 kc=$4 d=$5 t band
    rm -rf "$d"; mkdir -p "$d"      # a retried lap must not see the last try's
                                    # files: the finalizer appends to its output
    python3 "$TOOLS/E555_rotate.py" "$board" 1 --out "$d/turned.csv" --seed_file "$SEED" > /dev/null
    keep_block "$d/turned.csv" "$d/cut.csv" "$br" "$kc"

    t=$(cap "$BAND_TIME"); [ "$t" -gt 2 ] || { WHY="out of time"; return 1; }
    timeout -k 5 "$(left)" "$BIN/E555_backtracker" "$SEED" "$d/cut.csv" "$d/bt.csv" \
        --stop_row "$br" --with_frame "${REGION_ARG[@]}" "${CLUE_ARG[@]}" \
        --order "$CUT_ORDER" --break_mode any --breaks 0 \
        --max_emitted "$BANDS_ENUMERATED" --time_limit "$t" --threads "$THREADS" \
        --print_cmd > "$d/bt.log" 2>&1 || true
    band=$(grep -s "stop_row$br\.csv$" "$d/bt.csv.outputs.txt" | head -n 1 || true)
    [ -n "$band" ] && [ "$(count_boards "$band")" -gt 0 ] || { WHY="lap $k cut $br:$kc found no band"; return 1; }
    python3 "$TOOLS/E555_rank.py" "$band" --seed_file "$SEED" --diverse "$BANDS_GROWN" \
        --out "$d/bands.csv" > /dev/null 2>&1 || cp "$band" "$d/bands.csv"
    [ "$FREEZE" = 0 ] || [ "$FROZEN_CENTRE" = 1 ] || free_centre "$d/bands.csv"

    t=$(cap "$LAP_TIME"); [ "$t" -gt 5 ] || { WHY="out of time"; return 1; }
    local lock=()
    [ "$FREEZE" = 0 ] || lock=(--backtrack_row "$br" --time_limit "$(( t / BANDS_GROWN > 1 ? t / BANDS_GROWN : 1 ))")
    timeout -k 5 "$(left)" "$BIN/E555_finalizer" "$SEED" "$d/bands.csv" --out_dir "$d/fin" \
        --finalize_from "$br" --stop_row "$FIN_STOP_ROW" --keep_ring --no_fanout_sort \
        "${CLUE_ARG[@]}" "${FREEZE_ARG[@]}" "${lock[@]}" \
        --beam_width "$FIN_WIDTH" --lambda_Mahalanobis 0 \
        --frac_rand 0.0 --max_emitted 1 --wall_time "$t" --rng_seed "$((RNG_SEED + NB))" \
        --threads "$THREADS" --print_cmd > "$d/fin.log" 2>&1 || true
    xargs -r cat < "$d/fin/outputs.txt" > "$d/next.csv" 2> /dev/null || : > "$d/next.csv"
    [ "$(count_boards "$d/next.csv")" -gt 0 ] || { WHY="lap $k cut $br:$kc re-grew nothing"; return 1; }
    # The finalizer re-grows a band whose blocks did not all freeze without
    # them (its [freeze] lines in fin.log say why); such boards are dropped.
    if [ "$FREEZE" = 1 ]; then
        held_blocks "$d/next.csv" "$d/held.csv"; mv "$d/held.csv" "$d/next.csv"
        [ "$(count_boards "$d/next.csv")" -gt 0 ] ||
            { WHY="lap $k cut $br:$kc: the clue blocks did not hold (see [freeze] in fin.log)"; return 1; }
    fi
}

# ---- one board: depth-first through three laps --------------------------------------
# CHAIN[k] is the file holding s_k. whirl K tries to extend s_K to s4. It tries
# the cuts of lap K in order until one re-grows; then that cut's first CHILDREN
# boards, each taken straight into lap K+1. If none of them reaches s4, lap K
# gives up and the lap before tries its next child. Succeeds once CHAIN[1..4]
# is filled.
declare -a CHAIN
DEEPEST=1
whirl() {
    local k=$1 cuts cut i n d child
    if [ "$k" = 1 ]; then cuts=$LAP1_CUT; else cuts=$LATER_CUTS; fi
    for cut in $cuts; do
        d=$BOARD_DIR/lap$k.${cut/:/x}
        run_lap "$k" "${CHAIN[$k]}" "${cut%%:*}" "${cut##*:}" "$d" || continue
        n=$(count_boards "$d/next.csv")
        for ((i = 1; i <= n && i <= CHILDREN; i++)); do
            child=$d/s$((k + 1))_$i.csv
            nth_board "$d/next.csv" "$i" > "$child"
            CHAIN[$((k + 1))]=$child
            [ "$((k + 1))" -le "$DEEPEST" ] || DEEPEST=$((k + 1))
            [ "$k" = 3 ] && return 0
            [ "$(left)" -gt 10 ] || { WHY="out of time"; return 1; }
            whirl $((k + 1)) && return 0
        done
        return 1                    # this cut re-grew, but no child got through
    done
    return 1
}

# whirl_board N FILE -- whirl one board; on a jewel with a new frame, keep it
whirl_board() {
    local nb=$1 t0=$SECONDS key j
    BOARD_DIR=$RUN_DIR/boards/$nb; rm -rf "$RUN_DIR/boards"; mkdir -p "$BOARD_DIR"
    BOARD_END=$((SECONDS + BOARD_TIME)); DEEPEST=1; WHY=
    cp "$2" "$BOARD_DIR/s1.csv"; CHAIN=([1]="$BOARD_DIR/s1.csv")
    if ! whirl 1; then
        log "[board $nb] reached s$DEEPEST; last: $WHY ($((SECONDS - t0)) s)"; return
    fi
    cat "${CHAIN[@]}" > "$BOARD_DIR/chain.csv"
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
            "${CHAIN[$j]}" >> jewels.csv
    done
    echo "board=$nb jewel=$NJ frame=$key seconds=$((SECONDS - T0))" >> jewels.log
    log "[board $nb] JEWEL $NJ, new frame ($((SECONDS - t0)) s)"
}

# ---- main loop -----------------------------------------------------------------------
log "[cfg] run_dir=$RUN_DIR threads=$THREADS wall=$WALL board_time=$BOARD_TIME rng_seed=$RNG_SEED db_file=${DB_FILE:-<rebuilt each call>}"
log "[cfg] clues=$CLUES freeze=$FREEZE_MODE beam: stop_row=$BEAM_STOP_ROW columns=$BEAM_COLUMNS tau=$BEAM_TAU min_col=$BEAM_MIN_COL; cuts: $LAP1_CUT, then $LATER_CUTS, $BANDS_GROWN bands; finalizer: stop_row=$FIN_STOP_ROW lap_time=$LAP_TIME"
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
    if [ -n "$INPUT" ]; then
        [ "$NP" = 0 ] || break                  # INPUT: one pass over its boards
        cp "$INPUT" queue.csv
    else
        produce "$NP"
    fi
    NP=$((NP + 1)); nq=$(count_boards queue.csv)
    if [ -n "$INPUT" ]; then log "[input] $nq boards ($((SECONDS - T0)) s)"
    else log "[produce $NP] $nq of the $PRODUCED boards written: one per frame, top-left corner closable ($((SECONDS - T0)) s)"; fi
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
rm -rf boards board.csv queue.csv
log "[done] $NB boards, $NJ jewels in $((SECONDS - T0)) s -> $RUN_DIR/jewels.csv"

if [ "$DIVE" = 1 ] && [ "$NJ" -gt 0 ]; then
    grep -E '^b[0-9]+_s[234],' jewels.csv > to_dive.csv
    "$BIN/E555_diver" "$SEED" to_dive.csv dived.csv --end_dive 10000 --end_polish 20000 \
        --emit_score 0 --threads "$THREADS" --wall_time 900 --print_cmd > dive.log 2>&1 || true
    python3 "$TOOLS/E555_rank.py" dived.csv --seed_file "$SEED" --top 20 --out best.csv --rescore > /dev/null || true
    log "[dive] $(count_boards dived.csv) boards dived -> $RUN_DIR/best.csv"
fi
