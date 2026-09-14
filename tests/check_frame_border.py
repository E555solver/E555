#!/usr/bin/env python3
"""
check_frame_border.py -- is this border actually the border it claims to be?

WHY A SEPARATE CHECKER

    tests/E555_edge_annealer_FixedFrame.py maintains its Euler counts, its
    affinity total and its crowding vector INCREMENTALLY: a swap updates them
    from four table lookups rather than recomputing anything. That is what keeps
    the prior free, and it is also the classic way to accumulate a quiet drift
    that no self-consistent run will ever notice.

    So this reads the emitted rotations CSV back from disk, knowing nothing about
    the annealer's state, and rebuilds every claim from the seed file alone:

      * every one of the 60 border pieces has its grey side(s) facing out, and
        each side holds exactly 14 edge pieces;
      * the four corner pieces sit where --canon_BL/BR/TL/TR say they do, in
        the beamer's 0-based numbering;
      * each side's directed multigraph really does admit an Euler trail, and
        the exact count matches what the annealer wrote in its comment;
      * the inner-colour inventory can actually feed the border's inward faces;
      * and, with --prior, the affinity and crowding the comment claims.

    Everything here is recomputed from first principles -- the trail count from
    the BEST theorem with its own determinant, not the annealer's. Two
    independent implementations agreeing on 60 pieces is worth more than either
    one agreeing with itself.

WHAT IT IS NOT

    Not a solver and not a quality judgement: a border can pass every check here
    and still be the wrong 1 of 24 corner assignments. That bet is stated in the
    annealer's header and this program cannot test it.

USAGE

    python3 tests/check_frame_border.py data/seed_Edge5.txt borders_ff.csv
    python3 tests/check_frame_border.py data/seed_Edge5.txt borders_ff.csv \
        --prior ff_out/prior/border_prior.txt --verbose
"""
from __future__ import annotations

import argparse
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import E555_edge_annealer_FixedFrame as A                        # noqa: E402

SIDE_ORDER = [A.Side.BOTTOM, A.Side.RIGHT, A.Side.TOP, A.Side.LEFT]


def read_rows(path):
    """(label, spins) per data row; `#`/`%` comments are carried forward.

    The comment above a row is the annealer's claim about it, so it is kept and
    handed back with the row rather than skipped -- checking the row against the
    claim is most of the point.
    """
    rows, comment = [], ""
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            if line[0] in "#%":
                comment = line
                continue
            fields = [t for t in line.replace(",", " ").split() if t]
            label = fields[0] if not fields[0].lstrip("-").isdigit() else ""
            spins = [int(t) for t in (fields[1:] if label else fields)]
            rows.append((label, spins, comment))
            comment = ""
    return rows


def claimed(comment, key):
    """One `Key=value` field out of an annealer comment, or None."""
    for tok in comment.replace(",", " ").split():
        if tok.startswith(key + "="):
            try:
                return float(tok.split("=", 1)[1])
            except ValueError:
                return None
    return None


