#!/bin/bash
# topper_sweep.sh -- run a plan of topper passes over one slice of a board file.
#
#   bash pipeline/topper_sweep.sh INPUT=boards.csv
#   bash pipeline/topper_sweep.sh INPUT=boards.csv PRESET=deep OUT=deep.csv
#   bash pipeline/topper_sweep.sh INPUT=boards.csv FIRST_ROW=500 NUM_ROWS=500
#
# A PRESET is a sequence of topper passes. Each pass opens a band on one SIDE,
# WINDOW rows deep, with LOCKED of its outermost rows left unset so the pass may
# spend breaks there. A pass with LOCKED=0 closes the group: what went in and
# what came out are ranked together and the best BEAM_WIDTH kept, which is what
# makes a group monotone -- it can never return worse boards than it was given.
#
# FIRST_ROW/NUM_ROWS cut this run's slice of INPUT, so several runs can share
# one file. Under Slurm, pass FIRST_ROW=$SLURM_ARRAY_TASK_ID*NUM_ROWS from the
# submit script; this script knows nothing about schedulers.
#
# What each preset is for: pipeline/README.md
set -uo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
INPUT=                                  # board CSV to sweep -- required
OUT=                                    # result CSV; empty = OUT_DIR/topped.csv
OUT_DIR=topper_out
PRESET=safe             # closeT closeB closeR closeL safe window deep
FIRST_ROW=0             # first board row of INPUT this run takes
NUM_ROWS=500            # how many rows
WORKERS=12              # CP-SAT threads
MAX_TIME=50             # seconds per board per pass
STALL_TIME=20           # give up after this long with no gain
BEAM=1                  # --report_best per board
BEAM_DIFF=4             # cells by which beam ranks must differ
BEAM_SLACK=1            # extra breaks a lower rank may cost
CLUES=0                 # 1 = hold the Eternity II clue pieces in place
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
[ -n "$INPUT" ] || { echo "set INPUT=<board csv>" >&2; exit 1; }
case "$INPUT" in /*) ;; *) INPUT="$PWD/$INPUT" ;; esac
[ -n "$OUT" ] && case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
cd "$REPO"
[ -d bin ] && [ -d tools ] ||
    { echo "REPO=$REPO is not an E555 checkout -- set REPO at the top" >&2; exit 1; }

TOPPER=src/C_tail/E555_topper.py
RANK=tools/E555_rank.py
BEAM_WIDTH=$((NUM_ROWS * BEAM))         # boards carried into the next pass
CLUE_ARG=(); [ "$CLUES" = 1 ] && CLUE_ARG=(--clue_center --clue_corners)
mkdir -p "$OUT_DIR"
[ -n "$OUT" ] || OUT="$OUT_DIR/topped.csv"

echo
echo "=== E555 topper sweep ==="
echo "[cfg] seed=$SEED  input=$INPUT"
echo "[cfg] preset=$PRESET  rows $FIRST_ROW..$((FIRST_ROW + NUM_ROWS - 1))  out=$OUT"
echo "[cfg] workers=$WORKERS max_time=${MAX_TIME}s stall_time=${STALL_TIME}s"
echo "[cfg] beam=$BEAM diff=$BEAM_DIFF slack=$BEAM_SLACK"
date

# One topper pass, then -- when LOCKED is 0 -- the prune that closes the group.
# Reads $CUR, or this run's slice of INPUT when nothing has run yet.
run_pass() {  # SIDE WINDOW LOCKED
    local side=$1 window=$2 locked=$3
    STEP=$((STEP + 1)); LAST_LOCKED=$locked
    local out="$OUT_DIR/p${STEP}.csv" in="$CUR"
    local slice=(--row 0 --count "$BEAM_WIDTH")
    [ -z "$in" ] && { in="$ENTRY"; }

    echo
    echo ">> pass $STEP: --side $side, $window deep, $locked outermost unset"
    python3 "$TOPPER" "$SEED" "$in" "$out" \
        --side "$side" --work-rows "$window" --unused_rows "$locked" \
        --workers "$WORKERS" --max-time "$MAX_TIME" --stall-time "$STALL_TIME" \
        --report_best "$BEAM" --beam_diff "$BEAM_DIFF" --beam_slack "$BEAM_SLACK" \
        "${CLUE_ARG[@]}" "${slice[@]}" | tee "$OUT_DIR/p${STEP}.log"
    CUR="$out"

    # A board the topper calls "clean" had no break in the open band; one it
    # calls "optimal" is provably the best that band can hold. A pass where
    # every board is one or the other cannot improve anything however long it
    # is given, so count them: the summary can then say when the whole plan has
    # run out of moves, instead of leaving you to rerun it and find out.
    local boards settled
    boards=$(grep -c "Final Score:" "$OUT_DIR/p${STEP}.log")
    settled=$(grep -c "(clean)\|(optimal)" "$OUT_DIR/p${STEP}.log")
    [ "$boards" -gt 0 ] && [ "$settled" -eq "$boards" ] && NOOP=$((NOOP + 1))

    # LOCKED=0 ends a group: rank what came out together with what went in and
    # keep the best. rank.py sorts on breaks first and, among equals, on how few
    # ROWS those breaks occupy -- the more compact board is the one the next
    # pass can finish.
    if [ "$locked" -eq 0 ]; then
        local pruned="$OUT_DIR/g${STEP}.csv"
        echo "  [prune] group in + group out -> best $BEAM_WIDTH by breaks, then compactness"
        python3 "$RANK" "$GROUP_IN" "$CUR" --seed "$SEED" \
            --sort breaks,break_rows --top "$BEAM_WIDTH" --emit "$pruned" | tail -2
        CUR="$pruned"; GROUP_IN="$pruned"
    fi
    return 0
}

# The plans. One line per pass, so a plan can be read, copied and edited without
# a parser standing in the way.
plan_closeT() { run_pass T 6 2; run_pass T 4 0; }
plan_closeB() { run_pass B 6 2; run_pass B 4 0; }
plan_closeR() { run_pass R 6 2; run_pass R 4 0; }
plan_closeL() { run_pass L 6 2; run_pass L 4 0; }
plan_safe()   { run_pass T 5 0; run_pass TR 3 0; run_pass TL 3 0
                run_pass TB 3 0; run_pass R 3 0; run_pass L 3 0; run_pass B 3 0; }
plan_window() { run_pass T 8 3; run_pass T 6 2; run_pass T 5 1
                run_pass T 4 0; run_pass TR 4 0; run_pass L 4 0; }
plan_deep()   { run_pass T 7 3; run_pass T 4 0; run_pass TR 5 2; run_pass TR 3 0
                run_pass TL 5 2; run_pass TL 3 0; run_pass TB 5 2; run_pass TB 3 0
                run_pass R 5 2;  run_pass R 3 0;  run_pass L 5 2;  run_pass L 3 0
                run_pass B 5 2;  run_pass B 3 0; }

declare -F "plan_$PRESET" > /dev/null ||
    { echo "[ERROR] unknown PRESET '$PRESET'"
      echo "        (closeT closeB closeR closeL safe window deep)"; exit 1; }

# The board that ENTERS a group is what the prune at the end of that group ranks
# against the group's output, and that is the whole of what makes a group
# monotone. For the FIRST group that board is this run's slice of INPUT, so
# materialize it: rank.py takes files, not row ranges. The range is the one the
# topper's own --row/--count takes -- BOARD rows, skipping comment and blank
# lines -- so both tools cut the file at the same place.
ENTRY="$OUT_DIR/entry.csv"
awk -v a="$FIRST_ROW" -v n="$NUM_ROWS" \
    '!/^[ \t]*[#%]/ && NF { if (++b > a && b <= a + n) print }' "$INPUT" > "$ENTRY"
[ -s "$ENTRY" ] ||
    { echo "[ERROR] $INPUT has no rows at $FIRST_ROW..$((FIRST_ROW + NUM_ROWS - 1))"; exit 1; }

STEP=0; CUR=""; GROUP_IN="$ENTRY"; NOOP=0; LAST_LOCKED=0
"plan_$PRESET"

mv "$CUR" "$OUT"
rm -f "$OUT_DIR"/p*.csv "$OUT_DIR"/p*.log "$OUT_DIR"/g*.csv "$ENTRY"

echo
echo "=== run summary ==="
[ "$LAST_LOCKED" -eq 0 ] || echo "[warn] the plan ends on a LOCKED>0 pass, so these boards"\
    "still have the hole it opened. Add a SIDE WINDOW 0 pass to fill it."
[ "$NOOP" -eq "$STEP" ] && echo "[sum] plan exhausted: every pass found its band"\
    "already clean or provably optimal, so no amount of extra time moves these"\
    "boards. The documented next step is a plan that spends breaks to reach into"\
    "the core -- PRESET=deep."
echo "[sum] $STEP pass(es) -> $OUT"
python3 "$RANK" "$OUT" --seed "$SEED" --top 5
date
