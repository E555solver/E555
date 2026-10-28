#!/usr/bin/env python3
"""
judge.py -- the official verdict on whirlpool chains and on the boards queued
for them, for the whirlpool kit (agent/whirlpool_agent_mode.md). It imports the
repository's own tools/E555_viewer.py, E555_rotate.py and E555_rank.py and reads
the repository's own data/seed_Edge5.txt, so editing the copies in a campaign's
lab cannot change a verdict. agent/whirlpool/harness.sh calls it; run it by hand
to check a file.

    python3 judge.py --boards queue.csv [--seen F] [--good_out F]
    python3 judge.py chains.csv --input board.csv [--frames_seen F]
                     [--whirls_out F.csv.gz] [--id_prefix P] [--mark_seen F]
    python3 judge.py --self_test

BEAMER BOARD (--boards): exactly matched, every piece legal on its cell (grey
sides exactly where the cell faces out), rows 0..T complete with T >= 10, and
the frame not complete (a board holding all 60 frame cells is a lap's board,
not the beamer's; the beamer may write part of the top border).
--seen drops the boards whose hash is listed in F, repeats in the queue too;
--good_out writes the boards that pass, in queue order, one per line.
    BOARDS total=840 valid=812 unseen=812 invalid=28 reasons=breaks:20,full_frame:8

CHAIN: the rows of a chains file whose id is <tag>_s<k>, k = 1..4, in any order
and any number of tags. s1 is the input board, unchanged; s(k+1) is s(k) after
one lap: turned a quarter turn the same way every lap (d = 1 clockwise or 3,
as tools/E555_rotate.py counts), then re-grown. Each s(k) is written the way it
was grown, rows 0..10 at least complete. Turned back into s1's orientation,
R(k) = s(k) turned (k-1)(4-d) quarter turns:
  - laps   = how many laps in a row, from s1, are valid: s(k+1) is exactly
             matched, legally framed, rows 0..10 complete, and keeps at least
             10 interior cells of R(k) in place (same piece, same spin) --
             the lineage, which tells a re-grown board from an unrelated one;
  - frame  = held when R(2) .. R(1+laps) hold the same complete 60-cell frame,
             cell for cell, and R(1) agrees with it on every frame cell it has;
  - jewel  = laps 3 with the frame held: one fixed border, grown to row 10 or
             beyond from all four sides. depth = the lowest of the four depths,
             a board's depth being the highest row r with rows 0..r complete.
The direction is whichever of 1 and 3 explains the chain best. One line per tag:
    CHAIN tag=t1 laps=3 frame=held jewel=yes dir=1 depth=10 depths=10,10,11,10 kept=50,48,52 frame_id=3f2a... new=yes archived=yes line=12
    CHAIN tag=t2 laps=1 frame=incomplete:s2 jewel=no dir=1 ... reason=s3:breaks=2
    JUDGE chains=2 best_laps=3 laps1=2 laps2=1 laps3=1 frame_held=1 jewels=1 new_frames=1 archived=1 first_jewel_line=12
line is the file line of the chain's last row, which the harness maps to a
time. frame_id is the frame's hash, the same whichever way the board is turned.
--frames_seen F counts a jewel as new only if its frame_id is not in F, and
appends the new ones. --mark_seen F appends the input board's hash (the
harness's record of what it has whirled).
--whirls_out F archives the completed whirlpools, as they were grown, before
any dive: the four boards of every chain with 3 laps, jewel or not, with ids
<id_prefix><tag>_s<k> and their matched-edge count as the score. Every new-frame
jewel is archived; of the other completed chains, at most --archive_max (50)
per call, jewels first, so a candidate that writes thousands of chains cannot
flood the campaign's history. A name ending in .gz is written gzip-compressed,
one gzip member per call appended to the file (zcat reads them all).

Exit status 0, also when nothing passes; 2 on bad usage or an unreadable file.
"""
from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True               # leave no __pycache__ in tools/
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import E555_viewer as V                      # noqa: E402  seed, rows, boards, stats
import E555_rotate as T                      # noqa: E402  quarter turns, frame rule
import E555_rank as R                        # noqa: E402  FRAME_SIDES

