#!/usr/bin/env python3
"""run_farm.py -- produce, refine and harvest E555 boards, indefinitely.

    python3 pipeline/run_farm.py
    python3 pipeline/run_farm.py THREADS=32 DB_FILE=/tmp/E555.db CLUES=center
    python3 pipeline/run_farm.py FARM_DIR=/data/farm ANNEAL_EVERY=4 MAX_HOURS=48

NOT an example: start it once and leave it running for days. Ctrl-C is safe --
the pools are rewritten atomically and the farm exits after the step it is on.

This is a second runner beside run_board_farm.sh, not a replacement. The bash
farm drives run_pipeline.sh; this one runs its own chain, built around the fact
that ROW 12 IS WHERE EVERYTHING STALLS. No configuration of any tool here has
ever emitted a complete row 12, so the chain stops asking the beam for one and
hands the job to CP-SAT instead.

THE THREE POOLS -- a board's search history is which file it sits in.

    queue/produced.csv   built this pass, waiting for its first ender
    queue/refined.csv    endered once, waiting for the deep pass
    champions.csv        deeply endered, ranked, never searched again --
                         the hand-off set for manual work with other tools

Nothing is ever searched twice at the same depth, and champions.csv only grows.

ONE ITERATION = production, then refinement. Every HARVEST_EVERY iterations a
harvest runs after refinement and moves boards into champions.csv.

    1 borders    random, or annealed every ANNEAL_EVERY-th iteration: anneal
                 4x ANNEAL_BORDERS restarts and keep the best quarter
    2 beamer     to row 11, five columns per bottom, emissions uncapped
    3 clean      rank, then drop the frontier siblings -- boards the beam
                 emits in hundreds that differ only in the piece on top
    4 topper     FOCUSED: rows (16-FOCUS_DEPTH)..12 open, to close row 12
    5 roundhouse forward then reverse at width 4, complete row 12 only, and
                 never allowed to give back a board worse than it was handed
    6 topper     FINAL: the top band, to fill rows 13..15
    refine       the best REFINE_TOP get one ender pass  -> queue/refined.csv
    harvest      the best HARVEST_TOP get a deep ender   -> champions.csv

Watch: farm.log (one line per iteration), champions.csv, records/ (every board
that set a record, with the log that produced it).
"""

import gzip
import json
import os
import shutil
import signal
import subprocess
import sys
import time

