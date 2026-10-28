#!/bin/bash
# lab.sh -- the explorer kit's one script (agent/explorer/EXPLORER.md). A
# campaign is the folder agent/explorer/<id>/, named by its UTC start time; its
# lab/ (gitignored, never pushed) is a full copy of the toolchain the agent may
# change at will. Four commands:
#
#   bash agent/explorer/lab.sh setup [CAMPAIGN=new|<id>] [RESUME=none|<branch>] [ARCH=auto] [CLUES=center]
#   bash ../../lab.sh run "LABEL" [CAP=s] [THREADS=n] [OUT=glob,glob] -- COMMAND [ARGS...]
#   bash ../../lab.sh status [BUDGET=24h] [CHECKPOINT=30m] [RUN_CAP=3600]
#   bash ../../lab.sh checkpoint [BRANCH=agent_runs] [PARAGRAPH...]
#
# setup (from anywhere; inside a campaign it means that campaign) creates or
# repairs a campaign, and restores a missing one from RESUME, the results
# branch. A fresh lab is a copy of Makefile src tools examples pipeline data
# tests experiments and the two docs, with the campaign's patches/ laid over
# it, built for this CPU (ARCH=auto: x86-64-v3 when it runs here, else
# generic; native on a workstation), plus ref/: the other agents' runners,
# candidates, patches and reports from origin/agent_runs. It writes
# OPTIONS.md, the inventory of every tool option, when it is missing, and the
# machine's facts. CLUES (none, center, all) is fixed when a campaign starts.
#   SETUP campaign=20261029-0900 folder=fresh lab=fresh ref=31 path=... arch=v3 ...
#
# run (from the lab) is how a campaign spends its budget and the only way a
# result counts. It makes the run folder runs/<stamp>_<label>/ and writes it
# wherever {D} appears in COMMAND or OUT ({LAB}: the lab's absolute path, for
# runners that want absolute paths), runs COMMAND from the lab under
# `timeout -s INT -k 60 CAP` (default RUN_CAP), with its output in {D}/run.log,
# kills whatever it left running, notes the largest process's peak memory
# (max_rss_mb), then scores the board files the run wrote with score.py: OUT
# (globs, comma-separated; default every *.csv and *.csv.gz under {D}), but
# only files written during the run. THREADS is what the command uses (default
# all), for the per-CPU-hour yields. It appends one row to runs.csv, archives
# the run's best boards as boards/<run>.csv.gz, and keeps boards/best.csv (best
# VALID board) and boards/deepest.csv (best exact board) and scoreboard.json
# up to date. A command that reads a shipped board file (data/best_*,
# data/board_*, data/E565_*, data/synth_solution_*) is logged, but its boards
# never become the campaign's bests.
#   RUN id=20261029-091502_beam label="beam" runtime_s=301.2 exit=0 capped=no boards=3077 valid=12 best_score=461 exact_boards=2950 best_exact=387 new_best=score dir=runs/...
#
# status (from the campaign) prints the STATUS line. The settings are stored on
# the first call; the clock is the sum of runs.csv's runtime_s since then, so a
# usage-limit wait costs nothing.
#   STATUS best=461/480 bar=465 exact=387/480 runs=12 methods=2 options=40/412 run_time=3h10m/24h00m checkpoint=0h12m stop=none
#
# checkpoint (from the campaign) refreshes patches/ (every lab file under the
# copied folders that differs from the repository's or is new, up to 1 MB) and
# publishes the campaign folder, without lab/, to the results branch through
# agent/publish.sh (refusals: a change outside the folder, a file over 50 MB,
# more than 100 MB of history).
#   CHECKPOINT pushed=1a2b3c4 branch=agent_runs campaign=20261029-0900 best=461 exact=387 patches=3 history_mb=1.2/100
#
# Exit status 0 = done, 1 = failed (checkpoint: the push; run: the command's
# own failure is in its row, not here), 2 = refused or bad usage.
set -uo pipefail

KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$KIT/../.." && pwd)
ID_RE='^[0-9]{8}-[0-9]{4}(-[0-9]+)?$'
COPY="Makefile src tools examples pipeline data tests experiments PROJECT_E555.md README.md"
PATCHED="Makefile src tools examples pipeline tests experiments"
SHIPPED_RE='data/(best_[0-9]|board_|E565_|synth_solution)'

