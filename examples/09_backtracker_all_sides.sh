#!/bin/bash
# 09_backtracker_all_sides.sh -- fill each input board as far as it goes without
# breaking an edge, by searching it from several directions and keeping the best.
#
#   bash examples/09_backtracker_all_sides.sh
#   bash examples/09_backtracker_all_sides.sh BOARDS=stage_c_out/3_patched.csv
#   bash examples/09_backtracker_all_sides.sh OUT=run1.csv TIME_LIMIT=600 QUIET=0
#
# What it does:
#   1. runs bin/E555_backtracker once per entry in DIRS, each on the same input
#      board, at --breaks 0 so no mismatched piece is ever placed;
#   2. keeps, per input board, whichever direction filled the most cells;
#   3. runs the first direction once more over those winners;
#   4. reports with tools/E555_rank.py.
#
# OUT holds exactly one board per input board, break-free, in the orientation it
# came in.
#
# A direction is an --order plus a --rotate. --rotate turns the board before the
# search and turns the result back, which is the only way to make a static order
# start on another side. --rotate 2 puts the top border at row 0, which suits the
# usual Stage B board, filled from row 0 upward.
#
# Worth knowing:
#   - An exact search cannot skip a cell: a direction stops at the first empty
#     cell no piece fits. That is why several are tried.
#   - An input board that already has a broken edge is passed through unchanged,
#     because --breaks 0 refuses it. Feed this break-free partials.
#   - TIME_LIMIT is per call, and an exhaustive search need not finish, so set it.
#   - FOURSIDES=1 appends --order 4sides. It can step over a dead cell and so
#     fills more, but it has no early exit and will spend the whole TIME_LIMIT on
#     an open board. Off by default.
#   - HOLES= or CLUES=1 adds one preparation call in front, unrotated, which is
#     where the mask and the hint pieces are applied: the tool applies --holes
#     after --rotate, so a mask written in the CSV frame suits only an unrotated
#     call. A clue lands only in an empty cell, so a board whose clue cells are
#     taken is dropped; give it room with HOLES=, or start from a board built by
#     bin/E555_beamer --clue_center --clue_corners.
#   - The record window (FIRST_LINE, N_LINES) applies to every call that reads
#     BOARDS, which is all of them.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_partial_row12.csv     # break-free partial boards
OUT=backtracked_all_sides.csv           # the only file this run leaves behind
HOLES=                                  # optional --holes mask, first call only
FIRST_LINE=0            # --start_row, first call only
N_LINES=0               # --num_rows, first call only; 0 = every record
THREADS=8
TIME_LIMIT=300          # seconds per call
FOURSIDES=0             # 1 = finish with --order 4sides
CLUES=0                 # 1 = force the published hint pieces on first
CLUE_ORIENT=0           # which of the four orientations, for an unclued board
TOP=5                   # rows in the closing rank.py report
QUIET=1                 # 1 = hide the backtracker's own output
DIRS="--order rowmajor --rotate 2|--order spiral --reverse --rotate 2|--order rowmajor --reverse --rotate 2|--order colmajor --rotate 1|--order colmajor --reverse --rotate 3"
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
cd "$REPO"
[ -x bin/E555_backtracker ] || make backtracker
[ -f "$SEED" ]   || { echo "no seed file: $SEED" >&2; exit 1; }
[ -s "$BOARDS" ] || { echo "no input boards: $BOARDS" >&2; exit 1; }

IFS='|' read -r -a DIR_LIST <<< "$DIRS"
trap 'rm -f "$OUT".pass*' EXIT

rows()   { grep -v '^#' "$1" 2>/dev/null | grep -c . || true; }
# Readers take the last 512 fields as pos+rot, so pos is NF-511..NF-256 and 999
# means unplaced. This is the fullest board in a file.
placed() { awk -F, 'NF>=512 { n=0
                              for (i=NF-511; i<=NF-256; i++) if ($i!=999) n++
                              if (n>m) m=n }
                    END { print m+0 }' "$1" 2>/dev/null || echo 0; }

# One backtracker call.
call() {
    local src=$1 dst=$2; shift 2
    set -- bin/E555_backtracker "$SEED" "$src" "$dst" "$@" \
           --breaks 0 --time_limit "$TIME_LIMIT" --threads "$THREADS" --print_cmd
    if [ "$QUIET" = 1 ]; then "$@" >/dev/null 2>&1; else echo; "$@"; fi ||
        { echo "call failed on $src; re-run with QUIET=0 to see why" >&2; exit 1; }
}

# The record window selects input records, so EVERY call that reads $BOARDS
# needs it -- the directions all read $BOARDS, they are not chained -- and the
# calls that read this script's own output must not have it.
window=(--start_row "$FIRST_LINE" --num_rows "$N_LINES")
on_ours=(--start_row 0 --num_rows 0)

