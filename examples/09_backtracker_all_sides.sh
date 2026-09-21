#!/bin/bash
# 09_backtracker_all_sides.sh -- attack one board from every side with exact,
# break-free searches, and keep what each side manages.
#
#   bash examples/09_backtracker_all_sides.sh
#   bash examples/09_backtracker_all_sides.sh BOARDS=stage_c_out/3_patched.csv
#   bash examples/09_backtracker_all_sides.sh OUT=run1.csv TIME_LIMIT=600 QUIET=0
#
# Every call is an exhaustive exact search at --breaks 0. Nothing here ever
# places a mismatched piece and nothing uses --jump, so each call answers the
# honest question "how far can this direction fill the board without breaking an
# edge", and every board this writes is break-free.
#
# THE ONE THING THAT MATTERS: a hard exact pass cannot skip a cell. It walks its
# order and the first empty cell no piece fits ends it there. So a pass is worth
# exactly the length of its walk before it meets a dead cell -- which means WHERE
# THE WALK STARTS decides everything, and that is what this script varies.
#
# CHAINING ONE PASS INTO THE NEXT DOES NOT WORK, and that is why this script no
# longer does it. The first pass fills the cells that could be filled and leaves
# precisely the cells nothing fits, so the next order walks straight into one and
# stops. Measured: chaining six orders over data/board_partial_row12.csv reached
# 227 pieces, and a greedy search over all 56 available traversals could only
# push that to 228 before nothing could add anything at all.
#
# SO EVERY DIRECTION STARTS FROM THE SAME BOARD (phase 1, "spread"). Each one
# then produces its own break-free board and none is wasted, which is the point
# of the example. Only afterwards does a short chain run over the pool (phase 2,
# "consolidate"), where a direction that could do nothing for one board can still
# improve another.
#
# ROTATION IS THE REAL LEVER, and --rotate does it inside the tool: it turns the
# board K quarter-turns before the search and turns the result back, so the CSV
# comes out in the orientation it went in (verified: every input piece returns to
# its own cell at its own spin). A static order always starts at the same corner
# of ITS board, so rotating the board is the only way to make rowmajor start
# anywhere else. It is worth far more than the order flags:
#
#     --order rowmajor            --rotate 0    210 placed     (+2)
#     --order rowmajor            --rotate 1    212 placed     (+4)
#     --order rowmajor            --rotate 3    211 placed     (+3)
#     --order rowmajor            --rotate 2    227 placed    (+19)
#
# One flag, from +2 to +19 on the same board with the same order. The reason is
# the shape of a Stage B board: it is filled from row 0 up, so the empty region
# sits against the top border. --rotate 2 turns the top border into row 0, and a
# bottom-up order then LEADS FROM THE BORDER -- the most constrained cells, where
# a wrong piece is refuted immediately -- instead of starting in open space.
# Whenever your boards are filled from one side, turn that side to the bottom.
#
# THE DIRECTIONS, measured over five break-free partials of the shipped board
# (rows 0..12 down to rows 0..8, 880 pieces in total):
#
#     --order rowmajor --rotate 2                +205   top border down, L->R
#     --order rowmajor --reverse --rotate 2      +204   top border down, R->L
#     --order colmajor --rotate 1                +204   from one lateral side
#     --order colmajor --reverse --rotate 3      +204   from the other
#     --order snake --reverse --rotate 2         +203   boustrophedon from the top
#     --order spiral --reverse --rotate 2        +186   outer ring inward
#
# Six directions over five boards gave 30 rows holding 18 DISTINCT boards, and
# lifted the per-record best from 880 to 1089 pieces. Two consolidation passes
# took that to 1095, against 1085 for the single best direction run alone. The
# consolidation is worth about one piece a board -- real, but small, which is why
# CONSOLIDATE is two passes and not six.
#
# FOURSIDES=1 appends --order 4sides, and it is off by default because it is the
# one pass that can run out the clock. Side growth sets soft completion, so no
# prune can refute a subtree and it only stops when its sweep is exhausted:
# measured on the shipped board, 20 s with 48 cells open against 0.12 s with 29.
# It is also the one pass here that CAN step over a dead cell, so it is the way
# to grow a board the exact directions have finished with: on the shipped board
# they end at 228 and 4sides takes that to 245. Turn it on when you have time.
#
# CLUES=1 prepends one call carrying --clue_center --clue_corners --clue_orient,
# so the published hint pieces go on before any direction sees the board. The
# tool's rule stands: a clue lands only in an EMPTY cell, at --breaks 0 the board
# is dropped if the clue disagrees with a placed neighbour, and a clue cell
# holding another piece or a clue piece stranded elsewhere is a conflict that
# drops the board UNWRITTEN. data/board_partial_row12.csv has no room -- rows
# 0..12 are full, which takes the centre and both bottom corners and strands all
# five pieces -- so CLUES=1 needs either HOLES= a mask that frees the clue cell,
# every placed neighbour that disagrees with it and the cell each clue piece is
# stranded on, or a board built upstream by
# bin/E555_beamer --clue_center --clue_corners, which costs nothing.
# CLUE_ORIENT is consulted only for a board carrying no clue at all.
# Pair CLUES=1 with FOURSIDES=1: a clue mask frees exactly the cells nothing
# fits exactly any more, so every exact direction stops on one. Measured with
# the 22-cell mask for the shipped board, the clue call leaves 193 pieces, the
# six directions and the consolidation add nothing, and 4sides recovers to 223
# with all five clues in place.
#
# THE MASK AND THE RECORD WINDOW APPLY ONCE, to the first call only: --holes
# REOPENS cells, so handing it to every direction would re-empty that region
# every time.
#
# Only $OUT is left behind: the per-pass files go however the run ends.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_partial_row12.csv     # break-free partials: what this suits
OUT=backtracked_all_sides.csv           # the only file this run leaves behind
HOLES=                                  # optional mask, first call only
FIRST_LINE=0            # --start_row, first call only
N_LINES=0               # --num_rows, first call only; 0 = every record
THREADS=8
TIME_LIMIT=300          # seconds PER CALL, 0 = unlimited
CONSOLIDATE=2           # directions re-run over the pool, chained (0 = none)
FOURSIDES=0             # 1 = finish with --order 4sides (see above)
CLUES=0                 # 1 = force the published hint pieces on first
CLUE_ORIENT=0           # which of the four orientations, for an unclued board
SHORT_ID=1              # 1 = trim the chained config_id down to base_<final>
TOP=5                   # rows in the closing rank.py report
QUIET=1                 # 1 = hide the backtracker's own output; 0 = show it
DIRS="--order rowmajor --rotate 2|--order rowmajor --reverse --rotate 2|--order colmajor --rotate 1|--order colmajor --reverse --rotate 3|--order snake --reverse --rotate 2|--order spiral --reverse --rotate 2"
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
cd "$REPO"
[ -d bin ] && [ -d tools ] ||
    { echo "REPO=$REPO is not an E555 checkout -- set REPO at the top" >&2; exit 1; }