# ---- settings: pass NAME=value on the command line --------------------------
S = {
    "REPO": "",                 # E555 checkout; empty = the parent of this file
    "SEED": "data/seed_Edge5.txt",
    "FARM_DIR": "farm_py",      # relative to where you START it
    "THREADS": 8,
    "MAX_HOURS": 0,             # 0 = run until you stop it
    "KEEP": 500,                # boards kept in queue/produced.csv
    "CLUES": "none",            # none | center | all
    "DB_FILE": "chain.db",      # beamer chain cache; absolute puts it on local
                                # disk. Empty disables. Only the beamer can use
                                # one: the other tools build a database around
                                # the pieces each board locks.
    # Borders. Random costs nothing; annealed spends minutes in Stage A first.
    "ANNEAL_EVERY": 0,          # 0 or less = never anneal, always random (the
                                # default); 4 = every fourth production
                                # iteration anneals its border instead
    "ANNEAL_BORDERS": 40,       # borders kept, out of 4x that many restarts.
                                # Size it to what the beam can actually consume
                                # in BEAM_WALL, or the sweep starves and stops
                                # early. Measured at BEAM_WALL=420 on 4 threads:
                                # 36.1 s per (bottom x column) config, and
                                # --top_columns 5 makes 5 configs per border --
                                # about 1.8 borders, so ~4.5 in 900 s. That rate
                                # scales with cores, so a 32-thread node gets
                                # through roughly 20; 40 leaves headroom without
                                # paying for borders nobody reaches. Spare
                                # borders cost only annealer time, a starved
                                # beam costs sweep time.
    "ANNEAL_STEPS": 500000,     # below 250000 the annealer often finds none

    # Per-stage budgets, seconds. These are what you raise for a long run.
    #
    # Two kinds of budget, and mixing them up is how an iteration turns into a
    # day. The C tools take --wall_time: a total for the whole stage. The CP-SAT
    # tools take --time_limit: a ceiling PER BOARD. So a CP-SAT stage costs its
    # board cap times its time limit, and the defaults below are chosen to make
    # a full iteration land near three hours at worst:
    #
    #   beamer     BEAM_WALL                        900
    #   focused    FOCUS_TOP x FOCUS_TIME      20 x 120 = 2400
    #   roundhouse RH_WALL, twice                    600
    #   final      FINAL_TOP x TOPPER_TIME     20 x 300 = 6000
    #
    # WHERE THE TWO TOPPER BUDGETS COME FROM. Measured over 24 solves of each
    # stage across four production iterations, at --time_limit 90 --stall_time
    # 30 -- the two stages behave nothing alike:
    #
    #   focused  mean 35.8s and NOT ONE of the 24 reached the limit. 29% ended
    #            `optimal` at 17.5s -- CP-SAT proving no arrangement of rows
    #            8..12 on that locked floor has fewer breaks -- and the other
    #            71% stopped on the stall watchdog, having last improved around
    #            13s. --time_limit is therefore not this stage's constraint and
    #            raising it buys nothing; the stall is the only live dial,
    #            which is why FOCUS_STALL is its own setting. FOCUS_TIME just
    #            has to stay clear of 13s + FOCUS_STALL.
    #   final    mean 89.1s of the 90 allowed, 88% cut off BY THE CLOCK, mean
    #            gain +61.6 edges. The one stage in the chain that was
    #            genuinely truncated, so this is where the seconds go.
    #
    # The ender multiplies again, and this is the trap that matters. Ring and
    # patch mode both climb a LADDER per board -- [r1 m4], [r1 m8], [r1 m12],
    # ... -- one solve per rung, each granted the full --time_limit. The rung
    # count is
    #
    #       reach x (max_changes / 4)
    #
    # so refine is 2 x 4 = 8 solves a board and the harvest 3 x 6 = 18. A stage
    # therefore costs boards x rungs x time. Measured, not guessed: one refine
    # board climbed its 8 rungs in 290s. Before the ladder was understood these
    # defaults read as 30 minutes and were really 4 hours an iteration, with a
    # harvest of 10 x 30 x 900s x 2 modes -- SIX DAYS for one harvest.
    #
    #   refine     REFINE_TOP x 8 x REFINE_TIME     5 x 8 x 45 = 1800
    #   harvest    HARVEST_TOP x 18 x HARVEST_TIME x 2 modes
    #                                           10 x 18 x 120 x 2 = 43200
    #
    # A rung usually stalls well before its ceiling -- 34s against a 90s limit
    # in the measured run -- so the real figures run about a third of these.
    # The harvest is deliberately the expensive one: it fires only every
    # HARVEST_EVERY iterations, on the boards about to become champions.
    "BEAM_WALL": 900,
    "FOCUS_DEPTH": 8,           # rows (16-N)..12 open in the focused topper;
                                # 8 opens rows 8..12. THE dial if row 12 will
                                # not close -- deeper reaches further into the
                                # core for the pieces it needs, at a
                                # superlinear CP-SAT cost. Must exceed 3.
    "FOCUS_TIME": 120,          # a ceiling this stage has never reached
    "FOCUS_STALL": 35,          # what actually stops it: seconds without an
                                # improvement. This, not FOCUS_TIME, is the
                                # dial to raise if you want the focused topper
                                # to try harder -- and raise FOCUS_TIME with it
                                # or the ceiling starts binding instead.
    "RH_WALL": 300,             # generous on purpose: the width-4 cut keeps a
                                # 96-piece core, 108 of them once --hold_band
                                # pins row 12, and exhausted a real board in
                                # 0.5s. A ceiling for the pathological case,
                                # not a working budget
    "TOPPER_TIME": 300,         # the stage that was being truncated
    "TOPPER_STALL": 100,
    # Boards allowed into each CP-SAT stage, best first. These caps are not
    # optional. The topper and the ender take only --time_limit, which is per
    # SOLVE -- unlike the C tools there is no total --wall_time -- so a stage
    # costs (boards in) x (time each). Measured: one 420s beam emitted 425
    # boards and the focused topper was still on board 19 twenty minutes later,
    # around 13 hours for that one stage. The beam is cheap and the solver is
    # not, so the ranking picks who gets the solver's time.
    "FOCUS_TOP": 20,
    "FINAL_TOP": 20,

    "REFINE_TOP": 5,            # boards given one ender pass per iteration
    "REFINE_TIME": 45,
    "REFINE_REACH": 2,          # ladder: reach x (changes/4) solves per board
    "REFINE_CHANGES": 16,

    "HARVEST_EVERY": 10,        # iterations between harvests
    "HARVEST_TOP": 10,          # boards deep-endered and promoted per harvest.
                                # This is what sets the champion rate: 10 every
                                # HARVEST_EVERY iterations, so at ~2.4h an
                                # iteration a harvest lands about once a day.
    "HARVEST_TIME": 120,
    "HARVEST_REACH": 3,         # a longer ladder than refine: 18 solves a board
    "HARVEST_CHANGES": 24,

    "GIVE_UP_AFTER": 10,        # consecutive FAILED iterations that end the run;
                                # 0 never gives up. A failure is a tool exiting
                                # non-zero, never an iteration that merely found
                                # nothing; any success resets the count.
    "FAILED_KEEP": 20,          # failure logs kept before it stops saving them
}
# -----------------------------------------------------------------------------

CLUE_FLAGS = {"none": [], "center": ["--clue_center"],
              "all": ["--clue_center", "--clue_corners"]}

STOP = False        # set by SIGINT/SIGTERM; checked between stages


def die(msg):
    print("run_farm: " + msg, file=sys.stderr)
    sys.exit(1)


