#!/bin/bash
# run_whirlpool_jewels.sh -- re-grow a beamer board from its other three sides
# under one fixed frame, and keep the boards that make it: the jewels.
#
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db                  # an hour: all five clues, blocks frozen, K=4
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db K=3              # a narrower band: more jewels
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db K=3 FREEZE=corners  # centre block re-grown
#   bash pipeline/run_whirlpool_jewels.sh DB_FILE=E555.db K=3 DIVE=1       # each jewel dived as it is found
#   bash pipeline/run_whirlpool_jewels.sh INPUT=boards.csv RUN_DIR=w1      # whirl your boards
#
# The beamer grows a board upward from row 0, so its bottom rows were chosen
# first, by a beam that was still guessing, and are never searched again. The
# whirlpool turns the board and re-grows it, so another side becomes the
# bottom. A JEWEL is four boards around one beamer board s1, all holding the
# same complete 60-cell frame (and, FREEZE, the same clue blocks):
#
#   s1   a beamer board, rows 0..BEAM_STOP_ROW, with the annealed left column
#   s2   lap 1: s1's left band (columns 0..K, its top-left corner closed),
#        turned counter-clockwise to rows 0..K, re-grown to row FIN_STOP_ROW
#   s3   lap 2: s1's right band (columns 15-K..15, its top-right corner
#        closed), turned clockwise, re-grown
#   s4   lap 3: s3's right band, turned clockwise -- or, when no s4 grows that
#        way, s2's left band, turned counter-clockwise; either way s1's top
#        side is s4's bottom
#
# So each of s1's four sides is a bottom once, and s2 and s3 both lock a band
# of s1 itself. The two side bands are cut separately and share only the frame
# and the blocks; each may borrow a piece the other uses, or one from s1's
# middle columns (the s1 written to jewels.csv leaves such a cell open).
#
# A band:
#   close     E555_backtracker --stop_column K --with_frame (the right band:
#             --reverse): every exact filling of those columns plus the frame,
#             closing a top corner block above BEAM_STOP_ROW (laps 1 and 2) or
#             above FIN_STOP_ROW (lap 3). The other inner columns are cleared
#             and lend their pieces. A board whose corner cannot close writes
#             no band, so a lap selects the boards that can.
#   turn      tools/E555_rotate.py: counter-clockwise for a left band,
#             clockwise for a right one; the band becomes rows 0..K
#   re-grow   E555_finalizer --finalize_from K --keep_ring up to FIN_STOP_ROW,
#             exhaustively from the lock (--backtrack_row K), the frame held
#
# A lap picks BANDS_GROWN x LAP_TRIES of its bands, most different first
# (E555_rank.py --diverse), and re-grows them BANDS_GROWN at a time; the search
# is depth-first, and the first s4 ends the board. A board's id names its
# parent: a band is <the board it was cut from>.<n>, a re-grown board <its
# band>.<n> (finalizer --keep_ids), and a lap-1 band paired with a lap-2 band
# <lap-2 band>_<n>.
#
# BORDERS: each produce call starts from a fresh border. anneal (the default)
# runs Stage A with --double_decker DD_SIDES and takes the best by Score_dd
# whose two top corner blocks exist in some clue orientation (a 1 s beamer
# probe, CLUES=all); the beamer spends the TOP reserve last (--lambda_reserve).
# A path takes its next row from that rotations file each call; random uses the
# beamer's own --random_edges. Every border used is appended to borders.csv.
#
# CLUES: every tool holds all five clues (CLUES=all, the default), the centre
# one (center) or none, each on its own cell through every lap; INPUT boards
# must carry the clues they are run with. FREEZE=1 (the default): the bands
# also fill the five 2x2 clue blocks -- the centre and the four inner corners
# -- and every lap holds them like the frame (finalizer --freeze_clues). A
# frozen lap stops lower the higher K is (FIN_STOP_ROW=auto: 11-K), and K=3
# finds more jewels than K=4. FREEZE=corners holds only the four corner blocks.
#
# DIVE=1 finishes each jewel's four boards with E555_diver as soon as it is
# found: DIVE_DIVES dives and DIVE_POLISH polish rounds (DIVE_COPIES copies),
# then DIVE_REOPEN seconds of re-dives of each board's most damaged band.
#
# NEEDS about 8 GB of RAM for the beamer, and DB_FILE: the beamer's 6.4 GB chain
# database is written there on the first call and loaded in seconds by every
# later one. A clued database leaves the clue pieces out, so the clue setting is
# appended to the name (E555.db.center, E555.db.all). Built with `make`.
#
# WRITES into RUN_DIR:
#   jewels.csv    the jewels' four boards, ids b<board>_s1 .. b<board>_s4
#   jewels.log    one line per jewel: board, s4's parent (from=s3|s2), the
#                 interior cells shared (shared=s1s2/s1s3/<from>s4/s1s4), the
#                 dived scores and their max (DIVE=1), time
#   borders.csv   every border a produce call used (anneal or file mode)
#   run.log       one line per board; boards/<n>/ holds a board's tool logs
#                 until the next board starts
#   dived.csv     DIVE=1: every jewel's four boards dived, and the 20 best of
#   best.csv      them
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
DIVE=0                  # 1 = dive each jewel's four boards as soon as it is found
DIVE_DIVES=100000       # E555_diver per board: dives (--end_dive),
DIVE_POLISH=200000      # polish rounds (--end_polish)
DIVE_COPIES=1           # and copies of both, the best kept (--copies);
DIVE_REOPEN=30          # then seconds per board of re-dives of its most damaged band
                        # (--reopen auto+2, never worse; 0 = none)
CLUES=all               # none | center | all: the Eternity II clues every tool holds
FREEZE=1                # 1 = the band also fills the five 2x2 clue blocks and every
                        # lap holds them, like the frame; corners = the four corner
                        # blocks only, the centre block re-grown every lap; 0 = off
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
BEAM_STOP_ROW=8         # s1 is complete up to this row
BEAM_BACKTRACK=6        # exhaustive from this row (--backtrack_row)
BEAM_MIN_COL=0          # N > 0: the beamer writes only boards whose columns 1..N grow
                        # whole to row 14 (--backtrack_min_col, 0..K): a pre-filter
                        # for BORDERS=random (a rotations border fixes the left column)
BEAM_BOTTOMS=0          # bottom orderings tried per border, sampled with BEAM_TAU;
                        # 0 = all, until BEAM_TIME or BEAM_BOARDS (not with random)
