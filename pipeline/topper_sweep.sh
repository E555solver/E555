#!/bin/bash
#SBATCH --job-name=E555_topsweep
#SBATCH --output=logs/topsweep_%A_%a.out
#SBATCH --error=logs/topsweep_%A_%a.err
#SBATCH --array=0-129
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=12
#SBATCH --hint=nomultithread
#SBATCH --time=18:00:00
#SBATCH --mem=6G
#
# =============================================================================
# topper_sweep.sh -- drive E555_topper.py through a list of passes
# =============================================================================
# One script for every topper sweep. What it does is decided entirely by PLAN,
# a space-separated list of passes, each written
#
#       SIDE:WINDOW:LOCKED
#
#   SIDE     which border band opens: T B L R, or an L-shaped / opposite pair
#            TR TL BR BL TB LR.
#   WINDOW   how deep that band is, in rows (T/B) or columns (L/R).
#   LOCKED   how many of its OUTERMOST rows/columns to unset and hold empty.
#
# LOCKED is the whole point of the deep sweeps. It does not merely freeze those
# cells: it UNSETS them, lifting their pieces back into the pool. The solver
# then rebuilds the INNER part of the window with far more material than cells
# -- that is how you reach pieces buried in the core -- and it hands back a
# board with a hole and a HIGHER break count. That is not a failure. A later
# pass with LOCKED=0 fills the hole back in and the count comes down.
#
# So the passes group themselves: everything up to and including the next
# LOCKED=0 pass is one group, and the sweep only prunes at the end of a group.
# Never prune in the middle of one -- you would be ranking boards that were
# deliberately taken apart.
#
# A group is also MONOTONIC: the prune ranks the board that ENTERED the group
# alongside what came out, so a group that runs out of time can never make the
# board worse. Delete "$GROUP_IN" from the rank call if you want to let a group
# spend breaks to escape a local optimum.
#
# PRESETS (used when PLAN is empty; PLAN always wins)
#
#   closeT  T:6:2 T:4:0     ("close" on its own means closeT)
#   closeB  B:6:2 B:4:0
#   closeR  R:6:2 R:4:0
#   closeL  L:6:2 L:4:0
#           Two passes on ONE named side, for a board whose hole or breaks sit
#           against that border. The first pass reaches 6 deep while holding the
#           outer 2 rows empty, so it can pull pieces out of the core; the
#           second fills everything back in. Measured on a real board: 26 breaks
#           either way against a single 4-deep pass, but with the border intact
#           and the damage folded into 3 rows instead of 5. The letter also ends
#           up in the output filename, so a directory of results says which side
#           each board was closed on.
#
#           closeT is the family default: a partial grown from the bottom
#           carries its breaks at the top. After an E555_roundhouse run the hole
#           is in the band of its LAST round instead:
#
#               roundhouse --rotate 1 (its default) -> closeB
#                          --rotate 2               -> closeR
#                          --rotate 3 (= -1)        -> closeT
#                          --rotate 0               -> closeL
#
#           For L and R the numbers are COLUMNS, not rows. The depths are a
#           starting point: a roundhouse run that stalls a round early leaves a
#           deeper, L-shaped hole -- measured boxes reached rows 0..8. Read the
#           box off its [emit] line and widen, e.g. PLAN="B:9:3 B:7:0".
#
#   safe    one 5-deep pass on T, then 3-deep on TR TL TB R L B, nothing ever
#           locked. No pass can make a board worse. Start here.
#
#   window  T:8:3 T:6:2 T:5:1 T:4:0 then TR:4:0 L:4:0. A window sliding up the
#           board, then two clean-ups on borders it never reached.
#
#   deep    every side as a pair: a wide pass with its outer band emptied, then
#           a narrow pass that refills it. The most expensive option, for when
#           the safe sweep has run out of moves.
#
# TIME. Every pass gets the same MAX_TIME. A deeper WINDOW is a much bigger
# CP-SAT model and needs more of it -- 4 rows of a 16-wide band is around 64
# free cells and finishes in minutes, 6 rows is 96 and can take an hour. If you
# want a long run, raise MAX_TIME for the whole sweep; if you want one pass to
# get more than the others, split the PLAN across two runs of this script.
#
# IF YOUR BOARD HAS EMPTY CELLS, the last LOCKED=0 pass must open every one of
# them. Any unplaced piece joins the solver's pool whether or not its cell is
# open, so a window that misses part of the hole leaves pieces with nowhere to
# go and wastes the pass. E555_roundhouse prints the hole box on its [emit]
# line ("60 empty in rows 12..15 x cols 0..14") -- read the side and the depth
# straight off it.
#
# Runs unchanged under Slurm (each array task takes its own NUM_ROWS-row slice)
# or as plain bash (no Slurm -> task 0, the first slice).
#
#   INPUT=boards.csv PRESET=closeT bash pipeline/topper_sweep.sh
#   INPUT=boards.csv PLAN="T:8:3 T:5:1 T:4:0" MAX_TIME=3600 bash pipeline/topper_sweep.sh
# =============================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOPPER="$REPO/src/C_tail/E555_topper.py"
RANK="$REPO/tools/E555_rank.py"

