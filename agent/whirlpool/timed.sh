#!/bin/bash
# timed.sh -- run one command and log its run time to the campaign's runs.csv,
# for the whirlpool kit (agent/whirlpool_agent_mode.md). The campaign's budget
# is the sum of that column (agent/whirlpool/status.sh), so every experiment that
# runs a tool for more than a few seconds goes through this script or through
# harness.sh, which logs itself. Run it from the campaign's lab.
#
#   bash ../../timed.sh "band cut, BAND_ROW=6" -- bin/E555_backtracker ...
#   TIMED label="band cut, BAND_ROW=6" runtime_s=41 exit=0
#
# The command's output goes where it would without the wrapper, and the
# command's exit status is this script's. Exit status 2 on bad usage.
set -uo pipefail

KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
case "$PWD/" in
    "$KIT"/*/lab/*) ;;
    *) echo "timed: run it from a campaign's lab, agent/whirlpool/<id>/lab/" >&2; exit 2 ;;
esac
DIR=${PWD#"$KIT/"}; DIR="$KIT/${DIR%%/*}"
[ $# -ge 3 ] && [ "$2" = "--" ] || { echo 'usage: bash ../../timed.sh "LABEL" -- COMMAND [ARGS...]' >&2; exit 2; }
label=$1; shift 2

start=$(date +%s.%N)
"$@"
rc=$?
end=$(date +%s.%N)
runtime=$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.1f", b - a}')
python3 - "$DIR/runs.csv" "$label" "$runtime" "$rc" <<'EOF'
import csv, sys, time
path, label, runtime, rc = sys.argv[1:]
now = int(time.time())
with open(path, "a", newline="") as fh:
    csv.writer(fh, lineterminator="\n").writerow(
        [now, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)), "timed", label, runtime, rc])
EOF
echo "TIMED label=\"$label\" runtime_s=$runtime exit=$rc"
exit "$rc"
