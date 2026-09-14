#!/bin/bash
# run_fixedframe_farm.sh -- harvest row-11 partials from all four sides of ONE
# pinned frame, then turn them all onto that frame.
#
#   bash tests/run_fixedframe_farm.sh
#   bash tests/run_fixedframe_farm.sh WALL=1800 THREADS=16
#   bash tests/run_fixedframe_farm.sh OUT_DIR=ff2 EXCLUDE=41,77,190
#
# WHAT IT DOES
#
# The beamer fills rows 0..11 bottom-up and dies attempting row 12, so one run
# only ever sees a 12-row band. This runs the same frame four times, once per
# side, and turns each side's output back onto the canonical frame -- four bands
# that between them cover the whole board, every board in one coordinate system.
#
#   side 0  bottom band   rows 0..11      turn 0
#   side 1  a 90-degree view            turn 3
#   side 2  a 180-degree view           turn 2
#   side 3  a 270-degree view           turn 1
#
# The turn is (4 - side) % 4 clockwise, and the beamer prints it per run. It is
# lossless: E555_rotate.py re-scores every row and says so.
#
# WHAT YOU GET, all under OUT_DIR:
#   beam_ff_s<N>_row11.csv    each side as searched, in its own frame
#   canon_s<N>.csv            the same boards turned onto the canonical frame
#   corpus.csv                all four concatenated -- this is the input to
#                             tests/E555_frame_stats.py
#   farm.log                  every run's full output
#
# TIME. Governed by WALL seconds PER SIDE, not by a config count, so a session is
# "an hour of borders" and needs no throughput guess. Most borders die at row 1
# or 2 in milliseconds against the clue pins; the rare survivor costs ~6s and
# yields a handful of boards. Measured on 4 cores: roughly 2 configs/sec scanned,
# about 1 in 30-130 reaching row 11. Breadth is the point -- each surviving
# border is one independent observation however many boards it emits.
#
# The first run builds the 6.4 GB chain database (~80s) and caches it to
# DB_FILE; the other three sides mmap it in seconds. Clue pieces leave the
# database, so this cache is specific to a clued run -- and an EXCLUDE list
# changes it again, which is why that gets its own cache path below.
set -uo pipefail        # not -e: one barren side must not kill the farm

# ---- settings: edit here, or pass NAME=value on the command line ------------
REPO=$(cd "$(dirname "$0")/.." && pwd)  # E555 checkout. Set this if you copied
                                        # this script somewhere else.
SEED=data/seed_Edge5.txt                # relative to REPO
OUT_DIR=ff_out                          # relative to where you START it
THREADS=8
WALL=900                # seconds PER SIDE; four sides, so 900 is ~1 hour total
STOP_ROW=11             # last row filled. 7 already covers the board from four
                        # sides and yields far more boards; 11 gives deeper,
                        # rarer material. See the note at the bottom.
BEAM_WIDTH=100000
MAX_PER_CONFIG=32       # boards written per border (0 = every survivor)
EMIT_MODE=sample        # sample | top -- see --help
DB_FILE=ff_chain.db     # relative to OUT_DIR; delete it to force a rebuild

# The canonical frame. The clue set is published and fixed; the corner
# assignment is NOT -- no clue pins a corner, so this is one of 4! = 24 possible
# assignments and only one of them is the solution's. It conditions the border
# heavily and the core barely. Change all four together or not at all.
CANON_BL=3
CANON_TL=0
CANON_BR=2
CANON_TR=1

EXCLUDE=                # comma-separated piece ids to bar from the database.
                        # The payoff loop: mine a list from a first corpus, put
                        # it here, and see whether row 12 gets easier.
RNG_SEED=0              # 0 = fresh per side from clock+pid (and printed). Set it
                        # for a reproducible farm; each side gets SEED+side.
# -----------------------------------------------------------------------------
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) declare "$arg" ;;
        *) echo "Unrecognised argument: $arg" >&2; exit 2 ;;
    esac
done

BIN="$REPO/bin/E555_beamer_FixedFrame"
if [ ! -x "$BIN" ]; then
    echo "Missing $BIN -- run: make beamer_fixedframe" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
LOG="$OUT_DIR/farm.log"
: > "$LOG"

# A corpus is grouped by config id downstream, and every run stamps its own tag
# into those ids, so appending across farm runs is safe. The per-side files are
# opened in APPEND mode by the beamer though, so a re-run into the same OUT_DIR
# grows them rather than replacing them -- which is usually what you want. Pass
# FRESH=1 to start over instead.
if [ "${FRESH:-0}" = "1" ]; then
    echo "[farm] FRESH=1: removing previous boards in $OUT_DIR"
    rm -f "$OUT_DIR"/beam_ff_s*_row*.csv "$OUT_DIR"/canon_s*.csv "$OUT_DIR"/corpus.csv
