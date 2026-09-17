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

## Two phases, two separate runs

Keeping them separate is deliberate: every other flag then keeps the meaning it already
has, instead of needing a phase-qualified twin.

| flag | meaning |
|---|---|
| `--learn PATH` | learning phase. Grows the board from all four sides in turn, counts where each piece lands in the canonical frame, writes the table to `PATH`. **Emits no boards.** |
| `--table PATH` | search phase. Ranks the beam — and the bottom row and left column — by whole-board fit to `PATH`. Emission is unchanged. |
| `--freq_alpha A` | shrinkage toward the piece-by-row prior, in pseudo-configurations (default 20). The one knob. |

Both phases need a rotations file and `--pin_clue 1..4`; without a pinned clue frame a
board's orientation is not readable and the four passes cannot be folded into one table.

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
   other, so a configuration's boards are divided by their own number. Each pass is then
   scaled to the same total weight, so a productive pass does not bend the table toward the
   bands only it covers.
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

The score becomes the **whole board, carried forward**: a sum over every placed cell, which
is the same number as "the parent's total plus this row", so the cheap form is used and
`BeamEntry.score` — which the stock beamer writes and never reads — becomes that running
total at no cost. A board that spent a top-loving piece on a low row keeps the deficit for
life and loses when the pool overflows. The stock score is recomputed fresh each row and
carries no history at all. `E555_FREQ_DEBUG=1` asserts the running total equals a
from-scratch sum over every placed cell.

The border ranking adds the same weights to the library's own fan-out measure; both are in
nats, so `--tau_bottoms` / `--tau_columns` keep their exact meaning — the orderings are
still drawn in proportion to `exp(rank/τ)`, now with the learned frequencies inside the rank.

## Measured so far — and what it does not show

On `seed_Edge5` / `borders_annealed_fix12.csv` row 1, `--stop_row 4`, a small budget:

- **The fork is the stock beamer when unsteered.** With no `--table` it reproduces
  `bin/E555_beamer` byte-for-byte on a 28,439-board completions file.
- **Held-out lift `+1.00` nats/cell over 49,000 boards**, scored leave-one-configuration-out
  (a board is measured against a table holding only configurations that closed before its
  own). Sinkhorn balance error `4.8e-14`.
- **Guided vs baseline at the same budget, searching one row deeper than the table was
  learned: 101 configurations reached the stop row against 2**, 193,649 boards against 4,963.

**That last number is not independent evidence, and should not be reported as if it were.**
The table was learned from boards grown on this same rotations row, so the border ranking is
substantially *recall* — it remembers which bottoms worked — rather than generalisation. A
cross-row test (learn row 1, search row 2) was attempted and is **inconclusive**: row 2
yields no survivors in either arm at any budget tried, though both arms showing identical
extinction profiles does confirm the border-mismatch guard behaves. The decisive experiment
is a production-scale A/B with `tools/E555_compare_sweeps.py`, and it has not been run.

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
| `runs/` | scratch: tables, logs, the cached chain database (gitignored) |

`freq_view.py` re-implements the estimator independently. That is deliberate: it lets
`--freq_alpha` be explored without re-learning, and `--check` compares the two
implementations against each other —

```bash
E555_FREQ_DUMP=runs/fw.txt bin/E555_beamer_datadriven SEED ROT --table runs/table.txt ...
python3 freq_view.py runs/table.txt --alpha 20 --check runs/fw.txt
```
