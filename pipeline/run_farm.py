#!/usr/bin/env python3
"""run_farm.py -- beamer, topper, roundhouse, ender, in a loop, for days.

    python3 pipeline/run_farm.py
    python3 pipeline/run_farm.py THREADS=10 DB=/tmp/E555.db HOURS=48

ONE ITERATION, top to bottom. Every stage is one tool call in produce():

    1 beamer      random borders, grow to row 11, emit everything
    2 clean_csv   drop the near-duplicates the beam emits in hundreds
    3 topper      open rows 8..12 and close row 12
    4 roundhouse  width 4, forward then reverse, TIES refills each
    5 clean_csv   again, over the spirals and the boards they came from
    6 topper      open rows 10..15 and fill the top
    7 ender       the best ENDER_TOP, one pass       -> good.csv
    8 ender       every ELITE_EVERY, the best of those, deep -> elite.csv

THREE FILES, and a board's history is which one it is in:

    boards.csv   just built, waiting for its first ender
    good.csv     endered once, waiting for the deep pass
    elite.csv    deeply endered -- the hand-off set, never searched again

Ctrl-C is safe: the farm finishes the tool it is on, writes the files, exits.
Why the budgets and the geometry are what they are: pipeline/README.md.
"""

import itertools
import os
import shutil
import signal
import subprocess
import sys
import time

# ---- settings: pass NAME=value on the command line --------------------------

REPO      = ""              # E555 checkout; empty = the parent of this file
SEED      = "data/seed_Edge5.txt"
FARM      = "farm_py"       # where the three files and the log go
DB        = "chain.db"      # beamer chain cache; absolute puts it on local
                            # disk, empty builds it in memory each run
THREADS   = 10
HOURS     = 0               # 0 = run until you stop it
CLUES     = "none"          # none | center | all
KEEP      = 500             # boards kept in boards.csv

BEAM      = 900             # 1 beamer, whole stage
TOP1      = 24              # 3 boards into the first topper
T1_TIME   = 120             #   per-board ceiling; it has never reached this
T1_STALL  = 10              #   what actually stops it: seconds with no gain
TIES      = 5               # 4 distinct refills each roundhouse pass keeps
RH        = 600             #   roundhouse, whole stage, per pass
TOP2      = 20              # 6 boards into the second topper
T2_TIME   = 300             #   this stage IS budget-bound; the seconds count
T2_STALL  = 45

ENDER_TOP     = 5           # 7 boards given one ender pass per iteration
ENDER_TIME    = 45
ENDER_REACH   = 2           #   ladder length is reach x (changes / 4) solves
ENDER_CHANGES = 16

ELITE_EVERY   = 10          # 8 iterations between deep passes
ELITE_TOP     = 10
ELITE_TIME    = 120
ELITE_REACH   = 3
ELITE_CHANGES = 24

GIVE_UP_AFTER = 10          # consecutive failed iterations that end the run

SETTINGS = ("REPO SEED FARM DB THREADS HOURS CLUES KEEP BEAM TOP1 T1_TIME "
            "T1_STALL TIES RH TOP2 T2_TIME T2_STALL ENDER_TOP ENDER_TIME "
            "ENDER_REACH ENDER_CHANGES ELITE_EVERY ELITE_TOP ELITE_TIME "
            "ELITE_REACH ELITE_CHANGES GIVE_UP_AFTER").split()

# -----------------------------------------------------------------------------

CLUE = {"none": [], "center": ["--clue_center"],
        "all": ["--clue_center", "--clue_corners"]}

# Everything below is filled in by paths() once REPO is known, and named here
# so the module reads without having to run main() to find out what exists.
BIN = TOOLS = TOPPER = ENDER = ""       # where the tools are
POOL = GOOD = ELITE = ""                # the three files boards graduate through
STOP = False                # set by Ctrl-C, checked between stages
LOG = None                  # this iteration's log file
FAILED = 0                  # worst exit code this iteration. sh() records it,
                            # so every stage below is just its tool call


def die(msg):
    sys.exit("run_farm: " + msg)


def settings(argv):
    for arg in argv:
        name, _, value = arg.partition("=")
        if name not in SETTINGS:
            die("unknown setting %r. Known: %s" % (name, " ".join(SETTINGS)))
        globals()[name] = int(value) if isinstance(globals()[name], int) else value


# ---- running a tool ---------------------------------------------------------