# ---- settings ---------------------------------------------------------------
PIECE_SEED="${PIECE_SEED:-$REPO/data/seed_Edge5.txt}"
INPUT="${INPUT:?set INPUT=<board csv>}"
PRESET="${PRESET:-safe}"          # close | safe | window | deep
PLAN="${PLAN:-}"                  # overrides PRESET; SIDE:WINDOW:LOCKED ...
OUT_DIR="${OUT_DIR:-topper_out}"
WORKERS="${WORKERS:-12}"          # CP-SAT threads
MAX_TIME="${MAX_TIME:-50}"        # seconds per board per pass
STALL_TIME="${STALL_TIME:-20}"    # give up after this long with no gain
NUM_ROWS="${NUM_ROWS:-500}"       # input rows per array task
BEAM="${BEAM:-1}"                 # --report_best per board
BEAM_DIFF="${BEAM_DIFF:-4}"       # cells by which beam ranks must differ
BEAM_SLACK="${BEAM_SLACK:-1}"     # extra breaks a lower rank may cost
EXTRA="${EXTRA:-}"                # appended to every topper call, e.g. --verbose
CLUES="${CLUES:-0}"               # 1 = hold the Eternity II clue pieces in place

CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
# -----------------------------------------------------------------------------

case "$PRESET" in
    close|closeT) DEFAULT_PLAN="T:6:2 T:4:0" ;;
    closeB) DEFAULT_PLAN="B:6:2 B:4:0" ;;
    closeR) DEFAULT_PLAN="R:6:2 R:4:0" ;;
    closeL) DEFAULT_PLAN="L:6:2 L:4:0" ;;
    safe)   DEFAULT_PLAN="T:5:0 TR:3:0 TL:3:0 TB:3:0 R:3:0 L:3:0 B:3:0" ;;
    window) DEFAULT_PLAN="T:8:3 T:6:2 T:5:1 T:4:0 TR:4:0 L:4:0" ;;
    deep)   DEFAULT_PLAN="T:7:3 T:4:0 TR:5:2 TR:3:0 TL:5:2 TL:3:0 TB:5:2 TB:3:0"
            DEFAULT_PLAN="$DEFAULT_PLAN R:5:2 R:3:0 L:5:2 L:3:0 B:5:2 B:3:0" ;;
    *)      echo "[ERROR] unknown PRESET '$PRESET'"
            echo "        (closeT closeB closeR closeL safe window deep)"; exit 1 ;;
esac
[ -n "$PLAN" ] || PLAN="$DEFAULT_PLAN"

BEAM_WIDTH=$((NUM_ROWS * BEAM))   # boards carried into the next pass
TASK_ID="${SLURM_ARRAY_TASK_ID:-0}"
FIRST_ROW=$((TASK_ID * NUM_ROWS))
TAG="${PRESET}_${FIRST_ROW}"
N_PASSES=$(echo $PLAN | wc -w)
mkdir -p logs "$OUT_DIR"

echo
echo "=== E555 topper sweep ==="
echo
echo "[cfg] seed=$PIECE_SEED"
echo "[cfg] input=$INPUT  out_dir=$OUT_DIR"
echo "[cfg] plan=$PLAN   ($N_PASSES pass(es), preset '$PRESET')"
echo "[cfg] workers=$WORKERS max_time=${MAX_TIME}s stall_time=${STALL_TIME}s extra='$EXTRA'"
echo "[cfg] task=$TASK_ID rows $FIRST_ROW..$((FIRST_ROW + NUM_ROWS - 1))  beam=$BEAM diff=$BEAM_DIFF slack=$BEAM_SLACK"
date

