# Whirlpool campaign 20261028-0249: 616 jewels, 87 per hour from a runner that started at zero

## Result

| | baseline (v00) | best (v08_cheapcut) |
|---|---|---|
| jewels per hour | 0.0 | **87.4** (444 jewels in 5.1 h of standard runs) |
| funnel, laps 1/2/3 per attempt | 30/7/2 of 31 | 481/449/445 of 745 (all v08 runs) |
| frame held | 0 of 31 | 481 of 745 |
| depth | - | 10 (every jewel) |
| minutes measured | 30.7 | 304.7 (five runs) |

- **Jewels found:** 616 distinct frames (`frames_seen.txt`), all of depth 10. Each is a beamer board plus three laps grown clockwise from the three other sides, all four holding the same complete 60-cell frame. Every one is in `whirlpools/*.csv.gz`, listed in `whirlpool_index.csv` (`jewel=yes`).
- **Best dived board:** **461/480**, `REFEREE verdict=VALID` (`best_boards.csv`, from jewel `r20261028-072750-a47-t4`). Not a record: the repository's best is 465.
- v08's five standard runs (`harness.csv`): 82.9, 95.9, 93.9, 77.7 and 86.9 jewels/h (runs r20261028-095414, -105623, -115642, -125940, -140011).

## How to run it

```bash
cp agent/whirlpool/20261028-0249/patches/src/B_beam/E555_finalizer.c src/B_beam/   # the one patch
make
bash agent/whirlpool/20261028-0249/run_pipeline_whirlpool.sh DB_FILE=E555.db WALL=3600 RUN_DIR=w1
bash agent/whirlpool/20261028-0249/run_pipeline_whirlpool.sh DB_FILE=E555.db WALL=3600 RUN_DIR=w1 DIVE=1
```