def sh(*cmd):
    """Run one tool. Everything it prints goes to this iteration's log.

    A non-zero exit is remembered in FAILED, not returned, so a stage reads as
    the tool call it is. A stage that dies never stops the iteration: whatever
    the earlier stages made still reaches the pool."""
    global FAILED
    cmd = [str(c) for c in cmd]
    LOG.write("\n$ " + " ".join(cmd) + "\n")
    LOG.flush()
    try:
        rc = subprocess.run(cmd, cwd=REPO, stdout=LOG,
                            stderr=subprocess.STDOUT).returncode
    except FileNotFoundError:
        rc = 127
    # A killed process returns a NEGATIVE code, which would compare below zero
    # and read as success. Fold signals into the shell's convention.
    if rc < 0:
        rc = 128 - rc
    if rc:
        LOG.write("!! %s exited %d\n" % (cmd[0], rc))
        FAILED = max(FAILED, rc)
    return rc


# ---- board files: every tool reads and writes the same 514-field CSV --------

def read(path):
    if not path or not os.path.exists(path):
        return []
    return [ln.rstrip("\n") for ln in open(path)
            if ln.strip() and ln.lstrip()[0] not in "#%"]


def write(path, lines):
    """Atomic: a farm killed mid-write must not leave half a file."""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.writelines(ln + "\n" for ln in lines)
    os.replace(tmp, path)


def emitted(out_dir):
    """The files a C tool reports writing, minus the ones it left empty.

    The tools append and write their header with the first board, so a stage
    that found nothing still lists a zero-byte file in outputs.txt."""
    listed = read(os.path.join(out_dir, "outputs.txt"))
    return [p for p in listed if os.path.exists(p) and os.path.getsize(p)]


def board_id(line):
    return line.split(",", 1)[0].strip()


def cells(line):
    """Where each piece sits: pos[256], read from the end of the row because
    tools disagree about what goes in front of it. 999 means unplaced."""
    return [x.strip() for x in line.split(",")[-512:-256]]


def placed(line):
    return sum(1 for c in cells(line) if c != "999")


def closed_to(line, row):
    """True when every cell up to and including `row` holds a piece. This is
    what decides whether a board is worth spiralling."""
    filled = {int(c) for c in cells(line) if c != "999"}
    return filled.issuperset(range(16 * (row + 1)))


def floor(line):
    """The rows 0..7 the first topper locks. It frees rows 8..12, so this IS
    its problem: two boards with the same floor are the same search."""
    field = [x.strip() for x in line.split(",")]
    pos, rot = field[-512:-256], field[-256:]
    return frozenset((p, pos[p], rot[p]) for p in range(256)
                     if pos[p] != "999" and int(pos[p]) < 128)


# ---- the three helpers the stages share -------------------------------------

def rank(dst, *src):
    """Sort best first -- fewest breaks, then breaks packed into fewest rows.

    --rescore also RENAMES, to "<id>_<old field 2>", so anything reading
    provenance out of an id has to run before a ranking, not after."""
    src = [p for p in src if p and os.path.exists(p) and os.path.getsize(p)]
    if not src:
        write(dst, [])
        return []
    sh(sys.executable, TOOLS + "/E555_rank.py", *src, "--seed_file", SEED,
       "--sort", "breaks,break_rows", "--out", dst, "--rescore")
    return read(dst)


def clean(dst, src):
    """Drop a board that repeats an earlier one with a single frontier piece
    swapped, and exact duplicates. Those collapse into the same search the
    moment a later stage frees the frontier, so they are rows, not boards."""
    if not read(src):
        return []
    if sh(sys.executable, TOOLS + "/E555_clean_csv.py", src, "--out", dst):
        return read(src)
    return read(dst) or read(src)


def spread(boards, n):
    """Take n boards, as varied as we can make them.

    Every row-11 board scores exactly the same -- 192 pieces, break-free -- so
    ranking cannot choose between them and "the top n" is really "the first
    config's first n". Measured: 20 boards, 1 config, 13 distinct floors.
    Going round-robin across configs and skipping a floor already taken gives
    20 boards, 4 configs, 20 floors, for the same money."""
    group = {}
    for b in boards:
        group.setdefault(board_id(b).rsplit("_", 1)[0], []).append(b)
    out, seen = [], set()
    for row in itertools.zip_longest(*group.values()):
        for b in row:
            if b is None or len(out) >= n:
                continue
            key = floor(b)
            if key not in seen:
                seen.add(key)
                out.append(b)
        if len(out) >= n:
            break
    return out


