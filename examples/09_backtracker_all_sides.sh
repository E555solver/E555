#!/bin/bash
# 09_backtracker_all_sides.sh -- one board, five exact passes, each from a
# different direction.
#
#   bash examples/09_backtracker_all_sides.sh
#   bash examples/09_backtracker_all_sides.sh BOARDS=stage_c_out/3_patched.csv
#   bash examples/09_backtracker_all_sides.sh OUT=run1.csv TIME_LIMIT=3600 QUIET=0
#
# The backtracker fills empty cells and never removes a piece, so one run
# commits to whatever its cell order reaches first. On a board that cannot be
# closed exactly, WHICH order you used decides how far you get -- and there is
# no way to ask for several orders in one invocation. This chains the runs,
# feeding each the previous one's output.
#
# Every pass runs --breaks 0, so nothing ever places a mismatched piece: every
# board in the chain is break-free, and the final board is break-free with empty
# cells wherever no exact piece fits.
#
#   #  flags                          leads from        order_key
#   1  --order rowmajor               bottom, L->R      r*16 + c
#   2  --order rowmajor --reverse     same rows, R->L   r*16 + (15-c)
#   3  --order colmajor               left              c*16 + r
#   4  --order colmajor --reverse     right, downward   (15-c)*16 + (15-r)
#   5  --order spiral                 outer ring in     min(r,15-r,c,15-c)
#   6  --order 4sides    FOURSIDES=1  all four sides    literal side sequence
#
# Passes 1..5 are exhaustive EXACT completion attempts: each either closes the
# board, proves no zero-break completion exists from its starting board, or
# times out, and the board it emits is the deepest node it reached.
#
# Pass 5 is why spiral is here at all: no LINEAR order leads from the top. Every
# one keys on r ascending or on c, and --reverse on rowmajor only flips the
# within-row walk, so 1..4 lead from the bottom, bottom, left and right. spiral
# keys on the ring distance to the nearest border, so the whole outer ring --
# top row included -- comes before anything inward.
#
# THE CHAIN EARNS ITS KEEP ON TIME-OUTS. A pass that runs to exhaustion has
# proved no completion exists from its board; later passes start from a strictly
# smaller space and mostly re-prove it in milliseconds. On an easy board passes
# 2..5 are near no-ops. TIME_LIMIT is what turns this into a portfolio of
# attempts rather than one search repeated.
#
# And commitments are permanent: nothing downstream can undo a piece an earlier
# pass placed, and the deepest node of an exact search can be a fluke of the
# traversal.
#
# FEED IT BREAK-FREE BOARDS. --breaks 0 DROPS any record that already carries a
# broken edge ("input has N broken edge(s) > --breaks 0") and writes it through
# unchanged, so a board with breaks survives every pass untouched. The shipped
# default, data/board_partial_row12.csv, is break-free with rows 12..15 still
# open. Its sibling data/board_example_462.csv carries 18 breaks and is passed
# straight through.
#
# FOURSIDES=1 -- the optional sixth pass, and why it is off by default
#   4sides is a side-growth order, and side growth sets SOFT completion by
#   construction: all three completion prunes (zero-domain, remaining
#   feasibility, Hall) are skipped, and a cell with no candidate is stepped over
#   instead of forcing a backtrack. Nothing ever refutes a subtree, so the
#   search has no natural end -- it runs until --time_limit, on every board,
#   every time. That is the whole cost: measured on the shipped default at
#   --time_limit 20, passes 1..5 together finish in well under a second (spiral
#   exhausts its space in 0.003 s, proving no completion exists from there),
#   while 4sides alone burns all 20 s. It does buy something -- 242 pieces
#   against spiral's 226, because stepping over a dead cell is exactly how you
#   grow past one -- so it is the best-partial finisher and belongs last. Turn
#   it on when you want the biggest break-free partial and will pay for it:
#     bash examples/09_backtracker_all_sides.sh FOURSIDES=1 TIME_LIMIT=3600
#
# CLUES=1 -- force the published hint pieces on first
#   This prepends one more backtracker call carrying --clue_center
#   --clue_corners --clue_orient, so the clues go on before any search order
#   sees the board. The tool's own rule applies unsoftened: a clue lands only in
#   an EMPTY cell, and at --breaks 0 the board is dropped if the clue disagrees
#   with a placed neighbour. A clue cell holding another piece, or a clue piece
#   already sitting elsewhere on the board, is a conflict -- and a conflicted
#   board is dropped UNWRITTEN, so the chain has nothing to carry forward and
#   this script stops at step 1 and says so.
#
#   CLUES=1 therefore wants a board with room. data/board_partial_row12.csv has
#   none: rows 0..12 are full, which takes the centre and both bottom corners
#   and strands all five clue pieces, so every orientation is refused. Two ways
#   to give the clues room:
#     HOLES=mask.csv  a 16x16 mask emptying, per clue, the clue cell, every
#                     placed neighbour whose facing edge disagrees with it, and
#                     the cell the clue piece is stranded on. A clue whose four
#                     orthogonal neighbours are all empty cannot break anything,
#                     since only placed neighbours are ever broken against. On
#                     the shipped default that is 22 cells, and the run ends
#                     with all 5 clues placed. The passes that follow refill
#                     what they can, but rarely all of it -- you buy the clues
#                     with score, 431 down to 364 here.
#     upstream        cheapest by far: bin/E555_beamer --clue_center
#                     --clue_corners builds the board with three of the five
#                     already in place, and then there is nothing to evict.
#
#   CLUE_ORIENT only matters for a board carrying no clue at all: one that
#   already holds a clue has committed, and the tool reads the orientation off
#   it. The clue table belongs to the real Eternity II seed, so CLUES=1 on
#   data/synth_seed.txt would force pieces into cells its solution does not use
#   and make that board uncompletable -- it is a fixture for a different puzzle.
#
# THE MASK APPLIES ONCE, to the first step only. --holes REOPENS cells, so
# giving it to every pass would re-empty that region each time and discard
# whatever the previous pass had put there.
#
# MEASURED, on data/board_partial_row12.csv (208 pieces in), TIME_LIMIT=20,
# 4 threads, ARCH=generic:
#   --order spiral alone         226 placed, score 405, 0.003 s, space exhausted
#   passes 1..5 chained          214 placed, score 395, under 1 s in total
#   passes 1..6, FOURSIDES=1     242 placed, score 431, 20 s, all of it in 4sides
#   CLUES=1, 22-cell mask, 1..6  224 placed, score 364, 5 of 5 clues
#
# Read the first two lines together: here the chain scores BELOW a single spiral
# pass, because rowmajor goes first and its commitments are permanent -- spiral
# then inherits a board it cannot use as well as the original. That is the
# honest cost of a portfolio whose members cannot undo one another, and it is
# why PASSES is one editable string: on a board like this, lead with spiral.
# What the chain reliably buys is the 4sides finisher, which wants the fullest
# board it can be given and so belongs at the end.
#
# Only $OUT is left behind: the per-pass files are deleted however the run ends.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_partial_row12.csv     # a break-free partial: what this suits
OUT=backtracked_all_sides.csv           # the only file this run leaves behind
HOLES=                                  # optional mask, applied to step 1 only
FIRST_LINE=0            # --start_row, first pass only
N_LINES=0               # --num_rows, first pass only; 0 = every record
THREADS=8
TIME_LIMIT=300          # seconds PER PASS, 0 = unlimited (an unbounded
                        # exhaustive search need never finish -- set this)
