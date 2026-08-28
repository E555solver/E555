# E555 -- Beam-Search Pipeline for Eternity II

  [Author] AB with Claude Code assistance
  [Date] July 2026

## The Puzzle

Eternity II is a 16x16 edge-matching jigsaw puzzle with 256 pieces. Each piece
has four colored edges (top, right, bottom, left). A placement is legal when
every shared edge between adjacent pieces carries the same color, and all
outward-facing border edges carry the *frame color* (color 0). With 256 pieces
and 480 binary equality constraints, the puzzle is an NP-complete CSP. No
solution has ever been published; the current public record is 470 of 480
inner edges correct.

**Color alphabet** (seed file `data/seed_Edge5.txt`):
- Color 0: frame -- all outward-facing border edges.
- Colors 1-5: frame-interface palette -- colors where border pieces adjoin.
- Colors 6-22: 17 inner colors used by inner pieces and the inward faces of
  border pieces.

**Piece types** (derived from the seed): 4 corner pieces (two frame edges),
56 edge pieces (one frame edge), 196 inner pieces (none).

---

## The Pipeline -- Overview

```
data/seed_Edge5.txt
  │
  ▼
┌───────────────────────────────────────────────────────────────────────┐
│ Stage A: E555_edge_annealer  (Python, stdlib only)          OPTIONAL  │
│   Anneals the assignment of the 60 border pieces to the four sides    │
│   so that every side is rich in valid orderings (Euler trails).       │
│   - laptop, minutes            - output → rotations.csv               │
│   - restarts in parallel (--threads)                                  │
└────────────────────────────────┬──────────────────────────────────────┘
                                 │        (or skip Stage A entirely:
                                 ▼         beamer --random_edges)
┌───────────────────────────────────────────────────────────────────────┐
│ Stage B: E555_beamer  (C, OpenMP)                                     │
│   Builds the 5-5-5 chain database (6.4 GB, in RAM), then sweeps       │
│   (bottom-row x left-column) border configurations, each searched by  │
│   a wide beam advancing one full row at a time. Boards surviving to   │
│   --stop_row are emitted.                                             │
│   - laptop, <=16 GB RAM         - output → beam_completions_*.csv     │
│                                                                       │
│ E555_finalizer  (C, OpenMP)                                           │
│   The same beam machinery started FROM a partial board: rows at or    │
│   below --finalize_from stay locked, a reduced database (built in     │
│   seconds without the locked pieces) searches the rows above at full  │
│   width. Consumes and produces the same CSV -- chains with itself.    │
└────────────────────────────────┬──────────────────────────────────────┘
                                 │
                                 ▼
┌───────────────────────────────────────────────────────────────────────┐
│ E555_roundhouse  (C, OpenMP)                                          │
│   Rotates the board 90 deg and refills a W-wide border strip from a   │
│   width-W chain database (megabytes, seconds). An exact backward DP   │
│   over the relaxed problem says which colorings can still finish the  │
│   strip before any piece is tried. --rounds 3 spirals around a        │
│   retained core, rebuilding three sides including 54 border pieces.   │
│   - laptop, seconds-hours      - output → roundhouse_r<N>.csv         │
└────────────────────────────────┬──────────────────────────────────────┘
                                 │
                                 ▼
┌───────────────────────────────────────────────────────────────────────┐
│ Stage C: the tail toolbox                                             │
│   E555_topper.py      break minimizer; herds breaks to the NEAREST    │
│                       corner; --side opens any border band            │
│   E555_backtracker    exact / bounded-mismatch DFS tail closer        │
│   E555_ender.py       budgeted local re-solve; --mode patch|ring      │
│   - laptop             - output → canonical board CSV, score /480     │
└───────────────────────────────────────────────────────────────────────┘
```

**The search philosophy.** Stage B does not try to be exhaustive. Which bottom
configuration admits a completion is essentially unknowable in advance -- so the
strategy is to *fail fast and play many hands*: advance one row at a time, keep
many diverse positions alive, recognize dead configurations quickly (an empty
candidate pool is an exact proof of death below the current row), and move on.
Stage C tools are the opposite: they spend real time on a few elite boards.

---

## Conventions

- Rows and columns are **0-indexed, bottom-up**: row 0 is the bottom border,
  row 15 the top border; col 0 is the left edge. Cell index = `row*16 + col`.
- Rotations are **CCW quarter-turns** `s  in  {0,1,2,3}`: side `d` of the rotated
  piece reads seed side `(d+s) mod 4`.
- **Canonical board CSV** (all Stage C tools write it; every tool reads it):

  ```
  config_id , score , pos[0..255] , rot[0..255]          (514 fields)
  ```

  `pos[p]` = cell of piece `p` (999 = unplaced); `score` = matched internal
  edges (0..480). Stage B writes its solution index in the second slot
  instead. Readers take the LAST 512 fields as pos+rot and treat leading
  fields as metadata, so both variants (and the legacy 515-field layout with a
  rank column) parse everywhere. `#`/`%` lines are comments.
  `tools/E555_rank.py --out FILE --rescore` rewrites any of them canonically,
  recomputing field 2 from the seed so the column can be sorted on.

---

## Stage A -- E555_edge_annealer

`src/A_border/E555_edge_annealer.py` (pure standard library)

Each board side defines a directed multigraph: nodes are frame-interface
colors, and every edge piece assigned to that side contributes one arc (the
color pair it exposes along the border). A legal left-to-right ordering of the
side's pieces is exactly an Euler trail of that graph, and the **exact trail
count** comes from the BEST theorem --
`#trails = tw(G) * prod(outdeg(v) - 1)!` -- with the arborescence count `tw(G)`
computed as a cofactor of the directed Laplacian by exact integer Bareiss
elimination. No floating point, no estimates.

A simulated-annealing loop with a tabu list swaps edge pieces between sides
(corner pieces swap positions in the early part of each restart), cooling
exponentially from `--T0` to `--Tf`. Hard constraints are enforced by
dominating penalties before trail counts matter: per-side degree balance,
weak connectivity, the inner-color inventory bound (inward-facing colors of
the border must not exceed the inner-piece supply), and parity of the surplus.

**Two scoring modes.**

1. *Log-sum (default)*: maximize `sum w(side) * log(trail_count)`, i.e. the
   weighted geometric mean of the four counts. It has no notion of "enough":
   whichever side is cheapest to enrich runs away, and with signed weights the
   minimized sides are driven toward starvation.
2. *Target balancing (`--target_scale N`)*: drive every side toward its own
   target of `w(side)*N` trails. A side is scored on how many **decades** its
   trail count sits from its own target -- `100 - 25*sum log10(count/target)^2` --
   so the penalty depends on the ratio and never on the raw magnitude: a side
   2x off its target costs the same 2.27 points whether that target is 250 or
   15000. The weights therefore choose targets only, never importance, and a
   side with a large target cannot drown out a small one. 100 = every side on
   target. The preferred mode: Stage B needs all four sides healthy
   simultaneously.

**Which mode, and why the counts matter.** A side's trail count is not an
abstract quality score -- it *is* the size of the space Stage B enumerates.
The bottom count is exactly the number of bottom-row orderings the beamer
sweeps and the left count the number of left columns per bottom: the border
`TOP=17280 RIGHT=11520 BOTTOM=1152 LEFT=2880` makes the beamer print
`bottoms=1152 ... left-cols=2880 enumerated`, followed by one `[rank]` line per
bottom saying how many of those 2880 can actually start row 1 with it. So a
bottom of 5000 with
`--top_bottoms 300` means the sweep only ever sees 6 % of its own space, while
a bottom of 400 means it sees three quarters of it. That is the argument for
targets: you are sizing a search, not maximizing a number.

**Allocating within that space** (`--bail_columns N`, 0 = off). Every
(bottom x left) config gets the same budget, which is the weakest allocation
when config quality varies by orders of magnitude. `--time_limit` does not
help: it is a per-row *deadline*, so on a config that finishes in seconds it
never fires, and being wall-clock based it would shift meaning with the core
count anyway. `--bail_columns` is work-based instead -- abandon a bottom once N
consecutive columns have emitted nothing, and move to the next. A productive
column resets the streak, so a bottom that is producing keeps its full
`--top_columns`; a barren one is cut short. Pair it with `--incomplete_top`:
partials emit below the stop row, so the signal resets often enough to be
adaptive. On completions alone at a high `--stop_row`, emissions are rare enough
that this acts as a flat cap of N columns per bottom -- still a reasonable trade
of depth for breadth, given how weak the column ranking is, but not adaptive.

Measured over 8 seeds x 100k steps, targets 250/5000/5000/15000
(bottom/left/right/top):

| objective | geometric-mean counts (T/R/B/L) | on-target score |
|---|---|---|
| log-sum, all `w=1` | 4065 / 1977 / **3033** / 2574 | 42.7 |
| log-sum, shaped `+9/-2/-5/-2` | 4825 / 550 / 449 / 643 | 39.6 |
| `--target_scale 250`, `w` 60/20/20/1 | 5465 / 3019 / **437** / 2720 | **85.8** |

Log-sum with equal weights lets the bottom run to 12x the wanted size (and
swing between 1152 and 7200 across seeds); the shaped weights over-correct and
starve three sides (weakest side seen: 128). Target mode lands every side
within about 2x of its target -- and gives up almost nothing in total richness
to do it: its best log-sum value is 8.17 against log-sum's own 8.51.

Use **log-sum** to explore what the piece set can do at all, or to push one
side as hard as possible and genuinely not care about the rest. Use
**`--target_scale`** for anything feeding Stage B, where a bottom that is too
rich is just as unhelpful as a top that is too poor.

**Output.** With `--out FILE` each restart's best border is appended to a
rotations CSV that the beamer reads directly: a `#` comment with the per-side
counts, then `id, spin[0..255]` (60 real spins + 196 zeros). That file is the
deliverable and is written in both output modes.

On stdout the default is one line per restart -- score, the four trail counts,
the step the best was found at, and the time -- so a 50-restart run is 50 lines.
`--verbose` instead prints the whole search: the full config, a per-step
temperature/acceptance/counts report every `report_every` steps, and each
border as a `BEST,...` line (trail counts + the 60 border spins) that
`grep '^BEST,'` collects.

**Parallelism.** The restarts are independent walks from independent random
starts, so `--threads N` runs them in parallel; `--threads 0`, the default, uses
one worker per core. They are worker *processes*, not threads: the hot loop is
pure Python, so the GIL would serialize threads and buy nothing. Each restart
derives its own RNG seed from `--rng_seed` and its own index rather than drawing
from a stream shared with the others, so **the thread count never changes the
result**, and the parent replays the restarts -- stdout and the rotations CSV
alike -- in restart order however the workers finish. The pool is capped at one
worker per restart. Measured on an 8-thread laptop the gain is ~3x (8 restarts x
50k steps: 24.5s serial, 7.9s parallel -- the workers share cores, so it is not
8x), which takes the `run_pipeline.sh` Stage A of 50 restarts x 500k
steps from ~14 min to ~5.

Key options: `--restarts`, `--steps`, `--rng_seed`, `--threads`, `--verbose`,
`--T0/--Tf`, `--w_top/right/bottom/left` (per-side target multipliers with
`--target_scale`, and signed weights in log-sum mode, where a negative weight
minimizes a side), `--tabu`, `--fix_corners {0,1,2}`, `--target_scale`, `--out`.

---

## Stage B -- E555_beamer

`src/B_beam/E555_database.{c,h}` + `src/B_beam/E555_beamer.{c,h}`

### The 5-5-5 row decomposition

Every inner row is a fixed left-edge column plus three 5-cell segments:

```
 col 0        cols 1-5        cols 6-10        cols 11-15
[left edge]  [seg A: 5 inner] [seg B: 5 inner] [seg C: 4 inner + right edge]
```

The left column comes from the border configuration. The right edge is **not**
searched separately -- it *emerges* from the database.

### The single chain database `DB_5pieces`

