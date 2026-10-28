#!/bin/bash
# v00_baseline.sh -- pipeline/run_pipeline_whirlpool.sh as it is, under the
# harness contract (agent/whirlpool_agent_mode.md, section 3). Nothing changed:
#   produce = its stage 0 (beam 200000 to row 10, 60 samples, tau 2/4, frac_rand
#             0.10, bc_window 3,3), with --wall_time fitted to the produce cap;
#   whirl   = its lap loop with WHIRL_ROWS="10 10 10": every board turned BOTH
#             ways, one band cut over all of them (BT_LIMIT 200, 60 s), BT_PICK 6
#             bands farthest-first, one finalizer (width 100000, 4 columns,
#             60 boards), POP 40 kept. The finalizer's wall time is the lap's
#             share of what is left of the 180 s.
# Lineage: each lap board's parent (and turn) is the turned board whose rows
# 0..BAND_ROW it agrees with most; every lineage is written as its own chain.
set -euo pipefail

# ---- the contract ----------------------------------------------------------------
MODE=; REPO=; SEED=; RUN_DIR=; THREADS=4; RNG_SEED=1; TIME_LIMIT=180; DB_FILE=; INPUT=; OUT=
# ---- the runner's settings ----------------------------------------------------------
BEAM_WIDTH=200000
BEAM_STOP=10
BEAM_BOARDS=60
BEAM_COLUMNS=8
BEAM_TAU_BOTTOMS=2
BEAM_TAU_COLUMNS=4
BEAM_FRAC_RAND=0.10
BC_WINDOW=3,3
WHIRL_ROWS="10 10 10"
BAND_ROW=5
BT_LIMIT=200
BT_PICK=6
BT_ORDER=rowmajor
BT_TIME=60
POP=40
FIN_WIDTH=100000
FIN_BOARDS=60
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
rows() { python3 "$TOOLS/E555_rank.py" "$1" --count 2>/dev/null || echo 0; }

if [ "$MODE" = produce ]; then
    DB_ARG=(); [ -n "$DB_FILE" ] && DB_ARG=(--db_file "$DB_FILE")
    "$BIN/E555_beamer" "$SEED" --random_edges --out_dir beam --threads "$THREADS" \
        --beam_width "$BEAM_WIDTH" --stop_row "$BEAM_STOP" \
        --samples "$BEAM_BOARDS" --top_columns "$BEAM_COLUMNS" \
        --tau_bottoms "$BEAM_TAU_BOTTOMS" --tau_columns "$BEAM_TAU_COLUMNS" \
        --frac_rand "$BEAM_FRAC_RAND" --bc_window "$BC_WINDOW" \
        --max_emitted "$BEAM_BOARDS" --wall_time $(( $(left) - 40 )) \
        --rng_seed "$RNG_SEED" "${DB_ARG[@]}" > beam.log 2>&1 ||
        echo "[produce] the beamer exited non-zero"
    xargs -r cat < beam/outputs.txt > "$OUT" 2> /dev/null || : > "$OUT"
    echo "[produce] $(rows "$OUT") boards"
    exit 0
fi

[ "$MODE" = whirl ] || { echo "MODE must be produce or whirl" >&2; exit 2; }

# lineage.py PARENTS ROTATED CHILDREN CHAINS_IN CHAINS_OUT OUT LAP
#   PARENTS:  the lap's input boards, line i;  ROTATED: <i>,<d> per line of the
#   turned file (written alongside); CHILDREN: the lap's kept boards. Each child's
#   parent is the turned board agreeing with it most on rows 0..BAND_ROW; its chain
#   is the parent's chain plus itself, appended to OUT under a new tag.
cat > lineage.py <<'EOF'
import sys, json
band = int(sys.argv[8])
def boards(path):
    out = []
    for line in open(path):
        if not line.strip() or line[0] in "#%": continue
        f = line.rstrip("\n").split(",")
        pos = list(map(int, f[-512:-256])); rot = list(map(int, f[-256:]))
        out.append((f, {c: (p, rot[p]) for p, c in enumerate(pos) if c != 999}))
    return out
rotated = boards(sys.argv[2]); keys = [l.split(",") for l in open(sys.argv[2] + ".key")]
children = boards(sys.argv[3])
chains = json.load(open(sys.argv[4]))          # list per parent: [row strings]
new = []
for k, (f, cells) in enumerate(children):
    best, bi = -1, 0
    for j, (_, rc) in enumerate(rotated):
        s = sum(1 for c, v in cells.items() if c < (band + 1) * 16 and rc.get(c) == v)
        if s > best: best, bi = s, j
    pi = int(keys[bi][0])
    new.append(chains[pi] + [",".join(f)])