SIDE, N_PIECES, UNPLACED = V.SIDE, V.N_PIECES, V.UNPLACED
FRAME_CELLS = sorted(R.FRAME_SIDES)          # the 60 border cells
INTERIOR = [c for c in range(N_PIECES) if c not in R.FRAME_SIDES]
MIN_DEPTH = 10                               # rows 0..10 complete
MIN_KEPT = 10                                # interior cells a lap keeps of its parent
ID_RE = re.compile(r"^(.+)_s([1-4])$")


# -- one board ----------------------------------------------------------------

def cells(pos, rot):
    """{cell: (piece, spin)} of the placed pieces."""
    return {c: (p, rot[p]) for p, c in enumerate(pos) if c != UNPLACED}


def board_hash(pos, rot):
    """The board, independent of the spins written for unplaced pieces."""
    key = ",".join(f"{c}:{s}" for c, s in sorted(cells(pos, rot).items()))
    return hashlib.sha1(key.encode()).hexdigest()[:20]


def depth_of(placed):
    """Highest r with rows 0..r complete; -1 when row 0 is not."""
    r = -1
    while r + 1 < SIDE and all((r + 1) * SIDE + c in placed for c in range(SIDE)):
        r += 1
    return r


def check_board(pos, rot, seed):
    """(reason or "", depth, placed cells). Exact, legal on every cell."""
    try:
        board = V.build_board(pos, rot)
    except ValueError as exc:
        return f"malformed({exc})", -1, {}
    placed = cells(pos, rot)
    for cell, (pid, spin) in placed.items():
        if not T.frame_legal(cell, pid, spin, seed):
            return f"frame_rule:cell{cell}", -1, placed
    st = V.board_stats(board, seed)
    breaks = st["h_tot"] - st["h_ok"] + st["v_tot"] - st["v_ok"]
    if breaks:
        return f"breaks={breaks}", -1, placed
    return "", depth_of(placed), placed


def score_of(pos, rot, seed):
    st = V.board_stats(V.build_board(pos, rot), seed)
    return st["correct_edges"]


# -- frames -------------------------------------------------------------------

def frame_of(placed):
    return {c: placed[c] for c in FRAME_CELLS if c in placed}


def frame_id(frame):
    """Hash of a complete frame, the smallest over the four quarter turns."""
    keys = []
    for n in range(4):
        turned = sorted((T.rotate_cell(c, n), p, (s + 3 * n) % 4) for c, (p, s) in frame.items())
        keys.append(";".join(f"{c}:{p}:{s}" for c, p, s in turned))
    return hashlib.sha1(min(keys).encode()).hexdigest()[:16]


def compare_frames(frames):
    """frames[0] is R(1)'s (partial), frames[1:] R(2).. ; "held" or the first fault."""
    if len(frames) < 2:
        return "none"
    ref = frames[1]
    for k, fr in enumerate(frames[1:], start=2):
        if len(fr) != len(FRAME_CELLS):
            return f"incomplete:s{k}"
        if fr != ref:
            cell = next(c for c in FRAME_CELLS if fr[c] != ref[c])
            return f"differs:s{k}:cell{cell}"
    for c, ps in frames[0].items():
        if ref.get(c) != ps:
            return f"differs:s1:cell{c}"
    return "held"


# -- chains -------------------------------------------------------------------

def turned(pos, rot, n):
    return T.rotate_board(pos, rot, n % 4)


