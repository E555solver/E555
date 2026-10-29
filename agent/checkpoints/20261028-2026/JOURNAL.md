
## 2026-10-28 20:27 UTC — bootstrap
Settings: BUDGET=24h RUN_CAP=540 PLATEAU=24h/99 CHECKPOINT=30m RESULTS_BRANCH=agent_runs. Lab fresh, 4 threads, ortools yes, stage_b_ok, db_on_disk. First checkpoint 04f8377. Wake-up task trig_01CjgZWc4PiRcUCPFd5Au1dK (hourly at :27).

## 20:28-21:00 — first sweep
- F1 reopen auto+1, 480 s on the 465 (seed 101): 465 -> 465. Polished; no gain.
- F4 beam BW=10000 BR=6 SR=11 ED=10000 EP=20000 seed 202, 480 s (built E555.db): 3866 boards, best 461, all one random border (rndb0l0).
- F1 reopen auto 120 s x4 diverse roots from that beam (seed 303): 457->460, 459->460; best stays 461.
- F2 ender deep 420 s on the 465 (seed 404): +0.
Decision: the 465 is hard-polished. Next: F5 rebuild top:6 on the 465 and the 464s; more F4 seeds for new borders.

## 21:00-21:40
- F5 rebuild top:6 SR=11 on elite top 4 (seed 505): runs in 0.6 s, best 462 (all worse than inputs). Lifting 6 rows of polished boards loses.
- F1 diver direct, alternating reopen auto/auto+1/top:5, --plateau, copies 8, 480 s on the 464 rnd2027_w16 (seed 606): 1194 rounds, +0.
- F8 finalizer (new family: lock bottom of the 465, regrow): from=7 width 1 (one completion, 465 itself); from=5 --free_sides 20 repeats: 465 x20 (one near-duplicate alt p0r0l1), 463, 462; from=3 BW=100000 BTR=8: best 457. The 465's tail is not beaten from its own bottom.
- F4 beam seed 203: 4316 boards, raw best 463 (rndb0l0). F1 reopen 60 s x8 distinct top boards: one 461->462 only. F2 ender deep 420 s on the 463: +0.
Decision: beam boards come out polished; the gain source is new beams. Keep cycling F4 with new seeds; try wider beam / different BACKTRACK_ROW / STOP_ROW 12; F2 only on >=463.

## 21:40-22:50
- F4 seed 204 ED=2000 EP=10000: 3 configs, 13488 boards, best 462 (14 at 461). Lighter dives cover more borders, no better max.
- F3 distiller on beams 203+204 (top 5): 463 (the same board), +1 on one probe. No gain.
- F4 with annealed borders (Stage A 16 restarts, seed 11; beamer tb6 tc12 tau0.1 xn0 ED=2000): 3 configs -> 460; 68 configs over 2 borders -> 461. Worse than random edges; abandoned.
- F4 random edges BW=20000 seed 207: 462. Maha10 (--lambda_Mahalanobis 10) seed 208: 462 x2, 461 x19 from 4203 boards (better tail density). Maha10 BR=5 seed 209: 86k boards, wall hit before writing anything (lost run; never BR=5 with full dives).
- 22:25 worker restart, CPU changed (2.10 -> 2.30 GHz Xeon), lab kept. Seed 210 killed (logged lost). Rerun stalled 8 min at row 4: the 6.8 GB E555.db was cold in page cache. `cat E555.db > /dev/null` (91 s) fixed it; rerun seed 210 BW=20000 Maha10: 461 x29.
Rule: after any restart, warm E555.db before F4.