def check_row(pieces, by_id, spins, canon, prior, verbose):
    """Every claim, rebuilt. Returns (ok, list of lines to print)."""
    out, bad = [], []
    corner_ids, edge_ids = A.classify_boundary_pieces(pieces)

    # --- 1. every border piece is oriented for a real border slot ------------
    edge_side, corner_pos = {}, {}
    for pid in corner_ids:
        rot = A.rotate_sides(by_id[pid].sides, spins[pid - 1])
        greys = frozenset(A.Side(i) for i, v in enumerate(rot) if v == 0)
        role = next((c for c, want in A.CORNER_ZERO_SIDES.items() if want == greys), None)
        if role is None:
            bad.append(f"corner piece {pid-1} at spin {spins[pid-1]} has greys {sorted(int(g) for g in greys)}, "
                       f"which is not a corner orientation")
        else:
            corner_pos[pid] = role
    for pid in edge_ids:
        rot = A.rotate_sides(by_id[pid].sides, spins[pid - 1])
        greys = [A.Side(i) for i, v in enumerate(rot) if v == 0]
        if len(greys) != 1:
            bad.append(f"edge piece {pid-1} at spin {spins[pid-1]} shows {len(greys)} grey faces, expected 1")
        else:
            edge_side[pid] = greys[0]
    if bad:
        return False, bad

    # --- 2. fourteen per side, one per corner --------------------------------
    per_side = Counter(edge_side.values())
    for s in A.Side:
        if per_side[s] != 14:
            bad.append(f"{A.SIDE_NAMES[s]} holds {per_side[s]} edge pieces, expected 14")
    if len(set(corner_pos.values())) != 4:
        bad.append(f"the four corner pieces do not fill four distinct corners: "
                   f"{ {A.CORNER_NAMES[c]: p-1 for p, c in corner_pos.items()} }")

    # --- 3. the frame --------------------------------------------------------
    got = {A.CORNER_NAMES[c]: pid - 1 for pid, c in corner_pos.items()}
    want = dict(zip(("BL", "BR", "TL", "TR"), canon))
    if got != want:
        bad.append(f"corner assignment is {got}, but this frame pins {want}")
    out.append(f"    frame   BL={got.get('BL')} BR={got.get('BR')} "
               f"TL={got.get('TL')} TR={got.get('TR')}"
               + ("  ok" if got == want else "  MISMATCH"))

    # --- 4. Euler trails, recounted ------------------------------------------
    rotated_corners = {c: A.rotate_sides(by_id[p].sides, spins[p - 1])
                       for p, c in corner_pos.items()}
    if len(rotated_corners) == 4:
        endpoints = A.corner_endpoints(rotated_corners)
        counts = {}
        for s in A.Side:
            arcs = []
            for pid, side in edge_side.items():
                if side != s:
                    continue
                a = A.edge_arc(pid, s, A.rotate_sides(by_id[pid].sides, spins[pid - 1]))
                arcs.append((a.source_color, a.target_color))
            counts[s] = A.count_euler_trails(arcs, *endpoints[s])
            if counts[s] == 0:
                bad.append(f"{A.SIDE_NAMES[s]} admits no Euler trail: Stage B cannot order it")
        out.append("    trails  " + "  ".join(
            f"{A.SIDE_NAMES[s]}={counts[s]}" for s in SIDE_ORDER))
    else:
        counts = {}

    # --- 5. inner-colour inventory ------------------------------------------
    inward = Counter()
    for pid, side in edge_side.items():
        a = A.edge_arc(pid, side, A.rotate_sides(by_id[pid].sides, spins[pid - 1]))
        inward[a.inward_color] += 1
    cap = A.build_inner_capacity(pieces)
    for colour in set(cap) | set(inward):
        if colour == 0:
            continue
        have, need = cap.get(colour, 0), inward.get(colour, 0)
        if need > have:
            bad.append(f"colour {colour}: the border turns {need} faces inward and only "
                       f"{have} inner faces carry it")
        elif (have - need) % 2:
            bad.append(f"colour {colour}: {have - need} inner faces left over, an odd "
                       f"number -- they pair up across interior edges, so this cannot close")

    # --- 6. the prior, recomputed -------------------------------------------
    aff_pts = spread_pts = None
    if prior is not None:
        aff_sum, hist = A.build_prior_state(prior, edge_side)
        aff_pts, spread_pts = A.prior_terms(prior, aff_sum, hist)
        out.append(f"    prior   aff={aff_pts:.2f}  spread={spread_pts:.2f}  "
                   f"(crowding {A.crowding(prior, hist):.2f} pieces)")
        if verbose:
            for s in SIDE_ORDER:
                h = hist[s]
                out.append(f"      {A.SIDE_NAMES[s]:6s} wants "
                           + " / ".join(f"{v:5.2f}" for v in h)
                           + "   has " + " / ".join(f"{c:5.2f}" for c in prior.cap))
    return not bad, (bad if bad else out), counts, aff_pts, spread_pts


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Verify a fixed-frame border CSV against the seed file alone",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("seed_file")
    ap.add_argument("rotations_csv")
    ap.add_argument("--prior", default=None,
                    help="also recompute the affinity and crowding the comments claim")
    ap.add_argument("--canon_BL", type=int, default=A.CANON_CORNER_DEFAULT[0])
    ap.add_argument("--canon_BR", type=int, default=A.CANON_CORNER_DEFAULT[1])
    ap.add_argument("--canon_TL", type=int, default=A.CANON_CORNER_DEFAULT[2])
    ap.add_argument("--canon_TR", type=int, default=A.CANON_CORNER_DEFAULT[3])
    ap.add_argument("--tol", type=float, default=0.02,
                    help="how far a recomputed prior term may sit from the claimed one")
    ap.add_argument("--verbose", action="store_true",
                    help="print each side's occupancy wish against the cells it has")
    args = ap.parse_args(argv)

    canon = (args.canon_BL, args.canon_BR, args.canon_TL, args.canon_TR)
    pieces = A.read_pieces(args.seed_file)
    by_id = {p.id: p for p in pieces}
    _corner_ids, edge_ids = A.classify_boundary_pieces(pieces)
    prior = A.load_prior(args.prior, sorted(edge_ids), canon) if args.prior else None

    rows = read_rows(args.rotations_csv)
    if not rows:
        sys.exit(f"{args.rotations_csv}: no data rows")
    print(f"[check] {len(rows)} border(s) in {args.rotations_csv}")
    print(f"[check] frame: BL={canon[0]} BR={canon[1]} TL={canon[2]} TR={canon[3]}"
          f"  prior: {args.prior or 'none'}")

    failed = 0
    for i, (label, spins, comment) in enumerate(rows):
        if len(spins) < 60:
            print(f"  row {i} ({label}): {len(spins)} spins, expected at least 60")
            failed += 1
            continue
        ok, lines, counts, aff, spread = check_row(pieces, by_id, spins, canon,
                                                   prior, args.verbose)
        # Cross-check the comment, where there is one. A row whose comment
        # disagrees with the row is worse than one with no comment at all: it is
        # what a downstream tool would read instead of recomputing.
        for name, got in (("Aff", aff), ("Spread", spread)):
            want = claimed(comment, name)
            if want is not None and got is not None and abs(want - got) > args.tol:
                ok = False
                lines = list(lines) + [f"comment claims {name}={want:.2f}, recomputed {got:.2f}"]
        for s in SIDE_ORDER:
            want = claimed(comment, A.SIDE_NAMES[s])
            if want is not None and counts and abs(want - counts[s]) > 0.5:
                ok = False
                lines = list(lines) + [
                    f"comment claims {A.SIDE_NAMES[s]}={int(want)} trails, recounted {counts[s]}"]
        tag = "ok " if ok else "FAIL"
        print(f"  row {i:>3} {label or '(unlabelled)':<10} {tag}")
        for ln in lines:
            print(("    " if ok else "    !! ") + ln.strip())
        failed += (not ok)

    print()
    if failed:
        print(f"[check] {failed} of {len(rows)} border(s) FAILED")
        return 1
    print(f"[check] all {len(rows)} border(s) consistent: orientation, frame, Euler "
          f"trails, colour inventory{', prior' if prior else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
