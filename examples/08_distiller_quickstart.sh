#!/bin/bash
# 08_distiller_quickstart.sh -- distil a corpus of boards to the few worth the
# CP-SAT tail, finished and ready for the ender.
#
#   bash examples/08_distiller_quickstart.sh
#   bash examples/08_distiller_quickstart.sh BOARDS=beam_out/partials.csv.gz TOP=25
#
# Stages: drop exact repeats -> screen every partial with 300 seeded dives ->
# finish the best half (at most 400 x TOP) -> probe the best 4 x TOP with the
# ender's redive -> keep TOP. Writes OUT, OUT.plan.sh (the ender command for
# them) and OUT_work/ (every stage's results; a rerun resumes there).
# PROJECT_E555.md, "E555_distiller.py", has the stages and their calibration.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/E565_lowB_baseline.csv      # partial or complete boards, .csv or .csv.gz
TOP=5                   # boards to keep
OUT=distilled.csv       # also writes OUT.plan.sh and OUT_work/
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

python3 tools/E555_distiller.py "$BOARDS" --top "$TOP" --out "$OUT" --seed_file "$SEED"

echo
echo "Next, the CP-SAT closer on the $TOP boards (about 15 min a board):"
echo "    bash ${OUT%.csv}.plan.sh"