- `MODE=all` (default) loops produce -> whirl for `WALL` seconds, each beamer board capped at `CAP` (180) s. It writes `RUN_DIR/chains.csv` (every chain), `jewels.csv` (the jewels' four boards, judged by `agent/whirlpool/judge.py`), `jewels.log` (one CHAIN line per jewel) and, with `DIVE=1`, `dived.csv`.
- `DB_FILE` matters: the first call writes the 6.4 GB chain database (about 2.5 min), every later produce call loads it in seconds. The beamer needs about 8 GB of RAM.
- `MODE=produce` / `MODE=whirl` keep the whirlpool kit's candidate contract, so `agent/whirlpool/harness.sh` can still score it.
- Smoke test (`MODE=all WALL=600 DIVE=1` in the lab, 4 cores): one produce call (3145 boards, 9 s), 21 boards whirled, 15 jewels in 628 s (about 86 per hour), dived to a best of 458 (VALID). Its last step called `E555_rank.py --rescore` without `--out`; that line was fixed afterwards and checked on the smoke run's `dived.csv` (`best.csv`, VALID 458), not re-run end to end.

## What changed

Each change is a candidate in `candidates/`; each header says what it changes.

**One turn direction, a ring held from lap 1 (v01, v02).** The old runner turned every board both ways each lap, so a lineage could return to its starting side, and its finalizer re-chose the right column each lap, so the frame changed from lap to lap. Every baseline chain broke its frame at s2 (`frame=incomplete:s2`, 31 of 31). The new runner turns clockwise every lap, goes depth-first (a lap's first child goes straight into the next lap), and runs every finalizer with `--keep_ring`. The ring the lap-1 band cut completes becomes the board's fixed border, and every lap writes it out whole. Frame held: 0 of 31 (v00) became 16 of 16 completed lap-1 boards (v02).

**Room for the band cut: keep a small block, not the whole slab (v02, v05).** After a turn, the old lap kept the parent's whole six-row slab and asked the band cut to fill a 4-cell-wide wedge between the slab and the fixed frame. On lap 2 that wedge is usually unfillable, and when it is filled, the beam above it dies at row 6 or 7 between the two fixed ring sides. Exhaustively: 0 of 16 bands re-grew. The new lap blanks the slab except a small kept block (rows 1..3, columns 1..4: 12 interior cells, enough to count as a re-grow), cuts rows 0..3 exactly, and lets the finalizer's beam grow rows 4..10 from that wider start.
- Measured on 10 lap-1 boards: cut 3:4 re-grew 10 of 10, cut 5:5 0 of 16.
- v02 used it on laps 2-3 only: 22.4 jewels/h (r20261028-033346, 32 min).
- v05 used it on lap 1 too, where it raised lap-1 success on 16 fresh boards from 6 to 14: 25.3/h (r20261028-062332, 62 min). Funnel 37/37/37: every board through lap 1 finished all three laps.

**Spread the queue over frames (v06).** In v05, 11 of 37 completed chains repeated a frame found minutes earlier. Consecutive beamer boards differ in a few interior cells, and the lap-1 cut then finds the same ring. Produce now keeps one board per frame-cell signature and shuffles the queue. Result: 31 of 31 completed chains were new frames, 30.4/h (r20261028-072750). 31 jewels against v05's 26 is not significant by itself, but the repeats are gone.

**Skip the fan-out sort (v07, patch).** A lap was one finalizer call per band, and 25 of its ~27 s were spent rebuilding the reduced chain database: 8 s to build it and 17 s to sort each cell by fan-out. That order only steers which records a parent tries first. The new flag `--no_fanout_sort` skips the sort.
- Lap 1 on the same 16 fresh boards: the same 14 re-grew, in 13.6 s instead of 37 s.
- v07: **73.4/h** (r20261028-084040) against v06's 30.4: 74 vs 31 jewels, above the 2·sqrt(105) ≈ 20 threshold, so significant.

**A cheaper cut first on laps 2-3 (v08).** Without the sort, cut 4:3 (rows 0..4, 3 kept columns) rebuilds a smaller database. On 10 lap-1 boards it re-grew 7 of 10 in 7.8 s, against 9 of 10 in 13.2 s for 3:4. Laps 2 and 3 try 4:3 first, then 3:4; lap 1 stays 3:4 (4:3 re-grew only 8 of 16 there).
- First hour: 82.9/h (r20261028-095414), against v07's 73.4. That's 83 vs 74 jewels, under the 25-jewel threshold, so not significant on its own.
- Five hours: 87.4/h. It became best by the harness rule and was used for production.

## Code changes

- `patches/src/B_beam/E555_finalizer.c` (patch hash c3b1573d): new flag **`--no_fanout_sort`**, off by default. It skips `sort_db_by_fanout()` after each rebuild of the inner database; the fan-out table itself, used for pruning, is still built. Without the flag the code path is unchanged. Added in all four places a C flag lives except `PROJECT_E555.md` (the kit forbids editing it): the parser, the usage text, `print_cmd()`, and a comment at the declaration. Upstream, the finalizer table in PROJECT_E555.md §6.10 needs the row.
- Checks: `ARCH=generic bash tests/run_tests.sh finalizer` in the lab: 13 of 13 passed (`finalizer_rotations`, `finalizer_determinism`, `finalizer_top_lock`, `whirlpool_lap`, `band_with_frame`, ...). With `ARCH=v3` the compile check fails on 15 `-Wmaybe-uninitialized` warnings in `E555_beamer.c`, which the patch does not touch: a compiler/arch matter on this machine, not the patch.
- No new gate check was added (the kit forbids editing `tests/`); a check that runs one lap with and without the flag would be the natural one.

## What did not work

- **Keeping the 5-column or full slab on laps 2-3 (v01):** lap 2 found bands, but 0 of 16 re-grew rows 6..10, even exhaustively (`--backtrack_row 5`, 582k nodes). Not run in the harness.
- **Many rings per board (v03, r20261028-040924):** every lap-1 band carries its own ring (60 of 60 distinct), so v03 tried each in turn after a jewel. 15.2/h against v02's 22.4: the first ring costs ~70 s, each further one ~100 s, so moving to a new board is cheaper.
- **The 5:2 cut (v04, r20261028-045604):** cheap (13 s) but it re-grew 6 of 10 even on favourable boards, and fewer in practice. As a first try before 3:4 it slowed jewels from ~70 s to ~100 s: 20.5/h, no better than v02. Without the sort it re-grew only 1 of 16 lap-1 and 2 of 10 lap-2 boards.
- **Cut 4:3 on lap 1:** 9 of 16 (with the sort) and 8 of 16 (without) against 14 of 16 for 3:4.
- **The baseline's population carry (v00, r20261028-025542):** funnel 30/7/2 of 31 attempts, frame never held: 0 jewels.
- `agent/whirlpool/example_candidate.sh` was not run in the harness: it is documented to fail exactly as v00 did (`incomplete:s2`), and its 20 minutes went to v02 instead.
- **Diving deeper:** four rounds of `E555_diver` (`--end_dive 10000 --end_polish 20000`) plus `--reopen auto --copies 8` on the top 16 gave 459, 461, 461, 461. More jewels did not raise the best: lap boards of depth 10 under a fixed frame dive to 455-461.

## Jewels and boards

- 616 jewels with distinct frames, all of depth 10. `whirlpool_index.csv` lists 695 archived chains: 629 jewel chains (13 of them repeat a frame, nearly all in v05) and 66 baseline chains that completed three laps without a fixed frame.
- Dived: the s2..s4 boards of the first 445 jewels (rounds 1-4, 1335 boards). The 171 jewels of the last two production runs (r20261028-125940, r20261028-140011) were not dived: the budget ran out.
- `best_boards.csv`: the 20 best complete boards; the best is 461/480 (`bash agent/referee.sh best_boards.csv`: VALID, 19 breaks, no frame violations). No record.
- Depth 11 was never tried.

## Machine

- **CPU:** a cloud VM with 4 vCPUs (4 cores × 1 thread, 1 socket) of an "Intel(R) Xeon(R) Processor @ 2.10GHz". The hypervisor masks the model name.
  - It has AVX-512 (FP16, BF16, VNNI) and AMX, so it is Sapphire Rapids generation or newer; the 260 MiB L3 it reports points to Emerald or Granite Rapids.
  - L2 2 MiB per core, 15 GB RAM. Binaries built for x86-64-v3.
  - Same CPU for the whole campaign: `cpu_changed=no` at every hourly health check.
- **Extrapolating to a bigger workstation:** a jewel costs about 3 finalizer calls, and each is almost entirely the reduced-database build. That build takes ~8 s at band row 3 and ~4 s at band row 4 on these 4 cores (OpenMP). Band cuts and scripts add ~1 s per lap, and a produce call is ~10 s per few thousand boards, so produce is under 1% of the time.
  - Jewels per hour should therefore follow how the database build scales with cores and memory bandwidth. Measured at 4 threads only: speedup beyond that is a projection, not a measurement.
  - The beamer needs ~8 GB of RAM for its chain database; the finalizer's reduced database is much smaller.
- **Restores:** none. **Usage-limit pauses:** none. **Checkpoints:** 18 pushed to `agent_runs`, the last one carrying this README.
- The `/goal` line carried two copies of the settings, with BUDGET=8h and BUDGET=12h; the campaign ran 12h, which the user confirmed.
