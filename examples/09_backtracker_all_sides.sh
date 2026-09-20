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
#   1  --order spiral                 outer ring in     min(r,15-r,c,15-c)
#   2  --order rowmajor               bottom, L->R      r*16 + c
#   3  --order rowmajor --reverse     same rows, R->L   r*16 + (15-c)
#   4  --order colmajor               left              c*16 + r
#   5  --order colmajor --reverse     right, downward   (15-c)*16 + (15-r)
#   6  --order 4sides    FOURSIDES=1  all four sides    literal side sequence
#
# Passes 1..5 are exhaustive EXACT completion attempts: each either closes the
# board, proves no zero-break completion exists from its starting board, or
# times out, and the board it emits is the deepest node it reached.
#
# spiral goes first because it is the only order here that leads from the top,
# and because it is the strongest: no LINEAR order can lead from the top at all.
# Every one keys on r ascending or on c, and --reverse on rowmajor only flips
# the within-row walk, so 2..5 lead from the bottom, bottom, left and right.
# spiral keys on the ring distance to the nearest border, so the whole outer
# ring -- top row included -- comes before anything inward. It also earns the
# place on results: see MEASURED below. Since commitments are permanent, the
# strongest pass should be the one that commits first.
#
# EXPECT MOST PASSES TO ADD NOTHING, AND KNOW WHY. A hard exact pass cannot skip
# a cell: at --breaks 0 it walks its own order, and the first empty cell that no
# piece fits exactly kills the search there and then -- children=0 at depth 0
# means the pass ends having added zero pieces. Worse, if the remaining-piece
# relaxation can already prove no break-free completion exists, the root gate
# refuses to start at all and prints ROOT-INFEASIBLE.
#
# The first pass takes the cells that could be filled exactly. What it leaves is
# precisely the set of cells nothing fits -- so the orders after it walk straight
# into those and stop. Measured on the shipped default, per pass:
#     spiral +18, rowmajor +0, rowmajor --reverse +0, colmajor +0,
#     colmajor --reverse +1, 4sides +17
# That is not a malfunction and it is not this script mis-wiring the tool; it is
# what an exhaustive exact search means. The chain still earns its keep when a
# pass TIMES OUT, because then the order decides which part of the space was
# explored and the next order explores elsewhere. On a board small enough to
# exhaust in milliseconds, it cannot.
#
# WHAT DOES GROW SUCH A BOARD is a SOFT pass, which steps over a dead cell
# instead of stopping at it. 4sides is the only soft order here (--jump is the
# other soft mode, and is deliberately not used). So a run where most passes add
# nothing and FOURSIDES=0 has switched off the one pass that could have helped:
# the script says so at the end, and re-running with FOURSIDES=1 is the answer.
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
#   instead of forcing a backtrack. Nothing ever refutes a subtree, so the only
#   way it finishes is by exhausting its sweep along every branch -- and that
#   cost grows with the number of open cells it can still put something in. It
#   is the one pass that can run out the clock, and how badly depends entirely
#   on how full the board already is. On the shipped default, measured:
#     48 cells open (raw input)      20 s, timed out, reaching 242 pieces
#     29 cells open (after 1..5)     0.12 s, space exhausted, reaching 245
#   The other five passes together finish in well under a second either way,
#   so on a board with room 4sides alone sets the wall clock -- which is what
#   a first run should not have to pay for. It does buy real pieces, because
#   stepping over a dead cell is exactly how you grow past one, so it is the
#   best-partial finisher and belongs last, after the exact passes have filled
#   what they can. Turn it on when you want the biggest break-free partial:
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
#                     with score, 435 down to 364 here.
#
#                     PAIR CLUES=1 WITH FOURSIDES=1. The freed cells are exactly
#                     the ones no piece fits exactly any more, so all five exact
#                     passes add nothing: measured, the clue step drops 208 to
#                     193 pieces, passes 2..6 add zero between them, and 4sides
#                     alone brings it back to 224. Without it a clue run just
#                     hands you a board 15 pieces poorer.
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
# SHORT_ID=1 trims the config_id of the FINAL file. Each pass appends the
# board's connected-edge count to the id it was given, so five passes turn
# r0c669 into r0c669_0_390_395_395_395 and only the last number describes what
# is in the file. The trim drops the components the chain added and keeps the
# final one, leaving r0c669_395. Set SHORT_ID=0 to keep the full trail, which
# records what each pass reached. Either way the intermediates carry the long
# form, so the config_id length guard below still applies to long input ids.
#
# THE MASK APPLIES ONCE, to the first step only. --holes REOPENS cells, so
# giving it to every pass would re-empty that region each time and discard
# whatever the previous pass had put there.
#
# MEASURED, on data/board_partial_row12.csv (208 pieces in), TIME_LIMIT=20,
# 4 threads, ARCH=generic:
#   --order spiral alone         226 placed, score 405, 0.003 s, space exhausted
#   passes 1..5 chained          227 placed, score 406, under 1 s in total
#   passes 1..6, FOURSIDES=1     244 placed, score 435, still under a second
#   CLUES=1, 22-cell mask, 1..6  224 placed, score 364, 5 of 5 clues
#
# The order of PASSES is worth more than it looks. Leading with rowmajor instead
# of spiral gave 214 pieces on this board -- WORSE than a single spiral pass --
# because commitments are permanent and spiral then inherited a board it could
# not use as well as the original. Leading with the strongest pass instead has
# it commit first, and the chain now beats it. The corollary: if you change
# PASSES, put the order you trust most at the front, not at the end.
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
SHORT_ID=1              # 1 = trim the chained config_id down to base_<final>
TOP=5                   # rows in the closing rank.py report
QUIET=1                 # 1 = hide the backtracker's own output; 0 = show it
PASSES="--order spiral|--order rowmajor|--order rowmajor --reverse|--order colmajor|--order colmajor --reverse"
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

