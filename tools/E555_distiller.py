#!/usr/bin/env python3
"""
E555_distiller.py -- distil a corpus of boards to the few worth the CP-SAT tail.

    python3 tools/E555_distiller.py BOARDS.csv[.gz] ... [--top N] [--out FILE]

Reads board CSVs (plain or gzip; partial or complete; any stop rows) and writes

    FILE            N complete boards, canonical rows (id, score, pos, rot),
                    best first: the input for E555_ender.py
    FILE.plan.sh    the E555_ender.py command that takes them on
    FILE_work/      every stage's scores and boards; a rerun resumes there

N = --top (default 25), P = unique partial boards. Stages:

    0 read     parse every row; drop exact repeats; group partials by the
               number of placed cells
    1 screen   every unique partial: the best of 300 seeded dives
               (E555_diver --end_dive 300); keep K = min(ceil(P/2), 400 N),
               split over the groups in proportion to their size, closure
               breaking ties
    2 finish   the K kept: E555_diver --end_dive 20000 --end_polish 5000
    3 probe    the best 4 N complete boards: the ender's redive at fixed work,
               E555_diver --reopen auto --rounds 8 --copies 8 --end_dive 3000
               --end_polish 1000 --prior 1 --nogo 1 (never a worse board;
               boards carrying clue pieces skip it)
    4 select   best first by probed score, probe gain, finished score, screen
               score, closure; skip a board sharing more than 80% of its
               cells with a better one; keep N

Every diver call is seeded and keys each board's random streams on the board
itself, so the output depends on neither the thread count nor where a run was
interrupted. Calibration of the screen: PROJECT_E555.md, E555_distiller.py.
"""
from __future__ import annotations

import argparse
import array
import csv
import gzip
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import E555_viewer as V                     # seed loading, row parsing, clues
import E555_rank as R                       # near-duplicate filter

SIDE, N_PIECES, N_EDGES, UNPLACED = V.SIDE, V.N_PIECES, V.N_EDGES, V.UNPLACED
NORTH, EAST, SOUTH, WEST = R.NORTH, R.EAST, R.SOUTH, R.WEST

# Colour 0 is the frame and 1..5 face it from inside; only the interior colours
# 6..22 pair with each other, so only they enter closure.
INNER_MIN, N_INNER = 6, 17

RNG_SEED = "1"
SCREEN = ["--end_dive", "300"]
FINISH = ["--end_dive", "20000", "--end_polish", "5000"]
PROBE = ["--reopen", "auto", "--rounds", "8", "--copies", "8", "--end_dive", "3000",
         "--end_polish", "1000", "--prior", "1", "--nogo", "1"]
KEEP_FRACTION = 0.5         # screen: keep this share of the unique partials,
KEEP_PER_TOP = 400          # but at most this many per board asked for
PROBE_PER_TOP = 4
MAX_AGREE = 0.8             # select: max shared-cell fraction with a better board
CHUNK = {"screen": 5000, "finish": 500, "probe": 50}   # boards per diver call
# Seconds per board on one thread (measured on 4 threads, times 4): the report's
# time estimates.
COST = {"screen": 0.15, "finish": 3.0, "probe": 28.0}
VERSION = 2


# -- boards ------------------------------------------------------------------

def open_text(path):
    """A text handle on a plain or gzip CSV (sniffed, not guessed from the name)."""
    with open(path, "rb") as fh:
        gz = fh.read(2) == b"\x1f\x8b"
    return gzip.open(path, "rt", newline="") if gz else open(path, newline="")


def read_rows(paths):
    """Yield (file_index, line_number, id, pos, rot) for every board row.

    A row that parses but is not a board (a cell outside 0..255, two pieces on
    one cell, a rotation outside 0..3) is yielded with pos None."""
    for fi, path in enumerate(paths):
        with open_text(path) as fh:
            for line_no, fields in enumerate(csv.reader(fh), 1):
                rec = V.parse_row(fields)
                if rec is None:
                    continue
                cid, _sol, pos, rot = rec
                cells = [c for c in pos if c != UNPLACED]
                if (len(set(cells)) != len(cells) or any(not 0 <= c < N_PIECES for c in cells)
                        or any(not 0 <= r <= 3 for r in rot)):
                    pos = None
                yield fi, line_no, cid, pos, rot


