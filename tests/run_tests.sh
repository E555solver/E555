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
#   bash tests/run_tests.sh roundhouse_selfcheck  by name
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
#   SKIP_BEAMER=1   skip the three database checks (low-RAM machines)
#   DB_FILE=path    keep the 6.4 GB chain database here (~6.5 GB on disk)
#                   instead of under tests/out, so it survives the wipe and
#                   every later run loads it rather than building it. The
#                   three real-seed checks share one cache either way.
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
# skips all three, so nothing is built at all.
GATE_DB="${DB_FILE:-$OUT/chain.db}"

# -----------------------------------------------------------------------------
# The checks, in order. "name|one-line label"; the label is what gets printed.
# -----------------------------------------------------------------------------
ALL_STEPS=(
    "compile|make all, zero compiler warnings tolerated"
    "viewer|the known synthetic solution scores 480/480"
    "rank|measures agree with the viewer, --out verbatim, --rescore canonical"
    "rotate|a quarter-turn preserves every measure, four turns are the identity"
    "annealer|Stage A short run: BEST lines and a beamer-format --out CSV"
    "finalizer_synth|REGRESSION: rediscovers the synthetic solution from row 10"
    "finalizer_rotations|re-imposes a matching rotations row's side assignment"
    "finalizer_determinism|one seed re-run reproduces the search exactly"
    "roundhouse_synth|REGRESSION: rebuilds the solution at strip widths 3 and 5"
    "roundhouse_two_rounds|closes the board in two rounds, rotating between them"
    "roundhouse_selfcheck|the relaxed oracle against brute-force enumeration"
    "roundhouse_legal|every emitted board is break-free and frame-legal"
    "roundhouse_reverse|--reverse mirrors the seed, so the spiral runs the other way round"
    "roundhouse_hold_band|--hold_band keeps the last band's standing levels and fills up to meet them"
    "backtracker_dives|greedy dives on the example board, plus an own-output round-trip"
    "backtracker_exhaustive|exhaustive enumeration identical at 1 and 4 threads"
    "backtracker_stop_band|--stop_row/--stop_column emit exact, finalizer-shaped bands"
    "whirlpool_lap|one whirlpool lap: turn, re-cut rows 0..5, re-grow to row 11"
    "clue_orient|a band carrying no clue is searched at all four orientations"
    "band_with_frame|--with_frame carries all 60 frame cells, so the finalizer fixes the sides"
    "cpsat_chain|topper -> ender(ring) -> ender(patch), each fed by the last"
    "beamer_micro|random_edges micro-run: builds the real 6.4 GB database"
    "scripts_parse|every shipped script parses, and passes only flags that exist"
    "example_finalizer|examples/02 re-grows the synthetic board"
    "example_roundhouse|examples/03 refills one strip"
    "example_bothways|examples/06 runs both chains over one board, ids intact"
    "example_cpsat|examples/04, the whole Stage C chain"
    "example_backtracker|examples/05 dives on the example board"
    "pipeline_topper_sweep|pipeline/topper_sweep.sh through a two-pass plan"
    "example_beamer|examples/01 both ways, random and annealed borders"
    "pipeline_full|pipeline/run_pipeline.sh, all seven stages"
    "pipeline_whirl_lap|pipeline/run_pipeline_whirlpool.sh, one lap end to end"
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

# --verbose: the BEST lines grepped below are verbose-only, the default being
# one summary line per restart.
step_annealer() {
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
        bin/E555_roundhouse data/synth_seed.txt "$OUT/$src" --rounds 1 --strip_width "$w" \
            --ties 50 --out_dir "$OUT/rh_$w" > "$OUT/roundhouse_$w.log"
        comp=$(first_match "$OUT/rh_$w"/roundhouse_*_miss0.csv)
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
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_corebreak.csv" --rounds 1 --strip_width 0 \
        --out_dir "$OUT/rh_cb" > "$OUT/roundhouse_corebreak.log" 2>&1 || true
    grep -q "break inside the kept region" "$OUT/roundhouse_corebreak.log" || \
        { cat "$OUT/roundhouse_corebreak.log"; fail "a break inside the core was not refused"; }
    cb=$(first_match "$OUT"/rh_cb/roundhouse_*.csv)      # any file at all
    [ -n "$cb" ] && [ -s "$cb" ] && fail "a core-break board must emit nothing"
    echo "ok: a break inside the core is refused, one in the freed band is ignored"
}

# --rounds 2 frees the right and top bands and refills both, so like --rounds 1
# it ends on a COMPLETE board -- and unlike --rounds 1 it exercises the rotation
# between rounds, an open-topped strip followed by a closed one. If a board can
# be finished in two rounds this is what has to see it.
step_roundhouse_two_rounds() {
    rh_fixtures
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows10.csv" --rounds 2 --strip_width 5 \
        --ties 50 --wall_time 300 --out_dir "$OUT/rh_r2" > "$OUT/roundhouse_r2.log"
    comp=$(first_match "$OUT/rh_r2"/roundhouse_*_miss0.csv)
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

# The relaxed dynamic program is what every prune in the strip search rests on.
# --selfcheck re-counts the same relaxation by enumeration, signature by
# signature; a disagreement means the oracle is pruning live branches.
step_roundhouse_selfcheck() {
    rh_fixtures
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows12.csv" --rounds 3 --strip_width 3 \
        --selfcheck --out_dir "$OUT/rh_sc" > "$OUT/roundhouse_selfcheck.log"
    grep -q "PASS" "$OUT/roundhouse_selfcheck.log" || \
        { cat "$OUT/roundhouse_selfcheck.log"; fail "oracle disagrees with brute force"; }
    grep -E "selfcheck" "$OUT/roundhouse_selfcheck.log"
}

# --reverse mirrors the seed left-right, runs the unchanged right/top/left spiral
# in that mirror, and mirrors every emitted board back. Four maps have to compose
# to the identity for that to work -- the seed, the clue table, the board in and
# the board out -- and rebuilding the KNOWN SOLUTION is what proves they do.
# Break-freeness alone could not: a mirror preserves every match, so a board
# emitted still mirrored would score a clean 480 and look perfectly fine. Only
# equality with data/synth_solution_480.csv catches a missing un-mirror.
step_roundhouse_reverse() {
    rh_fixtures
    for spec in "rh_rows12.csv:0" "rh_rows10.csv:5"; do
        src="${spec%%:*}"; w="${spec##*:}"
        bin/E555_roundhouse data/synth_seed.txt "$OUT/$src" --reverse --rounds 1 \
            --strip_width "$w" --ties 50 --out_dir "$OUT/rh_rev_$w" > "$OUT/roundhouse_rev_$w.log"
        comp=$(first_match "$OUT/rh_rev_$w"/roundhouse_*rev_*_miss0.csv)
        [ -s "$comp" ] || { tail -5 "$OUT/roundhouse_rev_$w.log"; fail "reverse ($src) emitted nothing"; }
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
    # searched here: --start_row is past the end of the one-line CSV.
    plan() {
        bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows12.csv" --rounds 3 \
            --strip_width 3 --rotate 1 --start_row 9 --out_dir "$OUT/rh_rev_plan" "$@" 2>&1 |
            sed -n 's/^\[plan\] rounds refill the input.s //p' | sed 's/ band;.*//'
    }
    [ "$(plan)" = "TOP -> LEFT -> BOTTOM" ] || fail "the forward spiral changed: $(plan)"
    [ "$(plan --reverse)" = "TOP -> RIGHT -> BOTTOM" ] || \
        fail "--reverse did not turn the spiral round: $(plan --reverse)"
    echo "ok: --rotate 1 refills TOP -> LEFT -> BOTTOM, and TOP -> RIGHT -> BOTTOM reversed"

    # Corner roles are read in the INPUT's coordinates, so --BL still means the
    # board the user handed in. --rotate 3 frees the bottom band either way, and
    # the synthetic solution has piece 95 at (0,0) and piece 31 at (0,15): pinning
    # the right one must find the solution, pinning the other must refute.
    for spec in "95:1" "31:0"; do
        pid="${spec%%:*}"; want="${spec##*:}"
        rm -rf "$OUT/rh_rev_pin"
        bin/E555_roundhouse data/synth_seed.txt data/synth_solution_480.csv --reverse \
            --rounds 1 --strip_width 3 --rotate 3 --ties 50 --BL "$pid" \
            --out_dir "$OUT/rh_rev_pin" > "$OUT/roundhouse_rev_pin$pid.log"
        got=$(first_match "$OUT/rh_rev_pin"/roundhouse_*rev_*_miss0.csv)
        n=0; [ -n "$got" ] && [ -s "$got" ] && n=1
        [ "$n" = "$want" ] || fail "--reverse --BL $pid: expected emitted=$want, got $n"
    done
    echo "ok: --BL still names the input board's bottom-left corner under --reverse"

    # A break inside the kept core must be reported on the user's board, not in
    # mirror space. The fixture swaps the pieces at (4,6) and (4,11), so both
    # directions have to name a row-4 cell -- and un-mirroring the column is the
    # only way the reversed run can.
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_corebreak.csv" --reverse --rounds 1 \
        --strip_width 3 --rotate 1 --out_dir "$OUT/rh_rev_cb" \
        > "$OUT/roundhouse_rev_cb.log" 2>&1 || true
    grep -qE "break inside the kept region at \(4,[0-9]+\)-\(4,[0-9]+\)" "$OUT/roundhouse_rev_cb.log" || \
        { cat "$OUT/roundhouse_rev_cb.log"; fail "the core break was not reported in the input's coordinates"; }
    echo "ok: a core break is reported on the input board, not on its mirror"
}

# --hold_band stops the last round freeing what is already standing in its band,
# so two passes can compound instead of the second erasing the first. The fixture
# is the known solution with the near half of the right band emptied: eight
# complete chain levels stay at the far end, and the search has to fill up to
# MEET them. Getting all 256 pieces back is the assertion -- the held pieces are
# never re-placed, so the run can only reach 480 by filling to fit them.
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
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_hold.csv" --rounds 1 --strip_width 3 \
        --rotate 0 --hold_band --ties 1 --out_dir "$OUT/rh_hold" \
        > "$OUT/roundhouse_hold.log" 2>&1
    grep -q "holding 24 piece(s) = 8 chain level(s)" "$OUT/roundhouse_hold.log" || \
        { cat "$OUT/roundhouse_hold.log"; fail "--hold_band did not hold the standing levels"; }
    comp=$(first_match "$OUT"/rh_hold/roundhouse_*_miss0.csv)
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
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_damaged.csv" --rounds 1 \
        --strip_width 3 --hold_band --out_dir "$OUT/rh_hold_fb" \
        > "$OUT/roundhouse_hold_fb.log" 2>&1 || true
    grep -q "not holdable" "$OUT/roundhouse_hold_fb.log" || \
        { cat "$OUT/roundhouse_hold_fb.log"; fail "an unholdable band was not reported"; }
    fb=$(first_match "$OUT"/rh_hold_fb/roundhouse_*_miss0.csv)
    [ -s "$fb" ] || { tail -5 "$OUT/roundhouse_hold_fb.log"; fail "the fallback did not search"; }
    # --stop_row would move the strip's top, which the held block already fixes.
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_hold.csv" --rounds 1 --strip_width 3 \
        --hold_band --stop_row 5 --out_dir "$OUT/rh_hold_bad" \
        > "$OUT/roundhouse_hold_bad.log" 2>&1 && fail "--hold_band --stop_row was accepted"
    grep -q "both set where the strip ends" "$OUT/roundhouse_hold_bad.log" || \
        { cat "$OUT/roundhouse_hold_bad.log"; fail "the conflicting pair was not explained"; }
    echo "ok: held levels met, a damaged band falls back and still runs, --stop_row refused"
}

# The roundhouse only ever places pieces matching on every committed side, so a
# break or a frame violation in its output is a bug, not merely a worse board.
step_roundhouse_legal() {
    rh_fixtures
    bin/E555_roundhouse data/synth_seed.txt "$OUT/rh_rows10.csv" --rounds 3 --strip_width 5 \
        --ties 4 --wall_time 60 --out_dir "$OUT/rh_r3" > "$OUT/roundhouse_r3.log"
    python3 - data/synth_seed.txt "$(first_match "$OUT"/rh_r3/roundhouse_*_miss0.csv)" <<'EOF' || exit 1
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
    # ender ring-sweep, then a localized patch pass, each feeding the next
    python3 src/C_tail/E555_ender.py data/seed_Edge5.txt \
        "$OUT/chain1.csv" "$OUT/chain2.csv" \
        --mode ring --reach 1 --max_changes 4 --time_limit 12 --stall_time 6 --threads 4 > "$OUT/ender_ring.log"
    python3 src/C_tail/E555_ender.py data/seed_Edge5.txt \
        "$OUT/chain2.csv" "$OUT/chain3.csv" \
        --reach 1 --max_changes 4 --time_limit 12 --stall_time 6 --threads 4 > "$OUT/ender_patch.log"
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
    CMD+=(--db_file "$GATE_DB")
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
    comp=$(first_match "$OUT/ex04"/roundhouse_*.csv)
    [ -s "$comp" ] || { tail -5 "$OUT/ex04.log"; fail "examples/03 emitted nothing"; }
    nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$comp")
    [ "$nf" = "514" ] || fail "examples/03 wrote $nf fields, want 514"
    echo "ok: examples/03 refilled one strip, canonical output"
}

# examples/06 chains two roundhouse passes per board, once each way round, and
# --hold_band lets the second keep what the first left. On the synthetic board
# the first pass of each chain already closes the puzzle, so both chains report
# 480 and the ids carry every stage: the input's own, the tool's suffix, then
# the pass that wrote it.
step_example_bothways() {
    rh_fixtures
    bash examples/06_roundhouse_both_ways.sh SEED=data/synth_seed.txt \
        BOARDS="$OUT/rh_rows12.csv" OUT_DIR="$OUT/ex06" \
        ROUNDS=1 WIDTH=3 ROTATE=1 N_LINES=1 HOLD=1 MAX_WALL=120 \
        THREADS=4 > "$OUT/ex06.log" \
        || { tail -5 "$OUT/ex06.log"; fail "examples/06 exited non-zero"; }
    grep -q "a=480 *b=480" "$OUT/ex06.log" || \
        { tail -8 "$OUT/ex06.log"; fail "both chains should reach the known solution"; }
    comp=$(first_match "$OUT/ex06"/cfg0/a1/roundhouse_*_miss0.csv)
    [ -s "$comp" ] || { tail -5 "$OUT/ex06.log"; fail "examples/06 emitted nothing"; }
    nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$comp")
    [ "$nf" = "514" ] || fail "examples/06 wrote $nf fields, want 514"
    id=$(awk -F, '!/^ *[#%]/{print $1; exit}' "$comp")
    case "$id" in board_*_a1) ;; *) fail "id lost its provenance: $id" ;; esac
    # Holding a whole band leaves no pool and no strip. That is a result, not a
    # failure: the run says so and the chain carries on.
    grep -q "nothing to search" "$OUT/ex06/cfg0/a2/log" || \
        { tail -5 "$OUT/ex06/cfg0/a2/log"; fail "a fully held band should report, not abort"; }
    echo "ok: examples/06 ran both chains to 480, id $id"
}

