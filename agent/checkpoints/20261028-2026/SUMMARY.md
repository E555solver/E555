# Campaign 20261028-2026: SUMMARY
Final: REFEREE verdict=VALID score=465 breaks=15 placed=256 frame_violations=0 id=lowB_free_Maha10_1568_redive0_465 file=frontier/frontier.csv
Stop rule: budget (run_time 24h01m of 24h00m; 220 runs; no new best, so the frontier is still the shipped 465).
Best board: frontier/frontier.csv = data/best_465.csv (unchanged). Nothing beat 465; no board scored above it.
Best fresh boards of this campaign: five 464s (distilled_2, FixCorners23 r3c122, distilled_18, distilled_20, distilled_23), all from
the F4-partials + F3 distiller pipeline; frontier/elite.csv holds 1x465, 8x464, 41x463.
Winning family: none (no new best). Most productive: F4 partial beam (no dives) + lab wide distiller (finish 5000/2000, probe 12N): 40 distillates, best 464 x4, 463 x26, 462 x10 (plus the FixCorners23 464).
Key finding: the 465 is a strong local optimum. Every designed exact hole up to 62 cells is proven unimprovable,
and its top 6 rows lifted, re-solved by E555_soft and reopened all return to the identical 465.

## Final referee line
REFEREE verdict=VALID score=465 breaks=15 placed=256 frame_violations=0 id=lowB_free_Maha10_1568_redive0_465 file=frontier/frontier.csv

## Winning family / tool / command / file
No run beat the starting 465. The board that holds the frontier is the shipped data/best_465.csv.
The best new material came from:
```
bin/E555_beamer data/seed_Edge5.txt --random_edges --samples 4 --top_columns 4 --db_file E555.db --backtrack_row 6 \
  --beam_width 10000 --stop_row 11 --extend_nodes 0 --lambda_Mahalanobis 10 --wall_time 300 --threads 4 --rng_seed 218 --out_dir runs/20261029_010236_beam
python3 tools/E555_distiller.py runs/20261029_010236_beam/beam_completions_random_11.csv --top 10 --out pool/distilled_2.csv   # -> 464
```

## Five best runs (run_log.csv, by referee score)
| iso | family | tool | params | score |
|---|---|---|---|---|
| 2026-10-28T20:35Z | F1 | diver_reopen | REOPEN=auto+1 SPB=480 seed=101 on the 465 | 465 (= input) |
| 2026-10-28T20:59Z | F2 | ender | deep btl=420 seed=404 on the 465 | 465 (= input) |
| 2026-10-28T21:08Z | F8 | finalizer | from=7 SR=11 BTR=9 on the 465 | 465 (the same board) |
| 2026-10-28T21:09Z | F8 | finalizer | from=5 --free_sides, 20 repeats | 465 (near-duplicate) |
| 2026-10-28T23:32Z | F1 | diver_reopen | elite 5 (the 465's 463 root), 240 s | 465 (rediscovered) |
Best runs that produced new boards: F3 distilled_2 (464), distilled_fc23 (464), distilled_18 (464), distilled_20 (464), distilled_23 (464).

## Score over time
`awk -F, 'NR>1{print $2, $8}' run_log.csv`: flat at best=465 throughout. Fresh-board bests per run rose from 461-463 (full-dive beams,
first 4 h) to a steady 462-464 per distiller cycle once the partials+distiller pipeline took over (from 01:00).

## Families exhausted (STRATEGY.md)
- F1 reopen on elites (auto, auto+1, auto+2, top:5, plateau, prior/nogo, up to 1194 rounds on one board): +0 except the 465's own root, which rediscovers the same 465.
- F2 ender deep (420 s) on the 465, a 463 and two 464s: +0.
- F5 rebuild top:6: loses 2-7. F8 finalizer on the 465 (from 7/5/3): 465 at best.
- F4 variants: annealed borders worse (461); BR=5 too many boards; BR=7 SR=12 unreachable; BW=20000 and Maha 20 / lambda_J 0.5 no better than Maha 10.
- F6 exact holes (new tools/holemaker.py + btdrive.py, backtracker --order frontier --break_mode any): ~300 holes on the 465, five 464s and five 463s;
  every finished one a proof (no fill with fewer breaks), up to 62 cells; holes covering the 12-break cluster time out (k=10 not exhausted; lds too).
- F7 E555_soft (experiments/soft, built in the lab with libnlopt-dev): core 0 -> 427; top:6 refill -> 453 -> reopen returns the identical 465; top:8 -> 458.

## Three things to try next
1. Volume on a bigger machine: the partials+wide-distiller cycle gives a 464 in about 1 of 8 cycles on 4 cores (~22 min each). 32+ cores
   would run ~40 cycles an hour; the 465 itself came from such a tail. Seed many cycles in parallel and pool the 464s.
2. Exact holes that reach the row under the damage: the 465's big cluster (19 cells, 12 breaks) needs exhausting k <= 11. A backtracker
   option to start iterative deepening at a given level (or an upper-bound-first DFS) would make 80-120-cell holes over rows 9-15 provable or improvable.
3. Lift-and-rebuild from deeper rows with the 465's piece set: the soft experiment shows rows 10-15 of the 465 are an attractor given rows 0-9.
   Re-search rows 6-9 with the finalizer (locked bottom 0-5, --free_sides, many repeats, no dives), distil those partials, then reopen auto+2.

## Machine
Claude Code cloud container, Intel Xeon @ 2.10 GHz and @ 2.30 GHz (the session moved twice: worker restarts at 22:25 and 23:48 UTC on
Oct 28, cpu_changed=yes each time; the lab was kept both times, no restore needed), arch v3, 4 threads, 15 GB RAM. The 6.8 GB E555.db
had to be re-warmed in page cache after each move (91 s and 491 s). Lost runs: 3 (two to worker restarts, one to the 2 h background-task limit).
No usage-limit pauses. Checkpoints pushed to agent_runs roughly every 30-70 min (about 22 in total), last one after this summary.
