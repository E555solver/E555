# Data-driven beam steering — an experiment

Everything here is self-contained: a fork of the beamer, its own `Makefile`, and one
viewer script. Nothing in `src/`, `tools/`, `data/`, `examples/`, `pipeline/` or the
release gate is touched, and `src/B_beam/E555_database.c` is **linked unmodified**, the
way `E555_finalizer.c` already reuses it.

```bash
cd tests/datadriven && make ARCH=generic     # -> bin/E555_beamer_datadriven
```

## The problem this is aimed at

Stage B's objective is *local*. A child row is scored on the colour ledger plus a one-row
fan-out lookahead, so the beam has no notion of **which piece belongs where on the board as
a whole**. Rows 1–10 get committed on colour grounds alone, and by row 13 the pieces left
over are the wrong pieces — which is why the beamer produces hundreds of row-12 boards
whose top rows are unfinishable.

The missing signal is positional, and it can be *measured* rather than invented. With
`--clue_center --clue_corners` on and a Stage A rotations row fixing the four corners, the
puzzle is anchored: a piece's position is no longer free up to symmetry, so "piece *p*
tends to sit at cell *x*" is a real geometric statement about this seed and this border.

`PROJECT_E555.md` makes the same argument from the other end for the border
(`E555_database.c:1348`): every colour-multiset functional is *constant* across a border
row's columns, so only positional information can tell two orderings apart. A piece-by-cell
table is exactly that, which is why it ranks the border here too.

## Yes, it is two runs

Learning and searching are separate invocations, and that is deliberate: every
other flag then keeps the meaning it already has in whichever phase it is passed
to, instead of needing a phase-qualified twin. `run_datadriven.sh` runs them in
order:

```bash
bash tests/datadriven/run_datadriven.sh                       # learn, then search
bash tests/datadriven/run_datadriven.sh THREADS=16 ALPHA=50   # override anything
bash tests/datadriven/run_datadriven.sh LEARN=0               # reuse the table
```

`LEARN=0` is the one you want when tuning: the table holds **raw counts**, so
re-reading it at another `--freq_alpha` costs milliseconds, while learning again
costs an hour. Learning writes no boards at all, so only the search phase needs
disk.

### The three new flags

| flag | meaning |
|---|---|
| `--learn PATH` | learning phase. Grows the board from all four sides in turn, counts where each piece lands in the canonical frame, writes the table to `PATH`. **Emits no boards.** |
| `--table PATH` | search phase. Ranks the beam — and the bottom row and left column — by whole-board fit to `PATH`. Emission is unchanged. |
| `--freq_alpha A` | shrinkage toward the piece-by-row prior, in pseudo-configurations (default 20). The one knob. Swept: anything from 2 to 50 is indistinguishable, 200 is clearly too much (26 configurations reaching the stop row against 35–38). The default is left at 20 because the differences below 50 are inside the noise. |

Both phases need a rotations file and `--pin_clue 1..4`; without a pinned clue frame a
board's orientation is not readable and the four passes cannot be folded into one table.
Learning additionally refuses `--num_rows` other than 1 (the default, 0, means *every
remaining row of the file*, which would fold incompatible borders into one table),
`--resume` (the checkpoint records no pass number) and `--free_edges` (the border block
assumes a fixed side per edge piece). Each of those would corrupt a table quietly rather
than loudly, which is the only reason they are checked.

```bash
# learn (writes no boards, so it is cheap on disk)
bin/E555_beamer_datadriven ../../data/seed_Edge5.txt ../../data/borders_annealed_fix12.csv \
    --start_row 1 --num_rows 1 --top_bottoms 400 --top_columns 5 --stop_row 10 \
    --beam_width 200000 --threads 16 --clue_center --clue_corners --pin_clue 1 \
    --db_file runs/chain_clued.db --learn runs/table.txt --out_dir runs/learn

# search with it
bin/E555_beamer_datadriven ../../data/seed_Edge5.txt ../../data/borders_annealed_fix12.csv \
    --start_row 1 --num_rows 1 --top_bottoms 40 --top_columns 5 --stop_row 12 \
    --beam_width 200000 --threads 16 --clue_center --clue_corners --pin_clue 1 \
    --db_file runs/chain_clued.db --table runs/table.txt --out_dir runs/guided

# look at what was learned
python3 freq_view.py runs/table.txt --out runs/table.html
python3 freq_view.py runs/table.txt --text
```

## Choosing the learning settings

