# tests/

Two unrelated things live here.

**`run_tests.sh`** is the release gate: build, every tool on the synthetic
fixture, a regression against a known 480/480 solution, and `check_script_flags.py`.
Run it before you push.

Everything else is **the fixed-frame experiment**, described below. It is a
self-contained side study: it has its own `Makefile`, it is not part of the
pipeline, not built by `make all`, not run by `run_tests.sh`, and nothing outside
this folder refers to it or depends on it.

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
make -C tests                                # self-contained; not part of `make all`
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
| `Makefile` | the experiment's own build (`make -C tests`). Separate from the root Makefile on purpose, so nothing outside `tests/` has to change. Links the **unmodified** `src/B_beam/E555_database.c`. |
| `E555_beamer_FixedFrame.c` | a copy of the Stage B beamer with the frame pinned. New flags: `--clue_orient` (required), `--canon_BL/BR/TL/TR`, `--exclude_pieces`, `--max_per_config`, `--emit_mode`. Deliberately a copy: the experiment must be free to diverge, and production must not move. |
| `run_fixedframe_farm.sh` | the four sides, then `tools/E555_rotate.py` on each, then one `corpus.csv`. Budgeted by wall clock per side, not by config count. |
| `E555_frame_stats.py` | zone and far-side affinity per piece: exposure-corrected lifts, bootstrap intervals over border configs, cross-side replication, a label-shuffle control, the colour mechanism. Writes `zones.csv`, `replication.csv`, `exclude_pieces.txt`, `summary.json`, `arrays.npz`. |
| `E555_frame_plots.py` | static matplotlib figures from `arrays.npz`. |
| `E555_frame_report.py` | figures + written explanation → one self-contained HTML report. |
| `run_fixedframe_ab.sh` | the practical test: the beamer twice, identical but for `--exclude_pieces`. Refuses a list that would overrun the piece budget. |
| `E555_ab_analyze.py` | depth reached per border, Wilson intervals per arm, Newcombe on the difference. |
| `check_fixedframe.py` | proves the canonicalisation map, as arithmetic, from the clue table alone. Run it by hand; it is not wired into `run_tests.sh`. |
| `E555_border_prior.py` | the Stage A half: reduces the corpus to "which of the 56 edge pieces belong on which side of the border", with bootstrap intervals, a cross-view replication check and a label-shuffle null. Writes `border_prior.txt` and `border_prior.json`. |
| `E555_edge_annealer_FixedFrame.py` | a copy of the Stage A annealer that pins the study's corner assignment and adds the measured prior to its Euler-trail objective. New flags: `--prior`, `--w_affinity`, `--w_spread`, `--canon_BL/BR/TL/TR`, `--pool`, `--fix_corners 3`. |
| `check_frame_border.py` | rebuilds every claim an emitted border makes — orientation, frame, Euler counts, colour inventory, prior fit — from the seed file alone, with its own determinant. The annealer keeps those numbers incrementally; this is what catches a drift. |
| `run_fixedframe_border.sh` | the whole Stage A loop and its A/B: prior → two border pools (with and without the prior) → verification → the beamer on each → `E555_ab_analyze.py`. |

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

### What settles it, and what it settled

Not the statistics. `run_fixedframe_ab.sh` runs the beamer twice in this frame,
identical but for `--exclude_pieces`, and `E555_ab_analyze.py` compares survival
and frontier width per row.

**It came out negative, decisively.** Excluding the 12 far-side pieces dropped
row-1 survival from 32.2 % to 7.2 % and left the row-10 frontier 0.03× as wide.

**The reason is arithmetic, not statistical, and it bounds the whole technique.**
A chain record holds five pieces and needs all five available, so the database
shrinks as roughly the *fifth power* of the surviving piece fraction:

    (1 − 12/194)⁵ = 0.73        predicted chain loss 27 %
    2,730,016,036 → 2,137,097,200   observed loss     22 %