# One topper pass. Reads $CUR, or the INPUT slice when nothing has run yet.
run_pass() {  # side window locked
    local side=$1 window=$2 locked=$3
    STEP=$((STEP + 1))
    local out="$OUT_DIR/${TAG}_p${STEP}.csv" in="$CUR"
    local slice=(--row 0 --count "$BEAM_WIDTH")
    [ -z "$in" ] && { in="$INPUT"; slice=(--row "$FIRST_ROW" --count "$NUM_ROWS"); }

    echo
    echo ">> pass $STEP/$N_PASSES: --side $side, $window deep, $locked outermost unset"
    python3 "$TOPPER" "$PIECE_SEED" "$in" "$out" \
        --side "$side" --work-rows "$window" --unused_rows "$locked" \
        --workers "$WORKERS" --max-time "$MAX_TIME" --stall-time "$STALL_TIME" \
        --report_best "$BEAM" --beam_diff "$BEAM_DIFF" --beam_slack "$BEAM_SLACK" \
        "${CLUE_ARG[@]}" "${slice[@]}" $EXTRA | tee "$OUT_DIR/${TAG}_p${STEP}.log"
    CUR="$out"

    # A board the topper calls "clean" had no break in the open band; one it
    # calls "optimal" is provably the best that band can hold. A pass where
    # every board is one or the other cannot improve anything however long it
    # is given, so count them: the summary can then say when the whole plan has
    # run out of moves, instead of leaving you to rerun it and find out.
    local boards settled
    boards=$(grep -c "Final Score:" "$OUT_DIR/${TAG}_p${STEP}.log" || true)
    settled=$(grep -c "(clean)\|(optimal)" "$OUT_DIR/${TAG}_p${STEP}.log" || true)
    [ "$boards" -gt 0 ] && [ "$settled" -eq "$boards" ] && NOOP=$((NOOP + 1))
    return 0
}

# End of a group: rank what came out together with what went in, keep the best.
# rank.py sorts on breaks first and, among equals, on how few ROWS those breaks
# occupy -- the more compact board is the one the next pass can finish.
close_group() {
    local pruned="$OUT_DIR/${TAG}_g${STEP}.csv"
    echo "  [prune] $(basename "$GROUP_IN") (went in) + $(basename "$CUR") (came out)" \
         "-> best $BEAM_WIDTH by breaks, then compactness"
    python3 "$RANK" "$GROUP_IN" "$CUR" --seed "$PIECE_SEED" \
        --sort breaks,break_rows --top "$BEAM_WIDTH" --emit "$pruned" | tail -2
    CUR="$pruned"; GROUP_IN="$pruned"
}

# The board that ENTERS a group is what the prune at the end of that group ranks
# against the group's output, and that is the whole of what makes a group
# monotone. For the FIRST group that board is this task's slice of INPUT, so
# materialize it: rank.py takes files, not row ranges, and handing it the whole
# INPUT would drag in the slices belonging to the other array tasks. The range
# is the one the topper's own --row/--count takes: BOARD rows, skipping the
# comment and blank lines (bin/E555_backtracker writes a '#' header), so both
# tools cut the file at the same place.
ENTRY="$OUT_DIR/${TAG}_in.csv"
awk -v a="$FIRST_ROW" -v n="$NUM_ROWS" \
    '!/^[ \t]*[#%]/ && NF { if (++b > a && b <= a + n) print }' "$INPUT" > "$ENTRY"
[ -s "$ENTRY" ] || { echo "[ERROR] $INPUT has no rows at $FIRST_ROW..$((FIRST_ROW + NUM_ROWS - 1))"; exit 1; }

STEP=0; CUR=""; GROUP_IN="$ENTRY"; NOOP=0; LAST_LOCKED=0
for spec in $PLAN; do
    IFS=: read -r side window locked <<<"$spec"
    case "$side:$window:$locked" in
        [TBLR]*:[0-9]*:[0-9]*) ;;
        *) echo "[ERROR] bad plan entry '$spec', want SIDE:WINDOW:LOCKED"; exit 1 ;;
    esac
    run_pass "$side" "$window" "$locked"
    LAST_LOCKED=$locked
    [ "$locked" -eq 0 ] && close_group
done

FINAL="$OUT_DIR/topped_${TAG}.csv"
mv "$CUR" "$FINAL"
rm -f "$OUT_DIR/${TAG}"_p*.csv "$OUT_DIR/${TAG}"_p*.log \
      "$OUT_DIR/${TAG}"_g*.csv "$ENTRY"

echo
echo "=== run summary ==="
[ "$LAST_LOCKED" -eq 0 ] || echo "[warn] the plan ends on a LOCKED>0 pass, so these boards"\
    "still have the hole it opened. Add a SIDE:WINDOW:0 pass to fill it."
[ "$NOOP" -eq "$N_PASSES" ] && echo "[sum] plan exhausted: every pass found its band"\
    "already clean or provably optimal, so no amount of extra time moves these"\
    "boards. The documented next step is a plan that spends breaks to reach into"\
    "the core -- PRESET=deep."
echo "[sum] $N_PASSES pass(es) -> $FINAL"
python3 "$RANK" "$FINAL" --seed "$PIECE_SEED" --top 5
date