def judge_chain(boards, seed, inputs):
    """boards: {k: (pos, rot)} for one tag. Returns a dict of the verdict."""
    out = dict(laps=0, frame="none", jewel=False, dir=0, depths=[], kept=[],
               frame_id="", reason="")
    ks = sorted(boards)
    L = 0
    while L + 1 in boards:
        L += 1
    if L == 0:
        out["reason"] = "no_s1"
        return out
    if ks != list(range(1, L + 1)):
        out["reason"] = f"gap_after_s{L}"
    pos1, rot1 = boards[1]
    if board_hash(pos1, rot1) not in inputs:
        out["reason"] = "s1_is_not_the_input"
        return out
    checks = []
    for k in range(1, L + 1):
        reason, depth, placed = check_board(*boards[k], seed)
        if not reason and depth < MIN_DEPTH:
            reason = f"depth={depth}"
        checks.append((reason, depth, placed))
    if checks[0][0]:
        out["reason"] = f"s1:{checks[0][0]}"
        return out

    best = None
    for d in (1, 3):
        R_ = [cells(*turned(*boards[k], (k - 1) * (4 - d))) for k in range(1, L + 1)]
        laps, kept, reason = 0, [], ""
        for k in range(2, L + 1):
            if checks[k - 1][0]:
                reason = f"s{k}:{checks[k - 1][0]}"
                break
            same = sum(1 for c in INTERIOR if c in R_[k - 1] and R_[k - 2].get(c) == R_[k - 1][c])
            if same < MIN_KEPT:
                reason = f"s{k}:lineage_kept={same}"
                break
            laps += 1
            kept.append(same)
        frames = [frame_of(R_[k]) for k in range(laps + 1)]
        frame = compare_frames(frames)
        rank = (laps, frame == "held")
        if best is None or rank > best[0]:
            best = (rank, d, laps, kept, reason, frame, frames)
    _, d, laps, kept, reason, frame, frames = best
    out.update(laps=laps, kept=kept, dir=d, frame=frame,
               depths=[checks[k][1] for k in range(laps + 1)])
    if reason:
        out["reason"] = reason
    if laps == 3 and frame == "held":
        out["jewel"] = True
        out["frame_id"] = frame_id(frames[1])
    return out


def read_chains(path):
    """{tag: {k: (pos, rot)}}, {tag: last line}, and duplicate faults."""
    tags, last, faults = {}, {}, {}
    with open(path, newline="") as fh:
        reader = csv.reader(fh)
        for raw in reader:
            rec = V.parse_row(raw)
            if rec is None:
                continue
            cid, _sol, pos, rot = rec
            m = ID_RE.match(cid)
            if not m:
                continue
            tag, k = m.group(1), int(m.group(2))
            got = tags.setdefault(tag, {})
            if k in got and board_hash(*got[k]) != board_hash(pos, rot):
                faults[tag] = f"two_different_s{k}"
            got.setdefault(k, (pos, rot))
            last[tag] = reader.line_num
    return tags, last, faults


def read_lines(path):
    p = Path(path)
    return set(p.read_text().split()) if p.exists() else set()


def write_board(writer, cid, pos, rot, seed):
    writer.writerow([cid, score_of(pos, rot, seed)] + [str(v) for v in pos + rot])


def archive(path, prefix, chains, seed):
    """Append the four boards of each (tag, boards) to path, gzip if .gz."""
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "at", newline="") as fh:
        w = csv.writer(fh, lineterminator="\n")
        for tag, boards in chains:
            for k in range(1, 5):
                write_board(w, f"{prefix}{tag}_s{k}", *boards[k], seed)