def kept_spirals(spirals, parents):
    """Spirals worth carrying on: never fewer pieces than they started with.

    A roundhouse pass frees three bands to search them, and when it cannot
    refill them break-free it emits the deepest board it reached -- which can
    hold less than its parent. Measured before this guard: parents at 208
    placed came back at 169 and took four of six slots in the next stage.

    The roundhouse keeps its input's config_id and appends _<line><tag><n>, so
    a spiral's parent is the board whose id is the longest prefix of its own."""
    by_id = {board_id(b): b for b in parents}
    out = []
    for s in spirals:
        ident = board_id(s)
        root = max((k for k in by_id if ident.startswith(k + "_")),
                   key=len, default=None)
        if root is None or placed(s) >= placed(by_id[root]):
            out.append(s)
    return out


# ---- stages 1 to 6: build boards ----------------------------------------------------

def build(d):
    """Stages 1 to 6, one tool call each. Returns the boards that came out."""
    clue = CLUE[CLUES]

    # 1. BEAMER -- random borders up to row 11, nothing capped but the clock.
    #    Random mode counts bottoms with --samples; --num_rows does not apply.
    sh(BIN + "/E555_beamer", SEED,
       "--random_edges", "--samples", 0,
       "--stop_row", 11, "--top_columns", 5, "--max_emitted", 0,
       "--wall_time", BEAM, "--threads", THREADS,
       "--out_dir", d + "/beam", "--print_cmd",
       *clue, *(["--db_file", DB] if DB else []))

    write(d + "/beam.csv", [ln for p in emitted(d + "/beam") for ln in read(p)])
    if not read(d + "/beam.csv"):
        LOG.write("!! the beamer emitted nothing; raise BEAM\n")
        return []

    # 2. CLEAN_CSV -- the beam emits in hundreds and many are one board with a
    #    different piece on the frontier. Stage 3 frees rows 8..12, so those
    #    are the same search twice. Measured: 79 of 312 dropped.
    rank(d + "/beam_ranked.csv", d + "/beam.csv")
    clean(d + "/beam_clean.csv", d + "/beam_ranked.csv")
    write(d + "/top1_in.csv", spread(read(d + "/beam_clean.csv"), TOP1))

    # 3. TOPPER -- band_depth 8 opens rows 8..15, locked_rows 3 empties 13..15,
    #    leaving rows 8..12 free: deep enough to lift a piece out of the core
    #    and spend it closing row 12, which is the whole point of the stage.
    sh(sys.executable, TOPPER, SEED, d + "/top1_in.csv", d + "/top1.csv",
       "--side", "T", "--band_depth", 8, "--locked_rows", 3,
       "--num_rows", 0, "--time_limit", T1_TIME, "--stall_time", T1_STALL,
       "--threads", THREADS, *clue)

    closed = read(d + "/top1.csv") or read(d + "/top1_in.csv")
    ready = [b for b in closed if closed_to(b, 12)]
    geom = ["--rounds", 3, "--strip_width", 4, "--rotate", -1, "--hold_band",
            "--ties", TIES, "--num_rows", 0, "--wall_time", RH,
            "--threads", THREADS, "--print_cmd", *clue]
    spirals = []

    # 4. ROUNDHOUSE, forward then reverse. Width 4 keeps rows 4..11 -- the part
    #    stage 3 proved -- and frees the bottom, right and top bands; width 3
    #    would keep row 12 as well and reject every board on the breaks left
    #    there. --rotate -1 re-cuts the bottom band the beam fixed at row 0 and
    #    never revisited. Only a board that really closed row 12 is worth it.
    if ready:
        write(d + "/rh_in.csv", ready)
        sh(BIN + "/E555_roundhouse", SEED, d + "/rh_in.csv",
           "--out_dir", d + "/rh1", *geom)

        spirals = [ln for p in emitted(d + "/rh1") for ln in read(p)]
        if spirals:
            write(d + "/rh1.csv", spirals)
            sh(BIN + "/E555_roundhouse", SEED, d + "/rh1.csv",
               "--out_dir", d + "/rh2", "--reverse", *geom)

            spirals += [ln for p in emitted(d + "/rh2") for ln in read(p)]
    else:
        LOG.write("!! nothing closed row 12, so there is nothing to spiral\n")

    # 5. CLEAN_CSV again -- spirals and the boards they came from, together.
    #    No board is dropped for having been spiralled; the ranking chooses.
    write(d + "/top2_all.csv", kept_spirals(spirals, closed) + closed)
    rank(d + "/top2_ranked.csv", d + "/top2_all.csv")
    clean(d + "/top2_clean.csv", d + "/top2_ranked.csv")
    write(d + "/top2_in.csv", read(d + "/top2_clean.csv")[:TOP2])

    # 6. TOPPER again -- band_depth 6 with locked_rows 0 opens rows 10..15 and
    #    fills the top. locked_rows must be 0 here: it unsets what it locks,
    #    and no later pass would put those rows back.
    sh(sys.executable, TOPPER, SEED, d + "/top2_in.csv", d + "/top2.csv",
       "--side", "T", "--band_depth", 6, "--locked_rows", 0,
       "--num_rows", 0, "--time_limit", T2_TIME, "--stall_time", T2_STALL,
       "--threads", THREADS, *clue)

    return read(d + "/top2.csv") or read(d + "/top2_in.csv")


