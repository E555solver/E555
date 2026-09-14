# tests/results/ -- one fixed-frame run, kept as a baseline

Output of `tests/run_fixedframe_farm.sh` + `tests/E555_frame_stats.py` on
`seed_Edge5`, so the next run has something to compare against. See
[../README.md](../README.md) for what the experiment is and how to reproduce it.

## This run

**55,712 boards from 1,741 independent borders**, four sides × 3000 s on 4 cores
at `--stop_row 10`. N_eff = 1,741 (the border is the independent unit). Frame:
orientation 0, corners `BL=3 BR=2 TL=0 TR=1`. Every board verified on-frame; all
five pinned clue pieces recovered at their own zones.

Per side: 460 / 428 / 415 / 438 borders.

| file | what it is |
|---|---|
| `zones.csv` | one row per piece: lift and 95 % bootstrap interval in each of the nine zones, its best zone, and its far-side score. |
| `replication.csv` | Spearman ρ between the two sides that see each zone. |
| `exclude_pieces.txt` | the 12 far-side candidates, ready to paste into `--exclude_pieces`. |

## What it found

**Pieces do have corners.** Two independent sides see each corner, and they agree
about which pieces go there at mean Spearman **ρ = +0.701** — BL +0.643,
BR +0.706, TL +0.701, TR +0.755. Under a label shuffle the same statistic is
**+0.006**. The sides share different borders, a different RNG stream and a
different search direction, so agreeing this closely is not the heuristic talking
to itself.

**246 of 247 pieces** have at least one zone whose 95 % interval clears 1.00×,
against **51 (39–69)** from the same procedure on label-shuffled corpora.

Strongest per corner (lift, ×average piece of the same kind):

| corner | pieces |
|---|---|
| BL | 185 (2.02×) · 171 (1.88×) · 15 (1.86×, edge) · 155 (1.80×) · 59 (1.77×, edge) |
| BR | 32 (2.40×, edge) · 25 (2.24×, edge) · 231 (2.13×) · 100 (2.01×) · 94 (1.97×) |
| TL | 29 (1.95×, edge) · 30 (1.84×, edge) · 150 (1.83×) · 103 (1.79×) · 116 (1.76×) |
| TR | 124 (2.74×) · 244 (2.24×) · 41 (2.03×, edge) · 6 (1.94×, edge) · 121 (1.92×) |

**Every piece has an anti-corner.** Pieces that prefer BL actively avoid TR and
vice versa — the clearest structure in the whole corpus, and the property the
far-side score depends on.

**Almost nothing prefers an edge zone.** Pieces sort into the four corners and
the centre, not into nine regions: the constraint propagates into a corner but
not sideways along an edge.

**The mechanism is colour.** Each corner has its own signature once the four are
compared against each other rather than against the middle: colour 18 is 1.64×
in TR against 0.64× in BL and BR, colour 16 is 1.54× in BL, colour 7 is 1.53× in
TL, colour 11 is 1.52× in BR.

## What it did not find

**The far-side ranking does not replicate nearly as well.** The three sides that
see rows 13–15 agree at ρ = +0.417 (1 vs 2), +0.391 (2 vs 3) and **−0.075**
(1 vs 3). 90 pieces are significantly far-side, but the ordering among them is
much less stable than the corner preferences. The corner result and the exclusion
list do not stand or fall together.

## An aside worth keeping

Of every 100 borders the sweep tries, about **96 die at row 1 or 2** — the rows
the clues pin — and **98 % of the survivors reach row 10**. The survival curve is
a cliff followed by a plateau. Rows 3–10 are close to free; the beam is not
grinding down, it is being killed at the clue rows and then coasting. So an
exclusion list helps or hurts mainly through what it does to rows 1–2, and taking
pieces away can only make those harder to thread.

## The A/B, and what it settled

`tests/run_fixedframe_ab.sh` ran the beamer in this frame three ways, 45 min for
the far-side arm and its baseline, 15 min for the control and its own baseline.
`ab_farside.json` and `ab_control.json` hold the per-row numbers.

| excluded set | chains left | database lost | row-1 survival | baseline kept |
|---|---|---|---|---|
| none | 2,730,016,036 | — | 32.3 % | 100 % |
| 12 far-side (mined) | 2,137,097,200 | 21.7 % | 7.2 % | **22.4 %** |
| 12 undecided (control) | 2,317,226,804 | 15.1 % | 0.7 % | **2.1 %** |

**Excluding twelve pieces is fatal either way** — the far-side arm's row-10
frontier is 0.03× the baseline's, the control's 0.14×.

**But the ranking is not arbitrary, and this is the useful result.** The control
removed a *smaller* share of the database (15.1 % against 21.7 %) and did **ten
times more damage**. Database size therefore does not explain the harm: *which*
pieces you remove dominates, and the corpus picked pieces the low rows can
genuinely spare — spending more of the database for a tenth of the cost.

So the dose was wrong, not the statistics. A chain holds five pieces and needs
all five, so the database shrinks as roughly the fifth power of the surviving
piece fraction: three pieces costs ~7 % against 22 % for twelve. That is the
experiment to run next, along with a score penalty instead of a ban, and handing
the same prior to Stage C where there is no chain database at all.