[ -x bin/E555_backtracker ] || make backtracker
[ -f "$SEED" ]   || { echo "no seed file: $SEED" >&2; exit 1; }
[ -s "$BOARDS" ] || { echo "no input boards: $BOARDS" >&2; exit 1; }

IFS='|' read -r -a DIR_LIST <<< "$DIRS"
NDIR=${#DIR_LIST[@]}
[ "$NDIR" -gt 0 ] || { echo "DIRS is empty" >&2; exit 1; }
[ "$CONSOLIDATE" -le "$NDIR" ] ||
    { echo "CONSOLIDATE=$CONSOLIDATE exceeds the $NDIR direction(s) in DIRS" >&2; exit 1; }

# Every working file is named after $OUT, so one glob clears them -- on success,
# on failure and on Ctrl-C alike.
cleanup() { rm -f "$OUT".pass*; }
trap cleanup EXIT

# Data rows only: the tools write a comment header, and a call that produced
# nothing would otherwise look like a record.
count_rows()  { grep -v '^#' "$1" 2>/dev/null | grep -c . || true; }
# Readers take the LAST 512 fields as pos+rot, so pos is NF-511..NF-256 and 999
# means unplaced. The best board in a file is the number this run is judged on.
best_placed() {
    awk -F, '/^#/ { next }
             NF >= 512 { n = 0
                         for (i = NF - 511; i <= NF - 256; i++) if ($i != 999) n++
                         if (n > m) m = n }
             END { print m + 0 }' "$1" 2>/dev/null || echo 0
}

# Each call appends _<connected edges> to every config_id it is handed, and the
# reader rejects one of 256 characters or more. A board here collects one
# component per call on its own path: the clue call, its own direction, then the
# consolidation and 4sides calls.
TRIM=$((CLUES + 1 + CONSOLIDATE + FOURSIDES))
longest=$(grep -v '^#' "$BOARDS" | grep . | cut -d, -f1 |
          awk '{ if (length($0) > n) n = length($0) } END { print n+0 }')
if [ $((longest + 5 * TRIM)) -ge 256 ]; then
    echo "config_id too long to chain: the longest in $BOARDS is $longest characters," >&2
    echo "  and the calls add ~$((5 * TRIM)) more, over the reader's 255-character limit." >&2
    echo "  Shorten the ids first, e.g.:" >&2
    echo "    awk -F, 'BEGIN{OFS=\",\"} /^#/{print;next} {\$1=substr(\$1,length(\$1)-20)}1' \\" >&2
    echo "        $BOARDS > short.csv" >&2
    exit 1
fi

# One call. $1 = input, $2 = output, rest = the flags that make it a direction.
# first_call adds the record window and the mask, which describe the INPUT and
# so must be applied exactly once.
first_call=1
run_pass() {
    local src=$1 dst=$2; shift 2
    local win=(--start_row 0 --num_rows 0) hole=() rc=0
    if [ "$first_call" = "1" ]; then
        win=(--start_row "$FIRST_LINE" --num_rows "$N_LINES")
        [ -n "$HOLES" ] && hole=(--holes "$HOLES")
        first_call=0
    fi
    if [ "$QUIET" = "1" ]; then
        bin/E555_backtracker "$SEED" "$src" "$dst" "${hole[@]}" "${win[@]}" "$@" \
            --breaks 0 --time_limit "$TIME_LIMIT" --threads "$THREADS" \
            --print_cmd > /dev/null 2>&1 || rc=$?
    else
        echo
        bin/E555_backtracker "$SEED" "$src" "$dst" "${hole[@]}" "${win[@]}" "$@" \
            --breaks 0 --time_limit "$TIME_LIMIT" --threads "$THREADS" \
            --print_cmd || rc=$?
    fi
    if [ "$rc" -ne 0 ]; then
        echo "FAILED (exit $rc)"
        echo "  a call failed on $src" >&2
        [ "$QUIET" = "1" ] && echo "  re-run with QUIET=0 to see why" >&2
        exit "$rc"
    fi
}

NCALL=$((CLUES + NDIR + CONSOLIDATE + FOURSIDES))
echo "=== E555 backtracker, all sides ==="
echo "  seed   : $SEED"
echo "  input  : $BOARDS ($(count_rows "$BOARDS") board(s), best $(best_placed "$BOARDS") placed)"
echo "  output : $OUT"
echo "  calls  : $NCALL, --breaks 0, ${TIME_LIMIT}s each, $THREADS thread(s)$(
        [ "$QUIET" = "1" ] && echo '   (QUIET=1: tool output hidden)')"
