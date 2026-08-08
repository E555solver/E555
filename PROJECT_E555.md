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
  `tools/E555_rank.py --emit FILE --rescore` rewrites any of them canonically,
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
`bottoms=1152 ... left-cols=2880 -> 9000 configs`. So a bottom of 5000 with
`--top_bottoms 300` means the sweep only ever sees 6 % of its own space, while
a bottom of 400 means it sees three quarters of it. That is the argument for
targets: you are sizing a search, not maximizing a number.

**Allocating within that space** (`--bail_columns N`, 0 = off). Every
(bottom x left) config gets the same budget, which is the weakest allocation
when config quality varies by orders of magnitude. `--config_time_sec` does not
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
derives its own RNG seed from `--seed` and its own index rather than drawing
from a stream shared with the others, so **the thread count never changes the
result**, and the parent replays the restarts -- stdout and the rotations CSV
alike -- in restart order however the workers finish. The pool is capped at one
worker per restart. Measured on an 8-thread laptop the gain is ~3x (8 restarts x
50k steps: 24.5s serial, 7.9s parallel -- the workers share cores, so it is not
8x), which takes the `run_pipeline_annealed.sh` Stage A of 50 restarts x 300k
steps from ~14 min to ~5.

Key options: `--restarts`, `--steps`, `--seed`, `--threads`, `--verbose`,
`--T0/--Tf`, `--w-top/right/bottom/left` (per-side target multipliers with
`--target_scale`, and signed weights in log-sum mode, where a negative weight
minimizes a side), `--tabu`, `--fix-corners {0,1,2}`, `--target_scale`, `--out`.

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

### The beam loop

For each (bottom ordering x left-column ordering) configuration -- enumerated
as Euler trails and ranked by fan-out -- one beam advances row by row:

1. **Expand.** Every board fills its next row A→B→C from the database, with
   exact 256-bit piece-disjointness masks. Cells are pre-sorted by promise, so
   a first budgeted phase scans the most continuable chains; a second phase
   visits the cell in a random full-cycle permutation (random start + coprime
   stride) so random exploration never regenerates a duplicate child within a
   slice. Per-parent work is bounded by `--scan_factor` (decode budget) and
   `--pool_factor` (child quota).
2. **Score.** See below.
3. **Select.** Children dedup by a 64-bit *frontier signature* -- a hash of
   (used-piece set, exposed top colors), which provably determines a board's
   entire future -- keeping the best copy. Survivors are pruned to the row's
   width by a score band (with a per-parent offspring cap) plus a random band.
4. **Materialize.** Moves go to an ancestry log; beam entries stay ~150 B.

An empty child pool **proves** the configuration dead below the current row
(`extinct`); the sweep moves on. Every board that completes `--stop_row` is
emitted, best first -- deliberately with no lookahead at the stop row: whether
the board continues is the next stage's problem.

**Width and randomness schedules.** Extinction pressure concentrates in the
high rows, where the piece supply thins. Three coupled schedules concentrate
effort there (K = `--beam_width`, E = `--beam_expand`, R = `--beam_expand_row`):

| rows | width | random band | parent cap |
|---|---|---|---|
| 1 ... R-2 | K | `frac_rand` | `parent_cap` |
| R-1 | max(K, K*E/2) | `frac_rand`/2 | 2x`parent_cap` |
| R ... stop_row | K*E | 0 | 2x`parent_cap` |

Early rows explore (the heuristic knows little about an empty board); late
rows are pure exploitation.

**Gumbel top-K selection** (`--gumbel_tau0`, `--gumbel_tau1`; 0 = off, the
default). The `frac_rand` band above is *uniform over survivors* -- blind to
the score -- and it is switched off entirely from row R on, exactly where
extinction pressure peaks. An alternative is to perturb the sort key instead:

```
key = score/tau + Gumbel(0,1),      Gumbel = -log(-log U)
```

