#!/bin/bash
# harness.sh -- run a whirlpool candidate and score it, for the whirlpool kit
# (agent/whirlpool_agent_mode.md, section 3). The only source of a campaign's
# results: it alone writes whirlpools/, whirlpool_index.csv, attempts.csv,
# harness.csv and the candidates' totals in scoreboard.json. Run it from the
# campaign's lab, agent/whirlpool/<id>/lab/.
#
#   bash ../../harness.sh CANDIDATE=../candidates/v03_keep_ring.sh MINUTES=60 NOTE="ring held from lap 2"
#   HARNESS run=r20261027-201500 candidate=v03_keep_ring@1a2b3c4d+none minutes=60.4 standard=yes produced=812 attempted=19 funnel=lap1:15/lap2:9/lap3:6 frame_held=4 jewels=3 archived=7 per_hour=3.0 depth_max=11 ...
#
# The loop, until MINUTES of wall time have passed:
#   produce  when the queue is empty: CANDIDATE MODE=produce writes beamer
#            boards to OUT, in the order it wants them whirled. The judge keeps
#            the valid ones never whirled before in this campaign
#            (seen_boards.txt). Killed after PRODUCE_CAP seconds.
#   whirl    the next queued board: CANDIDATE MODE=whirl INPUT=<that board>
#            appends chains to OUT as its laps complete. Killed after CAP
#            seconds, or once its run directory passes DISK_CAP_MB. The judge
#            then scores the chains and archives every completed whirlpool
#            (3 laps, jewel or not) as it was grown, before any dive:
#            whirlpools/<run>.csv.gz gets its four boards, appended after each
#            attempt, and whirlpool_index.csv a line. A jewel counts only when
#            its frame is new to the campaign (frames_seen.txt). A run's file is
#            never written again once the run ends, so git keeps each jewel once.
# The candidate is called with NAME=value settings (the contract, kit section 3):
#   MODE=produce REPO=<lab> SEED=<lab>/data/seed_Edge5.txt RUN_DIR=<dir> THREADS=<n>
#                RNG_SEED=<s> DB_FILE=<lab>/E555.db or empty TIME_LIMIT=<PRODUCE_CAP> OUT=<dir>/queue.csv
#   MODE=whirl   REPO=<lab> SEED=<seed> RUN_DIR=<dir> THREADS=<n> RNG_SEED=<s>
#                TIME_LIMIT=<CAP> INPUT=<dir>/board.csv OUT=<dir>/chains.csv
# Each call gets a fresh RUN_DIR, deleted afterwards; when it ends, every
# process it started is killed, however it was started.
#
# Before the run: the candidate must be a file of this campaign (candidates/
# or run_pipeline_whirlpool.sh); the lab's src/, tools/ and Makefile must equal
# the repository's plus patches/ (else run setup.sh); make brings bin/ up to
# date. A run is standard when CAP is 180: only standard runs count towards a
# candidate's jewels per hour and the best. Exit status 0 = the run finished
# (whatever it found), 1 = it could not start, 2 = usage.
# ---- settings: pass NAME=value on the command line ---------------------------
CANDIDATE=          # the candidate script, inside this campaign's folder
MINUTES=60          # wall time of the run, 5..90
CAP=180             # seconds per whirl; the goal's limit, so other values are non-standard
PRODUCE_CAP=600     # seconds per produce call
THREADS=            # empty: every core
DISK_CAP_MB=2000    # an attempt whose run directory grows past this is killed
NOTE=               # free text for harness.csv
# -----------------------------------------------------------------------------
SETTINGS=" CANDIDATE MINUTES CAP PRODUCE_CAP THREADS DISK_CAP_MB NOTE "
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) [[ "$SETTINGS" == *" ${arg%%=*} "* ]] ||
                          { echo "unknown setting ${arg%%=*}; the settings are:$SETTINGS" >&2; exit 2; }
                      declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 2 ;;
    esac
done
set -euo pipefail

KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$KIT/../.." && pwd)
case "$PWD/" in
    "$KIT"/*/lab/*) ;;
    *) echo "harness: run it from a campaign's lab, agent/whirlpool/<id>/lab/" >&2; exit 2 ;;
esac
CAMPAIGN=${PWD#"$KIT/"}; CAMPAIGN=${CAMPAIGN%%/*}
DIR="$KIT/$CAMPAIGN"
LAB="$DIR/lab"
cd "$LAB"
JUDGE=(python3 "$KIT/judge.py")
SEED="$LAB/data/seed_Edge5.txt"
die() { echo "HARNESS refused: $*"; exit 1; }

[[ "$MINUTES" =~ ^[0-9]+$ ]] && [ "$MINUTES" -ge 5 ] && [ "$MINUTES" -le 90 ] ||
    { echo "harness: MINUTES must be 5..90 (a background command lives at most 2 h)" >&2; exit 2; }
for v in CAP PRODUCE_CAP DISK_CAP_MB; do
    [[ "${!v}" =~ ^[0-9]+$ ]] && [ "${!v}" -gt 0 ] || { echo "harness: $v must be a positive integer" >&2; exit 2; }
done
THREADS=${THREADS:-$(nproc)}
[ -n "$CANDIDATE" ] && [ -f "$CANDIDATE" ] || { echo "harness: CANDIDATE=$CANDIDATE is not a file" >&2; exit 2; }
CAND=$(cd "$(dirname "$CANDIDATE")" && pwd)/$(basename "$CANDIDATE")
case "$CAND" in
    "$DIR"/candidates/*.sh|"$DIR"/run_pipeline_whirlpool.sh) ;;
    *) die "CANDIDATE must be in $DIR/candidates/ (or be its run_pipeline_whirlpool.sh)" ;;
esac
cmp -s "$SEED" "$REPO/data/seed_Edge5.txt" || die "the lab's data/seed_Edge5.txt differs from the repository's"

# the lab is the repository plus patches/, nothing else
drift=()
while IFS= read -r -d '' f; do
    rel=${f#"$LAB/"}
    if [ -f "$DIR/patches/$rel" ]; then
        cmp -s "$f" "$DIR/patches/$rel" || drift+=("$rel (differs from patches/$rel)")
    elif [ -f "$REPO/$rel" ]; then
        cmp -s "$f" "$REPO/$rel" || drift+=("$rel")
    else
        drift+=("$rel (not in the repository)")
    fi
done < <(find "$LAB/src" "$LAB/tools" "$LAB/Makefile" -type f ! -name '*.pyc' ! -name '*.o' -print0)
while IFS= read -r -d '' f; do
    rel=${f#"$DIR/patches/"}
    [ -f "$LAB/$rel" ] || drift+=("patches/$rel (not in the lab)")
done < <(find "$DIR/patches" -type f -print0)
if [ "${#drift[@]}" -gt 0 ]; then
    echo "HARNESS refused: the lab does not match the repository plus patches/. Put every change"
    echo "into patches/ (full copies at the same paths) and run setup.sh. Differences:"
    printf '  %s\n' "${drift[@]}"
    exit 1
fi
arch=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("arch") or "generic")' "$DIR/scoreboard.json")
make ARCH="$arch" > runs/make.log 2>&1 || die "make ARCH=$arch failed; see $LAB/runs/make.log"

sha=$(sha1sum "$CAND" | cut -c1-8)
patches=none
if [ -n "$(find "$DIR/patches" -type f -print -quit)" ]; then
    patches=$(cd "$DIR/patches" && find . -type f -print0 | sort -z | xargs -0 sha1sum | sha1sum | cut -c1-8)
fi
name=$(basename "$CAND" .sh)
KEY="$name@$sha+$patches"
standard=no; [ "$CAP" = 180 ] && standard=yes
DB=""
db_ok=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("db_on_disk", False))' "$DIR/scoreboard.json")
[ "$db_ok" = True ] && DB="$LAB/E555.db"
RUNID=r$(date -u +%Y%m%d-%H%M%S)
RUN="$LAB/harness/$RUNID"
mkdir -p "$RUN" "$DIR/whirlpools"
TOTALS="$RUN/totals.json"
echo '{"produced": 0, "attempted": 0, "laps1": 0, "laps2": 0, "laps3": 0, "frame_held": 0, "jewels": 0, "depth_max": 0, "produce_s": 0}' > "$TOTALS"
echo "[harness] run=$RUNID candidate=$KEY minutes=$MINUTES cap=$CAP threads=$THREADS db_file=${DB:-<in memory>} standard=$standard"

# kill_tag TAG -- every process started under E555_HARNESS_TAG=TAG, wherever it went
kill_tag() {
    local sig pids
    for sig in TERM KILL; do
        pids=$(grep -sl --null-data -x "E555_HARNESS_TAG=$1" /proc/[0-9]*/environ 2>/dev/null |
               sed 's#/proc/\([0-9]*\)/environ#\1#' | grep -vx "$$" || true)
        [ -n "$pids" ] || return 0
        kill -"$sig" $pids 2>/dev/null || true
        [ "$sig" = KILL ] || sleep 2
    done
}