# ---- stages 7 and 8: refine the best, graduate the very best ---------------

def ender(d, boards, out, deep):
    """7 and 8. One ender pass, or ring then patch for the deep one.

    The ender never returns a board worse than its input, so a generous budget
    carries no risk and chaining the two modes is free. It climbs a ladder per
    board -- [r1 m4], [r1 m8], ... -- one solve per rung at the full
    --time_limit, and the rungs number reach x (changes / 4): 8 solves a board
    here, 18 deep. That multiplier is how an iteration turns into a day."""
    if not boards:
        return []
    write(d + "/ender_in.csv", boards)
    budget = ELITE_TIME if deep else ENDER_TIME
    reach = ELITE_REACH if deep else ENDER_REACH
    changes = ELITE_CHANGES if deep else ENDER_CHANGES
    src = d + "/ender_in.csv"

    # Ring first: the topper leaves its damage on and near the border ring,
    # which is exactly what the ring sweep re-threads.
    modes = ["ring", "patch"] if deep else ["ring"]
    for i, mode in enumerate(modes):
        # The LAST pass writes to `out`; the deep one's ring pass feeds patch
        # from a scratch file. Getting this wrong sends the single-mode result
        # to the scratch file, and the boards never reach good.csv at all.
        dst = out if i == len(modes) - 1 else d + "/ender_%s.csv" % mode
        sh(sys.executable, ENDER, SEED, src, dst,
           "--mode", mode, "--reach", reach, "--max_changes", changes,
           "--num_rows", 0, "--time_limit", budget,
           "--stall_time", max(10, budget // 3),
           "--threads", THREADS, *CLUE[CLUES])

        if not read(dst):
            break
        src = dst
    return read(src)


def refine(d, n):
    """Stages 7 and 8. Everything built joins the pool; the best of it is
    endered once and moves to good.csv; every ELITE_EVERY iterations the best
    of THOSE gets the deep pass and graduates to elite.csv, where nothing
    searches it again. Returns (endered, promoted) for the log line."""
    rank(d + "/pool.csv", d + "/made.csv", POOL)
    clean(d + "/pool_clean.csv", d + "/pool.csv")
    write(POOL, read(d + "/pool_clean.csv")[:KEEP])

    # 7. One ender pass for the best few, which then leave the pool for good.
    endered = ender(d, take(POOL, ENDER_TOP), d + "/good.csv", False)
    rank(d + "/good_all.csv", d + "/good.csv", GOOD)
    write(GOOD, read(d + "/good_all.csv"))

    # 8. The deep pass, on a schedule because it costs 18 solves a board.
    promoted = []
    if not STOP and ELITE_EVERY and n % ELITE_EVERY == 0:
        promoted = ender(d, take(GOOD, ELITE_TOP), d + "/elite.csv", True)
        rank(d + "/elite_all.csv", d + "/elite.csv", ELITE)
        write(ELITE, read(d + "/elite_all.csv"))
    return endered, promoted


def take(path, n):
    """Take the best n off a file, leaving the rest. Because every stage takes
    the top of a ranked file, "remove what we just took" is a slice -- which is
    why no board ever needs an attempt counter stored anywhere."""
    rows = read(path)
    if rows[:n]:
        write(path, rows[n:])
    return rows[:n]


# ---- the loop ---------------------------------------------------------------

def best(path):
    """Matched edges of the best board in a file, or 0."""
    if not read(path):
        return 0
    try:
        out = subprocess.run([sys.executable, TOOLS + "/E555_rank.py", path,
                              "--seed_file", SEED, "--field", "score"],
                             cwd=REPO, capture_output=True, text=True, timeout=600)
        return int(out.stdout.strip())
    except Exception:
        return 0


def note(msg):
    print("[farm] " + msg, flush=True)
    with open(FARM + "/farm.log", "a") as f:
        f.write("[farm] %s %s\n" % (time.strftime("%F %T"), msg))


def preflight():
    """Everything that must be true before iteration 1, checked once."""
    for stale in os.listdir(FARM):           # whatever a killed farm left
        if stale.startswith("run_"):
            shutil.rmtree(FARM + "/" + stale, ignore_errors=True)
    signal.signal(signal.SIGINT, stop)       # Ctrl-C finishes the iteration
    signal.signal(signal.SIGTERM, stop)
    if not os.path.exists(BIN + "/E555_beamer"):
        subprocess.run(["make"], cwd=REPO)
    if not os.path.exists(SEED):
        die("seed file not found: " + SEED)
    if CLUES not in CLUE:
        die("CLUES must be none, center or all")
    if subprocess.run([sys.executable, "-c", "import ortools"],
                      capture_output=True).returncode:
        die("ortools is missing, so both toppers and the ender would fail in\n"
            "        every iteration. Install it with:  pip install ortools")
    if not DB and BEAM < 420:
        note("warning: BEAM=%d with no DB. --wall_time covers the whole run and"
             " building the chain database takes 90-180s of it, so little is"
             " left to search with and the iteration can yield nothing at all."
             " Set DB, or raise BEAM past ~420." % BEAM)


def keep_log(d, name):
    """Copy this iteration's log out before its directory is deleted. Only
    three things are worth keeping: a record, a failure, and an iteration that
    quietly produced nothing -- which is the one you most need to explain."""
    dst = FARM + "/" + name
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy(d + "/iter.log", dst)


def main():
    global LOG, FAILED
    settings(sys.argv[1:])
    globals().update(paths())
    os.makedirs(FARM, exist_ok=True)
    preflight()
    note("%s, %d threads, clues=%s, db=%s"
         % (FARM, THREADS, CLUES, DB or "<in memory>"))

    record = max(best(ELITE), best(GOOD), best(POOL))
    failures, started, it = 0, time.time(), 0
    while not STOP:
        if HOURS and time.time() - started >= HOURS * 3600:
            note("HOURS reached, stopping.")
            break
        it += 1
        d = "%s/run_%d" % (FARM, it)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        t0 = time.time()

        with open(d + "/iter.log", "w") as LOG:
            FAILED = 0
            write(d + "/made.csv", build(d))        # stages 1 to 6
            refined, promoted = refine(d, it)       # stages 7 and 8

        now = max(best(ELITE), best(GOOD), best(POOL))
        took = int(time.time() - t0)
        made = len(read(d + "/made.csv"))
        tail = "%d made, %d endered, %d to elite" % (made, len(refined),
                                                     len(promoted))
        if not made and not FAILED:
            keep_log(d, "empty_iter%d.log" % it)
        if FAILED:
            failures += 1
            keep_log(d, "failed/iter%d.log" % it)
            note("iter %d FAILED (exit %d) after %ds, best still %d/480"
                 % (it, FAILED, took, record))
            if GIVE_UP_AFTER and failures >= GIVE_UP_AFTER:
                note("%d failures -- stopping. Read %s/failed/ for the reason."
                     % (failures, FARM))
                break
            time.sleep(5)                    # a broken setup must not spin
        else:
            failures = 0
            if now > record:
                keep_log(d, "best_%d.log" % now)
                note("iter %d -> NEW RECORD %d/480 (was %d), %s in %ds"
                     % (it, now, record, tail, took))
                record = now
            else:
                note("iter %d -> %s, best still %d/480, %ds"
                     % (it, tail, record, took))
        shutil.rmtree(d, ignore_errors=True)

    note("%d iterations (%d failed), best %d/480" % (it, failures, record))
    note("elite boards in " + ELITE)


def stop(_sig, _frame):
    """Finish the tool we are on, write the files, exit cleanly."""
    global STOP
    STOP = True
    print("\n[farm] stop requested -- finishing this iteration.", flush=True)


def paths():
    repo = REPO or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    farm = os.path.abspath(FARM)
    db = DB
    if db:
        db = db if os.path.isabs(db) else farm + "/" + db
        # A clued run leaves the clue pieces out of the chains, so its cache is
        # not interchangeable with an unclued one; one file per setting.
        if CLUES != "none":
            db += "." + CLUES
    return {"REPO": repo, "BIN": repo + "/bin", "TOOLS": repo + "/tools",
            "TOPPER": repo + "/src/C_tail/E555_topper.py",
            "ENDER": repo + "/src/C_tail/E555_ender.py",
            "SEED": SEED if os.path.isabs(SEED) else repo + "/" + SEED,
            "FARM": farm, "DB": db,
            "POOL": farm + "/boards.csv",
            "GOOD": farm + "/good.csv",
            "ELITE": farm + "/elite.csv"}


if __name__ == "__main__":
    main()