FOURSIDES=0             # 1 = append the 4sides best-partial pass (see above;
                        # it always burns the whole TIME_LIMIT)
CLUES=0                 # 1 = force the published hint pieces on first (see above)
CLUE_ORIENT=0           # which of the four orientations, for an unclued board
TOP=5                   # rows in the closing rank.py report
QUIET=1                 # 1 = hide the backtracker's own output; 0 = show it
PASSES="--order rowmajor|--order rowmajor --reverse|--order colmajor|--order colmajor --reverse|--order spiral"
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

# The soft best-partial finisher is opt-in: it cannot refute a subtree, so it
# runs for the full TIME_LIMIT whatever the board, and it alone would set the
# wall clock of the whole chain.
[ "$FOURSIDES" = "1" ] && PASSES="$PASSES|--order 4sides"

IFS='|' read -r -a PASS_LIST <<< "$PASSES"
NPASS=${#PASS_LIST[@]}
[ "$NPASS" -gt 0 ] || { echo "PASSES is empty" >&2; exit 1; }

# Every per-pass file is named after $OUT, so one glob clears them -- on success,
# on failure, and on Ctrl-C alike.
cleanup() { rm -f "$OUT".pass*; }
trap cleanup EXIT

# Each pass appends _<score> to every config_id, and the reader rejects one of
# CONFIG_ID_LEN characters or more, so a long id chained enough times would stop
# parsing and the board would vanish mid-chain. The limit is 256 now, which is
# room for a 47-character Stage-B id and forty passes, but check anyway: failing
# here beats failing at the pass that dies.
longest=$(grep -v '^#' "$BOARDS" | grep . | cut -d, -f1 |
          awk '{ if (length($0) > n) n = length($0) } END { print n+0 }')
if [ $((longest + 5 * (NPASS + CLUES))) -ge 256 ]; then
    echo "config_id too long to chain: the longest in $BOARDS is $longest characters," >&2
    echo "  and the passes add ~$((5 * (NPASS + CLUES))) more, over the reader's 255-character limit." >&2
    echo "  Shorten the ids first, e.g.:" >&2
    echo "    awk -F, 'BEGIN{OFS=\",\"} /^#/{print;next} {\$1=substr(\$1,length(\$1)-20)}1' \\" >&2
    echo "        $BOARDS > short.csv" >&2
    exit 1
fi

# Data rows only: the tools write a comment header, and a record that produced
# nothing would otherwise look like a record.
count_rows() { grep -v '^#' "$1" 2>/dev/null | grep -c . || true; }

echo "=== E555 backtracker, all sides ==="
echo "  seed   : $SEED"
echo "  input  : $BOARDS ($(count_rows "$BOARDS") board(s))"
echo "  output : $OUT"
NSTEP=$((NPASS + CLUES))
echo "  passes : $NSTEP, --breaks 0, ${TIME_LIMIT}s each, $THREADS thread(s)$(
        [ "$QUIET" = "1" ] && echo '   (QUIET=1: tool output hidden)')"
