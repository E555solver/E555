#!/bin/bash
##SBATCH --job-name=E555_tests
##SBATCH --ntasks=1 --cpus-per-task=8 --mem=10G --time=01:00:00
##SBATCH --output=logs/tests_%j.out
#
# =============================================================================
# run_tests.sh -- the E555 release gate
# =============================================================================
# Runs every check in ALL_STEPS below, in order, and stops at the first failure.
# That array is the ONLY list of checks: it fixes the numbering printed at run
# time, and every entry NAME has a function `step_NAME` further down with the
# reasoning for the check written above it. There is deliberately no second copy
# of the list in this header -- the two used to disagree.
#
#   bash tests/run_tests.sh                       every check
#   bash tests/run_tests.sh --list                the numbered list, then exit
#   bash tests/run_tests.sh 6                     just check 6
#   bash tests/run_tests.sh 8-11 14               checks 8, 9, 10, 11 and 14
#   bash tests/run_tests.sh roundhouse_cache      by name
#
# Any check runs on its own: none of them consumes a previous step's artifacts
# (the roundhouse partials that four checks share are built on demand). Leaving
# check 1 out of the selection uses whatever is already in bin/.
#
# RUNTIME  about 4 minutes with SKIP_BEAMER=1. The three checks that need the
# real 6.4 GB chain database -- beamer_micro, example_beamer and pipeline_full
# -- share one cache and want ~8 GB of RAM.
#
# Environment switches:
#   ARCH=generic    build for any CPU rather than the build host. Set it in CI
#                   and containers; -march=native is the Makefile default.
#   SKIP_BEAMER=1   skip the three database checks (low-RAM machines)
#   DB_FILE=path    keep the 6.4 GB chain database here (~6.5 GB on disk)
#                   instead of under tests/out, so it survives the wipe and
#                   every later run loads it rather than building it. The
#                   three real-seed checks share one cache either way.
#   DB_IN_MEMORY=1  never write the database to disk: each of the three checks
#                   builds it in RAM and drops it (full-disk machines). Four
#                   builds instead of one, so ~4x the database time, and each
#                   build needs 8 GB free. Overrides DB_FILE.
#
# This gate proves the tools find the RIGHT answer.
#
# All artifacts go to tests/out/ (wiped at start).
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
OUT=tests/out

# The three checks that use the real seed each rebuilt the 6.4 GB chain database
# from scratch: 78 s of build apiece, four builds across the run, and most of
# the gate's wall time. They now share one cache, written by whichever runs
# first and mmapped by the rest. Only the beamer's full inner database is
# shareable -- the finalizer's is rebuilt per configuration without the locked
# pieces, and is cheap anyway (9 s).
#
# It lives under tests/out, so it is wiped with everything else and never
# outlives a run; DB_FILE=path in the environment points them at a cache that
# does, which turns the build into a load on every future run. SKIP_BEAMER=1
# skips all three, so nothing is built at all. DB_IN_MEMORY=1 empties GATE_DB:
# the tools and scripts all omit --db_file for an empty path and build in RAM,
# so a drive with no room for 6.5 GB can still run every check.
if [ "${DB_IN_MEMORY:-0}" = "1" ]; then GATE_DB=""
else GATE_DB="${DB_FILE:-$OUT/chain.db}"; fi