def board_digest(pos, rot):
    """16 bytes identifying a board: the placed (cell, piece, spin) triples."""
    h = hashlib.blake2b(digest_size=16)
    for p in range(N_PIECES):
        if pos[p] != UNPLACED:
            h.update(bytes((pos[p], p & 0xFF, p >> 8, rot[p])))
    return h.digest()


def score(pos, rot, seed):
    """Matched internal edges of a complete board, out of 480."""
    at = {pos[p]: V.rotate_edges(seed[p], rot[p]) for p in range(N_PIECES)}
    n = 0
    for x in range(N_PIECES):
        r, c = divmod(x, SIDE)
        if c < SIDE - 1 and at[x][EAST] == at[x + 1][WEST]:
            n += 1
        if r < SIDE - 1 and at[x][NORTH] == at[x + SIDE][SOUTH]:
            n += 1
    return n


def closure(pos, rot, seed):
    """The beamer's --lambda_J objective (closure_raw() in E555_beamer.c) of a
    partial board; higher is better, 0 on a complete board.

    With R_c the interior half-edges of colour c in the unplaced pieces, D_c the
    interior faces of placed pieces that look at an empty cell, S_c = R_c - D_c:

        J = 1/2 sum_c S_c (log S_c - log mean S) + sum_c D_c (log R_c - log mean R)
    """
    placed = {}
    supply = [0] * N_INNER
    for pid, cell in enumerate(pos):
        if cell == UNPLACED:
            for col in seed[pid]:
                if col >= INNER_MIN:
                    supply[col - INNER_MIN] += 1
        else:
            placed[cell] = pid
    demand = [0] * N_INNER
    for r in range(SIDE):
        for c in range(SIDE):
            if r * SIDE + c in placed:
                continue
            for nr, nc, side in ((r + 1, c, SOUTH), (r - 1, c, NORTH),
                                 (r, c + 1, WEST), (r, c - 1, EAST)):
                if not (0 <= nr < SIDE and 0 <= nc < SIDE):
                    continue
                pid = placed.get(nr * SIDE + nc)
                if pid is None:
                    continue
                col = V.rotate_edges(seed[pid], rot[pid])[side]
                if col >= INNER_MIN:
                    demand[col - INNER_MIN] += 1
    free = [supply[k] - demand[k] for k in range(N_INNER)]
    sum_s, sum_r = sum(free), sum(supply)
    if sum_s <= 0 or sum_r <= 0:
        return 0.0
    log_sbar, log_rbar = math.log(sum_s / N_INNER), math.log(sum_r / N_INNER)
    conc = sum(s * (math.log(s) - log_sbar) for s in free if s > 0)
    dem = sum(demand[k] * (math.log(supply[k]) - log_rbar)
              for k in range(N_INNER) if demand[k] > 0 and supply[k] > 0)
    return 0.5 * conc + dem


def row_text(bid, sc, pos, rot):
    return ",".join([bid, str(sc)] + [str(v) for v in pos] + [str(v) for v in rot]) + "\n"


# -- the diver -----------------------------------------------------------------