[ "$CLUES" = "1" ] && echo "  clues  : forced on first, orientation $CLUE_ORIENT"
[ -n "$HOLES" ]    && echo "  holes  : $HOLES (step 1 only)"
echo

src="$BOARDS"
for n in $(seq 1 "$NSTEP"); do
    step=()
    if [ "$CLUES" = "1" ] && [ "$n" -eq 1 ]; then
        # mrv is right for the clue step: whatever the mask freed is a handful
        # of scattered holes, and most-constrained-first is what fills those.
        label="clues + --order mrv"
        step=(--clue_center --clue_corners --clue_orient "$CLUE_ORIENT" --order mrv)
    else
        label="${PASS_LIST[$((n - 1 - CLUES))]}"
        read -r -a step <<< "$label"
    fi
    if [ "$n" -eq "$NSTEP" ]; then dst="$OUT"; else dst="$OUT.pass$n.csv"; fi

    # Both the window and the mask describe the INPUT, so both apply once. The
    # window would otherwise re-narrow an already-narrowed file, and the mask
    # would re-empty cells a previous pass had filled.
    if [ "$n" -eq 1 ]; then
        win=(--start_row "$FIRST_LINE" --num_rows "$N_LINES")
        hole=(); [ -n "$HOLES" ] && hole=(--holes "$HOLES")
    else
        win=(--start_row 0 --num_rows 0)
        hole=()
    fi

    printf '[%d/%d] %-30s ' "$n" "$NSTEP" "$label"
    t0=$SECONDS
    if [ "$QUIET" = "1" ]; then
        bin/E555_backtracker "$SEED" "$src" "$dst" "${hole[@]}" "${win[@]}" \
            "${step[@]}" --breaks 0 --time_limit "$TIME_LIMIT" \
            --threads "$THREADS" --print_cmd > /dev/null 2>&1 || rc=$?
    else
        echo
        bin/E555_backtracker "$SEED" "$src" "$dst" "${hole[@]}" "${win[@]}" \
            "${step[@]}" --breaks 0 --time_limit "$TIME_LIMIT" \
            --threads "$THREADS" --print_cmd || rc=$?
    fi
    if [ "${rc:-0}" -ne 0 ]; then
        echo "FAILED (exit ${rc})"
        echo "  pass $n ($label) failed on $src" >&2
        [ "$QUIET" = "1" ] && echo "  re-run with QUIET=0 to see why" >&2
        exit "$rc"
    fi
    rows=$(count_rows "$dst")
    echo "$rows board(s), $((SECONDS - t0))s"
    # An empty file fed forward would fail the next pass with a far less useful
    # message than this one. A clue step that empties it means every board was
    # dropped unwritten for a clue conflict, which has its own remedies.
    if [ "$rows" -eq 0 ]; then
        if [ "$CLUES" = "1" ] && [ "$n" -eq 1 ]; then
            echo "  no board could take the clues: every one was dropped for a clue" >&2
            echo "  conflict (a clue cell already holds another piece, or a clue piece" >&2
            echo "  is stranded elsewhere), and a dropped board is not written." >&2
            echo "  Free those cells with HOLES=mask.csv, or start from a board built" >&2
            echo "  upstream by bin/E555_beamer --clue_center --clue_corners." >&2
            [ "$QUIET" = "1" ] &&
                echo "  Re-run with QUIET=0 to see which clue cell each board failed on." >&2
        else
            echo "  pass $n emitted no board; nothing left to chain" >&2
        fi
        exit 1
    fi
    src="$dst"
done

echo
echo "Wrote $OUT"
if [ -s "$OUT" ]; then
    echo
    python3 tools/E555_rank.py "$OUT" --seed_file "$SEED" --top "$TOP"
fi