# -----------------------------------------------------------------------------
# The checks, in order. "name|one-line label"; the label is what gets printed.
# -----------------------------------------------------------------------------
ALL_STEPS=(
    "compile|make all, zero compiler warnings tolerated"
    "viewer|the known synthetic solution scores 480/480"
    "rank|measures agree with the viewer, --out verbatim, --rescore canonical"
    "rotate|a quarter-turn preserves every measure, four turns are the identity"
    "sink|--sink drops N rows, frees the frame it broke, and keeps the core intact"
    "distiller|per-board windows differ, masks cover every break, --plan runs"
    "consensus|four turns of one board score identically, and --border_out runs"
    "annealer|Stage A short run: BEST lines, a beamer-format --out CSV, and spins that match their comment"
    "annealer_refine|Stage A warm start: every shipped row round-trips, and refining one cannot lose ground"
    "sort_rotations|both comment forms sort alike, and --max_top turns every row onto its own best side"
    "finalizer_synth|REGRESSION: rediscovers the synthetic solution from row 10"
    "finalizer_rotations|re-imposes a matching rotations row's side assignment"
    "finalizer_determinism|one seed re-run reproduces the search exactly"
    "roundhouse_synth|REGRESSION: rebuilds the solution at strip widths 3 and 5"
    "roundhouse_two_rounds|closes the board in two rounds, rotating between them"
    "roundhouse_cache|the transition cache agrees with decoding every record"
    "roundhouse_legal|every emitted board is break-free and frame-legal"
    "roundhouse_cw|--cw mirrors the seed, so the spiral runs the other way round"
    "roundhouse_hold_band|--hold_band keeps the far half of the final side and searches the near half"
    "backtracker_dives|greedy dives on the example board, plus an own-output round-trip"
    "backtracker_exhaustive|exhaustive enumeration identical at 1 and 4 threads"
    "backtracker_stop_band|--stop_row/--stop_column emit exact, finalizer-shaped bands"
    "whirlpool_lap|one whirlpool lap: turn, re-cut rows 0..5, re-grow to row 11"
    "clue_orient|a band carrying no clue is searched at all four orientations"
    "band_with_frame|--with_frame carries all 60 frame cells, so the finalizer fixes the sides"
    "cpsat_chain|topper -> ender -> ender, each fed by the last"
    "beamer_micro|random_edges micro-run: builds the real 6.4 GB database"
    "scripts_parse|every shipped script parses, and passes only flags that exist"
    "example_finalizer|examples/02 re-grows the synthetic board"
    "example_roundhouse|examples/03 refills one strip"
    "example_bothways|examples/06 runs both chains over one board, ids intact"
    "example_cpsat|examples/04a, scout -> promote -> polish -> close"
    "example_backtracker|examples/05 dives on the example board"
    "pipeline_topper_sweep|pipeline/topper_sweep.sh through a two-pass plan"
    "example_beamer|examples/01 both ways, random and annealed borders"
    "pipeline_full|pipeline/run_pipeline.sh, all seven stages"
    "pipeline_whirl_lap|pipeline/run_pipeline_whirlpool.sh, one lap end to end"
    "farm_pools|run_farm.py: file handling, spread(), and the spiral guard"
    "no_stray_output|no check left a file in the repository root"
)
TOTAL=${#ALL_STEPS[@]}

# -----------------------------------------------------------------------------
# Harness
# -----------------------------------------------------------------------------
STEP=""
fail() { echo "!!! FAILED at: $STEP -- $1"; exit 1; }

usage() {
    echo "usage: bash tests/run_tests.sh [--list] [N | N-M | NAME]..."
    echo "       no argument runs every check; see the header of this file."
}

list_steps() {
    local i=1 e
    for e in "${ALL_STEPS[@]}"; do
        printf '%3d  %-22s %s\n' "$i" "${e%%|*}" "${e#*|}"
        i=$((i + 1))
    done
}

# index_of TOKEN -- 1-based position of a step name, or nothing.
index_of() {
    local i=1 e
    for e in "${ALL_STEPS[@]}"; do
        [ "${e%%|*}" = "$1" ] && { echo "$i"; return 0; }
        i=$((i + 1))
    done
    return 0
}

# first_match GLOB... -- print the first path that exists, or nothing.
# Never write this as `$(ls GLOB 2>/dev/null | head -1)`: when the glob matches
# nothing, `ls` exits 2, `pipefail` promotes that to the pipeline's status and
# `set -e` kills the script -- which is exactly the case some checks below are
# trying to detect ("the roundhouse must emit nothing here"). An unmatched glob
# stays literal in bash, so `-e` is the test that works. Always returns 0.
first_match() { local f; for f in "$@"; do [ -e "$f" ] && { printf '%s\n' "$f"; return 0; }; done; return 0; }

# has_step N -- is check N in this run's selection?
has_step() { local i; for i in "${SEL[@]}"; do [ "$i" = "$1" ] && return 0; done; return 1; }

# The roundhouse partials that four checks share. Built on demand so that any
# one of those checks can be selected on its own.
rh_fixtures() {
    [ -s "$OUT/rh_rows12.csv" ] && return 0
    python3 - data/synth_solution_480.csv "$OUT/rh_rows12.csv" "$OUT/rh_rows10.csv" \
            "$OUT/rh_damaged.csv" "$OUT/rh_corebreak.csv" <<'EOF'
import sys
src, out12, out10, damaged, corebreak = sys.argv[1:6]
line = [l for l in open(src) if l.strip() and not l.lstrip().startswith(("#", "%"))][0]
f = [t.strip() for t in line.split(",")]
pos, rot = [int(x) for x in f[-512:-256]], f[-256:]
def write(dst, p):
    with open(dst, "w") as fh:
        fh.write("board, 0, " + ", ".join(str(x) for x in p) + ", " + ", ".join(rot) + "\n")
for dst, keep_upto in ((out12, 12), (out10, 10)):
    write(dst, [x if x == 999 or x // 16 <= keep_upto else 999 for x in pos])
at = {pos[p]: p for p in range(256) if pos[p] != 999}
# Damaged: holes AND breaks, but only inside the band a W=3 run frees anyway.
# The core (rows 0..12) is untouched, so the run must ignore all of it.
d = list(pos)
for c in range(3, 8): d[at[14 * 16 + c]] = 999
a, b = at[13 * 16 + 2], at[13 * 16 + 9]
d[a], d[b] = d[b], d[a]
write(damaged, d)
# The mirror image: one swap INSIDE the core, which must be refused outright.
cb = list(pos)
a, b = at[4 * 16 + 6], at[4 * 16 + 11]
cb[a], cb[b] = cb[b], cb[a]
write(corebreak, cb)
print("ok: partials cut at rows 12 and 10, plus damaged and core-break variants")
EOF
}

# =============================================================================
# The checks
# =============================================================================

step_compile() {
    make clean >/dev/null
    make all 2> "$OUT/warnings.txt"
    if [ -s "$OUT/warnings.txt" ]; then cat "$OUT/warnings.txt"; fail "compiler warnings"; fi
    echo "ok: 4 binaries, no warnings"
}

step_viewer() {
    python3 tools/E555_viewer.py data/synth_solution_480.csv --seed_file data/synth_seed.txt \
        --no_board --no_url > /dev/null   # parse check
    # Not piped into `grep -q`: the match is on line 5 of 43, so grep exits at
    # once, the viewer takes SIGPIPE on the rest of the board, and `pipefail`
    # turns that into a failed check -- a gate that goes red on a passing tool.
    python3 tools/E555_viewer.py data/synth_solution_480.csv --seed_file data/synth_seed.txt \
        --no_url > "$OUT/viewer.txt"
    grep -q "Correct edges : 480 / 480" "$OUT/viewer.txt" || fail "expected 480/480"
    echo "ok: 480/480"
}

# --rescore is the only writer in the repo that rewrites field 2 as the true
# matched-edge count, which is what makes a mixed corpus sortable by score.
step_rank() {
    python3 tools/E555_rank.py data/board_example_462.csv --seed_file data/seed_Edge5.txt \
        --out "$OUT/norm.csv" --rescore | grep -q "1 canonical row" \
        || fail "rank --rescore did not write 1 canonical row"
    fields=$(awk -F, 'NR==1{print NF}' "$OUT/norm.csv")
    [ "$fields" = "514" ] || fail "canonical row has $fields fields, want 514"
    score=$(cut -d, -f2 "$OUT/norm.csv")
    [ "$score" = "462" ] || fail "expected score 462, got $score"
    # the rewrite must preserve the board itself: last 512 fields unchanged
    cmp -s <(cut -d, -f3- "$OUT/norm.csv") \
           <(cut -d, -f3- data/board_example_462.csv) \
        || fail "--rescore altered the board, not just the score column"
    echo "ok: canonical score 462, board preserved"

    # rank must agree with the viewer's independent count, and re-emit verbatim
    python3 tools/E555_rank.py data/board_example_462.csv --csv > "$OUT/rank.csv"
    rscore=$(awk -F, 'NR==2{print $5}' "$OUT/rank.csv")   # solid
    rbreak=$(awk -F, 'NR==2{print $3}' "$OUT/rank.csv")   # breaks
    [ "$rscore" = "229" ] || fail "rank solid=$rscore, viewer says 229"
    [ "$rbreak" = "18" ] || fail "rank breaks=$rbreak, want 18"
    python3 tools/E555_rank.py data/board_example_462.csv --out "$OUT/rank_emit.csv" > /dev/null
    cmp -s data/board_example_462.csv "$OUT/rank_emit.csv" \
        || fail "rank --out did not reproduce the input row verbatim"
    echo "ok: rank agrees with the viewer (18 breaks, 229 solid), --out is verbatim"
}

# A quarter-turn must move the board without changing it: same breaks, same
# solid count, transposed span. Four turns must return the original bytes.
# The distiller ranks by what a board could BECOME, so the regression that
# matters is that its board-DEPENDENT layer actually varies: b* of a fixed
# window shape is the same for every board, so a build where the window search
# has broken still prints a plausible table, just a useless one.
step_distiller() {
    python3 tools/E555_distiller.py data/best_463.csv --seed_file data/seed_Edge5.txt \
        --top 7 > "$OUT/distil.txt" 2>"$OUT/distil.err" || fail "distiller exited nonzero"
    rows=$(grep -cE '^ *[0-9]+ ' "$OUT/distil.txt")
    [ "$rows" = "7" ] || fail "expected 7 ranked rows, got $rows"

    # All seven boards score 463, so a ranking that works has to come from the
    # structure. Distinct window names across the corpus is the cheapest proof
    # that the per-board window search ran at all.
    wins=$(awk '$1 ~ /^[0-9]+$/ {print $7}' "$OUT/distil.txt" | sort -u | wc -l)
    [ "$wins" -ge 2 ] || fail "every board picked the same window: window search is dead"
    echo "ok: 7 boards ranked, $wins distinct windows"

    # --explain names the board's own geometry; row 2's breaks reach row 10, so
    # no top band covers them cheaply and the hull has to win.
    python3 tools/E555_distiller.py data/best_463.csv --seed_file data/seed_Edge5.txt \
        --explain 2 > "$OUT/distil_explain.txt" 2>&1
    grep -q "chosen window  hull" "$OUT/distil_explain.txt" \
        || fail "--explain 2 should pick hull, not a band"

    # --plan writes into the working directory, so run it somewhere disposable.
    ( cd "$OUT" && python3 "$REPO/tools/E555_distiller.py" "$REPO/data/best_463.csv" \
        --seed_file "$REPO/data/seed_Edge5.txt" --top 3 --plan > plan.txt 2>&1 ) \
        || fail "distiller --plan exited nonzero"
    [ -f "$OUT/plan_best_463/run_plan.sh" ] || fail "--plan wrote no run_plan.sh"
    bash -n "$OUT/plan_best_463/run_plan.sh" || fail "run_plan.sh does not parse"
    masks=$(ls "$OUT"/plan_best_463/*.holes.csv | wc -l)
    [ "$masks" = "3" ] || fail "expected 3 hole masks, got $masks"

    # A mask that misses a break is worse than useless: Stage C would re-solve a
    # region that cannot contain the fix.
    python3 - "$REPO" "$OUT/plan_best_463" <<'PY' || fail "a hole mask does not cover its board's breaks"
import csv, sys
from pathlib import Path
root, plan = Path(sys.argv[1]), Path(sys.argv[2])
sys.path.insert(0, str(root / "tools"))
import E555_viewer as V, E555_distiller as D
seed = V.load_seed(root / "data" / "seed_Edge5.txt")
rows = [r for r in csv.reader(open(root / "data" / "best_463.csv")) if V.parse_row(r)]
for mask_file in sorted(plan.glob("*.holes.csv")):
    idx = int(mask_file.name[1:5])
    _, _, pos, rot = V.parse_row(rows[idx])
    _, bad = D.board_colors(pos, rot, seed)
    vals = []
    for line in open(mask_file):
        if not line.strip().startswith("#"):
            vals.extend(line.replace(",", " ").split())
    if len(vals) != 256:
        sys.exit(f"{mask_file.name}: {len(vals)} values, want 256")
    free = {i for i, v in enumerate(vals) if int(v) == 1}
    if not bad <= free:
        sys.exit(f"{mask_file.name}: misses {len(bad - free)} break cell(s)")
PY
    echo "ok: --explain picks hull, 3 masks cover every break, run_plan.sh parses"

    # --triage ranks on closure alone. A complete board has no pool left, so its
    # closure must be exactly 0 -- the cheapest proof the ledger is being read
    # rather than invented.
    python3 tools/E555_distiller.py data/best_463.csv --seed_file data/seed_Edge5.txt \
        --triage --top 3 > "$OUT/triage.txt" 2>"$OUT/triage.err" \
        || fail "--triage exited nonzero"
    trows=$(grep -cE '^ *[0-9]+ ' "$OUT/triage.txt")
    [ "$trows" = "3" ] || fail "--triage: expected 3 rows, got $trows"
    nonzero=$(awk '$1 ~ /^[0-9]+$/ && $6 != "0.00"' "$OUT/triage.txt" | wc -l)
    [ "$nonzero" = "0" ] || fail "--triage: a complete board scored closure != 0"

    # The point of the measure: a corpus the ranker cannot separate at all --
    # every board grown to the same stop row, so every shape measure is a
    # constant -- must still come out ordered.
    python3 tools/E555_distiller.py data/E565_lowB_baseline.csv \
        --seed_file data/seed_Edge5.txt --triage --top 50 \
        > "$OUT/triage_partial.txt" 2>&1 || fail "--triage on the partial corpus failed"
    spread=$(awk '$1 ~ /^[0-9]+$/ {print $6}' "$OUT/triage_partial.txt" | sort -u | wc -l)
    [ "$spread" -ge 10 ] || fail "closure took only $spread value(s) over 50 partials: it is not separating them"
    shapes=$(python3 tools/E555_rank.py data/E565_lowB_baseline.csv --csv 2>/dev/null \
        | awk -F, 'NR>1{$1="";$2="";print}' | sort -u | wc -l)
    echo "ok: --triage keeps closure 0 on complete boards, and splits into $spread value(s)"
    echo "    a corpus E555_rank.py reduces to $shapes distinct measure vector(s)"
}

step_rotate() {
    python3 tools/E555_rotate.py data/board_example_462.csv 0 \
        --seed_file data/seed_Edge5.txt --out "$OUT/rot0.csv" > /dev/null
    prev="$OUT/rot0.csv"
    for t in 1 2 3 4; do
        python3 tools/E555_rotate.py "$prev" 1 --seed_file data/seed_Edge5.txt \
            --out "$OUT/rot$t.csv" > /dev/null || fail "rotate failed at turn $t"
        prev="$OUT/rot$t.csv"
    done
    cmp -s "$OUT/rot0.csv" "$OUT/rot4.csv" \
        || fail "four quarter-turns did not return the original board"
    python3 tools/E555_rank.py "$OUT/rot0.csv" "$OUT/rot1.csv" "$OUT/rot2.csv" \
        "$OUT/rot3.csv" --seed_file data/seed_Edge5.txt --csv > "$OUT/rot_rank.csv"
    # columns: file,row,id,breaks,score,solid,placed,border,break_rows,break_cols,span
    awk -F, 'NR>1{b[$4]=1; s[$6]=1; sp[$11]=1}
             END{ if (length(b)!=1) exit 1
                  if (length(s)!=1) exit 2
                  if (length(sp)!=2) exit 3 }' "$OUT/rot_rank.csv" \
        || fail "a turn changed breaks/solid, or span did not transpose"
    # rank sorts its output, so look the two spans up by file name, not by row
    span_of() { awk -F, -v f="$1" 'NR>1 && $1==f {print $11}' "$OUT/rot_rank.csv"; }
    [ "$(span_of rot0.csv)" = "5x15" ] || fail "unrotated span is not 5x15"
    [ "$(span_of rot1.csv)" = "15x5" ] || fail "90-degree span did not transpose to 15x5"

    # --rotations turns a Stage A border's side assignment instead of a board.
    # Same identity law, and the spins it writes must be the ones the turned
    # board carries -- that equality is exactly what fin_rot_match compares.
    python3 tools/E555_rotate.py data/borders_annealed_fix12.csv 0 --rotations \
        --seed_file data/seed_Edge5.txt --out "$OUT/br0.csv" > /dev/null
    prev="$OUT/br0.csv"
    for t in 1 2 3 4; do
        python3 tools/E555_rotate.py "$prev" 1 --rotations --seed_file data/seed_Edge5.txt \
            --out "$OUT/br$t.csv" > /dev/null || fail "--rotations failed at turn $t"
        prev="$OUT/br$t.csv"
    done
    grep -v '^ *#' "$OUT/br0.csv" > "$OUT/br0.spins"
    grep -v '^ *#' "$OUT/br4.csv" > "$OUT/br4.spins"
    cmp -s "$OUT/br0.spins" "$OUT/br4.spins" \
        || fail "four --rotations turns did not return the original spins"
    n=$(grep -vc '^ *#' "$OUT/br0.csv")
    [ "$n" = 12 ] || fail "--rotations kept $n border rows of data/borders_annealed_fix12.csv, expected 12"

    python3 - "$OUT" <<'EOF' || exit 1
import csv, subprocess, sys
out = sys.argv[1]
def frame_spins(path):                     # {piece: rotation} over the 60 frame cells
    row = next(r for r in csv.reader(open(path))
               if r and not r[0].lstrip().startswith("#"))
    f = [x.strip() for x in row][-512:]
    pos, rot = [int(x) for x in f[:256]], [int(x) for x in f[256:]]
    return {p: rot[p] for p, v in enumerate(pos)
            if v != 999 and (v // 16 in (0, 15) or v % 16 in (0, 15))}
subprocess.run(["python3", "tools/E555_rank.py", "data/best_463.csv",
                "--seed_file", "data/seed_Edge5.txt", "--border_only",
                "--out", f"{out}/rr_full.csv"], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
open(f"{out}/rr_board.csv", "w").write(open(f"{out}/rr_full.csv").readline())
sp = [0] * 256
for p, s in frame_spins(f"{out}/rr_board.csv").items():
    sp[p] = s
open(f"{out}/rr_row.csv", "w").write("000," + ",".join(str(x) for x in sp) + "\n")
for tool, src, dst, extra in (
        ("board", f"{out}/rr_board.csv", f"{out}/rr_board1.csv", []),
        ("row",   f"{out}/rr_row.csv",   f"{out}/rr_row1.csv",   ["--rotations"])):
    subprocess.run(["python3", "tools/E555_rotate.py", src, "1",
                    "--seed_file", "data/seed_Edge5.txt", "--out", dst] + extra,
                   check=True, stdout=subprocess.DEVNULL)
turned = frame_spins(f"{out}/rr_board1.csv")
row = next(r for r in csv.reader(open(f"{out}/rr_row1.csv"))
           if r and not r[0].lstrip().startswith("#"))
row = [int(v.strip()) for v in row][-256:]
bad = [p for p, s in turned.items() if row[p] != s]
if len(turned) != 60 or bad:
    print(f"!!! --rotations disagreed with the turned board on {len(bad)} of "
          f"{len(turned)} frame pieces", file=sys.stderr)
    raise SystemExit(1)
EOF
    echo "ok: rotation is lossless, span transposes, 4 turns = identity;"
    echo "    --rotations agrees with the turned board on all 60 frame spins"
}

step_sink() {
    # --sink 0 must leave the rotate path byte for byte where it was.
    python3 tools/E555_rotate.py data/best_463.csv 1 --seed_file data/seed_Edge5.txt \
        --out "$OUT/sink_none.csv" > /dev/null
    python3 tools/E555_rotate.py data/best_463.csv 1 --sink 0 \
        --seed_file data/seed_Edge5.txt --out "$OUT/sink_zero.csv" > /dev/null
    cmp -s "$OUT/sink_none.csv" "$OUT/sink_zero.csv" \
        || fail "--sink 0 is not the identity"

    for d in 2 3; do
        python3 tools/E555_rotate.py data/best_463.csv 2 --sink $d \
            --seed_file data/seed_Edge5.txt --out "$OUT/sink$d.csv" \
            > "$OUT/sink$d.log" || fail "--sink $d failed"
    done
    # the shipped boards carry their 17 breaks in the top rows, so a 180-degree
    # turn puts them at the bottom and --sink 3 drops all but one of them.
    grep -q "6 of 7 clean" "$OUT/sink3.log" \
        || fail "--sink 3 no longer leaves 6 of 7 cores break-free"

    python3 - "$OUT" <<'EOF' || exit 1
import sys
sys.path.insert(0, "tools")
import E555_viewer as V, E555_rank as R
out, seed = sys.argv[1], V.load_seed(V.find_seed("data/seed_Edge5.txt"))
SIDE, UNP = V.SIDE, V.UNPLACED
grey = [sum(1 for x in e if x == 0) for e in seed]
for depth in (2, 3):
    rows = list(V.iter_records(f"{out}/sink{depth}.csv"))
    if len(rows) != 7:
        raise SystemExit(f"!!! --sink {depth} wrote {len(rows)} rows, expected 7")
    for idx, cid, sol, pos, rot in rows:
        board = V.build_board(pos, rot)          # also proves no two pieces collide
        empty = [r * SIDE + c for r in range(SIDE) for c in range(SIDE)
                 if board[r][c] is None]
        # --sink N opens row 0 and rows 15-N..15, and nothing else
        want = [0] + list(range(SIDE - 1 - depth, SIDE))
        if sorted({c // SIDE for c in empty}) != want or len(empty) != 16 * (depth + 2):
            raise SystemExit(f"!!! --sink {depth} row {idx}: opened {len(empty)} "
                             f"cell(s) in rows {sorted({c // SIDE for c in empty})}")
        # every piece still placed is legal where it sits: its grey sides are
        # exactly the sides of its cell that face out of the board
        for p in range(256):
            if pos[p] == UNP:
                continue
            shown = V.rotate_edges(seed[p], rot[p])
            faces = R.FRAME_SIDES.get(pos[p], ())
            if any((shown[d] == 0) != (d in faces) for d in range(4)):
                raise SystemExit(f"!!! --sink {depth} row {idx}: piece {p} shows "
                                 f"grey into the board at cell {pos[p]}")
        # and the freed pieces are exactly what the holes need, kind for kind --
        # 4 corners, borders for the ring, inner for the interior. Without this
        # the board is short a corner and can never be completed.
        need, have = {0: 0, 1: 0, 2: 0}, {0: 0, 1: 0, 2: 0}
        for c in empty:
            need[len(R.FRAME_SIDES.get(c, ()))] += 1
        for p in range(256):
            if pos[p] == UNP:
                have[grey[p]] += 1
        if need != have:
            raise SystemExit(f"!!! --sink {depth} row {idx}: holes need {need}, "
                             f"the sink freed {have}")
EOF

    # Sinks compose, which is what lets N be turned one notch at a time on a
    # file that has already been sunk.
    python3 tools/E555_rotate.py data/best_463.csv 2 --sink 1 \
        --seed_file data/seed_Edge5.txt --out "$OUT/sink_1a.csv" > /dev/null
    python3 tools/E555_rotate.py "$OUT/sink_1a.csv" 0 --sink 1 \
        --seed_file data/seed_Edge5.txt --out "$OUT/sink_1b.csv" > /dev/null
    cmp -s "$OUT/sink_1b.csv" "$OUT/sink2.csv" \
        || fail "--sink 1 twice did not give what --sink 2 gives once"

    # the centre clue moves with the board but its spin does not, so the filter
    # keeps only a board already carrying the spin of the cell it lands on.
    python3 tools/E555_rotate.py data/best_463.csv 0 --sink 4 --clue_center \
        --seed_file data/seed_Edge5.txt --out "$OUT/sink_clue.csv" > /dev/null \
        || fail "--clue_center dropped the one qualifying board"
    n=$(python3 tools/E555_rank.py "$OUT/sink_clue.csv" --seed_file data/seed_Edge5.txt --count)
    [ "$n" = 1 ] || fail "--clue_center kept $n of the 7 boards, expected 1"
    if python3 tools/E555_rotate.py data/best_463.csv 2 --sink 2 --clue_center \
            --seed_file data/seed_Edge5.txt --out "$OUT/sink_noclue.csv" \
            > /dev/null 2>&1; then
        fail "--clue_center kept a board at a setting where none qualifies"
    fi

    for bad in "--sink 15" "--sink 2 --rotations" \
               "--sink 2 --holes data/holes_open_border_TR.csv"; do
        if python3 tools/E555_rotate.py data/best_463.csv 0 $bad \
                --seed_file data/seed_Edge5.txt --out "$OUT/sink_bad.csv" \
                > /dev/null 2>&1; then
            fail "'$bad' was accepted"
        fi
    done

    echo "ok: --sink opens row 0 and the top N+1 rows and nothing else, every"
    echo "    surviving piece is frame-legal, the freed pieces balance the holes"
    echo "    kind for kind, sinks compose, and --clue_center keeps 1 of 7"
}

# --verbose: the BEST lines grepped below are verbose-only, the default being
# one summary line per restart.
# The consensus ranker pools boards across the four clue frames, so the error
# that matters is a wrong canonicalisation: it would leave every number
# plausible and every ranking meaningless. The test is an identity. One board
# and its three quarter-turns are the SAME board once canonicalised, so they
# have to score bit for bit alike -- and the band --best_top picks has to be
# the band rotate_cell actually sends the top rows to, checked against the map
# rather than against the tool's own table.
step_consensus() {
    python3 - data/best_463.csv "$OUT/cons_clued.csv" <<'EOF' || exit 1
import sys, csv
sys.path.insert(0, "tools")
import E555_viewer as V
# data/ ships no clued board, and this tool reads nothing else. Piece 138 and
# cell (7,7) are both interior, so planting the centre clue by swapping leaves
# every frame piece where it was and the board frame-legal.
w = csv.writer(open(sys.argv[2], "w", newline=""), lineterminator="\n")
n = 0
for raw in csv.reader(open(sys.argv[1], newline="")):
    rec = V.parse_row(raw)
    if rec is None:
        continue
    cid, sol, pos, rot = rec
    cell, piece, spin = 7 * 16 + 7, 138, 0
    other = next((p for p, c in enumerate(pos) if c == cell), None)
    if other is not None and other != piece:
        pos[other], pos[piece] = pos[piece], cell
        rot[other], rot[piece] = rot[piece], spin
    # Two rows of best_463.csv share a config_id, and the identity below
    # groups the four turns of one board by id -- so number them here.
    w.writerow([f"{cid}_c{n}", sol] + pos + rot)
    n += 1
assert n == 7, n
EOF
    : > "$OUT/cons_corpus.csv"
    for k in 0 1 2 3; do
        python3 tools/E555_rotate.py "$OUT/cons_clued.csv" $k \
            --out "$OUT/cons_rot$k.csv" > /dev/null
        cat "$OUT/cons_rot$k.csv" >> "$OUT/cons_corpus.csv"
    done
    n=$(python3 tools/E555_extract_consensus.py "$OUT/cons_corpus.csv" --count)
    [ "$n" = "28" ] || fail "--count saw $n clued boards, want 28"

    python3 tools/E555_extract_consensus.py "$OUT/cons_corpus.csv" \
        --seed_file data/seed_Edge5.txt --csv --quiet > "$OUT/cons_rank.csv"
    python3 - "$OUT/cons_rank.csv" <<'EOF' || exit 1
import sys, csv, collections
rows = list(csv.DictReader(open(sys.argv[1])))
assert len(rows) == 28, len(rows)
# The identity: a board and its three turns canonicalise to one board, so the
# four have to agree on every measure. Grouped by the id the turns share.
by = collections.defaultdict(list)
for r in rows:
    by[r["id"]].append(r)
assert len(by) == 7, sorted(by)
for cid, group in by.items():
    assert len(group) == 4, (cid, len(group))
    assert sorted(int(r["orient"]) for r in group) == [0, 1, 2, 3], cid
    for k in ("lift", "logp", "rank", "top1", "cells"):
        vals = {r[k] for r in group}
        assert len(vals) == 1, f"{cid}: {k} differs across turns: {vals}"
print("ok: each board and its three quarter-turns score identically")
EOF

    # --out is verbatim, in the input's own un-rotated frame: the same bytes back.
    python3 tools/E555_extract_consensus.py "$OUT/cons_corpus.csv" --quiet \
        --out "$OUT/cons_emit.csv"
    cmp -s <(sort "$OUT/cons_corpus.csv") <(sort "$OUT/cons_emit.csv") \
        || fail "--out did not reproduce the input rows verbatim"

    # The band --best_top scores against must be the band the rotation map
    # really sends the top rows to, for each of the four clue frames.
    python3 tools/E555_extract_consensus.py "$OUT/cons_corpus.csv" \
        --best_top --csv --quiet > "$OUT/cons_top.csv"
    python3 - "$OUT/cons_top.csv" <<'EOF' || exit 1
import sys, csv
sys.path.insert(0, "tools")
import E555_rotate as RT
NAMES = ("TOP", "RIGHT", "BOTTOM", "LEFT")
def band_of(k):
    """Where rows 11..15 land after k quarter-turns clockwise, from the map."""
    dst = {divmod(RT.rotate_cell(r * 16 + c, k), 16)
           for r in range(11, 16) for c in range(16)}
    rows, cols = {a for a, _ in dst}, {b for _, b in dst}
    if len(rows) == 5:
        return "TOP" if min(rows) == 11 else "BOTTOM"
    return "RIGHT" if min(cols) == 11 else "LEFT"
for r in csv.DictReader(open(sys.argv[1])):
    o = int(r["orient"])
    want = band_of((4 - o) % 4)          # the turn that canonicalises it
    assert r["band"] == want, (o, r["band"], want)
print("ok: --best_top scores each clue frame against the band the map gives it")
EOF

    # --border_out must produce rows the rest of the toolkit accepts as borders:
    # the 14/14/14/14-plus-four-corners partition E555_database.c's
    # classify_deal_from_rotations demands. --BL/--BR/--TR/--TL pin all four
    # corners, which leaves exactly ONE corner class and so exactly one row.
    # Two seconds of search reaches a legal partition; the trail floor is not
    # the point here.
    python3 tools/E555_extract_consensus.py "$OUT/cons_corpus.csv" \
        --border_out "$OUT/cons_border.csv" --border_time 2 --min_trails 1 \
        --BL 1 --BR 2 --TR 0 --TL 3 --min_corner_boards 1 \
        --quiet > "$OUT/cons_border.log" 2>&1
    rows=$(grep -c '^c0,' "$OUT/cons_border.csv")
    [ "$rows" = "1" ] || fail "--border_out with all four corners pinned wrote $rows rows, want 1"
    python3 tools/E555_rotate.py --rotations "$OUT/cons_border.csv" 0 \
        --out "$OUT/cons_border_rot.csv" > "$OUT/cons_border_rot.log"
    grep -q "1 border row(s) turned" "$OUT/cons_border_rot.log" \
        || { cat "$OUT/cons_border_rot.log"; fail "the emitted border is not a legal 14/14/14/14 partition"; }
    echo "ok: --border_out wrote a border the rotations reader accepts"

    # THE FAIRNESS TEST. Scoring each board on its own placed cells ranks a
    # pool largely BY STOP ROW: the higher rows carry less consensus, so every
    # extra placed cell drags a board's mean down. Measured on this exact
    # fixture, all seven row-10 copies outranked all seven row-11 copies of the
    # SAME boards. --cells common must make the twins identical -- and --cells
    # placed must still separate them, or the flag has quietly stopped working
    # and a one-sided test would not notice.
    python3 - "$OUT/cons_clued.csv" "$OUT/cons_twins.csv" <<'EOF' || exit 1
import sys, csv
sys.path.insert(0, "tools")
import E555_viewer as V
w = csv.writer(open(sys.argv[2], "w", newline=""), lineterminator="\n")
n = 0
for idx, cid, sol, pos, rot in V.iter_records(sys.argv[1]):
    for stop in (10, 11):
        p = [c if c != 999 and c // 16 <= stop else 999 for c in pos]
        r = [rot[i] if p[i] != 999 else 0 for i in range(256)]
        w.writerow([f"b{n // 2}@{stop}", sol] + p + r)
        n += 1
assert n == 14, n
EOF
    for mode in common placed; do
        python3 tools/E555_extract_consensus.py "$OUT/cons_twins.csv" \
            --cells $mode --csv --quiet > "$OUT/cons_twins_$mode.csv"
    done
    python3 - "$OUT/cons_twins_common.csv" "$OUT/cons_twins_placed.csv" <<'EOF' || exit 1
import sys, csv, collections
def twins(path):
    by = collections.defaultdict(dict)
    for r in csv.DictReader(open(path)):
        name, stop = r["id"].split("@")
        by[name][stop] = r
    assert len(by) == 7, sorted(by)
    return by
common, placed = twins(sys.argv[1]), twins(sys.argv[2])
for name, pair in common.items():
    assert pair["10"]["lift"] == pair["11"]["lift"], \
        f"--cells common: {name} scored {pair['10']['lift']} at row 10 and " \
        f"{pair['11']['lift']} at row 11; the stop row still reaches the score"
    assert pair["10"]["cells"] == pair["11"]["cells"], name
assert any(p["10"]["lift"] != p["11"]["lift"] for p in placed.values()), \
    "--cells placed no longer separates the stop rows, so the common-cell " \
    "test above is proving nothing"
print("ok: --cells common makes a board's stop row invisible to the score, "
      "and --cells placed still does not")
EOF

    # --border_out emits one row per corner class, and each row has to SEAT the
    # corners its own comment claims. Get that mapping wrong -- CLI order BL BR
    # TR TL against the annealer's TL TR BR BL -- and every number downstream
    # still looks healthy while the border is wrong.
    python3 tools/E555_extract_consensus.py "$OUT/cons_corpus.csv" \
        --border_out "$OUT/cons_classes.csv" --min_corner_boards 4 \
        --border_time 6 --min_trails 1 --quiet > "$OUT/cons_classes.log" 2>&1
    python3 tools/E555_rotate.py --rotations "$OUT/cons_classes.csv" 0 \
        --out "$OUT/cons_classes_rot.csv" > "$OUT/cons_classes_rot.log"
    grep -q "border row(s) turned" "$OUT/cons_classes_rot.log" \
        || { cat "$OUT/cons_classes_rot.log"; fail "an emitted class row is not a legal partition"; }
    python3 - "$OUT/cons_classes.csv" <<'EOF' || exit 1
import sys, re
sys.path.insert(0, "tools")
import E555_viewer as V
seed = V.load_seed("data/seed_Edge5.txt")
N, E, S, W = 0, 1, 2, 3
WHERE = {frozenset((S, W)): "BL", frozenset((S, E)): "BR",
         frozenset((N, E)): "TR", frozenset((N, W)): "TL"}
claims, rows = [], []
for line in open(sys.argv[1]):
    if line.startswith("#"):
        m = re.match(r"#  (BL=\d+ BR=\d+ TR=\d+ TL=\d+)", line)
        if m:
            claims.append({k: int(v) for k, v in
                           (kv.split("=") for kv in m.group(1).split())})
    elif line.strip():
        rows.append([int(x) for x in line.split(",")[1:]])
assert rows and len(rows) == len(claims), (len(rows), len(claims))
for i, (claim, spins) in enumerate(zip(claims, rows)):
    seat = {}
    for pid, e in enumerate(seed):
        shown = V.rotate_edges(e, spins[pid])
        grey = frozenset(d for d in range(4) if shown[d] == 0)
        if len(grey) == 2:
            seat[WHERE[grey]] = pid
    assert seat == claim, f"row {i}: claimed {claim}, seated {seat}"
print(f"ok: {len(rows)} corner-class row(s), each seating the corners it claims")
EOF

    # --border_out also lays the 60 border pieces onto their cells, as a board
    # the finalizer can lock. The rotations row only says which SIDE each piece
    # belongs on; turning that into a frame means choosing one Euler trail per
    # side, and a side walked backwards would still pass every structural check
    # while handing Stage C a broken border. rank.py is the independent judge:
    # border==60 is only reachable when all 60 cells are placed, frame-legal,
    # and every one of the 60 seams matches.
    frame="$OUT/cons_border_frame.csv"
    [ -s "$frame" ] || fail "--border_out wrote no companion frame file"
    for field in border score placed; do
        got=$(python3 tools/E555_rank.py "$frame" --field "$field")
        [ "$got" = "60" ] || fail "the laid-out frame has $field=$got, want 60"
    done
    nb=$(grep -c '^c' "$OUT/cons_border.csv")
    nf=$(grep -c '^c' "$frame")
    [ "$nb" = "$nf" ] || fail "$nb border row(s) but $nf frame row(s); they must correspond"
    fields=$(grep -v '^#' "$frame" | head -1 | awk -F, '{print NF}')
    [ "$fields" = "514" ] || fail "frame row has $fields fields, want 514"

    # The finalizer itself is NOT run here. --finalize_from 0 locks only row 0,
    # so it rebuilds essentially the whole 6.44 GB inner database -- 68 of the
    # 80 seconds that invocation costs, for an assertion this check already
    # makes: fin_pos_border_complete() tests exactly that row 0, row 15, column
    # 0 and column 15 are occupied, and border==60 above is strictly stronger
    # (it needs those 60 cells placed, frame-legal AND every seam matched).
    # The end-to-end run was done by hand and reported mode=fixed, lock rows
    # 0..0, 145053 boards grown from row 1.
    echo "ok: the laid-out frame is a clean 60-seam border, 1:1 with the rotations rows"

    # No class can clear the default bar on a 28-board fixture, and that must be
    # a refusal with the largest count named -- not a border built from noise.
    if python3 tools/E555_extract_consensus.py "$OUT/cons_corpus.csv" \
            --border_out "$OUT/cons_noise.csv" > "$OUT/cons_noise.log" 2>&1; then
        fail "--border_out emitted a border with no class clearing --min_corner_boards"
    fi
    grep -q "no corner class has the --min_corner_boards" "$OUT/cons_noise.log" \
        || { tail -3 "$OUT/cons_noise.log"; fail "the min_corner_boards refusal did not fire"; }

    # The corner filter selects in the canonical frame, and an unplaced corner
    # cannot contradict it.
    python3 tools/E555_extract_consensus.py "$OUT/cons_corpus.csv" --BL 1 --BR 2 \
        --csv --quiet > "$OUT/cons_bl.csv"
    kept=$(($(wc -l < "$OUT/cons_bl.csv") - 1))
    [ "$kept" -gt 0 ] && [ "$kept" -lt 28 ] \
        || fail "--BL/--BR kept $kept of 28 boards, expected a proper subset"
    echo "ok: corner classes, the seating check, and the corner filter"

    # A corpus with no centre clue is refused, loudly and nonzero: the whole
    # tool rests on being able to read a board's clue frame.
    if python3 tools/E555_extract_consensus.py data/best_463.csv \
            > "$OUT/cons_unclued.log" 2>&1; then
        fail "an unclued corpus was accepted"
    fi
    grep -q "no board in the input carries the centre clue" "$OUT/cons_unclued.log" \
        || fail "the unclued corpus failed for the wrong reason"
    echo "ok: an unclued corpus is refused"
}

step_annealer() {
    rm -f "$OUT/rotations.csv"
    python3 -u src/A_border/E555_edge_annealer.py data/seed_Edge5.txt \
        --restarts 2 --steps 3000 --rng_seed 42 --verbose \
        --out "$OUT/rotations.csv" > "$OUT/annealer.log"
    grep -c "^BEST," "$OUT/annealer.log" | grep -q "^2$" || fail "expected 2 BEST lines"
    python3 - "$OUT/rotations.csv" <<'EOF' || exit 1
import sys
rows = [l for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")]
assert len(rows) == 2, f"expected 2 data rows, got {len(rows)}"
for l in rows:
    f = l.replace(","," ").split()
    spins = [int(x) for x in f[1:]]
    assert len(spins) == 256, f"need id+256 spins, got {len(spins)}"
    assert all(0 <= s <= 3 for s in spins), "spin out of range"
    assert spins[60:] == [0]*196, "inner pads must be zero"
print("ok: 2 beamer-format rotation rows")
EOF

    # The spins are the deliverable and the comment is the only description of
    # them, so they have to agree: reconstruct each row's border from its spins
    # alone and recompute the four counts. This is what catches a best recorded
    # from a CANDIDATE swap whose rotation vector was assembled wrongly -- the
    # counts would still look plausible, and Stage B would search a border
    # nobody scored. Corner swaps and edge swaps both take that path.
    python3 - "$OUT/rotations.csv" <<'EOF' || exit 1
import re, sys
sys.path.insert(0, "src/A_border")
import E555_edge_annealer as A

pieces = A.read_pieces("data/seed_Edge5.txt")
pbi    = {p.id: p for p in pieces}
cap    = A.build_inner_capacity(pieces)
cfg    = A.AnnealingConfig()
rows   = A.read_rotations(sys.argv[1])
assert len(rows) == 2, f"expected 2 rows, got {len(rows)}"
for i, (spins, comment, lineno) in enumerate(rows):
    state = A._build_run_state(pbi, *A.border_from_spins(pbi, spins), cap, cfg)
    got  = {A.SIDE_NAMES[s]: state.evals[s].euler_count for s in A.Side}
    want = A.counts_in_comment(comment)
    assert want == got, f"row {i} (line {lineno}): comment {want} != spins {got}"
print("ok: both rows' spins reproduce the counts their comments claim")
EOF
}

# --input hands the annealer a border it did not find itself, which means two
# things can now go wrong silently. A row reconstructed against the wrong seed
# file would anneal a different board than the one named -- so the four trail
# counts are recomputed from the spins and checked against the comment, and
# that check is what this step exercises on every shipped row. And a warm run
# that came back WORSE than its input would quietly poison a refined pool: the
# starting border is seeded as the restart's first best precisely so it cannot,
# and the run summary states the count, so the guarantee is asserted, not
# assumed. Uses data/borders_annealed_fix12.csv, which is shipped (so this step
# consumes no other step's artifacts) and carries the older comma comment form,
# so the two-form comment reader is covered here too.
step_annealer_refine() {
    local IN=data/borders_annealed_fix12.csv
    local A=src/A_border/E555_edge_annealer.py
    rm -f "$OUT/refined.csv"

    python3 - "$IN" <<'EOF' || exit 1
import sys
sys.path.insert(0, "src/A_border")
import E555_edge_annealer as A

pieces = A.read_pieces("data/seed_Edge5.txt")
pbi    = {p.id: p for p in pieces}
cap    = A.build_inner_capacity(pieces)
cfg    = A.AnnealingConfig()
rows   = A.read_rotations(sys.argv[1])
assert len(rows) == 12, f"expected 12 data rows, got {len(rows)}"
for i, (spins, comment, lineno) in enumerate(rows):
    edge_side, corner_pos = A.border_from_spins(pbi, spins)
    per = {s: sum(1 for v in edge_side.values() if v == s) for s in A.Side}
    assert all(per[s] == A.EDGE_PER_SIDE for s in A.Side), f"row {i}: {per}"
    assert len(set(corner_pos.values())) == 4, f"row {i}: corners {corner_pos}"
    state = A._build_run_state(pbi, edge_side, corner_pos, cap, cfg)
    got  = {A.SIDE_NAMES[s]: state.evals[s].euler_count for s in A.Side}
    want = A.counts_in_comment(comment)
    assert want is not None, f"row {i} (line {lineno}): comma-form counts not read"
    assert want == got, f"row {i} (line {lineno}): comment {want} != recomputed {got}"
    assert A.hard_penalty(state.evals, state.inward_tally, cap, cfg) == 0.0, \
        f"row {i}: reconstructed border is not feasible"
print(f"ok: {len(rows)} legacy-form rows round-trip through their spins")
EOF

    # Naming a row that is not there, and naming the corners twice, are both
    # refusals -- a warm start that silently fell back to row 0 or to random
    # corners would be worse than no warm start at all.
    python3 -u "$A" data/seed_Edge5.txt --input "$IN" --row 99 \
        --restarts 1 --steps 1 > "$OUT/refine_row99.log" 2>&1 \
        && fail "--row 99 was accepted"
    grep -q "12 data row" "$OUT/refine_row99.log" \
        || fail "--row out of range did not say how many rows the file holds"
    python3 -u "$A" data/seed_Edge5.txt --input "$IN" --fix_corners 1 \
        --restarts 1 --steps 1 > "$OUT/refine_clash.log" 2>&1 \
        && fail "--fix_corners together with --input was accepted"
    python3 -u "$A" data/seed_Edge5.txt --row 2 \
        --restarts 1 --steps 1 > "$OUT/refine_norow.log" 2>&1 \
        && fail "--row without --input was accepted"

    # The schedule is probed once in the parent, so it must depend on
    # --rng_seed alone: same seed, same sigma, whatever the thread count.
    for t in 1 2; do
        python3 -u "$A" data/seed_Edge5.txt --input "$IN" --row 1 --rng_seed 42 \
            --threads "$t" --restarts 1 --steps 1 2>&1 | grep "move scale sigma"
    done > "$OUT/refine_sigma.txt"
    [ "$(sort -u "$OUT/refine_sigma.txt" | wc -l)" = "1" ] \
        || { cat "$OUT/refine_sigma.txt"; fail "the probe is not reproducible"; }
    grep -q "polishing:" "$OUT/refine_sigma.txt" \
        || { cat "$OUT/refine_sigma.txt"; fail "a row already suited to these weights did not get the polishing schedule"; }

    # The same row, scored against targets it was never annealed for, is not a
    # refinement target any more and must get the cold schedule instead. This
    # is the one branch the polish/search rule exists to make: a cool schedule
    # here sat stuck 35 points below what a hot one reached.
    python3 -u "$A" data/seed_Edge5.txt --input "$IN" --row 1 --restarts 1 --steps 1 \
        --target_scale 250 --w_top 60 --w_right 20 --w_bottom 20 --w_left 1 \
        > "$OUT/refine_search.log" 2>&1 || { cat "$OUT/refine_search.log"; fail "target-mode warm start failed"; }
    grep -q "schedule: searching:" "$OUT/refine_search.log" \
        || { grep schedule "$OUT/refine_search.log"; fail "a row far from these weights still got the polishing schedule"; }

    python3 -u "$A" data/seed_Edge5.txt --input "$IN" --row 1 \
        --restarts 3 --steps 4000 --rng_seed 42 --threads 2 \
        --out "$OUT/refined.csv" > "$OUT/refine.log" 2>&1 \
        || { cat "$OUT/refine.log"; fail "the refine run failed"; }
    grep -q "3/3 restarts matched or beat the input row" "$OUT/refine.log" \
        || { cat "$OUT/refine.log"; fail "a refinement lost ground against its input"; }

    python3 - "$OUT/refined.csv" "$IN" <<'EOF' || exit 1
import sys
sys.path.insert(0, "src/A_border")
import E555_edge_annealer as A

pieces = A.read_pieces("data/seed_Edge5.txt")
pbi    = {p.id: p for p in pieces}
cap    = A.build_inner_capacity(pieces)
cfg    = A.AnnealingConfig()
src    = A.read_rotations(sys.argv[2])
base   = A._build_run_state(pbi, *A.border_from_spins(pbi, src[1][0]), cap, cfg).score

rows = A.read_rotations(sys.argv[1])
assert len(rows) == 3, f"expected 3 refined rows, got {len(rows)}"
raw = [l.rstrip("\n") for l in open(sys.argv[1]) if l.lstrip().startswith("#")]
assert any("input=" in l and "row=1" in l for l in raw), "the # run marker lost --input/--row"
for i, (spins, comment, lineno) in enumerate(rows):
    assert "From=" in comment and ":row1" in comment, f"row {i}: no provenance: {comment}"
    assert spins[60:] == [0] * 196, f"row {i}: inner pads must stay zero"
    state = A._build_run_state(pbi, *A.border_from_spins(pbi, spins), cap, cfg)
    assert A.hard_penalty(state.evals, state.inward_tally, cap, cfg) == 0.0, \
        f"row {i}: emitted an unusable border"
    got  = {A.SIDE_NAMES[s]: state.evals[s].euler_count for s in A.Side}
    want = A.counts_in_comment(comment)
    assert want == got, f"row {i}: comment {want} != recomputed {got}"
    assert state.score >= base - 1e-12, \
        f"row {i}: scored {state.score:.4f} below its input's {base:.4f}"
print(f"ok: 3 refined rows, all provenant and none below the input's {base:.4f}")
EOF
}

# The tool reads a score and four trail counts out of the annealer's PROSE, and
# it can now turn each row by its own angle. Both halves fail silently: an
# unreadable comment sorts the row last and degenerates the order to the input's
# (which is what data/borders_annealed_fix12.csv's older comma form did to every
# one of its rows), and a relabelled comment looks perfectly plausible whatever
# the spins actually did. So this checks the numbers against a second reader and
# the spins against the tool that already owns rotations turns.
step_sort_rotations() {
    src=data/borders_annealed_fix12.csv
    SR="python3 tools/E555_sort_rotations.py"

    # The same 12 borders with their comments rewritten into the annealer's `=`
    # form. Two readers, one set of numbers: the orders have to agree.
    sed -E 's/Score,([0-9.]+)/Score=\1/; s/(TOP|RIGHT|BOTTOM|LEFT),([0-9]+)/\1=\2/g' \
        "$src" > "$OUT/sr_eq.csv"
    grep -q 'TOP=' "$OUT/sr_eq.csv" || fail "the = fixture was not rewritten"

    # Without --out the file goes to stdout -- it used to go nowhere at all,
    # taking --top with it -- and stdout must stay free of diagnostics.
    $SR "$src" > "$OUT/sr_comma.csv" 2>/dev/null || fail "sort to stdout exited nonzero"
    n=$(grep -cv '^ *#' "$OUT/sr_comma.csv")
    [ "$n" = "12" ] || fail "stdout carried $n row(s), expected 12"
    n=$($SR "$src" --top 4 2>/dev/null | grep -cv '^ *#')
    [ "$n" = "4" ] || fail "--top 4 kept $n row(s) on stdout"

    for f in "$src" "$OUT/sr_eq.csv"; do
        $SR "$f" > /dev/null 2>"$OUT/sr.err"
        grep -q 'no readable' "$OUT/sr.err" && fail "$f: a comment went unread"
    done
    $SR "$OUT/sr_eq.csv" 2>/dev/null | grep -v '^ *#' > "$OUT/sr_eq_out.csv"
    cmp -s <(grep -v '^ *#' "$OUT/sr_comma.csv") "$OUT/sr_eq_out.csv" \
        || fail "the two comment forms sorted the same borders differently"

    # --max_top must leave every row showing its own largest count on top, with
    # the four numbers only permuted; --sort min_side must invert the file.
    $SR "$OUT/sr_eq.csv" --max_top --seed_file data/seed_Edge5.txt \
        -o "$OUT/sr_maxtop.csv" 2>/dev/null || fail "--max_top exited nonzero"
    n=$(grep -cv '^ *#' "$OUT/sr_maxtop.csv")
    [ "$n" = "12" ] || fail "--max_top dropped rows: $n of 12 survived"
    python3 - "$OUT/sr_eq.csv" "$OUT/sr_maxtop.csv" <<'EOF' || exit 1
import re, subprocess, sys
KEY = re.compile(r'(TOP|RIGHT|BOTTOM|LEFT)=(\d+)')
def sides(p):
    return [dict((k, int(v)) for k, v in KEY.findall(l))
            for l in open(p) if l.lstrip().startswith('#') and 'TOP=' in l]
src, turned = sides(sys.argv[1]), sides(sys.argv[2])
assert len(src) == len(turned) == 12, (len(src), len(turned))
for v in turned:
    assert v['TOP'] == max(v.values()), f"largest count is not on top: {v}"
assert sorted(tuple(sorted(v.values())) for v in src) == \
       sorted(tuple(sorted(v.values())) for v in turned), \
       "the turned counts are not a permutation of the originals"
# --sort min_side asks the opposite question from --sort score, so on a file
# the annealer built to score well it has to come back in a different order.
def ids(*flags):
    out = subprocess.run(["python3", "tools/E555_sort_rotations.py", sys.argv[1], *flags],
                         capture_output=True, text=True, check=True).stdout
    return [l.split(",")[0] for l in out.splitlines() if not l.lstrip().startswith("#")]
by_score, by_tight = ids("--sort", "score"), ids("--sort", "min_side")
assert sorted(by_score) == sorted(by_tight), "a sort key lost or invented a row"
assert by_score != by_tight, "--sort min_side reproduced the score order"
print(f"ok: 12 borders turned onto their own best side, {len(set(by_score))} ids intact")
EOF

    # The spins, not just the prose: four quarter-turns by the tool that owns
    # rotations turns must bring the turned file back to itself.
    prev="$OUT/sr_maxtop.csv"
    for t in 1 2 3 4; do
        python3 tools/E555_rotate.py "$prev" 1 --rotations \
            --seed_file data/seed_Edge5.txt --out "$OUT/sr_t$t.csv" > /dev/null \
            || fail "rotating the turned file failed at turn $t"
        prev="$OUT/sr_t$t.csv"
    done
    grep -v '^ *#' "$OUT/sr_maxtop.csv" | tr -d ' ' > "$OUT/sr_a.spins"
    grep -v '^ *#' "$OUT/sr_t4.csv"     | tr -d ' ' > "$OUT/sr_b.spins"
    cmp -s "$OUT/sr_a.spins" "$OUT/sr_b.spins" \
        || fail "four turns of the --max_top file did not return its own spins"
    echo "ok: both comment forms agree, --max_top permutes the counts, four turns are the identity"
}

# The strongest correctness proof in the repo: the beam machinery, the database,
# parity pruning and emission all have to be right for this to pass.
step_finalizer_synth() {
    bin/E555_finalizer data/synth_seed.txt data/synth_solution_480.csv \
        --finalize_from 10 --stop_row 14 --beam_width 20000 --frac_rand 0 \
        --rng_seed 1 --out_dir "$OUT/fin" > "$OUT/finalizer.log"
    comp="$OUT/fin/beam_completions_finalized_14.csv"
    [ -s "$comp" ] || { tail -5 "$OUT/finalizer.log"; fail "no boards emitted"; }
    python3 - "$comp" data/synth_solution_480.csv <<'EOF' || exit 1
import sys
def rows(p): return [l.split(",") for l in open(p) if l.strip() and not l.startswith("#")]
truth = rows(sys.argv[2])[0]
tpos, trot = truth[-512:-256], truth[-256:]
hit = False
for r in rows(sys.argv[1]):
    pos, rot = r[-512:-256], [x.strip() for x in r[-256:]]
    # every piece the finalizer placed must sit exactly where the truth put it
    # (row 15, the top border, is deliberately left unplaced at stop_row 14)
    ok = all(p.strip() == "999" or (p.strip() == tp.strip() and ro == tr.strip())
             for p, ro, tp, tr in zip(pos, rot, tpos, trot))
    hit = hit or ok
assert hit, "no emitted board matches the known solution"
print("ok: known solution rediscovered")
EOF
}

# A partial with an incomplete border normally falls back to --free_edges, where
# all 56 edges are candidates for every side. Given the Stage A rotations row the
# board came from, the finalizer must recognize it from the LOCKED border alone
# and re-impose that row's piece->side assignment -- far fewer left columns, same
# answer. stop_row 12 (< 14) also covers the left-interface demand accounting,
# which the exhaustive enumerator leaves partly unfixed.
step_finalizer_rotations() {
    python3 - data/synth_seed.txt data/synth_solution_480.csv \
             "$OUT/rot_row.csv" "$OUT/rot_partial.csv" <<'EOF' || exit 1
import sys
seed = [list(map(int, l.split())) for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")]
row  = [f.strip() for f in open(sys.argv[2]).read().strip().split(",")]
pos, rot = list(map(int, row[-512:-256])), list(map(int, row[-256:]))
border = [sum(1 for x in p if x == 0) > 0 for p in seed]
# Stage A rotations row: the solution's 60 border spins, 196 inner zeros.
spins = [rot[p] if border[p] else 0 for p in range(256)]
open(sys.argv[3], "w").write("# derived from synth_solution_480\n"
                             "synthrot," + ",".join(map(str, spins)) + "\n")
# The same board with everything above row 10 unplaced: border incomplete.
p2, r2 = pos[:], rot[:]
for p in range(256):
    if pos[p] // 16 > 10: p2[p], r2[p] = 999, 0
open(sys.argv[4], "w").write("synthp10,0," + ",".join(map(str, p2)) + ","
                             + ",".join(map(str, r2)) + "\n")
EOF
    for sr in 12 14; do
        for mode in free rot; do
            rotarg=""; [ "$mode" = rot ] && rotarg="$OUT/rot_row.csv"
            bin/E555_finalizer data/synth_seed.txt "$OUT/rot_partial.csv" $rotarg \
                --finalize_from 10 --stop_row "$sr" --top_columns 0 --frac_rand 0 \
                --beam_width 20000 --rng_seed 1 --out_dir "$OUT/rot_${mode}_$sr" \
                > "$OUT/rot_${mode}_$sr.log"
        done
        nf=$(grep -oE 'enumerated [0-9]+' "$OUT/rot_free_$sr.log" | grep -oE '[0-9]+')
        nr=$(grep -oE 'enumerated [0-9]+' "$OUT/rot_rot_$sr.log"  | grep -oE '[0-9]+')
        grep -q "matches rotations row 0" "$OUT/rot_rot_$sr.log" \
            || fail "stop_row $sr: rotations row was not matched"
        [ -n "$nr" ] && [ "$nr" -ge 1 ] || fail "stop_row $sr: constrained run enumerated no column"
        [ "$nr" -lt "$nf" ] || fail "stop_row $sr: constrained columns $nr not fewer than free $nf"
        comp="$OUT/rot_rot_$sr/beam_completions_finalized_$sr.csv"
        [ -s "$comp" ] || { tail -5 "$OUT/rot_rot_$sr.log"; fail "stop_row $sr: no boards emitted"; }
        echo "ok: stop_row $sr -- left columns $nf -> $nr, boards emitted"
    done
    # The constrained stop_row-14 run must still contain the known solution.
    python3 - "$OUT/rot_rot_14/beam_completions_finalized_14.csv" data/synth_solution_480.csv <<'EOF' || exit 1
import sys
def rows(p): return [l.split(",") for l in open(p) if l.strip() and not l.startswith("#")]
truth = rows(sys.argv[2])[0]
tpos, trot = truth[-512:-256], truth[-256:]
hit = any(all(p.strip() == "999" or (p.strip() == tp.strip() and ro.strip() == tr.strip())
              for p, ro, tp, tr in zip(r[-512:-256], r[-256:], tpos, trot))
          for r in rows(sys.argv[1]))
assert hit, "constrained run lost the known solution"
print("ok: known solution still rediscovered with sides constrained")
EOF
    # A rotations file that describes a different border must be rejected, not
    # misapplied: the run falls back to free mode and behaves exactly as before.
    bin/E555_finalizer data/seed_Edge5.txt data/board_partial_row12.csv \
        data/borders_annealed_fix12.csv --finalize_from 10 --stop_row 11 \
        --top_columns 2 --beam_width 2048 --rng_seed 1 --wall_time 60 \
        --out_dir "$OUT/rot_nomatch" > "$OUT/rot_nomatch.log" || true
    grep -q "no rotations row matches the locked border" "$OUT/rot_nomatch.log" \
        || fail "a non-matching rotations file was not rejected"
    grep -q "mode=free" "$OUT/rot_nomatch.log" || fail "no-match did not fall back to free mode"
    echo "ok: non-matching rotations file falls back to free mode"
}

# A randomized, threaded beam that cannot be reproduced cannot be bisected: a
# regression shows up as a run that went differently, with no way to tell a real
# change from the scheduler. Two runs on one seed must produce the same search,
# configuration for configuration.
#
# What this fixture had to get right, and nearly did not:
#
#   * --beam_width 200, not the 20000 the other synthetic checks use. Above the
#     candidate pool, select_beam returns early and the entire selection path --
#     score, random band, parent cap -- never runs. At 20000 on this board it
#     never ran: two DIFFERENT seeds produced byte-identical output, so the
#     check would have passed with the RNG disconnected.
#   * --finalize_from 5, so several rows clear the 64-sample Mahalanobis floor
#     and the per-thread reduction behind it is actually exercised.
#
# Both arms fix --threads. Reproducibility across thread counts is deliberately
# NOT asserted, because the finalizer does not offer it: the work partition
# follows the thread count, and a beam that keeps a bounded number of candidates
# keeps a different subset from a different partition -- at 2 vs 4 threads this
# board scores 8731 candidates against 9009. Same count, same seed, same answer
# is the contract; reproducing a run means recording --threads with the seed.
step_finalizer_determinism() {
    for arm in a b; do
        bin/E555_finalizer data/synth_seed.txt data/synth_solution_480.csv \
            --finalize_from 5 --stop_row 10 --beam_width 200 --frac_rand 0.30 \
            --finalize_repeats 2 --threads 4 --rng_seed 777 --verbose \
            --out_dir "$OUT/det_$arm" > "$OUT/det_$arm.log" \
            || { tail -5 "$OUT/det_$arm.log"; fail "the finalizer failed on arm $arm"; }
        # keep the lines that describe the SEARCH, drop every timing field
        grep -E "^\[sweep\] p|^\[sum\] (configs|rows|extinctions|emitted|maha)" \
            "$OUT/det_$arm.log" \
          | sed -E 's/wall=[0-9.]+s//g
                    s/\([0-9.]+ s\/config, [0-9.]+ configs\/hour\)//g
                    s/\([0-9.]+ Mcand\/s in expand\)//g' > "$OUT/det_$arm.norm"
    done

    # Three guards against a check that passes without testing anything -- the
    # state the first version of this check was actually in.
    n=$(grep -c "^\[sweep\] p" "$OUT/det_a.norm" || true)
    [ "${n:-0}" -ge 2 ] || fail "only $n configurations captured (want >= 2)"
    grep -q "maha sd by row:  r" "$OUT/det_a.norm" \
        || fail "no row cleared the Mahalanobis sample floor -- its reduction went unchecked"
    # If selection were inert the repeats would be identical to each other, and
    # two runs matching would prove nothing.
    [ "$(sort -u "$OUT/det_a.norm" | grep -c "^\[sweep\] p" || true)" -ge 2 ] \
        || fail "every repeat searched identically -- the random band is not running"

    diff "$OUT/det_a.norm" "$OUT/det_b.norm" > "$OUT/det.diff" \
        || { head -10 "$OUT/det.diff"; fail "two runs on one seed searched differently"; }
    echo "ok: $n configurations reproduced exactly on a re-run"
}

# The roundhouse rebuilds a board from a rotated frame, so a wrong rotation, a
# wrong spin transform or an off-by-one in the strip geometry all surface here
# as "solution not found". Two widths are exercised because W sets the chain
# length, the cell index and the oracle's state space at once.
#
# --ties 50 because the search is exhaustive and a band usually has several
# break-free refills: with the default --ties 1 only ONE of them is written, and
# the known solution need not be the one picked.
step_roundhouse_synth() {
    rh_fixtures
    for spec in "rh_rows12.csv:0" "rh_rows10.csv:5" "rh_damaged.csv:0"; do
        src="${spec%%:*}"; w="${spec##*:}"
        # Third positional = the file every emitted board goes to. Named per
        # fixture because two specs share W=0 and would otherwise collide.
        comp="$OUT/rh_${src%.csv}_$w.csv"
        bin/E555_roundhouse data/synth_seed.txt "$OUT/$src" "$comp" --rounds 1 \
            --strip_width "$w" --ties 50 > "$OUT/roundhouse_$w.log"
        [ -s "$comp" ] || { tail -5 "$OUT/roundhouse_$w.log"; fail "roundhouse ($src) emitted nothing"; }
        python3 - "$comp" data/synth_solution_480.csv "$src" <<'EOF' || exit 1
import sys
def rows(p): return [l.split(",") for l in open(p) if l.strip() and not l.startswith("#")]
truth = rows(sys.argv[2])[0]
tpos, trot = [x.strip() for x in truth[-512:-256]], [x.strip() for x in truth[-256:]]
hit = False
for r in rows(sys.argv[1]):
    pos, rot = [x.strip() for x in r[-512:-256]], [x.strip() for x in r[-256:]]
    assert "999" not in pos, "a --rounds 1 completion must be a full 256-piece board"
    hit = hit or (pos == tpos and rot == trot)
assert hit, "no emitted board matches the known solution (%s)" % sys.argv[3]
print("ok: known solution rediscovered from %s" % sys.argv[3])
EOF
    done
    # The complement: breaks are tolerated only where the run frees them. One
    # inside the core has to be refused, or every strip would be grown against
    # a lie.
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_corebreak.csv" "$OUT/rh_cb.csv" \
        --rounds 1 --strip_width 0 > "$OUT/roundhouse_corebreak.log" 2>&1 || true
    grep -q "break in kept shape" "$OUT/roundhouse_corebreak.log" || \
        { cat "$OUT/roundhouse_corebreak.log"; fail "a break inside the core was not refused"; }
    # The output file is created and left empty when nothing is emitted.
    if [ -s "$OUT/rh_cb.csv" ]; then fail "a core-break board must emit nothing"; fi
    echo "ok: a break inside the core is refused, one in the freed band is ignored"
}

# --rounds 2 frees the right and top bands and refills both, so like --rounds 1
# it ends on a COMPLETE board -- and unlike --rounds 1 it exercises the rotation
# between rounds, an open-topped strip followed by a closed one. If a board can
# be finished in two rounds this is what has to see it.
step_roundhouse_two_rounds() {
    rh_fixtures
    comp="$OUT/rh_r2.csv"
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows10.csv" "$comp" --rounds 2 \
        --strip_width 5 --ties 50 --wall_time 300 > "$OUT/roundhouse_r2.log"
    [ -s "$comp" ] || { tail -5 "$OUT/roundhouse_r2.log"; fail "two-round run emitted nothing"; }
    python3 - "$comp" data/synth_solution_480.csv <<'EOF' || exit 1
import sys
def rows(p): return [l.split(",") for l in open(p) if l.strip() and not l.startswith("#")]
truth = rows(sys.argv[2])[0]
tpos, trot = [x.strip() for x in truth[-512:-256]], [x.strip() for x in truth[-256:]]
full = hit = 0
for r in rows(sys.argv[1]):
    pos, rot = [x.strip() for x in r[-512:-256]], [x.strip() for x in r[-256:]]
    if "999" not in pos: full += 1
    hit += (pos == tpos and rot == trot)
assert full, "a completed two-round run must leave no cell unplaced"
assert hit, "no emitted board matches the known solution"
print("ok: %d complete board(s), the known solution among them" % full)
EOF
}

# Every prune in the strip search reads a chain record's successor signature.
# The transition cache decodes each record once when the database is built; with
# --no_transition_cache the DFS and both oracles decode on the fly instead. The
# two paths must agree EXACTLY, because a wrong cached successor does not crash
# -- it silently refutes live branches and the run still reports a clean proof.
# So the whole emitted set is compared byte for byte, not a summary line.
#
# This replaced the old --selfcheck check, which validated the endpoint oracle
# against brute-force enumeration inside the binary. That machinery was removed
# with the rewrite; the depth oracle it grew instead has no in-binary proof yet,
# and writing one is still open work.
step_roundhouse_cache() {
    rh_fixtures
    for w in 3 5; do
        bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows12.csv" "$OUT/rh_cache_$w.csv" \
            --rounds 1 --strip_width "$w" --ties 50 > "$OUT/roundhouse_cache_$w.log"
        bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows12.csv" "$OUT/rh_nocache_$w.csv" \
            --rounds 1 --strip_width "$w" --ties 50 --no_transition_cache \
            > "$OUT/roundhouse_nocache_$w.log"
        [ -s "$OUT/rh_cache_$w.csv" ] || \
            { tail -5 "$OUT/roundhouse_cache_$w.log"; fail "W=$w cached run emitted nothing"; }
        cmp -s "$OUT/rh_cache_$w.csv" "$OUT/rh_nocache_$w.csv" || \
            fail "W=$w: the transition cache changed the emitted boards"
    done
    echo "ok: cached and decoded successors agree at W=3 and W=5"
}

# --cw mirrors the seed left-right, runs the unchanged right/top/left spiral
# in that mirror, and mirrors every emitted board back. Four maps have to compose
# to the identity for that to work -- the seed, the clue table, the board in and
# the board out -- and rebuilding the KNOWN SOLUTION is what proves they do.
# Break-freeness alone could not: a mirror preserves every match, so a board
# emitted still mirrored would score a clean 480 and look perfectly fine. Only
# equality with data/synth_solution_480.csv catches a missing un-mirror.
step_roundhouse_cw() {
    rh_fixtures
    for spec in "rh_rows12.csv:0" "rh_rows10.csv:5"; do
        src="${spec%%:*}"; w="${spec##*:}"
        comp="$OUT/rh_cw_$w.csv"
        bin/E555_roundhouse data/synth_seed.txt "$OUT/$src" "$comp" --cw --rounds 1 \
            --strip_width "$w" --ties 50 > "$OUT/roundhouse_cw_$w.log"
        [ -s "$comp" ] || { tail -5 "$OUT/roundhouse_cw_$w.log"; fail "--cw ($src) emitted nothing"; }
        python3 - "$comp" data/synth_solution_480.csv "$src" <<'EOF' || exit 1
import sys
def rows(p): return [l.split(",") for l in open(p) if l.strip() and not l.startswith("#")]
truth = rows(sys.argv[2])[0]
tpos, trot = [x.strip() for x in truth[-512:-256]], [x.strip() for x in truth[-256:]]
hit = False
for r in rows(sys.argv[1]):
    pos, rot = [x.strip() for x in r[-512:-256]], [x.strip() for x in r[-256:]]
    assert "999" not in pos, "a --rounds 1 completion must be a full 256-piece board"
    hit = hit or (pos == tpos and rot == trot)
assert hit, "the mirrored search did not rebuild the known solution (%s)" % sys.argv[3]
print("ok: mirrored spiral rebuilt the known solution from %s" % sys.argv[3])
EOF
    done

    # The spiral really turned round. --rotate names the side round 1 attacks and
    # keeps it for odd K, so it is rounds 2 and 3 that have to diverge. Nothing is
    # searched here: --start_row is past the end of the one-line CSV. The side
    # order is the middle field of the banner:
    #   Search: CCW; TOP > LEFT > BOTTOM; rounds=3; W=3; hold=off; ...
    plan() {
        bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows12.csv" "$OUT/rh_plan.csv" \
            --rounds 3 --strip_width 3 --rotate 1 --start_row 9 "$@" 2>&1 |
            sed -n 's/^Search: [A-Z]*; \(.*\); rounds=.*/\1/p'
    }
    [ "$(plan --ccw)" = "TOP > LEFT > BOTTOM" ] || \
        fail "the CCW spiral changed: $(plan --ccw)"
    [ "$(plan --cw)" = "TOP > RIGHT > BOTTOM" ] || \
        fail "--cw did not turn the spiral round: $(plan --cw)"
    echo "ok: --rotate 1 refills TOP > LEFT > BOTTOM, and TOP > RIGHT > BOTTOM under --cw"

    # Corner roles are read in the INPUT's coordinates, so --BL still means the
    # board the user handed in. --rotate 3 frees the bottom band either way, and
    # the synthetic solution has piece 95 at (0,0) and piece 31 at (0,15): pinning
    # the right one must find the solution, pinning the other must not.
    #
    # "Must not" is no longer "emits nothing". The final side runs the depth
    # oracle, which keeps the deepest exact prefix when the endpoint cannot be
    # reached -- so the impossible pin yields 253-piece boards with the pinned
    # corner cell empty, not an empty file. The assertion is therefore on the
    # boards: the good pin rebuilds the known solution, the bad pin never
    # produces a complete board and never seats piece 31 at (0,0).
    for spec in "95:1" "31:0"; do
        pid="${spec%%:*}"; want="${spec##*:}"
        got="$OUT/rh_cw_pin$pid.csv"
        bin/E555_roundhouse data/synth_seed.txt data/synth_solution_480.csv "$got" --cw \
            --rounds 1 --strip_width 3 --rotate 3 --ties 50 --BL "$pid" \
            > "$OUT/roundhouse_cw_pin$pid.log"
        python3 - "$got" data/synth_solution_480.csv "$pid" "$want" <<'EOF' || exit 1
import sys
def rows(p):
    try: return [[x.strip() for x in l.split(",")] for l in open(p) if l.strip() and not l.startswith("#")]
    except OSError: return []
got, truth, pid, want = rows(sys.argv[1]), rows(sys.argv[2])[0], int(sys.argv[3]), sys.argv[4]
t = tuple(truth[-512:])
solved = any(tuple(r[-512:]) == t for r in got)
complete = any("999" not in r[-512:-256] for r in got)
seated = any(r[-512:-256][pid] == "0" for r in got)
if want == "1":
    assert solved, "--cw --BL %d did not rebuild the known solution" % pid
else:
    assert not complete, "--cw --BL %d produced a complete board from an impossible pin" % pid
    assert not seated, "--cw --BL %d seated the pinned piece at (0,0) anyway" % pid
print("ok: --BL %d -> %d board(s), solution=%s, complete=%s" % (pid, len(got), solved, complete))
EOF
    done
    echo "ok: --BL still names the input board's bottom-left corner under --cw"

    # A break inside the kept core must be reported on the user's board, not in
    # mirror space. The fixture swaps the pieces at (4,6) and (4,11), so both
    # directions have to name a row-4 cell -- and un-mirroring the column is the
    # only way the reversed run can.
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_corebreak.csv" "$OUT/rh_cw_cb.csv" \
        --cw --rounds 1 --strip_width 3 --rotate 1 \
        > "$OUT/roundhouse_cw_cb.log" 2>&1 || true
    grep -qE "break in kept shape at \(4,[0-9]+\)-\(4,[0-9]+\)" "$OUT/roundhouse_cw_cb.log" || \
        { cat "$OUT/roundhouse_cw_cb.log"; fail "the core break was not reported in the input's coordinates"; }
    echo "ok: a core break is reported on the input board, not on its mirror"
}

# --hold_band stops the last round freeing what is already standing in the FAR
# half of its final side, so two passes can compound instead of the second
# erasing the first. The fixture is the known solution with the near half of the
# right band emptied: rows 8..15 stay standing -- exactly the half the flag
# retains -- and the search refills rows 0..7 beneath them.
#
# The seam between the two halves is deliberately NOT constrained: the retained
# half is a ceiling, not a required endpoint, so a shorter prefix is a legal
# result and a mismatched join is emitted and labelled hold-join. Getting all
# 256 pieces back is still the assertion here, because the held pieces are never
# re-placed and the deepest reach on this fixture is the completion that fits
# them. verify_hold_snapshot() aborts the run if a held piece ever moves, so the
# "held pieces untouched" check below is belt and braces.
step_roundhouse_hold_band() {
    rh_fixtures
    python3 - data/synth_solution_480.csv "$OUT/rh_hold.csv" <<'EOF' || exit 1
import sys
src, dst = sys.argv[1:3]
line = [l for l in open(src) if l.strip() and not l.lstrip().startswith(("#", "%"))][0]
f = [t.strip() for t in line.split(",")]
pos, rot = [int(x) for x in f[-512:-256]], f[-256:]
# Columns 13..15 are the band a W=3 --rounds 1 --rotate 0 run frees; emptying
# rows 0..7 of it leaves rows 8..15 standing as eight whole levels.
free = {r * 16 + c for r in range(8) for c in range(13, 16)}
open(dst, "w").write("held, 0, " + ", ".join(str(999 if p in free else p) for p in pos)
                     + ", " + ", ".join(rot) + "\n")
print("ok: fixture keeps rows 8..15 of the right band, empties rows 0..7")
EOF
    # The [hold] line is a --verbose line; kept=24 is 8 levels x W=3.
    comp="$OUT/rh_hold.out.csv"
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_hold.csv" "$comp" --rounds 1 \
        --strip_width 3 --rotate 0 --hold_band --ties 1 --verbose \
        > "$OUT/roundhouse_hold.log" 2>&1
    grep -qE "\[hold\].*status=ACTIVE.*kept=24" "$OUT/roundhouse_hold.log" || \
        { cat "$OUT/roundhouse_hold.log"; fail "--hold_band did not hold the standing levels"; }
    [ -s "$comp" ] || { tail -5 "$OUT/roundhouse_hold.log"; fail "--hold_band emitted nothing"; }
    python3 - "$comp" "$OUT/rh_hold.csv" data/synth_solution_480.csv <<'EOF' || exit 1
import sys
def row(p):
    l = [x for x in open(p) if x.strip() and not x.lstrip().startswith(("#", "%"))][0]
    f = [t.strip() for t in l.split(",")]
    return f[0], [int(x) for x in f[-512:-256]], f[-256:]
oid, opos, orot = row(sys.argv[1])
_,   hpos, hrot = row(sys.argv[2])
_,   tpos, trot = row(sys.argv[3])
held = [p for p in range(256) if hpos[p] != 999]
moved = [p for p in held if opos[p] != hpos[p] or orot[p] != hrot[p]]
assert not moved, "held pieces were moved: %s" % moved[:5]
assert "999" not in [str(x) for x in opos], "the board is not complete"
assert opos == tpos and orot == trot, "the completion is not the known solution"
assert oid.startswith("held_"), "the input config_id was not kept: %s" % oid
print("ok: 232 held/core pieces untouched, 24 filled to meet them, id %s" % oid)
EOF
    # A band that cannot be held is NOT a refusal: it is freed and searched from
    # nothing, exactly as without the flag, so a damaged band still gets a run.
    fb="$OUT/rh_hold_fb.csv"
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_damaged.csv" "$fb" --rounds 1 \
        --strip_width 3 --hold_band --verbose \
        > "$OUT/roundhouse_hold_fb.log" 2>&1 || true
    grep -qE "\[hold\].*status=FALLBACK" "$OUT/roundhouse_hold_fb.log" || \
        { cat "$OUT/roundhouse_hold_fb.log"; fail "an unholdable band was not reported"; }
    [ -s "$fb" ] || { tail -5 "$OUT/roundhouse_hold_fb.log"; fail "the fallback did not search"; }
    # --stop_row would move the strip's top, which the held block already fixes.
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_hold.csv" "$OUT/rh_hold_bad.csv" \
        --rounds 1 --strip_width 3 --hold_band --stop_row 5 \
        > "$OUT/roundhouse_hold_bad.log" 2>&1 && fail "--hold_band --stop_row was accepted"
    grep -q "already fixes the final-side endpoint" "$OUT/roundhouse_hold_bad.log" || \
        { cat "$OUT/roundhouse_hold_bad.log"; fail "the conflicting pair was not explained"; }
    echo "ok: held levels met, a damaged band falls back and still runs, --stop_row refused"
}

# The roundhouse only ever places pieces matching on every committed side, so a
# break or a frame violation in its output is a bug, not merely a worse board.
step_roundhouse_legal() {
    rh_fixtures
    # --rounds 3 --strip_width 5 is the one cut too wide to exhaust, so this run
    # stops on its budget and the emitted set is whatever it reached. Legality is
    # the assertion, not which boards came out.
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows10.csv" "$OUT/rh_r3.csv" \
        --rounds 3 --strip_width 5 --ties 4 --wall_time 60 > "$OUT/roundhouse_r3.log"
    python3 - data/synth_seed.txt "$OUT/rh_r3.csv" <<'EOF' || exit 1
import sys
seed = [list(map(int, l.split())) for l in open(sys.argv[1]) if l.strip()]
face = lambda p, rot, d: seed[p][(d + rot) % 4]
n = 0
for line in open(sys.argv[2]):
    if not line.strip() or line.lstrip()[0] in "#%": continue
    f = [t.strip() for t in line.split(",")][-512:]
    cell = {}
    for p, x in enumerate(int(v) for v in f[:256]):
        if x != 999: cell[divmod(x, 16)] = (p, int(f[256 + p]))
    for (r, c), (p, ro) in cell.items():
        if (r, c + 1) in cell:
            q, qo = cell[(r, c + 1)]
            assert face(p, ro, 1) == face(q, qo, 3), "break at (%d,%d)-(%d,%d)" % (r, c, r, c + 1)
        if (r + 1, c) in cell:
            q, qo = cell[(r + 1, c)]
            assert face(p, ro, 0) == face(q, qo, 2), "break at (%d,%d)-(%d,%d)" % (r, c, r + 1, c)
        for d, on in ((0, r == 15), (1, c == 15), (2, r == 0), (3, c == 0)):
            assert (face(p, ro, d) == 0) == on, "frame violation at (%d,%d)" % (r, c)
    n += 1
assert n, "no boards emitted"
print("ok: %d board(s), every placed junction matched, frame intact" % n)
EOF
}

# Default --break_mode stuck is the greedy dive engine: every dive fills all 256
# cells, so this run must always produce a complete board.
step_backtracker_dives() {
    bin/E555_backtracker data/seed_Edge5.txt data/board_example_462.csv "$OUT/bt1.csv" \
        --holes data/holes_open_border_TRL.csv --breaks 60 \
        --restarts 500 --time_limit 15 --threads 4 > "$OUT/bt1.log"
    nf=$(awk -F, '!/^#/{print NF; exit}' "$OUT/bt1.csv")
    [ "$nf" = "514" ] || fail "expected 514 fields, got $nf"
    grep -q "\[dive\]" "$OUT/bt1.log" || fail "greedy engine did not run for --break_mode stuck"
    # Every dive completes, so the streamed best board must have no unplaced cell.
    unplaced=$(awk -F, '!/^#/{n=0; for(i=3;i<=258;i++) if ($i==999) n++; print n; exit}' "$OUT/bt1.csv")
    [ "$unplaced" = "0" ] || fail "greedy dive left $unplaced cells unplaced"
    bin/E555_backtracker data/seed_Edge5.txt "$OUT/bt1.csv" "$OUT/bt2.csv" \
        --holes data/holes_open_border_TRL.csv --breaks 60 \
        --restarts 500 --time_limit 5 --threads 4 > "$OUT/bt2.log"
    [ -s "$OUT/bt2.csv" ] || fail "round-trip run produced no output"
    echo "ok: greedy dives complete the board, canonical output, round-trip accepted"
}

# The exhaustive modes exist to produce trustworthy negative results, so the
# solution count must not depend on how many threads happen to be available.
# tests/fixtures/holes_top3.csv reopens the top three rows of the known
# solution: small enough to enumerate exhaustively in well under a second.
step_backtracker_exhaustive() {
    for t in 1 4; do
        bin/E555_backtracker data/synth_seed.txt data/synth_solution_480.csv "$OUT/ex$t.csv" \
            --holes tests/fixtures/holes_top3.csv --order mrv --breaks 0 \
            --max_emitted 0 --threads $t > "$OUT/ex$t.log"
    done
    s1=$(grep -oE 'full_solutions *= *[0-9]+' "$OUT/ex1.log" | grep -oE '[0-9]+$')
    s4=$(grep -oE 'full_solutions *= *[0-9]+' "$OUT/ex4.log" | grep -oE '[0-9]+$')
    [ -n "$s1" ] && [ "$s1" = "$s4" ] || fail "solution count differs by thread count: 1thr=$s1 4thr=$s4"
    echo "ok: $s1 solutions at both thread counts"
}

# --stop_row/--stop_column restrict the search to a band and emit every way to
# fill it, for the finalizer to resume from.  tests/fixtures/holes_row0_x4.csv
# reopens four cells of the bottom border row, which enumerates exhaustively in
# well under a second.  The three things that must hold are the three the
# finalizer depends on: exactly the band is placed, nothing outside it is, and
# the band is perfectly matched (score 15 = the 15 horizontal edges of one row).
step_backtracker_stop_band() {
    bin/E555_backtracker data/synth_seed.txt data/synth_solution_480.csv "$OUT/sb.csv" \
        --holes tests/fixtures/holes_row0_x4.csv --stop_row 0 --order rowmajor \
        --breaks 0 --max_emitted 0 --threads 4 > "$OUT/sb.log"
    band="$OUT/sb.csv.stop_row0.csv"
    [ -s "$band" ] || fail "no stop-band file at $band"
    n=$(grep -vc '^#' "$band")
    [ "$n" -gt 0 ] || fail "stop-band file has no data lines"
    bad=$(awk -F, '!/^#/{p=0; out=0; for(i=3;i<=258;i++) if ($i!=999) { p++; if ($i>15) out++ }
                          if (p!=16 || out!=0 || $2!=15) n++ } END{print n+0}' "$band")
    [ "$bad" = "0" ] || fail "$bad of $n bands are not an exact, row-0-only band"

    # Emission order is racy across threads, but the SET enumerated must not be.
    for t in 1 4; do
        bin/E555_backtracker data/synth_seed.txt data/synth_solution_480.csv "$OUT/sb$t.csv" \
            --holes tests/fixtures/holes_row0_x4.csv --stop_row 0 --order rowmajor \
            --breaks 0 --max_emitted 0 --threads $t > "$OUT/sb$t.log"
        grep -v '^#' "$OUT/sb$t.csv.stop_row0.csv" | cut -d, -f2- | sort > "$OUT/sbset$t.txt"
    done
    cmp -s "$OUT/sbset1.txt" "$OUT/sbset4.txt" \
        || fail "stop-band enumeration differs between 1 and 4 threads"

    # --reverse anchors the band at the far side: rows 15..15 for --stop_row 0.
    bin/E555_backtracker data/synth_seed.txt data/synth_solution_480.csv "$OUT/sbr.csv" \
        --stop_row 0 --reverse --breaks 0 --max_emitted 1 --threads 1 > "$OUT/sbr.log"
    [ -s "$OUT/sbr.csv.stop_row0_rev.csv" ] || fail "no reversed stop-band file"
    top=$(awk -F, '!/^#/{for(i=3;i<=258;i++) if ($i!=999 && $i<240) bad++} END{print bad+0}' \
          "$OUT/sbr.csv.stop_row0_rev.csv")
    [ "$top" = "0" ] || fail "--reverse band placed $top cells outside the top row"

    # A band must be refused where it could only mislead: a broken edge would be
    # rejected by the finalizer later, and --jump can never complete the band.
    bin/E555_backtracker data/synth_seed.txt data/synth_solution_480.csv "$OUT/sbx.csv" \
        --stop_row 3 --breaks 5 > "$OUT/sbx.log" 2>&1 && \
        fail "--stop_row accepted --breaks 5"
    grep -q "requires --breaks 0" "$OUT/sbx.log" || fail "wrong error for --breaks"

    echo "ok: $n exact row-0 bands, thread-independent, --reverse and guards correct"
}

# One whirlpool lap: turn the board, re-cut rows 0..5 exactly, re-grow to row 11.
# The assertions are the lap's geometry, which is what a rotation-sense error
# would silently break: a turned rows-0..10 board must have 11 complete COLUMNS
# and no complete row (that is why the band cut is needed at all), the band must
# be rows 0..5 at the theoretical 170, and the re-grow must reach rows 0..11 at
# the theoretical 356 = 15*12 + 16*11.
step_whirlpool_lap() {
    python3 - "$OUT/wp_row10.csv" <<'EOF'
import sys
line = [l for l in open("data/synth_solution_480.csv")
        if l.strip() and not l.startswith(("#", "%"))][0].rstrip("\n")
f = line.split(",")
meta, pos, rot = f[:-512], [p.strip() for p in f[-512:-256]], [r.strip() for r in f[-256:]]
pos = ["999" if p != "999" and int(p) // 16 > 10 else p for p in pos]
open(sys.argv[1], "w").write(",".join(meta + pos + rot) + "\n")
EOF
    n=$(awk -F, '!/^#/{p=0; for(i=3;i<=258;i++) if($i!=999)p++; print p}' "$OUT/wp_row10.csv")
    [ "$n" = 176 ] || fail "the rows-0..10 fixture has $n placed cells, expected 176"

    # A quarter-turn CW puts the filled region on columns 0..10 and leaves NO
    # complete row, so no finalizer could start from it.
    python3 tools/E555_rotate.py "$OUT/wp_row10.csv" 1 \
        --out "$OUT/wp_rot.csv" --seed_file data/synth_seed.txt > /dev/null
    read -r nc nr <<<"$(awk -F, '!/^#/{delete col; delete row;
        for(i=3;i<=258;i++) if($i!=999){col[$i%16]++; row[int($i/16)]++}
        nc=0; for(c=0;c<16;c++) if(col[c]==16)nc++
        nr=0; for(r=0;r<16;r++) if(row[r]==16)nr++
        print nc, nr}' "$OUT/wp_rot.csv")"
    [ "$nc" = 11 ] || fail "a turned rows-0..10 board has $nc complete columns, expected 11"
    [ "$nr" = 0 ]  || fail "a turned rows-0..10 board has $nr complete rows, expected 0"

    bin/E555_backtracker data/synth_seed.txt "$OUT/wp_rot.csv" "$OUT/wp_bt.csv" \
        --stop_row 5 --order rowmajor --break_mode any --breaks 0 \
        --max_emitted 2 --time_limit 60 --threads 4 > "$OUT/wp_bt.log"
    band="$OUT/wp_bt.csv.stop_row5.csv"
    [ -s "$band" ] || { tail -5 "$OUT/wp_bt.log"; fail "the band cut emitted nothing"; }
    bad=$(awk -F, '!/^#/{p=0; hi=0; for(i=3;i<=258;i++) if($i!=999){p++; if(int($i/16)>5)hi++}
                          if (p!=96 || hi!=0 || $2!=170) n++} END{print n+0}' "$band")
    [ "$bad" = 0 ] || fail "$bad emitted bands are not an exact rows-0..5 band at 170"

    bin/E555_finalizer data/synth_seed.txt "$band" \
        --out_dir "$OUT/wp_fin" --threads 4 \
        --finalize_from 5 --stop_row 11 --beam_width 20000 --frac_rand 0 \
        --num_rows "$(grep -vc '^#' "$band")" --top_columns 0 \
        --rng_seed 1 --max_emitted 8 --wall_time 300 > "$OUT/wp_fin.log"
    comp="$OUT/wp_fin/beam_completions_finalized_11.csv"
    [ -s "$comp" ] || { tail -5 "$OUT/wp_fin.log"; fail "no board re-grew from row 5 to row 11"; }
    # Stage B writes its solution INDEX in field 2, so the score has to be
    # recomputed from the seed before it can be asserted on.
    python3 tools/E555_rank.py "$comp" --seed_file data/synth_seed.txt \
        --out "$OUT/wp_final.csv" --rescore > /dev/null
    comp="$OUT/wp_final.csv"
    bad=$(awk -F, '!/^#/{p=0; hi=0; for(i=3;i<=258;i++) if($i!=999){p++; if(int($i/16)>11)hi++}
                          if (p!=192 || hi!=0 || $2!=356) n++} END{print n+0}' "$comp")
    [ "$bad" = 0 ] || fail "$bad re-grown boards are not an exact rows-0..11 board at 356"
    echo "ok: turn -> band(170) -> re-grow(356), $(grep -vc '^#' "$comp") boards"
}

# A plain --stop_row clears everything outside the band, the outer frame with it,
# so the band reaches the finalizer holding 26 of 60 border cells and fixed-sides
# mode is not available to it. --with_frame widens the band to take the frame in:
# border cells the turned board already holds are retained, the ones it does not
# are searched, and the band arrives complete. This check pins both halves of
# that -- the count, and the mode the finalizer actually chooses because of it.
step_band_with_frame() {
    python3 - "$OUT/wf_row10.csv" <<'EOF'
import sys
line = [l for l in open("data/synth_solution_480.csv")
        if l.strip() and not l.startswith(("#", "%"))][0].rstrip("\n")
f = line.split(",")
meta, pos, rot = f[:-512], [p.strip() for p in f[-512:-256]], [r.strip() for r in f[-256:]]
pos = ["999" if p != "999" and int(p) // 16 > 10 else p for p in pos]
open(sys.argv[1], "w").write(",".join(meta + pos + rot) + "\n")
EOF
    python3 tools/E555_rotate.py "$OUT/wf_row10.csv" 1 \
        --out "$OUT/wf_rot.csv" --seed_file data/synth_seed.txt > /dev/null

    # count_frame FILE -- placed cells and frame cells of the first board.
    count_frame='!/^#/{p=0; f=0
        for(i=3;i<=258;i++) if($i!=999){p++; v=$i+0
            if(int(v/16)==0||int(v/16)==15||v%16==0||v%16==15) f++}
        print p, f; exit}'

    bin/E555_backtracker data/synth_seed.txt "$OUT/wf_rot.csv" "$OUT/wf_plain.csv" \
        --stop_row 5 --order rowmajor --break_mode any --breaks 0 \
        --max_emitted 1 --time_limit 90 --threads 4 > "$OUT/wf_plain.log"
    plain="$OUT/wf_plain.csv.stop_row5.csv"
    [ -s "$plain" ] || { tail -5 "$OUT/wf_plain.log"; fail "the plain band cut emitted nothing"; }
    read -r p f <<<"$(awk -F, "$count_frame" "$plain")"
    [ "$p" = 96 ] || fail "a plain rows-0..5 band has $p placed cells, expected 96"
    [ "$f" = 26 ] || fail "a plain rows-0..5 band carries $f frame cells, expected 26"

    bin/E555_backtracker data/synth_seed.txt "$OUT/wf_rot.csv" "$OUT/wf_frame.csv" \
        --stop_row 5 --with_frame --order rowmajor --break_mode any --breaks 0 \
        --max_emitted 1 --time_limit 120 --threads 4 > "$OUT/wf_frame.log"
    band="$OUT/wf_frame.csv.stop_row5.csv"
    [ -s "$band" ] || { tail -5 "$OUT/wf_frame.log"; fail "--with_frame emitted no band"; }
    read -r p f <<<"$(awk -F, "$count_frame" "$band")"
    # 96 band cells, plus the 34 frame cells that sit outside rows 0..5:
    # columns 0 and 15 over rows 6..14, and the whole of row 15.
    [ "$p" = 130 ] || fail "a --with_frame band has $p placed cells, expected 130"
    [ "$f" = 60 ]  || fail "a --with_frame band carries $f frame cells, expected 60"

    # The point of the count: 60 is what fixed-sides mode requires. 26 is not.
    bin/E555_finalizer data/synth_seed.txt "$band" \
        --out_dir "$OUT/wf_fin" --threads 4 \
        --finalize_from 5 --stop_row 10 --beam_width 20000 --frac_rand 0 \
        --num_rows 1 --top_columns 0 \
        --rng_seed 1 --max_emitted 8 --wall_time 300 > "$OUT/wf_fin.log"
    grep -q "mode=fixed" "$OUT/wf_fin.log" \
        || { grep -m2 "mode=" "$OUT/wf_fin.log"; fail "a complete frame did not select fixed sides"; }
    comp="$OUT/wf_fin/beam_completions_finalized_10.csv"
    [ -s "$comp" ] || { tail -5 "$OUT/wf_fin.log"; fail "no board re-grew from the framed band"; }

    bin/E555_finalizer data/synth_seed.txt "$plain" \
        --out_dir "$OUT/wf_fin2" --threads 4 \
        --finalize_from 5 --stop_row 10 --beam_width 2000 --frac_rand 0 \
        --num_rows 1 --top_columns 1 \
        --rng_seed 1 --max_emitted 1 --wall_time 120 > "$OUT/wf_fin2.log"
    grep -q "mode=free" "$OUT/wf_fin2.log" \
        || { grep -m2 "mode=" "$OUT/wf_fin2.log"; fail "an incomplete frame did not fall back to free sides"; }

    # --with_frame widens a band; without one there is nothing to widen.
    if bin/E555_backtracker data/synth_seed.txt "$OUT/wf_rot.csv" "$OUT/wf_bad.csv" \
           --with_frame --breaks 0 --threads 1 > "$OUT/wf_bad.log" 2>&1; then
        fail "--with_frame without a stop band was accepted"
    fi

    echo "ok: band 96/26 frame cells plain, 130/60 with --with_frame -> mode=fixed"
}

step_clue_orient() {
    # The real seed, not the synthetic one: the clue table names real piece ids.
    # A band cut at row 5 carries no clue -- the centre sits on row 7 or 8 -- so
    # the finalizer has to CHOOSE an orientation rather than read one, which is
    # exactly the case that used to be refused outright.
    python3 - "$OUT/co_band.csv" <<'EOF'
import sys
line = [l for l in open("data/board_partial_row12.csv")
        if l.strip() and not l.startswith(("#", "%"))][0].rstrip("\n")
f = line.split(",")
meta, pos, rot = f[:-512], [p.strip() for p in f[-512:-256]], [r.strip() for r in f[-256:]]
pos = ["999" if p != "999" and int(p) // 16 > 5 else p for p in pos]
open(sys.argv[1], "w").write(",".join(meta + pos + rot) + "\n")
EOF
    n=$(python3 tools/E555_rank.py "$OUT/co_band.csv" --seed_file data/seed_Edge5.txt --field clues)
    [ "$n" = 0 ] || fail "the rows-0..5 band carries $n clue(s), expected none"

    # stop_row 10, never lower: every orientation's centre cell (row 7 or 8) is
    # inside the searched region well before that, and depth is what keeps the
    # output small -- measured here, row 10 writes 51 boards and 100 KB where
    # row 8 wrote 109 and 208 KB, in the same 10 s.
    bin/E555_finalizer data/seed_Edge5.txt "$OUT/co_band.csv" \
        --out_dir "$OUT/co_fin" --threads 4 --clue_center \
        --finalize_from 5 --stop_row 10 --beam_width 2000 --top_columns 1 \
        --rng_seed 3 --wall_time 300 > "$OUT/co_fin.log"
    comp="$OUT/co_fin/beam_completions_finalized_10.csv"
    [ -s "$comp" ] || { tail -5 "$OUT/co_fin.log"; fail "the unclued band emitted nothing"; }

    n=$(grep -cE "^\[sweep\] line 0: orientation [0-3] \(" "$OUT/co_fin.log")
    [ "$n" = 4 ] || fail "expected 4 orientation passes, saw $n"
    # One database for all four: the clue pieces are the same set in every
    # orientation, so only the pins move between passes.
    n=$(grep -c "DB inner stored" "$OUT/co_fin.log")
    [ "$n" = 1 ] || fail "expected 1 database build for the line, saw $n"

    # Every emitted board carries the centre clue, whichever orientation placed it.
    bad=$(python3 tools/E555_rank.py "$comp" --seed_file data/seed_Edge5.txt --csv |
          awk -F, 'NR>1 && $NF < 1 {n++} END{print n+0}')
    [ "$bad" = 0 ] || fail "$bad emitted boards lost the centre clue"

    # Pinning one orientation runs one pass and puts piece 138 on that cell only.
    # Orientation 1 because it is the one that survives to row 10 from this band
    # -- the other three go extinct, which is itself the point of searching all
    # four rather than picking one.
    bin/E555_finalizer data/seed_Edge5.txt "$OUT/co_band.csv" \
        --out_dir "$OUT/co_fin2" --threads 4 --clue_center --clue_orient 1 \
        --finalize_from 5 --stop_row 10 --beam_width 2000 --top_columns 1 \
        --rng_seed 3 --wall_time 300 > "$OUT/co_fin2.log"
    comp2="$OUT/co_fin2/beam_completions_finalized_10.csv"
    [ -s "$comp2" ] || { tail -5 "$OUT/co_fin2.log"; fail "--clue_orient 1 emitted nothing"; }
    # Piece 138 is field 3+138 of the pos block; orientation 1 puts it on cell 135.
    bad=$(awk -F, '!/^#/{ if ($(3+138)+0 != 135) n++ } END{print n+0}' "$comp2")
    [ "$bad" = 0 ] || fail "$bad boards from --clue_orient 1 do not hold the clue at cell 135"

    echo "ok: 4 orientations over 1 database, $(grep -vc '^#' "$comp") boards, all clued"
}

step_cpsat_chain() {
    if ! python3 -c "import ortools" 2>/dev/null; then
        echo "SKIPPED: OR-Tools not installed (pip install ortools)"
        return 0
    fi
    # topper: an L-shaped band plus a real 2-board beam (chain1 holds the beam)
    python3 src/C_tail/E555_topper.py data/seed_Edge5.txt \
        data/board_example_462.csv "$OUT/chain1.csv" \
        --side TR --band_depth 4 --top 2 --beam_diff 4 \
        --time_limit 20 --stall_time 8 --threads 4 > "$OUT/topper.log"
    # Two adaptive ender passes, each fed by the previous one. There is no mode
    # to choose any more: the ender picks its own focused and broad
    # neighbourhoods, so a pass is a profile plus a true per-board budget.
    # --board_time_limit is that budget -- every call the portfolio makes is
    # inside it -- and is not the same thing as the topper's --time_limit.
    python3 src/C_tail/E555_ender.py data/seed_Edge5.txt \
        "$OUT/chain1.csv" "$OUT/chain2.csv" \
        --profile overnight --search_mode improve \
        --board_time_limit 12 --threads 4 > "$OUT/ender_1.log"
    python3 src/C_tail/E555_ender.py data/seed_Edge5.txt \
        "$OUT/chain2.csv" "$OUT/chain3.csv" \
        --profile overnight --search_mode improve \
        --board_time_limit 12 --threads 4 > "$OUT/ender_2.log"
    for f in chain1 chain2 chain3; do
        nf=$(awk -F, '{print NF; exit}' "$OUT/$f.csv")
        [ "$nf" = "514" ] || fail "$f.csv has $nf fields (want 514)"
        python3 tools/E555_viewer.py "$OUT/$f.csv" --no_board --no_url
    done
    # the topper beam ranks must be genuinely different boards, not near-duplicates
    diffs=$(awk -F, 'NR<=2{for(i=3;i<=258;i++)a[NR,i]=$i}
                     END{n=0; for(i=3;i<=258;i++) if(a[1,i]!=a[2,i]) n++; print n}' "$OUT/chain1.csv")
    rows=$(wc -l < "$OUT/chain1.csv")
    if [ "$rows" -ge 2 ]; then
        [ "${diffs:-0}" -ge 4 ] || fail "beam ranks differ in only $diffs cells (want >= 4)"
        echo "ok: beam ranks differ in $diffs cells"
    else
        echo "note: only $rows rank emitted (no distinct board within slack)"
    fi
    echo "ok: three stages chained, all outputs canonical"
}

step_beamer_micro() {
    if [ "${SKIP_BEAMER:-0}" = "1" ]; then echo "SKIPPED (SKIP_BEAMER=1)"; return 0; fi
    # No --lambda_Mahalanobis: it used to pass 8, a value in the raw-d2n units
    # the term had before it was normalised by the live per-row spread. In
    # score-SD, 8 is enormous and drowns the colour objective. The same stale
    # value came out of the pipelines; leaving the flag off exercises the
    # shipped default, which is what a smoke test should be testing.
    CMD=(bin/E555_beamer data/seed_Edge5.txt --random_edges
         --samples 1 --top_columns 1 --beam_width 20000 --stop_row 10
         --rng_seed 1 --out_dir "$OUT/beam")
    if [ -n "$GATE_DB" ]; then CMD+=(--db_file "$GATE_DB"); fi
    # E555_COL_VERIFY needs a real database to build strips out of, and this is
    # the only check that has one. It asserts the left-column window score reads
    # the board the right way up -- a direction that fails silently, since a
    # reversed read still returns a large plausible number for every column.
    E555_COL_VERIFY=1 "${CMD[@]}" > "$OUT/beamer.log"
    grep -q "run summary" "$OUT/beamer.log" || fail "no run summary in beamer log"
    grep -qE "^\[colv\] [0-9]+ strips: downward ([0-9]+)/\1 exact,.* 0 error" "$OUT/beamer.log" \
        || { grep "^\[colv\]" "$OUT/beamer.log"; fail "column rotation self-check did not pass"; }
    comp="$OUT/beam/beam_completions_random_10.csv"
    if [ -s "$comp" ]; then
        python3 tools/E555_viewer.py "$comp" --no_board --no_url
        echo "ok: run complete, emissions parse"
    else
        echo "ok: run complete (this config went extinct -- normal for a micro-run)"
    fi
}

# =============================================================================
# The shipped scripts. Everything above calls bin/* and the Python tools
# directly, which is how two scripts that could not complete a run at all came
# to ship. Every check below runs a real example or pipeline script at its
# smallest useful settings, with every output path redirected into tests/out.
# No script here caches the chain database to disk: the examples have no such
# option, and the pipeline runner is handed an empty DB_FILE.
# =============================================================================

# WHIRL_ROWS with a single entry is one lap, which is the whole reason lap() no
# longer talks to its caller through globals. It needs a board to whirl, so it
# is handed the rows-0..10 fixture the roundhouse checks build, and stage 0 is
# skipped -- the 6.4 GB beam is already covered by beamer_micro.
step_pipeline_whirl_lap() {
    if ! python3 -c "import ortools" 2>/dev/null; then
        echo "SKIPPED: OR-Tools not installed (the closing stages would be skipped)"
        return 0
    fi
    rh_fixtures
    bash pipeline/run_pipeline_whirlpool.sh SEED=data/synth_seed.txt \
        INPUT="$OUT/rh_rows12.csv" RUN_DIR="$PWD/$OUT/whirl" THREADS=4 \
        WHIRL_ROWS=10 BAND_ROW=5 BT_LIMIT=20 BT_PICK=2 BT_TIME=15 POP=4 \
        FIN_WIDTH=2000 FIN_BOARDS=4 FIN_WALL=120 FIN_COLUMNS=2 \
        RH_WIDTH=3 RH_ROUNDS=1 RH_LINES=2 RH_WALL=60 \
        BT_RESTARTS=2000 BT_FINAL_TIME=15 TOP_N=4 > "$OUT/whirl.log" \
        || { tail -20 "$OUT/whirl.log"; fail "the whirlpool exited non-zero"; }
    grep -q "LAP 1/1" "$OUT/whirl.log" || fail "the run never reached lap 1"
    grep -q "WHIRLPOOL DONE" "$OUT/whirl.log" || fail "the lap never finished"
    echo "ok: one whirlpool lap ran end to end"
}

step_scripts_parse() {
    n=0
    for s in examples/*.sh pipeline/*.sh tests/*.sh; do
        bash -n "$s" || fail "$s does not parse"
        n=$((n + 1))
    done
    for s in pipeline/*.py; do
        python3 -m py_compile "$s" || fail "$s does not compile"
        n=$((n + 1))
    done
    echo "ok: $n scripts parse"
    # Parsing is not running. When --gumbel_tau0/--gumbel_tau1 were removed
    # from the beamer, two pipelines went on passing them and went on parsing
    # perfectly; the binary rejects an unknown flag at startup, so the failure
    # waited for whoever ran the pipeline next. This reads the accepted flags
    # out of the parsers themselves, so it cannot drift from the code.
    python3 tests/check_script_flags.py || fail "a script passes a flag its binary rejects"
}

# On the synthetic fixture, where the finalizer's chain database is 0.03 GB and
# builds in half a second -- the real seed would want ~6 GB here.
step_example_finalizer() {
    bash examples/02_finalizer_regrow.sh SEED=data/synth_seed.txt \
        BOARDS=data/synth_solution_480.csv OUT_DIR="$OUT/ex03" \
        FROM=10 STOP_ROW=12 REPEATS=1 BEAM_WIDTH=20000 N_LINES=1 \
        MAX_WALL=120 THREADS=4 > "$OUT/ex03.log" \
        || { tail -5 "$OUT/ex03.log"; fail "examples/02 exited non-zero"; }
    comp="$OUT/ex03/beam_completions_finalized_12.csv"
    [ -s "$comp" ] || { tail -5 "$OUT/ex03.log"; fail "examples/02 emitted nothing"; }
    nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$comp")
    [ "$nf" = "514" ] || fail "examples/02 wrote $nf fields, want 514"
    echo "ok: examples/02 re-grew rows 11..12, canonical output"
}

step_example_roundhouse() {
    rh_fixtures
    bash examples/03_roundhouse_strip.sh SEED=data/synth_seed.txt \
        BOARDS="$OUT/rh_rows12.csv" OUT_DIR="$OUT/ex04" \
        ROUNDS=1 WIDTH=3 ROTATE=1 TIES=1 BREAKS=0 N_LINES=1 \
        MAX_WALL=120 THREADS=4 > "$OUT/ex04.log" \
        || { tail -5 "$OUT/ex04.log"; fail "examples/03 exited non-zero"; }
    comp="$OUT/ex04/strip.csv"
    [ -s "$comp" ] || { tail -5 "$OUT/ex04.log"; fail "examples/03 emitted nothing"; }
    nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$comp")
    [ "$nf" = "514" ] || fail "examples/03 wrote $nf fields, want 514"
    echo "ok: examples/03 refilled one strip, canonical output"
}

# examples/06 chains two roundhouse passes per board, once each way round, and
# --hold_band lets the second keep the far half of what the first left. On the
# synthetic board the first pass of each chain already closes the puzzle, so both
# chains report 480 and the ids carry every stage: the input's own, the tool's
# suffix, then the pass that wrote it.
#
# Each pass names its own output, so the file is cfg<N>/<pass>/boards.csv and
# there is nothing to glob for.
step_example_bothways() {
    rh_fixtures
    bash examples/06_roundhouse_both_ways.sh SEED=data/synth_seed.txt \
        BOARDS="$OUT/rh_rows12.csv" OUT_DIR="$OUT/ex06" \
        ROUNDS=1 WIDTH=3 ROTATE=1 N_LINES=1 HOLD=1 MAX_WALL=120 \
        THREADS=4 > "$OUT/ex06.log" \
        || { tail -5 "$OUT/ex06.log"; fail "examples/06 exited non-zero"; }
    grep -q "a=480 *b=480" "$OUT/ex06.log" || \
        { tail -8 "$OUT/ex06.log"; fail "both chains should reach the known solution"; }
    comp="$OUT/ex06/cfg0/a1/boards.csv"
    [ -s "$comp" ] || { tail -5 "$OUT/ex06.log"; fail "examples/06 emitted nothing"; }
    nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$comp")
    [ "$nf" = "514" ] || fail "examples/06 wrote $nf fields, want 514"
    id=$(awk -F, '!/^ *[#%]/{print $1; exit}' "$comp")
    case "$id" in board_*_a1) ;; *) fail "id lost its provenance: $id" ;; esac
    # The second pass holds only the far half of its final side, so it always has
    # a near half to search even when the first pass closed the board. It must
    # report a board, and the held pieces must survive -- verify_hold_snapshot()
    # aborts the run if one moves, so reaching a [board] line at all proves it.
    grep -q "\[board\]" "$OUT/ex06/cfg0/a2/log" || \
        { tail -5 "$OUT/ex06/cfg0/a2/log"; fail "the held second pass reported nothing"; }
    echo "ok: examples/06 ran both chains to 480, id $id"
}

step_example_cpsat() {
    if ! python3 -c "import ortools" 2>/dev/null; then
        echo "SKIPPED: OR-Tools not installed (pip install ortools)"
        return 0
    fi
    # examples/04a is the funnel: topper scout -> diverse promotion -> topper
    # polish -> one adaptive ender close, each pass fed by the previous one.
    # There is no ring stage any more; the ender picks its own neighbourhoods.
    bash examples/04a_CP-SAT_top_and_end.sh OUT_DIR="$OUT/ex04cp" \
        SIDE=T WORK_ROWS=3 N_LINES=1 THREADS=4 \
        SCOUT_TIME=10 SCOUT_STALL=5 POLISH_TIME=10 POLISH_STALL=5 \
        ENDER_PROFILE=overnight ENDER_BOARD_TIME=20 > "$OUT/ex04c.log" \
        || { tail -5 "$OUT/ex04c.log"; fail "examples/04a exited non-zero"; }
    for f in "$OUT/ex04cp/1_scout.csv" "$OUT/ex04cp/2_promoted.csv" \
             "$OUT/ex04cp/3_polished.csv" "$OUT/ex04cp/4_closed.csv"; do
        [ -s "$f" ] || fail "$(basename "$f") was not written"
        nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$f")
        [ "$nf" = "514" ] || fail "$(basename "$f") has $nf fields, want 514"
    done
    echo "ok: examples/04a chained scout, promote, polish and close"
}

step_example_backtracker() {
    bash examples/05_backtracker_dives.sh OUT="$OUT/ex07.csv" \
        MODE=stuck MAX_MISMATCH=60 RESTARTS=2000 N_LINES=1 THREADS=4 \
        TIME_LIMIT=15 > "$OUT/ex07.log" \
        || { tail -5 "$OUT/ex07.log"; fail "examples/05 exited non-zero"; }
    [ -s "$OUT/ex07.csv" ] || fail "examples/05 emitted nothing"
    nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$OUT/ex07.csv")
    [ "$nf" = "514" ] || fail "examples/05 wrote $nf fields, want 514"
    echo "ok: examples/05 dived and wrote a canonical board"
}

# Two passes: one that unsets the outer rows, one that fills them back in. That
# is one complete GROUP, so the group prune at the end of the plan runs -- the
# part of this script that decides which board survives.
step_pipeline_topper_sweep() {
    if ! python3 -c "import ortools" 2>/dev/null; then
        echo "SKIPPED: OR-Tools not installed (pip install ortools)"
        return 0
    fi
    # PRESET=closeT is the two-pass plan: a deep pass that may spend breaks,
    # then a LOCKED=0 pass that closes the group and triggers the prune.
    bash pipeline/topper_sweep.sh INPUT=data/board_example_462.csv \
        PRESET=closeT OUT="$PWD/$OUT/sweep/topped.csv" OUT_DIR="$OUT/sweep" \
        WORKERS=4 MAX_TIME=10 STALL_TIME=5 NUM_ROWS=1 BEAM=1 > "$OUT/sweep.log" \
        || { tail -5 "$OUT/sweep.log"; fail "topper_sweep.sh exited non-zero"; }
    final="$OUT/sweep/topped.csv"
    [ -s "$final" ] || { tail -5 "$OUT/sweep.log"; fail "topper_sweep.sh produced no final board"; }
    nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$final")
    [ "$nf" = "514" ] || fail "topper_sweep.sh wrote $nf fields, want 514"
    grep -q "\[prune\]" "$OUT/sweep.log" || fail "the group prune never ran"
    echo "ok: two passes, one group prune, canonical board out"
}

# Builds the real chain database twice -- once per invocation. ANNEAL=1 is the
# only check anywhere that drives the beamer from a Stage A rotations file
# rather than --random_edges.
# STOP_ROW 10, which is the example's own default -- the check used to override
# it down to 4 and got a 29 MB CSV on the real seed for it. Nothing has gone
# extinct that shallow, so the stop-row beam is emitted in full; by row 10 it has
# thinned. Neither assertion below needs a surviving board (the example reports
# "no board survived" and exits 0), so depth costs the check nothing.
step_example_beamer() {
    if [ "${SKIP_BEAMER:-0}" = "1" ]; then echo "SKIPPED (SKIP_BEAMER=1)"; return 0; fi
    bash examples/01_beamer_quickstart.sh OUT_DIR="$OUT/ex01" BEAM_WIDTH=2000 \
        STOP_ROW=10 N_BOTTOMS=1 N_COLUMNS=1 THREADS=4 DB_FILE="$GATE_DB" \
        > "$OUT/ex01.log" \
        || { tail -5 "$OUT/ex01.log"; fail "examples/01 exited non-zero"; }
    grep -q "run summary" "$OUT/ex01.log" || fail "examples/01 printed no run summary"
    bash examples/01_beamer_quickstart.sh ANNEAL=1 \
        ROTATIONS="$OUT/ex01_rotations.csv" BORDERS=2 THREADS=4 \
        OUT_DIR="$OUT/ex01a" BEAM_WIDTH=2000 STOP_ROW=10 N_BOTTOMS=2 \
        N_COLUMNS=1 DB_FILE="$GATE_DB" > "$OUT/ex01a.log" \
        || { tail -5 "$OUT/ex01a.log"; fail "examples/01 ANNEAL=1 exited non-zero"; }
    [ -s "$OUT/ex01_rotations.csv" ] || fail "examples/01 ANNEAL=1 wrote no rotations file"
    grep -q "run summary" "$OUT/ex01a.log" || fail "examples/01 ANNEAL=1 printed no run summary"
    echo "ok: examples/01 completed a run both ways"
}

# The only check that runs a whole pipeline. It matters because stages 5..7 are
# reachable no other way: the runner has no stage selector, so the CP-SAT and
# backtracker stages sit behind two chain-database builds. Until that selector
# exists, SKIP_BEAMER=1 leaves the second half of this script unexercised.
# Stop row 10, not 6. --incomplete_top emits a sibling partial for every pair of
# the three segments, and at a shallow row nothing has gone extinct yet, so the
# stop-row beam is reported in full and the siblings multiply it: measured on
# this fixture, row 6 wrote 807 042 boards and 1.5 GB, enough to push
# E555_rank.py to 13.9 GB RSS and get it OOM-killed mid-gate. The same run at
# row 10 writes 1 136 boards and 2.2 MB in the same 5 s, because by then the
# beam has thinned. Depth is what bounds this output, not width or --top_bottoms
# (capping that to 2 changed nothing), and row 10 is also where the pipeline
# really operates.
step_pipeline_full() {
    if [ "${SKIP_BEAMER:-0}" = "1" ]; then echo "SKIPPED (SKIP_BEAMER=1)"; return 0; fi
    if ! python3 -c "import ortools" 2>/dev/null; then
        echo "SKIPPED: OR-Tools not installed (stages 5 and 6 would be skipped)"
        return 0
    fi
    bash pipeline/run_pipeline.sh RUN_DIR="$PWD/$OUT/pipeline" THREADS=4 \
        BORDERS=annealed ROUNDS=2 STEPS=250000 \
        `# 250000 is the annealer's floor, and it is where feasibility stops`\
        `# being a coin flip: 2x2000 found a border on one run and none on`\
        `# the next, while 250000 succeeded on every restart. 2 of them, 15 s` \
        BEAM_WIDTH=2000 BEAM_STOP_ROW=10 BEAM_MAX_PARTIALS=2 \
        BEAM_MAX_WALL=300 BEAM_COLUMNS=2 FIN_WIDTH=2000 FIN_STOP_ROW=11 \
        FIN_FROM=5 FIN_MAX_PARTIALS=2 FIN_MAX_WALL=300 \
        RH_WIDTH=5 RH_LINES=2 RH_WALL=60 \
        DB_FILE="$GATE_DB" \
        `# stage 6 was 474 s of a 620 s run: the ender solves once per break,`\
        `# so --time_limit is per solve and not per board. A small model and a`\
        `# short budget still exercise both of its modes.` \
        TOP_N=2 CPSAT_TIME=3 CPSAT_STALL=2 ENDER_REACH=1 ENDER_CHANGES=4 \
        BT_MISMATCH=60 BT_RESTARTS=2000 BT_TIME=15 > "$OUT/pipeline.log" \
        || { tail -20 "$OUT/pipeline.log"; fail "run_pipeline.sh exited non-zero"; }
    for stage in "STAGE 1/7" "STAGE 2/7" "STAGE 3/7" "STAGE 4/7" \
                 "STAGE 5/7" "STAGE 6/7" "STAGE 7/7"; do
        grep -q "$stage" "$OUT/pipeline.log" || fail "the run never reached $stage"
    done
    echo "ok: all seven stages ran to completion"
}

# Every example defaults its output into the current directory, so one missing
# environment variable above litters the working tree on an otherwise green run.
# bin/ and logs/ are exempt: check 1 rebuilds bin/ from scratch, and both
# topper_sweep.sh and the pipeline runners mkdir logs/ for their Slurm headers,
# so on a fresh clone those two appear legitimately.
# The farm's own bookkeeping. Every tool it drives is covered elsewhere; what is
# only here is how boards move between the pools -- and a slip there is silent,
# because a farm that loses or re-searches boards still looks like it is working.
step_farm_pools() {
    python3 - <<'EOF' || fail "run_farm.py board handling is wrong"
import importlib.util, os, subprocess, sys, tempfile
spec = importlib.util.spec_from_file_location("rf", "pipeline/run_farm.py")
rf = importlib.util.module_from_spec(spec); spec.loader.exec_module(rf)

d = tempfile.mkdtemp()
pool = os.path.join(d, "pool.csv")

# Comments are not boards, and a round trip must not invent or drop rows.
open(pool, "w").write("# a comment\n\nb1,1\nb2,2\nb3,3\n% another\n")
assert rf.read(pool) == ["b1,1", "b2,2", "b3,3"], rf.read(pool)

# take() is the whole graduation mechanism: what it takes must leave.
assert rf.take(pool, 2) == ["b1,1", "b2,2"]
assert rf.read(pool) == ["b3,3"], rf.read(pool)
assert rf.take(pool, 9) == ["b3,3"]              # asking for more than exists
assert rf.read(pool) == []
assert rf.take(os.path.join(d, "nope.csv"), 3) == []      # missing file

# Writes are atomic: no .tmp may survive a completed write.
rf.write(pool, ["x,1"])
assert not os.path.exists(pool + ".tmp")

# A canonical row: id, score, pos[256], rot[256], 999 for unplaced.
def board(ident, cells, spin=None):
    pos = ["999"] * 256
    for piece, cell in enumerate(cells):
        pos[piece] = str(cell)
    rot = ["0"] * 256
    if spin is not None:
        rot[spin] = "1"
    return ",".join([ident, "0"] + pos + rot)

full12 = board("rndb0l0_1", range(208))          # rows 0..12 complete
row11 = board("rndb0l0_2", range(192))           # the beam's usual shape
assert rf.closed_to(full12, 12) and rf.closed_to(full12, 11)
assert not rf.closed_to(row11, 12) and rf.closed_to(row11, 11)
assert rf.placed(full12) == 208 and rf.placed(row11) == 192

# spread() picks for variety, because every row-11 board scores the same and
# ranking cannot choose between them. Round-robin across configs, one board
# per distinct rows 0..7 floor -- the whole of the first topper's problem.
same_floor = board("rndb0l0_3", range(192), spin=200)   # differs above row 7
boards = [row11, same_floor, board("rndb0l1_1", range(1, 193)),
          board("rndb0l2_1", range(2, 194)), board("rndb0l3_1", range(3, 195))]
picked = rf.spread(boards, 4)
assert len(picked) == 4, picked                  # the twin floor is skipped
assert len({rf.board_id(b).rsplit("_", 1)[0] for b in picked}) == 4
assert len({rf.floor(b) for b in picked}) == 4

# The roundhouse appends _<line><tag><n>, so a spiral's parent is the board
# whose id is the longest prefix of its own. One that came back with fewer
# pieces than its parent must never displace it: measured, parents at 208
# came back at 169 and took four of six slots in the next stage.
spirals = [board("rndb0l0_1_0d0", range(208), spin=3),   # held its ground
           board("rndb0l0_1_0d1", range(169)),           # 39 pieces short
           board("rndb0l0_9_0d0", range(208))]           # not ours: no parent
kept = [rf.board_id(x) for x in rf.kept_spirals(spirals, [full12, row11])]
assert kept == ["rndb0l0_1_0d0", "rndb0l0_9_0d0"], kept

# The ender pass has to write to the file the caller named. It did not once:
# the result went to a scratch file, "1 endered" was logged, and good.csv stayed
# empty -- the boards were searched and then dropped. The output path is the
# positional argument just before the first flag, so the stub reads it there and
# records which profile the pass asked for.
calls = []
rf.sh = lambda *cmd: (calls.append(cmd[cmd.index("--profile") + 1]),
                      rf.write(cmd[cmd.index("--profile") - 1], ["z,1"]))[0] is None or 0
for deep, want in ((False, [rf.ENDER_PROFILE]), (True, [rf.ELITE_PROFILE])):
    del calls[:]
    out = os.path.join(d, "ender_out_%s.csv" % deep)
    got = rf.ender(d, ["a,1"], out, deep)
    assert calls == want, (deep, calls)
    assert os.path.exists(out) and got, (deep, out)

# An unknown setting must stop the run, not be ignored for two days.
r = subprocess.run([sys.executable, "pipeline/run_farm.py", "NO_SUCH=1"],
                   capture_output=True, text=True)
assert r.returncode != 0 and "unknown setting" in r.stderr, r
print("ok: files round-trip, take() slices, spread() varies, spirals guarded")
EOF
}

step_no_stray_output() {
    ls -A | grep -vxE 'bin|logs' > "$OUT/root_after.txt"
    stray=$(comm -13 "$OUT/root_before.txt" "$OUT/root_after.txt" | tr '\n' ' ')
    [ -z "${stray// /}" ] || fail "new entries in the repository root: $stray"
    echo "ok: the repository root is unchanged"
}

# =============================================================================
# Driver
# =============================================================================
SEL=()
case "${1:---all}" in
    -h|--help) usage; exit 0 ;;
    --list)    list_steps; exit 0 ;;
esac
if [ "$#" -eq 0 ]; then
    for ((i = 1; i <= TOTAL; i++)); do SEL+=("$i"); done
else
    for tok in "$@"; do
        case "$tok" in
            [0-9]*-[0-9]*)
                lo="${tok%%-*}"; hi="${tok##*-}"
                [ "$lo" -ge 1 ] && [ "$hi" -le "$TOTAL" ] && [ "$lo" -le "$hi" ] \
                    || { echo "!!! range out of 1..$TOTAL: $tok"; exit 2; }
                for ((i = lo; i <= hi; i++)); do SEL+=("$i"); done ;;
            [0-9]*)
                [ "$tok" -ge 1 ] && [ "$tok" -le "$TOTAL" ] \
                    || { echo "!!! no check $tok (1..$TOTAL)"; exit 2; }
                SEL+=("$tok") ;;
            *)
                idx=$(index_of "$tok")
                [ -n "$idx" ] || { echo "!!! no check named '$tok'"; usage; exit 2; }
                SEL+=("$idx") ;;
        esac
    done
fi

# Canonical order, no repeats, whatever order the arguments came in: several
# checks only make sense in sequence -- no_stray_output has to be last of all,
# and compile first.
mapfile -t SEL < <(printf '%s\n' "${SEL[@]}" | sort -n -u)

rm -rf "$OUT" && mkdir -p "$OUT"
ls -A | grep -vxE 'bin|logs' > "$OUT/root_before.txt"   # for no_stray_output

echo "=== E555 release gate ==="
if [ "${#SEL[@]}" -eq "$TOTAL" ]; then
    echo "[cfg] all $TOTAL checks; about 4 min, plus ~20 min for the three"\
         "database checks unless SKIP_BEAMER=1"
else
    echo "[cfg] ${#SEL[@]} of $TOTAL checks selected: ${SEL[*]}"
fi
if ! has_step 1; then
    for b in beamer finalizer roundhouse backtracker; do
        [ -x "bin/E555_$b" ] || { echo "!!! bin/E555_$b is missing: run make, or include check 1"; exit 1; }
    done
    echo "[cfg] check 1 not selected, using the binaries already in bin/"
fi

PASS=0
TIMES=()
for idx in "${SEL[@]}"; do
    entry="${ALL_STEPS[idx - 1]}"
    name="${entry%%|*}"
    STEP="${entry#*|}"
    echo ""
    echo "=== [test $idx/$TOTAL] $name -- $STEP ==="
    t0=$SECONDS
    # Called bare, on purpose. Putting this in an `if`, a `&&` or a `|| fail`
    # would disable `set -e` for everything inside the function body, so a
    # failing tool would stop aborting and only the body's last command would
    # decide the result -- a gate that is green because it stopped checking.
    # Every check signals failure with an explicit `fail`, or by dying under -e.
    "step_$name"
    TIMES+=("$(printf '%3d  %-22s %4ds' "$idx" "$name" $((SECONDS - t0)))")
    PASS=$((PASS + 1))
done

echo ""
echo "=== run summary ==="
if [ "${#TIMES[@]}" -gt 0 ]; then
    for t in "${TIMES[@]}"; do echo "[sum] $t"; done
fi
if [ "$PASS" -eq "$TOTAL" ]; then
    echo "[sum] all $TOTAL checks passed in ${SECONDS}s"
else
    echo "[sum] $PASS of $TOTAL checks passed in ${SECONDS}s -- PARTIAL RUN, not a full gate"
fi