class Diver:
    """bin/E555_diver on a stream of boards, in chunks, into a stage file.

    The stage file holds the diver's rows (id b<k>, score, pos, rot), or only
    "b<k>,score" for the screen, whose boards are not needed again. Boards a
    stage file already holds are skipped, so an interrupted stage resumes where
    it stopped and gives the same result."""

    def __init__(self, exe, seed_path, work):
        self.exe, self.seed, self.work = exe, seed_path, work

    def done(self, stage):
        """{k: (score, line or None)} of the boards a stage file already holds."""
        out = {}
        path = self.work / f"{stage}.csv"
        if path.exists():
            with open(path, newline="") as fh:
                for line in fh:
                    k, _, rest = line.partition(",")
                    if k.startswith("b"):
                        sc = int(rest.split(",", 1)[0])
                        out[int(k[1:])] = (sc, line if "," in rest else None)
        return out

    def run(self, stage, boards, flags, total, keep_lines=True):
        """Dive `boards` ((k, pos, rot) triples, any iterable); return
        {k: (score, line)}, line None when not kept. `total` sizes the report."""
        results = self.done(stage)
        todo = total - len(results)
        if todo <= 0:
            return results
        cmd = [str(self.exe), str(self.seed), "IN", "OUT", *flags,
               "--emit_score", "0", "--rng_seed", RNG_SEED]
        print(f"[{stage}] {todo} board(s): E555_diver {' '.join(cmd[4:])}"
              + (f" (resuming, {len(results)} done)" if results else ""), flush=True)
        t0, n_done, chunk = time.time(), 0, []
        with open(self.work / f"{stage}.csv", "a", newline="") as sink:
            def dive(chunk):
                with tempfile.TemporaryDirectory(dir=self.work) as tmp:
                    src, dst = Path(tmp) / "in.csv", Path(tmp) / "out.csv"
                    with open(src, "w") as fh:
                        for k, pos, rot in chunk:
                            fh.write(row_text(f"b{k}", 0, pos, rot))
                    cmd[2], cmd[3] = str(src), str(dst)
                    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.PIPE)
                    if proc.returncode != 0 or not dst.exists():
                        tail = proc.stderr.decode(errors="replace").strip().splitlines()[-1:]
                        raise SystemExit(f"[{stage}] {self.exe} failed: "
                                         f"{'; '.join(tail) or proc.returncode}")
                    with open(dst, newline="") as fh:
                        for line in fh:
                            k, _, rest = line.partition(",")
                            if not k.startswith("b"):
                                continue
                            sc = int(rest.split(",", 1)[0])
                            results[int(k[1:])] = (sc, line if keep_lines else None)
                            sink.write(line if keep_lines else f"{k},{sc}\n")
                sink.flush()

            for b in boards:
                if b[0] in results:
                    continue
                chunk.append(b)
                if len(chunk) == CHUNK[stage]:
                    dive(chunk)
                    n_done += len(chunk)
                    chunk = []
                    rate = n_done / max(time.time() - t0, 1e-9)
                    print(f"[{stage}] {n_done}/{todo} ({rate:.1f} boards/s, "
                          f"{(todo - n_done) / rate / 60:.0f} min left)", flush=True)
            if chunk:
                dive(chunk)
        return results


# -- the pipeline ----------------------------------------------------------------

def manifest_of(paths, seed_path, top):
    return {"version": VERSION, "top": top, "seed": str(Path(seed_path).resolve()),
            "inputs": [[str(Path(p).resolve()), os.path.getsize(p), int(os.path.getmtime(p))]
                       for p in paths]}


def prepare_work(work, manifest):
    """Reuse the work directory of the same run; start afresh otherwise."""
    work.mkdir(parents=True, exist_ok=True)
    mpath = work / "manifest.json"
    if mpath.exists():
        try:
            if json.loads(mpath.read_text()) == manifest:
                return True
        except ValueError:
            pass
    for stage in CHUNK:
        (work / f"{stage}.csv").unlink(missing_ok=True)
    mpath.write_text(json.dumps(manifest, indent=1))
    return False


