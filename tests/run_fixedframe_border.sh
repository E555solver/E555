#!/bin/bash
# run_fixedframe_border.sh -- does a prior-guided border survive deeper?
#
#   bash tests/run_fixedframe_border.sh
#   bash tests/run_fixedframe_border.sh WALL=1800 RESTARTS=24 STOP_ROW=11
#   bash tests/run_fixedframe_border.sh CORPUS=ff_out/corpus.csv
#
# THE LOOP THIS CLOSES
#
# Stage A makes borders, Stage B searches them, and until now nothing carried
# information from B back to A. The corner study measured, from 55,712 partials
# in one pinned frame, which edge pieces sit on which side of the border in
# boards that got deep. This runs the whole circuit:
#
#   1. corpus            -> tests/E555_border_prior.py       -> border_prior.txt
#   2. border_prior.txt  -> the fixed-frame annealer          -> two border pools
#   3. each pool         -> tests/check_frame_border.py       -> verified
#   4. each pool         -> bin/E555_beamer_FixedFrame        -> two --verbose logs
#   5. the two logs      -> tests/E555_ab_analyze.py          -> the answer
#
# THE ARMS
#
#   guided     the annealer with --prior: trail targets plus the measured
#              affinity and spread terms
#   plain      the same annealer, same seed, same restarts and steps, with
#              --w_affinity 0 --w_spread 0 -- the original Stage A objective
#              with the corners pinned
#
# Both arms pin the SAME four corners, so the beamer runs the identical frame
# over both and the only difference is which 14 edge pieces sit on each side.
# That is the whole point: this measures the prior, not the frame, and not the
# corner bet (which no experiment here can test -- see the annealer's header).
#
# WHAT IS MEASURED
#
# Not emissions. At a high --stop_row with every clue on, a small machine emits
# essentially nothing and counting boards compares zero against zero. Both arms
# run --verbose and the comparison uses the per-row `uniq` the beam prints on
# the way up, exactly as tests/run_fixedframe_ab.sh does. tests/E555_ab_analyze.py
# reports survival to each row and the median frontier width among the borders
# that got there.
#
# WHY THIS COMPARISON IS FAIR AND THE EXCLUSION A/B WAS NOT
#
# Excluding pieces shrinks the chain database, so that experiment paid a cost at
# every border to buy a property. Choosing WHICH edge pieces go on which side
# costs nothing at all: the database is identical, the piece budget is identical,
# and both arms hand the beamer the same number of borders. Any difference is
# the prior.
#
# AND WHAT THIS DESIGN GIVES AWAY, DELIBERATELY
#
# TOP_BOTTOMS caps both arms at the same number of (bottom x column) configs per
# border. A guided border typically admits MORE orderings than a plain one -- the
# annealer reports 864..1920 Euler trails per side against 936..1152 -- and this
# cap throws that advantage away, crediting the prior only for the quality of the
# configs it produces and not at all for producing more of them. The unfair
# version of this experiment, uncapped, would look better. This one is the
# conservative reading.
set -uo pipefail

# ---- settings: edit here, or pass NAME=value on the command line -------------
REPO=$(cd "$(dirname "$0")/.." && pwd)
SEED=data/seed_Edge5.txt                # relative to REPO
OUT_DIR=border_out                      # relative to where you START it
CORPUS=                                 # corpus to measure the prior from; empty
                                        # = unpack the committed one
PRIOR=                                  # skip step 1 by naming a prior file

RESTARTS=16             # borders per arm, before --pool
STEPS=200000            # annealing steps per restart
POOL=1                  # borders kept per restart (see --pool in the annealer)
TARGET_SCALE=1000
W_AFFINITY=3
W_SPREAD=1
ANNEAL_THREADS=8
RNG_SEED=20260914       # both arms share it: same walks, different objective

THREADS=8
WALL=900                # beamer seconds PER ARM
STOP_ROW=10             # NOT 11, and this one is expensive to get wrong. At 11 a
                        # config that reaches row 10 with a handful of states can
                        # spend HOURS on the row-11 expansion: from 90 survivors
                        # and a 400k expanded width the generator has an enormous
                        # space to grind through and very few legal completions in
                        # it. Neither --time_limit nor --wall_time can preempt
                        # that, because both are only consulted at row boundaries
                        # inside beam_search_config -- a single runaway row runs to
                        # completion whatever the budget says. 10 is the depth the
                        # farm itself ran at, it is the depth the prior was
                        # measured at, and the metric here is per-row `uniq` rather
                        # than emissions, so the shallower stop costs nothing.
TIME_LIMIT=60           # per-config budget. Hygiene only, for the reason above:
                        # it bounds a slow config, not a runaway row.
BEAM_WIDTH=100000
MAX_PER_CONFIG=4        # this is about depth, not about boards
TOP_BOTTOMS=150         # (bottom x left-column) configs tried per border. The
TOP_COLUMNS=1           # default 10 is far too few here: about 96% of configs
                        # die on the clue rows, so a border needs a hundred-odd
                        # tries before it has said anything. 0 = every ordering,
                        # which on a trail-rich side is thousands and would spend
                        # the whole budget on the first border.

CLUE_ORIENT=0           # the canonical frame -- the one the corpus measured
CANON_BL=3; CANON_BR=2; CANON_TL=0; CANON_TR=1
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "Unrecognised argument: $arg" >&2; exit 2 ;;
    esac
done

BIN="$REPO/bin/E555_beamer_FixedFrame"
[ -x "$BIN" ] || { echo "Missing $BIN -- run: make -C tests" >&2; exit 1; }