`DB[left_color][b1][b2][b3][b4][b5]` is a 6-D direct-indexed array of packed
**5-piece horizontal chains**, keyed by the inner color exposed to the chain's
left plus the five colors it must present downward (the exposed tops of the
row below). A cell holds either five inner pieces (when `b5` is an inner
color, 6-22) or four inner pieces plus a frame-right **edge terminal** (when
`b5` is a frame-interface color, 1-5). The two families can never collide
because the color ranges are disjoint -- so ONE database serves all three
segments of every row: segment A is keyed by the left column's exposed color,
B by A's rightmost exposed color, C by B's; and because segment C's rightmost
bottom color is always a frame-interface color (seeded by the bottom-right
corner, perpetuated by each right edge's top), C's cell automatically supplies
the right edge piece.

Records pack to ~2 bytes each: a piece is stored as its index inside the tiny
(left,bottom)-color bucket of the oriented-piece catalog (both colors known at
decode time; typical bucket <= 7 pieces → 3 bits per piece). For the official
piece set: **3.12 x 10^9 chains, 6.41 GB**, built once in two parallel passes
(~1 min) and then promise-sorted (~2-3 min).

The inner cells depend only on the seed and can be cached on disk
(`--db_file`, ~6.5 GB; subsequent runs `mmap` it read-only and start in
seconds). The border-dependent edge cells are tiny and rebuilt per border row.

### The fan-out table

`fanout[b1..b5]` caches, for every bottom signature, the record count summed
over all 17 possible left neighbours (~1.9 M entries, 15 MB): "how continuable
are these five exposed tops?" in one lookup. It powers the in-cell promise
sort, border ranking, and the beam's one-row lookahead.

### Ranking a left column

A bottom row is ranked by `bottom_rank_of`: the log fan-out of the three
segments it presents, `db_seg_fanout(rt[1..5]) + (rt[6..10]) + (rt[11..15])`.
Those three windows are not a cut someone chose -- they *are* the 5-5-5
architecture, so the score counts the search's actual first move.

A column has no such decomposition. It faces fourteen different rows and each
row's segment A takes exactly **one** colour from it, as the `left_color` key --
never five. `left_rank_of` scores it in two parts.

**Windows, by rotation.** Turn the board a quarter-turn counter-clockwise and the
left column *is* a bottom row: per piece CCW sends N→W, W→S, S→E, E→N, so
`phi(v).bottom = v.left` and `phi(v).top = v.right`. The column's fixed rightward
faces become the fixed bottom colours a chain sits on, the free leftward faces of
col 1 become its free tops, and the chain axis becomes bottom↔top read
**downward**. So the vertical 5-strip of inner pieces standing against rows
`r..r+4` is counted by

```
db_seg_fanout(right[r+4], right[r+3], right[r+2], right[r+1], right[r])
```

marginalised over the unknown colour above it -- the same marginalisation
`bottom_rank_of` accepts over the unknown colour to its left. This is an exact
count, not an analogy: `g_cat` holds all four spins of every inner piece, so the
record set is closed under the rotation. **The argument order is load-bearing.**
Read the other way round, a real board's own column counts zero: on
`data/synth_solution_480.csv` the downward read is a legal chain for 10 of 10
windows of the true left column and the upward read for 0 of 10.
`E555_COL_VERIFY=1` guards it at startup by rebuilding strips out of the database
and asserting the window returns exactly the cell they came from (300/300 exact,
and the reversed order differs on 299 of 300, so the check has teeth).

The windows **slide** (`r = 1..10`) rather than partitioning into three the way
the bottom's do. Since a 5-window is a proxy object here, no cut point is
privileged, and partitioning at 1-5/6-10/10-14 would blind the score across rows
5-6 and 10-11 by accident of where counting started -- when order-awareness is
the entire point of doing this at all. Sliding also keeps every window all-inner:
one reaching row 0 would need the DB's edge cells, whose terminal pool is the
*right* edges, where the rotation calls for the *bottom* edges.

**The joint term.** `right[1]` **is** segment A's `left_color` at row 1, and the
bottom fixes `rtop0[1..5]`, so `db_seg_count(right[1], rt[1..5])` is the exact
number of legal row-1 segment-A chains for this pair -- precisely the quantity
`bottom_rank_of` has to marginalise away, un-marginalised by the column. It is
the only place in the database where the two borders meet, and **zero proves the
pair cannot complete row 1**. Such columns sort last and are not run: on 150
distinct configs from an earlier sweep, 17 (11.3 %) died at row 1 in 0.0 s, and
for two bottoms it was 3 of their 6 columns.

Because of that term the ranking is **per bottom**, inside the sweep's bottom
loop rather than once per border row, and `l0` means the best column for *this*
bottom. Its RNG stream is keyed by the bottom index so `--resume` re-derives the
same ordering. Cost is a few thousand table lookups and a qsort of at most a few
thousand structs -- sub-millisecond against a 600 s config slice.

**The finalizer keeps the old measure.** `fin_left_rank` is still
`sum_r log1p(la_total[right[r]])`, and deliberately so. Its column is half
locked -- col 1 at rows `1..finalize_from` is already occupied, and the reduced
database has had those very pieces removed -- so a rotation window reaching
below the lock line counts strips for cells nothing will fill, against a
database skewed against them. Restricting to windows above the line is correct
but leaves none at all once fewer than five rows are free, which is the common
case. And unlike the beamer's, its measure is not degenerate: `fin_enumerate_lefts`
and `fin_sample_left` draw from the remaining edge pool, so piece sets genuinely
vary between samples and a multiset census still discriminates. Measured both
ways, the rotation windows cost the `clue_orient` regression its emissions
outright, where the old measure passes it.

**Why there is no colour-supply term beside it.** Every enumerated column of a
border row is a permutation of the *same* `EDGE_LEN` pieces, so its exposed
colour multiset is invariant, so every functional of that multiset is constant
across columns: `sum_c D_c log R_c`, the ratio form `sum_c D_c log(D_c/R_c)`, and
`closure_raw` on the initial board alike -- the last because `color_consumed` is
all zero there and the bottom's demands are an additive constant inside the
bottom loop. Only **positional** information distinguishes columns, and database
lookups are how it is read. (`--random_edges` differs: columns are drawn from the
56-edge pool minus the bottom's, so piece sets genuinely vary and the old measure
was merely order-blind there rather than constant. Both terms above are live in
both modes, so one score serves both.)

### The beam loop

For each (bottom ordering x left-column ordering) configuration -- enumerated
as Euler trails, the bottoms ranked by fan-out and the columns ranked per
bottom (above) -- one beam advances row by row:

1. **Expand.** Every board fills its next row A→B→C from the database, with
   exact 256-bit piece-disjointness masks. Cells are pre-sorted by promise, so
   a first budgeted phase scans the most continuable chains; a second phase
   visits, in a random full-cycle permutation (random start + coprime stride),
   exactly the records phase 1 did NOT reach -- both phases index the same
   slice-local space, so no A record is ever tried twice for one parent. That
   matters most where quota is not the binding constraint, i.e. the early rows
   and the collapsing rows 9-12: permuting the whole cell there re-walked
   everything phase 1 had just done, and `try_A` is deterministic in its record,
   so every one of those was a bit-identical duplicate child. Measured before
   the fix, candidates/unique sat at exactly 2.0000 on every row with quota
   headroom. Per-parent work is bounded by `--pool_factor` (child quota) and a
   fixed decode budget.
2. **Score.** See below.
3. **Select.** Children dedup by a 64-bit *frontier signature* -- a hash of
   (used-piece set, exposed top colors), which provably determines a board's
   entire future -- keeping the best copy. Survivors are pruned to the row's
   width by a score band (with a per-parent offspring cap) plus a random band.
4. **Materialize.** Moves go to an ancestry log; beam entries are exactly 128 B
   (two cache lines, held there by a `_Static_assert`).

An empty child pool ends the configuration (`extinct`); the sweep moves on. It
is **not** a proof that the configuration is dead: the generator keeps a bounded
number of children per segment-A record, spends a bounded quota per parent, and
starts from an already-pruned beam, so an empty pool means only that this
bounded search found no child from the states it still held. Every board that
completes `--stop_row` is emitted, best first -- deliberately with no lookahead
at the stop row: whether the board continues is the next stage's problem.

### Reading the sweep log

One `[sweep]` line per configuration, in one of three shapes:

```
[sweep] r1b3l4 filled=11 width=2 reason=stop_row emitted=2 sol_total=2 wall=13.6s
[sweep] r0b0l0 died=1 width=1 reason=extinct(clue_row) wall=0.0s
[sweep] r0b0l1-l19 x19 died=1 width=1 reason=extinct(clue_row) wall=0.1s
```

`filled=` and `died=` are separate fields because one number cannot be both.
`filled=R` is the last row COMPLETED, and the width beside it is the beam that
completed it; `died=R` is the row that FAILED, and the width beside it is what
the beam carried INTO that row -- never a width row R ever reached, which is 0
by definition of an extinction. `died=1 width=1` therefore says the search never
got past the bare border board.

A configuration that emitted nothing prints only those fields; `partials=` and
`part_total=` appear only under `--incomplete_top`. Consecutive barren
configurations that died identically under one bottom collapse into a single
`l<first>-l<last> x<n>` line whose `wall=` is their combined time -- one clued
production log spent 474 of its 653 lines on byte-identical deaths. The first of
a run always prints in full, and a run is flushed once its configurations have
cost 30 s between them, so a slow sequence still reports progress.

`reason=extinct(clue_row)` marks a death on a row a clue constrains, which is
worth calling out because it is rarely the row you expect: **a clue pins the row
below it as well as its own**, to the colour it will stand on. `--clue_corners`
names cells on row 2 and so bites at row 1, and with the four orientations all
enabled the pinned walk must satisfy one of four different colour pairs at
cols 2 and 13 on top of a border that already fixes all fourteen of row 1's
bottom colours. Measured on `data/borders_annealed_fix12.csv` border row 0, that
is the difference between every configuration reaching the stop row and every
configuration dying at row 1. The `[cfg]` banner lists the pinned rows up front
(`pinned_rows=1,2,6,7,8`) so a run that dies at row 1 explains itself.

**Width and randomness schedules.** Extinction pressure concentrates in the
high rows, where the piece supply thins. Three coupled schedules concentrate
effort there (K = `--beam_width`, E = `--beam_expand`, R = `--beam_expand_row`):

| rows | width | random band | parent cap |
|---|---|---|---|
| 1 ... R-2 | K | `frac_rand` | `parent_cap` |
| R-1 | max(K, K*E/2) | `frac_rand` | 2x`parent_cap` |
| R ... stop_row | K*E | `frac_rand` | 2x`parent_cap` |

Width and the offspring cap still step up late, where extinction pressure is
highest. The random band does NOT: `--frac_rand` is flat across every row.

It used to taper -- full early, half at R-1, zero from R on -- which was the
right shape for a band of 0.75, where the late rows needed protecting from it.
At 0.10 the taper buys nothing and costs the late rows their only hedge
against a biased objective. It is also close to a no-op either way: the band
is split off inside `select_beam`, which only runs when the pool EXCEEDS the
row width, and at the expanded rows it usually does not (measured mean
occupancy at row 8 was 700k of 1.31M slots). Where selection does not bind,
every candidate survives and the fraction never applies.

Both bands are drawn from the same deduplicated pool and sum to the row width
(`k_rand = frac_rand * rem`, `k_top = rem - k_rand`), so lowering `frac_rand`
does not send more states forward -- it sends better-chosen ones. Measured
over 18 viable configs at production width, two seeds, counting configs that
filled row 11: 0.75 -> 9.0, 0.50 -> 12.0, 0.25 -> 12.5, 0.10 -> 15.0, 0 ->
14.5. A separate check confirmed the extra depth is real material and not a
collapsed lineage: the emitted row-10 boards are 34-39% MORE numerous at 0.10
than at 0.75, 100% distinct, with the same mean pairwise separation (317 of
512 cells) and the same ~46 pieces available per cell.

**The finalizer's band is flat too, but at 0.30 -- and that difference is
deliberate.** Its taper was worse than the beamer's, because a schedule written
for a search starting at row 1 does not survive being handed a board already
filled to `finalize_from`. At the default `finalize_from 8` the searched rows
are 9..12 and `beam_expand_row` is 8, so exactly one row kept any randomness and
the other three were purely fan-out selected -- the old code carved out that one
row as an explicit exception and named the cost in its own comment ("repeated
runs over the same partial would retrace each other"). Repeated runs are how the
tool is used; `--finalize_repeats` exists for them. So the band that makes them
differ has to be alive on every row.

It sits at 0.30 rather than the beamer's 0.10 because the two tools spend a pass
differently. The beamer gets one pass at a configuration and wants its budget on
what the objective likes best. The finalizer can be re-run over the same partial
as often as it is worth doing, so a wider random band is not a tax on one pass
but coverage across many.

**Gumbel top-K selection on the beam rows was removed.** The idea was to
perturb the sort key with `score/tau + Gumbel(0,1)`, whose top-K is provably a
sample of K distinct boards drawn without replacement with probability
proportional to `exp(score/tau)` (Kool, van Hoof & Welling 2019). It is a
prettier instrument than the uniform `frac_rand` band and it measured worse:
`--gumbel_tau0 1` lost to a plain `--frac_rand 0.10` on 20 of 20 paired
configurations (0.89x row-10 width). The principled sampler did not beat the
blunt one here, so the beam keeps the blunt one and `dedup_and_rank` keeps one
code path instead of two. The primitive survives where it does earn its place --
on the border ranking, below.

**The same primitive on the borders** (`--tau_bottoms`,
`--tau_columns`; 0 = off, the default; the finalizer takes the columns
one). Choosing *which* borders to run has exactly the shape the perturbation is
for -- the enumerated ranking is a top-K of `BottomOrder.rank`/`LeftOrder.rank`,
and the `--random_edges` and finalizer samplers are the K=1 case, an argmax over
32 draws. Above 0, `rank/tau + Gumbel` replaces the plain rank as the comparison
key, so `--top_bottoms`/`--top_columns` become a sample without replacement
rather than the greedy head.

**On the column rank, every measurement of this predating the column rewrite is
void.** The old `left_rank_of` was `sum_r log1p(la_total[c_r])` -- a sum over
rows, hence symmetric in the row index, hence a function of the column's exposed
colour **multiset** alone. And that multiset is invariant:
`classify_deal_from_rotations` pins `g_left_pool` to exactly `EDGE_LEN` pieces
(it is fatal if not), and `rec_left` permutes them, so every enumerated column of
a border row exposes the same colours in a different order. The rank was
therefore *one constant*. At `tau 0`, `cmp_left_rank` fell through to `memcmp`
and ordered columns lexicographically by exposed colour; at `tau > 0` the
constant cancelled out of `rank/tau`, leaving the Gumbel noise alone -- so the
ordering did not depend on tau at all. Measured: `--tau_columns 2` and
`4` chose identical columns config for config across a whole run, while the same
comparison on `--tau_bottoms` differed. That is also the explanation for
the sweep's puzzling finding that columns at ranks 2-4 outlived rank 1 by 2.2x
(p = 0.0008) while bottoms showed no such inversion: there was no column ranking
to invert.

The rank is now real (see **Ranking a left column**, below), so the perturbation
has something to perturb. Re-measure before raising it.

**That argument is structural, and one measurement does not back it.** A
240 s-per-arm, single-seed `--random_edges` run at `--stop_row 11
--incomplete_top`:

| arm | completions/min | partials/min |
|---|---|---|
| baseline | 2.25 | 55.5 |
| `tau_columns 40` | **0.25** | 53.5 |
| `tau_columns 40 tau_bottoms 5` | 7.78 | 67.9 |
| + `bail_columns 3` | 3.87 | 61.0 |

High `tau_columns` **alone came out worst**, which is the arm that tests the
tie-breaking argument directly. The arm that did best changed two knobs at once,
so it isolates nothing. And each arm samples entirely different random borders,
so border luck -- not the knob -- may be most of the spread; with completion
counts in single digits, none of this is significant. Treat the defaults of 0 as
the known-good setting and measure on your own seed before moving either knob.

Reproducibility note: at tau > 0 the ranked order is a seed-dependent
permutation, and `sweep_checkpoint.txt` stores *indices* into it -- so `--resume`
then requires the original `--rng_seed`, and the beamer refuses the combination
without one.

### Scoring

A child's score is the sum of three terms.

**One-row lookahead.** `log n(cell_A') + log(1+f_B') + log(1+f_C')` -- the
exact record count of the database cell the child's tops present to next-row
segment A (whose left color is known), plus fan-out lookups for B and C. The
log of an upper bound on next-row completions; any zero factor is an exact
one-row proof of death and rejects the child (below the stop row).

`f_B'` and `f_C'` come from `db_seg_fanout`, which sums record counts over all
17 left colors **with no reference to `used[]`**: two boards with identical
exposed tops but disjoint remaining piece sets score identically. At row 3 that
is a mild overcount; at row 11, with 154 of 196 pieces consumed, most counted
records are unbuildable and the overcount is large and board-dependent.
There is no correction for this in the beamer any more. `--avail_correct` used
to discount the fan-out by each frontier colour's remaining supply; it was
removed after losing all 61 paired configurations it was measured on (0.82x
against the plain baseline, 0.70x against Mahalanobis, 0.68x against closure).
It rewards holding abundant frontier colours, while the closure term often wants
to spend a colour whose demand is already covered -- the two pull against each
other. The overcount itself is largest at rows 10-11, where selection no longer
discards anything, so correcting it there changes no decision.

**The closure objective** (`--lambda_J`), the primary colour term, derived
rather than tuned. Every free inner half-edge
must eventually meet another of the *same* color; of the `(2A-1)!!` ways to
pair up `2A = sum_c S_c` free half-edges, `prod_c (S_c-1)!!` are
color-consistent, so

```
P = prod_c (S_c - 1)!! / (2A - 1)!!
```

This vanishes exactly when some `S_c` is odd -- so the parity certificate below
and this objective are one formula, its hard part and its soft part. Stirling
turns the log into `-A*H(pi)`, i.e. (up to a constant at fixed depth)

```
J_conc = A_tot * KL(pi || uniform),   pi_c = S_c / sum_d S_d
J_dem  = sum_c D_c * log(R_c / Rbar)
```

`J_conc` reads as "how far this board's remaining color mix has drifted from
flat, weighted by how many pairings are left to make". It is convex, so it
rewards *extreme* profiles -- exhausting some colors -- without naming which:
at a balanced start it is identically zero and has no preference to express.
`J_dem` is the safety rail, penalizing demand for a color whose supply is thin.
Both are in nats, like the fan-out terms, so they add without a fudge factor,
and both carry their own depth dependence -- there is no schedule. Cost is 34
table lookups against `maha_term`'s 16x17 matrix-vector product, so the derived
term is also the cheaper one.

**Mahalanobis color-usage term** (`--lambda_Mahalanobis`, the project's main
heuristic contribution). Let **x** be the 17-vector of inner-color occurrences
consumed by the *n* placed inner pieces, drawn from the population of M = 196.
Under uniform sampling without replacement, **x** has mean `(n/M)*t` (t = the
population totals) and covariance `fn*A` with `A = M*sum_if_if_i^T - t*t^T` and the
hypergeometric scaling

```
fn = n(M-n) / (M^2(M-1))
```

The term computes the normalized Mahalanobis distance `d2n = D^2/E[D^2]` in the
16-dimensional Helmert contrast space (E[d2n] = 1 for a typical random sample)
and adds `schedule(row)*lambda*d2n` to the score; the schedule ramps over rows 3-9,
peaking at row 6. With lambda > 0 this **rewards atypical color consumption** --
empirically, boards that exhaust some colors early leave a healthier supply
for the endgame. (An earlier scarcity-penalty heuristic was removed: it
penalized exactly the pattern the Mahalanobis term rewards, hard infeasibility
is already caught by the parity check, and supply health is already encoded in
the fan-out lookahead.)

### Eternity II clue pieces (`--clue_center`, `--clue_corners`)

The five published hints, converted once into this repo's numbering (0-based
ids, bottom-up rows, CCW spins) and validated against the classic piece table:
all 256 pieces match under `our_id = classic - 1` with a complete, unique
23-colour bijection.

| our piece | cell | (row, col) at 0 deg | spin |
|---|---|---|---|
| 138 (centre) | 119 | (7, 7) | 0 |
| 180 | 34 | (2, 2) | 0 |
| 248 | 45 | (2, 13) | 3 |
| 207 | 210 | (13, 2) | 3 |
| 254 | 221 | (13, 13) | 1 |

Each clue pins a cell **and** a spin. Rotating a solved board 90 degrees keeps
the edge rules but moves the clues, so the clue-satisfying solution set is not
rotation-closed; since our border is chosen by our own search, "which puzzle
side is our row 0" is free and all four orientations must be searched. Every
orientation places the centre plus two corner clues below row 12 and leaves the
other two on row 13 -- which is why `--stop_row` is capped at 12 when a clue
flag is on. The centre visits a different one of the four centre cells per
orientation, so `{119,120,135,136}` are the four centre placements.

A board is unassigned until it places its first clue, then owes that
orientation for life; the orientation lives in `BeamEntry.flags` and joins the
dedup signature, or boards owing different clue sets would merge. `select_beam`
reserves a floor of `K/8` per orientation ahead of the score band, and hands
whatever a thin orientation cannot fill back to merit.

**The constraint bites one row early.** A clue's bottom face must meet the piece
below it, so every clue demands a colour from the row *under* it, and that
demand is met by generating the row with a `TOPCOLOR` pin rather than filtering
the finished children. The distinction is not cosmetic: the beam keeps roughly
one child per A record, so rejecting the children that miss starves the row
instead of reshaping it.

Both rules -- a clue pins its own cell, a clue pins a colour on the cell below
-- are driven off `g_clue`, so the schedule falls out of the table rather than
being written per row. For the row-2 corners it puts two `TOPCOLOR` pins on row
1, in segment A (col 2) and segment C (col 13); measured, 22 surviving boards
filtering against 3328 pinning. For the centre it puts one on row 6 or 7
depending on orientation, which matters just as much: without it the centre row
kept **1075 boards of 104833**, and with it 6069, with row 9 going 194 -> 884 on
the same borders. And for the row-13 pair it puts two on row 12, which is what
makes the attached pieces below meet the board rather than land on whatever
colour the search happened to choose.

`E555_CLUE_DEBUG=1` prints the whole schedule at startup, before any search
runs -- rows 11 and 12 are reached too rarely to be a practical way of checking
what they owe.

**The two unreachable corners are attached to the emitted board.** With
`--clue_corners` on, every emitted board also carries its orientation's two
row-13 clue pieces at their cells. The search never reaches row 13 -- which is
the point: the viewer makes it obvious the corners were pinned, and a hole-free
Stage C solve has to build around them. There is no orientation to choose: the
board committed to one when it placed its row-2 corners.

**The attach yields to the board.** A clue is written only where its own cell
and the cell below it are both empty. Below `--stop_row 12` that is always true
and the pair costs nothing (verified: 294/480 with and without them). At
`--stop_row 12` row 12 is filled, so the pair is simply left off rather than
bolted on to assert two junctions the search never chose and never scored.

That is deliberate, and it is the one place the clues yield to the search rather
than the other way round. Rows 11 and 12 are where the beam nearly dies, and the
row-13 pair is never *searched* -- it is reserved, never pinned, and this program
does not enforce it. Pinning row 12 to make those two junctions match cost the
whole row: a pinned row is generated by the clued expander, which never reaches
the `--incomplete_top` emitters, so every 11-of-16 board at the hardest row in
the run was silently discarded. Since the corner clues are optional and the
backtracker is the better tool that high up, nothing at rows 12-13 is worth
spending a partial on. There is consequently **no clue cap on `--stop_row`**.

**Every later stage can hold them too.** `E555_finalizer`, `E555_roundhouse`,
`E555_topper.py` and `E555_ender.py` all take the same two flags, all default
off, and each holds what its own geometry lets it hold:

| tool | what it can hold | what it cannot |
|---|---|---|
| finalizer | the centre clue and any clue on a searched row, by pinning it during generation | the row-13 pair: reserved and attached, so row 13 must not be searched |
| roundhouse | the four corner clues, by filtering the strip records that would misplace them | nothing -- the centre never leaves the core, so `--clue_center` only verifies it |
| topper / ender | every clue its open region reaches, including all four corners | a clue outside the window, reported and skipped |

The finalizer is the one that mattered most: the shipped pipelines run it at
`--finalize_from = BEAM_STOP - 5` (6 at the default stop row 11), which frees
rows 7 and 8 and so the centre clue's cell. Measured on the delivered clued
boards at that exact setting, a run without the flags kept **2/5 clues on 2146
boards and 3/5 on 226** -- never 5 -- while the same run with them kept **5/5 on
all 222** it emitted. The roundhouse is as stark: a `--rounds 3 --strip_width 5`
cut with `--breaks` returns complete boards holding **1/5** clues without the
flags (only the centre, which it never frees) and **5/5** with them.

**How Stage C holds them.** `E555_topper.py` and `E555_ender.py` take the
same two flags, and without them they treat a clue like any other piece: the
topper's `--side T --band_depth 4` band covers rows 12..15, exactly where the two
row-13 clues sit, and the ender's `--mode ring` opens the whole border with the
centre clue inside any interior break box. Measured on 40 clued row-11 partials,
a single default topper pass drops every board from 5/5 clues to 3/5; with
`--clue_center --clue_corners` all 40 keep 5/5.

Three differences from the beamer are worth knowing. **All four corner clues are
enforceable here**, not just the two on row 2, because Stage C sees the whole
board -- this is the only place entries 3..4 of the table are ever constrained.
**Orientation is inherited, never chosen**: `--clue_orient auto` (the default)
reads off the input board which of the four orientations it committed to, and
only a board carrying no clue at all needs an explicit `--clue_orient 0..3`
(measured unambiguous -- each of 329 clued boards matched exactly one
orientation, never two). The finalizer takes the same flag but reads `auto`
more freely: it *searches* every viable orientation of an unclued board rather
than asking for one, because it is growing rows and can afford four passes,
where these tools are performing one repair. And **a clue the open region cannot reach is reported
and skipped**, not an error: an early window of a sliding-window sweep
legitimately cannot touch the far side of the board.

The clue table itself lives once, in `tools/E555_viewer.py`, transcribed
verbatim from `g_clue` in `E555_database.c` -- the row/column and spin
conventions are identical, so no conversion is involved. `tools/E555_rank.py`
reads it for its `clues` column.

**The database.** Clue pieces are barred from the chain database
(`g_db_exclude`), so no chain can hold one; the pinned walk places them
directly and is exempt from the board's reserved mask. Only the enabled clues
are excluded -- `--clue_center` alone leaves the corner pieces as ordinary inner
pieces, because two of them genuinely sit at row 2 in the solution and
excluding them without forcing them would make the answer unreachable.
Measured record counts: 3.119e9 with no clues, 3.042e9 centre only
(predicted (195/196)^5), 2.730e9 with all five (predicted (191/196)^5). A cache
carries a hash of its exclusion set and refuses a mismatched run, so **use a
separate `--db_file` per clue setting**.

### Color-parity pruning

For every inner color c, let `S = total(c) - consumed(c) - required(c)`, where
*required* counts frontier tops plus all committed future interfaces (left
column, right terminals, top-border demands). Every side of every unplaced
piece is either matched to a requirement or paired internally, so S >= 0
always, and S must be even. O(17) per child, checked before scoring.

**Free mode is no longer exempt** (`--no_free_demand` restores the old
behaviour). Free-edges mode used to zero the left, right and top-border demands
and skip the evenness test, because it does not know which edge piece will end
up on which side. It does not need to: an edge piece carries **one** inner
color, and in either remaining role -- frame-right (inner side faces left) or
frame-up (inner side faces down) -- it exposes exactly that one inner half-edge
into the interior. The split therefore does not change the demand multiset at
all, and free mode's demands are exact without enumerating it. The bookkeeping
is one used-tested pass over the edge pool (which in free mode holds all 56
non-corner edges) plus the left column, and it reproduces the fixed-mode count
`sum_c D_c = 56 - 2r` exactly at every depth -- fixed mode is the same rule with
the roles pre-assigned.

**What the evenness bit is actually worth: nothing.** Writing `adj_c` for the
interior adjacencies already formed and `B_c` for the number of edge pieces
whose inner color is c, the same accounting gives

```
S_c = tot_c - B_c - 2*adj_c - 2*frontier_c
```

so `S_c = tot_c - B_c (mod 2)` for every board at depth >= 1 -- the parity of S
is a property of the **seed**, not of the board. On `data/seed_Edge5.txt`
`tot_c - B_c` is even for all 17 colors (it must be, for any seed admitting a
solution), so the evenness test can never fire. Measured over 27.6 M checks in
a free-mode finalizer run: 0 rejections on parity, 2720 on `S < 0` (0.01%). The
test is kept because it costs nothing and would fire immediately on a
malformed seed -- but it is not a source of pruning, and the exact free-mode
demands matter only through the `S >= 0` half.

### A certificate that was tried and removed (`--supply_check`)

Worth recording because the negative result is the useful part. A Hall-type
condition counted **pieces** where the parity test counts half-edges, so
neither implied the other: each of the 14 frontier columns needs a distinct
remaining inner piece carrying its exposed top color, and columns demanding the
same color have identical candidate sets, so the singleton case was

```
for each inner color c:  #{columns demanding c} <= #{unused inner pieces carrying c}
```

It never binds. A color is carried by ~40 of the 196 inner pieces and 14
columns cannot exhaust that until the board is very nearly full -- measured, it
rejected 0 of 25 M candidates in the finalizer, and two independent 24-minute
beamer runs with and without it agreed on config count, extinctions and
row-11 yield to within noise. The option and its `g_color_pieces` table were
deleted rather than left off by default: an inner-loop test that provably
cannot fire is cost without pruning, and a knob nobody should set is a knob
that misleads.

### Random border mode (`--random_edges`)

No Stage A input at all: the solver samples borders directly from the seed's
4 corner + 56 edge pieces. A bottom sample assigns corner roles at random and
grows a random legal chain of 14 frame-down edges BL→BR; a left-column sample
grows a chain BL→TL from the edges the bottom did not consume. Each published
border is the best of 32 samples by the same fan-out measures used for
enumerated borders. Corners can be pinned with `--BL/--BR/--TL/--TR <piece>`.
Free-edges mode is implied; `--samples` = number of random bottoms and
`--top_columns` = random left columns per bottom. Typical use is a long
unattended run stocking varied partials for the finalizer and Stage C.

### Determinism and reproducibility

Runs are intentionally **not** reproducible unless `--rng_seed` is given: the
master seed defaults to a clock/PID mixture and is printed in the `[cfg]`
banner. Passing that seed back reproduces the sampling decisions on the same
build; bit-exact replay across thread counts is not a design goal.

### CLI summary

```
bin/E555_beamer seed.txt [rotations.csv] [options]
```

| option | default | meaning |
|---|---|---|
| `--out_dir DIR` | `beam_out` | output directory (completions CSV + checkpoint) |
| `--start_row N` | 0 | first rotations-CSV data row to use (fixed mode only) |
| `--num_rows N` | 0 | consecutive border rows to sweep (fixed mode only; 0 = every remaining row) |
| `--samples N` | 1 | random bottoms to try (random mode only; 0 = uncapped, governed by `--wall_time`/`--max_emitted`) |
| `--db_file PATH` | -- | on-disk DB cache (~6.5 GB; built on first run) |
| `--free_edges` | off | any edge piece may terminate a row (relaxed parity) |
| `--random_edges` | off | sample borders from the seed; rotations CSV optional |
| `--BL/--BR/--TL/--TR P` | -- | pin corner piece P (random mode) |
| `--incomplete_top` | off | also emit stop-row boards holding two of the three segments -- A+B, A+C or B+C |
| `--beam_width K` | 250000 | boards kept per row |
| `--stop_row R` | 11 | last row filled (1-13); the beam fills 11 and dies at 12, so 11 emits |
| `--beam_expand E` | 4 | late-search width multiplier |
| `--beam_expand_row R` | 7 | row with the full ExK width |
| `--lambda_J F` | 1.0 | weight of the CLOSURE term, the primary color objective (useful 0.5-1.5) |
| `--lambda_Mahalanobis F` | 0.6 | weight of the piece-structure correction, in units of its own measured per-row SD (useful 0.3-0.7) |
| `--no_free_demand` | -- | **disable** the free-mode demand accounting (on by default) |
| `--frac_rand F` | 0.10 | random selection band, flat across rows |
| `--parent_cap N` | 4 | children per parent in the score band |
| `--pool_factor N` | 8 | candidate-pool target, x beam width |
| `--bc_window nB,nC` | `3,3` | while the beam is FULL, score up to nB x nC (B,C) completions per A record and keep the best; while it is BELOW capacity, enumerate and keep every one |
| `--top_bottoms N` | 10 | ranked bottom orderings tried per border row |
| `--top_columns N` | 12 | ranked left columns per bottom, ranked separately for each bottom |
| `--tau_bottoms T` | 0 | selection temperature for the bottom ranking (0 = off) |
| `--tau_columns T` | 0 | ditto for left columns (every measurement predating the column rewrite is void: see above) |
| `--bail_columns N` | 0 | abandon a bottom after N consecutive columns that emitted nothing (0 = off) |
| `--clue_center` | off | force the published centre clue (piece 138) onto its cell, at its orientation's spin |
| `--clue_corners` | off | force the two reachable corner clues (row 2); the row-13 pair is reserved, never pinned |
| `--time_limit S` | 600 | wall-time slice per configuration |
| `--wall_time S` | 0 | total budget (0 = unlimited) |
| `--max_emitted N` | 0 | stop after N boards reported -- completions **plus** `--incomplete_top` partials (0 = unlimited) |
| `--resume` | off | continue from the sweep checkpoint |
| `--threads N` | all | OpenMP threads |
| `--rng_seed S` | random | master RNG seed |
| `--verbose` | off | per-row `[beam]` progress lines |

`--bc_window` is the one place where extra compute buys objective rather than
more candidates. Without it `try_A` commits to the **first** conflict-free
(B, C) and returns, so segments B and C -- 10 of the row's 14 pieces -- are
chosen by the database's global, board-blind fan-out sort: the score filters an
unbiased sample but never steers it. With a window open, up to nB workable B
chains x nC C completions are scored with the same formula used for selection and
only the best is kept. **While the beam is at capacity, exactly one child per A
record survives** -- there the window is not a narrowing, it is the same width
with a better choice inside it. While the beam is BELOW capacity the window
opens instead: `select_beam` discards nothing once the pool stops filling the
row width, so the other candidates are not losers but completions being thrown
away, and every one is kept (quota still bounds the total). There is no
counterpart in the finalizer, whose beam rows already enumerate *every*
conflict-free (B, C) -- the defect the window fixes does not exist there.

The retry budget has to grow with the window or it cannot fill: `b_left` is
spent on every conflict-free B chain, while `nb_done` counts only a B chain that
*produced* a child, so at nB = 3 the loop must find three productive B chains
inside `B_TRY` conflict-free tries. Deep rows are conflict-dominated, and with a
fixed `B_TRY` the window is starved -- an early sweep measured only +5% for
`2,2` and `3,3` for exactly this reason, which is a measurement of the
starvation, not of the window. The budget is `B_TRY + nB - 1`, which is `B_TRY`
at nB = 1.

Where the default comes from -- two seeds, `--random_edges`, `--beam_width
200000`, `--stop_row 11`, ~16 min of sweep per arm, metric **distinct
rows-0..10 foundations per minute** (Stage C consumes foundations, and the 4.9
`--incomplete_top` siblings per foundation make partials/min a misleading
count):

| window | found/min (2 seeds) | vs `1,1` | borders/hour | reached stop row | died at stop row |
|---|---|---|---|---|---|
| `1,1` | 657 / 600 | -- | 234 / 260 | 61% / 52% | 14 / 16 |
| `2,2` | 824 / 759 | +26% | 174 / 181 | 70% / 60% | 8 / 11 |
| **`3,2`** | **913 / 804** | **+37%** | 154 / 151 | **83% / 83%** | **1 / 2** |
| `3,3` | 867 / 795 | +32% | 147 / 147 | 77% / 69% | 3 / 7 |

`3,2` wins on both seeds and on both criteria at once, which is the important
part: it examines ~35% *fewer* borders per hour, yet more of them survive to the
stop row and each yields more foundations. The window buys depth and throughput
together rather than trading one for the other. Beyond `3,2` the extra column
costs more than it returns.

### Performance (reference set, 4 laptop threads)

| phase | cold | with `--db_file` cache |
|---|---|---|
| inner DB build (2 passes) | ~40-60 s | -- |
| promise sort | ~2-3 min | -- |
| startup to first config | ~3-5 min | seconds (lazy page-in) |

RAM at defaults: 6.4 GB DB + ~2 GB beam workspace + ~0.4 GB tables; the
workspace scales linearly in `beam_width x beam_expand` (~9 KB per unit).

---

## E555_finalizer -- resuming from a partial board

`src/B_beam/E555_finalizer.c` (same database module; shares the beam machinery)

```
bin/E555_finalizer seed.txt partials.csv [rotations.csv] --finalize_from 10 --stop_row 14 ...
```

**Settings track the beamer's where the meaning is the same** -- `--beam_width
250000`, `--beam_expand 4`, `--parent_cap 4`, `--lambda_J 1.0`,
`--lambda_Mahalanobis 0.6`, `--pool_factor 8`, `--top_columns 12`,
`--stop_row 11`, `--time_limit 600` -- so one number means one thing across
Stage B, and
`--bail_columns` exists here too (it abandons a partial line after N consecutive
columns that report nothing). Two deliberately differ:

- `--frac_rand 0.30` against 0.10, for the reason given above.
- `--beam_expand_row 8` against 7. The row number is absolute in both, but this
  search starts at `finalize_from + 1`, so at the default it is already past the
  threshold on its first row and the two numbers do not mean the same thing.

`--bc_window` has no counterpart here on purpose: this tool enumerates *every*
conflict-free (B, C) completion of an A record rather than scoring a window and
keeping the best, which is the right economy when the beam grows from a single
locked board over a sparse database.

**The Mahalanobis correction needs a row that was actually searched.** It is
denominated in the per-row spread of `d2n`, measured live, and the beamer can
simply use row `r-1` because it searched it a moment ago. This tool cannot: its
first searched row is `finalize_from + 1` and everything below is locked, so
`r-1` has no sample at all -- at the default that is row 9 normalising against
row 8. A run short enough to search only two rows therefore never applied the
correction once, whatever `--lambda_Mahalanobis` said. It now prefers *this*
row's own spread, which an earlier configuration over the same database will
have measured, then the row below, then the nearest measured row either way.
Only the very first configuration of a run scores its first row uncorrected.

Two things made that worse than it had to be, both fixed in both binaries: a row
whose sample fell under the 64-sample floor used to **zero** the stored spread
rather than leave the last good estimate standing, so one narrow configuration
stripped the calibration every later one would have scored against; and the
`--verbose` table printed nothing at all when no row was measured, which reads
as "no correction needed" rather than "the correction never ran". It now prints
the sample count behind each figure and says so explicitly when there is none.

**Locking.** `--start_row/--num_rows` select the CSV lines, and
`--num_rows 0` (also the default) reads to the end of the file. Each line is structurally
validated (piece types per cell, frame orientation, every color match inside
the locked region). Pieces at or below `--finalize_from`
(default 5) are locked; pieces placed above return to the pool. That default is
low on purpose: the beam grows from a single locked board, so it needs rows to
widen in before selection means anything -- see below.

**Input dedup.** Long partial lists are full of near-siblings that seed
*identical* searches once their top rows are freed. Every line is hashed on
exactly what the search will see (the placement of the `finalize_from` row,
fixed borders above it in fixed mode, and the free-piece set); duplicates are
skipped, consistently across chunked runs (a prescan hashes the lines before
the window). `--finalize_repeats` deliberately re-runs a seen partial with
fresh randomness.

**The reduced database.** Rebuilt per partial with the locked pieces excluded
outright: with half the board locked the chain DFS shrinks super-exponentially
(a `finalize_from 10` database builds in ~2 s -- 5.5 M records vs 3.1 G) and
contains no chain that could be rejected for reusing a locked piece. That is
what makes rows 11-14 searchable at full width, in trivial memory, with no
disk cache. Consecutive lines sharing a locked set skip the rebuild -- with
clues on, the reuse key is the locked set *plus the clue pieces*, since two
partials can share a locked region and still owe different ones.

**`--clue_center` / `--clue_corners`.** Both default off; with neither, this
program behaves exactly as before. They matter here more than anywhere else in
the pipeline, because the shipped `--finalize_from = BEAM_STOP - 5` frees the
centre clue's row and the search then quietly refills that cell with something
else (measured above: never 5/5 without the flags, always 5/5 with them).

Unlike the beamer, which explores all four orientations at once and pays for it
with orientation bits in the beam entry, a term in the frontier signature and a
reserve pass in selection, the finalizer searches **one orientation per pass**.
None of that machinery exists here; the orientation is folded into the
input-dedup hash instead, because everything freed above the lock collapses to
one bucket there and two lines differing only in orientation would otherwise
merge.

**`--clue_orient LIST`** (`auto` by default, or a comma list from `0,1,2,3`)
decides where that orientation comes from, and the distinction it draws is
between *reading* one and *choosing* one:

- a line **carrying** a clue has committed, so its orientation is read off the
  board and never reconsidered -- naming a different one in `--clue_orient`
  skips the line rather than re-orienting it;
- a line carrying **none** has committed to nothing. Every partial locked below
  row 7 is such a line under `--clue_center`, the centre clue being the only
  reachable one and sitting on row 7 or 8. It is searched once per orientation
  the lock does not already contradict.

Up to four passes, then, over **one** database: the five clue pieces are the
same set in all four rows of `g_clue` -- only their cells and spins rotate --
so the reduced database and the reserved mask are shared and only the pins
move. The passes share the run's column-sampling stream, so each sees different
left columns; that is more coverage, at the price that a pinned re-run does not
reproduce the columns the auto run gave that orientation.

A clue on a searched row is pinned during generation, via the same
`enumerate_pinned_segment` walk the beamer uses; a clue on the row *above* a
searched row pins a colour there instead, because a clue's bottom face has to
meet whatever sits under it. Rows below the lock are checked, not searched: a
partial whose locked region already contradicts a clue is skipped with the
reason, since no row above it could repair the board. The row-13 pair is
reserved and attached to the emitted board exactly as in the beamer -- attached
only where its own cell and the cell below are empty, so a searched row 13 or a
filled row 12 simply leaves it off. Nothing about the clues caps `--stop_row`.

**Side modes.** Without `--free_edges`, the partial's border placement is used
as a fixed assignment (requires all 60 border pieces in the line; otherwise
free-edges activates automatically -- beamer partials always trigger this). In
free mode the left column above the locked rows is sampled: `--top_columns`
columns per partial, each best-of-32 by fan-out. `--top_columns 0` instead
**enumerates every legal left column exhaustively** up to the stop row -- with
`--frac_rand 0` and a sufficient width this recovers a known solution with
certainty (the basis of the regression test in `tests/run_tests.sh`).

**Sides from the annealer** (optional third positional, normally the very
rotations CSV the beamer was given). Free mode is the price of an incomplete
border, and it is steep: all 56 edges become candidates for the left column
*and* for the right terminals -- discarding exactly the side structure Stage A
was run to produce. Given the rotations file, each partial's **locked** border
(rows 0..`--finalize_from`, which are always fully placed) is compared against
every row; the first row that assigns those same pieces to those same sides is
re-imposed. The left column is then enumerated from the annealer's 14 left
edges instead of 56, and right terminals and top demands come from the row --
while the column *ordering* is still searched, so `--top_columns 0` keeps its
meaning. On the synthetic regression this cuts a `finalize_from 10 --stop_row
14` sweep from 593 legal left columns to 2, with the known solution still
found.

The saving is now purely in the size of the search space. It used to be larger
on paper: free mode also zeroed the top-border demands and switched the
even-parity prune off, so re-imposing a row restored a certificate as well. It
no longer has to -- free mode's demands are exact in their own right (see
**Color-parity pruning**), so both modes now carry the same colour tests.

Only the locked border is compared, deliberately. Pieces above `finalize_from`
return to the pool and are re-searched, and a beamer run with `--free_edges`
would not have respected any row up there -- requiring agreement would produce
false misses in exactly the case the feature is for. Pieces at or below it stay
on the board, so agreement there is not evidence but a *precondition*: a locked
right-column piece the row called "top" would have `build_top_border_demands`
reserve a color for a piece already placed. A partial matching no row is
searched exactly as before, with a note; the file is ignored outright under
`--free_edges` or when the border is already complete. Matching is one spin
comparison per locked border cell (a border piece's rotation on a border cell
is already forced to face the frame, so same-spin *is* same-side), and rows are
structurally validated at load, so a match can never fail mid-sweep.

**Row 14.** `--stop_row` may go up to 14 (15 is rejected: the top border is
deliberately outside the database, and placing it on a finished row 14 is
trivial). Committing row 14 counts its tops as *satisfying* the top border,
and the parity invariant holds with equality on a completed inner board.

**Search differences from the beamer** (deliberate): (1) beam rows enumerate
*every* conflict-free (B,C) completion of each segment-A record -- necessary
over a sparse reduced database; (2) the first searched row always uses the
full random band, so every repeat injects fresh variability.

`--tau_columns` works here as in the beamer: above 0 the sampled left
column is drawn in proportion to `exp(rank/tau)` rather than being the best of
the 32 samples. It has a second use on this side -- repeats dedup the columns
they have already tried, so a mode-seeking argmax keeps re-drawing the same
winner and exhausts the pool early; sampling spreads the draws out. It does
nothing under `--top_columns 0`, which enumerates exhaustively and is what the
regression test uses.

The scoring and certificate options are shared with the beamer and mean the
same thing here, with one exception: **`--bc_window` does not exist in the
finalizer**, because (1) above already does more than any window -- the defect
the window fixes is the beamer's "take the first (B, C) that fits", and this
loop never did. The closure term is a better fit here than the
Mahalanobis one: `maha_d2n` infers the placed-piece count from `n = 14*row`,
whereas closure reads the colour counts straight off the board and stays exact
from a locked partial at any `--finalize_from`.

**Output** appends to `beam_completions_finalized_<stop_row>.csv` with config
ids `p<line>r<repeat>l<column>`; several instances on the same machine may
share one output file (each line is one atomic append).

**Bounding a run.** Both tools accept `--wall_time` (time) and
`--max_emitted` (output): the latter ends the run once N boards have been
written, counting completions and `--incomplete_top` partials together. The
stop-row beam in flight is always reported in full, so the final count
overshoots N by up to one beam width. Because the CSVs are *appended* to, the
`[sweep]` line of a configuration that produced something reports both the
per-config counts (`emitted=`, and `partials=` under `--incomplete_top`) and the
run totals written so far (`sol_total=`, `part_total=`) -- a fresh run into a
used `--out_dir` starts its totals at 0 while the file keeps growing.

---

## E555_roundhouse -- rotating the board and refilling a strip

`src/B_beam/E555_roundhouse.c` (same database module; builds its own chains)

```
bin/E555_roundhouse seed.txt partials.csv --rounds 1 --strip_width 4 ...
```

The beamer grows a board one **row** at a time, so its frontier is 16 colors
wide: it cannot be enumerated, cannot be counted, and admits no exact
feasibility test. The roundhouse turns the board 90 deg and grows a W-wide
vertical **strip** instead. Every strip level is one segment-C lookup --
`W-1` inner pieces plus a frame-right edge terminal -- so the frontier is W
colors wide, and three things follow that the row-wise search cannot have.

### The width-W chain database

`CHAIN_LEN` in the shared module is a compile-time 5; the roundhouse builds its
own chains so that W is a knob. Only **edge-terminal** chains are ever needed
(each level ends on the right border), which is the small half of the database:

| W | cells `17^W*5` | records (no exclusions) | size | frontier states `17^(W-1)*5` |
|---|---|---|---|---|
| 5 | 7.10 M | 228 682 101 | 0.51 GB | 417 605 |
| 4 | 418 k | ~ 5.0 M | ~11 MB | 24 565 |
| 3 | 24.6 k | ~ 108 k | ~0.3 MB | 1 445 |
| 2 | 1.4 k | ~ 2.3 k | trivial | 85 |

Excluding the retained board shrinks it further -- a W=5 three-round core leaves
47.8 M records (~0.11 GB, ~2 s); a W=5 single strip on a row-12 partial leaves
634 k. **No 6.4 GB inner arena, no promise sort, no `--db_file`.**

### The oracle

Drop the rule that a strip may not reuse a piece and what remains is a layered
graph: nodes are `(level, signature)`, arcs are database records. One backward
sweep computes exactly which signatures can still finish the strip (a bitset per
level) and in how many ways (`g_cnt`). It costs one pass over the records
carrying each wall color -- ~13.4 M decodes per level at W=5, so **0.4 s for a
whole strip**; microseconds at W=3.

What it buys: a single bitset test prunes any branch dead on color grounds, so
the only way a live branch can still fail is piece reuse; the level-0 border
chains are filtered exactly (of 7 341 raw chains on a real board, 5 541 live);
the counts rank those chains and drive the uniform sampler; and an empty live
set at level 1 **proves** the wall dead. `--verbose` prints the live count per
level, which names the level at which the coloring collapses -- the most
diagnostic number the tool produces.

`--selfcheck` re-counts the same relaxation by brute-force enumeration and
compares signature by signature. It is the regression that makes every prune
trustworthy, and it is wired into `tests/run_tests.sh`.

The relaxation is strong while the pool is rich and weak in the last round,
where exhaustion rather than color is what kills. There the **parity/supply
prune** carries the load: whenever the strip is the last unfilled region (cells
remaining == pieces unplaced, checked at every node), every side of every
unplaced piece either faces a known boundary -- the frontier below, one wall
color per remaining level, or the frame -- or pairs with another unplaced piece
inside the strip, so each inner color's surplus must be non-negative *and even*.

### Geometry

Work in a **frame**: the board mirrored by `--reverse`, then rotated `--rotate`
quarter-turns clockwise, so
the strip is always the rightmost W columns. A rotation maps `(r,c) → (15-c, r)`
and spin `s → (s+3)&3`. Boards are emitted in the input's orientation.

`--rounds` sets how many W-wide bands are freed **and refilled** -- right, then
top, then left. The cuts nest, so each round frees exactly what it will put
back, a lower `--rounds` is a cheaper experiment rather than a truncated one,
and a run that completes never leaves a hole behind:

| `--rounds` | frees | keeps at W=5 | rebuilds |
|---|---|---|---|
| 1 | right band, `W*16` | `11x16 = 176` | one side |
| 2 | + top band, `W*(16-W)` more | `11x11 = 121` | two sides |
| 3 | + left band, `W*(16-W)` more | `6x11 = 66` | three sides, all 4 corners |

**All three end on a complete board when they succeed.** The last round's strip
runs to row 15 and its top row is closed by a border chain -- `W-1` top edges
plus a corner, matched against the level below and the piece to its left -- so
nothing is handed over half-done. That closure is also a prune: without it the
search would commit to tops no remaining border piece can sit on.

Succeeding is the rare case. On the real seed the strips die well short of the
top, and the useful output is `--emit_deepest`: the deepest board reached, whose
empty region is the rest of the strip it died in plus any band it never started.
Those are the boards Stage C closes -- a completion means the puzzle is solved,
which so far happens only on the synthetic set.

**`--rounds 1`** refills one strip. If it covers the whole empty region the run
either returns a **complete 256-piece board** or exhausts -- and exhausting is a
theorem: this board and this pool admit no perfect completion. On the seven
`data/best_463.csv` boards, freeing the top four rows is refuted in milliseconds
each.

**`--rounds 2`** frees the right and top bands. Those two strips plus the kept
`(16-W)x(16-W)` square tile the board exactly -- `(16-W)^2 + W(16-W) + 16W = 256`
 -- so it also ends complete, while rebuilding two sides of the frame and
exercising the rotation between rounds. The natural first experiment: if a board
closes in two rounds, this is what shows it.

**`--rounds 3`** spirals counter-clockwise around a retained core. In original
coordinates that core is `rows W..(15-W) x cols W..15`, hugging the right
border, and its top, left and bottom boundaries are the walls of rounds 1, 2
and 3:

```
      cols 0..W-1            cols W..15
    +-----------+---------------------------+
    |  round 2  |          round 1          |  rows 16-W..15
    |           +---------------------------+
    |           |       RETAINED CORE       |  rows W..15-W
    +-----------+---------------------------+
    |               round 3                 |  rows 0..W-1
    +---------------------------------------+
```

It re-searches **54 of the 60 border pieces and all four corners** at W=5,
which no other stage does.

W defaults to the **narrowest width whose kept region is complete and
break-free**; on a board filled in whole rows from the bottom that is exactly
`16 - rows filled`, so a partial filled through row 12 gives chains of 3 and one
through row 11 gives chains of 4. Raise `--strip_width` to free already-solved
rows deliberately.

The geometry is forced, not chosen: requiring each rotation to land the next
wall on column `15-W` gives core height `16-W` and core width `16-2W` uniquely
(check: `(16-2W)(16-W) + 2W(16-W) + 15W + W = 256`). Note that all four center
cells lie inside the `--rounds 3` core at every W, so a center clue piece's
placement is inherited from the input and can never be created by a strip.

**`--clue_center` / `--clue_corners`** follow from exactly that. The centre is in
the core at every width and round count, so `--clue_center` here can only verify
it -- a board whose core contradicts a clue is skipped, since no strip could
repair it. The four corner clues at (2,2) (2,13) (13,2) (13,13) *are* freed, and
those the flags hold. Enforcement is a filter inside `strip_dfs`'s record loop,
not a pinned enumerator: this search is exhaustive and has no beam to starve, so
rejecting a record at the level its clue sits on simply prunes the subtree, and
the "clue's bottom must meet the piece below" condition is already guaranteed by
`rh_decode` matching every record against the level below. A second guard bars a
clue piece from any cell but its own, so round 1 cannot spend a piece round 3
still needs. `--breaks` respects both, or the dive would scatter the clues
the proof engine just held. The oracle stays deliberately clue-blind: it solves
the colour-only relaxation, and ignoring pins keeps it admissible.

### --rotate: which side each round attacks

The bands are always freed right, top, left **in the frame**; `--rotate` decides
what that means on the input board. Negative values are the same turns the other
way (`-1` == `3`), so `--rotate -1` leaves the input's left side as the frame's
bottom. The last column is where a failed `--rounds 1` strip leaves its hole,
because a strip fills from one end of its band to the other:

| `--rotate` | round 1 | round 2 | round 3 | core hugs | residue corner |
|---|---|---|---|---|---|
| 0 | right | top | left | bottom | top-right |
| 1 | top | left | bottom | right | top-left |
| 2 | left | bottom | right | top | bottom-left |
| 3 or -1 | bottom | right | top | left | bottom-right |

The default `K=1` attacks a Stage B partial's unsolved top **first**, while the
pool is still rich - most-constrained-first, and the right default. `K=-1`
attacks the top last and re-cuts the bottom band first. That band is the one the
pipeline fixes at row 0 by random sampling and never revisits, so `K=-1` is the
setting that actually asks *was the bottom border the problem?* It also carries
a witness: the input's own pieces are a legal filling of that band, so round 1
provably has a solution, which makes a round-1 failure diagnostic rather than
normal. Expect it to die earlier overall, since it reaches the hard region with
a depleted pool.

### --reverse: the spiral the other way round

Every strip level ends on a frame-**right** edge terminal, so the spiral has one
handedness and the four rows above are all `--rotate` can offer. `--reverse`
mirrors the board left-right instead and gives the other four:

| `--reverse --rotate` | round 1 | round 2 | round 3 | core hugs |
|---|---|---|---|---|
| 0 | left | top | right | bottom |
| 1 | top | right | bottom | left |
| 2 | right | bottom | left | top |
| 3 or -1 | bottom | left | top | right |

Reflecting the board swaps each piece's left and right colours and negates its
spin: a placement `(p, (r,c), s)` mirrors to `(p, (r,15-c), (4-s)&3)`, the piece
id untouched, and mirroring twice is the identity. The mirrored pieces do not
exist in the box, but they are a legal seed - swapping left and right preserves
each piece's grey count, and a corner's two greys stay adjacent - so every
derived table builds unchanged and the whole search simply runs in the mirror.
Boards are mirrored on the way in and back on the way out, so nothing outside the
process ever sees a mirrored piece. `--rotate` keeps its meaning for round 1 at
odd `K`: `--rotate 1` still attacks the input's top either way.

It does **not** free a region `--rotate` cannot already free. The kept core is
either mirror-symmetric or lands on another `--rotate`'s core - at `--rounds 3
--strip_width 5` the 11x6 core sits on columns 5..10, dead centre, so `--reverse
--rotate 0` keeps exactly the same 66 cells. What changes is the **search**: each
band is traversed the other way with the wall on the other side, so the strip DFS
and the oracle's layered graph are different problems over the same cells, and
because the rounds nest, the order in which bands are rebuilt decides which
partial the run reaches. Run to exhaustion the two directions prove the same
theorem. The value is in the cut that does *not* exhaust - `--rounds 3
--strip_width 5`, the one the sizing note above admits ends on a budget - where
the two reach different deepest boards. Eight distinct searches instead of four.

### What gets reported

The search is **exhaustive and deterministic** - no beam, no sampling, no random
seed - so it enumerates every break-free filling of the freed bands, and both
outcomes are exact. A complete board is a solution; finishing without one proves
this core admits no break-free refill of these bands. The only thing that can
weaken that is a budget (`--max_nodes`, `--time_limit`, `--wall_time`,
`--max_emitted`); when one bites, the summary marks that board **TRUNCATED**
instead of exhausted.

What comes out is **the furthest it got**: one board per input board, the state
that placed the most pieces. Pieces placed is the only ranking used, because it
is the one measure comparable across rounds - a per-strip level restarts each
round. `--ties N` widens that to N boards at the same depth, dropping any that
repeats an earlier one with a single frontier piece swapped, since those collapse
into the same board the moment a later stage frees the frontier.

Boards go to `<out_dir>/roundhouse_round<N>_rot<K>[rev]_W<w>_miss<B>.csv`,
appended one atomic line at a time, duplicates suppressed. The name carries
`--rounds`, `--rotate`, `--reverse` and `--strip_width`, so runs with different
geometry never share a file -- while runs with the *same* geometry do, which is
what lets a corpus sweep accumulate.

**Two files, split by breaks.** A board with no mismatch goes to `miss0`; one
with mismatches goes to `miss<--breaks>`. So `miss0` is always a corpus you
can trust break-free and the two never have to be told apart afterwards. Routing
is on the board's *own* break count, so a greedy fill that happens to land
perfectly is filed with the clean boards. Both open on first write, so a run that
emits nothing leaves nothing behind. Three kinds of board:

| kind | when | id tag |
|---|---|---|
| solved | the board is complete **and** break-free: the puzzle, for this cut | `s` |
| deepest | the furthest the exhaustive break-free search got | `d` |
| filled | `--breaks B` bought a complete board with at most B mismatches | `f` |

Ids are `p<line><tag><n>`, `n` counting boards written by this run, so every line
is uniquely named and stdout names the id it just wrote.

**Boards are always written in the input's orientation** - the frame is rotated
back (and un-mirrored) first, so cell `(r,c)` means what it meant in the input
whatever `--rotate` and `--reverse` were. Every placed junction matches, so `score` is 480 minus the junctions a hole
still leaves unrealized - it falls with every *empty* cell and never with a
mismatch. A 191-piece board scoring 350 is perfectly matched, not damaged;
compare a partial with a partial, or re-score both with `tools/E555_rank.py`.

*Worked example.* A partial correct through row 11, `--strip_width 4 --rounds 1
--rotate 1`: the strip is the input's rows 12..15 and fills from column 15
leftwards, so a strip dying after level `d` leaves rows 12..15 of columns
`0..14-d` unplaced - a rectangular hole in the **top-left** corner, 4 rows deep
and `15-d` wide, with the rest of the board matched. `--rotate` chooses which
corner that is.

**Emitting nothing is itself a result.** If the oracle kills every level-0 border
chain the search never starts and no board is written: a proof, not a silent
failure. The run prints `REFUTED ... colour alone rules this band out` with the
level the relaxation dies at, and the summary repeats it with the fix to try
next. Because the oracle ignores the piece supply, a refuted band cannot be
filled by *any* arrangement of *any* pieces.

**`--breaks B` finishes the board anyway.** A break-free refill usually does
not exist, and a board with 80 empty cells is awkward to hand on. With `B > 0`
the run takes the deepest break-free board it found and greedily fills every
remaining cell, spending at most B mismatched junctions - the same idiom as the
backtracker's `--break_mode stuck`: most constrained cell first, prefer a piece
that fits exactly, break an edge only when no cell has an exact fit. It is a
dive, not a search: no backtracking, and B is not proved minimal. It always
completes when B is large enough, because a cell's frame type fixes which pieces
may sit there and the type counts stay balanced (4 corners, 56 edges, 196 inner).
The exhaustive part is untouched - the fill runs afterwards, from its best board.

*Measured on the real seed*, three complete 455-457 boards whose breaks all sit in
rows 13-15: `--rounds 1 --strip_width 3` frees 48 cells and refills them for
27-40 breaks; `--rounds 3 --strip_width 4` frees 160 and costs 48-52. Roughly one
break per two cells filled. The fill is there so Stage C receives a full board
rather than a hole, not because it beats the board you fed in.

Volume is set by `--ties` (boards kept at the deepest reach), `--max_emitted` and
`--only_complete`. There is no diversity knob and no need for one: the search
enumerates every break-free filling, so what limits the output is the depth the
board reaches, not which subtree the engine happened to explore.

### What to expect

Naive first-moment branching per level,
`mean_cell x (avail_inner/196)^(W-1) x (avail_edge/56)`:

| W | round 1 | round 3 start |
|---|---|---|
| 5 | ~ 7.4 | ~ 0.09 |
| 4 | ~ 1.9 | ~ 0.06 |
| 3 | ~ 0.6 | ~ 0.04 |

Round 3 is subcritical at every width: the roundhouse **relocates** the wall the
beamer meets at rows 12-15, it does not remove it. Measured on
`data/board_partial_row12.csv` at W=5, round 1 has 4.7*10^9 relaxed completions
and finishes freely; round 2 dies around level 6-8. Every emitted board is
break-free by construction, so its score is `480 - unrealized junctions` and it
feeds any stage unchanged.

### CLI summary

| option | default | meaning |
|---|---|---|
| `--out_dir DIR` | `round_out` | output directory; names carry the geometry, and break-free boards are filed apart from break-bought ones |
| `--start_row N` / `--num_rows N` | 0 / 0 | first input CSV data line, and how many (0 = to the end of the file) |
| `--strip_width W` | 5 | 2..5; chain length, and hence the core. 0 = narrowest usable |
| `--rounds N` | 3 | 1..3; bands freed and refilled (right, top, left) |
| `--rotate K` | 1 | quarter-turns before the cut, -3..3; negative turns anticlockwise |
| `--reverse` | off | spiral the other way round, by mirroring the seed; a second exhaustive attack on the same cells, not a new region |
| `--stop_row R` | last level | stop each strip at this level instead of its last |
| `--BL/--BR/--TL/--TR P` | -- | pin a corner piece by its role on the **input** board |
| `--breaks B` | 0 = off | after the exhaustive search, greedily fill the rest of the deepest board, spending at most B mismatches |
| `--max_nodes N` | 0 | node budget per input board |
| `--time_limit S` | 600 | wall-time budget per input board |
| `--wall_time S` / `--max_emitted N` | 0 / 0 | budgets for the whole run |
| `--ties N` | 1 | boards to emit at the deepest reach |
| `--only_complete` | off | emit only boards with all 256 pieces placed |
| `--selfcheck` | off | validate the oracle against brute force and exit |
| `--threads N` / `--verbose` | all / off | as Stage B |

Retired in the exhaustive rewrite and now accepted with a warning, so older
scripts keep running: `--beam_width`, `--mode`, `--frac_rand`, `--repeats`,
`--finalize_repeats`, `--lambda_Mahalanobis`, `--top_bottoms`,
`--emit_each_round`, `--emit_deepest`, `--rng_seed`, `--max_partials`,
`--free_edges`. The search is exhaustive and deterministic, so none of them
have anything left to do, and the deepest board is emitted by default.

### What the input has to satisfy

Only the **kept region** is validated, and it is validated completely: every
cell placed, every piece seated legally against the frame, every junction inside
it matched. Everything outside is freed, so **breaks, holes and mis-seated
pieces out there are ignored** -- a board whose top rows are broken is exactly
the board you want to hand a strip run, and a board with no complete row at the
bottom is fine as long as some width's core is intact. A break *inside* the core
is refused outright, with the offending junction reported in the input board's
coordinates: occupancy is not completeness, and one stale mismatch poisons every
strip grown against it.

Two consequences worth using. A mid-spiral board -- round 1 done, round 2 dead,
rows 0..4 still empty -- **feeds the next run directly** if you pick a rotation
whose core lies in the filled part, so rotations can be chained without a Stage C
round-trip. And inputs are deduplicated on the core: two boards agreeing there
seed an identical search whatever they do outside it, which collapses a corpus of
near-siblings hard.

---

## The whirlpool -- turning the board between every re-grow

`pipeline/run_pipeline_whirlpool.sh` (no new tool; it chains the existing four)

Every Stage B tool grows **rows upward from the bottom**. The beam advances a
row at a time and the finalizer locks rows `0..N` and frees everything above, so
the rows a board stands on were chosen early, by a beam that was guessing, and
are never revisited however often the top is re-grown.

Turn the board 90 degrees and those buried rows become **columns** on one side,
where a re-grow can reach them. What blocked that until now is that a turned
board has complete *columns* and the finalizer can only start from complete
*rows* -- and nothing converted one into the other. `E555_backtracker
--stop_row` does: it searches rows `0..N` only and emits every exact filling.
That is its role here, and it is the whole reason the loop exists.

### The lap

```
rows 0..T full
  ├─ rotate +-90 deg    tools/E555_rotate.py in.csv 1   (and 3)
  │      T+1 complete COLUMNS, and zero complete rows
  ├─ backtracker        --stop_row 5 --with_frame --order rowmajor --break_mode any
  │      completes rows 0..5 AND the outer frame, clears everything else
  └─ finalizer          --finalize_from 5 --stop_row T
         rows 6..T re-grown at full width over a reduced database,
         with the border held fixed
```

The lap ends where it began -- rows `0..T` full -- but rebuilt from a different
direction. **Four laps is one full turn of the board.**

### Carrying the border round the lap -- `--with_frame`

A plain `--stop_row` clears **everything** outside the band, and the outer frame
is outside it. So the band used to reach the finalizer holding 26 of the 60
border cells, and `fin_border_complete()` -- which needs all 60 -- had no option
but to fall back to `--free_edges`. The border could never be a fixed thing the
loop carried; it was re-guessed from scratch on every lap.

`--with_frame` widens the band to **rows `0..N` plus all 60 frame cells**. That
one predicate does three jobs: border cells the turned board already holds are no
longer cleared, border cells it does not hold are *searched* as part of the band,
and band completeness now demands the frame close, so a board whose leftover
border pool cannot chain is dropped instead of being handed on. The band arrives
at the finalizer with all 60 and fixed-sides mode selects itself.

Two things it is not.

It is a **harder cut**: many turned boards admit an exact band but no exact frame
to go with it, and those drop out. That is a real filter and a real loss of
population -- the loop's attrition goes up, deliberately.

And it does **not** make the frame byte-identical from lap to lap. Fixed mode
pins the *set* of pieces on each side, not their order: the finalizer draws each
row's right terminal from that pool (`rows[r].rterm` indexes `g_edge_term`), so
the frame is re-completed every lap rather than carried unchanged. Pinning it per
cell would mean constraining the terminals inside the beam, which this is not.
What you get is that every lap runs against a committed border rather than a free
one, which is a much tighter search, not a frozen frame.

`FIXED_BORDER=0` restores the old free-border lap. Keep it available: a free
border out-yields a fixed one by roughly an order of magnitude at the same depth,
so it is the control arm any claim about fixed borders has to beat.

`--order rowmajor` at the cut is deliberate: after a turn the empty cells are
whole *columns* of rows `0..5`, and rowmajor walks them row by row, closing the
border row 0 first, which is the shape the finalizer's lock needs. `--break_mode
any` is required because the default `stuck` takes a minimal break where no
exact fit exists and a broken band is dropped at emission.

A quarter-turn CW (`1`) sends the old **right** column down to the new bottom
row and leaves the filled region on columns `0..T`; CCW (`3`) takes the old
**left** column down and fills columns `15-T..15`. The two keep different halves
of the board, so running both genuinely doubles the field rather than mirroring
it.

### What it buys

The lap keeps rows `0..5` of the turned board -- a six-deep slab against **one
side** -- and that side moves 90 degrees every lap:

| at `T = 11` (12 filled columns) | |
|---|---|
| **cells** preserved -- rows 0..5 of the filled columns | 72 |
| **cells** rebuilt by the band cut | 24 |
| **cells** re-grown by the finalizer, rows 6..11 | 96 |
| **pieces** free for the search -- 120 freed plus 64 never placed | **184** |

Cells and pieces are counted separately on purpose: `72 + 24 + 96 = 192` is the
board's filled area, while `184` is how much material the search may draw on.
Measured on a real lap-1 board (`T = 10`, so 11 filled columns): 66 cells kept
in place and 110 freed, exactly `6*11` and `10*11`.

The four slabs hug four different sides and their common intersection is empty,
so **no piece survives a full circle untouched**: the centre is re-searched in
all four laps, a corner in two. A board that comes out the far end admits an
exact rows-`0..T` partial cut from every direction, which is far stronger
evidence than surviving once from the bottom.

### It holds depth; it does not climb

`WHIRL_ROWS` defaults inside 10..12 on purpose. Below 10 the output floods --
nothing has gone extinct that shallow, so the whole stop-row beam is emitted and
`--incomplete_top`'s siblings multiply it (row 6 wrote 807 042 boards and 1.5 GB
where row 10 wrote 1 136 and 2.2 MB, same 5 s, same config) -- and above 12 the
beam is spent and the Stage C tools are simply better. So the loop holds its
depth and spends its time on coverage instead. Both bounds are **advice the
runner prints and then ignores**: a shallow stop row is a legitimate thing to
ask for, and only a row the beamer cannot accept at all (outside 1..13) is
refused. What must not run shallow is a test or an example, where nobody is
watching the disk fill, so those pass their depth explicitly. Stage C runs **once, at the end**, on the survivors --
roundhouse (`--rotate -1 --rounds 3 --strip_width 4`, the width raised on
purpose to free already-solved rows) and then the backtracker's mismatch dives.

Nothing inside the loop ranks, because there is nothing to rank: every stage
emits **exactly matched** boards, so at equal depth they all score the same. A
rows-`0..T` board scores exactly `15(T+1) + 16T` -- 356 at `T = 11` -- and the
ranker's `breaks` column is then just `480 - score`, counting adjacencies
against empty cells rather than real defects. What thins the field is
**attrition**: a board whose turned band admits no exact filling, or that will
not re-grow to `T`, drops out. The per-lap counts the script prints are
therefore the real diagnostic, and a lap that returns what it was given means
the neighbourhood is exhausted.

### Cost, and why `--max_emitted` is load-bearing

The cut is effectively free and the finalizer is the whole cost, which is the
opposite of what the shape of the pipeline suggests. Measured on **one** turned
synthetic board, `--max_emitted 0`: **4 787 556 exact bands in 120 s** (6.9 GB
of CSV), 100% accepted, and still running when the clock stopped. Meanwhile
every distinct band is a distinct locked set, so the finalizer rebuilds its
reduced database for each one -- at `--finalize_from 5`, 337 M records, 0.82 GB,
about 9 s.

So the loop consumes a vanishing fraction of what the cut offers, and
`--max_emitted` is not a safety net but the setting that defines the run.
Bands per lap is `2 * POP * BT_LIMIT`. Two consequences worth knowing:

- `BT_LIMIT` takes the DFS's **first** K bands, which share a long prefix.
  `BT_ORDER` is the lever on that -- a different static order starts from a
  different corner and returns a different first K. Ranking the bands would be
  the principled fix, but there is nothing to rank them *by*: they are all
  exact and all score the same (see above).
- Raising `BAND_ROW` shrinks the finalizer's database fast, at the cost of
  freeing less of the board per lap. That is the main speed/coverage dial.

### Clues

Clues are rotation-covariant, which is what makes the loop legal at all:
`g_clue` (`E555_database.c`) tabulates the published clues at **all four**
orientations -- the centre piece 138 sits at `(7,7) (8,7) (8,8) (7,8)` with
spins `0 3 2 1` -- and `g_clue_orients` is `0xF`, so every one of them is
enabled. A quarter-turn therefore maps a satisfied clue configuration to
another satisfied one rather than breaking it. That holds for the corner clues
too: the reachable pair is the row-2 cells at every orientation, with a
different piece on them.

**A band cut below row 7 carries no clue, and that is fine.** The centre clue
sits on row 7 or 8 depending on orientation, so a band of rows `0..5` strips it
and the line arrives at the finalizer carrying nothing to read an orientation
off. Reading and *choosing* are different things, and the finalizer now
distinguishes them: a board that carries a clue has committed to an orientation
and is never re-oriented, while a board that carries none has committed to
nothing and is searched once per orientation the locked region does not already
contradict (`--clue_orient`, default `auto`). Four different pins against the
same fixed band, so four genuinely different searches -- of which the old code,
which refused the line outright, ran none.

That refusal used to force `BAND_ROW >= 8` on every clued whirlpool, and the
cost was a shallower cut: at `BAND_ROW = 8` a lap preserves 99 cells and frees
77, against 66 and 110 at `BAND_ROW = 5`. The constraint is gone; what replaces
it is a cost, up to 4x the finalizer work per band, over **one** shared
database (the five clue pieces are the same set in all four orientations, so
only the pins move between passes). `--clue_orient N` pins it back to one pass.
Measured on a rows-0..5 band with `--clue_center`, growing to row 8: 0 boards
before, 33,536 after, from four passes over one database, every one of them
carrying the centre clue -- 27,241 at orientation 0, 4,734 at 1, 1,561 at 2,
and 0 at 3, whose pass ran and went extinct at row 6. Depth discriminates
between the four: the same band grown to row 10 keeps only orientation 1, the
other three dying on the way up. That is the argument for searching all four
rather than picking one -- which orientation survives is a property of the
band, not something you can know in advance.

Locking below the centre was never exotic, which is why this mattered: the
shipped `--finalize_from = BEAM_STOP - 5` does it on every ordinary run.

The finalizer
needs the flag on **every lap** -- the centre sits on the rows it re-grows, and
without it the search quietly refills that cell. The backtracker is not
clue-aware at all, which is harmless at the band cut (rows `0..5` exclude the
centre) but means the closing dive must use a `--holes` mask that leaves the
centre cell shut; the shipped `holes_open_border_TR.csv` does. The roundhouse
keeps the old refusal, and should: it only *verifies* clues rather than placing
any, so a board carrying none gives it nothing to check and no choice to make.

---
## Stage C -- the tail toolbox

Three tools, one canonical CSV, different philosophies. None of them has
closed a real 480/480 yet -- this is the open front of the project.

### E555_topper.py -- break minimizer ( the Stage C workhorse)

OR-Tools CP-SAT model over the open cells: variables for (piece, rotation,
four exposed colors) with `AddAllowedAssignments` tables filtered by the frame
rule, `AddAllDifferent` per piece class. Lexicographic objective by dominating
weights: (1) minimize total breaks; (2) push unavoidable breaks to the nearest
horizontal border; (3) slide them along it to the nearest corner.
`--band_depth`/`--locked_rows` implement the **overlapping sliding window**
(see the strategy guide in the file header and
`pipeline/topper_sweep.sh PRESET=window`): move a constant-size work band across the
board in overlapping steps, so early mistakes stay repairable.
`--relax_breaks` trades a slightly worse total for a longer push in the
middle steps.

**Where breaks are pushed.** A naive "distance from the bottom" plus "distance
from the left" cost would send every break to the one top-right corner -- up to
15 rows and 15 columns. The topper measures each cell to the *nearest* border
instead:

```
_v(cell) = min(row, 15-row)     primary   -> nearest horizontal border
_h(cell) = min(col, 15-col)     secondary -> then along it to the corner
```

The worst case drops to 7 + 7, and the four corners share the load, so a break
born at bottom-left is no longer dragged across the whole board. Priorities
stay strictly lexicographic: (1) total breaks, (2) `_v`, (3) `_h`. The packing
weights are derived from the number of breakable junctions in the model, not
from all 480 -- with the smaller distance maxima (7+7, not 15+15) that keeps the
objective one to two orders of magnitude smaller (`w_b` ~ 2.8e6 for a typical
120-junction band), a friendlier LP relaxation than a whole-board cost would give.

**Which border is opened.** `--side` picks the band(s), each `--band_depth`
deep:

| `--side` | opens | use |
|---|---|---|
| `T` (default) | top rows | the classic upward sliding window |
| `B` / `R` / `L` | bottom rows / right cols / left cols | clean-up where breaks got stranded |
| `TR` / `TL` | an L of top + right (or left) | fold everything into one corner |
| `TB` | top **and** bottom rows | test whether the opposite borders can be re-cut against each other |

`--locked_rows N` applies to each
open band: its outermost N rows/cols are **unset to 999** and locked empty for
the run -- the freed pieces rejoin the pool, and the next window recovers the
gap. Two rules keep a `--side` run confined to that side: a break cell is
unlocked only if it lies in or touches the band (a break stranded on the far
side stays put, which is what keeps a one-sided run one-sided), and empty cells
outside the band stay empty.

**`--holes FILE`** replaces all of that with an explicit 16x16 0/1 mask -- the
same dialect the ender and the backtracker read -- and the mask is taken
literally: free = exactly the cells marked `1`, with no band, no adjacency
expansion and nothing held empty, so `--side`, `--band_depth` and
`--locked_rows` no longer apply (the last is rejected outright). Use it for a
region a band cannot express: an L around one corner, a ragged patch following
a cluster of breaks, or the board's interior -- including the `(16-2W)^2`
centre the roundhouse retains and can never itself reopen.

`--top N` is a real beam here: rank 1 is the optimum, and each further
rank is re-solved under a no-good cut requiring >= `--beam_diff` cells to differ
from every board already emitted, with the objective capped at
`--beam_slack` extra breaks. A naive "last N incumbents of one search" beam
would return near-duplicates of rank 1 that carry almost nothing into the next
window step; this one does not. The beam stops early rather than padding with
duplicates, and the run summary says how often that happened: on a tight band
the default `--beam_slack 1` often admits nothing at all (on
`data/board_example_462.csv` with a 2-row band, the nearest distinct board
costs 5 more breaks), so widen it when you want a genuinely wide beam. `--verbose` prints an ASCII map of the open band, and
boards with nothing broken or empty inside the band skip the solver entirely
(unless a clue inside the band is displaced, which is work whether or not the
band holds a break).

**`--clue_center` / `--clue_corners` / `--clue_orient`** hold the Eternity II
hint pieces in place; see the clue section above for what they cover and why
orientation is read off the board rather than chosen. Each pinned clue is two
`Add` constraints on cells the model already has, so nothing about the
objective, the break count or the corner pull changes. One consequence worth
knowing: a pinned cell can never differ between beam ranks, so with clues on
`--beam_diff` is effectively measured over the unpinned cells.

Three drivers ship with it, in increasing depth and cost:

| driver | shape | when |
|---|---|---|
All four sweeps are one script, `pipeline/topper_sweep.sh`, driven by a `PLAN`
of `SIDE:WINDOW:LOCKED` passes. `PRESET` names the four that were separate
scripts:

| `PRESET` | plan | when |
|---|---|---|
| `safe` | `T:5:0` then `TR TL TB R L B` at `3:0` | a board filled to row 11. Nothing is ever unset, so no pass can make the board worse. Start here |
| `window` | `T:8:3 T:6:2 T:5:1 T:4:0`, then `TR:4:0 L:4:0` | the general sliding window; what the pipeline's stage 4 runs |
| `deep` | the same seven sides, each as a *pair*: a wide pass with the outer band locked empty (`7:3` for `T`, `5:2` elsewhere), then a narrow pass that refills it (`4:0`, `3:0`) | when the safe sweep has run out of moves and you are willing to spend breaks to buy freedom |
| `closeT` `closeB` `closeR` `closeL` | `X:6:2 X:4:0` on the named side | closing a hole that sits against one border -- an `E555_roundhouse` `miss0` board, or any partial whose breaks are all on one side. The first pass reaches into the core, the second fills everything. Pick the letter for the band of the roundhouse's *last* round: `--rotate 1` (its default) ends on the bottom, so `closeB`; `--rotate -1` ends on the top, so `closeT`. `close` on its own means `closeT` |

The pairs of the third driver are inseparable: `--locked_rows N` unsets those
cells, so the wide pass *raises* the break count on purpose and only the narrow
pass that follows brings it back down. Never stop between the two halves of a
side, and never prune on score in between -- the driver prunes only after each
side is complete.

### E555_backtracker -- exact / bounded-mismatch DFS (strong, slower)

Pure C + OpenMP. Constrained DFS over the empty cells with a selectable cell
order (default `mrv`: most-constrained cell first). A static
(side,color) orientation-bitset index plus per-cell exact-fit domains give
O(word) MRV counts and immediate empty-domain cutoffs; classic mode adds three
sound completion prunes (global empty-domain lower bound, incremental
color/type accounting, Hall/deficiency bipartite bound). `--holes` reopens a
masked region of a complete board.

`--break_mode` selects between two very different engines, and the distinction
matters more than any other parameter here:

- **`stuck` (default) -- greedy dives, for triage.** A dive takes an exact fit
  where one exists and a minimal break where none does, never backtracks, and
  therefore always reaches 256 pieces in one pass. Because piece-type counts are
  exactly balanced, the candidate set is never empty, so a dive is O(cells) and
  cannot fail. `--restarts N` (default 100 000, ~5-10 s on four cores) runs
  N randomized dives and keeps the best. Divergence comes from random tie-breaking
  alone, and that is ample: a 200 000-dive batch produced 200 000 distinct boards.
  Throughput is ~9k-18k dives/s on four cores, so N in the millions is practical.
  This mode **proves nothing** -- it never establishes
  that a board cannot be completed with fewer breaks. Expect it to land well
  above a well-optimized incumbent (~28 breaks best-of-200k on a 74-cell region
  whose input carried 18); its job is ranking candidate partials cheaply, not
  improving them.
- **`any` / `lds` -- exhaustive, for proof.** These keep the iterative-deepening
  ladder over `k = input_breaks .. --breaks`, so an exhausted level is a
  theorem: no completion exists with <= k broken edges. Cost per level grows
  roughly exponentially. Use them overnight, on partials that triage picked out.

`--order` defaults to `mrv` (most-constrained cell first) for every mode. Static
orders land on cells with no exact fit far more often once breaks are allowed --
measured on a 48-cell tail, `spiralout` spent 232 M piece tests against `mrv`'s
1.6 M and still reached a worse board -- so `mrv` is the right universal default.

`--reverse` flips the traversal direction of any static `--order`: `rowmajor`
fills each row right-to-left (rows still bottom-to-top), `colmajor` right-to-left
top-to-bottom, and the ring/distance orders (`spiral`, `centerout`, `spiralout`)
reverse chirality while keeping their shell progression -- so `spiral --reverse`
still closes the border first but walks up the left column instead of along the
bottom. For `mrv` it swaps the tie-break from row-major to column-major (this
subsumes the former `mrv-colmajor` order, which was removed). It never changes the
solution *set* (exhaustive counts are identical), only the order cells are visited,
so under `--max_emitted 1` it returns a different first closure: two triage
passes (forward and `--reverse`) yield border-distinct partials that the
finalizer's fixed-mode dedup keeps separate. No effect on `2sides` or `4sides`.

**`--stop_row N` / `--stop_column N` -- enumerating partials for the finalizer.**
The same shape as the beamer's `--stop_row`: restrict the search to rows `0..N`
(or columns `0..N`) and emit *every* way to fill that band, one canonical line
each, to `<out>.stop_row<N>.csv`. Cells outside the band are cleared first --
their pieces return to the pool, so a partial that already carries rows above
the band does not starve it -- and are written unplaced (`999`), so each line
feeds `E555_finalizer --finalize_from N` directly.

The band is the whole search: `rem[]` holds only in-band empty cells, so the DFS
enumerates exactly the distinct fillings, each leaf is one emission, and no two
lines can repeat a band. `--reverse` anchors the band at the far side instead
(rows `15-N..15`, columns `15-N..15`) *as well as* flipping static traversal --
one flag, two effects, whenever a stop option is on. A band completion is the
solution here, so `--max_emitted` caps emissions and defaults to **1**;
`--max_emitted 0` enumerates, and should be used with care, since rows 0..3
alone ran to 7.6 M bands and 11 GB in five minutes.

**`--with_frame` -- keep the border instead of clearing it.** The outer frame is
outside the band, so a plain cut clears it: a rows-`0..5` band reaches the
finalizer holding 26 of 60 border cells, and fixed-sides mode, which needs all
60, is not available to it. `--with_frame` makes the band *rows `0..N` plus the
60 frame cells*. Border cells the input holds are retained; border cells it does
not hold are searched as part of the band; and completeness now demands the frame
close, so a band whose leftover border pool cannot chain is never emitted. The
frame's pieces stay out of the pool -- they are committed -- so the rows have
that many fewer to draw on. Requires a stop band; rejected without one.

This is what lets a whirlpool lap hand a committed border to the next lap. It
does not freeze the frame: fixed mode pins the piece *set* per side, not the
order, so the finalizer re-chooses each row's terminal and the frame is
re-completed each lap rather than carried unchanged.

Two options are refused rather than silently useless: `--breaks` must be
`0`, because the finalizer validates every colour match inside its locked region
and would reject a broken band one stage later; and `--jump` must be off, since
skipping a dead cell leaves the band forever incomplete. `--break_mode stuck`
is accepted but takes a minimal break where no exact fit exists, so the bands it
produces are dropped at emission -- the summary reports `band_accept_rate` so a
run that emits nothing is diagnosable. Under `--rotate` the band is defined in
the original frame, the one the CSV is written in, unlike `--holes`, which is
read in the rotated frame.

Parallelism is automatic: one record per thread, or every thread on one record's
search when there are no more records than threads (`--all_for_one` forces the
latter). Note that this search is memory-system bound, not scheduling bound -- on
a 4-core laptop, four *independent* single-threaded runs already slow each other
to 2.44x aggregate, and the threaded search achieves 2.37x, so there is little
left for tuning to recover. Crash-safe: appends an improving checkpoint line per
record; output is re-feedable.

### E555_ender.py -- the closer, two neighbourhoods ( power tool)

For a full board the topper has already tidied. One CP-SAT engine opens a slice
of the board, caps how many pieces may actually move (`--max_changes`), and
re-solves to cut breaks -- never returning a board worse than its input. An
internal escalation ladder climbs (reach x budget) rungs, warm-starting from
the best board so far and stopping the instant breaks hit zero; you set only the
ceilings (`--reach`, `--max_changes`). `--mode` picks the neighbourhood and the
objective:

| `--mode` | interior opened | objective | good for |
|---|---|---|---|
| `patch` (default) | a box around the current breaks, or an explicit `--holes` mask | minimize breaks, then **compact** the broken region and its perimeter | surgically healing and tidying a few local breaks |
| `ring` | every cell within `--reach` BFS layers of a break | pure break count (kept lean so the big model stays fast) | breaks on/near the border, which heal by an "avalanche" cascading around the frame |

Both modes always free the whole 60-cell border ring; only the interior set and
the objective differ. It exploits the border's Euler-trail richness -- a top
break can cascade all the way around the frame -- and the corners' three-way
commodity symmetry falls out of the per-class `AddAllDifferent` for free.
`--verbose` prints an ASCII map of the open pool per rung.

It takes the same **`--clue_center` / `--clue_corners` / `--clue_orient`** as the
topper, with two extras this tool needs. Its piece domains are exactly the
pieces already in the pool -- it is a permutation repair, not a filler -- so a
displaced clue also opens the cell its piece currently sits in, or the pin would
be infeasible rather than merely unsatisfied; a clue already in place opens
nothing. And its never-worse guard becomes lexicographic, clues before breaks,
because a clue repair is often break-neutral and a break-only test would discard
the repaired board. Both are recomputed per rung, so a clue fixed on one rung
stops asking for cells on the next. Repairing a clue spends at least two of
`--max_changes` (its cell and its donor), so the cheapest rungs of the ladder may
come back infeasible on a clue-broken board and the ladder simply climbs.

---

## Tools

- **`tools/E555_viewer.py`** -- ASCII board (`#` marks broken junctions),
  placement/edge/solid statistics, frame-violation check, e2.bucas.name URL;
  `--diff A B` overlays two rows of a CSV. `--seed_file PATH` (defaults to
  `./seed_Edge5.txt`, then the repo's `data/` copy). It is also the toolkit's
  shared Python module: `E555_rank.py` and the two Stage C CP-SAT tools import
  it, and it holds the single copy of the Eternity II clue table
  (`CLUE`, `clue_list`, `clue_orient`, `clue_pins`).
- **`tools/E555_rank.py`** -- ranks and sorts board CSVs by what the score
  cannot see. Eighteen breaks spread over seven rows is a mess; the same
  eighteen packed into rows 14-15 is nearly finished. Per board it derives
  `breaks`, `solid` (the viewer's fully-satisfied pieces), `break_rows` /
  `break_cols` (how many distinct lines hold a break -- the compactness
  measure), `span` (their bounding box), `clean_b/t/l/r` (contiguous
  break-free rows or columns from each border; `clean_b` is the old
  "completed rows"), `corner_d` (the distance the breaks still have to
  travel to their nearest corner -- the quantity `E555_topper` minimizes
  after the break count, and a good tie-breaker), and `clues` (how many of the
  five Eternity II clue pieces still sit at their published cell and spin,
  0..5, for whichever orientation the board matches -- always measured, no flag,
  and 0 for a board that never carried clues). `--sort` takes any of them,
  best board first, several files at once; `--out` re-orders the input rows
  verbatim, so the canonical format never changes and old files rank fine.
  `--out FILE --rescore` instead rewrites every row canonically
  (`config_id, score, pos[256], rot[256]`) with the score recomputed from the
  seed -- the one way to make a mixed corpus sortable by field 2, since Stage B
  writes its solution index there and older dialects write other things again.
  `--diverse K` answers a different question: a run that emits a thousand
  boards rarely emits a thousand ideas, so the top of a ranking is usually one
  lineage and post-processing its top five spends five budgets on one
  hypothesis. It picks K boards farthest-first on **cell agreement** (two
  boards agree on a cell when both put the same piece there at the same spin),
  starting from the best-ranked board and adding whichever board's closest
  chosen root is furthest away -- K x M comparisons, not M^2 -- and prints each
  root's agreement with the roots before it. On the 15 exact row-12 partials of
  a whirlpool run it returns one board from each of the four lineages the pool
  holds (199-202 of 203 cells shared inside a lineage, 0-14 across).
  `--max_agree P` is the blunt version: drop anything agreeing with a kept
  board on more than fraction P of its placed cells.
  Memory is bounded rather than hoped for: a record is the input line plus its
  measures, about 1.8x the row on disk (51,000 boards of a 96 MB CSV peak at
  165 MB, against 2.2 GB before), `--top N` streams into a bounded heap so peak
  memory stops depending on file size at all (14 MB for the same input), and
  without `--top` an input projected past `--max_mem` (default 8 GB) is refused
  up front instead of being OOM-killed half way.
- **`tools/E555_rotate.py`** -- turns every board in a CSV by `N` quarter-turns
  clockwise, same convention as `E555_roundhouse --rotate`. Lossless: the frame
  rule is identical on all four sides, so a rotated board is the same board
  seen from a different corner, and the tool re-scores every row before and
  after to prove it. What changes is which rows and columns hold the open cells,
  and therefore the direction the next (direction-biased) stage attacks them
  from -- run a board at 0/1/2/3 and hand all four to the finalizer, the
  roundhouse or the topper. `--all` writes all four turns in one pass, and
  `--holes FILE` turns a mask alongside the board so the next stage still opens
  the same physical pieces. `--rotations` turns a Stage A rotations CSV instead
  of a board -- a border piece's spin *is* its side, so a turned board needs a
  turned rotations file or `fin_rot_match` stops recognising it and the
  finalizer drops to `--free_edges`.

---

## File formats

| producer | file | layout |
|---|---|---|
| Stage A | `rotations.csv` | `# comment` lines + `id, spin[0..255]` (60 border spins, 196 zeros) |
| beamer | `beam_completions_<border>_<row>.csv` / `..._random_<row>.csv` | `config_id, sol_idx, pos[256], rot[256]` (514) |
| beamer | `sweep_checkpoint.txt` | resume state, one line |
| roundhouse | `roundhouse_round<N>_rot<K>[rev]_W<w>_miss0.csv` (break-free) and `..._miss<B>.csv` (with breaks) | canonical 514-field layout, ids `p<line><tag><n>` |
| finalizer | `beam_completions_finalized_<row>.csv` | same 514-field layout, ids `p<line>r<repeat>l<column>` |
| Stage C (all) | output CSV | **canonical**: `config_id, score, pos[256], rot[256]` (514) |
| backtracker | `<out>.checkpoint.csv`, `<out>.status.csv`, `<out>.best_*.csv` | canonical rows / diagnostic sidecars |
| backtracker | `<out>.stop_row<N>.csv` / `<out>.stop_col<N>.csv` (`_rev` when reversed) | every completed stop band, canonical 514-field layout |

---

## Build & Run

```bash
make                        # bin/E555_beamer, bin/E555_finalizer,
                            # bin/E555_roundhouse, bin/E555_backtracker
pip install ortools         # only for topper / ender

# no Stage A needed -- the 5-minute demo:
bash examples/01_beamer_quickstart.sh

# the whole chain in five calls, no arguments, output in the current directory:
cd ~/runs && bash ~/E555/examples/07_barebones_chain.sh

# stage by stage. Settings are NAME=value ARGUMENTS, not environment variables:
bash examples/01_beamer_quickstart.sh ANNEAL=1          # Stage A then Stage B
bash examples/02_finalizer_regrow.sh BOARDS=beam_out/beam_completions_0_10.csv
bash examples/04_stage_c_close.sh BOARDS=final_out/beam_completions_finalized_12.csv

# validate everything (includes the synthetic-solution regression):
bash tests/run_tests.sh
```

---

## Search-strategy trade-offs

After the one-time DB build, wall-clock is roughly
`(configs searched) x (rows reached x K x pool_factor x const)`.
Dead configurations cost almost nothing; the budget is spent on those that
survive several rows.

- **Beam width K** -- diversity per configuration; linear cost. The lever for
  drilling a promising border.
- **Number of configurations** -- the primary coverage lever.
- **Multiple seeds** -- re-sample the same configurations along different
  stochastic paths; re-pays the DB build (use `--db_file`).
- **`--stop_row`** -- lower = cheaper Stage B, more work for Stage C; 12 is the
  designed balance, 10-11 stocks the finalizer cheaply.
- **`--random_edges`** -- unlimited fresh borders, zero Stage A cost, weaker
  guarantees per border. The breadth end of the spectrum.
- **finalizer `--finalize_from`** -- lower = more re-searched rows per partial
  (deeper resampling, costlier); higher = cheap top-row re-rolls. The useful
  range depends on whether the partial's border is COMPLETE, and the two cases
  pull opposite ways. On a beamer partial with a complete border the left column
  is fixed and `--top_columns` samples orderings, so lower is better until the
  beam stops filling: measured on `board_partial_row12.csv` with 12 sampled
  columns, `4` reached row 11 on 8 configurations of 12 and `5` on 6, while `6`
  and above reached it on none and left the beam under 1% of its cap -- an
  exhaustive walk wearing a beam's clothes, which is why the default is `5`.
  With an INCOMPLETE border the finalizer falls back to `--free_edges`, and
  `--top_columns 0` then enumerates every legal left column; that enumeration
  grows explosively as rows are freed, so the same board wants `7` (see the
  measured table in `examples/README.md`).

**Practical default:** breadth first (`--random_edges` or many border rows,
moderate K), finalize the survivors from row 4-5 with repeats, topper the
best finals through the sliding window, then throw the backtracker and the
ender at anything above ~460.

**Reproducibility.** A run is reproducible from `--rng_seed` **together with
`--threads`**, not from the seed alone. The work partition follows the thread
count, and a beam that keeps a bounded number of candidates keeps a different
subset from a different partition -- on the synthetic board at a 200-wide beam,
2 threads score 8731 candidates where 4 score 9009, and the searches diverge
from there. Both are valid searches; neither is the other. Record the thread
count with the seed, and `finalizer_determinism` in the release gate holds the
tools to the same-seed-same-threads contract.

---

## File summary

| file | role |
|---|---|
| `src/A_border/E555_edge_annealer.py` | Stage A border annealer (BEST theorem + SA). |
| `src/B_beam/E555_database.c/.h` | Seed/catalog, border enumeration + ranking, `DB_5pieces` build, fan-out table, disk cache. |
| `src/B_beam/E555_beamer.c/.h` | Stage B beam search: expand/score/select/materialize/emit, sweep driver, CLI. |
| `src/B_beam/E555_finalizer.c` | Beam from a partial: locking, input dedup, reduced DB, column sampling/enumeration. |
| `src/B_beam/E555_roundhouse.c` | Strip solver: board rotation, width-W chain DB, relaxed DP oracle, exhaustive/sampling strip search, 3-round spiral. |
| `src/C_tail/E555_topper.py` | CP-SAT break minimizer, nearest-corner pull, `--side` bands + sliding window, or an explicit `--holes` mask. |
| `src/C_tail/E555_backtracker.c` | Exact/bounded-mismatch DFS tail closer. |
| `src/C_tail/E555_ender.py` | CP-SAT closer: budgeted local re-solve, `--mode patch` (compacting LNS) or `ring` (border sweep). |
| `tools/E555_viewer.py` | Board viewer/differ + bucas URL. |
| `tools/E555_rank.py` | Ranks/sorts board CSVs by compactness, solidity, clean rows; `--rescore` rewrites them canonically; `--diverse K` picks independent roots. |
| `tools/E555_rotate.py` | Turns every board in a CSV by a quarter-turn multiple, losslessly. |
| `data/` | Seeds, known synthetic solution, example boards, masks (see `data/README.md`). |
| `examples/` | One small script per tool: read these first. |
| `pipeline/` | The full pipeline, the board farm and the topper sweeps -- long unattended runs. |
| `tests/run_tests.sh` | The release gate. |
| `tests/check_script_flags.py` | Gate check: every `--flag` a shipped script passes is one its binary accepts. |
| `tests/compare_sweeps.py` | Turns two `--verbose` logs into a paired A/B with a sign test. Not part of the gate. |
