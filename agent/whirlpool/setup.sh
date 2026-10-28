#!/bin/bash
# setup.sh -- start, repair or restore a whirlpool campaign
# (agent/whirlpool_agent_mode.md, agent/launch_whirlpool_agent.md). A campaign
# is one launch of the whirlpool agent. It writes only into its own folder,
# agent/whirlpool/<id>/, named by the UTC date and time it started
# (20261027-1930), which agent/whirlpool/checkpoint.sh pushes to the results
# branch; it works in that folder's lab/. Code branches ignore the folder. Safe to run at any time: each step does only
# what is missing, so a healthy campaign costs a few seconds.
#
#   bash agent/whirlpool/setup.sh                          # a new campaign
#   bash ../../setup.sh                                    # inside a lab: that campaign's health check
#   bash agent/whirlpool/setup.sh CAMPAIGN=20261027-1930   # the same, from anywhere
#   bash agent/whirlpool/setup.sh CAMPAIGN=20261027-1930 RESUME=agent_runs
#                                                          # bring back a campaign from the results branch
#   SETUP campaign=20261027-1930 folder=fresh lab=fresh path=/.../agent/whirlpool/20261027-1930/lab arch=v3 ...
#
# 1. The folder: kept; restored from branch RESUME (the results branch that
#    checkpoint.sh pushes to, agent_runs by default) when this container has
#    lost it; or created, with its empty logs (see the kit, "The campaign folder").
# 2. The lab: a copy of Makefile, src, tools, examples, pipeline, data, tests
#    and the two docs, then the folder's patches/ laid over it. patches/ holds
#    full copies of changed src/ and tools/ files (and the Makefile) at their
#    repository paths; it is the only place a code change survives a lost
#    container. A file dropped from patches/ is put back to the repository's.
# 3. The binaries: ARCH=auto builds v3 (AVX2) where the CPU runs x86-64-v3,
#    else generic; a binary that dies with SIGILL forces generic.
# 4. This machine's facts in scoreboard.json (cpu, arch, threads, ram_gb,
#    disk_gb, stage_b_ok, db_on_disk), and the judge's self-test. stage_b_ok
#    needs 8 GB of free RAM for the beamer's chain database; db_on_disk means
#    the lab can keep it as lab/E555.db (already there, or 8 GB of free disk).
#    That file is never committed: *.db is gitignored.
# Exit status 0 = ready, 1 = a step failed, 2 = usage.
# ---- settings: pass NAME=value on the command line ---------------------------
CAMPAIGN=           # empty: the campaign this runs in, else a new one;
                    # new: a new campaign; or an id. With RESUME, empty or new
                    # means the one campaign on that branch
ARCH=auto           # auto, v3, v2, generic or native (the Makefile's ARCH)
RESUME=none         # a git branch whose agent/whirlpool/<id>/ restores a lost campaign
# -----------------------------------------------------------------------------
SETTINGS=" CAMPAIGN ARCH RESUME "
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
ID_RE='^[0-9]{8}-[0-9]{4}(-[0-9]+)?$'
fail() { echo "SETUP failed: $*"; exit 1; }
FETCH_LOG=${TMPDIR:-/tmp}/e555_whirlpool_fetch.log