def parse_args(argv):
    ints = {k for k, v in S.items() if isinstance(v, int)}
    for arg in argv:
        if "=" not in arg or not arg[0].isalpha():
            die("expected NAME=value, got: " + arg)
        k, v = arg.split("=", 1)
        if k not in S:
            die("unknown setting %s. Known: %s" % (k, " ".join(sorted(S))))
        if k in ints:
            try:
                S[k] = int(v)
            except ValueError:
                die("%s needs an integer, got: %s" % (k, v))
        else:
            S[k] = v


# ---- board files ------------------------------------------------------------
# Every tool here reads and writes the same 514-field CSV, and treats a blank,
# # or % line as a comment. One definition of "a board row" for the whole farm.

def is_board(line):
    s = line.lstrip()
    return bool(s) and s[0] not in "#%"


def read_boards(path):
    if not path or not os.path.exists(path):
        return []
    with open(path) as f:
        return [ln.rstrip("\n") for ln in f if is_board(ln)]


def write_boards(path, lines):
    """Atomic: a farm killed mid-write must never leave a truncated pool."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        for ln in lines:
            f.write(ln + "\n")
    os.replace(tmp, path)


def board_id(line):
    return line.split(",", 1)[0].strip()


def placement(line):
    """The board itself: where every piece sits and how it is turned.

    A canonical row is id, score, pos[256], rot[256]. Read from the END --
    every tool agrees on the last 512 fields and they do not all agree on what
    sits in front of them. Two rows with different ids and the same placement
    are the same board."""
    return tuple(x.strip() for x in line.split(",")[-512:])


def occupied(line):
    """The cells this board has a piece in. 999 is the unplaced marker."""
    cells = {int(x) for x in placement(line)[:256]}
    cells.discard(999)
    return cells


def closed_to_row(line, row):
    """True when every cell up to and including `row` holds a piece.

    This is what keeps a board that did not really finish row 12 out of the
    roundhouse. Never raises: one malformed line must not end a run that has
    been going for days, and a board we cannot read is simply not eligible."""
    try:
        return occupied(line).issuperset(range(16 * (row + 1)))
    except ValueError:
        return False


def spirals_worth_keeping(rh_boards, parents):
    """The roundhouse outputs that earned a place beside the boards they
    came from. Run this BEFORE any ranking: E555_rank.py --rescore renames
    every board, and this reads provenance out of the ids.

    The roundhouse KEEPS its input board's config_id and appends
    "_<line><tag><n>" (E555_roundhouse.c:1243), so a spiral's parent is the
    board whose id is the longest prefix of its own -- no tagging and no log
    parsing needed to tell who came from whom. A second-pass spiral is matched
    against the stage-4 board at the ROOT of its chain, since that is the board
    we promise not to damage.

    Two rules, both from watching it work:

    NOTHING COMES BACK UNSET. A spiral has to free three bands to search them,
    and when it cannot refill them break-free it emits the deepest board it
    reached -- which can hold fewer pieces than the one it started from.
    Measured: parents at 208 placed came back at 169 and took four of the six
    slots in the next stage. A spiral that lost pieces is dropped.

    AND NO COPIES. Often the only break-free refill is the one already
    standing, and the output is its input under a new name. Measured on a real
    row-12 board: the forward pass exhausted in 0.5s and gave it back with NOT
    ONE PIECE MOVED, having proved the standing refill is the only break-free
    one; the reverse pass, which frees the left band instead of the right,
    moved 2 pieces at (12,0). The first is a slot spent on a search already
    done, the second is a genuinely different board. Only the second is kept."""
    roots = {board_id(ln): ln for ln in parents}
    kept = []
    for line in rh_boards:
        ident = board_id(line)
        root = max((k for k in roots if ident.startswith(k + "_")),
                   key=len, default=None)
        if root is None:            # no parent of ours: nothing to compare to
            kept.append(line)
            continue
        try:
            if len(occupied(line)) < len(occupied(roots[root])):
                continue
        except ValueError:
            continue
        if placement(line) != placement(roots[root]):
            kept.append(line)
    return kept


def take_top(path, n):
    """Take the first n boards off a ranked pool, leaving the rest behind.

    Because every stage takes the TOP of a ranked file, "remove what we just
    took" is a slice -- which is why a board's attempt count never has to be
    stored anywhere."""
    rows = read_boards(path)
    taken, rest = rows[:n], rows[n:]
    if taken:
        write_boards(path, rest)
    return taken


def manifest(out_dir):
    """What a C tool reported writing, filtered to files that hold boards.

    The tools open their CSVs in append mode and write the header with the
    first board, so a run that emitted nothing leaves a zero-byte file listed
    in outputs.txt. Existence is not the test; content is."""
    p = os.path.join(out_dir, "outputs.txt")
    if not os.path.exists(p):
        return []
    out = []
    for line in open(p):
        q = line.strip()
        if q and os.path.exists(q) and os.path.getsize(q) > 0:
            out.append(q)
    return out


# ---- running tools ----------------------------------------------------------

def run_tool(cmd, logfh):
    """Run one tool, its whole output going to this iteration's log.

    Returns the exit code. Every caller checks it: a stage that dies must not
    be mistaken for one that ran and found nothing."""
    logfh.write("\n$ " + " ".join(str(c) for c in cmd) + "\n")
    logfh.flush()
    try:
        rc = subprocess.run([str(c) for c in cmd], cwd=REPO,
                            stdout=logfh, stderr=subprocess.STDOUT).returncode
    except FileNotFoundError:
        logfh.write("!! not found: %s\n" % cmd[0])
        return 127
    # A process killed by a signal returns a NEGATIVE code here. Callers combine
    # codes with max(), so a raw -4 (SIGILL) would compare below 0 and a crashed
    # tool would be indistinguishable from a clean run -- which is exactly how a
    # stale -march=native binary crashed eight iterations in a row while the farm
    # reported no failures at all. Fold signals into the shell's convention.
    if rc < 0:
        logfh.write("!! %s died on signal %d\n" % (cmd[0], -rc))
        return 128 - rc
    return rc


def rank_boards(inputs, out_path, logfh):
    """Rank best-first and rewrite canonically. Returns the boards written.

    --rescore makes field 2 the true matched-edge count whatever tool wrote the
    line, which is what makes scores comparable across stages; the tie-break is
    compactness, so of two equal boards the one with its breaks packed into
    fewer rows wins.

    Note --rescore also renames: the emitted id is "<id>_<old field 2>"
    (E555_rank.py:370). Anything that reads provenance out of an id has to run
    BEFORE a ranking, not after it."""
    inputs = [p for p in inputs if p and os.path.exists(p) and os.path.getsize(p)]
    if not inputs:
        write_boards(out_path, [])
        return []
    cmd = [sys.executable, os.path.join(TOOLS, "E555_rank.py"), *inputs,
           "--seed_file", SEED, "--sort", "breaks,break_rows",
           "--out", out_path, "--rescore"]
    if run_tool(cmd, logfh) != 0:
        return []
    return read_boards(out_path)


def score_of(path):
    """Matched edges of the best board in a file, or 0. Never raises: this
    number drives the record check and a stray parse error must not end a run
    that has been going for days."""
    if not path or not os.path.exists(path) or not os.path.getsize(path):
        return 0
    try:
        out = subprocess.run(
            [sys.executable, os.path.join(TOOLS, "E555_rank.py"), path,
             "--seed_file", SEED, "--field", "score"],
            cwd=REPO, capture_output=True, text=True, timeout=600)
        return int(out.stdout.strip())
    except Exception:
        return 0


def best_pool():
    """(score, path) of the pool holding the best board anywhere.

    Scored across all three: champions.csv is fed by promotion, so scoring only
    the queues would let a later, worse board read as a record the moment the
    real best was promoted. Returning the pool as well as the number is what
    lets a record snapshot copy the board that actually set it -- each pool is
    ranked best-first, so the winner's head is the board."""
    return max((score_of(p), p) for p in (CHAMPIONS, REFINED, PRODUCED))


