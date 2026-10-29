
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
