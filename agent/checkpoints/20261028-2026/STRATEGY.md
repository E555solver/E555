# Strategy ledger

Measured before this campaign (PROJECT_E555.md 9.2, 9.3); re-test only with a new idea:
- CP-SAT windows around the breaks of polished boards (457-463): already optimal
  (78/78 4x4 windows); exact regions of 48-102 cells gained nothing.
- diver --reopen auto on the seven first 463s of data/best_463.csv, 120 s each:
  the first gained 2 (that board is also the root of data/best_465.csv), the
  other six gained nothing.
- Best measured reopen setting: --copies 8 --end_dive 1000 --end_polish 10000;
  it kept gaining from 15 s to 120 s per board.

## Families tried in this campaign
- F1 reopen on the 465 (auto+1 480 s) and on a 464 (alternating auto/auto+1/top:5, plateau, 480 s, 1194 rounds): +0. Elite reopen exhausted at this budget.
- F2 ender deep 420 s on the 465 and a fresh 463: +0.
- F5 rebuild top:6 SR=11 on elites: loses 2-7 edges. Exhausted.
- F8 finalizer on the 465: from 7 (1 completion), from 5 free_sides (465 max), from 3 (457 max). Exhausted for the 465.
- F4 annealed borders (tb6 tc12): worse than random edges (461 max). F4 BR=5 with ED=10000: too many boards, nothing written in 480 s.
- F4 random edges, BR=6 SR=11, ED=10000 EP=20000: raw bests 461-463 per 480 s run; Maha10 raises tail density.
- Elite reopen, 240 s/board with auto/auto+1 (elites 3-14) or auto+1/auto+2 (15-18): +0 everywhere except the 465's own root (rediscovers it). Exhausted.
- Best pipeline so far: F4 partials (samples4 tc4 xn0 Maha10, 300 s) + F3 distiller top 10 -> 464.
- F6 backtracker on designed holes (tools/holemaker.py + btdrive.py), 465 K=14, 464s K=15: ~150 holes, every finished one a proof; local optimum up to 62-cell holes.
- F7 soft (experiments/soft, lab build): top:6 refill + reopen returns to the identical 465; top:8 refill + reopen 458; core 0 427. Not a source of better boards here.
