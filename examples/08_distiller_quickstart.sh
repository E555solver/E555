#!/bin/bash
# 08_distiller_quickstart.sh -- pick the few boards worth Stage C time.
#
#   bash examples/08_distiller_quickstart.sh
#   bash examples/08_distiller_quickstart.sh BOARDS=beam_out/all.csv TOP=40
#   bash examples/08_distiller_quickstart.sh WORKDIR=/tmp/distil
#
# E555_rank.py sorts by what a board IS. Above ~450 that stops separating
# anything: every board sits within a couple of breaks of the same entropy
# floor, so CP-SAT time gets spread evenly over a corpus that is mostly
# finished. The distiller ranks by what a board could BECOME -- the cheapest
# repair window covering its own breaks, how many single-swap escape routes
# that window still holds, and how good randomized dives get inside it.
#
# --plan is the point of the tool: it writes plan_<name>/ holding one hole mask
# per kept board and a run_plan.sh you can execute as it stands. Boards whose
# breaks hug one border get a --side band; sprawling ones get a --holes mask,
# which is exactly the case no band can express.
#
# There is nothing to tune. The dive budget, the shortlist size and the window
# choice are all derived at runtime.
set -euo pipefail

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # paths below are relative to REPO
BOARDS=data/best_463.csv
TOP=7                   # how many boards to keep
WORKDIR=distiller_out   # plan_<name>/ and the distilled CSV land here
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
# Dives are optional -- without the binary the tool still ranks, on one measure
# fewer -- but they are the only signal that samples the repair landscape.
[ -x bin/E555_backtracker ] || make bin/E555_backtracker

BOARDS_ABS=$(cd "$(dirname "$BOARDS")" && pwd)/$(basename "$BOARDS")
SEED_ABS=$(cd "$(dirname "$SEED")" && pwd)/$(basename "$SEED")
mkdir -p "$WORKDIR"

echo "=== what E555_rank.py sees: one score, no way to choose between them ==="
python3 tools/E555_rank.py "$BOARDS" --seed_file "$SEED" --top "$TOP"

echo
echo "=== what the distiller sees ==="
# --plan writes into the working directory, so run it from WORKDIR.
( cd "$WORKDIR" && python3 "$REPO/tools/E555_distiller.py" "$BOARDS_ABS" \
      --seed_file "$SEED_ABS" --top "$TOP" --out distilled.csv --plan )

echo
echo "Files written:"
find "$WORKDIR" -type f | sort | sed 's/^/  /'
echo
echo "The plan is runnable as it stands:"
echo "    bash $WORKDIR/plan_$(basename "${BOARDS%.*}")/run_plan.sh"
echo
echo "Read one board's reasoning -- the ASCII map shows its breaks and the"
echo "window that was chosen to cover them:"
echo "    python3 tools/E555_distiller.py $BOARDS --explain 2"