die() { echo "$1 $2"; exit "${3:-2}"; }

# settings NAME=value, checked against a list; the rest are left in REST
parse_settings() {   # "LIST" args...
    local list=" $1 " arg; shift
    REST=()
    for arg in "$@"; do
        if [[ "$arg" =~ ^[A-Z_]+= ]] && [[ "$list" == *" ${arg%%=*} "* ]]; then
            declare -g "$arg"
        else
            REST+=("$arg")
        fi
    done
}

# the campaign the working directory is in: sets CAMPAIGN, DIR, LAB
in_campaign() {   # WHO
    case "$PWD/" in
        "$KIT"/*/*) CAMPAIGN=${PWD#"$KIT/"}; CAMPAIGN=${CAMPAIGN%%/*} ;;
        *) die "$1" "refused: run it inside a campaign, agent/explorer/<id>/ or its lab/" ;;
    esac
    DIR="$KIT/$CAMPAIGN"; LAB="$DIR/lab"
    [ -f "$DIR/scoreboard.json" ] || die "$1" "refused: no campaign in $DIR"
}

# kill_tag TAG -- every process started under E555_RUN_TAG=TAG, wherever it went
# (a background job of a script ignores SIGINT, so timeout alone can miss it)
kill_tag() {
    local sig pids
    for sig in TERM KILL; do
        pids=$(grep -sl --null-data -x "E555_RUN_TAG=$1" /proc/[0-9]*/environ 2>/dev/null |
               sed 's#/proc/\([0-9]*\)/environ#\1#' | grep -vx "$$" || true)
        [ -n "$pids" ] || return 0
        # shellcheck disable=SC2086
        kill -"$sig" $pids 2>/dev/null || true
        [ "$sig" = KILL ] || sleep 3
    done
}

json_get() { python3 -c 'import json,sys; v=json.load(open(sys.argv[1])).get(sys.argv[2]); print("" if v is None else v)' "$DIR/scoreboard.json" "$1"; }

# =============================================================================
cmd_setup() {
    CAMPAIGN=""; RESUME=none; ARCH=auto; CLUES=""
    parse_settings "CAMPAIGN RESUME ARCH CLUES" "$@"
    [ "${#REST[@]}" = 0 ] || die "SETUP" "refused: unknown argument ${REST[0]}; the settings are CAMPAIGN RESUME ARCH CLUES"
    case "$CLUES" in ""|none|center|all) ;; *) die "SETUP" "refused: CLUES must be none, center or all" ;; esac
    fail() { echo "SETUP failed: $*"; exit 1; }
    set -e
    local fetch_log="${TMPDIR:-/tmp}/e555_explorer_fetch.log"
    fetch() {   # BRANCH -> FETCH_HEAD, 5 tries
        local wait
        for wait in 2 4 8 16 0; do
            git -C "$REPO" fetch -q origin "$1" 2>> "$fetch_log" && return 0
            [ "$wait" = 0 ] || sleep "$wait"
        done
        return 1
    }

    # 0. which campaign
    if [ -z "$CAMPAIGN" ]; then
        case "$PWD/" in
            "$KIT"/*/*) CAMPAIGN=${PWD#"$KIT/"}; CAMPAIGN=${CAMPAIGN%%/*} ;;
            *) CAMPAIGN=new ;;
        esac
    fi
    if [ "$CAMPAIGN" = new ]; then
        if [ "$RESUME" != none ]; then
            fetch "$RESUME" || fail "git fetch origin $RESUME; see $fetch_log"
            local ids
            ids=$(git -C "$REPO" ls-tree -d --name-only FETCH_HEAD agent/explorer/ | sed 's#.*/##' | grep -E "$ID_RE" || true)
            [ -n "$ids" ] || fail "branch $RESUME holds no explorer campaign"
            [ "$(wc -l <<< "$ids")" = 1 ] || fail "branch $RESUME holds several campaigns; pass CAMPAIGN= one of:" $ids
            CAMPAIGN=$ids
        else
            CAMPAIGN=$(date -u +%Y%m%d-%H%M)
            local n=1
            while [ -e "$KIT/$CAMPAIGN" ]; do n=$((n + 1)); CAMPAIGN=$(date -u +%Y%m%d-%H%M)-$n; done
        fi
    fi
    [[ "$CAMPAIGN" =~ $ID_RE ]] || fail "bad campaign id '$CAMPAIGN' (expected like 20261029-0900)"
    DIR="$KIT/$CAMPAIGN"; LAB="$DIR/lab"

    # 1. the campaign folder
    local folder=kept
    if [ ! -f "$DIR/scoreboard.json" ]; then
        if [ "$RESUME" != none ]; then
            fetch "$RESUME" || fail "git fetch origin $RESUME; see $fetch_log"
            git -C "$REPO" archive FETCH_HEAD "agent/explorer/$CAMPAIGN" 2>> "$fetch_log" |
                tar -x -C "$REPO" || fail "branch $RESUME has no agent/explorer/$CAMPAIGN/"
            [ -f "$DIR/scoreboard.json" ] || fail "the checkpoint on $RESUME has no scoreboard.json"
            folder=restored
        else
            mkdir -p "$DIR"
            python3 - "$DIR/scoreboard.json" "$CAMPAIGN" "${CLUES:-center}" <<'EOF'