BEAM_COLUMNS=16         # left columns tried per bottom
BEAM_TAU=0.1            # randomness in picking bottoms and columns; 0 = the best-ranked
BEAM_CORNERS=0.5        # CLUES=all: --lambda_corners; a 1 s probe first reads the
                        # beamer's exact catalog of clued top-corner blocks per clue
                        # orientation, and the beam runs only in the orientations
                        # whose top-left and top-right catalogs are both non-empty
                        # (--pin_clue), one call each; the annealed border used is
                        # the best by Score_dd that has one. 0 = off: one unpinned call
BEAM_BOARDS=2000        # boards per produce call, shared by its beamer calls
BEAM_TIME=570           # seconds per produce call, shared by its beamer calls

# the band: E555_backtracker --stop_column K before each turn
BANDS=60                # fillings kept per board
BAND_TIME=15            # seconds per board

# the re-grow: E555_finalizer --finalize_from K
BANDS_GROWN=8           # bands per finalizer call
LAP_TRIES=3             # finalizer calls a lap may make before it gives up
FIN_STOP_ROW=auto       # s2..s4 are re-grown up to this row; auto = 11-K with FREEZE=1
                        # (K=2: 9, K=3: 8, K=4: 7), 9 with FREEZE=corners, 10 unfrozen
LAP_TIME=480            # seconds per finalizer call, shared by its bands (unfrozen: 240)
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
# holds them (--freeze_clues), which its exhaustive search from the lock sees.
REGION_ARG=(); FREEZE_ARG=()
FROZEN_CENTRE=0         # FREEZE=1 also holds the centre block; corners re-grows it
case "$FREEZE" in
    0) ;;
    1) REGION_ARG=(--with_clue_regions); FREEZE_ARG=(--freeze_clues); FROZEN_CENTRE=1 ;;
    corners) REGION_ARG=(--with_clue_regions); FREEZE_ARG=(--freeze_clues) ;;
    *) die "FREEZE must be 0, 1 or corners (got: $FREEZE)" ;;
esac
# Checked here, because a bad value would otherwise fail every lap quietly.
# FIN_STOP_ROW=auto: with all five blocks frozen, a lap re-grows only as far as
# the rows the frozen centre block leaves it, 11-K.
if [ "$FIN_STOP_ROW" = auto ]; then
    case "$FREEZE" in
        1) FIN_STOP_ROW=$(( 11 - K < 9 ? 11 - K : 9 )) ;;
        corners) FIN_STOP_ROW=9 ;;
        *) FIN_STOP_ROW=10 ;;
    esac
fi
for v in K BOARD_TIME DIVE_DIVES DIVE_POLISH DIVE_COPIES DIVE_REOPEN ANNEAL_RESTARTS ANNEAL_STEPS DD_RESERVE BEAM_MIN_COL BEAM_STOP_ROW BEAM_BACKTRACK \
         BEAM_BOTTOMS BEAM_COLUMNS BEAM_BOARDS BEAM_TIME BANDS BAND_TIME BANDS_GROWN \
         LAP_TRIES FIN_STOP_ROW LAP_TIME FIN_BOARDS; do
    [[ ${!v} =~ ^[0-9]+$ ]] || die "$v must be a whole number (got: ${!v})"
done
for v in BEAM_TAU RESERVE_F BEAM_CORNERS; do
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
for t in beamer backtracker finalizer diver; do
    [ -x "$BIN/E555_$t" ] || { echo "no $BIN/E555_$t: run make in $REPO" >&2; exit 1; }
done
mkdir -p "$RUN_DIR"; RUN_DIR=$(cd "$RUN_DIR" && pwd); cd "$RUN_DIR"
: > jewels.csv; : > jewels.log; : > run.log; rm -f dived.csv best.csv
log() { echo "$*" | tee -a run.log; }

# ---- helpers ------------------------------------------------------------------
# Boards are CSV rows whose last 512 fields are pos[256] (the cell of each piece,
# 999 = unplaced) and rot[256]; cell = row*16 + col, row 0 at the bottom. Field
# 1 is the board's id, which names its parent (see the header): that is how a
# jewel is traced back from s4 to s1.

# count_boards FILE / nth_board FILE N -- data lines only (# and % are comments)
count_boards() { awk '!/^[#%]/ && NF {n++} END {print n + 0}' "$1" 2> /dev/null || echo 0; }
nth_board()    { awk -v n="$2" '!/^[#%]/ && NF && ++k == n {print; exit}' "$1"; }
# board_by_id FILE ID -- the data line whose id is ID
board_by_id()  { awk -F, -v id="$2" '!/^[#%]/ && NF && $1 == id {print; exit}' "$1"; }
# tag_ids IN OUT PREFIX -- give every board the id PREFIX<n>
tag_ids() {
    awk -F, -v pre="$3" 'BEGIN {OFS = ","} !/^[#%]/ && NF {$1 = pre (++n); print}' "$1" > "$2"
}

# rank_to IN OUT ARGS... -- E555_rank.py IN ARGS --out OUT, quietly
rank_to() {
    local in=$1 out=$2; shift 2
    python3 "$TOOLS/E555_rank.py" "$in" --seed_file "$SEED" --quiet --out "$out" "$@" 2> /dev/null
}
# select_ids IDS FILE -- the data lines of FILE whose id is listed in IDS
select_ids() { awk -F, 'FILENAME == ARGV[1] { k[$1]; next } !/^[#%]/ && NF && ($1 in k)' "$1" "$2"; }
# first_child FILE PARENT [paired] -- the id of FILE's first board grown from
# PARENT: its id less the last .<n> (paired: less a _<n> too)
first_child() {
    awk -F, -v p="$2" -v pair="${3:+1}" '!/^[#%]/ && NF { q = $1; sub(/\.[0-9]+$/, "", q)
        if (pair) sub(/_[0-9]+$/, "", q)
        if (q == p) { print $1; exit } }' "$1"
}

# mask FILE EXPR -- a 16x16 0/1 region (the --holes format, first line row 0):
# 1 where the bash arithmetic EXPR in r (row) and c (column) holds
mask() {
    local r c line
    for ((r = 0; r < 16; r++)); do
        line=
        for ((c = 0; c < 16; c++)); do line+="${line:+,}$(( $2 ))"; done
        echo "$line"
    done > "$1"
}

# open_corner IN OUT -- clear the two top corners the side bands close: rows
# above BEAM_STOP_ROW x columns 1..K and 15-K..14 (masks/open.csv). A rotations
# border's left column stays as annealed; under BORDERS=random the column
# above the stop row is the extension's own choice, so it is cleared too. A
# band puts a cleared clue back on its own cell.
open_corner() {
    : > "$2"
    [ "$(count_boards "$1")" -gt 0 ] || return 0
    "$BIN/E555_diver" "$SEED" "$1" "$2" --holes "$MASKS/open.csv" --end_dive 0 --emit_score 0 \
        --threads "$THREADS" > "${2%.csv}.log" 2>&1 || : > "$2"
}