mkdir -p "$OUT_DIR"; OUT_DIR=$(cd "$OUT_DIR" && pwd)
echo "[border] out_dir=$OUT_DIR"
echo "[border] frame: BL=$CANON_BL BR=$CANON_BR TL=$CANON_TL TR=$CANON_TR  orient=$CLUE_ORIENT"

# ---- 1. the prior -----------------------------------------------------------
if [ -z "$PRIOR" ]; then
    PRIOR="$OUT_DIR/prior/border_prior.txt"
    if [ -s "$PRIOR" ]; then
        echo "[border] reusing $PRIOR"
    else
        if [ -z "$CORPUS" ]; then
            # The committed corpus is 16 MB packed and 79 MB raw, so it lives
            # gzipped and is unpacked to a scratch file rather than to the repo.
            CORPUS="$OUT_DIR/corpus_row10.csv"
            if [ ! -s "$CORPUS" ]; then
                echo "[border] unpacking the committed corpus (79 MB raw)..."
                gunzip -c "$REPO/tests/results/data/corpus_row10.csv.gz" > "$CORPUS" \
                    || { echo "[border] no corpus: pass CORPUS=..." >&2; exit 1; }
            fi
        fi
        echo "[border] measuring the prior from $CORPUS"
        python3 "$REPO/tests/E555_border_prior.py" "$CORPUS" \
            --out_dir "$OUT_DIR/prior" \
            --canon_BL "$CANON_BL" --canon_BR "$CANON_BR" \
            --canon_TL "$CANON_TL" --canon_TR "$CANON_TR" \
            --orient "$CLUE_ORIENT" 2>&1 | tee "$OUT_DIR/prior.log"
        [ -s "$PRIOR" ] || { echo "[border] the prior build produced nothing" >&2; exit 1; }
    fi
fi

# ---- 2. the two border pools ------------------------------------------------
anneal() {              # $1 = arm name, $2.. = extra flags
    local arm="$1"; shift
    local csv="$OUT_DIR/borders_$arm.csv"
    rm -f "$csv"        # the annealer APPENDS, and a stale pool would be searched
    echo "[border] ===== annealing: $arm ====="
    python3 -u "$REPO/tests/E555_edge_annealer_FixedFrame.py" "$REPO/$SEED" \
        --prior "$PRIOR" --out "$csv" \
        --restarts "$RESTARTS" --steps "$STEPS" --pool "$POOL" \
        --threads "$ANNEAL_THREADS" --rng_seed "$RNG_SEED" \
        --target_scale "$TARGET_SCALE" \
        --canon_BL "$CANON_BL" --canon_BR "$CANON_BR" \
        --canon_TL "$CANON_TL" --canon_TR "$CANON_TR" \
        "$@" > "$OUT_DIR/anneal_$arm.log" 2>&1
    tail -6 "$OUT_DIR/anneal_$arm.log"
    # Verified before it is searched, not after: a border that fails here would
    # make the beamer's half of the experiment meaningless, and the failure
    # would look like a null result rather than like a bug.
    python3 "$REPO/tests/check_frame_border.py" "$REPO/$SEED" "$csv" \
        --prior "$PRIOR" \
        --canon_BL "$CANON_BL" --canon_BR "$CANON_BR" \
        --canon_TL "$CANON_TL" --canon_TR "$CANON_TR" \
        > "$OUT_DIR/check_$arm.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "[border] $arm: BORDERS FAILED VERIFICATION, see $OUT_DIR/check_$arm.log" >&2
        exit 4
    fi
    tail -1 "$OUT_DIR/check_$arm.log"
}

anneal guided --w_affinity "$W_AFFINITY" --w_spread "$W_SPREAD"
anneal plain  --w_affinity 0             --w_spread 0

# ---- 3. the beamer, once per arm, equal wall time ---------------------------
# The chain database depends on the seed and the clue set, not on the border, so
# the two arms share one cache: built once by whichever runs first, mmapped in
# seconds by the second. That also removes build time as a confound.
DB="$OUT_DIR/chain.db"

beam() {                # $1 = arm name
    local arm="$1"
    local dir="$OUT_DIR/beam_$arm"
    mkdir -p "$dir"
    echo "[border] ===== beaming: $arm ====="
    "$BIN" "$REPO/$SEED" "$OUT_DIR/borders_$arm.csv" \
        --clue_orient "$CLUE_ORIENT" \
        --canon_BL "$CANON_BL" --canon_BR "$CANON_BR" \
        --canon_TL "$CANON_TL" --canon_TR "$CANON_TR" \
        --out_dir "$dir" --db_file "$DB" --threads "$THREADS" \
        --beam_width "$BEAM_WIDTH" --stop_row "$STOP_ROW" \
        --top_bottoms "$TOP_BOTTOMS" --top_columns "$TOP_COLUMNS" \
        --time_limit "$TIME_LIMIT" \
        --max_per_config "$MAX_PER_CONFIG" --emit_mode sample \
        --wall_time "$WALL" --verbose \
        > "$dir/run.log" 2>&1
    local n b
    n=$(grep -c '^\[sweep\]' "$dir/run.log" || true)
    b=$(grep -c '^\[beam\]'  "$dir/run.log" || true)
    echo "[border] $arm: $n config(s) swept, $b per-row samples -> $dir/run.log"
}

beam plain
beam guided

# ---- 4. the answer ----------------------------------------------------------
echo
echo "[border] ===== the comparison ====="
python3 "$REPO/tests/E555_ab_analyze.py" \
    "$OUT_DIR/beam_plain/run.log" "$OUT_DIR/beam_guided/run.log" \
    --label_a plain --label_b guided \
    --json_out "$OUT_DIR/ab_border.json" | tee "$OUT_DIR/ab_border.txt"

echo
echo "[border] everything is under $OUT_DIR"