[ "$CLUES" = "1" ] && echo "  clues  : forced on first, orientation $CLUE_ORIENT"
[ -n "$HOLES" ]    && echo "  holes  : $HOLES (first call only)"
echo

step=0
say() { step=$((step + 1)); printf '[%d/%d] %-46s ' "$step" "$NCALL" "$1"; }

# -- optional: put the clue pieces on before any direction sees the board ------
src="$BOARDS"
if [ "$CLUES" = "1" ]; then
    # mrv suits this call: whatever the mask freed is a handful of scattered
    # holes, and most-constrained-first is what fills those.
    say "clues + --order mrv"
    run_pass "$src" "$OUT.pass_clue.csv" \
             --clue_center --clue_corners --clue_orient "$CLUE_ORIENT" --order mrv
    src="$OUT.pass_clue.csv"
    echo "$(count_rows "$src") board(s), best $(best_placed "$src") placed"
    if [ "$(count_rows "$src")" -eq 0 ]; then
        echo "  no board could take the clues: every one was dropped for a clue" >&2
        echo "  conflict (a clue cell already holds another piece, or a clue piece" >&2
        echo "  is stranded elsewhere), and a dropped board is not written." >&2
        echo "  Free those cells with HOLES=mask.csv, or start from a board built" >&2
        echo "  upstream by bin/E555_beamer --clue_center --clue_corners." >&2
        exit 1
    fi