# one_per_frame IN OUT -- the best board of each distinct frame
one_per_frame() {
    : > "$2"
    [ "$(count_boards "$1")" -gt 0 ] || return 0
    rank_to "$1" "$2" --group_box "$MASKS/frame.csv" --per_group 1 || [ -f "$2" ] || : > "$2"
}

# overlay BANDS BOARDS OUT [SEED] -- every line of BANDS, keeping its own
# cells and id, with the board it was cut from (the BOARDS line whose id is the
# band's id up to its last dot; a single board serves every band) filling the
# cells it leaves empty, by pieces it does not place. A band with no such
# board is copied as it is. With SEED, a
# filled cell whose edge does not match a band cell next to it stays empty: a
# band may refill a cell beside columns it had cleared.
overlay() {
    awk -F, -v useseed="${4:+1}" 'BEGIN {OFS = ","}
        useseed && FILENAME == ARGV[1] { if (NF) { split($0, e, /[ \t]+/); m = (e[1] == "") ? 1 : 0
            for (d = 0; d < 4; d++) E[np, d] = e[d + 1 + m] + 0; np++ }; next }
        FILENAME == ARGV[1 + useseed] {
            if (!/^[#%]/ && NF) {
                n = NF - 512; id = $1; sub(/^ +/, "", id); sub(/ +$/, "", id); B[id] = 1; nb++; only = id
                for (p = 1; p <= 256; p++) { BP[id, p] = $(n + p) + 0; BR[id, p] = $(n + 256 + p) + 0 }
            }
            next }
        !/^[#%]/ && NF {
            src = $1; sub(/\.[0-9]+ *$/, "", src)
            if (!(src in B)) { if (nb == 1) src = only; else { print; next } }
            n = NF - 512; split("", at); split("", sp); split("", fill)
            for (p = 1; p <= 256; p++) { c = $(n + p) + 0; if (c != 999) { at[c] = p - 1; sp[c] = $(n + 256 + p) + 0 } }
            for (p = 1; p <= 256; p++) {
                c = BP[src, p]
                if ($(n + p) + 0 != 999 || c == 999 || (c in at)) continue
                orot[c] = $(n + 256 + p)
                $(n + p) = c; $(n + 256 + p) = BR[src, p]; at[c] = p - 1; sp[c] = BR[src, p]; fill[c] = 1
            }
            if (useseed) for (c in fill) {
                r = int(c / 16); k = c % 16; bad = 0
                if (k < 15 && ((c + 1) in at) && !((c + 1) in fill) && side(c, 1) != side(c + 1, 3)) bad = 1
                if (k > 0  && ((c - 1) in at) && !((c - 1) in fill) && side(c, 3) != side(c - 1, 1)) bad = 1
                if (r < 15 && ((c + 16) in at) && !((c + 16) in fill) && side(c, 0) != side(c + 16, 2)) bad = 1
                if (r > 0  && ((c - 16) in at) && !((c - 16) in fill) && side(c, 2) != side(c - 16, 0)) bad = 1
                if (bad) { $(n + at[c] + 1) = 999; $(n + 256 + at[c] + 1) = orot[c] }
            }
            print }
        function side(c, d) { return E[at[c], (d + sp[c]) % 4] }' ${4:+"$4"} "$2" "$1" > "$3"
}

# is_jewel -- whether BOARD_DIR/s1.csv .. s4.csv make a jewel: turned back to
# s1's orientation (s2 clockwise, s3 counter-clockwise, s4 twice), all four
# place every cell of masks/jewel.csv (the frame; FREEZE: and the frozen
# blocks) with the same pieces. The rest each lap guarantees: every board is
# exactly matched, and each lap locks its parent's band.
is_jewel() {
    local d=$BOARD_DIR j
    cp "$d/s1.csv" "$d/back.csv"
    for j in 2 3 4; do
        python3 "$TOOLS/E555_rotate.py" "$d/s$j.csv" "$(( j == 2 ? 1 : j == 3 ? 3 : 2 ))" \
            --out "$d/turn.csv" --seed_file "$SEED" > /dev/null || return 1
        grep -v '^[#%]' "$d/turn.csv" >> "$d/back.csv" || return 1
    done
    rank_to "$d/back.csv" "$d/held4.csv" --complete "$MASKS/jewel.csv" || return 1
    [ "$(count_boards "$d/held4.csv")" = 4 ] || return 1
    rank_to "$d/back.csv" "$d/frames.csv" --group_box "$MASKS/jewel.csv" --per_group 1 || return 1
    [ "$(count_boards "$d/frames.csv")" = 1 ]
}

# new_frame S1 -- whether no jewel found before holds S1's frame (FREEZE: and
# blocks) in any turn; jewels.csv holds every jewel in all four turns
new_frame() {
    [ -s jewels.csv ] || return 0
    cat jewels.csv "$1" > "$BOARD_DIR/seen.csv"
    rank_to "$BOARD_DIR/seen.csv" "$BOARD_DIR/frames.csv" --group_box "$MASKS/jewel.csv" \
        --per_group 1 || return 1
    [ "$(count_boards "$BOARD_DIR/frames.csv")" = "$(count_boards "$BOARD_DIR/seen.csv")" ]
}

# held_blocks FILE -- keep the boards of FILE that hold every frozen block cell
held_blocks() {
    rank_to "$1" "$1.held" --complete "$MASKS/held.csv" || :
    [ -f "$1.held" ] || : > "$1.held"
    mv "$1.held" "$1"
}

# free_centre FILE -- FREEZE=corners: clear the centre 2x2 block of the turned
# bands but for the centre clue, so the finalizer re-grows the block instead of
# freezing it, which leaves a high-K lap more room.
free_centre() {
    awk -F, -v keep="$([ "$CLUES" = none ] && echo -1 || echo 138)" 'BEGIN {OFS = ","}
        !/^[#%]/ && NF {
            n = NF - 512
            for (p = 1; p <= 256; p++) {
                c = $(n + p) + 0; if (c == 999 || p - 1 == keep) continue
                r = int(c / 16); k = c % 16
                if ((r == 7 || r == 8) && (k == 7 || k == 8)) $(n + p) = 999
            }
        }
        { print }' "$1" > "$1.tmp" && mv "$1.tmp" "$1"
}

# shared_cells A B TURN -- interior cells where board A, turned TURN times
# clockwise (3 = counter-clockwise), and board B hold the same piece at the
# same spin: E555_rank.py's agreement between the two, "?" if unknown
shared_cells() {
    local d=$BOARD_DIR last=
    python3 "$TOOLS/E555_rotate.py" "$1" "$3" --out "$d/turn.csv" --seed_file "$SEED" > /dev/null &&
        cat "$d/turn.csv" "$2" > "$d/pair.csv" &&
        last=$(python3 "$TOOLS/E555_rank.py" "$d/pair.csv" --seed_file "$SEED" --diverse 2 \
               --diversity_box 1:14,1:14 --csv 2> /dev/null | tail -n 1) || last=
    if [ -n "$last" ]; then echo "${last##*,}"; else echo "?"; fi
}

# ---- the band ---------------------------------------------------------------------
left() { echo $(( BOARD_END - SECONDS )); }
cap()  { local a=$1 b; b=$(left); echo $(( a < b ? a : b )); }   # min(a, time left)

# cut_bands POOL OUT LIMIT SIDE -- E555_backtracker --stop_column K
# --with_frame on every board of POOL: up to BANDS exact fillings of each
# board's columns 0..K (SIDE=left) or 15-K..15 (SIDE=right, --reverse) and the
# frame (FREEZE: and the clue blocks). Each band gets the id <board id>.<n>,
# so the board it was cut from is its id up to the last dot. The other inner
# columns are cleared and lend their pieces. Most corners have no filling,
# and mrv proves that fastest. LIMIT caps the whole call, in seconds.
cut_bands() {
    local pool=$1 out=$2 limit=$3 bt=${2%.csv}.bt.csv f rev=() suf=
    [ "$4" = left ] || { rev=(--reverse); suf=_rev; }
    [ "$limit" -gt 2 ] || { : > "$out"; return; }
    timeout -k 5 "$limit" "$BIN/E555_backtracker" "$SEED" "$pool" "$bt" \
        --stop_column "$K" "${rev[@]}" --with_frame "${REGION_ARG[@]}" "${CLUE_ARG[@]}" \
        --order mrv --break_mode any --breaks 0 \
        --max_emitted "$BANDS" --time_limit "$BAND_TIME" --threads "$THREADS" \
        --print_cmd > "${bt%.csv}.log" 2>&1 || true
    f=$bt.stop_col$K$suf.csv
    : > "$out"
    [ ! -s "$f" ] || awk -F, 'BEGIN {OFS = ","} !/^[#%]/ && NF {$1 = $1 "." (++n[$1]); print}' "$f" > "$out"
    rm -f "$bt" "$bt".*
}

# ---- produce: a border, beamer boards, one per frame, and their lap-1 bands -----
# Consecutive beamer boards differ in a few interior cells and share their ring,
# so lap 1 would find the same frame again; one board per frame avoids that.

# probe_corners FILE ROW DIR -- the beamer's exact catalog of top-left and
# top-right corner blocks for border ROW of FILE, per clue orientation (the
# corner clue, its 2x2 block, the edges beside it), from a 1-second
# --lambda_corners run. Sets PINS, the --pin_clue values of the orientations
# whose top-left and top-right catalogs are both non-empty (laps 1 and 2 close
# both corners of s1), and CORNER_NOTE. With the left column kept, a board in
# another orientation can never close its top-left corner.
probe_corners() {
    local file=$1 row=$2 d=$3 db=()
    [ -z "$DB_FILE" ] || db=(--db_file "$DB_FILE")
    rm -rf "$d/probe"
    "$BIN/E555_beamer" "$SEED" "$file" --start_row "$row" --num_rows 1 "${CLUE_ARG[@]}" \
        --lambda_corners "$BEAM_CORNERS" --top_bottoms 1 --top_columns 1 --beam_width 100 \
        --stop_row "$BEAM_STOP_ROW" --wall_time 1 --max_emitted 1 "${db[@]}" \
        --threads "$THREADS" --out_dir "$d/probe" > "$d/probe.log" 2>&1 || :
    # [corner] border row R, clue orientation O: TL n block(s) on ... | TR m block(s) on ...
    # --pin_clue N names the quadrant of the centre clue: orientation 0 1 2 3 = N 1 4 3 2
    PINS=$(awk '/^\[corner\] border row .* clue orientation/ {
            o = $7 + 0; tl = $9 + 0; tr = 0
            for (i = 10; i < NF; i++) if ($i == "TR") tr = $(i + 1) + 0
            if (tl > 0 && tr > 0) printf "%s ", substr("1432", o + 1, 1) }' "$d/probe.log")
    CORNER_NOTE="TL blocks per orientation $(awk '/^\[corner\] border row .* clue orientation/ {
            printf "%s%s", (n++ ? "/" : ""), $9 + 0 }' "$d/probe.log"), TR $(awk '/^\[corner\] border row .* clue orientation/ {
            for (i = 1; i <= NF; i++) if ($i == "TR") { printf "%s%s", (n++ ? "/" : ""), $(i + 1) + 0 } }' "$d/probe.log")"
    rm -rf "$d/probe"
}