def main():
    ap = argparse.ArgumentParser(
        description="Distil board CSVs (plain or gzip, partial or complete) to the "
                    "few worth the CP-SAT tail: screen with seeded dives, finish, "
                    "probe with the ender's redive, keep the best N.")
    ap.add_argument("inputs", nargs="+", help="board CSVs, plain or gzip")
    ap.add_argument("--top", type=int, default=25, help="boards to keep (default 25)")
    ap.add_argument("--out", default="distilled.csv",
                    help="output CSV (default distilled.csv); FILE.plan.sh and "
                         "FILE_work/ are written next to it")
    ap.add_argument("--seed_file", help="piece seed file (default data/seed_Edge5.txt)")
    args = ap.parse_args()
    if args.top < 1:
        raise SystemExit("--top must be at least 1")

    root = Path(__file__).resolve().parent.parent
    seed_path = Path(V.find_seed(args.seed_file)).resolve()
    seed = V.load_seed(seed_path)
    exe = root / "bin" / "E555_diver"
    if not exe.exists():
        raise SystemExit(f"{exe} not found: build it with `make diver` in {root}")
    out = Path(args.out)
    work = out.with_name(out.stem + "_work")
    resumed = prepare_work(work, manifest_of(args.inputs, seed_path, args.top))
    diver = Diver(exe, seed_path, work)
    threads = os.cpu_count() or 1
    t_start = time.time()

    # -- 0 read ------------------------------------------------------------------
    names = [Path(p).name for p in args.inputs]
    rows = malformed = repeats = 0
    seen = set()
    meta = []                    # per unique board: [file, line, id, placed, closure]
    slot = array.array("l")      # per board row: its unique index, or -1
    groups = {}
    for fi, line_no, cid, pos, rot in read_rows(args.inputs):
        rows += 1
        if pos is None:
            malformed += 1
            slot.append(-1)
            continue
        d = board_digest(pos, rot)
        if d in seen:
            repeats += 1
            slot.append(-1)
            continue
        seen.add(d)
        n = sum(c != UNPLACED for c in pos)
        clo = closure(pos, rot, seed) if n < N_PIECES else 0.0
        slot.append(len(meta))
        meta.append([fi, line_no, cid, n, clo])
        groups[n] = groups.get(n, 0) + 1
    del seen
    unique = len(meta)
    if not unique:
        raise SystemExit("[read] no board rows in the input")
    n_part = sum(v for g, v in groups.items() if g < N_PIECES)
    print(f"[read] {rows} board row(s): {malformed} malformed, {repeats} exact repeat(s), "
          f"{unique} unique -- " + ", ".join(
              f"{v} with {g} placed" if g < N_PIECES else f"{v} complete"
              for g, v in sorted(groups.items())), flush=True)
    keep = min(math.ceil(n_part * KEEP_FRACTION), KEEP_PER_TOP * args.top)
    n_probe = PROBE_PER_TOP * args.top
    est = (n_part * COST["screen"] + keep * COST["finish"]
           + n_probe * COST["probe"]) / threads / 60
    print(f"[plan] screen {n_part}, finish {keep}, probe {n_probe}, keep {args.top}: "
          f"about {est:.0f} min on {threads} thread(s)"
          + ("  (resuming)" if resumed else ""), flush=True)

    def boards_where(pick):
        """(k, pos, rot) of the unique boards `pick(k)` selects, in input order."""
        seq = 0
        for _fi, _ln, _cid, pos, rot in read_rows(args.inputs):
            k = slot[seq]
            seq += 1
            if k >= 0 and pick(k):
                yield k, pos, rot

    # -- 1 screen ------------------------------------------------------------------
    finished = {}                # k -> (score, line) of a complete board
    screen = {}
    if n_part:
        part = [k for k in range(unique) if meta[k][3] < N_PIECES]
        screen = {k: s for k, (s, _) in diver.run(
            "screen", boards_where(lambda k: meta[k][3] < N_PIECES), SCREEN, n_part,
            keep_lines=False).items()}
        if len(screen) < n_part:
            print(f"[screen] {n_part - len(screen)} board(s) the diver could not finish "
                  f"(corners, edges and inner pieces do not balance) are dropped", flush=True)
        # Keep K over the groups in proportion to their size, best screen first.
        chosen = set()
        for g, size in groups.items():
            if g == N_PIECES:
                continue
            quota = max(1, round(keep * size / n_part))
            members = sorted((k for k in part if meta[k][3] == g and k in screen),
                             key=lambda k: (-screen[k], -meta[k][4], k))
            chosen.update(members[:quota])
        print(f"[screen] kept {len(chosen)} of {n_part} (best of 300 dives; "
              f"top score {max(screen.values(), default=0)})", flush=True)
        # -- 2 finish --------------------------------------------------------------
        finished = diver.run("finish", boards_where(lambda k: k in chosen), FINISH, len(chosen))
        print(f"[finish] {len(finished)} board(s); best "
              f"{max((s for s, _ in finished.values()), default='-')}", flush=True)
    # Complete input boards join here, as they are.
    if N_PIECES in groups:
        for k, pos, rot in boards_where(lambda k: meta[k][3] == N_PIECES):
            sc = score(pos, rot, seed)
            finished[k] = (sc, row_text(f"b{k}", sc, pos, rot))

    # -- 3 probe -------------------------------------------------------------------
    def rank_key(k):
        return (-finished[k][0], -screen.get(k, 0), -meta[k][4], k)
    cands = sorted(finished, key=rank_key)[:n_probe]
    if not cands:
        raise SystemExit("[select] no board could be finished: nothing to write")
    parsed = {}
    for k in cands:
        _, _, pos, rot = V.parse_row(next(csv.reader([finished[k][1]])))
        parsed[k] = (pos, rot)
    clued = {k for k in cands if V.clue_orient(*parsed[k])[1] > 0}
    probed = dict((k, finished[k]) for k in clued)
    probed.update(diver.run("probe", [(k, *parsed[k]) for k in cands if k not in clued],
                            PROBE, len(cands) - len(clued)))
    gained = sum(probed[k][0] > finished[k][0] for k in cands if k in probed)
    print(f"[probe] {len(cands)} board(s), {gained} improved"
          + (f", {len(clued)} carrying clue pieces left as they were" if clued else ""),
          flush=True)

    # -- 4 select -------------------------------------------------------------------
    order = sorted((k for k in cands if k in probed),
                   key=lambda k: (-probed[k][0], -(probed[k][0] - finished[k][0]),
                                  -finished[k][0], -screen.get(k, 0), -meta[k][4], k))
    recs = [dict(k=k, line=probed[k][1]) for k in order]
    kept = R.filter_max_agree(recs, MAX_AGREE)[:args.top]

    # -- outputs ---------------------------------------------------------------------
    with open(out, "w", newline="") as fh, open(work / "summary.csv", "w", newline="") as sm:
        w = csv.writer(fh, lineterminator="\n")
        s = csv.writer(sm, lineterminator="\n")
        s.writerow(["rank", "id", "file", "line", "placed", "closure", "screen",
                    "finished", "probed"])
        print(f"\n{'rank':>4}  {'id':<22}{'from':<32}{'screen':>7}{'finish':>7}{'probe':>7}")
        for rank, rec in enumerate(kept, 1):
            k = rec["k"]
            fi, line_no, cid, placed, clo = meta[k]
            _, _, pos, rot = V.parse_row(next(csv.reader([probed[k][1]])))
            w.writerow([cid, probed[k][0]] + pos + rot)
            src = f"{names[fi]}:{line_no}"
            sc = screen.get(k, "-")
            s.writerow([rank, cid, names[fi], line_no, placed, f"{clo:.3f}", sc,
                        finished[k][0], probed[k][0]])
            print(f"{rank:>4}  {cid[-21:]:<22}{src[-31:]:<32}{sc:>7}{finished[k][0]:>7}"
                  f"{probed[k][0]:>7}")
    clue_flags = ""
    if any(V.clue_orient(*parsed[r["k"]])[1] >= 4 for r in kept):
        clue_flags = " --clue_center --clue_corners"
    plan = out.with_name(out.stem + ".plan.sh")
    closed = out.with_name(out.stem + "_closed.csv")
    reopened = out.with_name(out.stem + "_reopened.csv")
    plan.write_text(
        "#!/usr/bin/env bash\n"
        f"# The {len(kept)} distilled boards through the CP-SAT closer (E555_ender.py,\n"
        "# profile deep: about 15 min a board).\n"
        "set -euo pipefail\n"
        f'ROOT="{root}"\n'
        f'SEED="{seed_path}"\n'
        f'python3 "$ROOT/src/C_tail/E555_ender.py" "$SEED" "{out.resolve()}" '
        f'"{closed.resolve()}" \\\n'
        f'    --profile deep --threads "$(nproc)"{clue_flags}\n'
        "# Without OR-Tools, the ender's redive alone:\n"
        f'#   bash "$ROOT/examples/11_diver_reopen.sh" BOARDS="{out.resolve()}" '
        f'OUT="{reopened.resolve()}" SECONDS_PER_BOARD=900 THREADS="$(nproc)"\n')
    plan.chmod(0o755)
    print(f"\n[out] {out} ({len(kept)} boards), {plan}, {work}/ "
          f"-- {(time.time() - t_start) / 60:.1f} min")
    return 0


if __name__ == "__main__":
    sys.exit(main())
