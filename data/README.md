# E555 data files

Files are grouped by role: **seeds** (piece sets), **boards** (canonical board
CSVs), **borders** (Stage A output), and **holes** (movable-cell masks).

| file | what it is |
|---|---|
| **seeds** | |
| `seed_Edge5.txt` | The canonical 256-piece seed: one line per piece, four edge colors `top right bottom left` (0 = frame gray, 1-5 = frame-interface colors, 6-22 = inner colors). Pieces 0-59 are the 4 corners + 56 edges. Everything real runs on this seed. |
| `synth_seed.txt` | A **synthetic, solvable-by-construction** piece set in the same format. Exists so the pipeline can be tested against a known answer. |
| **boards** | |
| `board_example_462.csv` | A real 462/480 board from a long Stage B + Stage C campaign -- the standard guinea pig for the tail tools and the viewer. |
| `best_463.csv` | The **7 best full boards found so far**, all 463/480, as a gallery. `python3 tools/E555_rank.py data/best_463.csv` sorts them by compactness. |
| `best_465.csv` | **465/480**, the best board so far: the first board of `best_463.csv` with rows 12-15 re-dived (`E555_ender.py` redive; `E555_diver --end_dive 50000 --end_polish 50000`). 28 cells differ from the 463, all in rows 12-15. |
| `board_partial_row12.csv` | A genuine **Stage B beam output** on `seed_Edge5` (id `r0c669`, from the low-B Mahalanobis campaign): rows 0-12 filled, rows 13-15 still open, the bottom 12 rows already clean. The realistic input for the Stage C tools, which expect a partial. Field 2 here is the beamer's solution **index**, not the edge score (see the convention below). |
| `E565_lowB_baseline.csv` | 255 Stage B partials grown to row 12 (208 placed cells), some carrying clue pieces: the default corpus of `examples/08_distiller_quickstart.sh` and the distiller's gate check. |
| `E565_FixCorners23.csv.gz` | 64,324 Stage B partials grown to row 11 (192 placed cells; 30,565 unique): the corpus the distiller's screen was calibrated on. |
| `synth_solution_480.csv` | The known 480/480 solution of the synthetic set, as a canonical board row. Used by `tests/run_tests.sh` as the finalizer regression: the finalizer must rediscover it from a partial locked at row 10. |
| **borders** (Stage A output, feed to the beamer as its rotations CSV) | |
| `borders_annealed_fix12.csv` | A best-found Stage A annealer run with fixed corners (modes 1 and 2): border arrangements (`id, 256 spins`) with `#` comments carrying the per-side Euler-trail scores. The kind of file `bash examples/01_beamer_quickstart.sh ANNEAL=1` produces and the beamer reads. |
| **holes** (16x16 0/1 masks; `1` marks cells `--holes` may reopen; `#` comments allowed; first data line is row 0, the bottom) | |
| `holes_open_border_TBLR.csv` | Opens the whole border (Top, Bottom, Left, Right) and its adjacent ring. |
| `holes_open_border_TRL.csv` | Opens the Top, Right and Left borders plus adjacent corners. |
| `holes_open_border_TR.csv` | Opens the Top and Right borders plus adjacent corners. |

## Board CSV convention (used across the whole toolkit)

```
config_id , score , pos[0..255] , rot[0..255]        (514 fields)
```

- `pos[p] = row*16 + col` of piece `p`; row 0 is the **bottom**, row 15 the
  top; `999` = unplaced.
- `rot[p]` is 0..3 counter-clockwise quarter-turns.
- `score` = matched internal edges (0..480). Stage B writes its solution
  index in this slot instead; every reader takes the last 512 fields as
  `pos`+`rot` and treats the leading fields as metadata, so both variants --
  and the older 515-field layout -- load everywhere.
- Lines starting with `#` (or `%`) are comments.

`python3 tools/E555_rank.py FILE --out OUT.csv --rescore` rewrites any legacy
file into this canonical form, recomputing the score from the seed.
