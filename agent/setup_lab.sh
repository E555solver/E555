#!/bin/bash
# setup_lab.sh -- start, repair or restore a campaign's lab, the working copy of
# the /goal kit (agent/autonomous_agent_mode.md, sections 4 and 11). Every
# campaign gets a lab of its own, agent/labs/<id>/, named by the UTC date and
# time it started (20261027-1930), and its checkpoints go to
# agent/checkpoints/<id>/, so campaigns never write into each other's results.
# Safe to run at any time: each step does only what is missing, so a healthy
# lab costs a second, and it never touches a campaign's results.
#
#   bash agent/setup_lab.sh                          # a new campaign
#   bash ../../setup_lab.sh                          # inside a lab: that campaign's health check
#   bash agent/setup_lab.sh CAMPAIGN=20261027-1930   # the same, from anywhere
#   bash agent/setup_lab.sh RESUME=my-branch         # continue the campaign checkpointed on my-branch
#   SETUP campaign=20261027-1930 lab=fresh path=/.../agent/labs/20261027-1930 arch=v3 ...
#
# 1. Copies the toolchain into the lab when it is not there: Makefile, src,
#    tools, examples, pipeline, data, the two docs, and the kit as KIT.md.
# 2. Builds the lab's bin/. ARCH=auto picks v3 (AVX2) where the CPU runs
#    x86-64-v3, else generic. Measured on the reopen: v3 is as fast as native
#    and generic 45% slower, all three with identical output; a native build
#    dies with SIGILL when the session moves to an older CPU, a v3 build does
#    not on any current server. A changed ARCH rebuilds (bin/.buildflags), and a
#    binary that still dies with SIGILL forces generic.
# 3. Installs OR-Tools when Python lacks it (the ender and the topper need it).
# 4. Without a scoreboard.json: restores the campaign from its
#    agent/checkpoints/<id>/ on branch RESUME (written by agent/checkpoint.sh),
#    or starts a fresh one from data/best_465.csv. FRESH_CLOCK=1 gives a
#    restored campaign a new budget.
# 5. Writes this machine's facts into scoreboard.json (cpu, arch, threads,
#    ram_gb, disk_gb, ortools_ok, stage_b_ok, db_on_disk) and checks the
#    frontier with the referee. stage_b_ok needs 8 GB of free RAM for the
#    beamer's chain database; db_on_disk means the lab can keep that database
#    as E555.db (already there, or 8 GB of free disk). The file is never
#    committed: *.db is gitignored, and checkpoint.sh never saves it.
# Exit status 0 = ready, 1 = a step failed, 2 = usage.
# ---- settings: pass NAME=value on the command line ---------------------------
CAMPAIGN=           # empty: the lab this runs in, else a new campaign;
                    # new: a new campaign; or an id. With RESUME, empty or new
                    # means the one campaign checkpointed on that branch
ARCH=auto           # auto, v3, v2, generic or native (the Makefile's ARCH)
RESUME=none         # a git branch whose agent/checkpoints/<id>/ restores a lost campaign
FRESH_CLOCK=0       # 1: a restored campaign starts its BUDGET again
# -----------------------------------------------------------------------------
SETTINGS=" CAMPAIGN ARCH RESUME FRESH_CLOCK "
for arg in "$@"; do
    case "$arg" in
        [A-Za-z_]*=*) [[ "$SETTINGS" == *" ${arg%%=*} "* ]] ||
                          { echo "unknown setting ${arg%%=*}; the settings are:$SETTINGS" >&2; exit 2; }
                      declare "$arg" ;;
        *) echo "expected NAME=value, got: $arg" >&2; exit 2 ;;
    esac
done
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LABS="$REPO/agent/labs"
SEED="$REPO/data/seed_Edge5.txt"
fail() { echo "SETUP failed: $*"; exit 1; }

