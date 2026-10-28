#!/bin/bash
# example_candidate.sh -- the smallest whirlpool candidate that keeps the
# harness's contract (agent/whirlpool_agent_mode.md, section 3). It is a worked
# example of the contract, not a baseline and not a good candidate: one lineage,
# turned clockwise every lap, the first band that re-grows. Its lap boards lose
# the right column above row 10 (the finalizer does not write it), so the judge
# reports frame=incomplete:s2 and it finds no jewel. Copy it into the campaign's
# candidates/ to run it: bash ../../harness.sh CANDIDATE=../candidates/example.sh
#
#   MODE=produce: beamer boards, rows 0..10 complete, to OUT
#   MODE=whirl:   chains t1_s1, t1_s2, ... to OUT, one row per completed lap
set -euo pipefail

# ---- the contract: the harness passes all of these ----------------------------
MODE=               # produce | whirl
REPO=               # the lab: bin/, tools/, data/
SEED=               # the puzzle's seed file
RUN_DIR=            # a fresh directory of this call's own
THREADS=4
RNG_SEED=1
TIME_LIMIT=180      # seconds this call may take; the harness kills it after that
DB_FILE=            # produce: the chain database cache, or empty (build in memory)
INPUT=              # whirl: one beamer board
OUT=                # produce: the queue; whirl: the chains file (append)
# ---- this candidate's own settings ----------------------------------------------
BEAM_WIDTH=10000    # produce: a narrow beam to row 6, then exhaustive to row 10
BEAM_BACKTRACK=6    # (PROJECT_E555.md 5.13: the cheapest way to deep rows)
BEAM_BOARDS=300
BAND_ROW=5
BT_LIMIT=40         # bands enumerated per lap
BT_PICK=4           # bands grown, farthest-first
BT_TIME=20
FIN_WIDTH=20000
FIN_COLUMNS=4
# ---------------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 2 ;;
    esac
done
BIN=$REPO/bin; TOOLS=$REPO/tools
cd "$RUN_DIR"
START=$SECONDS
left() { echo $(( TIME_LIMIT - (SECONDS - START) )); }

if [ "$MODE" = produce ]; then
    DB_ARG=(); [ -n "$DB_FILE" ] && DB_ARG=(--db_file "$DB_FILE")
    "$BIN/E555_beamer" "$SEED" --random_edges --samples 0 --top_columns 4 \
        --out_dir beam --threads "$THREADS" --rng_seed "$RNG_SEED" \
        --beam_width "$BEAM_WIDTH" --backtrack_row "$BEAM_BACKTRACK" --stop_row 10 \
        --max_emitted "$BEAM_BOARDS" --wall_time $(( $(left) - 30 )) "${DB_ARG[@]}" > beam.log 2>&1 ||
        echo "[produce] the beamer exited non-zero; see the log"
    xargs -r cat < beam/outputs.txt > "$OUT" 2> /dev/null || : > "$OUT"
    echo "[produce] $(grep -vc '^[#%]' "$OUT" || true) boards"
    exit 0
fi

[ "$MODE" = whirl ] || { echo "MODE must be produce or whirl" >&2; exit 2; }
# first FILE -- its first board row. (Not grep | head: under pipefail, head
# closing the pipe kills grep with SIGPIPE, and set -e then ends the script.)
first() { awk '!/^[#%]/ && NF {print; exit}' "$1"; }
# has_board FILE -- true when FILE holds a board row, not only comments (the
# band cut writes its header even when it finds no band)
has_board() { [ -f "$1" ] && [ -n "$(first "$1")" ]; }
# a chain row: the board with its id replaced by <tag>_s<k>
emit() { first "$1" | awk -F, -v id="$2" 'BEGIN {OFS = ","} {$1 = id; print}' >> "$OUT"; }
emit "$INPUT" t1_s1
cp "$INPUT" cur.csv
for lap in 1 2 3; do
    python3 "$TOOLS/E555_rotate.py" cur.csv 1 --out rot.csv --seed_file "$SEED" > /dev/null
    rm -f bt.csv bt.csv.* band.csv
    "$BIN/E555_backtracker" "$SEED" rot.csv bt.csv --stop_row "$BAND_ROW" --with_frame \
        --order rowmajor --break_mode any --breaks 0 --max_emitted "$BT_LIMIT" \
        --time_limit "$BT_TIME" --threads "$THREADS" > "bt$lap.log" 2>&1 || true
    has_board "bt.csv.stop_row$BAND_ROW.csv" || { echo "[whirl] lap $lap: no band"; exit 0; }
    python3 "$TOOLS/E555_rank.py" "bt.csv.stop_row$BAND_ROW.csv" --seed_file "$SEED" \
        --diverse "$BT_PICK" --out band.csv > /dev/null 2>&1 || cp "bt.csv.stop_row$BAND_ROW.csv" band.csv
    budget=$(( $(left) / (4 - lap) - 5 ))
    [ "$budget" -gt 10 ] || { echo "[whirl] lap $lap: out of time"; exit 0; }
    rm -rf fin
    "$BIN/E555_finalizer" "$SEED" band.csv --out_dir fin --threads "$THREADS" \
        --finalize_from "$BAND_ROW" --stop_row 10 --beam_width "$FIN_WIDTH" \
        --top_columns "$FIN_COLUMNS" --lambda_Mahalanobis 0 --frac_rand 0.0 \
        --max_emitted 1 --wall_time "$budget" --rng_seed "$RNG_SEED" > "fin$lap.log" 2>&1 || true
    xargs -r cat < fin/outputs.txt > next.csv 2> /dev/null || : > next.csv
    has_board next.csv || { echo "[whirl] lap $lap: no band re-grew to row 10"; exit 0; }
    emit next.csv "t1_s$((lap + 1))"
    first next.csv > cur.csv
    echo "[whirl] lap $lap done at $((SECONDS - START)) s"
done
