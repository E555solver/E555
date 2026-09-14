# data/fixedframe/ -- results of one fixed-frame run

Output of `tests/run_fixedframe_farm.sh` + `tests/E555_frame_stats.py` on
`seed_Edge5`, kept so the next run has something to compare against. See
[../../tests/README.md](../../tests/README.md) for what the experiment is.

**This run**: 478 row-11 boards from 167 independent border configs, four sides,
600s each on 4 cores. N_eff = 282. Frame: orientation 0, corners
`BL=3 BR=2 TL=0 TR=1`. Every board verified on-frame; the five pinned clue
pieces were recovered at their own cells (the positive control).

| file | what it is |
|---|---|
| `pieces.csv` | one row per inner piece: ring distribution, lift per ring, mean ring, top-band lift, weighted observations. Sorted by mean ring. |
| `agreement.csv` | Spearman rank correlation of the per-piece mean-ring ranking between each pair of sides, over all cells and over the 8x8 core. |
| `exclude_pieces.txt` | the 20 pieces most concentrated in rows 12..15, ready to paste into `--exclude_pieces`. |

**The headline**: mean rho = **+0.242** over all cells, **+0.224** over the core.
Positive on all six side pairs, but weak. Four independent views of the same
frame lean the same way without agreeing closely -- which is what a real but
small effect looks like, and equally what an under-sampled one looks like. More
borders separate those two; more analysis of these 478 boards does not.

Mean ring spans only 2.27 to 3.70 across 191 pieces, so whatever preference
exists is a tendency, not a rule.

Nothing here is evidence that excluding those 20 pieces helps. That is settled
by counting `filled=12` lines with and without them -- see tests/README.md.
