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
