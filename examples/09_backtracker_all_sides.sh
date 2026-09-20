#!/bin/bash
# 09_backtracker_all_sides.sh -- one board, six exact passes, each from a
# different direction.
#
#   bash examples/09_backtracker_all_sides.sh
#   bash examples/09_backtracker_all_sides.sh BOARDS=stage_c_out/3_patched.csv
#   bash examples/09_backtracker_all_sides.sh OUT=run1.csv TIME_LIMIT=3600 QUIET=0
#
# The backtracker fills empty cells and never removes a piece, so one run
# commits to whatever its cell order reaches first. On a board that cannot be
# closed exactly, WHICH order you used decides how far you get -- and there is
# no way to ask for several orders in one invocation. This chains six runs,
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
#   6  --order 4sides                 all four sides    literal side sequence
#
# Passes 1..5 are exhaustive EXACT completion attempts: each either closes the
# board, proves no zero-break completion exists from its starting board, or
# times out, and the board it emits is the deepest node it reached. Pass 6 is
# the only best-partial pass -- side-growth modes defer a dead cell instead of
# backtracking and bypass the completion prunes, so 4sides belongs last, where
# it gets the fullest board and returns the biggest break-free partial.
#
# Pass 5 is why spiral is here at all: no LINEAR order leads from the top. Every
# one keys on r ascending or on c, and --reverse on rowmajor only flips the
# within-row walk, so 1..4 lead from the bottom, bottom, left and right. spiral
# keys on the ring distance to the nearest border, so the whole outer ring --
# top row included -- comes before anything inward.
#
# THE CHAIN EARNS ITS KEEP ON TIME-OUTS. A pass that runs to exhaustion has
# proved no completion exists from its board; later passes start from a strictly
# smaller space and mostly re-prove it in seconds. On an easy board passes 2..6
# are near no-ops. TIME_LIMIT is what turns this into a portfolio of six
# attempts rather than one search repeated.
#
# And commitments are permanent: nothing downstream can undo a piece an earlier
# pass placed, and the deepest node of an exact search can be a fluke of the
# traversal.
#
# FEED IT BREAK-FREE BOARDS. --breaks 0 DROPS any record that already carries a
# broken edge ("input has N broken edge(s) > --breaks 0") and writes it through
# unchanged, so a board with breaks survives all six passes untouched. The
# shipped default, data/board_partial_row12.csv, is break-free with rows 12..15
# still open -- 208 pieces in, 242 out in ten seconds a pass. Its sibling
# data/board_example_462.csv carries 18 breaks and is passed straight through.
#
# CLUES=1 -- force the published hint pieces on first
#   A filled board cannot simply take its clues: --breaks 0 drops any board whose
#   clue would land against a mismatching neighbour, and a clue cell already
#   holding another piece is a conflict that drops the board UNWRITTEN. On
#   data/board_partial_row12.csv all three of --clue_center, --clue_corners and
#   both together are refused outright, because rows 0..12 are full.
#
#   So CLUES=1 adds a step in front. It computes the MINIMAL set of cells that
#   have to be empty for every clue to land with zero breaks -- a clue whose four
#   orthogonal neighbours are all empty cannot break anything, since only placed
#   neighbours are ever broken against -- and passes that as the mask:
#     1. the clue cell, unless it already holds the right piece at the right spin;
#     2. each PLACED neighbour whose facing edge disagrees with the clue's edge;
#        neighbours that already agree are left alone;
#     3. the cell the clue piece is stranded on, if it is placed elsewhere.
#   On the shipped default that is 22 cells, and the run goes from 208 pieces and
#   0 clues to 224 pieces and 5 of 5, still with zero broken edges. You buy the
#   clues with score -- the freed cells do not all come back.
#
#   Two routes that do NOT work: a clue pass at --breaks N leaves those breaks on
#   the board permanently, so every later --breaks 0 pass drops it and no pass
#   can repair them. Cheapest of all is upstream --
#   bin/E555_beamer --clue_center --clue_corners builds the board with three of
#   the five already in place.
#
#   CLUE_ORIENT only matters for a board carrying no clue at all: one that
#   already holds a clue has committed, and the tool reads the orientation off it.
#   The clue table belongs to the real Eternity II seed, so CLUES=1 on
#   data/synth_seed.txt forces pieces into cells its solution does not use and
#   makes that board uncompletable -- it is a fixture for a different puzzle.
#
# THE MASK APPLIES ONCE, to the first step only. --holes REOPENS cells, so giving
# it to every pass would re-empty that region each time and discard whatever the
# previous pass had put there.
#
# Only $OUT is left behind: the per-pass files are deleted however the run ends.
# Measured, at ten seconds a pass: data/board_partial_row12.csv 208 -> 242 pieces
# break-free; the synthetic solution with rows 13..15 removed closes back to
# 480/480, and with rows 11..15 removed (80 pieces) it still closes.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/board_partial_row12.csv     # a break-free partial: what this suits
OUT=backtracked_all_sides.csv           # the only file this run leaves behind
HOLES=                                  # optional mask, passed to every pass
FIRST_LINE=0            # --start_row, first pass only
N_LINES=0               # --num_rows, first pass only; 0 = every record
THREADS=8
TIME_LIMIT=300          # seconds PER PASS, 0 = unlimited (six unbounded
                        # exhaustive searches will not finish -- set this)