# ---- production -------------------------------------------------------------

def anneal_borders(run, logfh):
    """Stage A: anneal four times what we need, keep the best quarter."""
    raw = os.path.join(run, "raw_rotations.csv")
    out = os.path.join(run, "borders.csv")
    rc = run_tool([sys.executable, os.path.join(SRC, "A_border", "E555_edge_annealer.py"),
                   SEED, "--restarts", 4 * S["ANNEAL_BORDERS"],
                   "--steps", S["ANNEAL_STEPS"], "--threads", S["THREADS"],
                   "--w_bottom", 0, "--w_left", 1, "--w_right", 3, "--w_top", 2,
                   "--out", raw], logfh)
    if rc != 0 or not read_boards(raw):
        logfh.write("!! the annealer produced no border; falling back to random\n")
        return None, rc
    rc = run_tool([sys.executable, os.path.join(TOOLS, "E555_sort_rotations.py"),
                   raw, "--top", S["ANNEAL_BORDERS"], "-o", out], logfh)
    return (out if rc == 0 and read_boards(out) else None), rc


def produce(run, logfh, annealed):
    """The six production stages. Returns (worst_rc, boards).

    A stage that dies does not discard what the stages before it made: the
    boards are threaded forward and whatever survives is returned."""
    clue = CLUE_FLAGS[S["CLUES"]]
    worst = 0
    borders = None

    if annealed:
        borders, rc = anneal_borders(run, logfh)
        worst = max(worst, rc)

    # 2. Beamer to row 11 -- where the beam reliably fills, so emissions are
    # uncapped (--max_emitted 0) and the wall clock is the only bound.
    beam_dir = os.path.join(run, "beam")
    worst = max(worst, run_tool(
        ["bin/E555_beamer", SEED]
        # Random mode counts bottoms with --samples, NOT --num_rows: the help
        # says outright that --start_row/--num_rows "do not apply" there, so
        # --num_rows leaves --samples at its default of 1 and the whole
        # iteration searches a single bottom. Both spellings are valid beamer
        # flags, so no flag check can catch that -- only a yield of zero can.
        + ([borders, "--num_rows", S["ANNEAL_BORDERS"]] if borders
           else ["--random_edges", "--samples", 0])    # 0 = uncapped bottoms
        + ["--stop_row", 11, "--top_columns", 5, "--max_emitted", 0,
           "--wall_time", S["BEAM_WALL"], "--threads", S["THREADS"],
           "--out_dir", beam_dir, "--print_cmd"]
        + clue
        + (["--db_file", DB_FILE] if DB_FILE else []), logfh))
    beam_out = manifest(beam_dir)
    if not beam_out:
        logfh.write("!! the beamer emitted nothing; raise BEAM_WALL\n")
        return worst, []

    # 3. Rank the beam's output, then drop its frontier siblings.
    #
    # The beam emits in hundreds, and many of those boards are one board plus a
    # different piece on the frontier: identical below row 11, differing only in
    # the cell on top. Stage 4 frees rows 8..12 outright, so a sibling pair
    # collapses into the SAME CP-SAT model -- same locked floor, same free pool
    # -- and two of the FOCUS_TOP slots buy one search. Measured on a real
    # sweep: 312 beamer boards carried only 126 distinct floors.
    #
    # clean_csv drops them structurally -- one cell differs and its top face is
    # exposed -- not on a similarity threshold. Ranking FIRST is what decides
    # which of a pair survives.
    beam_all = os.path.join(run, "beam_all.csv")
    write_boards(beam_all, [ln for p in beam_out for ln in read_boards(p)])
    ranked_path = os.path.join(run, "beam_ranked.csv")
    candidates = rank_boards([beam_all], ranked_path, logfh)
    if candidates:
        cleaned = os.path.join(run, "beam_clean.csv")
        if run_tool([sys.executable, os.path.join(TOOLS, "E555_clean_csv.py"),
                     ranked_path, "--out", cleaned], logfh) == 0:
            candidates = read_boards(cleaned) or candidates
    else:
        candidates = read_boards(beam_all)     # ranking died; press on unsorted

    # 4. Focused topper: close row 12, reaching deep for the pieces it needs.
    # --band_depth D counts inward from the top edge, so the band is rows
    # (16-D)..15; --locked_rows 3 unsets and locks 13..15. The solver's free
    # range is therefore rows (16-D)..12. A shallow band could only draw on
    # pieces still unplaced -- the ones needed to close row 12 are usually
    # already sitting in the core, which is what the depth is for.
    focus_in = os.path.join(run, "focus_in.csv")
    write_boards(focus_in, candidates[:S["FOCUS_TOP"]])
    focus_out = os.path.join(run, "focus_out.csv")
    if read_boards(focus_in):
        worst = max(worst, run_tool(
            [sys.executable, TOPPER, SEED, focus_in, focus_out,
             "--side", "T", "--band_depth", S["FOCUS_DEPTH"], "--locked_rows", 3,
             "--num_rows", 0, "--time_limit", S["FOCUS_TIME"],
             "--stall_time", S["FOCUS_STALL"],
             "--threads", S["THREADS"]] + clue, logfh))
    carried = read_boards(focus_out) or read_boards(focus_in)

    # 5. Double roundhouse at width 4, forward then reverse.
    #
    # WIDTH 4 IS FORCED BY THE GEOMETRY, not picked. --rounds 3 --rotate -1
    # frees the bottom, right and top bands and keeps rows W..(15-W) x columns
    # 0..(15-2W); core_usable() then demands every kept cell be placed,
    # frame-legal and break-free. Stage 4 hands over boards with rows 0..11
    # break-free, row 12 closed but carrying the residue, and 13..15 empty:
    #
    #     W=3 keeps rows 3..12 -- row 12's breaks make every board unusable,
    #                             which is why an earlier width-3 pass here
    #                             skipped its whole input and did no work
    #     W=4 keeps rows 4..11 -- exactly the part stage 4 proved
    #     W=5 keeps rows 5..10 -- proven too, but throws two more rows away
    #
    # So W=4 is the widest core these boards can honestly offer, and the spiral
    # re-cuts the bottom band the beam fixed at row 0 and never revisited --
    # which is the point of --rotate -1 and the reason border pieces buried low
    # can come back up.
    #
    # NOTHING COMES BACK UNSET. Three things see to it. Only boards that really
    # closed row 12 go in. --hold_band keeps the top band the cut would
    # otherwise free -- on a real row-12 board it reports "holding 12 piece(s)
    # = 3 chain level(s) in the TOP band", so the row stage 4 worked for is not
    # torn out to be searched again; it says so in the log when it cannot.
    # And spirals_worth_keeping() drops any output holding fewer pieces than
    # the board it came from, which is the case --hold_band cannot cover: the
    # bottom and side bands have to be freed for the search to happen at all.
    rh_boards = []
    rh_in = [ln for ln in carried if closed_to_row(ln, 12)]
    if rh_in:
        rh_src = os.path.join(run, "rh_in.csv")
        write_boards(rh_src, rh_in)
        geom = ["--rounds", 3, "--strip_width", 4, "--rotate", -1,
                "--hold_band", "--num_rows", 0, "--wall_time", S["RH_WALL"],
                "--threads", S["THREADS"], "--print_cmd"] + clue
        rh1 = os.path.join(run, "rh1")
        worst = max(worst, run_tool(
            ["bin/E555_roundhouse", SEED, rh_src, "--out_dir", rh1] + geom, logfh))
        rh_boards = [ln for p in manifest(rh1) for ln in read_boards(p)]
        if rh_boards:
            # The reverse spiral runs on the forward one's output, and BOTH are
            # carried forward -- pass 2 is not always an improvement on pass 1,
            # and the lineage guard is what picks between them.
            rh1_all = os.path.join(run, "rh1_all.csv")
            write_boards(rh1_all, rh_boards)
            rh2 = os.path.join(run, "rh2")
            worst = max(worst, run_tool(
                ["bin/E555_roundhouse", SEED, rh1_all, "--out_dir", rh2,
                 "--reverse"] + geom, logfh))
            rh_boards += [ln for p in manifest(rh2) for ln in read_boards(p)]
    elif carried:
        logfh.write("!! nothing closed row 12, so the roundhouse has nothing to"
                    " spiral this iteration -- raise FOCUS_DEPTH or FOCUS_STALL\n")

    # 6. Final topper over everything, opening the top band to fill rows 13..15.
    # --locked_rows must be 0 here: the flag unsets what it locks, and with no
    # second pass to recover those rows a board would leave missing its top two.
    keep = spirals_worth_keeping(rh_boards, carried) + carried
    final_all = os.path.join(run, "final_all.csv")
    write_boards(final_all, keep)
    ranked = rank_boards([final_all], os.path.join(run, "final_ranked.csv"), logfh)
    final_in = os.path.join(run, "final_in.csv")
    write_boards(final_in, (ranked or keep)[:S["FINAL_TOP"]])
    final_out = os.path.join(run, "final_out.csv")
    if read_boards(final_in):
        worst = max(worst, run_tool(
            [sys.executable, TOPPER, SEED, final_in, final_out,
             "--side", "T", "--band_depth", 6, "--locked_rows", 0,
             "--num_rows", 0, "--time_limit", S["TOPPER_TIME"],
             "--stall_time", S["TOPPER_STALL"],
             "--threads", S["THREADS"]] + clue, logfh))
    return worst, (read_boards(final_out) or read_boards(final_in))