Top-K of that is provably a sample of K **distinct** boards drawn without
replacement with probability proportional to `exp(score/tau)` (Kool, van Hoof &
Welling 2019) -- one RNG call per survivor, in a loop that already touches every
survivor, replacing ~196 k serial rejection draws at default settings. Because
the score is already a log record count, **tau = 1 samples in proportion to
estimated completions**; tau -> 0 is greedy and large tau is near-uniform. Tau
interpolates linearly from `tau0` at row 1 to `tau1` at `--stop_row`, so one
monotone knob expresses the whole "trust the score more as the board fills"
schedule. When tau > 0 the `frac_rand` band stands down; the pool's own scores
are never perturbed, so the beam, the emission order and every reported score
stay real.

**The same primitive on the borders** (`--gumbel_tau_bottoms`,
`--gumbel_tau_columns`; 0 = off, the default; the finalizer takes the columns
one). Choosing *which* borders to run has exactly the shape the perturbation is
for -- the enumerated ranking is a top-K of `BottomOrder.rank`/`LeftOrder.rank`,
and the `--random_edges` and finalizer samplers are the K=1 case, an argmax over
32 draws. Above 0, `rank/tau + Gumbel` replaces the plain rank as the comparison
key, so `--top_bottoms`/`--top_columns` become a sample without replacement
rather than the greedy head.

It is the **column** rank that needs this. `left_rank_of` is
`sum_r log1p(la_total[c_r])`: a board-blind census that never looks at the
bottom, and -- decisively -- a sum over rows, hence *symmetric in the row index*.
Two columns exposing the same colour multiset score identically however they
order it, though position is what matters (row 1 meets the bottom, row 14 the
top border). The tie classes are therefore large, `cmp_left_rank` settles them
by `memcmp`, and taking the top L hands the sweep L lexicographically adjacent
columns -- correlated, which is worse than random. Since the perturbation only
decides which configs run, it costs no search time at all.

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
then requires the original `--seed`, and the beamer refuses the combination
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
`--avail_correct` discounts it by `sum over the 14 inner frontier colors of
log(R_c/tot_c)` -- the fraction of each color's half-edges still in the
reservoir, a first-order estimate of how many counted chains survive. It
decodes nothing and scans nothing (that is what the fan-out table exists to
avoid); it is a closed-form correction on numbers already looked up, and it is
self-scheduling, since the ratios sit near 1 until the reservoir empties.

**The J objective** (`--score_model J`, `--lambda_J`). An alternative to the
Mahalanobis term below, derived rather than tuned. Every free inner half-edge
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

**Center-139 bonus** (`--soft_center_139`, `--bonus_139`). Clue piece 139 is
barred from rows 1-5; a board that later places it on one of the four true
center cells carries a score flag on every subsequent row, promoting that
lineage. The bonus is additive on a score measured in nats of log record count,
so its value *is* a claimed factor in continuability: the historical +10
asserted a center-139 board was worth `e^10 ~ 2.2e4` times one without, which
no real fan-out difference can overcome, and it then persisted in every
descendant from row 6 while the random band shut off at row 8. The default is
now **1.0** -- roughly one standard deviation of the color term -- so it breaks
near-ties in favour of a 139 lineage without overriding a board that is
genuinely more continuable.

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

### Piece-supply pruning (`--supply_check R`)

A certificate that counts **pieces** where the parity test counts half-edges,
so neither implies the other -- a piece with two sides of color c adds 2 to
`S_c` but can still serve only one column. Each of the 14 frontier columns
needs a distinct remaining inner piece carrying its exposed top color. Columns
demanding the same color have identical candidate sets, so Hall's condition
binds first on whole color classes, and the singleton case is

```
for each inner color c:  #{columns demanding c} <= #{unused inner pieces carrying c}
```

which is 4 `andn` + 4 `popcount` per demanded color against `g_color_pieces`.
Off by default, and worth measuring before trusting: in the finalizer regime
above it rejected 0 of 25 M candidates, because a color is carried by ~40 of
the 196 inner pieces and 14 columns cannot exhaust that until the board is very
nearly full.

### Random border mode (`--random_edges`)