step_example_cpsat() {
    if ! python3 -c "import ortools" 2>/dev/null; then
        echo "SKIPPED: OR-Tools not installed (pip install ortools)"
        return 0
    fi
    # One script now: topper -> ender ring -> ender patch, the documented
    # Stage C sequence, each pass fed by the previous one.
    bash examples/04_stage_c_close.sh OUT_DIR="$OUT/ex04" \
        SIDE=T WORK_ROWS=3 N_LINES=1 REACH=1 MAX_CHANGES=4 \
        WORKERS=4 MAX_TIME=10 STALL_TIME=5 > "$OUT/ex04c.log" \
        || { tail -5 "$OUT/ex04c.log"; fail "examples/04 exited non-zero"; }
    for f in "$OUT/ex04/1_topped.csv" "$OUT/ex04/2_ringed.csv" "$OUT/ex04/3_patched.csv"; do
        [ -s "$f" ] || fail "$(basename "$f") was not written"
        nf=$(awk -F, '!/^ *[#%]/{print NF; exit}' "$f")
        [ "$nf" = "514" ] || fail "$(basename "$f") has $nf fields, want 514"
    done
    echo "ok: examples/04 chained three passes, all outputs canonical"
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
        ROTATIONS="$OUT/ex01_rotations.csv" BORDERS=2 STEPS=2000 THREADS=4 \
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