def ingest(boards, run, logfh):
    """Rank what production made, drop trivial siblings, cap at KEEP, and add
    it to the produced queue. clean_csv drops a board repeating an earlier one
    with a single frontier piece swapped -- a structural rule, not a similarity
    threshold, which is what stops the pool filling with siblings of one lucky
    board.

    Stage 3 already ran it, on a different population and for a different
    reason: there it thins the beam's row-11 output so the focused topper does
    not solve the same model twice. Here the population is the whole queue,
    where a near-complete board has almost no frontier left and what it mostly
    removes is an exact repeat."""
    if not boards:
        return 0
    fresh = os.path.join(run, "fresh.csv")
    pool = boards + read_boards(PRODUCED)
    write_boards(fresh, pool)
    ranked = rank_boards([fresh], os.path.join(run, "ranked.csv"), logfh)
    if not ranked:
        # Ranking failed. Keep the boards anyway, unordered, rather than
        # discarding a pass's work; the next ingest will sort them out.
        write_boards(PRODUCED, pool[:S["KEEP"]])
        return len(boards)
    cleaned = os.path.join(run, "cleaned.csv")
    if run_tool([sys.executable, os.path.join(TOOLS, "E555_clean_csv.py"),
                 os.path.join(run, "ranked.csv"), "--out", cleaned], logfh) == 0:
        ranked = read_boards(cleaned) or ranked
    write_boards(PRODUCED, ranked[:S["KEEP"]])
    return len(boards)


