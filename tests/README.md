# tests/

Two unrelated things live here.

**`run_tests.sh`** is the release gate: build, every tool on the synthetic
fixture, a regression against a known 480/480 solution, and the static checks
(`check_script_flags.py`, `check_fixedframe.py`). Run it before you push.

Everything else is **the fixed-frame experiment**, described below. It is not
part of the pipeline, not built by `make all`, and nothing in `src/` depends on
it.

---

## The fixed-frame experiment

### The question

The frame pins four corner pieces and four corner clues, and that is a lot of
constraint: only a few pieces can sit beside a given corner piece and still leave
room for its clue two cells in. So placement near a corner is nowhere near
uniform — and it differs between the four corners, because they carry different
pieces and different clues.

So: **which pieces belong near which corner?** And, since the beamer grows
bottom-up and dies attempting row 12, can the pieces that belong on the far side
be taken out of its way with `--exclude_pieces`?

### Why it needs its own beamer

The production beamer picks its own border, its own corners, and hedges across
all four clue orientations at once. Every run therefore lives in its own frame,
and boards from two runs cannot be compared, let alone pooled.
`E555_beamer_FixedFrame.c` pins the frame instead — one orientation, one corner
assignment — so partials from separate runs share a coordinate system.

One side only sees an 11-row band, so the frame is attacked from all four sides
and each side's boards are turned back onto the canonical frame. Four bands, one
frame, whole board covered.

### Running it

```bash
make beamer_fixedframe                       # not part of `make all`
bash tests/run_fixedframe_farm.sh WALL=900   # four sides, ~1 hour total
python3 tests/E555_frame_stats.py  ff_out/corpus.csv --out_dir ff_out/stats
python3 tests/E555_frame_plots.py  ff_out/stats --out_dir ff_out/figs

# the part that decides anything
bash tests/run_fixedframe_ab.sh EXCLUDE=$(cat ff_out/stats/exclude_pieces.txt) \
     OUT_DIR=ab_out WALL=1800
python3 tests/E555_ab_analyze.py ab_out/baseline/run.log ab_out/excluded/run.log \
     --json_out ab_out/ab.json
python3 tests/E555_frame_plots.py  ff_out/stats --ab ab_out/ab.json --out_dir ff_out/figs
python3 tests/E555_frame_report.py ff_out/stats --figs ff_out/figs \
     --ab ab_out/ab.json --out report.html
```

The first run builds the 6.4 GB chain database (~80s) and caches it; the other
three sides mmap it in seconds.

### The files

| file | what it is |
|---|---|
| `E555_beamer_FixedFrame.c` | a copy of the Stage B beamer with the frame pinned. New flags: `--clue_orient` (required), `--canon_BL/BR/TL/TR`, `--exclude_pieces`, `--max_per_config`, `--emit_mode`. Deliberately a copy: the experiment must be free to diverge, and production must not move. |
| `run_fixedframe_farm.sh` | the four sides, then `tools/E555_rotate.py` on each, then one `corpus.csv`. Budgeted by wall clock per side, not by config count. |
| `E555_frame_stats.py` | zone and far-side affinity per piece: exposure-corrected lifts, bootstrap intervals over border configs, cross-side replication, a label-shuffle control, the colour mechanism. Writes `zones.csv`, `replication.csv`, `exclude_pieces.txt`, `summary.json`, `arrays.npz`. |
| `E555_frame_plots.py` | static matplotlib figures from `arrays.npz`. |
| `E555_frame_report.py` | figures + written explanation → one self-contained HTML report. |
| `run_fixedframe_ab.sh` | the practical test: the beamer twice, identical but for `--exclude_pieces`. Refuses a list that would overrun the piece budget. |
| `E555_ab_analyze.py` | depth reached per border, Wilson intervals per arm, Newcombe on the difference. |
| `check_fixedframe.py` | proves the canonicalisation map, as arithmetic. Part of the gate. |

### The zone partition, and why STOP_ROW is 10

The board is cut into 3×3 zones on the bands `[0-4] [5-10] [11-15]` — four corner
zones, four side zones, one centre. At `--stop_row 10` the four canonicalised
sides cover exactly

    side 0  rows 0..10        side 2  rows 5..15
    side 1  cols 5..15        side 3  cols 0..10

which are the **same cut points**. So every zone is wholly inside a side's band
or wholly outside it, never half-covered, and each corner zone is seen by exactly
two sides: `BL {0,3}`, `BR {0,1}`, `TL {2,3}`, `TR {1,2}`. Two independent views
of every corner is what makes a corner preference checkable rather than merely
assertable.