**`--stop_row 8` is enough, and cheaper than it looks.** Pass 0 covers canonical rows
`0..S` and pass 2 covers rows `(15-S)..15`, so the two of them alone cover the whole
board as soon as `S >= 7`; passes 1 and 3 then cover the same ground by column and give
the overlap. Measured on `borders_annealed_fix12.csv` row 1, `--stop_row 8` reaches
**all 247 free cells** (the other 9 of 256 are the four corners and the five clue cells,
excluded by design). Going deeper buys overlap, not coverage, and costs survivors.

**Raise `--top_bottoms` until the passes stop being starved, and leave `--top_columns`
small.** The two knobs are not interchangeable. A configuration is one vote, and the
configurations sharing a bottom row differ only in their left column, so they are
correlated: more columns per bottom inflates the vote count faster than it adds
information, and the reported effective sample size does not know that. More *bottoms* is
real independence. Keep `--top_columns` at about 5 and spend the budget on bottoms.

**How many bottoms is "enough" depends on the side, and the pools are wildly uneven.**
An annealed border is rich in Euler trails on some sides and poor on others, and each
pass puts a different side at the bottom. On `borders_annealed_fix12.csv` row 1:

| pass | bottoms in the pool | survival to row 8 |
|---|---|---|
| 0 | 480 | 10% |
| 1 | 25,920 | 0.2% |
| 2 | 46,080 | 2% |
| 3 | 432 | 18% |

Passes 0 and 3 exhaust their pools after a few hundred bottoms and cannot be improved by
raising the flag; passes 1 and 2 are starved by it and want tens of thousands. So set
`--top_bottoms` past the largest pool (50000 covers this file) and let each pass take
what it has. The learning summary prints the per-pass configuration counts and says
`raise --top_bottoms` when the thinnest pass is under a quarter of the fattest.

## Why four passes

A bottom-up beam barely reaches row 10, so it learns almost nothing about rows 11–14 —
exactly the rows that are failing. Pass *j* turns the rotations row *j* quarter-turns
clockwise and pins the clue frame to match. That is the **same anchored puzzle,
relabelled**, so growing from each of the four sides in turn and folding every board back
to the canonical frame fills the table from all four directions. At `--stop_row 10` the
canonical coverage is rows 1–10 from pass 0, rows 5–14 from pass 2, columns 1–10 from
pass 1 and columns 5–14 from pass 3: every interior cell covered, four times over in the
centre and twice in the corner regions. **The canonical top rows are supplied by pass 2's
rows 1–4 — its deepest and most heavily sampled.**

A border piece's spin *is* its side (its grey face points at the frame edge it belongs to,
which is why a rotations row carries no side field), so turning the row is `spin += 3n` on
exactly the pieces that have a grey face. That is the same map `tools/E555_rotate.py
--rotations` applies. The turn is checked at startup — four quarter-turns must be the
identity, and each single turn must actually move the row — and every learned board is
checked to canonicalise the centre clue back onto its pinned cell, following
`E555_roundhouse.c:359`: "a wrong spin direction here would quietly produce a board that
satisfies every edge but not the clue, so it is checked rather than commented."

## How a count becomes a weight

1. **One configuration, one vote.** Boards grown from one border are near-copies of each
   other, so a configuration's boards are divided by their own number. The passes are
   **not** rescaled against each other. An earlier version equalised them, and measurement
   showed that to be the wrong correction failing in the dangerous direction: passes come
   out as uneven as 48/1/10/78, so equalising multiplies a single-configuration pass by
   thirty-odd, and the shrinkage denominator then claims thirty configurations of evidence
   where one exists. That denominator is exactly what `--freq_alpha` is denominated in, so
   the effect is to switch off the backoff precisely where the backoff is all there is.
   Uneven coverage needs no correction: a quadrant that two thin passes cover really is
   less well known, shrinkage *should* be stronger there, and Sinkhorn removes the coverage
   bias from the final matrix anyway. The cure for an uneven pass count is more bottoms.
2. **Backoff.** `P(piece | row)` from the piece-by-row marginals — about 16× better sampled
   than the cell table, and the level the whole idea rests on: low pieces low, top pieces top.
3. **Shrinkage.** `P̂ = (N + α·P_row) / (W + α)`. Large `α` is the robust row prior, small
   `α` the sharp cell table. It also keeps every log finite — a zero count would otherwise
   be a hard rejection, fatal for a table this thin — and makes a thinly-sampled cell fall
   back by itself, so trust follows the data without a rule.
4. **Balance (Sinkhorn).** A row of the board is a *permutation*, so a beam maximising an
   unbalanced `Σ log P(p|x)` spends globally popular pieces on the low rows it commits
   first and strands the top — the very disease this table is meant to treat. Making every
   piece's mass and every cell's mass 1 removes that and the coverage bias in one step.