def run_chains(args, seed):
    inputs = {board_hash(pos, rot) for _i, _c, _s, pos, rot in V.iter_records(args.input)}
    if not inputs:
        sys.exit(f"judge: no board in {args.input}")
    tags, last, faults = read_chains(args.chains) if Path(args.chains).exists() else ({}, {}, {})
    seen = read_lines(args.frames_seen) if args.frames_seen else set()
    counts = dict(chains=0, laps1=0, laps2=0, laps3=0, frame_held=0, jewels=0, new_frames=0)
    best_laps, first_jewel = 0, 0
    new_frames, results = [], []
    for tag in sorted(tags, key=lambda t: last[t]):
        v = judge_chain(tags[tag], seed, inputs)
        if tag in faults:
            v.update(laps=0, jewel=False, frame="none", reason=faults[tag])
        counts["chains"] += 1
        for n in (1, 2, 3):
            counts[f"laps{n}"] += v["laps"] >= n
        counts["frame_held"] += v["laps"] >= 1 and v["frame"] == "held"
        best_laps = max(best_laps, v["laps"])
        v["new"] = ""
        if v["jewel"]:
            counts["jewels"] += 1
            first_jewel = first_jewel or last[tag]
            v["new"] = "no" if v["frame_id"] in seen else "yes"
            if v["new"] == "yes":
                seen.add(v["frame_id"])
                new_frames.append(v["frame_id"])
                counts["new_frames"] += 1
        results.append((tag, v))
    # the archive: every new-frame jewel, then up to --archive_max other
    # completed chains, the jewels among them first
    done = {t: v for t, v in results if v["laps"] == 3}
    keep = {t for t, v in done.items() if v["new"] == "yes"}
    others = sorted((t for t in done if t not in keep), key=lambda t: not done[t]["jewel"])
    keep.update(others[:args.archive_max])
    for tag, v in results:
        depth = min(v["depths"]) if v["jewel"] else ""
        line = (f"CHAIN tag={tag} laps={v['laps']} frame={v['frame']} "
                f"jewel={'yes' if v['jewel'] else 'no'} dir={v['dir']} depth={depth} "
                f"depths={','.join(map(str, v['depths']))} kept={','.join(map(str, v['kept']))}")
        if v["jewel"]:
            line += f" frame_id={v['frame_id']} new={v['new']}"
        if v["laps"] == 3:
            line += f" archived={'yes' if args.whirls_out and tag in keep else 'no'}"
        line += f" line={last[tag]}"
        if v["reason"]:
            line += f" reason={v['reason']}"
        print(line)
    archived = [(t, tags[t]) for t, _v in results if t in keep] if args.whirls_out else []
    if archived:
        archive(args.whirls_out, args.id_prefix, archived, seed)
    if args.frames_seen and new_frames:
        with open(args.frames_seen, "a") as fh:
            fh.writelines(f + "\n" for f in new_frames)
    if args.mark_seen:
        with open(args.mark_seen, "a") as fh:
            fh.writelines(h + "\n" for h in sorted(inputs))
    print("JUDGE " + " ".join(f"{k}={v}" for k, v in [("chains", counts["chains"]),
          ("best_laps", best_laps), ("laps1", counts["laps1"]), ("laps2", counts["laps2"]),
          ("laps3", counts["laps3"]), ("frame_held", counts["frame_held"]),
          ("jewels", counts["jewels"]), ("new_frames", counts["new_frames"]),
          ("archived", len(archived)), ("first_jewel_line", first_jewel)]))


# -- queued boards ------------------------------------------------------------

def check_beamer_board(pos, rot, seed):
    reason, depth, placed = check_board(pos, rot, seed)
    if reason:
        return reason.split("=")[0].split("(")[0].split(":")[0]
    if depth < MIN_DEPTH:
        return "depth"
    if all(c in placed for c in FRAME_CELLS):
        return "full_frame"
    return ""


def run_boards(args, seed):
    seen = read_lines(args.seen) if args.seen else set()
    total = valid = unseen = 0
    reasons = {}
    out = open(args.good_out, "w", newline="") if args.good_out else None
    w = csv.writer(out, lineterminator="\n") if out else None
    with open(args.boards, newline="") as fh:
        for raw in csv.reader(fh):
            rec = V.parse_row(raw)
            if rec is None:
                continue
            total += 1
            _cid, _sol, pos, rot = rec
            reason = check_beamer_board(pos, rot, seed)
            if reason:
                reasons[reason] = reasons.get(reason, 0) + 1
                continue
            valid += 1
            h = board_hash(pos, rot)
            if h in seen:
                continue
            seen.add(h)
            unseen += 1
            if w:
                w.writerow([f.strip() for f in raw])
    if out:
        out.close()
    print(f"BOARDS total={total} valid={valid} unseen={unseen} invalid={total - valid} "
          f"reasons={','.join(f'{k}:{v}' for k, v in sorted(reasons.items())) or 'none'}")


# -- self test ----------------------------------------------------------------