import json, sys
path, campaign, clues = sys.argv[1:]
json.dump({"kit": "explorer", "campaign": campaign, "clues": clues, "bar": 465,
           "settings": {"BUDGET": "24h", "CHECKPOINT": "30m", "RUN_CAP": "3600"},
           "goal_start_unix": None, "last_checkpoint_unix": None, "results_branch": None,
           "wakeup_trigger_id": None, "runs": 0,
           "best_score": 0, "best_run": None, "best_exact": 0, "best_exact_placed": 0,
           "best_exact_run": None, "history": []}, open(path, "w"), indent=1)
EOF
            printf 'unix,iso,run,label,cap,threads,runtime_s,exit,capped,cpu_h,max_rss_mb,shipped,files,boards,malformed,valid,best_score,exact_boards,best_exact,best_exact_placed,hist,new_best,command\n' > "$DIR/runs.csv"
            printf '# Journal, campaign %s\n\nOne block per experiment: hypothesis, command (lab.sh run id), result, decision.\n' "$CAMPAIGN" > "$DIR/JOURNAL.md"
            printf '# Methods, campaign %s\n\n(The deliverable: agent/explorer/EXPLORER.md, "METHODS.md".)\n' "$CAMPAIGN" > "$DIR/METHODS.md"
            folder=fresh
        fi
    fi
    mkdir -p "$DIR/methods" "$DIR/patches" "$DIR/boards"
    local clues
    clues=$(json_get clues)
    [ -z "$CLUES" ] || [ "$CLUES" = "$clues" ] ||
        echo "note: CLUES=$CLUES ignored, campaign $CAMPAIGN was started with CLUES=$clues"

    # 2. the lab: the toolchain, then the campaign's patches over it
    local lab=kept
    mkdir -p "$LAB"
    if [ ! -f "$LAB/Makefile" ]; then
        local paths=() p
        for p in $COPY; do [ -e "$REPO/$p" ] && paths+=("$p"); done
        tar -C "$REPO" --exclude=tests/out --exclude=__pycache__ --exclude='*.o' --exclude='*.db' \
            --exclude='experiments/*/bin' -cf - "${paths[@]}" | tar -C "$LAB" -xf -
        (cd "$DIR/patches" && find . -type f -print0) | while IFS= read -r -d '' f; do
            mkdir -p "$LAB/$(dirname "$f")"; cp -p "$DIR/patches/$f" "$LAB/$f"
        done
        lab=fresh
    fi
    mkdir -p "$LAB/runs"

    # 3. ref/: the other agents' work, read-only (never the big archives)
    local ref=kept
    if [ ! -d "$LAB/ref" ]; then
        ref=none
        if fetch agent_runs; then
            local list
            list=$(git -C "$REPO" ls-tree -r --name-only FETCH_HEAD agent/ |
                   grep -E '(\.md|\.sh|\.py|\.c|\.h)$|/(patches|candidates|methods)/' |
                   grep -vE '/(whirlpools|boards|lab)/|\.gz$' || true)
            if [ -n "$list" ]; then
                mkdir -p "$LAB/ref"
                # shellcheck disable=SC2086
                git -C "$REPO" archive FETCH_HEAD $list | tar -x -C "$LAB/ref" && ref=fresh
            fi
        fi
    fi
    local nref=0
    [ -d "$LAB/ref" ] && nref=$(find "$LAB/ref" -type f | wc -l)

    # 4. the build: x86-64-v3 where it runs, generic after a SIGILL
    cd "$LAB"
    if [ "$ARCH" = auto ]; then
        ARCH=generic
        if /lib64/ld-linux-x86-64.so.2 --help 2>/dev/null | grep -q 'x86-64-v3 (supported'; then ARCH=v3; fi
    fi
    make ARCH="$ARCH" > runs/make.log 2>&1 || fail "make ARCH=$ARCH; see $LAB/runs/make.log"
    local rc=0
    bin/E555_diver < /dev/null > /dev/null 2>&1 || rc=$?
    if [ "$rc" = 132 ] && [ "$ARCH" != generic ]; then
        ARCH=generic
        make ARCH=generic > runs/make.log 2>&1 || fail "make ARCH=generic; see $LAB/runs/make.log"
    fi
    python3 -c 'import ortools' 2> /dev/null || python3 -m pip install -q ortools > runs/pip.log 2>&1 || true
    local ortools=no
    python3 -c 'import ortools' 2> /dev/null && ortools=yes

    # 5. OPTIONS.md: every option of every tool, from the lab's sources
    if [ ! -f "$DIR/OPTIONS.md" ]; then
        python3 - "$LAB" "$DIR/OPTIONS.md" <<'EOF'