# Placed pieces over every record in a file. Readers take the LAST 512 fields as
# pos+rot, so pos is fields NF-511..NF-256 and 999 means unplaced -- the same
# rule every tool in the repo uses, and it survives the 514/515-field variants.
# This is what makes a no-op pass visible: without it a pass that added nothing
# still reports "1 board(s)" and looks like it worked.
count_placed() {
    awk -F, '/^#/ { next }
             NF >= 512 { for (i = NF - 511; i <= NF - 256; i++) if ($i != 999) n++ }
             END { print n + 0 }' "$1" 2>/dev/null || echo 0
}

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
before=$(count_placed "$BOARDS")   # so pass 1 reports a delta too
stalled=0                          # passes that added nothing: see the note below
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
    now=$(count_placed "$dst")
    printf '%s board(s), %d placed (%+d), %ds\n' \
           "$rows" "$now" "$((now - before))" "$((SECONDS - t0))"
    # Step 1 with a mask legitimately ENDS with fewer pieces than it started --
    # --holes reopens cells -- so it is not a stalled pass and must not be
    # counted as one, or a clue run would always accuse itself.
    if [ "$now" -le "$before" ] && { [ "$n" -gt 1 ] || [ -z "$HOLES" ]; }; then
        stalled=$((stalled + 1))
    fi
    before=$now
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

# Every pass appends _<connected-edge count> to the config_id it was handed, so
# a chain leaves r0c669_0_390_395_395_395 where only the last number says
# anything about this board. The chain added exactly NSTEP components -- one per
# step, including for a record a pass dropped and wrote through unchanged, which
# is why the count is NSTEP and not "however many the searches emitted" -- so
# dropping the last NSTEP and putting the final one back is exact, and awk does
# it in one pass. A row carrying fewer components than that was not written by
# this chain and is left alone. The trim is the LAST thing done: the
# intermediates keep their full ids, so the length guard above still applies.
if [ "$SHORT_ID" = "1" ] && [ -s "$OUT" ]; then
    awk -v n="$NSTEP" -F, 'BEGIN { OFS = "," }
        /^#/     { print; next }
        NF < 514 { print; next }
        { k = split($1, a, "_")
          if (k <= n) { print; next }
          id = a[1]
          for (i = 2; i <= k - n; i++) id = id "_" a[i]
          $1 = id "_" a[k]
          print }' "$OUT" > "$OUT.pass_short" && mv "$OUT.pass_short" "$OUT"
fi

echo
echo "Wrote $OUT"
if [ -s "$OUT" ]; then
    echo
    python3 tools/E555_rank.py "$OUT" --seed_file "$SEED" --top "$TOP"
fi

# A pass that adds nothing is normal here and is explained at the top, but it is
# invisible unless someone reads the tool's own output -- "1 board(s)" looks
# identical either way. Say it plainly, and name the one lever that changes it.
# A board with no empty cell left has nothing to add, so every pass "stalling"
# on it is the correct answer, not a diagnosis worth printing.
empty_left=$(( $(count_rows "$OUT") * 256 - $(count_placed "$OUT") ))
if [ "$stalled" -gt 0 ] && [ "$empty_left" -gt 0 ]; then
    echo
    echo "Note: $stalled of $NSTEP pass(es) added nothing."
    echo "  A hard exact pass cannot skip a cell no piece fits: it stops at the first"
    echo "  one in its order, or is refused at the root (ROOT-INFEASIBLE) when no"
    echo "  break-free completion exists at all. The first pass takes the exact fills,"
    echo "  so the later orders meet exactly those dead cells. This is the search"
    echo "  being exhaustive, not the chain being broken."
    if [ "$FOURSIDES" != "1" ]; then
        echo "  4sides is the only pass here that steps OVER a dead cell and keeps"
        echo "  growing. Re-run with FOURSIDES=1 to let it."
    fi
fi