# call TAG LIMIT LOG CHAINS_FILE ARGS... -- run the candidate, poll it, kill its tree.
# Sets RC, SECS and KILLED; samples "seconds lines" of CHAINS_FILE once a second.
call() {
    local tag=$1 limit=$2 log=$3 chains=$4 t0 lines mb
    shift 4
    local i=0
    KILLED=""
    E555_HARNESS_TAG=$tag timeout -k 10 "$limit" bash "$CAND" "$@" > "$log" 2>&1 &
    CPID=$!
    t0=$SECONDS
    : > "$log.poll"
    while kill -0 "$CPID" 2> /dev/null; do
        lines=0
        [ -n "$chains" ] && [ -f "$chains" ] && lines=$(wc -l < "$chains")
        echo "$((SECONDS - t0)) $lines" >> "$log.poll"
        i=$((i + 1))
        if [ $((i % 5)) = 0 ] && [ -z "$KILLED" ]; then
            mb=$(du -sm "$CALL_DIR" 2> /dev/null | cut -f1)
            if [ "${mb:-0}" -gt "$DISK_CAP_MB" ]; then KILLED=disk; kill -TERM "$CPID" 2> /dev/null || true; fi
        fi
        sleep 1
    done
    RC=0; wait "$CPID" || RC=$?
    CPID=""
    SECS=$((SECONDS - t0))
    kill_tag "$tag"
    [ -n "$chains" ] && [ -f "$chains" ] && echo "$SECS $(wc -l < "$chains")" >> "$log.poll"
    return 0
}

# SIGTERM or SIGINT stops the run cleanly: the call in flight is killed and
# judged, and the run's totals are written as usual.
STOP=0; CPID=""
trap 'STOP=1; KILLED=stopped; [ -n "$CPID" ] && kill -TERM "$CPID" 2> /dev/null' TERM INT
START=$SECONDS
DEADLINE=$((START + MINUTES * 60))
qpos=0; qlen=0; nprod=0; natt=0; empty=0
QUEUE="$RUN/queue_ok.csv"
while [ "$SECONDS" -lt "$DEADLINE" ] && [ "$STOP" = 0 ]; do
    if [ "$qpos" -ge "$qlen" ]; then
        nprod=$((nprod + 1))
        CALL_DIR="$RUN/p$nprod"; mkdir -p "$CALL_DIR"
        call "$RUNID-p$nprod" "$PRODUCE_CAP" "$RUN/p$nprod.log" "" \
            MODE=produce REPO="$LAB" SEED="$SEED" RUN_DIR="$CALL_DIR" THREADS="$THREADS" \
            RNG_SEED=$(( $(date +%s) % 1000000 * 10 + nprod )) DB_FILE="$DB" \
            TIME_LIMIT="$PRODUCE_CAP" OUT="$CALL_DIR/queue.csv"
        verdict="BOARDS total=0 valid=0 unseen=0 invalid=0 reasons=no_output"
        if [ -s "$CALL_DIR/queue.csv" ]; then
            verdict=$("${JUDGE[@]}" --boards "$CALL_DIR/queue.csv" --seen "$DIR/seen_boards.txt" --good_out "$QUEUE")
        else
            : > "$QUEUE"
        fi
        rm -rf "$CALL_DIR"
        qlen=$(grep -c . "$QUEUE" || true); qpos=0
        python3 - "$TOTALS" "$qlen" "$SECS" <<'EOF'