import re, sys
from pathlib import Path
lab, out = Path(sys.argv[1]), Path(sys.argv[2])
c_flag = re.compile(r'strn?cmp\s*\([^,]+,\s*"(--[A-Za-z0-9_-]+)"')     # as tests/check_script_flags.py
py_flag = re.compile(r'["\'](--[A-Za-z][A-Za-z0-9_-]*)["\']')
lines = ["# Options ledger", "",
         "Every option of every tool, generated from the lab's sources when the campaign",
         "started. Keep it current: status is untested, tested, promising or dead, with",
         "the run ids and one line of what was learned. Add the options of new tools,",
         "and settings of runners worth sweeping, as you meet them.", ""]
total = 0
for f in sorted(list(lab.glob("src/**/*.c")) + list(lab.glob("src/**/*.py")) + list(lab.glob("tools/*.py"))
                + list(lab.glob("experiments/**/*.c"))):
    text = f.read_text(errors="replace")
    if f.suffix == ".c":
        flags = set(c_flag.findall(text))
    else:
        flags = {m for line in text.splitlines() if "add_argument(" in line for m in py_flag.findall(line)}
    flags.discard("--help")
    if not flags:
        continue
    name = str(f.relative_to(lab))
    if f.name == "E555_database.c":
        name += " (shared by the beamer, finalizer and roundhouse)"
    lines += [f"## {name}", "", "| option | status | runs | notes |", "|---|---|---|---|"]
    lines += [f"| `{x}` | untested | | |" for x in sorted(flags)]
    lines.append("")
    total += len(flags)