No Stage A input at all: the solver samples borders directly from the seed's
4 corner + 56 edge pieces. A bottom sample assigns corner roles at random and
grows a random legal chain of 14 frame-down edges BL→BR; a left-column sample
grows a chain BL→TL from the edges the bottom did not consume. Each published
border is the best of 32 samples by the same fan-out measures used for
enumerated borders. Corners can be pinned with `--BL/--BR/--TL/--TR <piece>`.
Free-edges mode is implied; `--border_row_N` = number of random bottoms and
`--top_columns` = random left columns per bottom. Typical use is a long
unattended run stocking varied partials for the finalizer and Stage C.

### Determinism and reproducibility

Runs are intentionally **not** reproducible unless `--seed` is given: the
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
| `--border_row N` | 0 | first rotations-CSV data row to use |
| `--border_row_N N` | 1 | consecutive border rows to sweep (random mode: number of bottoms) |
| `--db_file PATH` | -- | on-disk DB cache (~6.5 GB; built on first run) |
| `--free_edges` | off | any edge piece may terminate a row (relaxed parity) |
| `--random_edges` | off | sample borders from the seed; rotations CSV optional |
| `--BL/--BR/--TL/--TR P` | -- | pin corner piece P (random mode) |
| `--incomplete_top` | off | also emit stop-row boards holding two of the three segments -- A+B, A+C or B+C |
| `--beam_width K` | 262144 | boards kept per row |
| `--stop_row R` | 12 | last row filled (1-13) |
| `--beam_expand E` | 5 | late-search width multiplier |
| `--beam_expand_row R` | 8 | row with the full ExK width |
| `--score_model M` | `legacy` | color term: `legacy` (Mahalanobis) or `J` (pairing combinatorics) |
| `--lambda_J F` | 1 | weight of the J terms (1 = as derived) |
| `--lambda_Mahalanobis F` | 0 | weight of the color-usage atypicality bonus (legacy model) |
| `--avail_correct` | off | discount B/C fan-out by each frontier color's remaining supply |
| `--bonus_139 F` | 1 | center-139 score bonus (was a hard-coded 10) |
| `--no_free_demand` | -- | **disable** the free-mode demand accounting (on by default) |
| `--supply_check R` | 0 | piece-supply certificate from row R (0 = off) |
| `--frac_rand F` | 0.75 | random selection band (halved at R-1, zero from R) |
| `--gumbel_tau0 T` | 0 | selection temperature at row 1 (0 = off, exact legacy) |
| `--gumbel_tau1 T` | 0 | selection temperature at `--stop_row` |
| `--parent_cap N` | 5 | children per parent in the score band |
| `--pool_factor N` | 8 | candidate-pool target, x beam width |
| `--scan_factor N` | 1024 | decode budget per requested child |
| `--bc_window nB,nC` | `1,1` | score up to nB x nC (B,C) completions per A record, keep the best |
| `--top_bottoms N` | 300 | ranked bottom orderings tried per border row |
| `--top_columns N` | 10 | ranked left columns per bottom |
| `--gumbel_tau_bottoms T` | 0 | selection temperature for the bottom ranking (0 = off) |
| `--gumbel_tau_columns T` | 0 | ditto for left columns (measure before raising: see above) |
| `--bail_columns N` | 0 | abandon a bottom after N consecutive columns that emitted nothing (0 = off) |
| `--config_time_sec S` | 600 | wall-time slice per configuration |
| `--max_wall_sec S` | 0 | total budget (0 = unlimited) |
| `--max_partials N` | 0 | stop after N boards reported -- completions **plus** `--incomplete_top` partials (0 = unlimited) |
| `--resume` | off | continue from the sweep checkpoint |
| `--threads N` | all | OpenMP threads |
| `--seed S` | random | master RNG seed |
| `--soft_center_139` | off | center-clue handling (see `--bonus_139`) |
| `--verbose` | off | per-row `[beam]` progress lines |

`--bc_window` is the one place where extra compute buys objective rather than
more candidates. `try_A` normally commits to the **first** conflict-free (B, C)
and returns, so segments B and C -- 10 of the row's 14 pieces -- are chosen by
the database's global, board-blind fan-out sort: the score filters an unbiased
sample but never steers it. With a window open, up to nB workable B chains x nC
C completions are scored with the same `score_child` used for selection and only
the best is kept. Note `--scan_factor`'s budget counts segment-A decodes only,
so an open window multiplies work the budget does not see. There is no
counterpart in the finalizer, whose beam rows already enumerate *every*
conflict-free (B, C) -- the defect the window fixes does not exist there.

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

