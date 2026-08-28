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
are, not just how many there are. And when a stage keeps stalling on the same
region, `python3 tools/E555_rotate.py FILE 1` turns every board a quarter-turn
so the next stage attacks it from a different side; the turn is lossless and the tool re-scores to prove it.

To validate the whole toolkit on your machine (including a regression against
a known solution): `bash tests/run_tests.sh`.

## The pipeline

```
 Stage A  border annealer      rotations.csv     (optional: --random_edges
    │      (Euler-trail SA)                       samples borders instead)
    ▼
 Stage B  beamer  ─────────►  partial boards, rows 1..12 filled
    │      (5-5-5 chain DB + wide beam + Mahalanobis heuristic)
    │
    │     finalizer ────────►  re-attack partials from a lower row
    │     roundhouse ───────►  rotate 90 deg and refill a border strip
    ▼
 Stage C  tail toolbox ─────►  finished boards, scored /480
           topper * backtracker * ender
```

All stages speak one CSV dialect (`config_id, score, pos[256], rot[256]`), so
any stage's output feeds the next, or itself, for iterative improvement.

## The tools, by importance

| tier | tool | one-liner |
|---|---|---|
| flagship | **`E555_beamer`** (C, OpenMP) | Wide beam over the 5-5-5 chain database; fail-fast sweep over border configurations; the **colour heuristic** is the project's main original contribution -- a closure log-likelihood over the remaining colour ledger (`--lambda_J`) corrected by a Mahalanobis piece-structure term (`--lambda_Mahalanobis`), which together roughly doubled the rate at which configurations survive the deep rows. |
| power tool | **`E555_finalizer`** (C, OpenMP) | Restarts the beam from any partial, locked below a chosen row, over a reduced database built in seconds -- deep re-sampling at trivial memory cost. |
| power tool | **`E555_roundhouse`** (C, OpenMP) | Turns the board 90 deg and grows a W-wide **strip** instead of a row, so each level is one chain lookup and the frontier is W colors wide. Small enough to solve the relaxed problem exactly: a backward dynamic program says which colorings can still finish the strip *before* any piece is tried, so a hopeless board is refuted in milliseconds. **Exhaustive and deterministic** -- finishing without a solution is a proof that none exists for that cut -- and it reports the furthest it got, one board per input. `--max_breaks B` then fills the rest greedily so Stage C gets a complete board rather than a hole. Uses only the edge half of the database (megabytes, seconds -- no 6.4 GB arena), and re-searches three sides of the border, which no other stage does. |
| power tool | **`E555_topper.py`** (CP-SAT) | Break minimizer with a lexicographic objective that herds breaks to the *nearest* corner (a 7+7 trip instead of 15+15); `--side` opens the top, bottom, left, right or an L-shaped pair of bands, so breaks stranded on any border can be attacked where they are, and `--holes` takes an explicit mask for a region no band can express -- a ragged patch, or the interior. The sliding-window workflow is the workhorse of late-game improvement. |
| strong, slower | **`E555_backtracker`** (C, OpenMP) | Two engines in one: greedy dives (`--break-mode stuck`, ~10k boards/s) to triage which partials are worth pursuing, and exhaustive DFS (`any`/`lds`) that *proves* no completion exists below a given break count. Thorough where CP-SAT is opportunistic. |
| power tool | **`E555_ender.py`** (CP-SAT) | The closer. Opens a slice of a full board and re-solves it under a move budget to drive breaks toward zero, never returning a worse board. `--mode patch` (default) repairs and compacts a local neighborhood; `--mode ring` re-threads the whole border ring so a break can heal by cascading around it. |

The endgame (turning a 46x/480 board into 480/480) is the open problem;
the Stage C tools are an active toolbox, not a finished recipe. If you crack
it, you know where to send the postcard.

## Repository layout

```
src/A_border/   Stage A border annealer (pure Python)
src/B_beam/     Stage B beamer + finalizer + roundhouse + shared database (C)
src/C_tail/     Stage C tail toolbox (C + Python/OR-Tools)
tools/          board viewer, ranker, rotator, CSV cleaner, rotations sorter
data/           seeds, example boards, masks (see data/README.md)
examples/       seven short scripts: one per tool, plus the whole chain end to end
pipeline/       the full pipeline, the whirlpool and the board farm, plus a
                Slurm wrapper: long unattended runs
tests/          run_tests.sh: the release gate
agent/          an experimental self-driving optimisation mode: untested and just for fun
```

## Requirements

- GCC or Clang with OpenMP, 64-bit POSIX (Linux, WSL2, macOS+libomp).
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
