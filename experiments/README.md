# Experiments

Ideas that were tried and measured, kept for reference. They are not part of the
toolchain: each folder builds with its own Makefile (`make ARCH=generic` inside
it), no check in `tests/run_tests.sh` covers them, and they may not keep up with
the current tools. Each folder's README.md has the details.

| folder | what it is | outcome |
|---|---|---|
| `datadriven/` | A fork of the Stage B beamer that learns where each piece tends to sit on an anchored board and adds that evidence to the beam's score. | Not merged into the beamer. One learned table (+0.24 nats a cell over chance) and the 282 row-11 boards found with it are in `example_run/`. |
| `soft/` | `E555_soft`: a board's free cells become (piece, spin) mixes, annealed with NLopt's L-BFGS until they settle into a real board. Needs NLopt. | On Stage B partials its boards come within 3.5 edges of the dives and are mostly different boards. Re-dived, they reach the dives' plateau but rarely pass it. |
