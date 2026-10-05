#!/bin/bash
# 11_diver_reopen.sh -- improve finished boards; never returns a worse one.
#
#   bash examples/11_diver_reopen.sh                    # the seven 463s, a minute each
#   bash examples/11_diver_reopen.sh BOARDS=best.csv SECONDS_PER_BOARD=600 THREADS=32
#
# Each board in turn gets SECONDS_PER_BOARD of wall clock, spent in rounds:
#   1. lift the damaged band (REOPEN=auto: the outer rows or columns of the
#      side that holds most of the broken edges, up to 5 deep);
#   2. refill it COPIES times, each copy END_DIVE greedy random dives and then
#      END_POLISH rounds of kick-and-polish local search on the best of them;
#   3. keep the best copy if it beats the board; the next round starts from
#      whichever is better.
# OUT gets one row per board, in the order of BOARDS, and the run ends with a
# before -> after table. The defaults were the best setting measured at 15 s
# and at 120 s per board, and the longer budget found more (PROJECT_E555.md,
# "E555_diver"): to do better, give it more SECONDS_PER_BOARD or THREADS.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/best_463.csv                # complete boards to improve
OUT=reopened.csv                        # the same boards, improved or as they were
SECONDS_PER_BOARD=60                    # wall clock per board; more finds more
THREADS=8                               # all the cores you can spare
RNG_SEED=1                              # another value: a fresh run on the same boards
# The search:
REOPEN=auto             # the band: auto, auto+E (E rows deeper), top:K,
                        # box:R0-R1,C0-C1, or a 16x16 0/1 mask file
COPIES=8                # refills per round
END_DIVE=1000           # dives per copy
END_POLISH=10000        # kick-and-polish rounds per copy
# -----------------------------------------------------------------------------
SETTINGS=" REPO SEED BOARDS OUT SECONDS_PER_BOARD THREADS RNG_SEED REOPEN COPIES END_DIVE END_POLISH "
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) [[ "$SETTINGS" == *" ${arg%%=*} "* ]] ||
                          { echo "unknown setting ${arg%%=*}; the settings are:$SETTINGS" >&2; exit 1; }
                      declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 1 ;;
    esac
done
cd "$REPO"
[ -d bin ] && [ -d tools ] ||
    { echo "REPO=$REPO is not an E555 checkout -- set REPO at the top" >&2; exit 1; }
[ -x bin/E555_diver ] || make diver
LOG="${OUT%.csv}.diver.log"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
: > "$OUT"; : > "$LOG"

# One diver run per board, so that each board gets its own wall clock.
python3 - "$BOARDS" "$TMP" <<'EOF'
import sys
rows = [l for l in open(sys.argv[1]) if l.strip() and l.lstrip()[0] not in "#%"]
for i, l in enumerate(rows):
    open(f"{sys.argv[2]}/board_{i:04d}.csv", "w").write(l if l.endswith("\n") else l + "\n")
EOF
N=$(find "$TMP" -name 'board_*.csv' | wc -l)
echo "$N board(s) from $BOARDS, $SECONDS_PER_BOARD s each on $THREADS thread(s)"
score() { python3 tools/E555_rank.py "$1" --seed_file "$SEED" --csv | awk -F, 'NR == 2 {print $4}'; }
i=0
for f in "$TMP"/board_*.csv; do
    i=$((i + 1))
    before=$(score "$f")
    bin/E555_diver "$SEED" "$f" "$f.out" --reopen "$REOPEN" --rounds 1000000 \
        --wall_time "$SECONDS_PER_BOARD" --copies "$COPIES" \
        --end_dive "$END_DIVE" --end_polish "$END_POLISH" --emit_score 0 \
        --threads "$THREADS" --rng_seed "$RNG_SEED" --print_cmd > "$f.log"
    cat "$f.log" >> "$LOG"
    cat "$f.out" >> "$OUT"
    dived=$(sed -n 's/^\[sum\] end dives: M=[0-9]* boards=\([0-9]*\).*/\1/p' "$f.log")
    echo "  board $i of $N: $before -> $(score "$f.out") in $((${dived:-0} / COPIES)) round(s)"
done
echo "$OUT" > "$OUT.outputs.txt"
echo "$LOG" >> "$OUT.outputs.txt"

echo
echo "=== before -> after ==="
python3 tools/E555_rank.py "$BOARDS" --seed_file "$SEED" --csv > "$TMP/before.csv"
python3 tools/E555_rank.py "$OUT" --seed_file "$SEED" --csv > "$TMP/after.csv"
python3 - "$TMP/before.csv" "$TMP/after.csv" <<'EOF'
import csv, sys
# OUT holds the boards in the order of BOARDS, one row each: join on the row
before = {r["row"]: r for r in csv.DictReader(open(sys.argv[1]))}
better = gain = 0
for r in sorted(csv.DictReader(open(sys.argv[2])), key=lambda r: int(r["row"])):
    old, new = int(before[r["row"]]["score"]), int(r["score"])
    print(f"  {r['id']}: {old} -> {new}" + (f"  (+{new - old})" if new > old else ""))
    better += new > old
    gain += new - old
print(f"  {better} board(s) improved, +{gain} edges in all")
EOF
echo
echo "Files written:"
sed 's/^/  /' "$OUT.outputs.txt"
