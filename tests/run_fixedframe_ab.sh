#!/bin/bash
# run_fixedframe_ab.sh -- does excluding the far-side pieces actually help?
#
#   bash tests/run_fixedframe_ab.sh EXCLUDE=233,244,235 WALL=1800
#   bash tests/run_fixedframe_ab.sh EXCLUDE=$(cat ff_out/stats/exclude_pieces.txt)
#
# THE TEST
#
# Everything else in this experiment is description. This is the only part that
# decides anything: run the beamer in the canonical frame twice, identically
# except that one arm bars the pieces the corpus says belong in rows 13-15, and
# see whether the search gets deeper.
#
# Both arms use --clue_orient 0 (the frame the corpus was measured in) and the
# same --stop_row, --beam_width, --wall_time and thread count. Each arm gets its
# own RNG seed stream but sweeps hundreds of random borders, so the comparison is
# between two RATES, not two lucky borders.
#
# WHY THE ARMS ARE NOT PAIRED
#
# You might expect the same --rng_seed to give both arms the same borders, making
# this a paired test. It does not: the border sampler ranks candidates by fan-out
# into the chain database, and excluding pieces changes that database, so the two
# arms diverge from the first border. Hence the unpaired design and the emphasis
# on config COUNT -- tests/E555_ab_analyze.py reports the rate difference with a
# confidence interval rather than a per-border delta.
#
# THE PIECE BUDGET, WHICH BOUNDS THE WHOLE IDEA
#
# Rows 1..14 hold 14x14 = 196 inner cells and the puzzle has exactly 196 inner
# pieces: there are no spares. A beam to row R places R*14 of them and two more
# are reserved for the row-13 clues, so
#
#     slack = 194 - R*14        R=10 -> 54,  R=11 -> 40,  R=12 -> 26
#
# Excluding K pieces leaves slack-K. Push K toward the slack and the run collapses
# no matter how good the statistics are, so keep K well under half. The stats tool
# caps its own suggestion at slack/2 for this reason.
set -uo pipefail

# ---- settings ---------------------------------------------------------------
REPO=$(cd "$(dirname "$0")/.." && pwd)
SEED=data/seed_Edge5.txt
OUT_DIR=ab_out
THREADS=8
WALL=1800               # seconds PER ARM
STOP_ROW=12             # the depth we are trying to reach more often
BEAM_WIDTH=100000
MAX_PER_CONFIG=4        # this run is about depth, not about boards
CLUE_ORIENT=0           # the canonical frame -- the one the corpus measured
EXCLUDE=                # required: comma-separated piece ids
BASE_DB=                # optional path to an existing clue-built cache to reuse
CANON_BL=3; CANON_BR=2; CANON_TL=0; CANON_TR=1
RNG_SEED=0
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "Unrecognised argument: $arg" >&2; exit 2 ;;
    esac
done

BIN="$REPO/bin/E555_beamer_FixedFrame"
[ -x "$BIN" ] || { echo "Missing $BIN -- run: make beamer_fixedframe" >&2; exit 1; }
[ -n "$EXCLUDE" ] || { echo "EXCLUDE= is required (a comma-separated piece list)" >&2; exit 2; }

NEXCL=$(tr ',' '\n' <<<"$EXCLUDE" | grep -c .)
SLACK=$(( 194 - STOP_ROW * 14 ))
echo "[ab] excluding $NEXCL piece(s); a beam to row $STOP_ROW has $SLACK spare inner pieces"
if [ "$NEXCL" -ge "$SLACK" ]; then
    echo "[ab] REFUSING: excluding $NEXCL of $SLACK spare pieces leaves the beam" >&2
    echo "[ab] no room to fill rows 1..$STOP_ROW. Lower the list or the stop row." >&2
    exit 3
fi

mkdir -p "$OUT_DIR"; OUT_DIR=$(cd "$OUT_DIR" && pwd)
echo "[ab] out_dir=$OUT_DIR wall=${WALL}s/arm threads=$THREADS stop_row=$STOP_ROW"

run_arm() {           # $1 = arm name, $2 = extra args (may be empty)
    local arm="$1"; shift
    local dir="$OUT_DIR/$arm"
    mkdir -p "$dir"
    local db="$dir/chain.db"
    # The baseline can reuse a cache the farm already built; the excluded arm
    # cannot -- a different piece set is a different database, and the cache
    # header hashes the exclusion set so a stale one is refused, not reused.
    if [ "$arm" = "baseline" ] && [ -n "$BASE_DB" ] && [ -s "$BASE_DB" ]; then
        db="$BASE_DB"
    fi
    local seed_arg=()
    [ "$RNG_SEED" != "0" ] && seed_arg=(--rng_seed "$RNG_SEED")
    echo "[ab] ===== arm: $arm ====="
    "$BIN" "$REPO/$SEED" \
        --clue_orient "$CLUE_ORIENT" \
        --canon_BL "$CANON_BL" --canon_BR "$CANON_BR" \
        --canon_TL "$CANON_TL" --canon_TR "$CANON_TR" \
        --out_dir "$dir" --db_file "$db" --threads "$THREADS" \
        --beam_width "$BEAM_WIDTH" --stop_row "$STOP_ROW" \
        --max_per_config "$MAX_PER_CONFIG" --emit_mode sample \
        --wall_time "$WALL" "${seed_arg[@]}" "$@" > "$dir/run.log" 2>&1
    local n; n=$(grep -c '^\[sweep\]' "$dir/run.log" || true)
    echo "[ab] $arm: $n border config(s) swept -> $dir/run.log"
}

run_arm baseline
run_arm excluded --exclude_pieces "$EXCLUDE"

echo
echo "[ab] done. Now:"
echo "  python3 tests/E555_ab_analyze.py $OUT_DIR/baseline/run.log $OUT_DIR/excluded/run.log"
