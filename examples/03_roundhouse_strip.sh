#!/bin/bash
##SBATCH --job-name=E555_roundhouse
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=4G --time=12:00:00
##SBATCH --output=logs/roundhouse_%j.out
#
# =============================================================================
# 03_roundhouse_strip.sh -- rotate the board and refill a border strip
# =============================================================================
# WHAT IT DOES
#   Rotates the board 90 degrees and grows a W-wide vertical STRIP instead of a
#   row, so every level is one chain lookup and the search frontier is only W
#   colors wide. Two things follow: it needs just the edge half of the chain
#   database (megabytes, seconds -- no 6.4 GB build, no --db_file), and the
#   relaxed problem is small enough to solve exactly, so the tool knows which
#   colorings can still finish the strip BEFORE trying any piece.
#
#   The search is EXHAUSTIVE and deterministic: no beam, no sampling, no random
#   seed. Finishing without a complete board is therefore a proof that none
#   exists for this cut -- unless a budget stopped it first, which the run says.
#
# WIDTH is the dial that matters
#   WIDTH=0 picks the narrowest width whose kept region is complete and
#   break-free -- on a board filled in whole rows that is 16 minus the filled
#   rows. A higher WIDTH frees already-solved rows on purpose:
#
#      WIDTH   pieces kept at ROUNDS = 1 / 2 / 3     needs rows filled
#        3          208  /  169  /  130               0..12
#        4          192  /  144  /   96               0..11
#        5          176  /  121  /   66               0..10
#
# ROUNDS frees -- and refills -- that many W-wide bands: right, then top, then
#   left. The cuts nest, so a lower ROUNDS is a cheaper experiment, not a
#   truncated one. Work upward: 1, then 2, then 3.
#
#   COST. Every cut keeping 96 pieces or more exhausts in seconds. The one wide
#   cut is ROUNDS=3 WIDTH=5 (a 66-piece core): round 1 alone has hundreds of
#   thousands of break-free refills, so that one ends on MAX_WALL, and the
#   summary marks it TRUNCATED rather than proved.
#
# ROTATE picks which side of the ORIGINAL board each round attacks. Only the
#   KEPT region is validated, so aim the strip at where the breaks and holes are
#   and they are simply freed. Negative turns the other way (-1 == 3):
#
#      ROTATE   round 1   round 2   round 3   ROUNDS=1 leaves its hole at
#         0     right     top       left      top-right
#         1     top       left      bottom    top-left
#         2     left      bottom    right     bottom-left
#      3 / -1   bottom    right     top       bottom-right
#
#   ROTATE=1 (default) attacks a Stage B partial's unsolved top first, while the
#   piece pool is rich. ROTATE=-1 attacks it last but re-cuts the bottom band
#   first -- the band the pipeline fixes by random sampling at row 0 and never
#   revisits.
#
# WHAT YOU GET BACK
#   ONE board per input board: the furthest the search got, measured in pieces
#   placed. Tagged "s" when it is complete and break-free (the puzzle solved) and
#   "d" otherwise. TIES=N widens that to N boards that reached the same depth,
#   keeping only those differing by more than one frontier piece.
#
#   Emitting nothing is also a result: the oracle refuted every possible start,
#   which the run reports as "REFUTED ... colour alone rules this band out". The
#   oracle ignores the piece supply, so that holds for ANY arrangement.
#
#   READING THE SCORE. A "d" board is break-free: nothing here ever places a
#   piece against a colour it does not match. So its score is 480 minus the
#   junctions its EMPTY cells leave open, and a 191-piece board scoring 350 is
#   perfectly matched, not damaged. Compare a partial with a partial, never with
#   the complete board you fed in.
#
# BREAKS=B -- WHEN YOU WANT ALL 256 PIECES DOWN
#   With BREAKS>0 the run then greedily fills the rest of that deepest board,
#   spending at most B mismatches, and emits the complete result tagged "f".
#   That is a dive, not a search: it does not backtrack and B is not proved
#   minimal. It exists so Stage C gets a full board to attack break by break
#   instead of a hole. On the real seed, expect roughly one break per two cells
#   it has to fill.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---- settings ---------------------------------------------------------------
SEED="${SEED:-$REPO/data/seed_Edge5.txt}"
PARTIALS="${PARTIALS:-$REPO/data/board_partial_row12.csv}"
OUT_DIR="${OUT_DIR:-round_out}"
ROUNDS="${ROUNDS:-1}"             # 1, 2 or 3 bands
WIDTH="${WIDTH:-5}"               # 2..5 (chain length)
ROTATE="${ROTATE:-1}"             # -3..3, see the table above
TIES="${TIES:-1}"                 # boards to emit at the deepest reach
BREAKS="${BREAKS:-0}"             # >0: also emit a complete board bought with breaks
FIRST_LINE="${FIRST_LINE:-0}"
N_LINES="${N_LINES:-1}"
MAX_WALL="${MAX_WALL:-600}"       # seconds, 0 = unlimited
CLUES="${CLUES:-0}"                # 1 = hold the published Eternity II clue
                                  # pieces on their cells and spins
# -----------------------------------------------------------------------------

[ -x "$REPO/bin/E555_roundhouse" ] || make -C "$REPO" roundhouse

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)

"$REPO/bin/E555_roundhouse" "$SEED" "$PARTIALS" "${CLUE_ARG[@]}" \
    --border_row "$FIRST_LINE" \
    --border_row_N "$N_LINES" \
    --rounds "$ROUNDS" \
    --strip_width "$WIDTH" \
    --rotate "$ROTATE" \
    --ties "$TIES" \
    --max_breaks "$BREAKS" \
    --max_wall_sec "$MAX_WALL" \
    --out_dir "$OUT_DIR" \
    --verbose

# The tool names its files after the geometry and splits break-free boards from
# break-bought ones, so glob rather than rebuild the names.
RESULT=$(ls "$OUT_DIR"/roundhouse_*.csv 2>/dev/null)
echo
if [ -n "$RESULT" ]; then
    for f in $RESULT; do
        echo "Boards -> $f"
        echo "  python3 $REPO/tools/E555_rank.py $f --seed $SEED --top 10"
    done
else
    echo "Nothing emitted. If the log says REFUTED that is a PROOF that this"
    echo "board cannot be refilled this way without breaks -- try a different"
    echo "ROTATE, a larger WIDTH to free more pieces, or BREAKS=N for a complete"
    echo "board bought with mismatches."
fi