# ---- refinement and harvest -------------------------------------------------

def ender(inputs, out_path, run, logfh, deep):
    """One or two ender passes. The ender NEVER returns a board worse than its
    input -- an explicit never-worse guard, lexicographic (clues before breaks)
    when clues are held -- so a generous budget carries no risk, and chaining
    ring into patch is free."""
    if not inputs:
        return 0, []
    src = os.path.join(run, "ender_in.csv")
    write_boards(src, inputs)
    budget = S["HARVEST_TIME"] if deep else S["REFINE_TIME"]
    reach = S["HARVEST_REACH"] if deep else S["REFINE_REACH"]
    changes = S["HARVEST_CHANGES"] if deep else S["REFINE_CHANGES"]
    # Ring mode first: the topper leaves its damage on and near the border ring,
    # which is exactly what the ring sweep re-threads.
    modes = ["ring", "patch"] if deep else ["ring"]
    worst = 0
    cur = src
    for i, mode in enumerate(modes):
        dst = out_path if i == len(modes) - 1 else os.path.join(run, "ender_%s.csv" % mode)
        rc = run_tool([sys.executable, ENDER, SEED, cur, dst,
                       "--mode", mode, "--reach", reach, "--max_changes", changes,
                       "--num_rows", 0, "--time_limit", budget,
                       "--stall_time", max(10, budget // 3),
                       "--threads", S["THREADS"]] + CLUE_FLAGS[S["CLUES"]], logfh)
        worst = max(worst, rc)
        if not read_boards(dst):
            break
        cur = dst
    return worst, read_boards(cur)


def refine(run, logfh):
    """Give the best untouched boards one ender pass and move them on."""
    taken = take_top(PRODUCED, S["REFINE_TOP"])
    if not taken:
        return 0, 0
    rc, out = ender(taken, os.path.join(run, "refined_new.csv"), run, logfh, deep=False)
    # The result replaces its input: never-worse means the endered board
    # dominates the one it came from.
    merged = os.path.join(run, "refined_all.csv")
    write_boards(merged, (out or taken) + read_boards(REFINED))
    ranked = rank_boards([merged], os.path.join(run, "refined_ranked.csv"), logfh)
    write_boards(REFINED, ranked or (out or taken))
    return rc, len(taken)


def harvest(run, logfh):
    """Deep-refine the best refined boards and promote them to champions,
    where nothing will search them again."""
    taken = take_top(REFINED, S["HARVEST_TOP"])
    if not taken:
        return 0, 0
    rc, out = ender(taken, os.path.join(run, "harvest_new.csv"), run, logfh, deep=True)
    merged = os.path.join(run, "champions_all.csv")
    write_boards(merged, (out or taken) + read_boards(CHAMPIONS))
    ranked = rank_boards([merged], os.path.join(run, "champions_ranked.csv"), logfh)
    write_boards(CHAMPIONS, ranked or (out or taken))
    return rc, len(taken)


# ---- state ------------------------------------------------------------------

def load_state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except Exception:
        return {"iteration": 0, "produced": 0, "failed": 0, "best": 0}


def save_state(st):
    tmp = STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(st, f, indent=1)
    os.replace(tmp, STATE)


def note(msg):
    print("[farm] " + msg, flush=True)
    with open(LOG, "a") as f:
        f.write("[farm] %s %s\n" % (time.strftime("%F %T"), msg))


# ---- main -------------------------------------------------------------------

def preflight():
    if not os.path.isdir(BIN) or not os.path.isdir(TOOLS):
        die("REPO=%s is not an E555 checkout -- pass REPO=/path/to/E555" % REPO)
    if not os.path.exists(os.path.join(BIN, "E555_beamer")):
        subprocess.run(["make"], cwd=REPO)
    if not os.path.exists(SEED):
        die("seed file not found: " + SEED)
    if S["CLUES"] not in CLUE_FLAGS:
        die("CLUES must be none, center or all (got: %s)" % S["CLUES"])
    if S["FOCUS_DEPTH"] <= 3:
        die("FOCUS_DEPTH must exceed 3: --locked_rows 3 already takes rows "
            "13..15, so a shallower band leaves the solver nothing to fill")
    # Both CP-SAT stages need ortools, and without it every single iteration
    # would die in the same place hours after nobody was watching.
    if subprocess.run([sys.executable, "-c", "import ortools"],
                      capture_output=True).returncode != 0:
        die("ortools is not installed, so the topper and the ender would fail\n"
            "        in every iteration. Install it with:  pip install ortools")
    # --wall_time covers the beamer's whole run, database build included. With
    # no cache that build is 90-180s, so a small BEAM_WALL is silently spent
    # entirely on it and every iteration reports 0 boards while failing nothing.
    if not DB_FILE and S["BEAM_WALL"] < 240:
        note("warning: BEAM_WALL=%d with no DB_FILE. The beamer's budget covers"
             " building the 6.4 GB chain database (90-180s), so little or none"
             " of it will reach the search." % S["BEAM_WALL"])
    if DB_FILE:
        d = os.path.dirname(DB_FILE)
        os.makedirs(d, exist_ok=True)
        if not os.access(d, os.W_OK):
            die("DB_FILE directory is not writable: " + d)
        free = shutil.disk_usage(d).free / 1e9
        if free < 7:
            note("warning: only %.1fG free on %s; the chain database needs ~6.5G"
                 % (free, d))


def main():
    parse_args(sys.argv[1:])
    globals().update(resolve_paths())
    # Directories first: preflight's low-disk warning goes to farm.log.
    for d in (os.path.join(FARM_DIR, "queue"), os.path.join(FARM_DIR, "records")):
        os.makedirs(d, exist_ok=True)
    preflight()
    for stale in os.listdir(FARM_DIR):        # whatever a killed farm left
        if stale.startswith("run_"):
            shutil.rmtree(os.path.join(FARM_DIR, stale), ignore_errors=True)

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    st = load_state()
    st["best"] = max(st.get("best", 0), best_pool()[0])
    consec = 0
    started = time.time()
    note("%s, %d threads, clues=%s, db=%s"
         % (FARM_DIR, S["THREADS"], S["CLUES"], DB_FILE or "<in memory>"))
    note("best so far: %d/480. One line per iteration; the detail of each is in"
         " its own log, kept only for records and failures." % st["best"])

    while not STOP:
        if S["MAX_HOURS"] and time.time() - started >= S["MAX_HOURS"] * 3600:
            note("MAX_HOURS reached, stopping.")
            break
        st["iteration"] += 1
        it = st["iteration"]
        run = os.path.join(FARM_DIR, "run_%d" % it)
        shutil.rmtree(run, ignore_errors=True)
        os.makedirs(run)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        t0 = time.time()

        # Every tool's output goes here rather than to the farm's stdout. Days
        # of beam and CP-SAT telemetry is gigabytes; kept per iteration and
        # dropped with the run directory it costs nothing, and it is still
        # there when a record needs explaining.
        logpath = os.path.join(run, "iter.log")
        with open(logpath, "w") as logfh:
            st["produced"] += 1
            # Any value at or below zero means never. A bare `% ANNEAL_EVERY`
            # would make -4 behave as "every fourth" -- Python's modulo of a
            # negative divisor still hits zero every fourth step -- which is
            # the opposite of what anyone typing a negative intends.
            annealed = (S["ANNEAL_EVERY"] > 0
                        and st["produced"] % S["ANNEAL_EVERY"] == 0)
            what = "produce " + ("annealed" if annealed else "random")
            rc, boards = produce(run, logfh, annealed)
            made = ingest(boards, run, logfh)

            rc2, refined = (0, 0) if STOP else refine(run, logfh)
            harvested = 0
            if not STOP and S["HARVEST_EVERY"] and it % S["HARVEST_EVERY"] == 0:
                rc3, harvested = harvest(run, logfh)
                rc2 = max(rc2, rc3)
            rc = max(rc, rc2)

        new, winner = best_pool()
        took, mins = int(time.time() - t0), int((time.time() - started) / 60)
        tail = "%d made, %d refined, %d to champions" % (made, refined, harvested)

        if rc != 0:
            st["failed"] += 1
            consec += 1
            if st["failed"] <= S["FAILED_KEEP"]:
                fdir = os.path.join(FARM_DIR, "failed")
                os.makedirs(fdir, exist_ok=True)
                shutil.copy(logpath, os.path.join(fdir, "iter%d-%s.log" % (it, stamp)))
            note("iter %d: %s FAILED (exit %d) after %ds, best still %d/480"
                 % (it, what, rc, took, st["best"]))
            if S["GIVE_UP_AFTER"] and consec >= S["GIVE_UP_AFTER"]:
                note("%d iterations in a row failed -- stopping. The setup is "
                     "broken, not unlucky: read %s/failed/ for the reason."
                     % (consec, FARM_DIR))
                break
            # A dead iteration costs no time; without this pause a broken setup
            # would retry itself thousands of times an hour.
            time.sleep(5)
        else:
            consec = 0
            if new > st["best"]:
                rec = os.path.join(FARM_DIR, "records", "best_%d_%s" % (new, stamp))
                write_boards(rec + ".csv", read_boards(winner)[:1])
                with open(logpath, "rb") as src, gzip.open(rec + ".log.gz", "wb") as dst:
                    shutil.copyfileobj(src, dst)
                note("iter %d: %s -> NEW RECORD %d/480 (was %d), %s in %ds, %dm total"
                     % (it, what, new, st["best"], tail, took, mins))
                st["best"] = new
            else:
                note("iter %d: %s -> %s, best still %d/480, %ds, %dm total"
                     % (it, what, tail, st["best"], took, mins))
        save_state(st)
        shutil.rmtree(run, ignore_errors=True)

    if STOP:
        note("stop requested; pools are written and consistent.")
    note("%d iterations (%d failed), best %d/480" % (st["iteration"], st["failed"], st["best"]))
    note("champions in %s" % CHAMPIONS)
    save_state(st)


def request_stop(_sig, _frame):
    """Finish the step we are on, write the pools, exit cleanly."""
    global STOP
    STOP = True
    print("\n[farm] stop requested -- finishing this iteration.", flush=True)


def resolve_paths():
    repo = S["REPO"] or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    farm = os.path.abspath(S["FARM_DIR"])
    db = S["DB_FILE"]
    if db:
        db = db if os.path.isabs(db) else os.path.join(farm, db)
        # A clued run leaves the clue pieces out of the chains, so its cache is
        # not interchangeable with an unclued one -- the beamer checks the
        # exclusion set and rebuilds on a mismatch. One file per setting means
        # switching CLUES between runs does not throw the other cache away.
        if S["CLUES"] != "none":
            db += "." + S["CLUES"]
    return {
        "REPO": repo,
        "BIN": os.path.join(repo, "bin"),
        "TOOLS": os.path.join(repo, "tools"),
        "SRC": os.path.join(repo, "src"),
        "TOPPER": os.path.join(repo, "src", "C_tail", "E555_topper.py"),
        "ENDER": os.path.join(repo, "src", "C_tail", "E555_ender.py"),
        "SEED": S["SEED"] if os.path.isabs(S["SEED"]) else os.path.join(repo, S["SEED"]),
        "FARM_DIR": farm,
        "DB_FILE": db,
        "PRODUCED": os.path.join(farm, "queue", "produced.csv"),
        "REFINED": os.path.join(farm, "queue", "refined.csv"),
        "CHAMPIONS": os.path.join(farm, "champions.csv"),
        "LOG": os.path.join(farm, "farm.log"),
        "STATE": os.path.join(farm, "farm_state.json"),
    }


if __name__ == "__main__":
    main()