import json, sys
p, n, s = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
t = json.load(open(p)); t["produced"] += n; t["produce_s"] += s
json.dump(t, open(p, "w"))
EOF
        echo "PRODUCE n=$nprod seconds=$SECS exit=$RC${KILLED:+ killed=$KILLED} queued=$qlen ${verdict#BOARDS }"
        if [ "$qlen" = 0 ]; then
            empty=$((empty + 1))
            if [ "$empty" -ge 2 ]; then echo "[harness] two produce calls in a row queued nothing: the run stops"; break; fi
        else
            empty=0
        fi
        continue
    fi
    qpos=$((qpos + 1)); natt=$((natt + 1))
    A="$RUN/a$natt"; CALL_DIR="$A/run"; mkdir -p "$CALL_DIR"
    sed -n "${qpos}p" "$QUEUE" > "$A/board.csv"
    board_id=$(cut -d, -f1 "$A/board.csv" | tr -d ' ')
    call "$RUNID-a$natt" "$CAP" "$A/log.txt" "$A/chains.csv" \
        MODE=whirl REPO="$LAB" SEED="$SEED" RUN_DIR="$CALL_DIR" THREADS="$THREADS" \
        RNG_SEED=$(( $(date +%s) % 1000000 * 10 + natt % 10 )) TIME_LIMIT="$CAP" \
        INPUT="$A/board.csv" OUT="$A/chains.csv"
    rm -rf "$CALL_DIR"
    tail -n 300 "$A/log.txt" > "$A/log.tmp" && mv "$A/log.tmp" "$A/log.txt"
    exit_kind=ok
    if [ -n "$KILLED" ]; then exit_kind=$KILLED
    elif [ "$RC" = 124 ] || [ "$RC" = 137 ]; then exit_kind=cap
    elif [ "$RC" != 0 ]; then exit_kind=error; fi
    [ -f "$A/chains.csv" ] || : > "$A/chains.csv"
    if ! "${JUDGE[@]}" "$A/chains.csv" --input "$A/board.csv" --frames_seen "$DIR/frames_seen.txt" \
            --whirls_out "$DIR/whirlpools/$RUNID.csv.gz" --id_prefix "$RUNID-a$natt-" \
            --mark_seen "$DIR/seen_boards.txt" > "$A/judge.txt" 2> "$A/judge.err"; then
        echo "[harness] the judge failed on attempt $natt (see $A/judge.err); it counts as nothing"
        echo "JUDGE chains=0 best_laps=0 laps1=0 laps2=0 laps3=0 frame_held=0 jewels=0 new_frames=0 archived=0 first_jewel_line=0" > "$A/judge.txt"
    fi
    python3 - "$TOTALS" "$A" "$DIR" "$RUNID" "$natt" "$KEY" "$board_id" "$SECS" "$exit_kind" <<'EOF'
import csv, json, sys, time
totals, a, d, run, n, key, board, secs, kind = sys.argv[1:]
lines = open(f"{a}/judge.txt").read().split("\n")
chains = [dict(kv.split("=", 1) for kv in l.split()[1:] if "=" in kv) for l in lines if l.startswith("CHAIN ")]
judge = dict(kv.split("=", 1) for kv in next(l for l in lines if l.startswith("JUDGE ")).split()[1:])
poll = [tuple(map(int, l.split())) for l in open(f"{a}/log.txt.poll") if l.strip()]

def when(line):
    """Seconds into the attempt at which the chains file reached `line` lines."""
    return next((t for t, n in poll if n >= line), int(secs))

best = max(chains, key=lambda c: (int(c["laps"]), c["jewel"] == "yes", c["frame"] == "held"),
           default={"laps": "0", "frame": "none"})
now = int(time.time())
new = [c for c in chains if c.get("new") == "yes"]
depth = max((int(c["depth"]) for c in new), default=0)
first = min((when(int(c["line"])) for c in chains if c["jewel"] == "yes"), default="")
archived = [c for c in chains if c.get("archived") == "yes"]
with open(f"{d}/whirlpool_index.csv", "a", newline="") as fh:
    w = csv.writer(fh, lineterminator="\n")
    for c in archived:
        w.writerow([f"{run}-a{n}-{c['tag']}", c["laps"], c["jewel"], c["frame"], c.get("frame_id", ""),
                    c.get("new", ""), c["depth"], c["depths"].replace(",", " "), c["dir"], key, run, n,
                    when(int(c["line"])), now])
