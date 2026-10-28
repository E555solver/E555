# Journal, whirlpool campaign 20261028-0249

# Journal, campaign 20261028-0249

## 2026-10-28 02:50 UTC bootstrap
- /goal text held two copies of the launch line, BUDGET=8h and BUDGET=12h; running with 12h (the edited value). Other settings default.
- Machine: Intel Xeon @ 2.10GHz, 4 threads, 15 GB, arch v3. stage_b_ok=true.
- Wake-up task trig_01V9B7FYbZdtwh6ctsBiJCEr (hourly at :50).
- First checkpoint a12a62f on agent_runs.

## 03:30 baseline v00 (run r20261028-025542, 30.7 min)
- funnel lap1 30/31, lap2 7, lap3 2; frame_held 0 (incomplete:s2 always); 0 jewels. Produce: 16087 boards in 53 s, all one border config.
- example_candidate not run in the harness: v00 already shows the same frame=incomplete:s2 failure, and the kit documents it; the 20 min go to v02.

## 03:45 diagnosis (manual, timed.sh)
- v01 (keep_ring every lap, clockwise only, depth-first, lap 2+ band rows 0..5 with kept block cols 1..5): lap 1 with --keep_ring reaches row 10 (2 boards of 4 bands, 10 s), frame=held. Lap 2: bands found easily (2000 in <20 s once the slab margin is blanked), but no band re-grows rows 6..10 between the two held ring sides: 0 of 16 bands, even exhaustively (--backtrack_row 5, 582k nodes). One locked root, both row ends fixed: the tree dies at row 6-7.
- Lower lock on laps 2-3: band rows 0..3, kept block cols 1..4 (12 interior cells), finalizer width 20000: 1 of 2 bands reached row 10 (34 boards). DB build 6.7 s + sort 16.6 s per band.
- v02 = v01 with that lower lock on laps 2-3. Test on one board: lap1 10 s, lap2 36 s, lap3 62 s -> a jewel (depth 10, kept 54,14,12) in 62 s. Not counted (manual test).

## 04:10 v02 (run r20261028-033346, 32.1 min): 12 jewels, 22.4/h, funnel lap1 16/55, lap2 14, lap3 12, frame held 16
- Laps 2-3 succeed almost whenever lap 1 does. Lap 1 is the filter: 26 of 55 boards had no lap-1 band at all (1 s), 7 had bands none of which re-grew (35 s).
- First 4 jewels came while the health check ran; nothing restored.
- Next: v03, every lap-1 band (60/60 distinct rings in one band file) is a new frame; try them one at a time, keep going after a jewel.

## 04:45 v03 (run r20261028-040924, 31.7 min): 8 jewels, 15.2/h, funnel lap1 9/16, lap2 7, lap3 6
- Worse than v02 (22.4/h; not significant, 8 vs 12 < 2*sqrt(20)), and the reason is clear in attempts.csv: boards run to the 180 s cap; the first jewel lands at ~70 s as in v02, a second ring costs ~100 s more (failing lap-1 bands cost 9 s each). Moving to a new board is cheaper. v02 stays best.
- Experiment: lap-2 band row 3/4/5 (kept cols 4/3/2) on 10 lap-1 boards from jewel chains (biased towards row 3, which grew them).

## 05:00 lap-2 cut sweep (10 lap-1 boards from jewel chains, 2 bands each, width 20000)
| band_row:keep_cols | re-grew | mean s | s per success |
|---|---|---|---|
| 3:4 (v02) | 10/10 | 36.2 | 36.2 |
| 4:3 | 8/10 | 27.5 | 34.4 |
| 5:2 | 6/10 | 13.2 | 22.0 |
- So the kept-block width, not the band row, was v01's problem (5:5 gave 0/16). DB build+sort: 24 s at row 3, 14 s at row 4, 8 s at row 5.
- v04 = v02 with laps 2-3 trying 5:2 first, then 3:4. Test board: jewel at 63 s.

## 06:00 v04 (run r20261028-045604, 61.6 min): 21 jewels, 20.45/h, funnel lap1 26/93, lap2 23, lap3 21
- Not better than v02 (22.4/h over 32 min). Jewel attempts took ~100 s (v02 ~70 s): 5:2 fails more often on unselected lap-1 boards than on the 10 boards of the sweep (which came from jewel chains grown with 3:4), and a failed 5:2 adds ~17 s.
- Time split: 43 attempts end in ~1 s (no lap-1 band), 25 run ~36 s and get no lap-1 board (~900 s, 25% of the run), 24 run 50-180 s.
- Next: lap 1 keeps the 10-column wedge that killed v01's lap 2. Sweep the lap-1 cut on 16 fresh beamer boards.

## 06:20 lap-1 cut sweep (16 fresh beamer boards from the v00 produce test, 1 lap, keep_ring, width 20000)
| band_row:keep_cols | re-grew | mean s |
|---|---|---|
| 5:full slab (v02 lap 1, 4 bands) | 6/16 | 27 |
| 3:4 (2 bands) | 14/16 | 37 |
| 4:3 (2 bands) | 9/16 | 23 |
- v05 = v02 with lap 1 cut 3:4 too. Test board f1: jewel at 102 s.

## 07:30 v05 (run r20261028-062332, 61.6 min): 26 jewels, 25.3/h, funnel lap1 37/68, lap2 37, lap3 37 -> new best
- Lap-1 cut 3:4 removed the lap-1 failures: 29 attempts end in ~1 s (no band at all), 2 fail later, 37 go all three laps.
- But only 26 of the 37 completed chains had a new frame. The repeats are neighbouring attempts (27/28/23, 45/46/42, ...): consecutive beamer boards differ in a few cells and the lap-1 cut finds the same ring.
- v06 = v05 + produce keeps one board per frame-cell signature, shuffled (1868 of 3079 boards in one call).
- First dive round started: 67 new-frame jewels, 201 lap boards (s2-s4), diver 10000/20000, 900 s.
- Dive round 1: 201 boards dived in 34 s, best 458; reopen top 8 (13 s): 459. best_boards.csv: REFEREE VALID 459 (r20261028-062332-a54-t4_s2).

## 08:30 v06 (run r20261028-072750, 61.2 min): 31 jewels, 30.4/h, funnel lap1 33/53, lap2 32, lap3 31 -> new best
- 31 of 31 completed chains new frames (v05: 26 of 37). 31 vs 26 is not significant (needs > 2*sqrt(57) = 15), but the repeats are gone, as designed.
- Time: 15 attempts ~1 s (no band), 5 at 15-53 s (no lap 1), 33 at 77-177 s. A jewel costs ~95 s; next: profile one.

## 09:00 profile and patch
- Profile of one v06 jewel (114 s): the finalizer is all of it; per band DB build ~8 s + fan-out sort ~17 s at band row 3.
- Patch patches/src/B_beam/E555_finalizer.c: new flag --no_fanout_sort (off by default; without it the code path is unchanged) skips sort_db_by_fanout() after a rebuild. Usage text, print_cmd, parser updated.
- Lab checks: `ARCH=generic bash tests/run_tests.sh finalizer` 13/13 passed. With ARCH=v3 the compile check fails on 15 -Wmaybe-uninitialized warnings in E555_beamer.c (untouched by the patch; pre-existing with this compiler at -march v3).
- Lap 1 cut 3:4 on the 16 fresh boards: with --no_fanout_sort 14/16 re-grew (the same two fail) in 13.6 s mean, vs 37 s with the sort.
- v07 = v06 + --no_fanout_sort. Test board: jewel at 29 s (v06: 114 s on the same board).