**Locking.** `--border_row/--border_row_N` select the CSV lines; each line is
structurally validated (piece types per cell, frame orientation, every color
match inside the locked region). Pieces at or below `--finalize_from`
(default 8) are locked; pieces placed above return to the pool.

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
disk cache. Consecutive lines sharing a locked set skip the rebuild.

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

`--gumbel_tau_columns` works here as in the beamer: above 0 the sampled left
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
loop never did. `--score_model J` is a better fit here than in the beamer:
`maha_term` infers the placed-piece count from `n = 14*row`, whereas `J` reads
the colour counts straight off the board and stays exact from a locked partial
at any `--finalize_from`.

**Output** appends to `beam_completions_finalized_<stop_row>.csv` with config
ids `p<line>r<repeat>l<column>`; several instances on the same machine may
share one output file (each line is one atomic append).

**Bounding a run.** Both tools accept `--max_wall_sec` (time) and
`--max_partials` (output): the latter ends the run once N boards have been
written, counting completions and `--incomplete_top` partials together. The
stop-row beam in flight is always reported in full, so the final count
overshoots N by up to one beam width. Because the CSVs are *appended* to, the
`[sweep]` line reports both the per-config counts (`emitted=`, `partials=`) and
the run totals written so far (`sol_total=`, `part_total=`) -- a fresh run into a
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

Work in a **frame**: the board rotated `--rotate` quarter-turns clockwise, so
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
cells lie inside the `--rounds 3` core at every W, so clue piece 139's placement
is inherited from the input and can never be created by a strip.

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

### What gets reported

The search is **exhaustive and deterministic** - no beam, no sampling, no random
seed - so it enumerates every break-free filling of the freed bands, and both
outcomes are exact. A complete board is a solution; finishing without one proves
this core admits no break-free refill of these bands. The only thing that can
weaken that is a budget (`--max_nodes`, `--config_time_sec`, `--max_wall_sec`,
`--max_boards`); when one bites, the summary marks that board **TRUNCATED**
instead of exhausted.

What comes out is **the furthest it got**: one board per input board, the state
that placed the most pieces. Pieces placed is the only ranking used, because it
is the one measure comparable across rounds - a per-strip level restarts each
round. `--ties N` widens that to N boards at the same depth, dropping any that
repeats an earlier one with a single frontier piece swapped, since those collapse
into the same board the moment a later stage frees the frontier.

Boards go to `<out_dir>/roundhouse_round<N>_rot<K>_W<w>_miss<B>.csv`, appended
one atomic line at a time, duplicates suppressed. The name carries `--rounds`,
`--rotate` and `--strip_width`, so runs with different geometry never share a
file -- while runs with the *same* geometry do, which is what lets a corpus sweep
accumulate.

**Two files, split by breaks.** A board with no mismatch goes to `miss0`; one
with mismatches goes to `miss<--max_breaks>`. So `miss0` is always a corpus you
can trust break-free and the two never have to be told apart afterwards. Routing
is on the board's *own* break count, so a greedy fill that happens to land
perfectly is filed with the clean boards. Both open on first write, so a run that
emits nothing leaves nothing behind. Three kinds of board:

| kind | when | id tag |
|---|---|---|
| solved | the board is complete **and** break-free: the puzzle, for this cut | `s` |
| deepest | the furthest the exhaustive break-free search got | `d` |
| filled | `--max_breaks B` bought a complete board with at most B mismatches | `f` |

Ids are `p<line><tag><n>`, `n` counting boards written by this run, so every line
is uniquely named and stdout names the id it just wrote.

**Boards are always written in the input's orientation** - the frame is rotated
back first, so cell `(r,c)` means what it meant in the input whatever `--rotate`
was. Every placed junction matches, so `score` is 480 minus the junctions a hole
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