with open(f"{d}/attempts.csv", "a", newline="") as fh:
    csv.writer(fh, lineterminator="\n").writerow(
        [now, run, n, key, board, secs, kind, judge["chains"], best["laps"], best["frame"],
         judge["jewels"], len(new), len(archived), first, depth or ""])
t = json.load(open(totals))
t["attempted"] += 1
laps = int(judge["best_laps"])
for k in (1, 2, 3):
    t[f"laps{k}"] += laps >= k
t["frame_held"] += int(judge["frame_held"]) > 0
t["jewels"] += len(new)
t["archived"] = t.get("archived", 0) + len(archived)
t["depth_max"] = max(t["depth_max"], depth)
json.dump(t, open(totals, "w"))
print(f"ATTEMPT n={n} board={board} seconds={secs} exit={kind} chains={judge['chains']} "
      f"laps={best['laps']} frame={best['frame']} jewels={judge['jewels']} new={len(new)} "
      f"archived={len(archived)}"
      + (f" first_jewel_s={first}" if first != "" else "")
      + (f" reason={best['reason']}" if best.get("reason") else ""))
EOF
done

python3 - "$TOTALS" "$DIR" "$RUNID" "$KEY" "$name" "$sha" "$patches" "$((SECONDS - START))" \
          "$standard" "$THREADS" "$NOTE" "$CAP" <<'EOF'
import csv, json, sys, time
totals, d, run, key, name, sha, patches, wall, standard, threads, note, cap = sys.argv[1:]
t = json.load(open(totals))
wall = int(wall)
minutes = wall / 60
per_hour = t["jewels"] / (minutes / 60) if minutes else 0.0
now = int(time.time())
iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))
s = json.load(open(f"{d}/scoreboard.json"))
cpu = s.get("cpu", "")
with open(f"{d}/harness.csv", "a", newline="") as fh:
    csv.writer(fh, lineterminator="\n").writerow(
        [now, iso, run, key, sha, patches, f"{minutes:.1f}", standard, t["produced"], t["attempted"],
         t["laps1"], t["laps2"], t["laps3"], t["frame_held"], t["jewels"], t.get("archived", 0),
         f"{per_hour:.2f}",
         t["depth_max"] or "", cpu, threads, note])
with open(f"{d}/runs.csv", "a", newline="") as fh:
    csv.writer(fh, lineterminator="\n").writerow([now, iso, "harness", f"{run} {key}", wall, 0])
s["harness_runs"] = s.get("harness_runs", 0) + 1
if standard == "yes":
    c = s.setdefault("candidates", {}).setdefault(key, dict(
        file=f"candidates/{name}.sh" if name != "run_pipeline_whirlpool" else "run_pipeline_whirlpool.sh",
        sha=sha, patches=patches, runs=0, minutes=0.0, attempts=0, laps1=0, laps2=0, laps3=0,
        frame_held=0, jewels=0, depth_max=0))
    c["runs"] += 1
    c["minutes"] = round(c["minutes"] + minutes, 1)
    for k in ("laps1", "laps2", "laps3", "frame_held", "jewels"):
        c[k] += t[k]
    c["attempts"] += t["attempted"]
    c["depth_max"] = max(c["depth_max"], t["depth_max"])

    def rank(c):
        n = max(c["attempts"], 1)
        return (c["jewels"] / (c["minutes"] / 60) if c["minutes"] else 0.0,
                c["laps3"] / n, c["laps2"] / n, c["laps1"] / n)
    ready = {k: v for k, v in s["candidates"].items() if v["minutes"] >= 30}
    if ready:
        best = max(ready, key=lambda k: rank(ready[k]))
        if best != s.get("best"):
            s["best"], s["best_unix"] = best, now
json.dump(s, open(f"{d}/scoreboard.json", "w"), indent=1)
print(f"HARNESS run={run} candidate={key} minutes={minutes:.1f} standard={standard} "
      f"produced={t['produced']} attempted={t['attempted']} "
      f"funnel=lap1:{t['laps1']}/lap2:{t['laps2']}/lap3:{t['laps3']} frame_held={t['frame_held']} "
      f"jewels={t['jewels']} archived={t.get('archived', 0)} per_hour={per_hour:.2f} "
      f"depth_max={t['depth_max'] or '-'} "
      f"produce_share={100 * t['produce_s'] / max(wall, 1):.0f}% cap={cap} best={s.get('best') or 'none'}")
EOF