## 22:50-00:05
- F2 ender deep 225 s x2 on harvested 462s (seed 406): +0. pool/harvest.csv (tools/harvest.sh) collects distinct >=462 beam boards; consumed hashes in pool/consumed.txt.
- F4 Maha10 seeds 211 (461), 213 (+lambda_J 0.5: 462), 214 (Maha 20: 462), 215 (461). BR=7 SR=12 seed 212: row 12 unreachable (r11 457 boards, r12 0).
- New tools/f1e.sh: elite reopen, 240 s/board, auto/auto+1 alternating, plateau, copies 8. Elites 3-4 (464s): +0. Elite 5 (463, the 465's root): -> 465, the known 465 again (dedupe merges it). Elites 6,7,8: +0.
- 23:48 second worker restart (CPU 2.30 -> 2.10 GHz); elite 6-7 run killed 28 s in (logged lost). DB warm-up took 491 s on this machine.

## 00:05-01:55
- F4 seed 216 (Maha10): 461. Elite reopen (f1e, 240 s/board): elites 9-14 auto/auto+1, 15-18 auto+1/auto+2: all +0. Elite reopen is exhausted.
- F4 BR=5 BW=256 seed 217: 9061 boards, 461.
- NEW: F4 partials (no dives) + F3 distiller. Beamer --random_edges --samples 4 --top_columns 4 BW=10000 BR=6 SR=11 xn0 Maha10, wall 300, seed 218: 57980 row-11 partials over 15 configs. Distiller top 10 (4 pieces, ~33 min): screen 300 dives -> finish 4000 -> probe 40: best **464** (rndb0l1, screen 457, finish 461, probe 464), plus 463 x2. Best fresh board of the campaign.
- F1 reopen 480 s on that 464 (seed 616): +0. F2 ender deep 420 s (seed 407): +0.
Decision: the partials+distiller pipeline gives a better tail than full-dive beams (spreads over 15 configs, focuses dives). Repeat it with new seeds; feed >=463 to elite and the 464s to F1/F2.

## 01:55-04:15 — partials + distiller cycles
- seed 219 -> distilled_3 (top 10): 463 x2; F1 reopen auto+1 45 s x10 on them: two 462 -> 463.
- seed 220 -> distilled_4: 462. seed 221 -> distilled_5 (top 20: finish 8000, probe 80, ~55 min): 463 x1, 462 x18.
- seed 222 -> distilled_6 (top 10): 463 x2.
- Considered rotate --sink (opens 16(M+2) cells incl. the bottom border: too large for the closers) and the whirlpool (900 s laps > cap): skipped.
Decision: keep cycling (top 10, ~35 min); every 463 joins elite.csv.

## 04:15-05:30
- seed 223 -> distilled_7 (top 10): 463.
- F7-ish: tools/E555_distiller_wide.py (lab copy): finish 5000/2000 instead of 20000/5000, probe 12 N instead of 4 N. seed 224 -> distilled_8: 463, 27 of 120 probes improved, cycle ~22 min instead of ~35. Kept as the default cycle.

## 05:30-07:35 — user pointers: backtracker holes, E555_soft
- distilled_9 (seed 225, wide): 463. Starter corpora: lowB baseline (wide, top 10): 462; FixCorners23 (wide, top 10): **464** (r3c122, finish 461 -> probe 464); F1 reopen 480 s prior1/nogo1 +0; F2 ender deep +0. tools/probe.sh on finish ranks 121-180: 460 -> 462 max (default probe cut is right).
- User suggested experiments/soft/E555_soft.c and the backtracker's --order frontier on purpose-built holes.
- NEW tools/holemaker.py: mask = cells on broken junctions (8-connected clusters; --cluster, --focus r around one break cell), + donor cells holding pieces that fit a break cell on all its non-hole sides (--donors), + --extra random adjacent cells, --dilate. Checked: its scorer gives 465 on the frontier. tools/btdrive.py samples designs and runs `E555_backtracker --holes M --order frontier --break_mode any --breaks K` until a find or the wall.
- On the 465 (K=14): whole break set (57 cells, 15 breaks inside): k=9 not exhausted in 240 s. Clusters: 19 cells/12 breaks and 6 cells/3 breaks. Small cluster + donors + dilate (70 cells): PROOF no fill <= 14 (288 s). btdrive seeds 1-4: ~115 holes, ~100 PROOFS (up to 62 cells, most in < 1 s), cutoffs only on holes covering most of the big cluster. The 465 is locally optimal for every focused hole tried.
- btdrive on the four 464 elites (K=15, 120 s each): 44 proofs, 9 cutoffs, no find.
- E555_soft built in the lab (apt libnlopt-dev): --core 0 whole boards: 427 (88 s/start). 465 with top:6 lifted -> soft 16 starts: 449-453 -> reopen auto+1 55 s each: all 8 return to **the same 465** (agreement > 0.95). Given its bottom 10 rows, the 465's top is a strong attractor. top:8 -> soft (447) -> reopen auto+2/top:8 270 s: 458.
Decision: the 465 cannot be improved without changing rows <= 9. Next: bigger exact holes reaching into rows 8-10 (lds mode to find, not prove), and keep the distiller cycles for new bottoms.

## 07:35-09:10
- Big cluster (cluster 0) + dilate 1 + donors 6 (100 cells), lds lds_max 2, K=14: k=9 exhausted at 171 s, k=10 not. Exact search cannot reach the depth the 465 needs.
- Wide distiller cycles: seed 226 -> 463 x3; seed 227 -> 463 x5; seed 228 -> 463; seed 229 with BW=20000 -> 462 (no gain from width).
- btdrive K=16 on the five 463s of distilled_11 (90 s each): 51 proofs, 12 cutoffs, no find. Fresh 463s are locally optimal too.
- seed 230 -> distilled_14: 463 x3. 09:45: background loop tools/cycles.sh 15 18 (seeds 231-234), each step logged.
- Cycles 15-18 (seeds 231-234): 463, 462, 462, **464** (distilled_18). On that 464: btdrive K=15 470 s: 51 proofs, 6 cutoffs, no find; F1 reopen auto+1/auto+2/auto plateau 480 s: +0. 11:47: cycles 19-22 in background.
- Cycles 19-20 (seeds 235-236): 462, **464** (distilled_20).
- Cycles 21-22 (seeds 237-238): 463 x2 each. 13:30: cycles 23-27 in background.