Exposure is the whole problem here. Side 0 cannot place anything above row 10, so
a naive count says every piece avoids the top — an artefact of where the beam was
pointed. Lifts are therefore computed per side over the zones that side covers
and pooled afterwards, with per-kind denominators (an edge piece is compared
against edge pieces, never against inner ones) and pinned pieces kept out of the
baseline.

### The piece budget, which bounds the whole idea

Rows 1..14 hold 14×14 = 196 inner cells and the puzzle has **exactly 196 inner
pieces**. There are no spares. A beam to row R places R×14 of them and two more
are reserved for the row-13 clues, so

    slack = 194 − R×14      R=10 → 54,  R=11 → 40,  R=12 → 26

Excluding K pieces leaves `slack − K`. Push K toward the slack and the run
collapses however good the statistics are. `E555_frame_stats.py` caps its
suggestion at half the slack and `run_fixedframe_ab.sh` refuses a list that would
overrun it.

### The three things that keep it honest

None is a theory; all three are cheap, and without them the numbers would be
decoration.

**Borders are the observations, not boards.** Every board a config emits
descends from one border, shared exactly. They are one observation with
variations. Each board is weighted `1/(boards from its config)` and the reported
**N_eff** (Kish) is the honest corpus size — far below the board count, which is
the point. So the beamer defaults to breadth: `--samples 0`, `--top_columns 1`,
`--beam_width 100000`, `--max_per_config 32`, and `--emit_mode sample` draws
uniformly from the survivors rather than taking the score-ordered head (the head
of a beam is its most correlated slice).

**The sides are a built-in replication.** Two sides see each corner, with
different RNG, different borders and a different search direction, so they share
very little of the heuristic's bias. If they rank the pieces the same way,
something about the puzzle is driving it; if not, the ranking is noise and no
further analysis rescues it. That Spearman correlation is the headline. The five
pinned clue pieces are excluded from every ranking — they sit at the same cell in
every board and would agree perfectly for a trivial reason — and used instead as
a positive control: the machinery must recover them at their own zones.

**A label-shuffle control calibrates the counts.** "N pieces have a preference"
means nothing until you know what the same procedure returns with nothing to
find. Shuffling piece labels within each board, among pieces of the same kind,
keeps every board's shape and per-kind counts and moves only the labels. The
shuffle must be independent *per config*: one global permutation merely renames
the pieces, leaves every correlation and count exactly as observed, and produces
a null identical to the result it is supposed to be testing.

### The canonicalisation map

Orientation `O` means "our row 0 is the published board turned `O` quarter-turns
clockwise", so a board searched at side `O` is put back on the canonical frame by
`(4 - O) % 4` **clockwise** turns.

| side | pin piece 3 at | canonicalise with |
|---|---|---|
| 0 | BL | `E555_rotate.py FILE 0` |
| 1 | TL | `E555_rotate.py FILE 3` |
| 2 | TR | `E555_rotate.py FILE 2` |
| 3 | BR | `E555_rotate.py FILE 1` |

After the turn, the centre clue (piece 138) sits at row 7, col 7, spin 0 in every
board of every side. `check_fixedframe.py` proves this from the clue table and
re-checks it against a corpus when given one.

Only the two *bottom* corners are ever placed — rows 12..15 stay empty — so the
TL/TR pins merely reserve their pieces. Each canonical corner is physically
placed in two of the four sides.

### The corner bet

The clue set is published and fixed. The corner assignment is **not**: no clue
pins a corner, so the four corner pieces can fill the four corners 4! = 24 ways
and only one of them is the solution's. The default (`BL=3 BR=2 TL=0 TR=1`) is
one of those 24, chosen arbitrarily and then held fixed. It conditions everything
near the border heavily and the core barely — which is where the interesting
variation is anyway. Change all four together with the `--canon_*` flags.

### What settles it

Not the statistics. The exclusion list is specific to *this* frame, so test it
with this beamer, one row deeper, with and without:

```bash
bin/E555_beamer_FixedFrame data/seed_Edge5.txt --clue_orient 0 --stop_row 12 \
    --db_file excl.db --wall_time 1800 \
    --exclude_pieces $(cat ff_out/stats/exclude_pieces.txt)
```

Count `filled=12` lines in each log. That number decides whether any of this was
worth doing.

### What settles it

Not the statistics. The exclusion list is specific to *this* frame, so test it
with this beamer, one row deeper, with and without — `run_fixedframe_ab.sh` does
exactly that and `E555_ab_analyze.py` reports P(reach depth ≥ D) for each arm
with an interval on the difference. That number decides whether any of this was
worth doing.
