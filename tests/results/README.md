# tests/results/ — the run this study is based on

Everything here was produced by the scripts in `tests/`. See
[../README.md](../README.md) for what the experiment is and how to reproduce it.

**Start with [`report.html`](report.html)** — open it in a browser. It is the
whole study: static figures with the argument written beside them, in the order
the argument runs. Everything else in this directory is the data behind it.

## The run

**55,712 boards from 1,741 independent borders**, four sides × 3000 s on 4 cores
at `--stop_row 10`. N_eff = 1,741 (the border is the independent unit). Frame:
orientation 0, corners `BL=3 BR=2 TL=0 TR=1`. Every board verified on-frame; all
five pinned clue pieces recovered at their own zones. Per side: 460 / 428 / 415 /
438 borders.

## Files

| file | what it is |
|---|---|
| `report.html` | **the study** — self-contained, all figures embedded |
| `zones.csv` | one row per piece: lift and 95 % bootstrap interval in each of the nine zones, its best zone, its far-side score |
| `replication.csv` | Spearman ρ between the two sides that see each zone |
| `exclude_pieces.txt` | the 12 far-side candidates, as `--exclude_pieces` expects them |
| `summary.json` | every headline number the report renders, including the `reserve` block |
| `arrays.npz` | the raw tensors — re-run `E555_frame_plots.py` from this without recomputing the stats |
| `figs/*.png` | the seven figures, standalone |
| `data/corpus_row10.csv.gz` | the canonicalised corpus, 55,712 boards (16 MB packed, 79 MB raw) |
| `data/ab_*.log.gz` | the far-side A/B arms' beamer logs, with `--verbose` per-row lines |
| `data/abctl_*.log.gz` | the undecided-control arms' logs |
| `data/ab_farside.json`, `data/ab_control.json` | the parsed A/B results |
| `data/control12_undecided.txt` | the control set, drawn from pieces whose far-side interval straddles zero |

The 6.4 GB chain database is **not** here by design — recompute it, it takes
~80 s (`--db_file` writes the cache).

## What it found

**Pieces do have corners.** Two independent sides see each corner and agree on
which pieces go there at mean Spearman **ρ = +0.701** — BL +0.643, BR +0.706,
TL +0.701, TR +0.755. Under a label shuffle the same statistic is **+0.006**.
The sides use different borders, a different RNG stream and a different search
direction, so agreeing this closely is not the heuristic talking to itself.
(The first run, 117× smaller, gave +0.273.)

**246 of 247 pieces** have at least one zone whose 95 % interval clears 1.00×,
against **51 (39–69)** from the same procedure on label-shuffled corpora.

Strongest per corner (lift, × an average piece of the same kind):

| corner | pieces |
|---|---|
| BL | 185 (2.02×) · 171 (1.88×) · 15 (1.86×, edge) · 155 (1.80×) · 59 (1.77×, edge) |
| BR | 32 (2.40×, edge) · 25 (2.24×, edge) · 231 (2.13×) · 100 (2.01×) · 94 (1.97×) |
| TL | 29 (1.95×, edge) · 30 (1.84×, edge) · 150 (1.83×) · 103 (1.79×) · 116 (1.76×) |
| TR | 124 (2.74×) · 244 (2.24×) · 41 (2.03×, edge) · 6 (1.94×, edge) · 121 (1.92×) |

**Every piece has an anti-corner.** Pieces preferring BL actively avoid TR and
vice versa — the clearest structure in the corpus, and the property the far-side
score depends on.

**Almost nothing prefers an edge zone.** Pieces sort into the four corners and
the centre: the constraint propagates into a corner but not sideways along an
edge.

**The mechanism is colour.** Comparing the four corners against each other rather
than against the middle, each has its own signature: colour 18 is 1.64× in TR
against 0.64× in BL and BR, colour 16 is 1.54× in BL, colour 7 is 1.53× in TL,
colour 11 is 1.52× in BR.

## The A/B, and what it settled

Three ways in this frame at `--stop_row 12`: 45 min for the far-side arm and its
baseline, 15 min for the control and its own baseline.

| excluded set | chains left | database lost | row-1 survival | baseline kept |
|---|---|---|---|---|
| none | 2,730,016,036 | — | 32.3 % | 100 % |
| 12 far-side (mined) | 2,137,097,200 | 21.7 % | 7.2 % | **22.4 %** |
| 12 undecided (control) | 2,317,226,804 | 15.1 % | 0.7 % | **2.1 %** |

**Excluding twelve pieces is fatal either way** — the far-side arm's row-10
frontier is 0.03× the baseline's, the control's 0.14×. Neither arm emitted a
single board, because the beamer only emits at `--stop_row` and nothing reached
row 12.

**But the ranking is not arbitrary, and this is the useful result.** The control
removed a *smaller* share of the database and did **ten times more damage**.
Database size therefore does not explain the harm: *which* pieces you remove
dominates, and the corpus picked pieces the low rows can genuinely spare —
spending more of the database for a tenth of the cost. The dose was wrong, not
the statistics.

## What the A/B did not test

It measured **P(board emitted)**. It did not measure **P(completable | emitted)**
— and since Stage C is the bottleneck, few-but-elite could be a win. Measured
over the 14,720 side-0 boards (the `reserve` block in `summary.json`):

- the beam already leaves **4.37 of the 12** far-side pieces unplaced against
  **3.46** expected under indifferent placement;
- **2.6 %** of boards leave **8 or more**;
- **83 % of the variance is decided board by board**, not by the border.

So elite boards already exist in ordinary output and can be *selected* at zero
search cost, where *forcing* the property cost ~100× the throughput. And because
the property is a per-board one, `--max_per_config` currently caps exactly the
distribution you would want to sample deeply.

## Two caveats that bound everything above

**The corner assignment is a bet.** No clue pins a corner, so the four corner
pieces can fill the four corners 4! = 24 ways and only one is the solution's.
Everything near the border is conditioned on that choice; the centre barely
notices.

**The prior does not transfer to the production corpus as measured.** Checked
against `data/best_463.csv` and `data/board_example_462.csv`: not one matches
this frame, and most satisfy *zero* clues — they came from free runs. Re-key the
table from "prefers the top-right zone" to "prefers the corner holding
corner-piece 1" and it becomes frame-independent. §09 of the report has the
detail.