out.write_text("\n".join(lines) + "\n")
print(total)
EOF
    fi > /dev/null

    # 6. this machine
    local threads cpu ram_gb disk_gb stage_b_ok=false
    threads=$(nproc)
    cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo); cpu=${cpu:-unknown}
    ram_gb=$(awk '/^MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)
    disk_gb=$(df -Pk . | awk 'NR == 2 {printf "%d", $4 / 1048576}')
    [ "$ram_gb" -ge 8 ] && stage_b_ok=true
    local npatch nmeth
    npatch=$(find "$DIR/patches" -type f | wc -l)
    nmeth=$(find "$DIR/methods" -maxdepth 1 -name '*.sh' | wc -l)
    python3 - "$DIR/scoreboard.json" "$cpu" "$ARCH" "$threads" "$ram_gb" "$disk_gb" "$ortools" "$stage_b_ok" \
             "$folder" "$lab" "$nref" "$LAB" "$npatch" "$nmeth" <<'EOF'
import json, sys
path, cpu, arch, threads, ram, disk, ortools, stage_b, folder, lab, ref, labpath, npatch, nmeth = sys.argv[1:]
s = json.load(open(path))
changed = "yes" if s.get("cpu") not in (None, cpu) else "no"
seen = s.setdefault("cpus_seen", [])
if cpu not in seen:
    seen.append(cpu)
s.update(cpu=cpu, arch=arch, threads=int(threads), ram_gb=int(ram), disk_gb=int(disk),
         ortools_ok=ortools == "yes", stage_b_ok=stage_b == "true")
json.dump(s, open(path, "w"), indent=1)
print(f"SETUP campaign={s['campaign']} folder={folder} lab={lab} ref={ref} path={labpath} arch={arch} "
      f"cpu=\"{cpu}\" cpu_changed={changed} threads={threads} ram_gb={ram} disk_gb={disk} "
      f"ortools={ortools} stage_b_ok={stage_b} clues={s['clues']} patches={npatch} methods={nmeth} "
      f"best={s['best_score']} exact={s['best_exact']}")
EOF
}