# border N DIR -- sets BORDER_ARGS (the beamer's border arguments), PINS (the
# clue orientations to beam, empty = all of them, unpinned) and BORDER_NOTE
# for call N; BORDER_ARGS=() when no border is usable
border() {
    local n=$1 d=$2 rows row i
    PINS=; CORNER_NOTE=
    local probe=0; [ "$CLUES" = all ] && [ "$BEAM_CORNERS" != 0 ] && [ "$BORDERS" != random ] && probe=1
    case "$BORDERS" in
        random)
            BORDER_ARGS=(--random_edges --samples 0); BORDER_NOTE="random edges"; return ;;
        anneal)
            python3 "$REPO/src/A_border/E555_edge_annealer.py" "$SEED" --out "$d/borders.csv" \
                --restarts "$ANNEAL_RESTARTS" --steps "$ANNEAL_STEPS" \
                --double_decker "$DD_SIDES" --decker_reserve "$DD_RESERVE" \
                --threads "$THREADS" --rng_seed "$((RNG_SEED + n))" > "$d/anneal.log" 2>&1 ||
                { BORDER_ARGS=(); BORDER_NOTE="the annealer failed (see $d/anneal.log)"; return; }
            python3 "$TOOLS/E555_sort_rotations.py" "$d/borders.csv" --sort score_dd \
                -o "$d/sorted.csv" >> "$d/anneal.log" 2>&1 ||
                { BORDER_ARGS=(); BORDER_NOTE="sorting the borders failed (see $d/anneal.log)"; return; }
            # the best by Score_dd with both top corner blocks in some orientation
            rows=$(count_boards "$d/sorted.csv"); row=-1
            for ((i = 0; i < rows; i++)); do
                [ "$probe" = 1 ] || { row=$i; break; }
                probe_corners "$d/sorted.csv" "$i" "$d"
                [ -z "$PINS" ] || { row=$i; break; }
            done
            [ "$row" -ge 0 ] || { BORDER_ARGS=(); BORDER_NOTE="none of the $rows annealed borders has both top corner blocks in one clue orientation"; return; }
            # the row with its own comment line (Decker=, the reserve marks' context)
            awk -v r="$row" '/^#/ {c = $0; next} NF { if (k++ == r) { print c; print; exit } }' \
                "$d/sorted.csv" > "$d/border.csv"
            { echo "# produce $n"; cat "$d/border.csv"; } >> "$RUN_DIR/borders.csv"
            BORDER_ARGS=("$d/border.csv" --start_row 0 --num_rows 1)
            BORDER_NOTE="annealed, rank $((row + 1)) of $rows by Score_dd: $(grep -o -m 1 'Score_dd=[^ ]*' "$d/border.csv" || :) $(grep -o -m 1 'Decker=[^ ]*' "$d/border.csv" || :)" ;;
        *)
            rows=$(count_boards "$BORDERS"); row=$(( (n - 1) % rows ))
            BORDER_ARGS=("$BORDERS" --start_row "$row" --num_rows 1)
            BORDER_NOTE="$(basename "$BORDERS") row $row"
            if [ "$probe" = 1 ]; then
                probe_corners "$BORDERS" "$row" "$d"
                [ -n "$PINS" ] || { BORDER_ARGS=(); BORDER_NOTE="$BORDER_NOTE has both top corner blocks in no clue orientation"; return; }
            fi ;;
    esac
    [ -z "$CORNER_NOTE" ] || BORDER_NOTE="$BORDER_NOTE; $CORNER_NOTE; beamed in orientation pins: $PINS"
    BORDER_ARGS+=(--top_bottoms "$BEAM_BOTTOMS" --tau_bottoms "$BEAM_TAU")
    [ "$RESERVE_F" = 0 ] || BORDER_ARGS+=(--lambda_reserve "$RESERVE_F")
    [ "$probe" = 0 ] || BORDER_ARGS+=(--lambda_corners "$BEAM_CORNERS")
}