fi

EXCL_ARGS=()
DB_PATH="$OUT_DIR/$DB_FILE"
if [ -n "$EXCLUDE" ]; then
    EXCL_ARGS=(--exclude_pieces "$EXCLUDE")
    # A different piece set is a different database. The cache header hashes the
    # exclusion set so a stale cache is REFUSED rather than silently reused, but
    # sharing one path would still mean the two runs take turns rebuilding it.
    DB_PATH="$OUT_DIR/excl_$DB_FILE"
    echo "[farm] excluding pieces: $EXCLUDE (own cache: $DB_PATH)"
fi

echo "[farm] out_dir=$OUT_DIR wall=${WALL}s/side threads=$THREADS stop_row=$STOP_ROW"
echo "[farm] frame: canonical BL=$CANON_BL BR=$CANON_BR TL=$CANON_TL TR=$CANON_TR"
t0=$(date +%s)

for side in 0 1 2 3; do
    seed_arg=()
    [ "$RNG_SEED" != "0" ] && seed_arg=(--rng_seed $(( RNG_SEED + side )))
    echo "[farm] ===== side $side =====" | tee -a "$LOG"
    "$BIN" "$REPO/$SEED" \
        --clue_orient "$side" \
        --canon_BL "$CANON_BL" --canon_BR "$CANON_BR" \
        --canon_TL "$CANON_TL" --canon_TR "$CANON_TR" \
        --out_dir "$OUT_DIR" \
        --db_file "$DB_PATH" \
        --threads "$THREADS" \
        --beam_width "$BEAM_WIDTH" \
        --stop_row "$STOP_ROW" \
        --max_per_config "$MAX_PER_CONFIG" \
        --emit_mode "$EMIT_MODE" \
        --wall_time "$WALL" \
        "${EXCL_ARGS[@]}" "${seed_arg[@]}" >> "$LOG" 2>&1

    raw="$OUT_DIR/beam_ff_s${side}_row${STOP_ROW}.csv"
    if [ ! -s "$raw" ]; then
        echo "[farm] side $side emitted nothing (see $LOG)" | tee -a "$LOG"
        continue
    fi
    # (4 - side) % 4 clockwise quarter-turns puts this side onto the canonical
    # frame. The beamer prints the same number in its [frame] banner; if the two
    # ever disagree, trust neither and re-run the frame check in run_tests.sh.
    turn=$(( (4 - side) % 4 ))
    python3 "$REPO/tools/E555_rotate.py" "$raw" "$turn" \
        --out "$OUT_DIR/canon_s${side}.csv" >> "$LOG" 2>&1 \
        || { echo "[farm] side $side: rotate failed, see $LOG" >&2; continue; }
    n=$(grep -vc '^[#%]' "$OUT_DIR/canon_s${side}.csv")
    echo "[farm] side $side: $n board(s) -> canon_s${side}.csv (turn $turn)" | tee -a "$LOG"
done

# One corpus. Boards keep their config id, which carries the side and the run, so
# nothing is lost by concatenating and the stats script groups on it.
: > "$OUT_DIR/corpus.csv"
for side in 0 1 2 3; do
    [ -s "$OUT_DIR/canon_s${side}.csv" ] && grep -v '^[#%]' "$OUT_DIR/canon_s${side}.csv" >> "$OUT_DIR/corpus.csv"
done

total=$(wc -l < "$OUT_DIR/corpus.csv")
configs=$(cut -d, -f1 "$OUT_DIR/corpus.csv" | sort -u | wc -l)
echo "[farm] done in $(( $(date +%s) - t0 ))s: $total board(s) from $configs border config(s)"
echo "[farm] corpus -> $OUT_DIR/corpus.csv"
echo "[farm] next:  python3 tests/E555_frame_stats.py $OUT_DIR/corpus.csv --out_dir $OUT_DIR/stats"

# A NOTE ON STOP_ROW. Four sides each filling R rows cover the whole board as
# soon as 2R >= 16, i.e. STOP_ROW >= 7. Rows 8..11 are where the beam is most
# squeezed, so lowering STOP_ROW buys orders of magnitude more boards from more
# borders while still covering every cell -- at the price of shallower, less
# selected material. Running the farm at 7 and again at 11 and comparing is a
# cheap way to see which conclusions depend on the depth and which do not.