json.dump(new, open(sys.argv[5], "w"))
with open(sys.argv[6], "a") as o:
    for k, ch in enumerate(new):
        tag = "L%sc%d" % (sys.argv[7], k)
        for s, row in enumerate(ch):
            f = row.split(","); f[0] = "%s_s%d" % (tag, s + 1)
            o.write(",".join(f) + "\n")
EOF
first() { awk '!/^[#%]/ && NF {print; exit}' "$1"; }
first "$INPUT" | awk -F, 'BEGIN{OFS=","}{$1="t_s1"; print}' >> "$OUT"
first "$INPUT" > cur.csv
python3 -c 'import json,sys; json.dump([[open("cur.csv").read().strip()]], open("chains.json","w"))'

k=0
for target in $WHIRL_ROWS; do
    k=$((k + 1))
    rm -f rot.csv rot.csv.key bt.csv bt.csv.* band.csv next.csv
    n=$(rows cur.csv)
    python3 "$TOOLS/E555_rotate.py" cur.csv 1 --out cw.csv  --seed_file "$SEED" > /dev/null
    python3 "$TOOLS/E555_rotate.py" cur.csv 3 --out ccw.csv --seed_file "$SEED" > /dev/null
    cat cw.csv ccw.csv > rot.csv
    { for ((i = 0; i < n; i++)); do echo "$i,1"; done; for ((i = 0; i < n; i++)); do echo "$i,3"; done; } > rot.csv.key
    bt_t=$BT_TIME; [ "$bt_t" -lt "$(( $(left) / 3 ))" ] || bt_t=$(( $(left) / 3 ))
    "$BIN/E555_backtracker" "$SEED" rot.csv bt.csv \
        --stop_row "$BAND_ROW" --order "$BT_ORDER" --break_mode any --with_frame \
        --breaks 0 --max_emitted "$BT_LIMIT" --time_limit "$bt_t" --threads "$THREADS" > "bt$k.log" 2>&1 || true
    [ -s "bt.csv.stop_row$BAND_ROW.csv" ] && mv "bt.csv.stop_row$BAND_ROW.csv" band.csv || : > band.csv
    if [ "$(rows band.csv)" -gt "$BT_PICK" ]; then
        python3 "$TOOLS/E555_rank.py" band.csv --seed_file "$SEED" --diverse "$BT_PICK" \
            --out band.tmp > /dev/null 2>&1 && mv band.tmp band.csv
    fi
    nb=$(rows band.csv)
    [ "$nb" -gt 0 ] || { echo "[whirl] lap $k: no band"; exit 0; }
    budget=$(( $(left) / (4 - k) - 5 ))
    [ "$budget" -gt 5 ] || { echo "[whirl] lap $k: out of time"; exit 0; }
    rm -rf fin
    "$BIN/E555_finalizer" "$SEED" band.csv --out_dir fin --threads "$THREADS" \
        --finalize_from "$BAND_ROW" --finalize_repeats 1 \
        --beam_width "$FIN_WIDTH" --stop_row "$target" \
        --num_rows "$nb" --top_columns "$FIN_COLUMNS" \
        --lambda_Mahalanobis 0 --frac_rand 0.0 \
        --max_emitted "$FIN_BOARDS" --wall_time "$budget" --rng_seed "$RNG_SEED" > "fin$k.log" 2>&1 || true
    xargs -r cat < fin/outputs.txt > next.csv 2> /dev/null || : > next.csv
    [ "$(rows next.csv)" -gt 0 ] || { echo "[whirl] lap $k: $nb bands, none re-grew"; exit 0; }
    python3 "$TOOLS/E555_rank.py" next.csv --seed_file "$SEED" --sort breaks,break_rows \
        --out next.tmp --rescore > /dev/null && mv next.tmp next.csv
    awk '!/^[#%]/ && NF' next.csv | head -n "$POP" > kept.csv || true
    python3 lineage.py cur.csv rot.csv kept.csv chains.json chains.new "$OUT" "$k" "$BAND_ROW"
    mv chains.new chains.json; mv kept.csv cur.csv
    echo "[whirl] lap $k: $nb bands -> $(rows cur.csv) boards at $((SECONDS - START)) s"
done
