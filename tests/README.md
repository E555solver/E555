# tests/

Two unrelated things live here.

**`run_tests.sh`** is the release gate: build, every tool on the synthetic
fixture, a regression against a known 480/480 solution, and the static checks
(`check_script_flags.py`, `check_fixedframe.py`). Run it before you push.

Everything else is **the fixed-frame experiment**, described below. It is not
part of the pipeline, not built by `make all`, and nothing in `src/` depends on
it.

---

## The fixed-frame experiment

### The question

The beamer fills rows 0..11 fast and dies attempting row 12. Its partials solve
nothing — but do individual pieces nonetheless *prefer* a region of the board?
If a set of pieces reliably belongs in the top rows, the beam should not be
spending them low down, and `--exclude_pieces` can stop it.

### Why it needs its own beamer

The production beamer picks its own border, its own corners, and hedges across
all four clue orientations at once. Every run therefore lives in its own frame,
and boards from two runs cannot be compared, let alone pooled.
`E555_beamer_FixedFrame.c` pins the frame instead — one orientation, one corner
assignment — so partials from separate runs share a coordinate system.

One side only sees a 12-row band, so the frame is attacked from all four sides
and each side's boards are turned back onto the canonical frame. Four bands, one
frame, whole board covered.

### Running it

```bash
make beamer_fixedframe                      # not part of `make all`
bash tests/run_fixedframe_farm.sh WALL=900  # four sides, ~1 hour total
python3 tests/E555_frame_stats.py     ff_out/corpus.csv --out_dir ff_out/stats
python3 tests/E555_frame_dashboard.py ff_out/stats/summary.json --out board.html
```

The first run builds the 6.4 GB chain database (~80s) and caches it; the other
three sides mmap it in seconds.

### The files

| file | what it is |
|---|---|
| `E555_beamer_FixedFrame.c` | a copy of the Stage B beamer with the frame pinned. New flags: `--clue_orient` (required), `--canon_BL/BR/TL/TR`, `--exclude_pieces`, `--max_per_config`, `--emit_mode`. Deliberately a copy: the experiment must be free to diverge, and production must not move. |
| `run_fixedframe_farm.sh` | the four sides, then `tools/E555_rotate.py` on each, then one `corpus.csv`. Budgeted by wall clock per side, not by config count. |
| `E555_frame_stats.py` | ring affinity and top-band affinity per piece, weighted per border config. Writes `pieces.csv`, `agreement.csv`, `exclude_pieces.txt`, `summary.json`. |
| `E555_frame_dashboard.py` | `summary.json` → one self-contained HTML page. |
| `check_fixedframe.py` | proves the canonicalisation map, as arithmetic. Part of the gate. |

### The two things that keep it honest

Neither is a theory; both are cheap, and without them the numbers would be
decoration.

**Borders are the observations, not boards.** Every board a config emits
descends from one border, shared exactly. They are one observation with
variations. Each board is weighted `1/(boards from its config)` and the reported
**N_eff** (Kish) is the honest corpus size — far below the board count, which is
the point. So the beamer defaults to breadth: `--samples 0`, `--top_columns 1`,
`--beam_width 100000`, `--max_per_config 32`, and `--emit_mode sample` draws
uniformly from the survivors rather than taking the score-ordered head (the head
of a beam is its most correlated slice).

**The four sides are a built-in replication.** They use different RNG, different
borders and different search directions, so they share very little of the
heuristic's bias. If the per-piece ranking reproduces across them, something
about the puzzle is driving it; if it does not, the ranking is noise and no
further analysis will rescue it. That Spearman correlation is the headline
number, reported over all cells and over the 8×8 core (the only region all four
sides observe). The five pinned clue pieces are excluded from every ranking and
used instead as a positive control: the machinery must recover them at their own
cells.

### The canonicalisation map

Orientation `O` means "our row 0 is the published board turned `O` quarter-turns
clockwise", so a board searched at side `O` is put back on the canonical frame by
`(4 - O) % 4` **clockwise** turns.

| side | pin piece 3 at | canonicalise with |
|---|---|---|
| 0 | BL | `E555_rotate.py FILE 0` |
| 1 | TL | `E555_rotate.py FILE 3` |
| 2 | TR | `E555_rotate.py FILE 2` |
| 3 | BR | `E555_rotate.py FILE 1` |

After the turn, the centre clue (piece 138) sits at row 7, col 7, spin 0 in every
board of every side. `check_fixedframe.py` proves this from the clue table and
re-checks it against a corpus when given one.

Only the two *bottom* corners are ever placed — rows 12..15 stay empty — so the
TL/TR pins merely reserve their pieces. Each canonical corner is physically
placed in two of the four sides.

### The corner bet

The clue set is published and fixed. The corner assignment is **not**: no clue
pins a corner, so the four corner pieces can fill the four corners 4! = 24 ways
and only one of them is the solution's. The default (`BL=3 BR=2 TL=0 TR=1`) is
one of those 24, chosen arbitrarily and then held fixed. It conditions everything
near the border heavily and the core barely — which is where the interesting
variation is anyway. Change all four together with the `--canon_*` flags.

### What settles it

Not the statistics. The exclusion list is specific to *this* frame, so test it
with this beamer, one row deeper, with and without:

```bash
bin/E555_beamer_FixedFrame data/seed_Edge5.txt --clue_orient 0 --stop_row 12 \
    --db_file excl.db --wall_time 1800 \
    --exclude_pieces $(cat ff_out/stats/exclude_pieces.txt)
```

Count `filled=12` lines in each log. That number decides whether any of this was
worth doing.

### A cheaper corpus

Four sides each filling `R` rows cover the whole board as soon as `2R >= 16`,
i.e. `STOP_ROW >= 7`. Rows 8..11 are where the beam is most squeezed, so
`STOP_ROW=7` yields orders of magnitude more boards from more borders while still
covering every cell — shallower and less selected material. Running the farm at 7
and at 11 and comparing shows which conclusions depend on the depth.