produce() {    # produce N -> queue.csv; NOBORDER=1 when no border was usable
    local d=produce$1 db=() pins pin n t b
    rm -rf "$d"; mkdir "$d"
    [ -z "$DB_FILE" ] || db=(--db_file "$DB_FILE")
    border "$1" "$d"
    PRODUCED=0; NOBORDER=0; : > queue.csv; : > "$d/all.csv"
    [ "${#BORDER_ARGS[@]}" -gt 0 ] || { NOBORDER=1; return; }    # keeps $d/anneal.log
    # one beamer call per clue orientation to beam, sharing BEAM_TIME and
    # BEAM_BOARDS; one unpinned call when no orientation was chosen
    pins=${PINS:-0}; n=$(echo $pins | wc -w)
    t=$(( BEAM_TIME / n > 30 ? BEAM_TIME / n : 30 )); b=$(( BEAM_BOARDS / n > 1 ? BEAM_BOARDS / n : 1 ))
    for pin in $pins; do
        local pin_arg=(); [ "$pin" = 0 ] || pin_arg=(--pin_clue "$pin")
        timeout -k 10 $((t + 60)) "$BIN/E555_beamer" "$SEED" "${BORDER_ARGS[@]}" "${pin_arg[@]}" \
            --top_columns "$BEAM_COLUMNS" --tau_columns "$BEAM_TAU" "${CLUE_ARG[@]}" \
            --beam_width "$BEAM_WIDTH" --backtrack_row "$BEAM_BACKTRACK" --stop_row "$BEAM_STOP_ROW" \
            --backtrack_min_col "$BEAM_MIN_COL" \
            --max_emitted "$b" --wall_time "$t" "${db[@]}" \
            --threads "$THREADS" --rng_seed "$((RNG_SEED + $1))" --out_dir "$d/o$pin" --print_cmd \
            > "$d.o$pin.log" 2>&1 || log "[produce] the beamer exited non-zero (see $d.o$pin.log)"
        # a killed beamer leaves no outputs.txt
        [ ! -s "$d/o$pin/outputs.txt" ] || xargs -r cat < "$d/o$pin/outputs.txt" >> "$d/all.csv" 2> /dev/null || :
    done
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
    cut_bands queue.csv bands1.csv "$t" left
    awk -F, '!/^[#%]/ && NF {
        id = $1; sub(/\.[0-9]+$/, "", id)
        if (id != last) { if (f) close(f); last = id; f = "bands1/" id ".csv" }
        print >> f }' bands1.csv
    NBANDED=$(find bands1 -name '*.csv' | wc -l)
    rm -f bands1.csv
}

# ---- one lap ----------------------------------------------------------------------
# grow LABEL BANDS DIR TURN -- turn the bands TURN times clockwise (3 =
# counter-clockwise: the left band goes to the bottom; 1 = clockwise: the right
# band does) and re-grow them to FIN_STOP_ROW: DIR/next.csv, each board named
# <its band>.<n>. Fails when nothing re-grew, with the reason in WHY.
grow() {
    local label=$1 bands=$2 d=$3 turn=$4 t nb
    rm -rf "$d"; mkdir -p "$d"      # the finalizer appends to its output
    python3 "$TOOLS/E555_rotate.py" "$bands" "$turn" --out "$d/turned.csv" --seed_file "$SEED" > /dev/null ||
        { WHY="$label: the bands did not turn (E555_rotate.py)"; return 1; }
    [ "$FREEZE" != corners ] || free_centre "$d/turned.csv"
    nb=$(count_boards "$d/turned.csv")
    [ "$nb" -gt 0 ] || { WHY="$label: no band to re-grow"; return 1; }
    t=$(cap "$LAP_TIME"); [ "$t" -gt 5 ] || { WHY="out of time"; return 1; }
    # An exhaustive search from the lock (--backtrack_row = --finalize_from: the
    # locked band is the single root, no beam row runs), each band with an equal
    # share of LAP_TIME and at most FIN_BOARDS/bands of its boards, so every
    # band runs. Nearly all of the time is the reduced database's rebuild; the
    # beam width only sizes an unused workspace. --no_fanout_sort: for an
    # exhaustive search the sort only reorders the same boards.
    timeout -k 5 "$(left)" "$BIN/E555_finalizer" "$SEED" "$d/turned.csv" --out_dir "$d/fin" \
        --finalize_from "$K" --stop_row "$FIN_STOP_ROW" --keep_ring --no_fanout_sort \
        "${CLUE_ARG[@]}" "${FREEZE_ARG[@]}" \
        --backtrack_row "$K" --time_limit "$(( t / nb > 1 ? t / nb : 1 ))" --beam_width 1000 \
        --keep_ids --max_per_line "$(( (FIN_BOARDS + nb - 1) / nb ))" --wall_time "$t" \
        --rng_seed "$((RNG_SEED + NB))" --threads "$THREADS" --print_cmd > "$d/fin.log" 2>&1 || true
    : > "$d/next.csv"               # a killed finalizer leaves no outputs.txt
    [ ! -s "$d/fin/outputs.txt" ] ||
        xargs -r cat < "$d/fin/outputs.txt" 2> /dev/null | grep -v '^[#%]' > "$d/next.csv" || :
    rm -rf "$d/fin"
    [ "$(count_boards "$d/next.csv")" -gt 0 ] || { WHY="$label re-grew nothing"; return 1; }
    # The finalizer re-grows a band whose blocks did not all freeze without
    # them (its [freeze] lines in fin.log say why); such boards are dropped.
    if [ "$FREEZE" != 0 ]; then
        held_blocks "$d/next.csv"
        [ "$(count_boards "$d/next.csv")" -gt 0 ] ||
            { WHY="$label: the clue blocks did not hold (see [freeze] in fin.log)"; return 1; }
    fi
}