# =============================================================================
cmd_run() {
    in_campaign RUN
    [ "$PWD" = "$LAB" ] || die RUN "refused: run it from the lab, $LAB"
    [ $# -ge 1 ] || die RUN "refused: usage: lab.sh run \"LABEL\" [CAP=s] [THREADS=n] [OUT=glob,...] -- COMMAND..."
    local label=$1; shift
    CAP=""; THREADS=""; OUT=""
    local pre=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do pre+=("$1"); shift; done
    [ "${1:-}" = "--" ] && [ $# -ge 2 ] || die RUN "refused: no COMMAND after --"
    shift
    parse_settings "CAP THREADS OUT" "${pre[@]+"${pre[@]}"}"
    [ "${#REST[@]}" = 0 ] || die RUN "refused: unknown setting ${REST[0]}; the settings are CAP THREADS OUT"
    local clues cap threads
    clues=$(json_get clues)
    cap=${CAP:-$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["settings"].get("RUN_CAP","3600"))' "$DIR/scoreboard.json")}
    threads=${THREADS:-$(nproc)}
    [[ "$cap" =~ ^[0-9]+$ ]] && [[ "$threads" =~ ^[0-9]+$ ]] || die RUN "refused: CAP and THREADS are whole numbers"

    local slug stamp id D
    slug=$(tr -c 'A-Za-z0-9._-' '_' <<< "$label" | cut -c1-40 | sed 's/_*$//')
    stamp=$(date -u +%Y%m%d-%H%M%S)
    id="${stamp}_${slug}"
    D="runs/$id"
    mkdir -p "$D"
    local cmd=("${@//\{D\}/$D}")
    cmd=("${cmd[@]//\{LAB\}/$LAB}")
    local shipped=no
    [[ "${cmd[*]}" =~ $SHIPPED_RE ]] && shipped=yes
    printf '%q ' "${cmd[@]}" > "$D/command.txt"; echo >> "$D/command.txt"
    touch "$D/.start"

    local start end rc=0
    start=$(date +%s.%N)
    # the wrapper waits through the SIGINT and notes the largest process's peak memory
    E555_RUN_TAG=$id timeout -s INT -k 60 "$cap" python3 -c '
import resource, subprocess, sys
try:
    p = subprocess.Popen(sys.argv[2:])
except OSError as exc:
    sys.exit(f"lab.sh run: {exc}")
while True:
    try:
        rc = p.wait()
        break
    except KeyboardInterrupt:
        pass
open(sys.argv[1], "w").write(str(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss // 1024))
sys.exit(rc if rc >= 0 else 128 - rc)' "$D/.rss" "${cmd[@]}" > "$D/run.log" 2>&1 < /dev/null || rc=$?
    end=$(date +%s.%N)
    kill_tag "$id"
    local runtime capped=no
    runtime=$(awk -v a="$start" -v b="$end" 'BEGIN {printf "%.1f", b - a}')
    local rss=-
    [ -s "$D/.rss" ] && rss=$(cat "$D/.rss")
    [ "$rc" = 124 ] || [ "$rc" = 137 ] && capped=yes

    # the board files the run wrote
    local files=() g f
    if [ -n "$OUT" ]; then
        OUT=${OUT//\{D\}/$D}
        IFS=',' read -r -a globs <<< "${OUT//\{LAB\}/$LAB}"
        for g in "${globs[@]}"; do
            for f in $g; do [ -f "$f" ] && [ "$f" -nt "$D/.start" ] && files+=("$f"); done
        done
    else
        while IFS= read -r -d '' f; do files+=("$f"); done < <(
            find "$D" -type f \( -name '*.csv' -o -name '*.csv.gz' \) -newer "$D/.start" -print0 | sort -z)
    fi
    local scoreline="SCORE files=0 boards=0 malformed=0 valid=0 best_score=0 exact_boards=0 best_exact=0 best_exact_placed=0 hist=- clue=$clues solved=no"
    if [ "${#files[@]}" -gt 0 ]; then
        scoreline=$(python3 "$KIT/score.py" "${files[@]}" --clues "$clues" --top 50 \
                    --out "$D/top.csv.gz" --best_out "$D/.best.csv" --deepest_out "$D/.deepest.csv" 2> "$D/score.err") ||
            scoreline="SCORE files=${#files[@]} boards=0 malformed=0 valid=0 best_score=0 exact_boards=0 best_exact=0 best_exact_placed=0 hist=- clue=$clues solved=no error=see_$D/score.err"
    fi

    # bookkeeping, one run at a time
    (
        flock 9
        python3 - "$DIR" "$id" "$label" "$cap" "$threads" "$runtime" "$rc" "$capped" "$shipped" \
                  "${#files[@]}" "$scoreline" "$D" "$(cat "$D/command.txt")" "$rss" <<'EOF'
import csv, json, os, shutil, sys, time
d, rid, label, cap, threads, runtime, rc, capped, shipped, nfiles, scoreline, rundir, command, rss = sys.argv[1:]
sc = dict(kv.split("=", 1) for kv in scoreline.split()[1:] if "=" in kv)
board = json.load(open(f"{d}/scoreboard.json"))
best, exact = int(sc.get("best_score", 0)), int(sc.get("best_exact", 0))
placed = int(sc.get("best_exact_placed", 0))
new = []
now = int(time.time())
top = f"{rundir}/top.csv.gz"
if os.path.exists(top) and os.path.getsize(top) > 0:
    shutil.copy(top, f"{d}/boards/{rid}.csv.gz")
if shipped == "no":
    if best > board["best_score"] and os.path.exists(f"{rundir}/.best.csv"):
        shutil.copy(f"{rundir}/.best.csv", f"{d}/boards/best.csv")
        board.update(best_score=best, best_run=rid)
        board["history"].append({"unix": now, "run": rid, "what": "score", "value": best})
        new.append("score")
    if (exact, placed) > (board["best_exact"], board["best_exact_placed"]) and os.path.exists(f"{rundir}/.deepest.csv"):
        shutil.copy(f"{rundir}/.deepest.csv", f"{d}/boards/deepest.csv")
        board.update(best_exact=exact, best_exact_placed=placed, best_exact_run=rid)
        board["history"].append({"unix": now, "run": rid, "what": "exact", "value": exact, "placed": placed})
        new.append("exact")
board["runs"] = board.get("runs", 0) + 1
json.dump(board, open(f"{d}/scoreboard.json", "w"), indent=1)
cpu_h = float(runtime) * int(threads) / 3600
with open(f"{d}/runs.csv", "a", newline="") as fh:
    csv.writer(fh, lineterminator="\n").writerow(
        [now, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)), rid, label, cap, threads, runtime,
         rc, capped, f"{cpu_h:.3f}", rss, shipped, nfiles, sc.get("boards", 0), sc.get("malformed", 0),
         sc.get("valid", 0), best, sc.get("exact_boards", 0), exact, placed, sc.get("hist", "-"),
         "+".join(new) or "none", command.strip()])
print(f"RUN id={rid} label=\"{label}\" runtime_s={runtime} exit={rc} capped={capped} cpu_h={cpu_h:.2f} max_rss_mb={rss} "
      f"files={nfiles} boards={sc.get('boards', 0)} valid={sc.get('valid', 0)} best_score={best} "
      f"exact_boards={sc.get('exact_boards', 0)} best_exact={exact} best_exact_placed={placed} "
      f"hist={sc.get('hist', '-')} new_best={'+'.join(new) or 'none'}"
      + (" shipped=yes(not_counted)" if shipped == "yes" else "")
      + (" score_error=yes" if "error=" in scoreline else "") + f" dir={rundir}")
EOF
    ) 9> "$LAB/.lock"
    rm -f "$D/.best.csv" "$D/.deepest.csv" "$D/.rss"
    echo "--- last lines of $D/run.log:"
    tail -n 5 "$D/run.log" | cut -c1-200
    echo "disk_free_gb=$(df -Pk . | awk 'NR == 2 {printf "%d", $4 / 1048576}')"
    return 0
}

# =============================================================================
cmd_status() {
    in_campaign STATUS
    BUDGET=""; CHECKPOINT=""; RUN_CAP=""
    parse_settings "BUDGET CHECKPOINT RUN_CAP" "$@"
    [ "${#REST[@]}" = 0 ] || die STATUS "refused: unknown setting ${REST[0]}; the settings are BUDGET CHECKPOINT RUN_CAP"
    python3 - "$DIR" "$BUDGET" "$CHECKPOINT" "$RUN_CAP" <<'EOF'
import csv, json, re, sys, time
from pathlib import Path
d, budget_s, checkpoint_s, run_cap = sys.argv[1:]
def seconds(text, name):
    m = re.fullmatch(r"(?:(\d+)h)?(?:(\d+)m)?", text.strip())
    if not text.strip() or not m:
        sys.exit(f"STATUS refused: {name}={text} is not a duration like 24h, 90m or 1h30m")
    return int(m.group(1) or 0) * 3600 + int(m.group(2) or 0) * 60
def hm(s):
    s = max(0, int(s)); return f"{s // 3600}h{s % 3600 // 60:02d}m"
now = int(time.time())
board = json.load(open(f"{d}/scoreboard.json"))
st = board["settings"]
for name, value in (("BUDGET", budget_s), ("CHECKPOINT", checkpoint_s), ("RUN_CAP", run_cap)):
    if value:
        st[name] = value
if not str(st["RUN_CAP"]).isdigit():
    sys.exit("STATUS refused: RUN_CAP is a number of seconds")
budget = seconds(st["BUDGET"], "BUDGET")
if board.get("goal_start_unix") is None:
    board["goal_start_unix"] = now
json.dump(board, open(f"{d}/scoreboard.json", "w"), indent=1)
run_time = 0.0
with open(f"{d}/runs.csv", newline="") as fh:
    for row in csv.DictReader(fh):
        try:
            if int(row["unix"]) >= board["goal_start_unix"]:
                run_time += float(row["runtime_s"])
        except (KeyError, TypeError, ValueError):
            pass
opts = Path(f"{d}/OPTIONS.md").read_text().splitlines() if Path(f"{d}/OPTIONS.md").exists() else []
rows = [l for l in opts if l.startswith("| `")]
tried = sum(1 for l in rows if "| untested |" not in l)
methods = len(list(Path(f"{d}/methods").glob("*.sh")))
best = board["best_score"]
stop = "480" if best >= 480 else "budget" if run_time >= budget else "none"
fields = [f"best={best}/480", f"bar={board.get('bar', 465)}", f"exact={board['best_exact']}/480",
          f"runs={board.get('runs', 0)}", f"methods={methods}", f"options={tried}/{len(rows)}",
          f"run_time={hm(run_time)}/{hm(budget)}"]
if st["CHECKPOINT"] != "off":
    every = seconds(st["CHECKPOINT"], "CHECKPOINT")
    done = board.get("last_checkpoint_unix")
    age = now - done if done else None
    fields.append("checkpoint=" + ("never" if age is None else hm(age)) + ("(due)" if age is None or age >= every else ""))
print("STATUS " + " ".join(fields) + f" stop={stop}")
EOF
}

# =============================================================================
cmd_checkpoint() {
    in_campaign CHECKPOINT
    local branch="" paragraphs=() arg
    for arg in "$@"; do
        case "$arg" in BRANCH=*) branch=${arg#BRANCH=} ;; *) paragraphs+=("$arg") ;; esac
    done
    [ -f "$LAB/Makefile" ] || die CHECKPOINT "refused: the lab is missing; run lab.sh setup first (it would lose patches/)"
    branch=$(python3 - "$DIR/scoreboard.json" "$branch" <<'EOF'
import json, sys
path, branch = sys.argv[1:]
s = json.load(open(path))
s["results_branch"] = branch or s.get("results_branch") or "agent_runs"
json.dump(s, open(path, "w"), indent=1)
print(s["results_branch"])
EOF
)
    # patches/: the lab's changed and new source files, rebuilt from the lab
    local tmp skipped=""
    tmp=$(mktemp -d)
    local p f rel
    for p in $PATCHED; do
        [ -e "$LAB/$p" ] || continue
        while IFS= read -r -d '' f; do
            rel=${f#"$LAB/"}
            if [ -f "$REPO/$rel" ] && cmp -s "$f" "$REPO/$rel"; then continue; fi
            if [ "$(stat -c %s "$f")" -gt 1048576 ]; then skipped+=" $rel"; continue; fi
            mkdir -p "$tmp/$(dirname "$rel")"; cp -p "$f" "$tmp/$rel"
        done < <(find "$LAB/$p" \( -name bin -o -name __pycache__ -o -path "$LAB/tests/out" \) -prune -o \
                      -type f ! -name '*.o' ! -name '*.db*' ! -name '*.pyc' ! -name '.buildflags' -print0)
    done
    rm -rf "$DIR/patches"; mv "$tmp" "$DIR/patches"; chmod 755 "$DIR/patches"
    [ -z "$skipped" ] || echo "note: not in patches/ (over 1 MB):$skipped"
    local npatch
    npatch=$(find "$DIR/patches" -type f | wc -l)

    local rel_dir="agent/explorer/$CAMPAIGN" outside
    outside=$(git -C "$REPO" status --porcelain --untracked-files=all | cut -c4- | grep -v "^$rel_dir/" || true)
    if [ -n "$outside" ]; then
        echo "CHECKPOINT refused: changes outside $rel_dir/ (restore them, e.g. git checkout -- PATH, or delete them):"
        sed 's/^/  /' <<< "$outside"
        exit 2
    fi
    local best exact rc=0 out
    best=$(json_get best_score); exact=$(json_get best_exact)
    out=$(bash "$REPO/agent/publish.sh" BRANCH="$branch" SRC="$DIR" DEST="$rel_dir" EXCLUDE=lab \
              MESSAGE="Agent: explorer campaign $CAMPAIGN checkpoint, best $best, exact $exact" \
              "${paragraphs[@]+"${paragraphs[@]}"}" 2>&1) || rc=$?
    case "$rc" in
        0)  python3 - "$DIR/scoreboard.json" <<'EOF'
import json, sys, time
s = json.load(open(sys.argv[1]))
s["last_checkpoint_unix"] = int(time.time())
json.dump(s, open(sys.argv[1], "w"), indent=1)
EOF
            local sha mb
            sha=$(sed -n 's/^PUBLISH [a-z]*=\([0-9a-f]*\).*/\1/p' <<< "$out")
            mb=$(sed -n 's/.* history_mb=\([^ ]*\).*/\1/p' <<< "$out")
            echo "CHECKPOINT pushed=$sha branch=$branch campaign=$CAMPAIGN best=$best exact=$exact patches=$npatch history_mb=${mb:-unchanged}" ;;
        1)  echo "CHECKPOINT failed: the push to $branch failed; the next checkpoint pushes it too"
            sed 's/^/  /' <<< "$out" ;;
        *)  echo "CHECKPOINT refused:"
            sed 's/^/  /' <<< "$out" ;;
    esac
    exit "$rc"
}

# =============================================================================
case "${1:-}" in
    setup)      shift; cmd_setup "$@" ;;
    run)        shift; cmd_run "$@" ;;
    status)     shift; cmd_status "$@" ;;
    checkpoint) shift; cmd_checkpoint "$@" ;;
    *) echo "usage: lab.sh setup|run|status|checkpoint ... (see the header of $KIT/lab.sh)" >&2; exit 2 ;;
esac