5. **Weights.** `log(K·Q)` nats: zero at chance, additive, and in the same units as the
   fan-out terms they sit beside.

Effective sample size is reported in **configurations, not boards**. A configuration's
500 boards are near-copies, so counting them as 500 independent samples reports tens of
thousands of samples for evidence worth a few dozen borders — which is what an earlier
version did. Treat the figure as an upper bound even now: configurations sharing a bottom
row are still correlated (see *Choosing the learning settings*).

The file on disk holds **raw counts**, not processed weights, so `--freq_alpha` can be
retuned on the next search in milliseconds instead of by learning again. It is
line-oriented and greppable for the reason `E555_extract_consensus.py` gives: being able to
read one line with `grep` is worth more than loading it in one call. Provenance (seed hash,
rotations row, clue mask, pin) is **checked, not trusted** — a frozen table is a calibration
tied to all of those, and the objection to one is that it goes stale *silently*. A seed or
clue mismatch is fatal. A rotations-row mismatch drops the border block and says so: a
different row deals the 56 edges to different sides, so those weights would not be merely
stale but wrong, while the interior table is about the puzzle and still steers.

## What the search does with it

The frequency term is **added to** the library's own colour and fan-out measure, not
substituted for it, and it is **carried forward**: the parent's running total plus this
row. A board that spent a top-loving piece on a low row keeps the deficit for life and
loses when the pool overflows, where the stock score is recomputed fresh each row and
carries no history at all. `BeamEntry.score` — which the stock beamer writes and never
reads — becomes that running total at no cost.

Adding rather than replacing is a measured choice, not a hedge: position says where a
piece belongs, while the fan-out lookahead says whether *any* row fits above the one being
committed, and dropping the second halves the boards reaching a given depth (see below).

