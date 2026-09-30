#!/bin/bash
# 11_diver_reopen.sh -- improve complete boards: reopen the damaged rows and
# re-dive them, never returning a worse board.
#
#   bash examples/11_diver_reopen.sh
#   bash examples/11_diver_reopen.sh BOARDS=final_out/best.csv SECONDS_PER_BOARD=600 THREADS=20
#   bash examples/11_diver_reopen.sh REOPEN=auto+1 PRIOR=0 NOGO=0
#
# For complete boards that were dived and polished (their small windows are
# already optimal): lift the damaged band (REOPEN=auto) and re-dive it in COPIES
# copies, round after round from the best board so far, until SECONDS_PER_BOARD
# is spent. PRIOR/NOGO pull the dives toward the board's clean placements and
# off its broken ones (the best setting measured). Every board comes back as
# one row of OUT, never worse. PROJECT_E555.md, "E555_diver", has the details.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/best_463.csv                # complete boards to improve
OUT=reopened.csv
REOPEN=auto             # auto | auto+E | top:K | box:R0-R1,C0-C1 | a 16x16 mask file
SECONDS_PER_BOARD=60    # wall clock per board (the rounds run until it is spent)
COPIES=8                # copies per round, each on its own streams
END_DIVE=3000           # dives per copy
END_POLISH=1000         # kick-and-polish rounds per copy
PRIOR=1                 # pull toward the board's clean placements (0 = off)
NOGO=1                  # push off its placements on broken cells (0 = off)
PLATEAU=0               # 1 = a round may move to a different board of equal score
THREADS=8
RNG_SEED=1
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
[ -x bin/E555_diver ] || make diver

PLATEAU_ARG=(); [ "$PLATEAU" = 1 ] && PLATEAU_ARG=(--plateau)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
: > "$OUT"

# One diver run per board, so each board gets its own wall clock.
python3 - "$BOARDS" "$TMP" <<'EOF'
import sys
rows = [l for l in open(sys.argv[1]) if l.strip() and l.lstrip()[0] not in "#%"]
for i, l in enumerate(rows):
    open(f"{sys.argv[2]}/board_{i:04d}.csv", "w").write(l if l.endswith("\n") else l + "\n")
EOF
for f in "$TMP"/board_*.csv; do
    bin/E555_diver "$SEED" "$f" "$f.out" --reopen "$REOPEN" --rounds 1000000 \
        --wall_time "$SECONDS_PER_BOARD" --copies "$COPIES" "${PLATEAU_ARG[@]}" \
        --prior "$PRIOR" --nogo "$NOGO" \
        --end_dive "$END_DIVE" --end_polish "$END_POLISH" --emit_score 0 \
        --threads "$THREADS" --rng_seed "$RNG_SEED" --print_cmd \
        | grep -E "^\[(cmd|sum\] reopen|sum\] plateau)" || true
    cat "$f.out" >> "$OUT"
done
echo "$OUT" > "$OUT.outputs.txt"

echo
echo "=== before -> after ==="
python3 tools/E555_rank.py "$BOARDS" --seed_file "$SEED" --csv > "$TMP/before.csv"
python3 tools/E555_rank.py "$OUT" --seed_file "$SEED" --csv > "$TMP/after.csv"
python3 - "$TMP/before.csv" "$TMP/after.csv" <<'EOF'
import csv, sys
# OUT holds the boards in the order of BOARDS, one row each: join on the row
before = {r["row"]: r for r in csv.DictReader(open(sys.argv[1]))}
for r in sorted(csv.DictReader(open(sys.argv[2])), key=lambda r: int(r["row"])):
    old = before.get(r["row"], {}).get("score", "?")
    mark = "  (+%d)" % (int(r["score"]) - int(old)) if old != "?" and int(r["score"]) > int(old) else ""
    print(f"  {r['id']}: {old} -> {r['score']}{mark}")
EOF
echo
echo "Files written:"
sed 's/^/  /' "$OUT.outputs.txt"