fi

# -- phase 1, spread: every direction from the SAME board ---------------------
: > "$OUT.pass_pool.csv"
for i in $(seq 1 "$NDIR"); do
    dir="${DIR_LIST[$((i - 1))]}"
    read -r -a flags <<< "$dir"
    say "$dir"
    run_pass "$src" "$OUT.pass$i.csv" "${flags[@]}"
    echo "$(count_rows "$OUT.pass$i.csv") board(s), best $(best_placed "$OUT.pass$i.csv") placed"
    grep -v '^#' "$OUT.pass$i.csv" >> "$OUT.pass_pool.csv" || true
done
[ -s "$OUT.pass_pool.csv" ] ||
    { echo "  every direction came back empty; nothing to rank" >&2; exit 1; }
cur="$OUT.pass_pool.csv"
echo "        pooled: $(count_rows "$cur") board(s), $(cut -d, -f3- "$cur" | sort -u | wc -l) distinct, best $(best_placed "$cur") placed"

# -- phase 2, consolidate: the same directions, now chained over the pool ------
for i in $(seq 1 "$CONSOLIDATE"); do
    dir="${DIR_LIST[$((i - 1))]}"
    read -r -a flags <<< "$dir"
    say "consolidate: $dir"
    run_pass "$cur" "$OUT.pass_c$i.csv" "${flags[@]}"
    cur="$OUT.pass_c$i.csv"
    echo "$(count_rows "$cur") board(s), best $(best_placed "$cur") placed"
done

# -- optional finisher: the only pass here that steps over a dead cell ---------
if [ "$FOURSIDES" = "1" ]; then
    say "--order 4sides"
    run_pass "$cur" "$OUT.pass_4s.csv" --order 4sides
    cur="$OUT.pass_4s.csv"
    echo "$(count_rows "$cur") board(s), best $(best_placed "$cur") placed"
fi

# Each call appended one component to the ids on this board's path, so dropping
# the last TRIM and putting the final one back leaves base_<final>. A row with
# fewer components than that did not come through this run; leave it alone.
if [ "$SHORT_ID" = "1" ]; then
    awk -v n="$TRIM" -F, 'BEGIN { OFS = "," }
        /^#/     { print; next }
        NF < 514 { print; next }
        { k = split($1, a, "_")
          if (k <= n) { print; next }
          id = a[1]
          for (i = 2; i <= k - n; i++) id = id "_" a[i]
          $1 = id "_" a[k]
          print }' "$cur" > "$OUT.pass_short.csv" && mv "$OUT.pass_short.csv" "$cur"
fi

mv "$cur" "$OUT"
echo
echo "Wrote $OUT ($(count_rows "$OUT") board(s), best $(best_placed "$OUT") placed)"
if [ -s "$OUT" ]; then
    echo
    python3 tools/E555_rank.py "$OUT" --seed_file "$SEED" --top "$TOP"
fi