A 6 % cut in pieces buys a 22 % cut in chains. The clue-pinned rows have the
fewest viable chains to begin with, which is exactly where the damage lands —
63,659 of the excluded arm's 64,210 borders died at `extinct(clue_row)`.

A control arm excluding 12 pieces the corpus calls *undecided* separates "wrong
pieces" from "wrong mechanism". See `tests/results/README.md` for the numbers.

**So if you try this again**: make it a score penalty rather than a ban (costs no
chains), or hand the prior to Stage C where there is no chain database, or use a
dose of two or three pieces — three costs ~7 % of the database against 22 % for
twelve.

---

## Feeding it back into Stage A

The study's prior could not be handed to Stage C: no board in `data/` is in this
frame, most satisfy zero clues, and a zone prior measured under one corner
assignment says nothing about a board under another. That blocker does not exist
at Stage A, and the reason is the whole argument for putting it there.

**The annealer builds the frame.** Pin the corners the study pinned and the table
applies verbatim — same corners, same clue orientation, same coordinates, nothing
to re-key. Stage A is also where the information is cheapest to use: choosing
which 14 edge pieces sit on each side costs nothing. The chain database is
identical either way, the piece budget is identical, and unlike `--exclude_pieces`
there is no throughput to lose.

### What Stage A actually decides, and what was measured

One thing: which of the 56 edge pieces goes on each side. So the prior is measured
on the border cells alone — row 0, row 15, col 0, col 15 — and each side is split
into three buckets on the same band cuts the zone study used, `[1-4] [5-10]
[11-14]`, sizes 4, 6, 4.

Those cuts do more than keep the two studies comparable. A study side at
`--stop_row 10` sees some border sides whole and others in part — side 0 fills the
bottom row completely but only rows 1–10 of the two columns — which at cell
granularity is a partial exposure that has to be corrected for. At bucket
granularity it disappears: every (study side, bucket) pair is covered whole or not
at all. Measured, not assumed; `E555_border_prior.py` prints the coverage table it
found.

| | cross-view Spearman | label-shuffled |
|---|---|---|
| side affinity | **+0.38** | −0.03 |
| within-side contrast (which END of the side) | **+0.65** | |

The views being compared use different borders, a different RNG stream and a
different search role — one side's bottom **row** is another's left **column** —
so this is not the beam agreeing with itself. It also disposes of the obvious
confound: if the pattern came from how bottom rows are sampled it would be the
same on all four sides, and instead adjacent sides correlate *negatively*
(BOTTOM vs LEFT −0.48), which is the anti-corner structure the zone study found.

The within-side contrast replicating better than the side aggregate is not a
surprise either. "Which end of this side" *is* the corner preference; averaging a
side's three buckets is exactly what throws it away.

**What did not work, and is therefore not in the objective.** Shrinking each piece
toward the affinity of the colour it turns inward — the obvious move, and the
study's own stated mechanism — makes cross-view agreement monotonically *worse*:
+0.381, +0.373, +0.363, +0.336, +0.316, +0.255 at blend 0.00 → 1.00. The colour is
just a coarser view of the same thing. The table is written out and nothing reads
it.

### The objective, in three parts

All three in the same 100-point unit, so the weights are a real trade-off and not
a units conversion:

- **trails** — what the original annealer optimises. Each side scored on how many
  decades its Euler-trail count sits from its target. This number is not a proxy
  for anything: a Stage B run on one of these borders prints
  `bottoms=360 ... left-cols=432`, which are the annealer's own `BOTTOM=360` and
  `LEFT=432`. The trail count **is** the size of Stage B's option space.
- **affinity** — 0 = dealing the pieces out at random, 100 = the best assignment
  the prior admits, found by an exact transportation DP rather than a bound.
- **spread** — a side has 4 cells at one end, 6 in the middle and 4 at the other,
  and its 14 pieces have opinions. Crowding is the transport distance between the
  two, in pieces. This is the term that could not be guessed: a side made entirely
  of pieces that all want the bottom-left end scores well on affinity and is
  useless, because only four of them can have it.