fetched=0
fetch_resume() {
    [ "$fetched" = 1 ] && return 0
    local wait
    mkdir -p "$LABS"
    for wait in 2 4 8 16 0; do
        if git -C "$REPO" fetch -q origin "$RESUME" 2>> "$LABS/git_fetch.log"; then
            fetched=1; return 0
        fi
        [ "$wait" = 0 ] || sleep "$wait"
    done
    fail "git fetch origin $RESUME; see $LABS/git_fetch.log"
}

# 0. which campaign
if [ -z "$CAMPAIGN" ]; then
    case "$PWD/" in
        "$LABS"/*/*) CAMPAIGN=${PWD#"$LABS/"}; CAMPAIGN=${CAMPAIGN%%/*} ;;
        *) CAMPAIGN=new ;;
    esac
fi
if [ "$CAMPAIGN" = new ]; then
    if [ "$RESUME" != none ]; then
        fetch_resume
        ids=$(git -C "$REPO" ls-tree --name-only FETCH_HEAD agent/checkpoints/ | sed 's#.*/##')
        [ -n "$ids" ] || fail "branch $RESUME has no agent/checkpoints/"
        [ "$(wc -l <<< "$ids")" = 1 ] ||
            fail "branch $RESUME holds several campaigns; pass CAMPAIGN= one of:" $ids
        CAMPAIGN=$ids
    else
        CAMPAIGN=$(date -u +%Y%m%d-%H%M)
        n=1
        while [ -e "$LABS/$CAMPAIGN" ]; do
            n=$((n + 1)); CAMPAIGN=$(date -u +%Y%m%d-%H%M)-$n
        done
    fi
