#!/bin/bash
# 07_barebones_chain.sh -- the whole E555 solver, one call per tool.
#
#   cd ~/runs && bash /path/to/E555/examples/07_barebones_chain.sh
#
# No arguments and nothing to read first. It compiles the repo, grows boards
# from random borders up to row 12 (beamer), re-searches them a second way
# (finalizer), spirals any complete row 12 through three sides (roundhouse),
# closes the rest (backtracker), then ranks the results and draws the best
# board. Every value is a literal on the line that uses it: copy any call below
# into a terminal and edit it there.
#
# About 15 minutes on four cores, 8 GB of RAM. Output lands in the directory you
# run it from; change PREFIX between runs, since the tools append to their CSVs.
#
# Row 12 is where the beam dies, so most boards arrive with a piece of it
# missing and the backtracker fills that in. A longer --wall_time in stage 1
# is the one edit that buys better boards.
set -euo pipefail
shopt -s nullglob           # an output dir a stage never wrote just disappears

REPO=$(cd "$(dirname "$0")/.." && pwd)  # the E555 checkout; set it if you moved
                                        # this file, or make says "no makefile"
THREADS=8
PREFIX=bb1                              # names every file this run writes

make -C "$REPO" --no-print-directory

echo "E555 barebones chain: beamer, finalizer, roundhouse, backtracker, rank, view."
echo "Random bottoms straight to row 12, re-searched twice more, then closed."
echo "Row 12 is the wall: mostly boards missing one of its segments come through."
date

echo; echo "1/6 beamer: random bottoms, best column each, to row 12, 25 boards or 600 s"
"$REPO/bin/E555_beamer" "$REPO/data/seed_Edge5.txt" \
    --random_edges --clue_center --stop_row 12 --incomplete_top \
    --max_emitted 25 --samples 50 --top_columns 1 --wall_time 600 \
    --threads "$THREADS" --out_dir "${PREFIX}_beam" --print_cmd

cat "${PREFIX}_beam"/beam_completions_random_12*.csv > "${PREFIX}_beam12.csv"

echo; echo "2/6 finalizer: the same boards again, lock rows 0..4, re-search rows 5..12"
# --num_rows 0 means every line of the input CSV, whatever the last stage
# produced. --clue_center is load-bearing: below finalize_from 7 the centre cell
# is freed, and without the flag the beam refills it with anything.
"$REPO/bin/E555_finalizer" "$REPO/data/seed_Edge5.txt" "${PREFIX}_beam12.csv" \
    --clue_center --finalize_from 4 --stop_row 12 --incomplete_top \
    --finalize_repeats 2 --num_rows 0 \
    --threads "$THREADS" --out_dir "${PREFIX}_fin" --print_cmd

# Only a COMPLETE row 12 is worth spiralling: the roundhouse frees and refills
# border strips, which needs a board whose rows are actually filled. Both files
# are zero bytes when their stage emitted nothing, so -s is the whole test.
cat "${PREFIX}_beam"/beam_completions_random_12.csv \
    "${PREFIX}_fin"/beam_completions_finalized_12.csv > "${PREFIX}_full12.csv"

echo; echo "3/6 roundhouse: any complete row 12, three sides freed and refilled"
if [ -s "${PREFIX}_full12.csv" ]; then
    # --strip_width is the dial here: 5 frees a five-piece band per round, 2 the
    # narrowest. Three rounds at width 5 keep only the 66-piece core.
    "$REPO/bin/E555_roundhouse" "$REPO/data/seed_Edge5.txt" "${PREFIX}_full12.csv" \
        --rotate -1 --rounds 3 --strip_width 5 --num_rows 0 \
        --wall_time 600 --threads "$THREADS" \
        --out_dir "${PREFIX}_rh" --print_cmd
else
    echo "    nothing reached a complete row 12, so there is nothing to spiral."
    echo "    Raise --wall_time on stage 1 and this stage starts doing work."
fi

cat "${PREFIX}_beam12.csv" \
    "${PREFIX}_fin"/beam_completions_finalized_12*.csv \
    "${PREFIX}_rh"/*.csv > "${PREFIX}_row12.csv"

echo; echo "4/6 backtracker: every board from all three stages, fill row 12 and 13..15"
"$REPO/bin/E555_backtracker" "$REPO/data/seed_Edge5.txt" \
    "${PREFIX}_row12.csv" "${PREFIX}_solved.csv" \
    --breaks 20 --time_limit 20 --threads "$THREADS" --print_cmd

echo; echo "5/6 rank: every board, fewest breaks first, then breaks in fewest rows"
python3 "$REPO/tools/E555_rank.py" "${PREFIX}_solved.csv" \
    --seed_file "$REPO/data/seed_Edge5.txt" --rescore --out "${PREFIX}_ranked.csv"

rm -rf "${PREFIX}_beam" "${PREFIX}_fin" "${PREFIX}_rh" "${PREFIX}_beam12.csv" \
       "${PREFIX}_full12.csv" "${PREFIX}_row12.csv" "${PREFIX}_solved.csv"*

echo; echo "6/6 best board in ${PREFIX}_ranked.csv"
python3 "$REPO/tools/E555_viewer.py" "${PREFIX}_ranked.csv" \
    --seed_file "$REPO/data/seed_Edge5.txt" --row 0

echo; echo "total ${SECONDS}s"
date
