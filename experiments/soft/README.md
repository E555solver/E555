# E555_soft -- relaxed boards settled into real ones

```bash
cd experiments/soft && make ARCH=generic        # needs NLopt (libnlopt-dev, or NLOPT=/prefix)
bin/E555_soft seed.txt boards.csv output.csv [--core N] [--starts N] [--keep K]
```

The free cells of a board hold probability mixes over (piece, spin) instead of
pieces. The expected number of breaks is minimised while a falling temperature
lets every mix settle on one choice (mean-field annealing). The result is
rounded to a real board and hill-climbed. The aim was other starting boards for
`E555_diver --reopen` and the ender.

- **Free region.** By default, the open cells of each board. `--core N` keeps
  the N placed pieces nearest the centre and frees the rest; `--core 0` builds a
  whole new board. To lift the top rows of complete boards, cut them first:
  `E555_diver ... --holes top:K --end_dive 0 --emit_score 0`.
- **Method.** One softmax per free cell. NLopt's L-BFGS minimises
  `B + T sum y log y`, where B is the expected number of breaks, with the piece
  sums in an augmented Lagrangian. T falls from 0.45 to 0.12 over 30 stages.
  Each start is then rounded by a linear assignment per piece kind and climbed
  with `dv_climb` (`src/C_tail/E555_dive.c`).
- **Output.** Up to `--keep` distinct boards per input (default 4, from 16
  starts), as canonical rows `<id>_s<start>`. The same at any `--threads`.

The source header explains the model; the tuning is the `SOFT_*` defines.

## Why not the first design

The first version kept the mixes as variables in [0,1], put both sums in the
Lagrangian, and raised a concave penalty `mu sum y (1 - y)` from 0. The
synthetic solution with its top 4 rows cut, 16 starts:

| variant | rounded mean | climbed best |
|---|---|---|
| y in [0,1], both sums in the Lagrangian, mu 0.02 -> 4 | 381.8 | 440 |
| softmax per cell, the same mu ladder | 405.8 | 441 |
| softmax, entropy, T 0.5 -> 0.005 | 456.6 | 466 |

- Each cell sum spans ~170 variables. That made the bounded problem too stiff
  for L-BFGS, and the mixes never settled.
- The expected breaks are not convex. A ladder starting at mu = 0 freezes in
  the first local minimum it meets; the entropy gives a convex start.
- NLopt's `AUGLAG` evaluates every constraint gradient densely, and `MLSL`
  samples the box, not the assignment polytope. Neither is used.

## Results (seed_Edge5, 4 threads)

Refills with a known zero-break answer: 15 boards of `data/best_463.csv`,
turned 180 degrees, with their top K rows cut. The table gives breaks inside the
cut region:

| rows cut | soft | dives 10000/20000 |
|---|---|---|
| 4 | 5/15 perfect, 10.0 breaks, 12 s a board | 15/15 perfect, 0.16 s |
| 6 | 0/15 perfect, 26.5 breaks, 34 s | 11/15 perfect, 5.1 breaks |
| 8 | 0/15 perfect, 32.1 breaks, 69 s | 0/15 perfect, 25.8 breaks |

Stage B partials: 35 row-11 boards of `data/E565_FixCorners23.csv.gz`. Each arm
was re-dived with `E555_diver --reopen auto` (15 s a board), then the ender
took each arm's best 5:

| arm | start | after `--reopen` | ender on the best 5 |
|---|---|---|---|
| dives 10000/20000 | 458.3 | 459.8 (no gain from 15 s more) | one 461 -> 462 |
| soft, 16 starts | 454.8 | 459.7 | no gain |

The soft boards share only 15% of their free cells with the dived ones. Against
the dives' plateau, a re-dived soft board was better on 1 of 35 boards, equal
on 28 and worse on 6.

**Verdict.** At these budgets it is not a better source of starting boards than
the dives.