fetched=0
fetch_resume() {
    [ "$fetched" = 1 ] && return 0
    local wait
    for wait in 2 4 8 16 0; do
        if git -C "$REPO" fetch -q origin "$RESUME" 2>> "$FETCH_LOG"; then
            fetched=1; return 0
        fi
        [ "$wait" = 0 ] || sleep "$wait"
    done
    fail "git fetch origin $RESUME; see $FETCH_LOG"
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
        fetch_resume
        ids=$(git -C "$REPO" ls-tree -d --name-only FETCH_HEAD agent/whirlpool/ | sed 's#.*/##' |
              grep -E "$ID_RE" || true)
        [ -n "$ids" ] || fail "branch $RESUME has no campaign under agent/whirlpool/"
        [ "$(wc -l <<< "$ids")" = 1 ] ||
            fail "branch $RESUME holds several campaigns; pass CAMPAIGN= one of:" $ids
        CAMPAIGN=$ids
    else
        CAMPAIGN=$(date -u +%Y%m%d-%H%M)
        n=1
        while [ -e "$KIT/$CAMPAIGN" ]; do
            n=$((n + 1)); CAMPAIGN=$(date -u +%Y%m%d-%H%M)-$n
        done
    fi
fi
[[ "$CAMPAIGN" =~ $ID_RE ]] || fail "bad campaign id '$CAMPAIGN' (expected YYYYMMDD-HHMM)"
DIR="$KIT/$CAMPAIGN"
LAB="$DIR/lab"

# 1. the folder
folder=kept
if [ ! -f "$DIR/scoreboard.json" ]; then
    if [ "$RESUME" != none ]; then
        fetch_resume
        git -C "$REPO" archive FETCH_HEAD "agent/whirlpool/$CAMPAIGN" 2>> "$FETCH_LOG" |
            tar -x -C "$REPO" || fail "branch $RESUME has no agent/whirlpool/$CAMPAIGN/"
        [ -f "$DIR/scoreboard.json" ] || fail "the checkpoint on $RESUME has no scoreboard.json"
        folder=restored
    else
        mkdir -p "$DIR"
        cat > "$DIR/scoreboard.json" <<'JSON'
{ "kit": "whirlpool", "goal_start_unix": null, "last_checkpoint_unix": null,
  "best": null, "best_unix": null, "harness_runs": 0, "candidates": {},
  "best_dived": null }
JSON
        folder=fresh
    fi
fi
cd "$DIR"
mkdir -p candidates patches whirlpools
[ -f JOURNAL.md ] || printf '# Journal, whirlpool campaign %s\n\n' "$CAMPAIGN" > JOURNAL.md
[ -f runs.csv ] || printf 'unix,iso,kind,label,runtime_s,exit\n' > runs.csv
[ -f harness.csv ] || printf 'unix,iso,run,candidate,sha,patches,minutes,standard,produced,attempted,laps1,laps2,laps3,frame_held,jewels,archived,per_hour,depth_max,cpu,threads,note\n' > harness.csv
[ -f attempts.csv ] || printf 'unix,run,attempt,candidate,board,seconds,exit,chains,laps,frame,jewels,new,archived,first_jewel_s,depth\n' > attempts.csv
for f in seen_boards.txt frames_seen.txt; do [ -f "$f" ] || : > "$f"; done
[ -f whirlpool_index.csv ] || printf 'chain,laps,jewel,frame,frame_id,new_frame,depth,depths,dir,candidate,run,attempt,seconds,unix\n' > whirlpool_index.csv

# 2. the lab and the patches over it
lab=kept
mkdir -p "$LAB"
if [ ! -f "$LAB/Makefile" ]; then
    tar -C "$REPO" --exclude=tests/out --exclude=__pycache__ --exclude='*.o' -cf - \
        Makefile src tools examples pipeline data tests PROJECT_E555.md README.md |
        tar -C "$LAB" -xf -
    lab=fresh
fi
mkdir -p "$LAB/runs"
patched=()
while IFS= read -r -d '' f; do
    rel=${f#patches/}
    case "$rel" in
        src/*|tools/*|Makefile) ;;
        *) fail "patches/$rel: only src/, tools/ and the Makefile may be patched" ;;
    esac
    patched+=("$rel")
    mkdir -p "$LAB/$(dirname "$rel")"
    cmp -s "$f" "$LAB/$rel" || cp "$f" "$LAB/$rel"
done < <(find patches -type f -print0 | sort -z)
if [ -f "$LAB/.patched" ]; then
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        [[ " ${patched[*]-} " == *" $rel "* ]] && continue
        if [ -f "$REPO/$rel" ]; then cp "$REPO/$rel" "$LAB/$rel"; else rm -f "$LAB/$rel"; fi
    done < "$LAB/.patched"
fi
printf '%s\n' "${patched[@]-}" | sed '/^$/d' > "$LAB/.patched"
patches_sha=none
if [ "${#patched[@]}" -gt 0 ]; then
    patches_sha=$(cd patches && find . -type f -print0 | sort -z | xargs -0 sha1sum | sha1sum | cut -c1-8)
fi

# 3. the binaries
cd "$LAB"
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

# 4. this machine, and the judge
threads=$(nproc)
cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
cpu=${cpu:-unknown}
ram_gb=$(awk '/^MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)
disk_gb=$(df -Pk . | awk 'NR == 2 {printf "%d", $4 / 1048576}')
stage_b_ok=false
if [ "$ram_gb" -ge 8 ]; then stage_b_ok=true; fi
db_on_disk=false
if [ -s E555.db ] || [ "$disk_gb" -ge 8 ]; then db_on_disk=true; fi
read -r cpu_changed best < <(python3 - "$DIR/scoreboard.json" "$CAMPAIGN" "$cpu" "$ARCH" "$threads" \
        "$ram_gb" "$disk_gb" "$stage_b_ok" "$db_on_disk" "$patches_sha" <<'EOF'
import json, sys
path, campaign, cpu, arch, threads, ram, disk, stage_b, db_on_disk, patches = sys.argv[1:]
s = json.load(open(path))
changed = "yes" if s.get("cpu") not in (None, cpu) else "no"
s.update(campaign=campaign, cpu=cpu, arch=arch, threads=int(threads), ram_gb=int(ram),
         disk_gb=int(disk), stage_b_ok=stage_b == "true", db_on_disk=db_on_disk == "true",
         patches=patches)
json.dump(s, open(path, "w"), indent=1)
print(changed, s.get("best") or "none")
EOF
)
python3 "$KIT/judge.py" --self_test > runs/judge_self_test.log 2>&1 ||
    fail "the judge's self-test failed; see $LAB/runs/judge_self_test.log"
jewels=$(grep -c . "$DIR/frames_seen.txt" || true)
echo "SETUP campaign=$CAMPAIGN folder=$folder lab=$lab path=$LAB arch=$ARCH cpu=\"$cpu\"" \
     "cpu_changed=$cpu_changed threads=$threads ram_gb=$ram_gb disk_gb=$disk_gb" \
     "stage_b_ok=$stage_b_ok db_on_disk=$db_on_disk patches=${#patched[@]} jewels=$jewels best=$best"