CLUES=0                 # 1 = force the published hint pieces on first (see above)
CLUE_ORIENT=0           # which of the four orientations, for an unclued board
TOP=5                   # rows in the closing rank.py report
QUIET=1                 # 1 = hide the backtracker's own output; 0 = show it
PASSES="--order rowmajor|--order rowmajor --reverse|--order colmajor|--order colmajor --reverse|--order spiral|--order 4sides"
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

# The mask is applied EXACTLY ONCE, by the first step. --holes reopens cells, so
# handing it to every pass would re-empty that region each time and throw away
# what the previous pass put there -- the chain would only ever keep the last
# pass's work on those cells.
MASK="$HOLES"
if [ "$CLUES" = "1" ]; then
    # Force the hint pieces on before anything else. A clue whose four
    # orthogonal neighbours are all EMPTY creates zero breaks by construction,
    # and --breaks 0 drops any board that gains one, so this frees exactly what
    # it must: the clue cell, the placed neighbours whose facing edge disagrees
    # with the clue, and the cell the clue piece is stranded on. Anything already
    # correct is left alone. Whatever the caller put in HOLES is folded in.
    MASK="$OUT.pass0.mask.csv"
    python3 - "$SEED" "$BOARDS" "$CLUE_ORIENT" "$HOLES" > "$MASK" <<'PY'
import sys
sys.path.insert(0, "tools"); import E555_viewer as V
seed, boards, orient, extra = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
tiles = V.load_seed(seed)
free = set()
if extra:
    for i, v in enumerate(open(extra).read().replace("\n", ",").split(",")):
        if v.strip() and int(v):
            free.add(i)
for ln in open(boards):
    if ln.startswith("#") or not ln.strip():
        continue
    f = ln.strip().split(",")[-512:]
    pos = [int(x) for x in f[:256]]
    rot = [int(x) for x in f[256:]]
    at = {pos[p]: p for p in range(256) if pos[p] != 999}
    for cell, piece, spin in V.clue_list(orient):
        if pos[piece] == cell and rot[piece] == spin:
            continue                       # already right: free nothing
        free.add(cell)                     # must be empty for the clue to land
        e = V.rotate_edges(tiles[piece], spin)          # top,right,bottom,left
        r, c = divmod(cell, 16)
        for nb, mine, theirs, ok in ((cell + 16, 0, 2, r < 15),
                                     (cell + 1,  1, 3, c < 15),
                                     (cell - 16, 2, 0, r > 0),
                                     (cell - 1,  3, 1, c > 0)):
            if not ok or nb not in at:
                continue                   # off board, or already empty
            q = at[nb]
            if V.rotate_edges(tiles[q], rot[q])[theirs] != e[mine]:
                free.add(nb)               # this neighbour would break
        if pos[piece] != 999 and pos[piece] != cell:
            free.add(pos[piece])           # else the piece is "stranded"
    break                                  # the mask is built from row 0
print(",".join("1" if i in free else "0" for i in range(256)))
PY
    echo "  clues  : orientation $CLUE_ORIENT, mask frees $(tr ',' '\n' < "$MASK" | grep -c '^1$') cell(s)"
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
echo

src="$BOARDS"
for n in $(seq 1 "$NSTEP"); do
    step=()
    if [ "$CLUES" = "1" ] && [ "$n" -eq 1 ]; then
        # mrv is right for the clue pass: the freed cells are a handful of
        # scattered holes, and most-constrained-first is what fills those.
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
        hole=(); [ -n "$MASK" ] && hole=(--holes "$MASK")
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
    # message than this one. A clue pass that empties the file means the board
    # could not take its clues at all.
    [ "$rows" -gt 0 ] ||
        { echo "  pass $n emitted no board; nothing left to chain" >&2; exit 1; }
    src="$dst"
done

echo
echo "Wrote $OUT"
if [ -s "$OUT" ]; then
    echo
    python3 tools/E555_rank.py "$OUT" --seed_file "$SEED" --top "$TOP"
fi