**`--max_breaks B` finishes the board anyway.** A break-free refill usually does
not exist, and a board with 80 empty cells is awkward to hand on. With `B > 0`
the run takes the deepest break-free board it found and greedily fills every
remaining cell, spending at most B mismatched junctions - the same idiom as the
backtracker's `--break-mode stuck`: most constrained cell first, prefer a piece
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

Volume is set by `--ties` (boards kept at the deepest reach), `--max_boards` and
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
| `--border_row N` / `--border_row_N N` | 0 / 1 | first input CSV data line, and how many |
| `--strip_width W` | 5 | 2..5; chain length, and hence the core. 0 = narrowest usable |
| `--rounds N` | 3 | 1..3; bands freed and refilled (right, top, left) |
| `--rotate K` | 1 | quarter-turns before the cut, -3..3; negative turns anticlockwise |
| `--stop_row R` | last level | stop each strip at this level instead of its last |
| `--BL/--BR/--TL/--TR P` | -- | pin a corner piece by **original**-board role |
| `--max_breaks B` | 0 = off | after the exhaustive search, greedily fill the rest of the deepest board, spending at most B mismatches |
| `--max_nodes N` | 0 | node budget per input board |
| `--config_time_sec S` | 600 | wall-time budget per input board |
| `--max_wall_sec S` / `--max_boards N` | 0 / 0 | budgets for the whole run |
| `--ties N` | 1 | boards to emit at the deepest reach |
| `--only_complete` | off | emit only boards with all 256 pieces placed |
| `--selfcheck` | off | validate the oracle against brute force and exit |
| `--threads N` / `--verbose` | all / off | as Stage B |

Retired in the exhaustive rewrite and now accepted with a warning, so older
scripts keep running: `--beam_width`, `--mode`, `--frac_rand`, `--repeats`,
`--finalize_repeats`, `--lambda_Mahalanobis`, `--top_bottoms`,
`--emit_each_round`, `--emit_deepest`, `--seed`, `--max_partials`,
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

## Stage C -- the tail toolbox

Three tools, one canonical CSV, different philosophies. None of them has
closed a real 480/480 yet -- this is the open front of the project.

### E555_topper.py -- break minimizer ( the Stage C workhorse)

OR-Tools CP-SAT model over the open cells: variables for (piece, rotation,
four exposed colors) with `AddAllowedAssignments` tables filtered by the frame
rule, `AddAllDifferent` per piece class. Lexicographic objective by dominating
weights: (1) minimize total breaks; (2) push unavoidable breaks to the nearest
horizontal border; (3) slide them along it to the nearest corner.
`--work-rows`/`--unused_top_rows` implement the **overlapping sliding window**
(see the strategy guide in the file header and
`pipeline/topper_sweep.sh PRESET=window`): move a constant-size work band across the
board in overlapping steps, so early mistakes stay repairable.
`--allow_break_increase` trades a slightly worse total for a longer push in the
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

**Which border is opened.** `--side` picks the band(s), each `--work-rows`
deep:

| `--side` | opens | use |
|---|---|---|
| `T` (default) | top rows | the classic upward sliding window |
| `B` / `R` / `L` | bottom rows / right cols / left cols | clean-up where breaks got stranded |
| `TR` / `TL` | an L of top + right (or left) | fold everything into one corner |
| `TB` | top **and** bottom rows | test whether the opposite borders can be re-cut against each other |

`--unused_rows N` (the old `--unused_top_rows`, still accepted) applies to each
open band: its outermost N rows/cols are **unset to 999** and locked empty for
the run -- the freed pieces rejoin the pool, and the next window recovers the
gap. Two rules keep a `--side` run confined to that side: a break cell is
unlocked only if it lies in or touches the band (a break stranded on the far
side stays put, which is what keeps a one-sided run one-sided), and empty cells
outside the band stay empty.

**`--holes FILE`** replaces all of that with an explicit 16x16 0/1 mask -- the
same dialect the ender and the backtracker read -- and the mask is taken
literally: free = exactly the cells marked `1`, with no band, no adjacency
expansion and nothing held empty, so `--side`, `--work-rows` and
`--unused_rows` no longer apply (the last is rejected outright). Use it for a
region a band cannot express: an L around one corner, a ragged patch following
a cluster of breaks, or the board's interior -- including the `(16-2W)^2`
centre the roundhouse retains and can never itself reopen.