# ---- one board: depth-first through the three laps ----------------------------------
# The laps form a tree around s1, sharing only what a jewel must share, the
# frame (and, FREEZE, the five clue blocks):
#
#   lap-1 bands   s1's top-left corner and the frame (lap1_bands)
#   lap-2 bands   s1's top-right corner (--reverse), cut from s1 with a lap-1
#                 band's frame left of column K+1 and its blocks; they build the
#                 rest of the frame themselves, which lets far more corners
#                 close than under the lap-1 band's whole frame
#   lap 1         a lap-1 band under a lap-2 band's frame, turned
#                 counter-clockwise                                     -> s2
#   lap 2         that lap-2 band, turned clockwise                     -> s3
#   lap 3         s3's right band, turned clockwise                     -> s4
#                 or else s2's left band, turned counter-clockwise      -> s4
#
# A lap-1 band fits every lap-2 band cut from its start: they meet only on the
# frame, whose pieces never go inside, and on the blocks the start holds.
#
# whirl K POOL SIDE NAME re-grows POOL's bands (laps 1 and 2; lap 3 first cuts
# them on SIDE), the most different first, BANDS_GROWN at a time in
# BOARD_DIR/NAME; each group that re-grows feeds the next lap. NEXT[K+1] is the
# group directory the chain runs through. Succeeds once lap 3 re-grows.
declare -a NEXT
DEEPEST=1

# side_bands LAP1 -- BOARD_DIR/l1.csv and l2.csv: the lap-1 and lap-2 bands
# of the current board from its lap-1 bands LAP1 (ids <board>.<n>). A lap-2
# band's id is <start>.<m>, a start's the first lap-1 band giving it; a lap-1
# band paired with it is <lap-2 band>_<n>. Fails with WHY when there is none.
side_bands() {
    local d=$BOARD_DIR
    # lap 2's starts: each lap-1 band with only its frame left of column K+1
    # and (FREEZE) its blocks (masks/share.csv lifts the rest), s1's own cells
    # put back around them; one per distinct start, and class.txt maps every
    # lap-1 band to its start
    "$BIN/E555_diver" "$SEED" "$1" "$d/cut.csv" --holes "$MASKS/share.csv" --end_dive 0 \
        --emit_score 0 --threads "$THREADS" > "$d/cut.log" 2>&1 || : > "$d/cut.csv"
    : > "$d/class.txt"
    awk -F, -v cls="$d/class.txt" '!/^[#%]/ && NF {
            key = $0; sub(/^[^,]*,[^,]*,/, "", key); gsub(/ /, "", key)
            if (!(key in S)) { S[key] = $1; print }
            print $1 "," S[key] > cls }' "$d/cut.csv" > "$d/share.csv"
    overlay "$d/share.csv" "$d/s1raw.csv" "$d/start.csv" "$SEED"
    cut_bands "$d/start.csv" "$d/l2.csv" "$(left)" right
    [ "$(count_boards "$d/l2.csv")" -gt 0 ] ||
        { WHY="lap 2: no band (no top-right corner closes under a lap-1 band's frame)"; return 1; }
    # a lap-1 band under each lap-2 band's frame: the lap-1 band's inner cells,
    # the lap-2 band's frame
    awk -F, 'BEGIN {OFS = ","}
        FILENAME == ARGV[1] { split($0, f, ","); M[f[2]] = M[f[2]] " " f[1]; next }
        FILENAME == ARGV[2] { if (!/^[#%]/ && NF) { n = NF - 512; id = $1
            for (p = 1; p <= 256; p++) { c = $(n + p) + 0; if (c == 999) continue
                r = int(c / 16); k = c % 16
                if (r > 0 && r < 15 && k > 0 && k < 15) IN[id] = IN[id] " " p ":" c ":" ($(n + 256 + p) + 0) } }; next }
        !/^[#%]/ && NF {
            n = NF - 512; b2 = $1; st = b2; sub(/\.[0-9]+$/, "", st)
            for (p = 1; p <= 256; p++) { c = $(n + p) + 0; if (c == 999) continue
                r = int(c / 16); k = c % 16
                if (r > 0 && r < 15 && k > 0 && k < 15) { $(n + p) = 999 } }
            base = $0; nm = split(M[st], mem, " ")
            for (i = 1; i <= nm; i++) {
                $0 = base; b1 = mem[i]; j = b1; sub(/^.*\./, "", j)
                m = split(IN[b1], cell, " ")
                for (q = 1; q <= m; q++) { split(cell[q], x, ":"); $(n + x[1]) = x[2]; $(n + 256 + x[1]) = x[3] }
                $1 = b2 "_" j; print
            } }' "$d/class.txt" "$1" "$d/l2.csv" > "$d/l1.csv"
}