# A mask and the clues, though, are applied ONCE, by a preparation call, and the
# directions then read its output. The tool applies --holes after --rotate, so a
# mask written in the CSV frame is only correct for an unrotated call, and every
# direction here is rotated.
prep=()
[ -n "$HOLES" ] && prep+=(--holes "$HOLES")
[ "$CLUES" = 1 ] &&
    prep+=(--clue_center --clue_corners --clue_orient "$CLUE_ORIENT")

NCALL=$(( (${#prep[@]} ? 1 : 0) + ${#DIR_LIST[@]} + 1 + FOURSIDES))
n=0
step() { n=$((n+1)); printf '[%d/%d] %-42s ' "$n" "$NCALL" "$1"; }

echo "=== E555 backtracker, all sides ==="
echo "  seed   : $SEED"
echo "  input  : $BOARDS ($(rows "$BOARDS") board(s), fullest $(placed "$BOARDS") placed)"
echo "  output : $OUT"
echo "  calls  : $NCALL, --breaks 0, ${TIME_LIMIT}s each, $THREADS thread(s)"
[ -n "$HOLES" ] && echo "  holes  : $HOLES (first call only)"
echo

# -- optional: apply the mask and the clues, once, unrotated ------------------
src="$BOARDS"
if [ "${#prep[@]}" -gt 0 ]; then
    label="prepare:"
    [ -n "$HOLES" ]   && label="$label mask"
    [ "$CLUES" = 1 ]  && label="$label clues"
    step "$label --order mrv"
    call "$BOARDS" "$OUT.pass_prep.csv" "${window[@]}" "${prep[@]}" --order mrv
    src="$OUT.pass_prep.csv"
    window=("${on_ours[@]}")
    echo "$(rows "$src") board(s), fullest $(placed "$src")"
    if [ "$(rows "$src")" -eq 0 ]; then
        echo "every board was dropped: a clue cell holds another piece, or a clue" >&2
        echo "  piece sits elsewhere on the board, and such a board is not written." >&2
        echo "  Free those cells with HOLES=, or start from a board built with the" >&2
        echo "  clues by bin/E555_beamer --clue_center --clue_corners." >&2
        exit 1
    fi
fi

# -- one call per direction, all from the same board --------------------------
: > "$OUT.pass_pool.csv"
for i in "${!DIR_LIST[@]}"; do
    read -r -a flags <<< "${DIR_LIST[$i]}"
    step "${DIR_LIST[$i]}"
    call "$src" "$OUT.pass$i.csv" "${window[@]}" "${flags[@]}"
    echo "fullest $(placed "$OUT.pass$i.csv")"
    grep -v '^#' "$OUT.pass$i.csv" >> "$OUT.pass_pool.csv" || true
done
[ -s "$OUT.pass_pool.csv" ] || { echo "every direction came back empty" >&2; exit 1; }

# -- keep the fullest board per input board -----------------------------------
# Every direction writes the same config_id for a given input record, so that
# field groups the copies of one board.
awk -F, 'NF>=512 { n=0
                   for (i=NF-511; i<=NF-256; i++) if ($i!=999) n++
                   if (n>best[$1]) { best[$1]=n; row[$1]=$0 } }
         END { for (k in row) print row[k] }' \
    "$OUT.pass_pool.csv" > "$OUT.pass_best.csv"
echo "        kept $(rows "$OUT.pass_best.csv") board(s), one per input board"

# -- one more pass: the first direction over boards the others produced -------
read -r -a flags <<< "${DIR_LIST[0]}"
step "again: ${DIR_LIST[0]}"
call "$OUT.pass_best.csv" "$OUT.pass_again.csv" "${on_ours[@]}" "${flags[@]}"
cur="$OUT.pass_again.csv"
echo "fullest $(placed "$cur")"

if [ "$FOURSIDES" = 1 ]; then
    step "--order 4sides"
    call "$cur" "$OUT.pass_4s.csv" "${on_ours[@]}" --order 4sides
    cur="$OUT.pass_4s.csv"
    echo "fullest $(placed "$cur")"
fi

# Each call appends _<number> to the config_id it was given. Drop the ones this
# run added and keep the last, so r0c669_0_407_408 becomes r0c669_408.
TRIM=$(( (${#prep[@]} ? 1 : 0) + 2 + FOURSIDES))
awk -v n="$TRIM" -F, 'BEGIN { OFS="," }
    /^#/ { print; next }
    { k = split($1, a, "_")
      if (k > n) { id = a[1]
                   for (i = 2; i <= k - n; i++) id = id "_" a[i]
                   $1 = id "_" a[k] }
      print }' "$cur" > "$OUT.pass_trim.csv"
mv "$OUT.pass_trim.csv" "$OUT"

echo
echo "Wrote $OUT ($(rows "$OUT") board(s), fullest $(placed "$OUT") placed)"
echo
python3 tools/E555_rank.py "$OUT" --seed_file "$SEED" --top "$TOP"