### Two things the weight sweep settled

6 restarts × 120k steps per setting.

| target_scale | w_affinity | trails | affinity | weakest side | sum of 4 sides |
|---|---|---|---|---|---|
| 250 | 0 | 88.0 | 11.6 | 192 | 1,980 |
| 250 | 3 | 84.7 | 24.7 | 288 | 2,304 |
| 1000 | 0 | 90.1 | 15.1 | 432 | 4,056 |
| **1000** | **3** | **94.3** | **26.6** | **648** | **4,704** |
| 4000 | 0 | 74.0 | 18.8 | 684 | 6,360 |
| 4000 | 3 | 84.2 | 34.7 | 912 | 8,112 |

**The prior and the trail count were never in conflict.** At `--target_scale 250`
adding the prior costs 3.3 trail points, which looks like a trade — but the target
term punishes overshoot as hard as shortfall, and the prior pushes sides *past* a
low target. At 1000 and 4000 the prior **buys** trail points: 94.3 against 90.1,
with more trails on every side as well. So the default here is 1000, not the 250
it was first tried at.

**Spread is nearly free and affinity is not.** At a fixed target, `--w_spread 3`
costs a few trail points and drags affinity up with it — a side whose pieces all
want the same end is also a side whose pieces came from the same corner — while
`--w_affinity` past 10 abandons the trail targets to chase a term that saturates
near 47 anyway. Defaults: `--w_affinity 3 --w_spread 1`.

### Running it

```bash
# the whole loop, including the A/B that decides whether it helps
bash tests/run_fixedframe_border.sh WALL=900 RESTARTS=24

# or by hand
python3 tests/E555_border_prior.py ff_out/corpus.csv --out_dir ff_out/prior
python3 -u tests/E555_edge_annealer_FixedFrame.py data/seed_Edge5.txt \
    --prior ff_out/prior/border_prior.txt --out borders_ff.csv \
    --restarts 24 --steps 200000 --threads 8
python3 tests/check_frame_border.py data/seed_Edge5.txt borders_ff.csv \
    --prior ff_out/prior/border_prior.txt
bin/E555_beamer_FixedFrame data/seed_Edge5.txt borders_ff.csv --clue_orient 0 ...
```

`E555_beamer_FixedFrame.c` gained one capability for this: **give it a rotations
CSV and it searches those borders** instead of sampling its own. The `--canon_*`
flags then flip meaning — they stop pinning the corners and start *checking* them,
and a row from a different frame is refused rather than searched. Without a
rotations file nothing changed: it samples borders and pins corners exactly as
before.

### What this does not test

The corner bet. Every border here, and every number in the prior, is conditional
on `BL=3 BR=2 TL=0 TR=1` being the solution's assignment — 1 of 4! = 24. Both arms
of the A/B pin the same four corners, so the comparison measures the prior and
nothing else; it cannot say whether the frame itself is the right one. Nothing in
this folder can.

---

## The run this was measured on, and how to pick it up again

`tests/results/` holds the whole study as shipped — **start with
[`results/report.html`](results/report.html)**, which is the argument and the
figures in one self-contained file. `results/README.md` describes every file and
the findings in detail.

### It regenerates from what is committed

`tests/results/` is not just output; it is a complete stats directory. The
figures and the report rebuild from it with no beamer run and no chain database:

```bash
python3 tests/E555_frame_plots.py tests/results \
    --ab tests/results/data/ab_farside.json --out_dir tests/results/figs

python3 tests/E555_frame_report.py tests/results \
    --figs tests/results/figs \
    --ab         tests/results/data/ab_farside.json \
    --ab_control tests/results/data/ab_control.json \
    --db_records 2730016036,2137097200,2317226804 \
    --out tests/results/report.html
```