whirl() {
    local k=$1 pool=$2 side=$3 d=$BOARD_DIR/$4 bands n g turn=1 label="lap $1" e
    [ "$side" = right ] || turn=3
    [ "$4" != lap3s2 ] || label="lap 3 from s2"
    rm -rf "$d"; mkdir -p "$d"
    if [ "$k" -le 2 ]; then
        bands=$pool
    else
        cut_bands "$pool" "$d/bands.csv" "$(left)" "$side"; bands=$d/bands.csv
    fi
    [ "$(count_boards "$bands")" -gt 0 ] || { WHY="$label: no band (no corner closes)"; return 1; }
    python3 "$TOOLS/E555_rank.py" "$bands" --seed_file "$SEED" --diverse $(( BANDS_GROWN * LAP_TRIES )) \
        --out "$d/picked.csv" > /dev/null 2>&1 || cp "$bands" "$d/picked.csv"
    n=$(count_boards "$d/picked.csv")
    for ((g = 1; (g - 1) * BANDS_GROWN < n; g++)); do
        [ "$(left)" -gt 10 ] || { WHY="out of time"; return 1; }
        e=$d/g$g
        awk -v a=$(( (g - 1) * BANDS_GROWN )) -v b=$(( g * BANDS_GROWN )) \
            '!/^[#%]/ && NF { i++; if (i > a && i <= b) print }' "$d/picked.csv" > "$d/group$g.csv"
        grow "$label" "$d/group$g.csv" "$e" "$turn" || continue
        NEXT[$((k + 1))]=$e
        [ "$((k + 1))" -le "$DEEPEST" ] || DEEPEST=$((k + 1))
        case "$k" in
            1)  # lap 2 grows the lap-2 bands whose lap-1 partner re-grew (an s2
                # is <lap-2 band>_<j>.<n>)
                sed 's/,.*//; s/\.[0-9]*$//; s/_[0-9]*$//' "$e/next.csv" > "$e/b2.txt"
                select_ids "$e/b2.txt" "$BOARD_DIR/l2.csv" > "$e/l2.csv"
                whirl 2 "$e/l2.csv" right lap2 && return 0 ;;
            2)  whirl 3 "$e/next.csv" right lap3 && return 0
                # else s1's top from the other side: the s2s whose lap-2 band
                # re-grew an s3 here (an s3 is <lap-2 band>.<n>), each tried
                # once per board
                sed 's/,.*//; s/\.[0-9]*$//' "$e/next.csv" > "$e/b2.txt"
                awk -F, 'FILENAME == ARGV[1] { B[$1]; next }
                         FILENAME == ARGV[2] { T[$1]; next }
                         !/^[#%]/ && NF { b = $1; sub(/\.[0-9]+$/, "", b); sub(/_[0-9]+$/, "", b)
                                          if ((b in B) && !($1 in T)) print }' \
                    "$e/b2.txt" "$BOARD_DIR/s2_tried.txt" "${NEXT[2]}/next.csv" > "$e/s2.csv"
                cut -d, -f1 "$e/s2.csv" >> "$BOARD_DIR/s2_tried.txt"
                [ "$(count_boards "$e/s2.csv")" -gt 0 ] || continue
                whirl 3 "$e/s2.csv" left lap3s2 && return 0 ;;
            3)  return 0 ;;
        esac
    done
    return 1
}

# dive_jewel -- E555_diver on each of the four boards of the jewel just written
# to jewels.csv; appends them to dived.csv and sets DIVE_NOTE to their scores
# and the best of the four
dive_jewel() {
    local j d=$BOARD_DIR s scores= best=0
    for j in 1 2 3 4; do
        tail -n 4 jewels.csv | sed -n "${j}p" > "$d/to_dive.csv"
        rm -f "$d/dived.csv"
        "$BIN/E555_diver" "$SEED" "$d/to_dive.csv" "$d/dived.csv" \
            --end_dive "$DIVE_DIVES" --end_polish "$DIVE_POLISH" --copies "$DIVE_COPIES" \
            --emit_score 0 --threads "$THREADS" --print_cmd > "$d/dive$j.log" 2>&1 || true
        # a complete board has nothing to dive: it is re-dived as it is
        [ -s "$d/dived.csv" ] && [ "$(count_boards "$d/dived.csv")" -gt 0 ] ||
            cp "$d/to_dive.csv" "$d/dived.csv"
        # then its most damaged band, two rows deeper than the damage reaches,
        # dived again and again until DIVE_REOPEN seconds are spent; the board
        # written is never worse than the one read
        if [ "$DIVE_REOPEN" -gt 0 ]; then
            rm -f "$d/redived.csv"
            "$BIN/E555_diver" "$SEED" "$d/dived.csv" "$d/redived.csv" --reopen auto+2 \
                --rounds 1000000 --wall_time "$DIVE_REOPEN" --copies 8 --end_dive 1000 \
                --end_polish 10000 --emit_score 0 --threads "$THREADS" --print_cmd \
                > "$d/redive$j.log" 2>&1 || true
            if [ -s "$d/redived.csv" ] && [ "$(count_boards "$d/redived.csv")" -gt 0 ]; then
                mv "$d/redived.csv" "$d/dived.csv"
            fi
        fi
        s=$(python3 "$TOOLS/E555_rank.py" "$d/dived.csv" --seed_file "$SEED" --field score 2> /dev/null) || s=
        grep -v '^[#%]' "$d/dived.csv" >> dived.csv || :
        s=${s:-0}; scores=$scores${scores:+/}$s; [ "$s" -le "$best" ] || best=$s
    done
    DIVE_NOTE="dive=$scores max=$best"
}