`--report_best N` is a real beam here: rank 1 is the optimum, and each further
rank is re-solved under a no-good cut requiring >= `--beam_diff` cells to differ
from every board already emitted, with the objective capped at
`--beam_slack` extra breaks. A naive "last N incumbents of one search" beam
would return near-duplicates of rank 1 that carry almost nothing into the next
window step; this one does not. The beam stops early rather than padding with
duplicates, and the run summary says how often that happened: on a tight band
the default `--beam_slack 1` often admits nothing at all (on
`data/board_example_462.csv` with a 2-row band, the nearest distinct board
costs 5 more breaks), so widen it when you want a genuinely wide beam. `--verbose` prints an ASCII map of the open band, and
boards with nothing broken or empty inside the band skip the solver entirely.

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

The pairs of the third driver are inseparable: `--unused_rows N` unsets those
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

`--break-mode` selects between two very different engines, and the distinction
matters more than any other parameter here:

- **`stuck` (default) -- greedy dives, for triage.** A dive takes an exact fit
  where one exists and a minimal break where none does, never backtracks, and
  therefore always reaches 256 pieces in one pass. Because piece-type counts are
  exactly balanced, the candidate set is never empty, so a dive is O(cells) and
  cannot fail. `--stuck_restarts N` (default 100 000, ~5-10 s on four cores) runs
  N randomized dives and keeps the best. Divergence comes from random tie-breaking
  alone, and that is ample: a 200 000-dive batch produced 200 000 distinct boards.
  Throughput is ~9k-18k dives/s on four cores, so N in the millions is practical.
  This mode **proves nothing** -- it never establishes
  that a board cannot be completed with fewer breaks. Expect it to land well
  above a well-optimized incumbent (~28 breaks best-of-200k on a 74-cell region
  whose input carried 18); its job is ranking candidate partials cheaply, not
  improving them.
- **`any` / `lds` -- exhaustive, for proof.** These keep the iterative-deepening
  ladder over `k = input_breaks .. --max-mismatch`, so an exhausted level is a
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
so under `--solution-limit 1` it returns a different first closure: two triage
passes (forward and `--reverse`) yield border-distinct partials that the
finalizer's fixed-mode dedup keeps separate. No effect on `2sides` or `4sides`.

Parallelism is automatic: one record per thread, or every thread on one record's
search when there are no more records than threads (`--all-for-one` forces the
latter). Note that this search is memory-system bound, not scheduling bound -- on
a 4-core laptop, four *independent* single-threaded runs already slow each other
to 2.44x aggregate, and the threaded search achieves 2.37x, so there is little
left for tuning to recover. Crash-safe: appends an improving checkpoint line per
record; output is re-feedable.

### E555_ender.py -- the closer, two neighbourhoods ( power tool)

For a full board the topper has already tidied. One CP-SAT engine opens a slice
of the board, caps how many pieces may actually move (`--max-changes`), and
re-solves to cut breaks -- never returning a board worse than its input. An
internal escalation ladder climbs (reach x budget) rungs, warm-starting from
the best board so far and stopping the instant breaks hit zero; you set only the
ceilings (`--reach`, `--max-changes`). `--mode` picks the neighbourhood and the
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

---

## Tools