That second command reproduces the committed `report.html` **byte for byte**, so
it is also the check that an edit to `E555_frame_report.py` changed only what you
meant it to. The `--db_records` numbers are the chain counts the three beamer arms
printed (baseline, far-side, undecided-control); they are what the report uses to
show that database loss does not explain the damage.

To redo the statistics themselves rather than the presentation, unpack the corpus
first — it is committed gzipped:

```bash
gunzip -c tests/results/data/corpus_row10.csv.gz > /tmp/corpus.csv   # 79 MB raw
python3 tests/E555_frame_stats.py /tmp/corpus.csv --out_dir /tmp/stats
```

`--n_perm` drives the label-shuffle calibration and is most of the runtime; set
it to 0 for a quick pass.

### What is deliberately NOT committed

The **6.4 GB chain database**. Recompute it — the first beamer run builds it in
~80 s and caches it to `--db_file`. Nothing else is missing: re-running the farm
needs only the repo, and re-running the analysis needs only `results/`.

### Where this was left, and the three things worth doing next

The study answered its own question — pieces have strong, reproducible corner
preferences — and then the practical test of that answer came out **negative**:
banning the twelve far-side pieces cost ~78 % of row-1 survival. Three follow-ups
come out of that, in order of how much they are worth:

**0. Done, and it changed the order of the other three: the prior went to Stage A
instead.** Re-keying was only ever needed because the prior had to be carried
*to* a board built in some other frame. Stage A does not have that problem — the
annealer builds the frame, so pinning the study's corners makes the table apply
verbatim. That is what `E555_border_prior.py`,
`E555_edge_annealer_FixedFrame.py` and `run_fixedframe_border.sh` are, and the
section above is what they measured. The three below still stand for Stage C.

**1. Re-key the prior to corner pieces, which is a prerequisite for everything
else.** The table is currently expressed as "piece 124 prefers the top-right zone
*of this frame*". None of the production boards in `data/` is in this frame — most
satisfy zero clues, and each uses a different one of the 4! = 24 corner
assignments — so the prior as measured cannot be applied to them at all. Re-key it
to "piece 124 wants the corner occupied by **corner-piece 1**" (in this frame TR
holds corner-piece 1, BL holds 3, BR holds 2, TL holds 0) and it becomes
frame-independent, because the four corner pieces are identifiable in any board.
Expect it to be *weaker* than measured: each canonical corner is anchored by both
a corner piece and a nearby clue, and re-keying carries only the first. How much
weaker is measurable — re-run `E555_frame_stats.py` keyed by corner piece instead
of by zone and compare the replication ρ against the +0.701 reported here.
§09 of the report has this.

**2. Retry the exclusion at a dose of two or three, not twelve.** The control arm
settled that the *ranking* is sound — it removed a smaller share of the database
and did ten times more damage — so the dose was wrong, not the statistics. Three
pieces cost ~7 % of the database against 22 % for twelve. Better still, make it a
score penalty rather than a ban: a penalty costs no chains at all, and the chain
database is where a ban does its damage.

**3. Select for the property instead of forcing it, at `--stop_row 11`.** This is
the experiment the A/B should have been, and §08 of the report sets it up. The
A/B measured P(board emitted); the thing that actually matters is
P(completable | emitted), because Stage C is the bottleneck and high-scoring
partials are not scarce. The corpus already says the boards you want exist in
ordinary output: 2.6 % of baseline boards leave ≥ 8 of the 12 far-side pieces
unplaced, and 83 % of that variance is decided board by board rather than by the
border — so it is boards you must select, not borders, and filtering costs zero
search time where forcing cost ~100× throughput. Run both arms at `--stop_row 11`
(12 emits nothing, and 11 leaves 28 spare pieces instead of 14) with
`--max_per_config` raised rather than lowered, and score both sets with the same
`E555_backtracker --break_mode stuck` dives, comparing break distributions **per
unit of wall time spent producing the set** — that last clause is what makes it
fair, charging exclusion for its throughput loss and filtering for its selection
loss.