# whirl_board N FILE -- whirl one board; on a jewel with a new frame, keep it
whirl_board() {
    local nb=$1 t0=$SECONDS j id from b2 b1 s2 s3 s4 p shared
    BOARD_DIR=$RUN_DIR/boards/$nb; rm -rf "$RUN_DIR/boards"; mkdir -p "$BOARD_DIR"
    BOARD_END=$((SECONDS + BOARD_TIME)); DEEPEST=1; WHY=
    cp "$2" "$BOARD_DIR/s1raw.csv"; : > "$BOARD_DIR/s2_tried.txt"; id=$(cut -d, -f1 "$2")
    if [ ! -s "bands1/$id.csv" ]; then
        log "[board $nb] reached s1; last: lap 1: no band (the frame and columns 0..$K do not close) (0 s)"; return
    fi
    if ! side_bands "bands1/$id.csv" || ! whirl 1 "$BOARD_DIR/l1.csv" left lap1; then
        log "[board $nb] reached s$DEEPEST; last: $WHY ($((SECONDS - t0)) s)"; return
    fi
    # The tree, from s4 back, by the ids alone: a band is <the board it was cut
    # from>.<n>, a re-grown board <its band>.<n>, a paired lap-1 band <lap-2
    # band>_<n>
    nth_board "${NEXT[4]}/next.csv" 1 > "$BOARD_DIR/s4.csv"
    s4=$(cut -d, -f1 "$BOARD_DIR/s4.csv"); p=${s4%.*}; p=${p%.*}
    case "${NEXT[4]}" in
        */lap3s2/*) from=s2; s2=$p; b1=${s2%.*}; b2=${b1%_*}
                    s3=$(first_child "${NEXT[3]}/next.csv" "$b2") ;;
        *)          from=s3; s3=$p; b2=${s3%.*}
                    s2=$(first_child "${NEXT[2]}/next.csv" "$b2" paired); b1=${s2%.*} ;;
    esac
    board_by_id "${NEXT[2]}/next.csv" "$s2" > "$BOARD_DIR/s2.csv"
    board_by_id "${NEXT[3]}/next.csv" "$s3" > "$BOARD_DIR/s3.csv"
    # s1 as the jewel holds it: its two side bands on the beamer board, the
    # lap-2 band first; where they took the same piece, or a beamer cell no
    # longer fits, the cell stays open
    board_by_id "$BOARD_DIR/l1.csv" "$b1" > "$BOARD_DIR/b1.csv"
    board_by_id "$BOARD_DIR/l2.csv" "$b2" > "$BOARD_DIR/b2.csv"
    overlay "$BOARD_DIR/b1.csv" "$BOARD_DIR/s1raw.csv" "$BOARD_DIR/x.csv" "$SEED"
    overlay "$BOARD_DIR/b2.csv" "$BOARD_DIR/x.csv" "$BOARD_DIR/s1.csv" "$SEED"
    if ! is_jewel; then
        log "[board $nb] reached s4 (from $from), but the frame was not held ($((SECONDS - t0)) s)"; return
    fi
    if ! new_frame "$BOARD_DIR/s1.csv"; then
        log "[board $nb] jewel, frame already found ($((SECONDS - t0)) s)"; return
    fi
    NJ=$((NJ + 1))
    for j in 1 2 3 4; do sed "s/^[^,]*/b${nb}_s$j/" "$BOARD_DIR/s$j.csv" >> jewels.csv; done
    # interior cells kept: s1->s2 (counter-clockwise), s1->s3 (clockwise),
    # s4's parent->s4, and s1->s4 (two turns)
    shared=$(shared_cells "$BOARD_DIR/s1.csv" "$BOARD_DIR/s2.csv" 3)/$(shared_cells "$BOARD_DIR/s1.csv" "$BOARD_DIR/s3.csv" 1)
    if [ "$from" = s3 ]; then shared=$shared/$(shared_cells "$BOARD_DIR/s3.csv" "$BOARD_DIR/s4.csv" 1)
    else shared=$shared/$(shared_cells "$BOARD_DIR/s2.csv" "$BOARD_DIR/s4.csv" 3); fi
    shared=$shared/$(shared_cells "$BOARD_DIR/s1.csv" "$BOARD_DIR/s4.csv" 2)
    DIVE_NOTE=; [ "$DIVE" != 1 ] || dive_jewel
    echo "board=$nb jewel=$NJ from=$from shared=$shared${DIVE_NOTE:+ $DIVE_NOTE} seconds=$((SECONDS - T0))" >> jewels.log
    log "[board $nb] JEWEL $NJ (s4 from $from), new frame, shared $shared cells${DIVE_NOTE:+, $DIVE_NOTE} ($((SECONDS - t0)) s)"
}

# ---- main loop -----------------------------------------------------------------------
log "[cfg] run_dir=$RUN_DIR threads=$THREADS wall=$WALL board_time=$BOARD_TIME rng_seed=$RNG_SEED db_file=${DB_FILE:-<rebuilt each call>}"
log "[cfg] clues=$CLUES freeze=$FREEZE K=$K borders=$BORDERS beam: stop_row=$BEAM_STOP_ROW min_col=$BEAM_MIN_COL bottoms=$BEAM_BOTTOMS columns=$BEAM_COLUMNS tau=$BEAM_TAU corners=$BEAM_CORNERS; bands: $BANDS per board, $BANDS_GROWN x $LAP_TRIES grown; finalizer: stop_row=$FIN_STOP_ROW lap_time=$LAP_TIME pool=$FIN_BOARDS; dive=$DIVE ($DIVE_DIVES/$DIVE_POLISH x $DIVE_COPIES, reopen ${DIVE_REOPEN} s)"
T0=$SECONDS; NB=0; NJ=0; NP=0; EMPTY=0
# The regions the tools are asked about (rank --group_box/--complete, diver
# --holes), as mask files
MASKS=$RUN_DIR/masks; mkdir -p "$MASKS"
FRAME='(r == 0 || r == 15 || c == 0 || c == 15)'
BLOCKS="((r == 1 || r == 2 || r == 13 || r == 14) && (c == 1 || c == 2 || c == 13 || c == 14) || $FROZEN_CENTRE && (r == 7 || r == 8) && (c == 7 || c == 8))"
FROZEN=$([ "$FREEZE" = 0 ] && echo 0 || echo 1)
C0=$([ "$BORDERS" = random ] && echo 0 || echo 1)
mask "$MASKS/frame.csv" "$FRAME"                                   # one board per frame
mask "$MASKS/held.csv"  "$BLOCKS"                                  # the frozen blocks
mask "$MASKS/jewel.csv" "$FRAME || $FROZEN && $BLOCKS"             # what a jewel shares
mask "$MASKS/open.csv"  "r > $BEAM_STOP_ROW && (c >= $C0 && c <= $K || c >= 15 - $K && c <= 14)"
mask "$MASKS/share.csv" "!($FRAME && c <= $K) && !($FROZEN && $BLOCKS)"   # lifted for lap 2's starts
# The beamer maps the database file into memory and reads only the pages it
# touches, in random order, which from a cold disk can stall a produce call for
# minutes; one sequential read first (it counts against WALL) puts the whole
# file in the page cache.
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
        # a round whose annealed borders all lack a usable corner is not a
        # broken beamer: the next round anneals afresh
        [ "$NOBORDER" = 0 ] || continue
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

if [ "$DIVE" = 1 ] && [ -s dived.csv ]; then
    python3 "$TOOLS/E555_rank.py" dived.csv --seed_file "$SEED" --top 20 --out best.csv --rescore > /dev/null || true
    log "[dive] $(count_boards dived.csv) boards dived, best $(python3 "$TOOLS/E555_rank.py" dived.csv --seed_file "$SEED" --field score 2> /dev/null || echo "?") -> $RUN_DIR/best.csv"
fi