fi
[[ "$CAMPAIGN" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || fail "bad campaign id '$CAMPAIGN'"
LAB="$LABS/$CAMPAIGN"
mkdir -p "$LAB"
cd "$LAB"

# 1. the toolchain
state=kept
if [ ! -f Makefile ]; then
    cp -r "$REPO"/{Makefile,src,tools,examples,pipeline,data,PROJECT_E555.md,README.md} .
    state=fresh
fi
[ -f KIT.md ] || cp "$REPO/agent/autonomous_agent_mode.md" KIT.md
mkdir -p runs frontier pool

# 2. the binaries
if [ "$ARCH" = auto ]; then
    ARCH=generic
    if /lib64/ld-linux-x86-64.so.2 --help 2>/dev/null | grep -q 'x86-64-v3 (supported'; then
        ARCH=v3
    fi
fi
make ARCH="$ARCH" > runs/make.log 2>&1 || fail "make ARCH=$ARCH; see $LAB/runs/make.log"
rc=0
bin/E555_diver < /dev/null > /dev/null 2>&1 || rc=$?
if [ "$rc" = 132 ] && [ "$ARCH" != generic ]; then
    ARCH=generic
    make ARCH=generic > runs/make.log 2>&1 || fail "make ARCH=generic; see $LAB/runs/make.log"
fi

# 3. OR-Tools
if ! python3 -c 'import ortools' 2> /dev/null; then
    python3 -m pip install -q ortools > runs/pip.log 2>&1 || true
fi
ortools=no
if python3 -c 'import ortools' 2> /dev/null; then ortools=yes; fi

# 4. the campaign's state
if [ ! -f scoreboard.json ]; then
    if [ "$RESUME" != none ]; then
        fetch_resume
        TMP=$(mktemp -d)
        trap 'rm -rf "$TMP"' EXIT
        git -C "$REPO" archive FETCH_HEAD "agent/checkpoints/$CAMPAIGN" 2>> runs/git.log | tar -x -C "$TMP" ||
            fail "branch $RESUME has no agent/checkpoints/$CAMPAIGN/"
        cp -r "$TMP/agent/checkpoints/$CAMPAIGN/." .
        [ -f scoreboard.json ] || fail "the checkpoint on $RESUME has no scoreboard.json"
        if [ "$FRESH_CLOCK" = 1 ]; then
            python3 - <<'EOF'
import json
s = json.load(open("scoreboard.json"))
s.update(goal_start_unix=None, last_best_unix=None, families_since_best=[])
json.dump(s, open("scoreboard.json", "w"), indent=1)
EOF
        fi
        state=restored
    else
        cp data/best_465.csv frontier/frontier.csv
        python3 "$REPO/tools/E555_rank.py" data/best_46[345].csv --seed_file "$SEED" \
            --out frontier/elite.csv --rescore --quiet
        cat > scoreboard.json <<'JSON'
{ "puzzle": "seed_Edge5", "perfect_score": 480,
  "baseline_score": 465, "best_score": 465, "best_breaks": 15,
  "best_source": "data/best_465.csv (shipped baseline)",
  "best_file": "frontier/frontier.csv",
  "total_runs": 0, "goal_start_unix": null, "last_best_unix": null,
  "last_checkpoint_unix": null, "families_since_best": [], "history": [] }
JSON
        printf 'unix,iso,family,tool,params,input,output,score,breaks,verdict,runtime_s,note\n' > run_log.csv
        : > JOURNAL.md
        cat > STRATEGY.md <<'EOF'
# Strategy ledger

Measured before this campaign (PROJECT_E555.md 9.2, 9.3); re-test only with a new idea:
- CP-SAT windows around the breaks of polished boards (457-463): already optimal
  (78/78 4x4 windows); exact regions of 48-102 cells gained nothing.
- diver --reopen auto on the seven first 463s of data/best_463.csv, 120 s each:
  the first gained 2 (that board is also the root of data/best_465.csv), the
  other six gained nothing.
- Best measured reopen setting: --copies 8 --end_dive 1000 --end_polish 10000;
  it kept gaining from 15 s to 120 s per board.

## Families tried in this campaign
EOF
        state=fresh
    fi
fi

# 5. this machine, and the frontier
threads=$(nproc)
cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
cpu=${cpu:-unknown}
ram_gb=$(awk '/^MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)
disk_gb=$(df -Pk . | awk 'NR == 2 {printf "%d", $4 / 1048576}')
stage_b_ok=false
if [ "$ram_gb" -ge 8 ]; then stage_b_ok=true; fi
db_on_disk=false
if [ -s E555.db ] || [ "$disk_gb" -ge 8 ]; then db_on_disk=true; fi
read -r cpu_changed best < <(python3 - "$CAMPAIGN" "$cpu" "$ARCH" "$threads" "$ram_gb" "$disk_gb" "$ortools" "$stage_b_ok" "$db_on_disk" <<'EOF'
import json, sys
campaign, cpu, arch, threads, ram, disk, ortools, stage_b, db_on_disk = sys.argv[1:]
s = json.load(open("scoreboard.json"))
changed = "yes" if s.get("cpu") not in (None, cpu) else "no"
s.update(campaign=campaign, cpu=cpu, arch=arch, threads=int(threads), ram_gb=int(ram),
         disk_gb=int(disk), ortools_ok=ortools == "yes", stage_b_ok=stage_b == "true",
         db_on_disk=db_on_disk == "true")
json.dump(s, open("scoreboard.json", "w"), indent=1)
print(changed, s["best_score"])
EOF
)
ref=$(bash "$REPO/agent/referee.sh" frontier/frontier.csv) || fail "the frontier is not valid: $ref"
echo "$ref"
score=$(sed -n 's/.* score=\([0-9]*\) .*/\1/p' <<< "$ref")
[ "$score" = "$best" ] || fail "the frontier scores $score, but scoreboard.json says best_score=$best"
elite=$(python3 "$REPO/tools/E555_rank.py" frontier/elite.csv --seed_file "$SEED" --count)
echo "SETUP campaign=$CAMPAIGN lab=$state path=$LAB arch=$ARCH cpu=\"$cpu\"" \
     "cpu_changed=$cpu_changed threads=$threads ram_gb=$ram_gb disk_gb=$disk_gb" \
     "ortools=$ortools stage_b_ok=$stage_b_ok db_on_disk=$db_on_disk best=$best elite=$elite"