def self_test():
    """Chains cut from the synthetic 480 solution: one jewel, and each fault."""
    seed = V.load_seed(REPO / "data" / "synth_seed.txt")
    _i, _c, _s, fpos, frot = next(V.iter_records(REPO / "data" / "synth_solution_480.csv"))

    def keep(pos, rot, rows=11, frame=True, drop=()):
        p = [c if c != UNPLACED and (c // SIDE < rows or (frame and c in R.FRAME_SIDES))
             and c not in drop else UNPLACED for c in pos]
        return p, list(rot)

    def lap_board(n, **kw):                  # the full board turned n times, cut
        return keep(*turned(fpos, frot, n), **kw)

    s1 = keep(fpos, frot, frame=False)
    good = {1: s1, 2: lap_board(1), 3: lap_board(2), 4: lap_board(3)}
    inputs = {board_hash(*s1)}
    fails = []

    def expect(name, chain, laps, frame, jewel, reason="", inp=None):
        v = judge_chain(chain, seed, inputs if inp is None else inp)
        ok = (v["laps"] == laps and v["frame"].startswith(frame) and v["jewel"] == jewel
              and v["reason"].startswith(reason))
        print(f"{'ok  ' if ok else 'FAIL'} {name}: laps={v['laps']} frame={v['frame']} "
              f"jewel={v['jewel']} dir={v['dir']} depths={v['depths']} kept={v['kept']} "
              f"reason={v['reason'] or '-'}")
        if not ok:
            fails.append(name)
        return v

    v = expect("jewel, clockwise", good, 3, "held", True)
    ccw = {1: s1, 2: lap_board(3), 3: lap_board(2), 4: lap_board(1)}
    w = expect("jewel, counter-clockwise", ccw, 3, "held", True)
    if v["frame_id"] != w["frame_id"]:
        print("FAIL the same frame got two frame ids")
        fails.append("frame_id")
    expect("mixed directions", {**good, 3: keep(fpos, frot)}, 1, "held", False, "s3:lineage")
    expect("lap board turned the wrong way", {**good, 3: lap_board(3)}, 1, "held", False,
           "s3:lineage")
    pos, rot = lap_board(1)
    a, b = [p for p, c in enumerate(pos) if c in (2 * SIDE + 3, 2 * SIDE + 4)]
    pos[a], pos[b] = pos[b], pos[a]
    expect("a break in s2", {**good, 2: (pos, rot)}, 0, "none", False, "s2:breaks")
    expect("a hole in rows 0..10 of s3", {**good, 3: lap_board(2, drop=(5 * SIDE + 5,))},
           1, "held", False, "s3:depth=4")
    expect("s1 is not the input", good, 0, "none", False, "s1_is_not_the_input",
           inp={"0" * 20})
    right_top = tuple(r * SIDE + 15 for r in range(11, 15))
    expect("s2 without its right column above row 10 (today's lap)",
           {**good, 2: lap_board(1, drop=right_top)}, 3, "incomplete:s2", False)
    expect("s4 missing a top-border cell", {**good, 4: lap_board(3, drop=(15 * SIDE + 7,))},
           3, "incomplete:s4", False)
    expect("two laps only", {k: good[k] for k in (1, 2, 3)}, 2, "held", False)
    fr = frame_of(cells(*good[2]))
    bad = dict(fr)
    c1, c2 = FRAME_CELLS[3], FRAME_CELLS[4]
    bad[c1], bad[c2] = fr[c2], fr[c1]
    got = compare_frames([{}, fr, bad, fr])
    print(f"{'ok  ' if got.startswith('differs:s3') else 'FAIL'} frame differs in s3: {got}")
    if not got.startswith("differs:s3"):
        fails.append("frame differs")
    got = compare_frames([{c1: fr[c2]}, fr, fr, fr])
    print(f"{'ok  ' if got.startswith('differs:s1') else 'FAIL'} s1 disagrees with the frame: {got}")
    if not got.startswith("differs:s1"):
        fails.append("s1 differs")
    q = [(s1, ""), (good[2], "full_frame"), (keep(fpos, frot, rows=10, frame=False), "depth"),
         ((pos, rot), "breaks")]
    corner = next(p for p in range(N_PIECES) if sum(e == 0 for e in seed[p]) == 2)
    lone = keep(fpos, frot, frame=False)[0]
    lone[corner] = 13 * SIDE + 7
    q.append(((lone, list(frot)), "frame_rule"))
    for (pos_, rot_), want in q:
        got = check_beamer_board(pos_, rot_, seed)
        print(f"{'ok  ' if got == want else 'FAIL'} queue board: {got or 'valid'} (want {want or 'valid'})")
        if got != want:
            fails.append(f"queue {want}")
    fails += archive_test(seed, s1, good, lap_board, right_top)
    print(f"SELF_TEST {'passed' if not fails else 'FAILED: ' + ', '.join(fails)}")
    return 1 if fails else 0


def archive_test(seed, s1, good, lap_board, right_top):
    """run_chains end to end: what --whirls_out keeps, compressed, across calls."""
    import contextlib, io, tempfile
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        rows = {"j1": good, "j2": good, "n1": {**good, 2: lap_board(1, drop=right_top)},
                "p1": {k: good[k] for k in (1, 2, 3)}}
        with open(tmp / "chains.csv", "w", newline="") as fh:
            w = csv.writer(fh, lineterminator="\n")
            for tag, chain in rows.items():
                for k, (pos, rot) in chain.items():
                    w.writerow([f"{tag}_s{k}", 0] + pos + rot)
        with open(tmp / "board.csv", "w", newline="") as fh:
            csv.writer(fh, lineterminator="\n").writerow(["b", 0] + s1[0] + s1[1])

        def call(frames, cap, prefix):
            a = argparse.Namespace(chains=str(tmp / "chains.csv"), input=str(tmp / "board.csv"),
                                   frames_seen=str(tmp / frames), whirls_out=str(tmp / "w.csv.gz"),
                                   archive_max=cap, id_prefix=prefix, mark_seen=None)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                run_chains(a, seed)
            return out.getvalue()

        def stored():
            with gzip.open(tmp / "w.csv.gz", "rt") as fh:
                return [V.parse_row(r)[0] for r in csv.reader(fh)]

        cases = [("all completed chains archived, the 2-lap one not", "f1.txt", 50, "A-",
                  ["A-j1", "A-j2", "A-n1"]),
                 ("a known frame and a cap of 0 archive nothing", "f1.txt", 0, "B-", []),
                 ("a cap of 0 still archives a new-frame jewel", "f2.txt", 0, "C-", ["C-j1"])]
        seen = []
        for name, frames, cap, prefix, want in cases:
            text = call(frames, cap, prefix)
            seen += [f"{t}_s{k}" for t in want for k in range(1, 5)]
            ok = stored() == seen and f"archived={len(want)}" in text
            print(f"{'ok  ' if ok else 'FAIL'} archive: {name} ({len(stored())} rows in w.csv.gz)")
            if not ok:
                fails.append(f"archive: {name}")
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("chains", nargs="?", help="a chains file (needs --input)")
    ap.add_argument("--input", help="the board the chains were whirled from")
    ap.add_argument("--boards", help="validate the boards of a queue file instead")
    ap.add_argument("--seen", help="--boards: drop the boards whose hash is in this file")
    ap.add_argument("--good_out", help="--boards: write the boards that pass here")
    ap.add_argument("--frames_seen", help="frame ids already counted; new ones are appended")
    ap.add_argument("--whirls_out", help="archive the completed whirlpools here (.gz: compressed)")
    ap.add_argument("--archive_max", type=int, default=50,
                    help="completed chains archived per call besides the new-frame jewels")
    ap.add_argument("--id_prefix", default="", help="prefix for the ids written to --whirls_out")
    ap.add_argument("--mark_seen", help="append the input board's hash here")
    ap.add_argument("--seed_file", default=str(REPO / "data" / "seed_Edge5.txt"))
    ap.add_argument("--self_test", action="store_true", help="run the built-in checks")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    seed = V.load_seed(args.seed_file)
    if args.boards:
        run_boards(args, seed)
    elif args.chains and args.input:
        run_chains(args, seed)
    else:
        ap.error("give --boards FILE, or CHAINS --input BOARD, or --self_test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