Three environment variables exist for checking the machinery, not for tuning it.
`E555_FREQ_PURE=1` restores the position-only score; `E555_FREQ_DEBUG=1` then asserts that
the running total equals a from-scratch sum over every placed cell (it is only meaningful
under `PURE`, since the library's per-row terms are not a sum over cells), and a
deliberately broken build does trip it. `E555_FREQ_NOBORDER=1` drops the border block so
the beam score can be measured on its own. `E555_FREQ_DUMP=PATH` writes the processed
weights for `freq_view.py --check`.

The border ranking adds the same weights to the library's own fan-out measure; both are in
nats, so `--tau_bottoms` / `--tau_columns` keep their exact meaning — the orderings are
still drawn in proportion to `exp(rank/τ)`, now with the learned frequencies inside the rank.

## Measured: it does not work yet

**On this seed and this border file, the data-driven steering is neutral. It does not
beat the stock beamer, and an earlier result suggesting otherwise was an artefact.**

The artefact is worth stating plainly because it is easy to reproduce and easy to
believe. An early run showed guided search reaching the stop row on 101 configurations
against the baseline's 2. That run used `--top_bottoms 40`, so each arm searched the
*first forty* bottoms of its own ranking — and the guided arm's table had been learned
from boards grown on that same border, so its ranking was substantially recall of which
bottoms had already worked. Search the **whole** pool instead, and both arms see the same
configurations; the gap disappears.

Isolating the two halves of the idea at `--stop_row 10` on border row 1, 480 bottoms
(the full pool) × 5 columns, beam 20000, equal wall:

| arm | configs reaching row 10 | boards emitted |
|---|---|---|
| baseline | 48 | 1117 |
| beam score only, position added to the library's measure | 48 | 935 |
| beam score only, position *replacing* it | 47 | 496 |
| beam score only, position at 0.1 and 0.3 strength | 47 | 1091 / 1041 |
| border ranking only, stock beam score | 40 | 1199 |

Two things fall out. The **beam score is neutral** — every variant reaches the same depth,
so the piece-by-cell table is neither helping nor hurting the search. And **replacing the
fan-out lookahead is strictly worse than adding to it**: same depth, less than half the
boards, because a positionally handsome row that nothing can sit on top of is still a dead
end. That is now the default, and the weight between the two terms was swept over 0.1–1.0
and changed nothing, so there is no weight to tune.

The border ranking's apparent 40-against-48 is itself a truncation artefact: that arm was
cut off by the wall clock mid-sweep, so its *ordering* decided which configurations got
tried. Run to completion on border row 3 below, the border ranking is neutral too.

### Row 12

Not reached, by either arm, anywhere in this file.

Sweeping all 12 border rows at `--stop_row 12` (beam 20000, full bottom pools): 215
configurations died at row 11, exactly one died at row 12, and **none completed row 12**.
Eight of the twelve borders die at row 2, on the corner clues. The three that go deep are
rows 1, 8 and 11, and the best by a distance is **row 3**: 120 of its 456 configurations
reach row 10 or beyond.

Taking border row 3, learning its own table, and running `--stop_row 12` at beam 200000 —
ten times the width, every configuration searched to exhaustion so no ordering artefact is
possible:

| arm | completed row 12 | configs reaching row 11 |
|---|---|---|
| baseline | 0 | 115 |
| guided, beam score only | 0 | 117 |
| guided, beam score and border ranking | 0 | 117 |

At `--stop_row 11` the same border does produce boards, and they are the same boards by
every measure `tools/E555_rank.py` reports:

| arm | configs reaching row 11 | boards | score | solid edges | placed |
|---|---|---|---|---|---|
| baseline | 3 | 7 | 356 | 176 | 194 |
| guided | 4 | 7 | 356 | 176 | 194 |

So the honest summary: **rows 0–11 are reachable here, row 12 is not, and the table makes
no difference to either.** The four-pass machinery, the estimator and the plumbing all
work and are verified; what is missing is evidence that a piece-by-cell prior is the
signal the beam is short of.

### The likeliest reason it is neutral

The table is learned from the beam's own output. Every board it counts was produced by the
stock beam, growing from a border, surviving to `--stop_row 8` — so the distribution it
measures is the distribution the beam already draws from. Handing that back to the beam as
a prior tells it what it is already doing. It reinforces the existing bias instead of
correcting it, which is exactly why the lift can be real (+0.88 nats/cell, honestly held
out) while the search outcome does not move: the table predicts the beam's boards well,
and the beam's boards are the problem.

That reading also predicts what would break the tie, and it is not a better estimator or a
better alpha. It is a **better source of boards** — ones the beam cannot reach on its own.

### Why it might still be worth pursuing

The table itself is not empty — held-out lift is **+0.82 to +0.88 nats/cell** over hundreds
of thousands of boards, leave-one-configuration-out, so the positional structure is real
and measurable. The sorted piece-by-row panel in `freq_view.py` shows the most opinionated
pieces preferring rows 12–14, which is exactly the region a bottom-up beam is blind to.
The signal exists; it simply does not change which boards survive.

Three things would test that further, in rough order of cost:

1. **Score the top rows, not the whole board.** The table is applied to every placed cell,
   so its opinion about rows 1–4 — where the beam is already doing fine — dilutes its
   opinion about rows 11–14. Weighting the term by row, or applying it only above some
   row, is a small change.
2. **Judge by extendability, not depth.** Every comparison here counts boards reaching a
   row. The actual complaint is that row-12 boards have unfinishable *tops*, which is a
   question for `E555_finalizer` or `E555_roundhouse` run over both arms' output. That is
   the measurement this experiment still lacks.
3. **Learn from a deeper source.** Everything here learns at `--stop_row 8`. A table learned
   from genuinely deep boards — Stage C output, or a previous run's best — would carry
   information about the top rows that no bottom-up pass can supply.

## Unrelated bug found while building this

The stock beamer's `--incomplete_top` partial file is **not reproducible** run to run at the
same `--rng_seed` *and* `--threads`. Two stock runs at identical settings produced 609,494
partial boards each and shared only **0.2%** of them. The completions file is exactly
reproducible; only the partials are affected. `emit_incomplete` writes from inside the
parallel expansion, where pool indices depend on thread scheduling, so the dedup keeps
different representatives each run. This contradicts the determinism contract in
`CLAUDE.md` ("determinism is `--rng_seed` *together with* `--threads`"). Nothing here
depends on it — learning reads the stop-row ranking, which is stable — and it is left
alone as out of scope, but it is worth fixing in `src/`.

## Files

| file | role |
|---|---|
| `E555_beamer_datadriven.c` | the fork: learning phase, estimator, guided search |
| `Makefile` | builds into `bin/`, links `../../src/B_beam/E555_database.c` |
| `freq_view.py` | self-contained HTML report on a table; `--text`, `--check` |
| `run_datadriven.sh` | both phases in order; `LEARN=0` reuses the table |
| `runs/` | scratch: tables, logs, the cached chain database (gitignored) |

`freq_view.py` re-implements the estimator independently. That is deliberate: it lets
`--freq_alpha` be explored without re-learning, and `--check` compares the two
implementations against each other —

```bash
E555_FREQ_DUMP=runs/fw.txt bin/E555_beamer_datadriven SEED ROT --table runs/table.txt ...
python3 freq_view.py runs/table.txt --alpha 20 --check runs/fw.txt
```