- **`tools/E555_viewer.py`** -- ASCII board (`#` marks broken junctions),
  placement/edge/solid statistics, frame-violation check, e2.bucas.name URL;
  `--diff A B` overlays two rows of a CSV. `--seed PATH` (defaults to
  `./seed_Edge5.txt`, then the repo's `data/` copy).
- **`tools/E555_rank.py`** -- ranks and sorts board CSVs by what the score
  cannot see. Eighteen breaks spread over seven rows is a mess; the same
  eighteen packed into rows 14-15 is nearly finished. Per board it derives
  `breaks`, `solid` (the viewer's fully-satisfied pieces), `break_rows` /
  `break_cols` (how many distinct lines hold a break -- the compactness
  measure), `span` (their bounding box), `clean_b/t/l/r` (contiguous
  break-free rows or columns from each border; `clean_b` is the old
  "completed rows"), and `corner_d` (the distance the breaks still have to
  travel to their nearest corner -- the quantity `E555_topper` minimizes
  after the break count, and a good tie-breaker). `--sort` takes any of them,
  best board first, several files at once; `--emit` re-orders the input rows
  verbatim, so the canonical format never changes and old files rank fine.
  `--emit FILE --rescore` instead rewrites every row canonically
  (`config_id, score, pos[256], rot[256]`) with the score recomputed from the
  seed -- the one way to make a mixed corpus sortable by field 2, since Stage B
  writes its solution index there and older dialects write other things again.
- **`tools/E555_rotate.py`** -- turns every board in a CSV by `N` quarter-turns
  clockwise, same convention as `E555_roundhouse --rotate`. Lossless: the frame
  rule is identical on all four sides, so a rotated board is the same board
  seen from a different corner, and the tool re-scores every row before and
  after to prove it. What changes is which rows and columns hold the open cells,
  and therefore the direction the next (direction-biased) stage attacks them
  from -- run a board at 0/1/2/3 and hand all four to the finalizer, the
  roundhouse or the topper. `--all` writes all four turns in one pass, and
  `--holes FILE` turns a mask alongside the board so the next stage still opens
  the same physical pieces.

---

## File formats

| producer | file | layout |
|---|---|---|
| Stage A | `rotations.csv` | `# comment` lines + `id, spin[0..255]` (60 border spins, 196 zeros) |
| beamer | `beam_completions_<border>_<row>.csv` / `..._random_<row>.csv` | `config_id, sol_idx, pos[256], rot[256]` (514) |
| beamer | `sweep_checkpoint.txt` | resume state, one line |
| roundhouse | `roundhouse_round<N>_rot<K>_W<w>_miss0.csv` (break-free) and `..._miss<B>.csv` (with breaks) | canonical 514-field layout, ids `p<line><tag><n>` |
| finalizer | `beam_completions_finalized_<row>.csv` | same 514-field layout, ids `p<line>r<repeat>l<column>` |
| Stage C (all) | output CSV | **canonical**: `config_id, score, pos[256], rot[256]` (514) |
| backtracker | `<out>.checkpoint.csv`, `<out>.status.csv`, `<out>.best_*.csv` | canonical rows / diagnostic sidecars |

---

## Build & Run

```bash
make                        # bin/E555_beamer, bin/E555_finalizer,
                            # bin/E555_roundhouse, bin/E555_backtracker
pip install ortools         # only for topper / ender

# no Stage A needed -- the 5-minute demo:
bash examples/01_beamer_quickstart.sh

# the full pipeline:
ANNEAL=1 bash examples/01_beamer_quickstart.sh   # Stage A then Stage B
PARTIALS=beam_out/beam_completions_0_10.csv bash examples/02_finalizer_regrow.sh
BOARDS=final_out/beam_completions_finalized_12.csv \
    bash examples/04_stage_c_close.sh

# validate everything (includes the synthetic-solution regression):
bash tests/run_tests.sh
```

---

## Search-strategy trade-offs

After the one-time DB build, wall-clock is roughly
`(configs searched) x (rows reached x K x pool_factor x scan_factor x const)`.
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
  (deeper resampling, costlier); higher = cheap top-row re-rolls.

**Practical default:** breadth first (`--random_edges` or many border rows,
moderate K), finalize the survivors from row 8-10 with repeats, topper the
best finals through the sliding window, then throw the backtracker and the
ender at anything above ~460.

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
| `tools/E555_rank.py` | Ranks/sorts board CSVs by compactness, solidity, clean rows; `--rescore` rewrites them canonically. |
| `tools/E555_rotate.py` | Turns every board in a CSV by a quarter-turn multiple, losslessly. |
| `data/` | Seeds, known synthetic solution, example boards, masks (see `data/README.md`). |
| `examples/` | One small script per tool: read these first. |
| `pipeline/` | The full pipeline, the board farm and the topper sweeps -- long unattended runs. |
| `tests/run_tests.sh` | The release gate. |
