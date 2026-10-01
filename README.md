# E555: Eternity II solver pipeline with precomputed 5-piece sequences

E555 is a pipeline of solvers for [Eternity II](https://en.wikipedia.org/wiki/Eternity_II_puzzle),
the famously unsolved 16x16 edge-matching puzzle: 256 square pieces, 22 edge
colors, 480 interior edges to match. Since 2007 the best public board reaches 470/480 edges.

The toolkit grows boards **bottom-up with a wide beam search over a
precomputed r-piece chain database** (Stage B, the heart of the project), then attacks
the remaining top rows with a family of tail solvers (Stage C). A border
annealer (Stage A) prepares good boundary arrangements, although the beamer's
`--random_edges` mode needs no Stage A at all, which makes the first run easy.

This algorithm has been carefully designed by an amateur puzzle-solver human, with 
extensive coding assistance mostly from Claude Code.
All the tools can use multi-core processing and fit on a regular laptop: the chain database 
of all 3.1 billion legal 5-piece row chains packs into **6.4 GB of RAM**.

## Quick start (~5 minutes + compile)

```bash
make                                          # gcc + OpenMP, four binaries
bash examples/01_beamer_quickstart.sh  # borders sampled from the seed
python3 tools/E555_viewer.py beam_out/beam_completions_random_10.csv
```

The viewer prints the board as ASCII (with `#` marking mismatched junctions)
and a ready-to-open [e2.bucas.name](https://e2.bucas.name) URL. To compare many
boards instead of one, `python3 tools/E555_rank.py *.csv` ranks them by how *compact* their breaks
are, not just how many there are. When a corpus grows past what you can hand to CP-SAT,
`python3 tools/E555_distiller.py boards.csv.gz --top 25` screens every board with
seeded dives, finishes and probes the best, and writes the 25 worth the time together
with the ender command for them. And when a stage keeps stalling on the same
region, `python3 tools/E555_rotate.py FILE 1` turns every board a quarter-turn
so the next stage attacks it from a different side; the turn is lossless and the tool re-scores to prove it.
Adding `--sink N` goes further and drops the board N rows, so the bad rows fall out of it
altogether and a fresh top comes free.

To validate the whole toolkit on your machine (including a regression against
a known solution): `bash tests/run_tests.sh --all`; without `--all` it runs
only the fast core checks.

## The pipeline

```
 Stage A  border annealer      rotations.csv     (optional: --random_edges
    │      (Euler-trail SA)                       samples borders instead)
    ▼
 Stage B  beamer  ─────────►  partial boards, rows 1..12 filled
    │      (5-5-5 chain DB + wide beam + Mahalanobis heuristic;
    │       optional exhaustive tail, and end dives that finish every
    │       board to 256 pieces, scored /480)
    │
    │     finalizer ────────►  re-attack partials from a lower row
    │     roundhouse ───────►  rotate 90 deg and refill a border strip
    ▼
 Stage C  tail toolbox ─────►  finished boards, scored /480
           topper * backtracker * diver * ender
```

All stages speak one CSV dialect (`config_id, score, pos[256], rot[256]`), so
any stage's output feeds the next, or itself, for iterative improvement.

## The tools, by importance

| tier | tool | one-liner |
|---|---|---|
| flagship | **`E555_beamer`** (C, OpenMP) | Wide beam over the 5-5-5 chain database; fail-fast sweep over border configurations; the **colour heuristic** is the project's main original contribution -- a closure log-likelihood over the remaining colour ledger (`--lambda_J`) corrected by a Mahalanobis piece-structure term (`--lambda_Mahalanobis`), which together roughly doubled the rate at which configurations survive the deep rows. `--lambda_corners` adds an exact top-corner supply term: it keeps legal fillings of the corner blocks (around the row-13 clues under `--clue_corners`) buildable for Stage C. `--backtrack_row N` hands the last rows to an **exhaustive search** from every row-N board, and `--end_dive` **finishes every stop-row board** to all 256 pieces -- stuck-mode dives that learn from their own best completions, then `--end_polish` swap-and-rotate local search -- writing complete boards scored /480. Both keep every core busy, however few boards a configuration sends. |
| power tool | **`E555_finalizer`** (C, OpenMP) | Restarts the beam from any partial, locked below a chosen row, over a reduced database built in seconds -- deep re-sampling at trivial memory cost. A complete, clean border locks its top border and any clean rows under it (a double-decker witness keeps rows 14-15; `--keep_ring` pins the whole ring). Takes the beamer's `--lambda_corners`, `--backtrack_row` (from the lock itself, with no beam at all) and `--end_dive`. |
| power tool | **`E555_roundhouse`** (C, OpenMP) | Turns the board 90 deg and grows a W-wide **strip** instead of a row, so each level is one chain lookup and the frontier is W colors wide. Small enough to solve the relaxed problem exactly: a backward dynamic program says which colorings can still finish the strip *before* any piece is tried, so a hopeless board is refuted in milliseconds. **Exhaustive and deterministic** -- finishing without a solution is a proof that none exists for that cut -- and it reports the furthest it got, one board per input. `--breaks B` then fills the rest greedily so Stage C gets a complete board rather than a hole. Uses only the edge half of the database (megabytes, seconds -- no 6.4 GB arena), and re-searches three sides of the border, which no other stage does. |
| power tool | **`E555_topper.py`** (CP-SAT) | Break minimizer with a lexicographic objective that herds breaks to the *nearest* corner (a 7+7 trip instead of 15+15); `--side` opens the top, bottom, left, right or an L-shaped pair of bands, so breaks stranded on any border can be attacked where they are, and `--holes` takes an explicit mask for a region no band can express -- a ragged patch, or the interior. The sliding-window workflow is the workhorse of late-game improvement. |
| strong, slower | **`E555_backtracker`** (C, OpenMP) | Two engines in one: greedy dives (`--break_mode stuck`, ~10k boards/s) to triage which partials are worth pursuing, and exhaustive DFS (`any`/`lds`) that *proves* no completion exists below a given break count. Thorough where CP-SAT is opportunistic. |
| finisher | **`E555_diver`** (C, OpenMP) | The beamer's end-dive engine on any board file: fills the open cells with learning dives and polishes the winners, placed cells fixed. `--reopen` improves complete boards: it lifts the damaged band and re-dives it in `--copies` and `--rounds`, never writing a worse board. No database; output independent of the thread count. |
| triage | **`E555_distiller.py`** (Python) | Distils a corpus of any size (plain or gzip; partial or complete boards) to the N worth the CP-SAT tail: drops exact repeats, screens every partial with 300 seeded dives, finishes the best half (at most 400 N), probes the best 4 N with the ender's redive, keeps N with distinct foundations, and writes the `E555_ender.py` command for them. Reproducible and resumable; options `--top`, `--out`, `--seed_file` only. |
| triage | **`E555_extract_consensus.py`** (Python) | Ranks a large pool of CLUED partials by agreement with the pool itself. The five clues are one rigid body in four configurations, so a clued board's orientation can be read and every board turned into one frame; the piece-by-cell frequency table that follows is the *consensus*, and a board is scored in bits above chance against the whole frequency column, not just its mode. Scoring runs on the cells the whole corpus placed, so a board with a fuller top row is neither rewarded nor punished for it -- per-board cells would rank the pool largely by stop row. `--best_top`/`--best_bottom` ask the other question -- never mind where this board's pieces are, is the bag it has LEFT the right bag for the rows it has left -- and `--border_out` distils the same table into Stage A rotations rows the beamer reads, repaired with the annealer's Euler machinery so every side stays rich in trails -- plus a companion `_frame.csv` with those 60 pieces actually laid out, ordered by the maximum-weight Euler trail per side, which `E555_finalizer --finalize_from 0` locks as fixed sides and grows from row 1. Because the corners fix each side's Euler endpoints, it emits **one border per corner class** rather than averaging incompatible frames together; `--BL/--BR/--TR/--TL` narrow that, and the corner histogram is printed on every run. |
| power tool | **`E555_ender.py`** (CP-SAT) | The closer for complete boards: exact re-solves of regions (whole-board exchange cycles, windows, corners with their frame arms, the damaged band, the frame) plus the diver's redive of the damaged band, cheapest first, never a worse board. Pick a `--profile` and a `--board_time_limit`. |

The endgame (turning a 46x/480 board into 480/480) is the open problem;
the Stage C tools are an active toolbox, not a finished recipe. If you crack
it, you know where to send the postcard.

## Repository layout

```
src/A_border/   Stage A border annealer (pure Python)
src/B_beam/     Stage B beamer + finalizer + roundhouse + shared database (C)
src/C_tail/     Stage C tail toolbox (C + Python/OR-Tools)
tools/          board viewer, ranker, distiller, rotator, CSV cleaner, rotations sorter
data/           seeds, example boards, masks (see data/README.md)
examples/       thirteen short scripts (01-11): one per tool or task, plus the whole chain
pipeline/       the full pipeline, the whirlpool and the board farm, plus a
                Slurm wrapper: long unattended runs
tests/          run_tests.sh: the core checks, or the release gate with --all
agent/          an experimental self-driving optimisation mode: untested and just for fun
```

## Requirements

- GCC or Clang with OpenMP, 64-bit POSIX (Linux, WSL2, macOS+libomp).
- `make` targets the build host (`-march=native`). `make ARCH=v3` for a mixed
  x86-64 cluster, `ARCH=generic` for CI and containers; see the Makefile header.
- ~8 GB RAM for Stage B (6.4 GB database + workspace); everything else is tiny.
- Python >= 3.9; `pip install ortools` for the two CP-SAT tail tools only.

## Reading on

**[PROJECT_E555.md](PROJECT_E555.md)** is the technical document: the 5-5-5
decomposition, the database layout, scoring mathematics (including the exact
Mahalanobis formulation), parity pruning, every CLI flag, and the trade-offs
between search strategies.

## License

MIT -- (c) 2026 AB. If E555 helps you push past 470, please share your boards
with the community.
