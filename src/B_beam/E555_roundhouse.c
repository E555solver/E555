/*
 * E555_roundhouse.c -- Stage B: exhaustive strip solver over a width-W chain
 *                      database, with an optional greedy break-filler.
 *
 * WHAT IT DOES
 *   Takes a board, rotates it, frees one to three W-wide bands along its
 *   borders, and refills them from the chain database one W-cell chain at a
 *   time. The beamer grows a board one ROW at a time, so its frontier is 16
 *   colors wide; this grows a W-wide vertical STRIP, so the frontier is W
 *   colors wide. Everything below follows from that one change:
 *     - each strip level is ONE database lookup -- (W-1) inner pieces plus a
 *       frame-right edge terminal, i.e. a segment-C record, shortened;
 *     - only edge-terminal chains are needed, so the database is megabytes and
 *       builds in seconds (never the beamer's 6.4 GB inner arena);
 *     - the relaxed problem (same colors, piece reuse allowed) has only
 *       17^(W-1)*5 states per level, so one backward sweep solves it EXACTLY
 *       and refutes dead branches before a single piece is tried;
 *     - the strip re-chooses its own border pieces as it goes, so a run
 *       re-searches up to three sides of the frame.
 *
 * THE SEARCH IS EXHAUSTIVE, AND THAT IS THE POINT
 *   There is no beam, no sampling and no randomness: the run is deterministic
 *   and it enumerates every break-free filling of the freed bands. So both
 *   outcomes mean something exact:
 *     - a complete board is a solution;
 *     - finishing without one is a PROOF that this core admits no break-free
 *       refill of these bands.
 *   The only thing that can weaken the proof is a budget (--max_nodes,
 *   --config_time_sec, --max_wall_sec, --max_boards). When one bites, the run
 *   says so and the summary reports that board as TRUNCATED rather than
 *   exhausted. Nothing else stops the search early.
 *
 * WHAT COMES OUT: THE FURTHEST IT GOT
 *   One board per input board: the state that placed the most pieces before the
 *   search ran out. Pieces on the board is the only ranking used, because it is
 *   the one measure comparable across rounds -- a per-strip level restarts each
 *   round. `--ties N` widens that to N distinct boards that all reached the same
 *   depth, dropping any that repeats an earlier one with a single frontier piece
 *   swapped: those collapse into the same board the moment a later stage frees
 *   the frontier, so they carry nothing new.
 *
 *   Every board from the exhaustive search is BREAK-FREE, so its score is 480
 *   minus the junctions its EMPTY cells leave open -- never a mismatch. A
 *   191-piece board scoring 350 is perfectly matched, not damaged. Compare a
 *   partial with a partial, or re-score both with tools/E555_rank.py.
 *
 * --max_breaks B: FINISH THE BOARD ANYWAY
 *   A break-free refill usually does not exist, and a board with 80 empty cells
 *   is awkward to hand on. With --max_breaks B the run takes the deepest
 *   break-free board it found and GREEDILY fills every remaining cell, spending
 *   at most B mismatched junctions. Same idiom as the backtracker's
 *   --break-mode stuck: take the most constrained empty cell, prefer a piece
 *   that fits exactly, break an edge only when no cell has an exact fit.
 *
 *   This is a dive, not a search. It never backtracks and it does not prove B is
 *   minimal -- it hands you a complete 256-piece board that Stage C can attack
 *   break by break. It always completes when B is large enough, because a cell's
 *   frame type fixes which pieces may sit there and the type counts stay
 *   balanced (4 corners, 56 edges, 196 inner).
 *
 *   The exhaustive part is untouched: the fill runs afterwards, from the best
 *   board the proof engine reached. --max_breaks 0 (the default) skips it.
 *
 * BUILD
 *   make roundhouse                     # or plain `make`, which builds all four
 *   By hand -- E555_database.c MUST be compiled in, it owns the seed, the
 *   oriented catalog, the (left,bottom) buckets and the edge-terminal pool:
 *     gcc -Wall -Wextra -O3 -march=native -fopenmp \
 *         E555_database.c E555_roundhouse.c -o E555_roundhouse -lm
 *   Only the setup half of that module is used. build_db_inner() is never
 *   called, so there is no multi-GB arena and no --db_file: this tool builds its
 *   own chains at width W, because CHAIN_LEN over there is a compile-time 5.
 *
 * RUN
 *   bin/E555_roundhouse seed.txt boards.csv [options]      (--help for the list)
 *
 *   # prove whether a partial's top rows can be closed perfectly (milliseconds)
 *   bin/E555_roundhouse data/seed_Edge5.txt partial.csv --rounds 1
 *
 *   # the full spiral: keep a 66-piece core, rebuild the other three sides
 *   bin/E555_roundhouse data/seed_Edge5.txt partial.csv --rounds 3 --strip_width 5
 *
 *   # same, but hand back a COMPLETE board, buying it with up to 12 breaks
 *   bin/E555_roundhouse data/seed_Edge5.txt partial.csv --rounds 3 --max_breaks 12
 *
 * VOCABULARY (used throughout the file)
 *   frame       the board after the --reverse mirror and --rotate clockwise
 *               quarter-turns. All internal coordinates are frame coordinates;
 *               output is un-rotated and un-mirrored back.
 *               A rotation maps cell (r,c) -> (15-c, r) and spin s -> (s+3)&3.
 *   strip       the W rightmost columns of the frame -- the cells being filled.
 *   level       one row of the strip. Level 0 and (when the strip reaches the
 *               top) level 15 are border chains; levels 1..14 are database
 *               chains. Levels are filled bottom-up.
 *   wall        column 15-W, already placed. Its right-facing color at each
 *               level is that level's database key.
 *   signature   the W colors a level exposes upward: (W-1) inner plus one
 *               frame-interface. It indexes the next level's cell directly.
 *   core        the cells the run KEEPS. Everything else is freed.
 *
 * --rounds: HOW MUCH OF THE BOARD TO REBUILD
 *   It sets how many strips are refilled AND, to the cell, what gets freed: each
 *   round frees exactly the band it will refill. The three cuts NEST, so a lower
 *   --rounds is a cheaper experiment, never a truncated one.
 *
 *   --rounds 1  Free the right band and refill it. Keeps (16-W) columns, full
 *               height. If that band covers the whole empty region the run
 *               returns a COMPLETE board or proves none exists. The mode for
 *               endgames and for large-neighborhood re-solves.
 *   --rounds 2  Also free the top band. The two strips plus the kept
 *               (16-W)x(16-W) square tile the board exactly, so this also ends
 *               on a complete board. Rebuilds two sides of the frame.
 *   --rounds 3  Also free the left band: keeps only the (16-2W)x(16-W) core and
 *               rebuilds three sides -- 54 of the 60 border pieces and all four
 *               corners at W=5. The namesake cut.
 *   --rounds 4+ Rejected: after three bands the board is full. To keep going,
 *               feed the output back in with a different --rotate.
 *
 * --strip_width: THE ONE DIAL THAT MATTERS
 *   W is the chain length and it sets the whole geometry. Default 5. Range 2..5;
 *   5 is the ceiling because a 6-wide chain would need a 17^6*5-cell index.
 *   --strip_width 0 picks the narrowest width whose kept region is complete and
 *   break-free, which keeps the most proven structure. Pieces kept:
 *
 *      W   --rounds 1     --rounds 2     --rounds 3     input must cover
 *      3   13x16 = 208    13x13 = 169    10x13 = 130    rows 0..12
 *      4   12x16 = 192    12x12 = 144     8x12 =  96    rows 0..11
 *      5   11x16 = 176    11x11 = 121     6x11 =  66    rows 0..10
 *
 *   The geometry is forced, not chosen: requiring each rotation to land the next
 *   wall on column 15-W gives core height 16-W and width 16-2W uniquely. Check:
 *   (16-2W)(16-W) + 2W(16-W) + 15W + W = 256.
 *
 *   COST. On the real seed every cut keeping 96 pieces or more exhausts in
 *   seconds -- a full three-round W=4 spiral is tens of seconds. The one wide cut
 *   is --rounds 3 --strip_width 5 (66-piece core): round 1 alone has hundreds of
 *   thousands of break-free fillings and each one costs a fresh oracle sweep, so
 *   in practice that cut ends on a budget. It still reports the furthest it got,
 *   and the summary marks it TRUNCATED so the number is never read as a proof.
 *
 * --rotate: WHICH SIDE EACH ROUND ATTACKS
 *   The bands are always freed right, top, left IN THE FRAME. --rotate decides
 *   what that means on the input board. -1, -2, -3 are the same turns the other
 *   way (-1 == 3).
 *
 *      --rotate   round 1   round 2   round 3   core hugs
 *         0       right     top       left      bottom
 *         1       top       left      bottom    right
 *         2       left      bottom    right     top
 *      3 or -1    bottom    right     top       left
 *
 *   The default K=1 attacks a Stage B partial's unsolved top FIRST, while the
 *   piece pool is still rich, which is the right default. K=-1 attacks the top
 *   last and re-cuts the bottom band first -- the band the pipeline fixes at row
 *   0 by random sampling and never revisits.
 *
 * --reverse: THE SPIRAL THE OTHER WAY ROUND
 *   Every strip level ends on a frame-RIGHT edge terminal, so the spiral has one
 *   handedness and no --rotate can turn it round -- the four rows above are all
 *   there is. --reverse MIRRORS THE BOARD left-right instead, which costs one
 *   swap per piece and gives the other four:
 *
 *      --reverse  round 1   round 2   round 3   core hugs
 *         0       left      top       right     bottom
 *         1       top       right     bottom    left
 *         2       right     bottom    left      top
 *      3 or -1    bottom    left      top       right
 *
 *   At the default K=1 round 1 is still the input's top either way; only rounds
 *   2 and 3 diverge. Mirroring twice is the identity, so the mirrored pieces --
 *   which do not exist in the box -- never leave this process: boards come in,
 *   are mirrored, are searched, and are mirrored back before they are written.
 *
 *   WHAT IT DOES NOT DO is free a region --rotate cannot already free. The kept
 *   core is either mirror-symmetric or lands on another --rotate's core: at
 *   --rounds 3 --strip_width 5 the 11x6 core sits on columns 5..10, dead centre,
 *   so --reverse --rotate 0 keeps exactly the same 66 cells as --rotate 0. What
 *   changes is the SEARCH. Each band is traversed the other way with the wall on
 *   the other side, so the strip DFS and the oracle's layered graph are
 *   different problems over the same cells; and because the rounds nest, the
 *   order in which bands are rebuilt decides which partial the run reaches. Run
 *   to exhaustion both directions prove the same theorem. The value is in the
 *   case that does NOT exhaust -- the wide cut, which the section above admits
 *   ends on a budget -- where the two directions reach different deepest boards.
 *   Eight distinct searches instead of four.
 *
 * --hold_band: LET THE TWO SPIRALS COMPOUND
 *   By default every band outside the core is freed, and that includes whatever
 *   a previous roundhouse pass managed to put in the LAST one. Run CCW and then
 *   CW on its output and the second pass tears out the first pass's work in that
 *   band before it starts: measured over 25 boards, pass 1 placed 100 pieces in
 *   the last band and pass 2 kept none of them. Nor does the search win them
 *   back, because a strip is filled in whole chain LEVELS and a scatter of cells
 *   is not a reachable prefix.
 *
 *   --hold_band keeps them instead. It works because of what the other spiral
 *   leaves behind: the two handednesses traverse the same band from OPPOSITE
 *   ends, so pass 1 stops having filled k complete levels at the far end of
 *   pass 2's strip -- exactly the shape this search can terminate against. The
 *   held levels stay on the board, their pieces stay out of the pool, the strip
 *   stops one level below them, and the oracle is seeded with the single
 *   signature they sit on. Every other way of filling the level underneath is
 *   then dead on colour before a piece is tried, so the search does not merely
 *   avoid disturbing the block, it fills up to MEET it.
 *
 *   The point is that a chain can only improve: pass 2 starts from everything
 *   pass 1 proved and adds to it. The cost is that it also starts CONSTRAINED --
 *   a pass free to rebuild the whole band sometimes finds a better arrangement
 *   than one that must keep the far end. Both are worth running; that is why
 *   this is a flag and not the default.
 *
 *   NOT EVERY BAND CAN BE HELD, and an unholdable one is not a refusal. If what
 *   stands there is a partial level, or carries a break, or is mis-seated, the
 *   band is freed and searched from nothing exactly as without the flag -- the
 *   run still happens, and the reason is printed with the offending cell in the
 *   input board's coordinates. Rounds 1..N-1 always free their bands.
 *
 * INPUT REQUIREMENTS
 *   Only the kept region is validated, and it is validated completely: every
 *   cell placed, every piece legally seated against the frame, every junction
 *   inside it matched. Everything outside is freed, so breaks, holes and
 *   mis-seated pieces out there are ignored -- feeding this tool a board whose
 *   top rows are broken is the normal case. A break INSIDE the core is refused
 *   and reported in the input board's coordinates: every strip is grown against
 *   the core's colors, so one stale mismatch there poisons the whole run. Inputs
 *   are deduplicated on the core: two boards agreeing there seed an identical
 *   search.
 *
 * WHAT GETS REPORTED
 *   Boards go to <out_dir>/roundhouse_round<N>_rot<K>_W<w>_miss<B>.csv as canonical
 *   514-field rows (config_id, score, pos[256], rot[256]), appended one atomic line
 *   at a time. The name carries --rounds, --rotate and --strip_width, so runs with
 *   different geometry never share a file while runs with the SAME geometry do,
 *   which is what makes a corpus sweep accumulate.
 *
 *   TWO FILES, SPLIT BY BREAKS. A board with no mismatch goes to `miss0`; one with
 *   mismatches goes to `miss<--max_breaks>`. So `miss0` is always a corpus you can
 *   trust break-free, and the two never have to be told apart afterwards. Routing
 *   is on the board's OWN break count, so a greedy fill that lands perfectly is
 *   filed with the clean boards. Files open on first write: a run that emits
 *   nothing leaves nothing behind. THE INPUT BOARD'S config_id IS KEPT, and this
 *   run appends `_<line><tag><n>` to it -- the input data line, the tag below, and
 *   a counter unique within the run. So a board carries every stage it came
 *   through, chained roundhouse passes included, and a board with no id of its own
 *   is given `p<line>` to carry. The tag says what the board is:
 *
 *     s   SOLVED     complete and break-free. The puzzle, for this cut.
 *     d   DEEPEST    the furthest the exhaustive break-free search got.
 *     f   FILLED     complete, produced by the --max_breaks greedy fill.
 *
 *   BOARDS ARE ALWAYS WRITTEN IN THE INPUT'S ORIENTATION. The frame is rotated
 *   and un-mirrored back before writing, so cell (r,c) in the output means what
 *   it meant in the input, whatever --rotate and --reverse were. Output feeds
 *   any Stage B or Stage C tool unchanged -- including this one, with a
 *   different --rotate or --reverse, which is how the core (the one region a run
 *   never touches) moves.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <omp.h>

#include "E555_database.h"

#define MAX_W        5
#define MAX_LEVEL    16                 /* frame rows 0..15 */
#define MAX_ROUNDS   3

/* -- Tunables (the file header explains what each one is for) --------------- */

static const char *g_out_dir   = "round_out";
static uint32_t g_line_first   = 0;     /* --border_row */
static uint32_t g_line_count   = 1;     /* --border_row_N */
static int      g_opt_W        = 5;     /* --strip_width, 0 = narrowest usable */
static int      g_rounds       = 3;
static int      g_rotate       = 1;
static bool     g_reverse      = false; /* --reverse: mirror the board left-right */
static int      g_stop_level   = -1;    /* --stop_row, -1 = the strip's last */
static bool     g_hold_band    = false; /* --hold_band: keep the last band's pieces */
static int      g_pin_corner[4] = { -1, -1, -1, -1 };   /* BL BR TL TR (input) */
static uint64_t g_max_nodes    = 0;
static uint32_t g_ties         = 1;     /* boards to emit at the deepest reach */
static int      g_max_breaks   = 0;     /* --max_breaks: greedy fill budget */
static bool     g_only_complete = false;
static double   g_config_time_sec = 600.0;
static double   g_max_wall_sec = 0.0;
static uint64_t g_max_boards   = 0;
static bool     g_selfcheck    = false; /* --selfcheck: validate the oracle */

/* Corner roles, indexed as g_pin_corner: 0=BL 1=BR 2=TL 3=TR, in the INPUT
   board's coordinates (cell (0,0), (0,15), (15,0), (15,15)). */
static const char *k_corner_name[4] = { "BL", "BR", "TL", "TR" };

/* -- Board (frame coordinates) --------------------------------------------- */

static Oriented g_grid[PUZZLE_SIDE][PUZZLE_SIDE];
static bool     g_has[PUZZLE_SIDE][PUZZLE_SIDE];
static uint64_t g_placed[4];            /* pieces currently on the board */
static int      g_rot_applied;          /* CW quarter-turns vs the input board */
static char     g_in_id[96];            /* the input board's config_id, kept on output */
static int      g_W;                    /* strip width in force */
static int      g_n_placed;             /* pieces on the board: the depth metric */
static int      g_avail[NUM_COLORS_TOTAL];   /* free inner-color sides, for parity */

/* -- Run state ------------------------------------------------------------- */

/* Two output files, indexed by OUT_CLEAN / OUT_BROKEN: a board with no
   mismatch never shares a file with one that has them, so `miss0` is always a
   corpus you can trust and `miss<B>` is always the bought-with-breaks pile.
   Opened on first write, so a run that emits nothing leaves no empty file. */
#define OUT_CLEAN  0
#define OUT_BROKEN 1
static FILE    *g_out_fp[2]   = { NULL, NULL };
static char     g_out_path[2][1152];
static uint32_t g_line_id  = 0;         /* input CSV line being searched */
static uint64_t g_emitted  = 0;
static double   g_t_start  = 0.0;
static double   g_t_config = 0.0;
static uint64_t g_nodes;                /* nodes spent on the current input board */
static bool     g_truncated;            /* did a budget bite on this input board? */
static volatile sig_atomic_t g_stop = 0;
/* SIGINT/SIGTERM: stop at the next budget check so the output file stays valid. */
static void handle_stop(int sig) { (void)sig; g_stop = 1; }

/* -- Run statistics -------------------------------------------------------- */
/* A run that emits nothing but refutations is the normal case on the real seed,
   so the counters that explain WHY are the important ones: refuted[] holds the
   level at which colour alone killed a strip before any piece was tried. */
static struct {
    uint32_t lines_read, lines_used, lines_dup, lines_bad;
    uint32_t lines_exhausted, lines_truncated;
    uint32_t strips, strips_refuted;
    uint32_t refuted[MAX_LEVEL+2];
    uint64_t nodes;
    uint64_t emit_solved, emit_deepest, emit_filled;
} g_stats;

/* Which side of the INPUT board a round refills. The frame always cuts right,
   then top, then left; --rotate and --reverse decide what that means on the
   board handed in. (--rotate 1, the default, makes round 1 the input's top,
   either way round.) */
static const char *k_side_name[4] = { "RIGHT", "TOP", "LEFT", "BOTTOM" };
/* Under --reverse the frame is a mirror image of the input, and a left-right
   mirror swaps exactly those two sides. */
static const char *k_side_name_rev[4] = { "LEFT", "TOP", "RIGHT", "BOTTOM" };
static const char *round_side(int round) {
    return (g_reverse ? k_side_name_rev : k_side_name)[(g_rotate + round - 1) & 3];
}

/* Every stopping condition in one place. Anything that returns true here means
   the answer is no longer a proof, so callers also set g_truncated. */
static bool budget_spent(void) {
    if (g_stop) return true;
    if (g_max_wall_sec > 0.0 && omp_get_wtime() - g_t_start >= g_max_wall_sec) return true;
    if (g_max_boards > 0 && g_emitted >= g_max_boards) return true;
    if (g_config_time_sec > 0.0 && omp_get_wtime() - g_t_config >= g_config_time_sec) return true;
    if (g_max_nodes > 0 && g_nodes >= g_max_nodes) return true;
    return false;
}

/* -- Small helpers --------------------------------------------------------- */

static Oriented rh_oriented(uint16_t pid, uint8_t spin) {
    const int e[4] = { g_seed_top[pid], g_seed_right[pid],
                       g_seed_bottom[pid], g_seed_left[pid] };
    Oriented o;
    o.piece_id = pid; o.rotation = spin;
    o.top    = (uint8_t)e[(0 + spin) & 3]; o.right = (uint8_t)e[(1 + spin) & 3];
    o.bottom = (uint8_t)e[(2 + spin) & 3]; o.left  = (uint8_t)e[(3 + spin) & 3];
    return o;
}

/* Frame sides of a piece in its canonical orientation: 0 = inner, 1 = edge,
   2 = corner. The shared module's copy of this is static, hence the local one. */
static int rh_zero_count(int pid) {
    return (g_seed_top[pid] == 0) + (g_seed_right[pid] == 0) +
           (g_seed_bottom[pid] == 0) + (g_seed_left[pid] == 0);
}

/* -- --reverse: the left-right mirror ---------------------------------------
   The bands are always freed right, then top, then left IN THE FRAME, and every
   strip level ends on a frame-RIGHT edge terminal, so the spiral has one
   handedness and no --rotate can turn it round. Mirroring does, for free.

   Reflecting the board left-right maps side d to (4-d)&3 -- top and bottom stay,
   left and right swap -- so the mirrored piece is the real one with its left and
   right colours exchanged. Writing R_s for a spin, the reflection M satisfies
   M R_s = R_{-s} M, so one placement mirrors to

       piece p at cell (r,c) spin s   ->   piece p at (r, 15-c) spin (4-s)&3

   with the piece id untouched and the whole map its own inverse. The mirrored
   pieces do not exist in the box, but they form a legal seed: swapping left and
   right preserves each piece's grey count (and a corner's two greys stay
   adjacent), which is all load_seed_and_catalog validates, so every derived
   table -- catalog, (left,bottom) buckets, edge-terminal pool, colour totals --
   builds unchanged. And rotate_cw advances a spin by 3, which under s -> -s is a
   real ANTICLOCKWISE quarter-turn: the untouched frame spiral then walks the
   real board the other way round, which is the whole point.

   The search therefore runs entirely in mirror space. The mirror is applied at
   exactly four boundaries -- the seed, the clue table, each input board and each
   emitted board -- and nowhere else. */

/* Swap every piece's left and right colours, then rebuild the oriented catalog
   the way load_seed_and_catalog does. Called between that and
   build_catalog_indices(), so nothing downstream ever sees the real seed. */
static void mirror_seed(void) {
    for (int i = 0; i < NUM_PIECES; i++) {
        int t = g_seed_left[i]; g_seed_left[i] = g_seed_right[i]; g_seed_right[i] = t;
    }
    g_cat_count = 0;
    for (int k = 0; k < g_num_inner; k++)
        for (uint8_t spin = 0; spin < 4; spin++)
            g_cat[g_cat_count++] = rh_oriented((uint16_t)g_inner_ids[k], spin);
}

/* Mirror one board in the canonical pos[]/rot[] layout, in place. Used on the
   way in (before anything reads the board) and on the way out (on a copy). */
static void mirror_line(int pos[NUM_PIECES], int rot[NUM_PIECES]) {
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (pos[pid] == 999) continue;
        int r = pos[pid] / PUZZLE_SIDE, c = pos[pid] % PUZZLE_SIDE;
        pos[pid] = r * PUZZLE_SIDE + (PUZZLE_SIDE - 1 - c);
        rot[pid] = (4 - rot[pid]) & 3;
    }
}

/* Side indices as in rh_oriented: 0=top 1=right 2=bottom 3=left. */
static int side_color(const Oriented *o, int side) {
    return side == 0 ? o->top : side == 1 ? o->right : side == 2 ? o->bottom : o->left;
}

/* The spin that puts color 0 on `side` (edges), or on both sides (corners). */
static bool orient_frame(uint16_t pid, int sideA, int sideB, Oriented *out) {
    for (uint8_t spin = 0; spin < 4; spin++) {
        Oriented o = rh_oriented(pid, spin);
        if (side_color(&o, sideA) != 0) continue;
        if (sideB >= 0 && side_color(&o, sideB) != 0) continue;
        *out = o; return true;
    }
    return false;
}

/* Put a piece on / take it off the board's bookkeeping. The colour tally the
   parity prune reads is maintained here so the hot loop never rescans. */
static void take_piece(uint16_t pid) {
    used_set(g_placed, pid);
    g_n_placed++;
    int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
    for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) g_avail[e[k]]--;
}
static void drop_piece(uint16_t pid) {
    used_clear(g_placed, pid);
    g_n_placed--;
    int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
    for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) g_avail[e[k]]++;
}

/* -- Frame rotation -------------------------------------------------------- */

/* One clockwise quarter-turn of the whole board: cell (r,c) -> (15-c, r), and a
   piece's spin advances by 3 (its old left face becomes its new top face). */
static void rotate_cw(void) {
    static Oriented ng[PUZZLE_SIDE][PUZZLE_SIDE];
    static bool     nh[PUZZLE_SIDE][PUZZLE_SIDE];
    memset(nh, 0, sizeof nh);
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!g_has[r][c]) continue;
            ng[PUZZLE_SIDE-1-c][r] = rh_oriented(g_grid[r][c].piece_id,
                                                 (uint8_t)((g_grid[r][c].rotation + 3) & 3));
            nh[PUZZLE_SIDE-1-c][r] = true;
        }
    memcpy(g_grid, ng, sizeof g_grid);
    memcpy(g_has,  nh, sizeof g_has);
    g_rot_applied = (g_rot_applied + 1) & 3;
}

/* Map a frame cell back out of the turns, undoing `k` of them. One turn back is
   (R,C) -> (C, 15-R). This lands in the space the SEARCH calls the board, which
   under --reverse is still mirrored -- frame_to_input below goes the last step. */
static void frame_to_orig(int R, int C, int k, int *r, int *c) {
    for (int i = 0; i < (k & 3); i++) { int nr = C, nc = PUZZLE_SIDE-1-R; R = nr; C = nc; }
    *r = R; *c = C;
}

/* The same map carried one step further, to the board the user actually handed
   in: undo the turns, then the --reverse mirror. Messages, corner roles and the
   emitted boards go through here; the search itself never does, because it
   works in mirror space throughout. Identical to frame_to_orig without
   --reverse. */
static void frame_to_input(int R, int C, int k, int *r, int *c) {
    frame_to_orig(R, C, k, r, c);
    if (g_reverse) *c = PUZZLE_SIDE - 1 - *c;
}

/* -- Eternity II clue pieces ------------------------------------------------
   The retained core is (16-2W)x(16-W) and always spans the middle of the board,
   so all four candidate centre cells -- (7,7) (7,8) (8,7) (8,8) -- lie inside it
   at every W in 2..5 and every --rounds. The centre clue is therefore never
   freed here and --clue_center can only VERIFY it. The four corner clues, at
   (2,2) (2,13) (13,2) (13,13), do get freed, and those are what --clue_corners
   holds in place.

   Enforcement is a filter inside the record loop of strip_dfs, not a pinned
   enumerator: that search is exhaustive and has no beam to starve, so rejecting
   a record at the level the clue sits on simply prunes the subtree. Nor is
   there a push-down to do -- rh_decode already matches every record against the
   colours the level below exposes, so "the clue's bottom must meet the piece
   under it" holds by construction. */
static int g_rh_orient = -1;              /* orientation of the board being searched */

/* One clockwise quarter-turn applied to a cell and a spin, matching rotate_cw
   exactly: (r,c) -> (15-c, r) and spin -> (spin+3)&3. */
static void orig_to_frame(int r, int c, int spin, int k, int *R, int *C, int *S) {
    for (int i = 0; i < (k & 3); i++) {
        int nr = PUZZLE_SIDE-1-c, nc = r;
        r = nr; c = nc; spin = (spin + 3) & 3;
    }
    *R = r; *C = c; *S = spin;
}

/* orig_to_frame must invert frame_to_orig for every cell and turn count. A wrong
   spin direction here would pin the right piece the wrong way round and quietly
   produce a board that satisfies every edge but not the clue, so it is checked
   rather than commented. Runs once, only when a clue flag is set. */
static void check_frame_maps(void) {
    for (int k = 0; k < 4; k++)
        for (int r = 0; r < PUZZLE_SIDE; r++)
            for (int c = 0; c < PUZZLE_SIDE; c++) {
                int R, C, S, br, bc;
                orig_to_frame(r, c, 0, k, &R, &C, &S);
                frame_to_orig(R, C, k, &br, &bc);
                if (br != r || bc != c)
                    fatal("orig_to_frame/frame_to_orig disagree at k=%d (%d,%d) -> (%d,%d) -> (%d,%d)",
                          k, r, c, R, C, br, bc);
            }
}

/* Is this clue entry active under the current flags? Entry 0 is the center. */
static inline bool clue_on(int k) {
    return (k == 0) ? (g_clue_mask & CLUE_CENTER) != 0 : (g_clue_mask & CLUE_CORNERS) != 0;
}

/* The clue table this run searches against. It is g_clue, and under --reverse it
   is g_clue mirrored: a reflected board's clues are not any of the four
   quarter-turns g_clue holds, so without this every clued board would be
   skipped for "carries none of the enabled clue pieces". The row index keeps its
   meaning -- the input board is mirrored too, so a board that was orientation o
   still matches at row o -- and g_clue itself stays untouched, so a message that
   names a cell on the user's board reads it from there. */
static ClueCell g_rh_clue[4][CLUE_N];

static void init_clue_table(void) {
    memcpy(g_rh_clue, g_clue, sizeof g_rh_clue);
    if (!g_reverse) return;
    for (int o = 0; o < 4; o++)
        for (int k = 0; k < CLUE_N; k++) {
            ClueCell *cc = &g_rh_clue[o][k];
            cc->col  = (uint8_t)(PUZZLE_SIDE - 1 - cc->col);
            cc->spin = (uint8_t)((4 - cc->spin) & 3);
        }
}

/* The orientation this board committed to: the one satisfying the most enabled
   clues. -1 when it satisfies none, which is the only case this tool cannot
   resolve on its own. */
static int rh_clue_orient_of(const int pos[NUM_PIECES], const int rot[NUM_PIECES]) {
    int best = -1, best_n = 0;
    for (int o = 0; o < 4; o++) {
        int n = 0;
        for (int k = 0; k < CLUE_N; k++) {
            if (!clue_on(k)) continue;
            const ClueCell *cc = &g_rh_clue[o][k];
            if (pos[cc->piece] == cc->row * PUZZLE_SIDE + cc->col && rot[cc->piece] == cc->spin) n++;
        }
        if (n > best_n) { best = o; best_n = n; }
    }
    return best;
}

/* Is `pid` one of the enabled clue pieces? Five entries, scanned linearly --
   this sits in the record loop, so it stays a straight-line test over a table
   that is in cache. */
static inline bool rh_is_clue_piece(uint16_t pid) {
    if (!g_clue_mask || g_rh_orient < 0) return false;
    for (int k = 0; k < CLUE_N; k++)
        if (clue_on(k) && g_rh_clue[g_rh_orient][k].piece == pid) return true;
    return false;
}

/* The clue owed by frame cell (R,C) under the current rotation, or NULL. */
static const ClueCell *rh_clue_at_frame(int R, int C, int *spin_out) {
    if (!g_clue_mask || g_rh_orient < 0) return NULL;
    for (int k = 0; k < CLUE_N; k++) {
        if (!clue_on(k)) continue;
        const ClueCell *cc = &g_rh_clue[g_rh_orient][k];
        int fr, fc, fs;
        orig_to_frame(cc->row, cc->col, cc->spin, g_rot_applied, &fr, &fc, &fs);
        if (fr == R && fc == C) { *spin_out = fs; return cc; }
    }
    return NULL;
}

/* The same map on a whole frame rectangle, for the log. A quarter-turn sends a
   rectangle to a rectangle and so does the mirror, so mapping the two opposite
   corners and taking the min/max is exact -- no scan needed. Used to say where
   the core and the strip sit on the board the user handed in, which is the one
   thing no reader can work out from frame coordinates. */
static void box_to_orig(int R0, int R1, int C0, int C1, int k,
                        int *r0, int *r1, int *c0, int *c1) {
    int a_r, a_c, b_r, b_c;
    frame_to_input(R0, C0, k, &a_r, &a_c);
    frame_to_input(R1, C1, k, &b_r, &b_c);
    *r0 = a_r < b_r ? a_r : b_r; *r1 = a_r < b_r ? b_r : a_r;
    *c0 = a_c < b_c ? a_c : b_c; *c1 = a_c < b_c ? b_c : a_c;
}

/* -- The width-W chain database -------------------------------------------- */
/* A chain is (W-1) inner pieces plus one frame-right edge terminal -- exactly a
 * segment-C record, shortened. A cell is keyed by the color exposed to the
 * chain's left plus the W colors it presents downward, the last of which is a
 * frame-interface color (1..5) because the right border column perpetuates one
 * upward. Records pack to (W-1)*lb_bits + term_bits, as in E555_database.c.
 *
 * A signature is the same W-color tuple: (W-1) inner + 1 frame-interface, so a
 * chain's exposed tops index the next level's cell directly. */

static uint32_t s_nsig;                 /* 17^(W-1) * 5 */
static uint64_t s_ncell;                /* 17 * s_nsig */
static Cell   **s_db      = NULL;
static uint8_t *s_arena   = NULL;
static size_t   s_arena_bytes = 0;
static int      s_lb_bits, s_term_bits, s_rec_bytes;
static uint64_t s_records = 0;
static uint32_t s_max_cell_n = 0;

/* Smallest field width (>= 1 bit) that can index n items. */
static int bits_for(int n) { int b = 1; while ((1 << b) < n) b++; return b; }

/* The chain width, with its range visible to the optimizer. g_W is validated to
   2..MAX_W when a board is loaded; taking the clamped copy into a local lets the
   bounds checker see that every [MAX_W] index below is in range. */
static inline int chain_w(void) {
    int w = g_W;
    if (w < 2) w = 2;
    if (w > MAX_W) w = MAX_W;
    return w;
}

/* Pack W colors into a signature: (W-1) inner-color INDICES (0..16) and one raw
   frame-interface color (1..5). Mixed-radix, interface color innermost. */
static uint32_t sig_make(const int inner_idx[], int b_iface) {
    const int W = chain_w();
    uint32_t s = 0;
    for (int i = 0; i < W - 1; i++) s = s*DIM_INNER + (uint32_t)inner_idx[i];
    return s*MAX_EDGE_SIDE_COLOR + (uint32_t)(b_iface - 1);
}

/* Inverse of sig_make, but writing RAW colors: bot[] is what the record codec
   and decode_inner_chain() expect as the cell's bottom colors. */
static void sig_bottoms(uint32_t sig, uint8_t bot[]) {
    const int W = chain_w();
    bot[W-1] = (uint8_t)(sig % MAX_EDGE_SIDE_COLOR + 1);
    uint32_t q = sig / MAX_EDGE_SIDE_COLOR;
    for (int i = W - 2; i >= 0; i--) { bot[i] = (uint8_t)(q % DIM_INNER + COLOR_MIN); q /= DIM_INNER; }
}

/* Record codec. A record is (W-1) inner fields of s_lb_bits -- each piece's
   index inside its tiny (left,bottom) catalog bucket, both colors being known at
   decode time -- plus one terminal field of s_term_bits, its index inside the
   per-left edge-terminal bucket. The whole record fits in 32 bits (asserted at
   build time) and is stored in s_rec_bytes bytes. */
static inline uint32_t rh_pack(const uint8_t f[], int term) {
    const int W = chain_w();
    uint32_t w = 0;
    for (int i = 0; i < W - 1; i++) w |= (uint32_t)f[i] << (s_lb_bits * i);
    return w | ((uint32_t)term << (s_lb_bits * (W - 1)));
}

/* Undo rh_pack. */
static inline void rh_unpack(uint32_t w, uint8_t f[], int *term) {
    const int W = chain_w();
    uint32_t m = (1u << s_lb_bits) - 1u, tm = (1u << s_term_bits) - 1u;
    for (int i = 0; i < W - 1; i++) f[i] = (uint8_t)((w >> (s_lb_bits * i)) & m);
    *term = (int)((w >> (s_lb_bits * (W - 1))) & tm);
}

/* Decode record j of a cell keyed by (la_color, bot[0..W-1]). Fills the catalog
   indices of the inner pieces, the terminal's index into g_edge_term, and the
   signature of the chain's exposed tops. */
static inline bool rh_decode(const Cell *cell, uint32_t j, int la_color,
                             const uint8_t bot[], uint16_t ci[], int *term,
                             uint32_t *succ) {
    const int W = chain_w();
    uint8_t loc[MAX_W]; int t_idx;
    rh_unpack(rec_load(cell->rec, j, s_rec_bytes), loc, &t_idx);
    uint64_t ignored[4] = {0,0,0,0};
    if (!decode_inner_chain(loc, W - 1, la_color, bot, ci, ignored, ignored)) return false;
    int cl = g_cat[ci[W-2]].right;
    if (t_idx >= g_edge_term_by_left_n[cl]) return false;
    *term = g_edge_term_by_left[cl][t_idx];
    uint32_t s = 0;
    for (int i = 0; i < W - 1; i++) s = s*DIM_INNER + (uint32_t)INNER_IDX(g_cat[ci[i]].top);
    *succ = s*MAX_EDGE_SIDE_COLOR + (uint32_t)(g_edge_term[*term].top - 1);
    return true;
}

/* Grow one chain: walk (W-1) inner pieces left to right, chaining each piece's
   right color into the next one's left, then attach every legal frame-right edge
   terminal. `used` carries the excluded set down, so a record can never contain
   an excluded piece NOR repeat a piece within itself -- an invariant the search
   relies on. Two passes: store=false counts records per cell, store=true writes
   them into the laid-out arena. */
static void db_dfs(int depth, int la, int next_left, int bidx[], uint8_t locs[],
                   uint64_t used[4], uint32_t *work, bool store) {
    if (depth == chain_w() - 1) {
        int cl = next_left;
        if (!color_is_inner(cl)) return;
        for (int k = 0; k < g_edge_term_by_left_n[cl]; k++) {
            int t = g_edge_term_by_left[cl][k];
            if (used_test(used, g_edge_term[t].piece_id)) continue;
            uint64_t fi = (uint64_t)la * s_nsig + sig_make(bidx, g_edge_term[t].bottom);
            if (!store) {
                #pragma omp atomic
                work[fi]++;
            } else {
                uint32_t pos;
                #pragma omp atomic capture
                pos = work[fi]++;
                rec_store(s_db[fi]->rec, pos, s_rec_bytes, rh_pack(locs, k));
            }
        }
        return;
    }
    for (int i = 0; i < g_left_count[next_left]; i++) {
        int ci = g_left_bucket[next_left][i];
        uint16_t pid = g_cat[ci].piece_id;
        if (used_test(used, pid)) continue;
        bidx[depth] = INNER_IDX(g_cat[ci].bottom);
        locs[depth] = (uint8_t)g_cat_to_lb_local[ci];
        used_set(used, pid);
        db_dfs(depth+1, la, g_cat[ci].right, bidx, locs, used, work, store);
        used_clear(used, pid);
    }
}

/* One full pass of the build, parallel over the first piece of the chain. */
static void db_pass(uint32_t *work, bool store) {
    #pragma omp parallel for schedule(dynamic, 4)
    for (int ci0 = 0; ci0 < g_cat_count; ci0++) {
        uint16_t pid0 = g_cat[ci0].piece_id;
        if (used_test(g_db_exclude, pid0)) continue;
        uint64_t used[4];
        memcpy(used, g_db_exclude, sizeof used);
        uint8_t locs[MAX_W]; int bidx[MAX_W];
        int la = INNER_IDX(g_cat[ci0].left);
        bidx[0] = INNER_IDX(g_cat[ci0].bottom);
        locs[0] = (uint8_t)g_cat_to_lb_local[ci0];
        used_set(used, pid0);
        db_dfs(1, la, g_cat[ci0].right, bidx, locs, used, work, store);
    }
}

/* Build the chain database for the current g_db_exclude. Two passes (count, then
   store) into one arena, as the shared module does. */
static void build_chain_db(void) {
    double t0 = omp_get_wtime();
    if (s_arena) { munmap(s_arena, s_arena_bytes); s_arena = NULL; s_arena_bytes = 0; }
    if (!s_db) s_db = xmalloc(s_ncell * sizeof(Cell *));
    memset(s_db, 0, s_ncell * sizeof(Cell *));

    build_edge_terminal_pool();          /* honours g_db_exclude in free mode */
    int maxt = 1;
    for (int c = 0; c < NUM_COLORS_TOTAL; c++)
        if (g_edge_term_by_left_n[c] > maxt) maxt = g_edge_term_by_left_n[c];
    s_lb_bits = g_lb_bits; s_term_bits = bits_for(maxt);
    if ((g_W - 1) * s_lb_bits + s_term_bits > 32)
        fatal("chain record needs %d bits (>32)", (g_W-1)*s_lb_bits + s_term_bits);
    s_rec_bytes = ((g_W - 1) * s_lb_bits + s_term_bits + 7) / 8;

    uint32_t *cnt = calloc(s_ncell, sizeof(uint32_t));
    if (!cnt) fatal("calloc for chain count array (%" PRIu64 " cells) failed", s_ncell);
    db_pass(cnt, false);

    uint64_t bytes = 0; s_records = 0; s_max_cell_n = 0;
    uint64_t cells = 0;
    for (uint64_t fi = 0; fi < s_ncell; fi++) {
        if (!cnt[fi]) continue;
        bytes += cell_stride(cnt[fi], s_rec_bytes);
        s_records += cnt[fi]; cells++;
        if (cnt[fi] > s_max_cell_n) s_max_cell_n = cnt[fi];
    }
    if (!cells) fatal("chain database is empty: no legal width-%d chain from this pool", g_W);
    s_arena = arena_map(bytes); s_arena_bytes = bytes;
    uint64_t off = 0;
    for (uint64_t fi = 0; fi < s_ncell; fi++) {
        if (!cnt[fi]) continue;
        Cell *c = (Cell *)(s_arena + off);
        c->n = cnt[fi]; c->_pad = 0;
        s_db[fi] = c;
        off += cell_stride(cnt[fi], s_rec_bytes);
    }
    memset(cnt, 0, s_ncell * sizeof(uint32_t));
    db_pass(cnt, true);
    free(cnt);

    if (g_verbose)
        printf("[db] W=%d: %" PRIu64 " records  %" PRIu64 " cells  %.3f GB  rec=%dB  "
               "max=%u  (%.2fs)\n", g_W, s_records, cells, (double)bytes/1e9,
               s_rec_bytes, s_max_cell_n, omp_get_wtime()-t0);
}

/* -- Border chains (level 0 and the top closure) --------------------------- */
/* W pieces along a horizontal frame row: (W-1) edges with their frame side out,
 * then one corner in the last column with two frame sides. They chain on
 * frame-interface colors, starting from the color the placed cell at
 * (row, 15-W) exposes to its right. */

typedef struct {
    uint16_t pid[MAX_W];
    uint8_t  spin[MAX_W];
    uint32_t sig;                       /* tops (level 0) or bottoms (closure) */
    uint64_t mask[4];
} BorderChain;

/* Per round, because round 2 enumerates its own chains while round 1 is still
   walking its list. Nothing here is shared between rounds. */
static BorderChain *g_bc[MAX_ROUNDS+2];
static size_t       g_bc_n[MAX_ROUNDS+2], g_bc_cap[MAX_ROUNDS+2];
static BorderChain *g_close[MAX_ROUNDS+2];
static size_t       g_close_n[MAX_ROUNDS+2], g_close_cap[MAX_ROUNDS+2];

static void bc_push(BorderChain **arr, size_t *n, size_t *cap, const BorderChain *b) {
    if (*n == *cap) { *cap = *cap ? *cap*2 : 256; *arr = xrealloc(*arr, *cap * sizeof(BorderChain)); }
    (*arr)[(*n)++] = *b;
}

/* frame_side: 2 for the bottom row (frame points down), 0 for the top row. The
   corner also carries frame on its right (column 15). */
static void bc_dfs(int depth, int left_need, int frame_side, int pin_corner,
                   uint64_t used[4], BorderChain *cur, int inner[],
                   BorderChain **out, size_t *n, size_t *cap) {
    const int W = chain_w();
    if (depth == W - 1) {
        for (int pid = 0; pid < NUM_PIECES; pid++) {
            if (rh_zero_count(pid) != 2) continue;
            if (pin_corner >= 0 && pid != pin_corner) continue;
            if (used_test(used, (uint16_t)pid)) continue;
            Oriented o;
            if (!orient_frame((uint16_t)pid, frame_side, 1, &o)) continue;
            if (o.left != left_need) continue;
            int expose = (frame_side == 2) ? o.top : o.bottom;
            if (!color_is_edge_iface(expose)) continue;
            cur->pid[depth] = (uint16_t)pid; cur->spin[depth] = o.rotation;
            cur->sig = sig_make(inner, expose);
            memcpy(cur->mask, used, sizeof cur->mask);
            used_set(cur->mask, (uint16_t)pid);
            bc_push(out, n, cap, cur);
        }
        return;
    }
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (rh_zero_count(pid) != 1) continue;
        if (used_test(used, (uint16_t)pid)) continue;
        Oriented o;
        if (!orient_frame((uint16_t)pid, frame_side, -1, &o)) continue;
        if (o.left != left_need) continue;
        int expose = (frame_side == 2) ? o.top : o.bottom;
        if (!color_is_inner(expose)) continue;
        cur->pid[depth] = (uint16_t)pid; cur->spin[depth] = o.rotation;
        inner[depth] = INNER_IDX(expose);
        used_set(used, (uint16_t)pid);
        bc_dfs(depth+1, o.right, frame_side, pin_corner, used, cur, inner, out, n, cap);
        used_clear(used, (uint16_t)pid);
    }
}

/* Which corner role of the INPUT board sits at frame cell (R,15)? Reading the
   role in input coordinates is what keeps --BL/--BR/--TL/--TR naming the board
   the user handed in under --reverse, with no pin table to swap. */
static int corner_role_at(int R) {
    int r, c;
    frame_to_input(R, PUZZLE_SIDE-1, g_rot_applied, &r, &c);
    return (r == 0 ? 0 : 2) + (c == 0 ? 0 : 1);
}

/* All legal border chains for one horizontal frame row of the strip, starting
   from the color the placed wall cell exposes to its right. Pieces already on
   the board are unavailable. frame_side is 2 for the strip's bottom row (frame
   pointing down) and 0 for its top row. */
static void enumerate_border_row(int row, int frame_side,
                                 BorderChain **out, size_t *n, size_t *cap) {
    *n = 0;
    int left_need = g_grid[row][PUZZLE_SIDE-1-g_W].right;
    if (!color_is_edge_iface(left_need)) return;
    uint64_t used[4]; memcpy(used, g_placed, sizeof used);
    BorderChain cur; memset(&cur, 0, sizeof cur);
    int inner[MAX_W];
    bc_dfs(0, left_need, frame_side, g_pin_corner[corner_role_at(row)],
           used, &cur, inner, out, n, cap);
}

/* Closure chains are sorted by signature so try_close() can binary-search them. */
static int cmp_bc_sig(const void *a, const void *b) {
    uint32_t x = ((const BorderChain *)a)->sig, y = ((const BorderChain *)b)->sig;
    return (x > y) - (x < y);
}

/* -- The oracle ------------------------------------------------------------ */
/* One backward sweep per strip says, for every level and every frontier colour
 * signature, whether ANY colour-legal continuation reaches the far border. It
 * ignores the piece supply entirely (a piece may recur at different levels), so
 * it is a relaxation: dead in the relaxation means dead, full stop, whatever
 * pieces you hold. That is what makes an empty result a theorem.
 *
 * Only the boolean is kept. An earlier version also accumulated an exact
 * completion COUNT per state, which cost an array of 17^(W-1)*5 doubles per
 * level and forbade the early exit below; with the beam gone nothing reads the
 * count except --selfcheck, which does its own enumeration. */

static uint64_t *g_live[MAX_ROUNDS+2][MAX_LEVEL+2];
static int       g_wall[MAX_ROUNDS+2][MAX_LEVEL+2];   /* wall colour per level */
static int       g_top_level[MAX_ROUNDS+2];           /* last database level */
static bool      g_close_top[MAX_ROUNDS+2];           /* is there a closure row? */
static bool      g_held[MAX_ROUNDS+2];                /* --hold_band: levels held above */
static int       g_held_n[MAX_ROUNDS+2];              /* how many, for the log */
static uint32_t  g_held_sig[MAX_ROUNDS+2];            /* what they sit on */
static size_t    g_live_words = 0;

static inline bool live_test(const uint64_t *bs, uint32_t s) {
    return (bs[s >> 6] >> (s & 63)) & 1u;
}

/* (Re)size the per-level bitsets for the current signature space. Called once
   per strip; W (and so s_nsig) can change between input boards. */
static void oracle_alloc(void) {
    g_live_words = (s_nsig + 63) / 64;
    for (int rd = 0; rd <= MAX_ROUNDS+1; rd++)
        for (int r = 0; r <= MAX_LEVEL+1; r++)
            g_live[rd][r] = xrealloc(g_live[rd][r], g_live_words * sizeof(uint64_t));
}

/* One backward sweep over levels lo..hi, reading the bitset at hi+1.
   Parallel over WORDS of the bitset, not over signatures: each thread then owns
   its words outright, so setting a bit needs no atomic and cannot lose a
   neighbour's update. A live branch pruned by a lost bit would be silent and
   would quietly break the proof. */
static void oracle_backward(int round, int lo, int hi) {
    for (int r = hi; r >= lo; r--) {
        uint64_t *live = g_live[round][r];
        const uint64_t *above = g_live[round][r+1];
        const int wall = g_wall[round][r];
        if (!color_is_inner(wall)) { memset(live, 0, g_live_words * sizeof(uint64_t)); continue; }
        const uint64_t base = (uint64_t)INNER_IDX(wall) * s_nsig;
        /* Dynamic, not static: the record scan below stops at the first survivor,
           so a word of live signatures costs almost nothing while a word of dead
           ones is scanned in full -- and dead signatures cluster, because
           liveness follows the colours. A fixed contiguous quarter each left one
           thread with far the heaviest share. */
        #pragma omp parallel for schedule(dynamic, 8)
        for (long w = 0; w < (long)g_live_words; w++) {
            uint64_t bits = 0;
            for (int b = 0; b < 64; b++) {
                uint32_t sig = (uint32_t)(w*64 + b);
                if (sig >= s_nsig) break;
                const Cell *cell = s_db[base + sig];
                if (!cell) continue;
                uint8_t bot[MAX_W]; sig_bottoms(sig, bot);
                for (uint32_t j = 0; j < cell->n; j++) {
                    uint16_t ci[MAX_W]; int term; uint32_t succ;
                    if (!rh_decode(cell, j, wall, bot, ci, &term, &succ)) continue;
                    if (!live_test(above, succ)) continue;
                    bits |= 1ULL << b;      /* one survivor is enough: stop here */
                    break;
                }
            }
            live[w] = bits;
        }
    }
}

/* Seed the level above the last database level: the closure chains' required
   signatures, or "anything goes" when the strip stops below the top border. */
static void oracle_seed_top(int round) {
    int t = g_top_level[round] + 1;
    uint64_t *live = g_live[round][t];
    /* --hold_band: the level above is not open, it is the held block, and only
       the signature it sits on can be reached. One bit, and the existing
       backward sweep turns it into the meeting constraint for every level. */
    if (g_held[round]) {
        memset(live, 0, g_live_words * sizeof(uint64_t));
        uint32_t sg = g_held_sig[round];
        live[sg >> 6] |= 1ULL << (sg & 63);
        return;
    }
    if (!g_close_top[round]) {
        for (size_t i = 0; i < g_live_words; i++) live[i] = ~0ULL;
        return;
    }
    memset(live, 0, g_live_words * sizeof(uint64_t));
    for (size_t i = 0; i < g_close_n[round]; i++) {
        uint32_t s = g_close[round][i].sig;
        live[s >> 6] |= 1ULL << (s & 63);
    }
}

/* Live signatures at one level, for the log. */
static uint64_t live_count(int round, int level) {
    uint64_t n = 0;
    for (size_t i = 0; i < g_live_words; i++)
        n += (uint64_t)__builtin_popcountll(g_live[round][level][i]);
    return n;
}

/* -- Parity / supply prune ------------------------------------------------- */
/* Exact whenever the strip is the last unfilled region: then every side of every
 * unplaced piece either faces a known boundary -- the frontier below, one wall
 * color per remaining level, or the frame -- or pairs with another unplaced
 * piece inside the strip. So for every inner color the surplus must be
 * non-negative, and even. */

static int g_wall_suffix[MAX_ROUNDS+2][MAX_LEVEL+2][NUM_COLORS_TOTAL];
static int g_cells_above[MAX_ROUNDS+2][MAX_LEVEL+2];
/* --hold_band: the inner colours the held block's underside consumes. Without
   this the count above is wrong in exactly the case the prune is active. */
static int g_held_need[MAX_ROUNDS+2][NUM_COLORS_TOTAL];

static void parity_prepare(int round) {
    memset(g_wall_suffix[round], 0, sizeof g_wall_suffix[round]);
    /* The strip's topmost level does not face the frame when a block is held --
       it faces the block, and those sides are spoken for. The prune's premise is
       that every side of an unplaced piece pairs off or meets a KNOWN boundary,
       so this one has to be counted with the others or the surplus comes out too
       high and its parity flips, refuting arrangements that are perfectly good. */
    memset(g_held_need[round], 0, sizeof g_held_need[round]);
    if (g_held[round]) {
        uint8_t bot[MAX_W]; sig_bottoms(g_held_sig[round], bot);
        for (int i = 0; i < chain_w() - 1; i++) g_held_need[round][bot[i]]++;
    }
    for (int r = g_top_level[round]; r >= 0; r--) {
        memcpy(g_wall_suffix[round][r], g_wall_suffix[round][r+1],
               sizeof g_wall_suffix[round][r]);
        if (color_is_inner(g_wall[round][r])) g_wall_suffix[round][r][g_wall[round][r]]++;
    }
    for (int r = 0; r <= MAX_LEVEL+1; r++) {
        int lv = g_top_level[round] - r + 1; if (lv < 0) lv = 0;
        g_cells_above[round][r] = lv * chain_w() + (g_close_top[round] ? chain_w() : 0);
    }
}

/* Sound only when the strip is the last unfilled region, which is exactly the
   `cells remaining == pieces unplaced` test on the first line -- otherwise the
   unplaced pool also has to feed regions this strip knows nothing about, and
   nothing can be concluded. */
static bool parity_ok(int round, int level, uint32_t sig) {
    if (g_cells_above[round][level] != NUM_PIECES - g_n_placed) return true;
    /* Nothing above still to place: there are no sides left to pair off, and the
       junction `sig` names is between two levels that are already down. Only a
       held block reaches this level with the strip complete -- an open top means
       a later round still has cells, so the test above has already returned. */
    if (!g_cells_above[round][level]) return true;
    const int W = chain_w();
    uint8_t bot[MAX_W]; sig_bottoms(sig, bot);
    int need[NUM_COLORS_TOTAL] = {0};
    for (int i = 0; i < W - 1; i++) need[bot[i]]++;
    for (int c = COLOR_MIN; c <= COLOR_MAX; c++) {
        int s = g_avail[c] - need[c] - g_wall_suffix[round][level][c]
                - g_held_need[round][c];
        if (s < 0 || (s & 1)) return false;
    }
    return true;
}

/* -- The furthest it got --------------------------------------------------- */
/* The whole output of a run. Depth is PIECES ON THE BOARD -- the one measure
 * comparable across rounds, unlike a per-strip level, which restarts each time.
 * Boards are kept in the input's orientation, so taking a snapshot never has to
 * disturb the live grid. */

typedef struct { int pos[NUM_PIECES], rot[NUM_PIECES]; int placed; } Snap;

static Snap    *g_best      = NULL;     /* boards tied at the deepest reach */
static uint32_t g_best_n    = 0, g_best_cap = 0;
static int      g_best_depth = -1;
static uint64_t *g_key      = NULL;     /* identities of the boards kept */
static size_t    g_key_n    = 0, g_key_cap = 0;

static void best_reset(void) { g_best_n = 0; g_best_depth = -1; g_key_n = 0; }

/* The current board in the INPUT's orientation. Undoing k clockwise turns maps
   the cell with frame_to_orig and the spin by +k (a turn added 3, and -3k is +k
   mod 4), so no grid copy or rotation is needed. */
static void snapshot(Snap *s) {
    for (int i = 0; i < NUM_PIECES; i++) { s->pos[i] = 999; s->rot[i] = 0; }
    s->placed = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!g_has[r][c]) continue;
            int ar, ac; frame_to_orig(r, c, g_rot_applied, &ar, &ac);
            s->pos[g_grid[r][c].piece_id] = ar*PUZZLE_SIDE + ac;
            s->rot[g_grid[r][c].piece_id] = (g_grid[r][c].rotation + g_rot_applied) & 3;
            s->placed++;
        }
}

/* What one placed cell contributes to its board's identity. XOR-ing these gives
   an order-independent hash, and XOR-ing one back OUT blanks that cell -- which
   is how two boards differing at a single cell are recognized in O(1). */
static uint64_t cell_key(int cell, int piece, int spin) {
    uint64_t h = (uint64_t)cell * 1024u + (uint64_t)piece * 4u + (uint64_t)spin;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    return h ^ (h >> 33);
}

static uint64_t board_key(const Snap *s) {
    uint64_t h = 0;
    for (int p = 0; p < NUM_PIECES; p++)
        if (s->pos[p] != 999) h ^= cell_key(s->pos[p], p, s->rot[p]);
    return h;
}

static void key_push(uint64_t k) {
    if (g_key_n == g_key_cap) {
        g_key_cap = g_key_cap ? g_key_cap*2 : 1024;
        g_key = xrealloc(g_key, g_key_cap * sizeof(uint64_t));
    }
    g_key[g_key_n++] = k;
}
static bool key_seen(uint64_t k) {
    for (size_t i = 0; i < g_key_n; i++) if (g_key[i] == k) return true;
    return false;
}

/* Is `s` a board we have not kept already, give or take ONE frontier piece? A
   frontier cell is a placed cell with an empty orthogonal neighbour; swapping
   the piece there changes nothing a later stage would keep, because freeing the
   frontier collapses both boards into the same search. Exact repeats are caught
   by the same test with nothing blanked. */
static bool best_is_new(const Snap *s) {
    uint64_t full = board_key(s);
    if (key_seen(full)) return false;
    int occ[NUM_PIECES];
    for (int i = 0; i < NUM_PIECES; i++) occ[i] = -1;
    for (int p = 0; p < NUM_PIECES; p++) if (s->pos[p] != 999) occ[s->pos[p]] = p;
    uint64_t blanked[NUM_PIECES]; int nb = 0;
    for (int cell = 0; cell < NUM_PIECES; cell++) {
        int p = occ[cell];
        if (p < 0) continue;
        int r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
        bool frontier =
            (r+1 < PUZZLE_SIDE && occ[cell+PUZZLE_SIDE] < 0) ||
            (r   > 0           && occ[cell-PUZZLE_SIDE] < 0) ||
            (c+1 < PUZZLE_SIDE && occ[cell+1] < 0) ||
            (c   > 0           && occ[cell-1] < 0);
        if (!frontier) continue;
        uint64_t b = full ^ cell_key(cell, p, s->rot[p]);
        if (key_seen(b)) return false;
        blanked[nb++] = b;
    }
    key_push(full);
    for (int i = 0; i < nb; i++) key_push(blanked[i]);
    return true;
}

/* Offer the current board as the furthest reached. Cheap enough to call from
   every dead end: in the common case it compares two ints and returns. */
static void offer_board(void) {
    if (g_n_placed < g_best_depth) return;
    if (g_n_placed > g_best_depth) { best_reset(); g_best_depth = g_n_placed; }
    if (g_best_n >= g_ties) return;
    if (g_best_n == g_best_cap) {
        g_best_cap = g_best_cap ? g_best_cap*2 : 8;
        g_best = xrealloc(g_best, g_best_cap * sizeof(Snap));
    }
    snapshot(&g_best[g_best_n]);
    if (!best_is_new(&g_best[g_best_n])) return;
    g_best_n++;
}

/* -- Emission -------------------------------------------------------------- */

/* Matched junctions out of 480, and separately the mismatched ones. Both count
   only placed-placed pairs, so an empty cell costs score without ever counting
   as a break. */
static void score_snap(const Snap *s, int *matched, int *breaks) {
    Oriented o[NUM_PIECES]; bool has[NUM_PIECES];
    memset(has, 0, sizeof has);
    for (int p = 0; p < NUM_PIECES; p++)
        if (s->pos[p] != 999) {
            o[s->pos[p]] = rh_oriented((uint16_t)p, (uint8_t)s->rot[p]);
            has[s->pos[p]] = true;
        }
    *matched = 0; *breaks = 0;
    for (int cell = 0; cell < NUM_PIECES; cell++) {
        int r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
        if (c+1 < PUZZLE_SIDE && has[cell] && has[cell+1])
            ((o[cell].right == o[cell+1].left) ? (*matched)++ : (*breaks)++);
        if (r+1 < PUZZLE_SIDE && has[cell] && has[cell+PUZZLE_SIDE])
            ((o[cell].top == o[cell+PUZZLE_SIDE].bottom) ? (*matched)++ : (*breaks)++);
    }
}

/* Write one board as a canonical CSV line and say what it is. It is routed by
   the board's OWN break count, not by its tag: a greedy fill that happens to
   land perfectly belongs with the break-free boards. */
static void emit_snap(const Snap *snap_in, const char *tag) {
    if (g_only_complete && snap_in->placed < NUM_PIECES) return;

    /* The score is read off the board the SEARCH holds, because score_snap
       rebuilds each piece from g_seed_*, which under --reverse is the mirrored
       seed: scoring an un-mirrored board against it would compare real
       placements with mirrored colours. Mirroring is a symmetry of the puzzle,
       so the two agree -- as long as board and seed are in the same space. */
    int matched, breaks;
    score_snap(snap_in, &matched, &breaks);

    /* --reverse: back out of mirror space for everything that leaves this
       function -- the CSV fields and the hole box alike. On a COPY, because
       g_best[] entries and the greedy fill's base snapshot are both read again
       after they are emitted. */
    Snap mirrored;
    const Snap *s = snap_in;
    if (g_reverse) { mirrored = *snap_in; mirror_line(mirrored.pos, mirrored.rot); s = &mirrored; }

    int which = breaks ? OUT_BROKEN : OUT_CLEAN;
    if (!g_out_fp[which]) {
        g_out_fp[which] = fopen(g_out_path[which], "a");
        if (!g_out_fp[which]) fatal("cannot open %s: %s", g_out_path[which], strerror(errno));
    }
    /* Provenance, the way Stage C does it: the input's config_id is KEPT and this
       run appends its own suffix after an underscore -- the input line, the tag,
       and a counter unique within the run. Chain two roundhouse passes and the id
       carries both, so a merged corpus still says where every row came from. */
    char id[128];
    snprintf(id, sizeof id, "%s_%u%s%" PRIu64, g_in_id, g_line_id, tag, g_emitted);

    FILE *fp = g_out_fp[which];
    fprintf(fp, "%s, %d", id, matched);
    for (int i = 0; i < NUM_PIECES; i++) fprintf(fp, ", %d", s->pos[i]);
    for (int i = 0; i < NUM_PIECES; i++) fprintf(fp, ", %d", s->rot[i]);
    fputc('\n', fp);
    fflush(fp);

    char hole[64] = "complete";
    if (s->placed < NUM_PIECES) {
        bool occ[NUM_PIECES]; memset(occ, 0, sizeof occ);
        for (int p = 0; p < NUM_PIECES; p++) if (s->pos[p] != 999) occ[s->pos[p]] = true;
        int r0 = PUZZLE_SIDE, r1 = -1, c0 = PUZZLE_SIDE, c1 = -1;
        for (int cell = 0; cell < NUM_PIECES; cell++) {
            if (occ[cell]) continue;
            int r = cell / PUZZLE_SIDE, c = cell % PUZZLE_SIDE;
            if (r < r0) r0 = r;
            if (r > r1) r1 = r;
            if (c < c0) c0 = c;
            if (c > c1) c1 = c;
        }
        snprintf(hole, sizeof hole, "%d empty in rows %d..%d x cols %d..%d",
                 NUM_PIECES - s->placed, r0, r1, c0, c1);
    }
    printf("[emit] %s  %d/%d placed  %d/480 matched  %d break(s)  %s\n",
           id, s->placed, NUM_PIECES, matched, breaks, hole);
    g_emitted++;
    if      (tag[0] == 's') g_stats.emit_solved++;
    else if (tag[0] == 'f') g_stats.emit_filled++;
    else                    g_stats.emit_deepest++;
}

/* -- Strip search ---------------------------------------------------------- */
/* One serial depth-first walk over the whole spiral: a strip that reaches its
 * target level is applied to the board and handed straight to the next round,
 * with no list of survivors in between. That is what "no beam" means here --
 * nothing is ranked, capped or dropped, so finishing the walk is a proof.
 *
 * The walk is serial on purpose. It is not where the time goes: a strip costs a
 * few hundred nodes, while its oracle sweep costs milliseconds to tenths of a
 * second, and the sweep is what runs on every core. Keeping the tree walk
 * single-threaded removes every shared-state hazard between rounds and makes the
 * output deterministic. */

static void run_round(int round);

/* Put one chain level on the board / take it off again. */
static void level_place(int level, const uint16_t pid[], const uint8_t spin[]) {
    const int W = chain_w();
    for (int k = 0; k < W; k++) {
        int c = PUZZLE_SIDE - W + k;
        g_grid[level][c] = rh_oriented(pid[k], spin[k]);
        g_has[level][c] = true;
        take_piece(pid[k]);
    }
}
static void level_remove(int level) {
    const int W = chain_w();
    for (int k = 0; k < W; k++) {
        int c = PUZZLE_SIDE - W + k;
        g_has[level][c] = false;
        drop_piece(g_grid[level][c].piece_id);
    }
}

/* A strip has reached its target: either the spiral is done, or the board is
   rotated and handed to the next round. Recorded either way, so a board is never
   lost because the NEXT round happened to be refuted at once. */
static void strip_done(int round) {
    offer_board();
    if (round >= g_rounds) return;
    Oriented sg[PUZZLE_SIDE][PUZZLE_SIDE]; bool sh[PUZZLE_SIDE][PUZZLE_SIDE];
    int srot = g_rot_applied;
    memcpy(sg, g_grid, sizeof sg); memcpy(sh, g_has, sizeof sh);
    rotate_cw();
    run_round(round+1);
    memcpy(g_grid, sg, sizeof g_grid); memcpy(g_has, sh, sizeof g_has);
    g_rot_applied = srot;
}

/* Close the strip's top row from the precomputed chains (sorted by signature). */
static void try_close(int round, uint32_t sig) {
    const int W = chain_w();
    const BorderChain *cl = g_close[round];
    size_t lo = 0, hi = g_close_n[round];
    while (lo < hi) { size_t m = (lo+hi)/2; if (cl[m].sig < sig) lo = m+1; else hi = m; }
    bool any = false;
    for (size_t i = lo; i < g_close_n[round] && cl[i].sig == sig; i++) {
        bool clash = false;
        for (int k = 0; k < W && !clash; k++) clash = used_test(g_placed, cl[i].pid[k]);
        if (clash) continue;
        level_place(g_top_level[round]+1, cl[i].pid, cl[i].spin);
        strip_done(round);
        level_remove(g_top_level[round]+1);
        any = true;
        if (budget_spent()) break;
    }
    if (!any) offer_board();
}

/* Fill levels bottom-up. Per record, in the order that rejects most cheaply:
   decode, oracle bitset test (dead on colour -- one lookup), piece disjointness,
   parity/supply. `advanced` records whether any child survived, so a node that
   rejects everything reports itself as a dead end. */
static void strip_dfs(int round, int level, uint32_t sig) {
    const int W = chain_w();
    if (budget_spent()) { g_truncated = true; return; }
    if (level > g_top_level[round]) {
        if (g_close_top[round]) try_close(round, sig);
        else strip_done(round);
        return;
    }

    const Cell *cell = s_db[(uint64_t)INNER_IDX(g_wall[round][level]) * s_nsig + sig];
    if (!cell) { offer_board(); return; }
    uint8_t bot[MAX_W]; sig_bottoms(sig, bot);
    bool advanced = false;

    for (uint32_t j = 0; j < cell->n; j++) {
        g_nodes++;
        uint16_t ci[MAX_W]; int term; uint32_t succ;
        if (!rh_decode(cell, j, g_wall[round][level], bot, ci, &term, &succ)) continue;
        if (!live_test(g_live[round][level+1], succ)) continue;

        /* A record never repeats a piece within itself -- db_dfs carries a used
           mask -- so only the levels below have to be tested. */
        uint16_t pids[MAX_W]; uint8_t spins[MAX_W];
        bool clash = false;
        for (int i = 0; i < W - 1 && !clash; i++) {
            pids[i]  = g_cat[ci[i]].piece_id;
            spins[i] = g_cat[ci[i]].rotation;
            clash = used_test(g_placed, pids[i]);
        }
        if (clash) continue;
        pids[W-1]  = g_edge_term[term].piece_id;
        spins[W-1] = g_edge_term[term].rotation;
        if (used_test(g_placed, pids[W-1])) continue;

        /* Clue guards, two of them and both needed. A cell that owes a clue must
           receive exactly that piece at exactly that spin; and a clue piece may
           not be laid anywhere but its own cell -- the analogue of the beamer's
           database exclusion, without which round 1 could spend a piece round 3
           still needs and refute a strip that is actually fine. */
        if (g_clue_mask && g_rh_orient >= 0) {
            bool bad = false;
            for (int i = 0; i < W && !bad; i++) {
                int spin_need;
                const ClueCell *cc = rh_clue_at_frame(level, PUZZLE_SIDE - W + i, &spin_need);
                if (cc) bad = (pids[i] != cc->piece || spins[i] != (uint8_t)spin_need);
                else    bad = rh_is_clue_piece(pids[i]);
            }
            if (bad) continue;
        }

        level_place(level, pids, spins);
        if (parity_ok(round, level+1, succ)) {
            advanced = true;
            strip_dfs(round, level+1, succ);
        }
        level_remove(level);
        if (budget_spent()) { g_truncated = true; break; }
    }
    if (!advanced) offer_board();
}

/* -- Strip geometry -------------------------------------------------------- */

/* Derive this round's strip from the board: the wall is column 15-W, and the
   strip runs from row 0 up to the highest row whose wall cell is placed. */
static bool strip_geometry(int round) {
    int wall_col = PUZZLE_SIDE - 1 - g_W;
    if (wall_col < 0) return false;
    int top = -1;
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        if (!g_has[r][wall_col]) break;
        top = r;
    }
    if (top < 1) return false;

    /* The strip is normally empty: everything outside the core was freed. Under
       --hold_band the last round's band still carries the pieces it arrived
       with, and they must form COMPLETE chain levels stacked at the far end --
       which is exactly the shape the other spiral leaves, having filled that
       band from the opposite side until it stopped. Anything else (a partial
       level, or a gap underneath the block) is not a shape this search can meet,
       and is refused rather than quietly freed. */
    int held = 0;
    for (int r = 0; r <= top; r++) {
        int n = 0;
        for (int c = PUZZLE_SIDE-g_W; c < PUZZLE_SIDE; c++) if (g_has[r][c]) n++;
        if (n == 0) {
            if (held) return false;            /* a gap below the held block */
            continue;
        }
        if (n != g_W) return false;            /* a level only partly standing */
        held++;
    }
    if (held && !g_hold_band) return false;    /* without the flag: as before */

    g_held[round] = (held > 0);
    g_held_n[round] = held;
    g_close_top[round] = (top == PUZZLE_SIDE-1) && !held;
    g_top_level[round] = g_close_top[round] ? PUZZLE_SIDE-2 : top - held;

    /* What the held block sits on, packed the way the database keys a level.
       Seeding the oracle with this one signature is what makes the search meet
       the held pieces: every other way of filling the level below is dead on
       colour before a single piece is tried. */
    if (held) {
        if (g_top_level[round] < 1) return false;      /* nothing left to search */
        const int W = chain_w();
        int lvl = g_top_level[round] + 1, inner[MAX_W];
        for (int k = 0; k < W-1; k++) {
            int b = g_grid[lvl][PUZZLE_SIDE-W+k].bottom;
            if (!color_is_inner(b)) return false;
            inner[k] = INNER_IDX(b);
        }
        int iface = g_grid[lvl][PUZZLE_SIDE-1].bottom;
        if (iface < 1 || iface > MAX_EDGE_SIDE_COLOR) return false;
        g_held_sig[round] = sig_make(inner, iface);
    }
    /* --stop_row truncates the strip. Anything at or below the last database
       level drops the top border, and the strip stops there with its top open. */
    if (g_stop_level >= 0 && g_stop_level <= g_top_level[round]) {
        if (g_stop_level < 1) fatal("--stop_row must be >= 1");
        g_top_level[round] = g_stop_level;
        g_close_top[round] = false;
    }
    for (int r = 0; r <= g_top_level[round]; r++)
        g_wall[round][r] = g_grid[r][wall_col].right;
    for (int r = g_top_level[round]+1; r <= MAX_LEVEL+1; r++) g_wall[round][r] = 0;
    return true;
}

/* -- Oracle self-check ----------------------------------------------------- */
/* The boolean sweep is only worth trusting if it agrees with enumeration. This
 * counts every colour-legal strip by brute force -- no oracle, no piece
 * disjointness, precisely the relaxation the sweep claims to decide -- and
 * checks that the two agree on which start chains are alive, chain by chain.
 * Exponential by nature, hence the cap: a small-W test. */
static double brute_count(int round, int level, uint32_t sig, double cap) {
    if (level > g_top_level[round]) {
        if (!g_close_top[round]) return 1.0;
        double n = 0.0;
        for (size_t i = 0; i < g_close_n[round]; i++)
            if (g_close[round][i].sig == sig) n += 1.0;
        return n;
    }
    if (!color_is_inner(g_wall[round][level])) return 0.0;
    const Cell *cell = s_db[(uint64_t)INNER_IDX(g_wall[round][level]) * s_nsig + sig];
    if (!cell) return 0.0;
    uint8_t bot[MAX_W]; sig_bottoms(sig, bot);
    double total = 0.0;
    for (uint32_t j = 0; j < cell->n; j++) {
        uint16_t ci[MAX_W]; int term; uint32_t succ;
        if (!rh_decode(cell, j, g_wall[round][level], bot, ci, &term, &succ)) continue;
        total += brute_count(round, level+1, succ, cap);
        if (total > cap) break;
    }
    return total;
}

static void run_selfcheck(int round, const BorderChain *bc, size_t n) {
    size_t live = 0;
    for (size_t i = 0; i < n; i++) if (live_test(g_live[round][1], bc[i].sig)) live++;
    printf("[selfcheck] W=%d levels 1..%d%s  start chains=%zu, of which the oracle calls %zu live\n",
           chain_w(), g_top_level[round], g_close_top[round] ? " +closure" : "", n, live);
    size_t bad = 0; double total = 0.0;
    for (size_t i = 0; i < n; i++) {
        bool dp = live_test(g_live[round][1], bc[i].sig);
        double bf = brute_count(round, 1, bc[i].sig, 1e7);
        total += bf;
        if (dp != (bf > 0.0)) {
            if (bad < 5)
                printf("[selfcheck] MISMATCH start %zu sig=%u: oracle=%s brute=%.0f strip(s)\n",
                       i, bc[i].sig, dp ? "live" : "dead", bf);
            bad++;
        }
    }
    printf("[selfcheck] brute force found %.0f relaxed strip(s); start chains disagreeing"
           "=%zu -> %s%s\n", total, bad, bad == 0 ? "PASS" : "FAIL",
           /* Agreeing on zero proves nothing: both walked an empty space. */
           (bad == 0 && total == 0.0)
               ? " (VACUOUS: the relaxed space is empty, so nothing was compared;"
                 " rerun on a board whose strip is feasible)" : "");
}

/* -- One round ------------------------------------------------------------- */

static void do_strip(int round) {
    if (!strip_geometry(round)) {
        if (g_verbose) printf("[round %d] no searchable strip at W=%d\n", round, g_W);
        offer_board();
        return;
    }
    oracle_alloc();
    g_stats.strips++;

    /* Where the strip sits on the board the user handed in. Frame coordinates
       are an internal convenience; nobody can act on them. */
    if (g_verbose) {
        int sr0, sr1, sc0, sc1;
        int last = g_close_top[round] ? g_top_level[round]+1 : g_top_level[round];
        box_to_orig(0, last, PUZZLE_SIDE-g_W, PUZZLE_SIDE-1, g_rot_applied,
                    &sr0, &sr1, &sc0, &sc1);
        printf("[round %d] refills the input's %s band: rows %d..%d x cols %d..%d, "
               "%d chain level(s) of %d%s\n", round, round_side(round),
               sr0, sr1, sc0, sc1, g_top_level[round], g_W,
               g_close_top[round] ? " + a top border closure" :
               g_held[round] ? "" : " (far end left open)");
        if (g_held[round])
            printf("[round %d] %d level(s) held at the far end; the strip fills up to meet "
                   "them\n", round, g_held_n[round]);
    }

    /* Closure chains first: the backward sweep is seeded from them. */
    if (g_close_top[round]) {
        enumerate_border_row(PUZZLE_SIDE-1, 0,
                             &g_close[round], &g_close_n[round], &g_close_cap[round]);
        qsort(g_close[round], g_close_n[round], sizeof(BorderChain), cmp_bc_sig);
        if (!g_close_n[round]) {
            if (round == 1 || g_verbose)
                printf("[round %d] REFUTED: no border chain can close the far end of "
                       "the strip\n", round);
            g_stats.strips_refuted++; g_stats.refuted[g_top_level[round]+1]++;
            offer_board();
            return;
        }
    } else g_close_n[round] = 0;

    double t0 = omp_get_wtime();
    oracle_seed_top(round);
    oracle_backward(round, 1, g_top_level[round]);
    parity_prepare(round);

    int collapse = 0;                    /* lowest level with no live signature */
    for (int r = 1; r <= g_top_level[round]; r++)
        if (!live_count(round, r)) { collapse = r; break; }
    if (g_verbose) {
        printf("[oracle] round %d live signatures per level:", round);
        for (int r = 1; r <= g_top_level[round]; r++)
            printf(" %d:%" PRIu64, r, live_count(round, r));
        printf("  (of %u, %.2fs)\n", s_nsig, omp_get_wtime()-t0);
    }

    enumerate_border_row(0, 2, &g_bc[round], &g_bc_n[round], &g_bc_cap[round]);
    size_t raw = g_bc_n[round];
    if (g_selfcheck) { run_selfcheck(round, g_bc[round], raw); g_stop = 1; return; }
    size_t alive = 0;
    for (size_t i = 0; i < raw; i++)
        if (live_test(g_live[round][1], g_bc[round][i].sig))
            g_bc[round][alive++] = g_bc[round][i];
    g_bc_n[round] = alive;

    /* No live start is a THEOREM, not a silent failure: the oracle ignores the
       piece supply, so if colour alone rules the strip out, no arrangement of
       any pieces can fill this band. */
    if (!alive) {
        if (round == 1 || g_verbose)
            printf("[round %d] REFUTED in %.2fs: colour alone rules this band out -- 0 of "
                   "%zu start chain(s) survive, the relaxed strip dies at level %d of %d\n",
                   round, omp_get_wtime()-t0, raw, collapse ? collapse : 1,
                   g_top_level[round]);
        g_stats.strips_refuted++;
        g_stats.refuted[collapse ? collapse : 1]++;
        offer_board();
        return;
    }
    if (g_verbose)
        printf("[round %d] %zu of %zu start chain(s) live (oracle %.2fs)\n",
               round, alive, raw, omp_get_wtime()-t0);

    for (size_t b = 0; b < alive; b++) {
        if (budget_spent()) { g_truncated = true; break; }
        const BorderChain *bc = &g_bc[round][b];
        level_place(0, bc->pid, bc->spin);
        strip_dfs(round, 1, bc->sig);
        level_remove(0);
    }
}

/* One round: bail on the budget, otherwise search this rotation's strip. */
static void run_round(int round) {
    if (round > g_rounds) return;
    if (budget_spent()) { g_truncated = true; return; }
    do_strip(round);
}

/* -- Greedy break fill (--max_breaks) -------------------------------------- */
/* One descent, no backtracking: pick a cell, place a piece, never reconsider.
 * The same idiom as the backtracker's --break-mode stuck, and deliberately kept
 * apart from the proof engine above -- a change here cannot perturb it.
 *
 * WHY IT ALWAYS COMPLETES. A cell's frame type is fixed by its position, and the
 * type counts stay balanced (4 corners, 56 edges, 196 inner), so the number of
 * unused pieces of each type always equals the number of cells still needing it.
 * The candidate list at any cell is therefore never empty -- at worst every
 * option breaks an edge.
 *
 * CELL ORDER defers breaking as long as possible: take the cell whose cheapest
 * placement costs least, and among those the one with the fewest ways to achieve
 * it -- the narrowest commitment available. */

/* Does piece `pid` at spin `spin` seat legally in cell (r,c)? Grey faces must
   fall exactly on the board's rim and nowhere else. */
static bool fits_frame(int pid, int spin, int r, int c) {
    Oriented o = rh_oriented((uint16_t)pid, (uint8_t)spin);
    return (o.top    == 0) == (r == PUZZLE_SIDE-1) &&
           (o.bottom == 0) == (r == 0) &&
           (o.left   == 0) == (c == 0) &&
           (o.right  == 0) == (c == PUZZLE_SIDE-1);
}

/* Mismatches a placement would create against the neighbours already down. */
static int placement_cost(const Oriented *o, int r, int c) {
    int cost = 0;
    if (c+1 < PUZZLE_SIDE && g_has[r][c+1] && o->right  != g_grid[r][c+1].left)   cost++;
    if (c   > 0           && g_has[r][c-1] && o->left   != g_grid[r][c-1].right)  cost++;
    if (r+1 < PUZZLE_SIDE && g_has[r+1][c] && o->top    != g_grid[r+1][c].bottom) cost++;
    if (r   > 0           && g_has[r-1][c] && o->bottom != g_grid[r-1][c].top)    cost++;
    return cost;
}

/* Fill every empty cell of the board in place and return the breaks it cost.
   It always runs to completion, even past --max_breaks: the caller wants the
   number either way, because "this board needs 61 breaks" is a far more useful
   answer than "more than 40". */
static int greedy_fill(void) {
    int spent = 0;
    for (;;) {
        int best_r = -1, best_c = -1, best_pid = -1, best_spin = 0;
        int best_cost = 5, best_ways = 0;
        for (int r = 0; r < PUZZLE_SIDE; r++)
            for (int c = 0; c < PUZZLE_SIDE; c++) {
                if (g_has[r][c]) continue;
                int need = (r == 0) + (r == PUZZLE_SIDE-1) + (c == 0) + (c == PUZZLE_SIDE-1);
                int cell_cost = 5, ways = 0, pid_at = -1, spin_at = 0;
                /* The dive respects clues too, or a --max_breaks run would hand
                   back a complete board with the clues scattered -- the exhaustive
                   half having held them for nothing. A clue cell takes only its
                   own piece at its own spin; every other cell refuses clue pieces
                   outright, which also keeps one available for its cell. */
                int clue_spin = 0;
                const ClueCell *want_clue = rh_clue_at_frame(r, c, &clue_spin);
                for (int pid = 0; pid < NUM_PIECES; pid++) {
                    if (used_test(g_placed, (uint16_t)pid)) continue;
                    if (rh_zero_count(pid) != need) continue;
                    if (want_clue ? (pid != want_clue->piece) : rh_is_clue_piece((uint16_t)pid))
                        continue;
                    for (int spin = 0; spin < 4; spin++) {
                        if (want_clue && spin != clue_spin) continue;
                        if (!fits_frame(pid, spin, r, c)) continue;
                        Oriented o = rh_oriented((uint16_t)pid, (uint8_t)spin);
                        int cost = placement_cost(&o, r, c);
                        if (cost < cell_cost) {
                            cell_cost = cost; ways = 1; pid_at = pid; spin_at = spin;
                        } else if (cost == cell_cost) ways++;
                    }
                }
                if (pid_at < 0) continue;                  /* cannot happen: see above */
                if (cell_cost < best_cost ||
                    (cell_cost == best_cost && ways < best_ways)) {
                    best_cost = cell_cost; best_ways = ways;
                    best_r = r; best_c = c; best_pid = pid_at; best_spin = spin_at;
                }
            }
        if (best_r < 0) return spent;                      /* the board is full */
        g_grid[best_r][best_c] = rh_oriented((uint16_t)best_pid, (uint8_t)best_spin);
        g_has[best_r][best_c] = true;
        take_piece((uint16_t)best_pid);
        spent += best_cost;
    }
}

/* Load a snapshot back onto the grid (snapshots are in the input's orientation,
   so the fill works there and no rotation is involved). */
static void restore_snap(const Snap *s) {
    memset(g_has, 0, sizeof g_has);
    memset(g_placed, 0, sizeof g_placed);
    g_n_placed = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (s->pos[pid] == 999) continue;
        int r = s->pos[pid]/PUZZLE_SIDE, c = s->pos[pid]%PUZZLE_SIDE;
        g_grid[r][c] = rh_oriented((uint16_t)pid, (uint8_t)s->rot[pid]);
        g_has[r][c] = true;
        used_set(g_placed, (uint16_t)pid);
        g_n_placed++;
    }
    for (int c = 0; c < NUM_COLORS_TOTAL; c++) g_avail[c] = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (used_test(g_placed, (uint16_t)pid)) continue;
        int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
        for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) g_avail[e[k]]++;
    }
    g_rot_applied = 0;
}

/* -- Input ----------------------------------------------------------------- */

static bool parse_fields(char *s, char id_out[96], int pos[NUM_PIECES], int rot[NUM_PIECES]) {
    char *tok = strtok(s, ",\r\n");
    if (!tok) return false;
    while (*tok == ' ') tok++;
    if (id_out) snprintf(id_out, 96, "%s", tok);
    tok = strtok(NULL, ",\r\n");
    if (!tok) return false;
    for (int k = 0; k < 2*NUM_PIECES; k++) {
        tok = strtok(NULL, ",\r\n");
        if (!tok) return false;
        long v = strtol(tok, NULL, 10);
        if (k < NUM_PIECES) pos[k] = (int)v; else rot[k-NUM_PIECES] = (int)v;
    }
    return true;
}

/* Read the want-th data line of a board CSV; blank and '#'/'%' comment lines do
   not count. Any leading metadata is skipped -- only the last 512 fields matter. */
static bool read_line(const char *path, uint32_t want, char id_out[96],
                      int pos[NUM_PIECES], int rot[NUM_PIECES]) {
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open boards CSV %s: %s", path, strerror(errno));
    char *line = NULL; size_t sz = 0; long idx = -1; bool found = false;
    while (getline(&line, &sz, f) > 0) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s=='\0'||*s=='\n'||*s=='\r'||*s=='#'||*s=='%') continue;
        if ((uint32_t)++idx != want) continue;
        found = parse_fields(s, id_out, pos, rot);
        if (!found) fatal("boards CSV line %u is truncated (needs %d fields)",
                          want, 2 + 2*NUM_PIECES);
        break;
    }
    free(line); fclose(f);
    return found;
}

/* Data lines in a board CSV, so the banner can say what --border_row may ask
   for. Cheap next to everything else, and it turns "0 searched, 0 emitted" into
   a sentence the reader can act on. */
static uint32_t count_data_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open boards CSV %s: %s", path, strerror(errno));
    char *line = NULL; size_t sz = 0; uint32_t n = 0;
    while (getline(&line, &sz, f) > 0) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s=='\0'||*s=='\n'||*s=='\r'||*s=='#'||*s=='%') continue;
        n++;
    }
    free(line); fclose(f);
    return n;
}

/* Core dedup: two inputs agreeing on the retained core seed an identical search,
   whatever they do outside it. Hashed after the cut and the rotation, so the key
   covers exactly what the search will see. */
static uint64_t *g_dup = NULL; static size_t g_dup_sz = 0, g_dup_n = 0;

static bool dup_insert(uint64_t key) {
    if (!key) key = 1;
    if (g_dup_n*2 >= g_dup_sz) {
        size_t ns = g_dup_sz ? g_dup_sz*2 : 4096;
        uint64_t *nt = xmalloc(ns*sizeof(uint64_t));
        memset(nt, 0, ns*sizeof(uint64_t));
        for (size_t i = 0; i < g_dup_sz; i++) {
            if (!g_dup[i]) continue;
            size_t h = (size_t)(g_dup[i]*0x9E3779B97F4A7C15ULL) & (ns-1);
            while (nt[h]) h = (h+1) & (ns-1);
            nt[h] = g_dup[i];
        }
        free(g_dup); g_dup = nt; g_dup_sz = ns;
    }
    size_t h = (size_t)(key*0x9E3779B97F4A7C15ULL) & (g_dup_sz-1);
    while (g_dup[h]) { if (g_dup[h] == key) return false; h = (h+1) & (g_dup_sz-1); }
    g_dup[h] = key; g_dup_n++;
    return true;
}

/* Fingerprint of the cut board: piece and rotation per kept cell, plus W. */
static uint64_t core_hash(void) {
    uint64_t fp = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    fp ^= (uint64_t)g_W; fp *= prime;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            uint32_t v = g_has[r][c] ? (uint32_t)g_grid[r][c].piece_id*4u + g_grid[r][c].rotation : 0xFFFFu;
            fp ^= v; fp *= prime;
        }
    return fp;
}

/* Cells the run keeps, in frame coordinates. Each round frees exactly the band
   it will refill, so the three cuts nest: --rounds N frees the first N bands and
   nothing more.

       --rounds 1   free the right band          keep 16-W cols x 16 rows
       --rounds 2   ... and the top band         keep 16-W cols x 16-W rows
       --rounds 3   ... and the left band        keep 16-2W cols x 16-W rows  */
static bool cell_kept(int r, int c, int W) {
    bool keep = (c <= PUZZLE_SIDE-1-W);                            /* round 1: right  */
    if (g_rounds >= 2) keep = keep && (r <= PUZZLE_SIDE-1-W);      /* round 2: top    */
    if (g_rounds >= 3) keep = keep && (c >= W);                    /* round 3: left   */
    return keep;
}

/* Which round's strip refills a freed cell: 1, 2 or 3. The clauses in cell_kept
   nest, so a cell belongs to the first one that frees it, and the three bands
   tile the board's freed part exactly. Only meaningful where cell_kept is false.
   --hold_band reads this to find the band the LAST round will refill. */
static int band_of(int r, int c, int W) {
    if (c > PUZZLE_SIDE-1-W) return 1;      /* right band, full height   */
    if (r > PUZZLE_SIDE-1-W) return 2;      /* top band, clipped by 1    */
    return 3;                               /* left band, clipped by 1+2 */
}

/* The same rectangle on the INPUT board -- the only form a reader can check
   against their own CSV. Same bounds as cell_kept, mapped back. */
static void core_box_orig(int W, int *r0, int *r1, int *c0, int *c1) {
    box_to_orig(0, g_rounds >= 2 ? PUZZLE_SIDE-1-W : PUZZLE_SIDE-1,
                g_rounds >= 3 ? W : 0, PUZZLE_SIDE-1-W,
                g_rot_applied, r0, r1, c0, c1);
}

/* Is the region this width would keep usable as a core? It must be complete -- a
   hole in it can never be filled, since only the strip is searched -- and
   internally break-free, because every strip is grown against its colors and one
   stale mismatch poisons the whole run. Everything OUTSIDE it is freed, so
   holes, breaks and even mis-seated pieces out there are none of our business.
   On failure, reports the offending cell in the input board's coordinates. */
static bool core_usable(int W, char why[128]) {
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (!cell_kept(r, c, W)) continue;
            int ar, ac; frame_to_input(r, c, g_rot_applied, &ar, &ac);
            if (!g_has[r][c]) {
                if (why) snprintf(why, 128, "it keeps cell (%d,%d), which is unplaced", ar, ac);
                return false;
            }
            const Oriented *o = &g_grid[r][c];
            bool fb = (r==0), ft = (r==PUZZLE_SIDE-1), fl = (c==0), fr = (c==PUZZLE_SIDE-1);
            if (rh_zero_count(o->piece_id) != (fb+ft+fl+fr) ||
                (fb && o->bottom != 0) || (ft && o->top   != 0) ||
                (fl && o->left   != 0) || (fr && o->right != 0)) {
                if (why) snprintf(why, 128, "piece %u does not fit kept cell (%d,%d)",
                                  o->piece_id, ar, ac);
                return false;
            }
            int nr = -1, nc = -1;
            if (c+1 < PUZZLE_SIDE && cell_kept(r, c+1, W) && g_has[r][c+1] &&
                g_grid[r][c].right != g_grid[r][c+1].left) { nr = r; nc = c+1; }
            else if (r+1 < PUZZLE_SIDE && cell_kept(r+1, c, W) && g_has[r+1][c] &&
                     g_grid[r][c].top != g_grid[r+1][c].bottom) { nr = r+1; nc = c; }
            if (nr < 0) continue;
            if (why) {
                int br, bc; frame_to_input(nr, nc, g_rot_applied, &br, &bc);
                snprintf(why, 128, "break inside the kept region at (%d,%d)-(%d,%d)",
                         ar, ac, br, bc);
            }
            return false;
        }
    return true;
}

/* --hold_band pre-flight, run once per board while the supply counts are still
   about to be built from scratch.
 *
 * Holding the last band only makes sense if what stands in it is something the
 * strip search can MEET: whole chain levels, legally seated, and break-free both
 * inside themselves and against the core they touch. A band that holds a partial
 * level, or one with a break in it, is not refused -- the run is still worth
 * making, it just has to start that band from nothing like any other. So the
 * band is freed, the reason is printed, and the search carries on unheld.
 *
 * Returning false is reserved for a board that cannot be searched at all. */
static bool hold_band_prepare(uint32_t line) {
    const int W = g_W;
    int n = 0, bad_r = -1, bad_c = -1; const char *why = NULL;

    /* The cut has just run, so the only cells still standing outside the core
       are the ones it deliberately kept: the last round's band. Scanning
       everything outside the core therefore scans exactly the held band. */

    for (int r = 0; r < PUZZLE_SIDE && !why; r++)
        for (int c = 0; c < PUZZLE_SIDE && !why; c++) {
            if (cell_kept(r, c, W) || !g_has[r][c]) continue;
            n++;
            const Oriented *o = &g_grid[r][c];
            bool fb = (r==0), ft = (r==PUZZLE_SIDE-1), fl = (c==0), fr = (c==PUZZLE_SIDE-1);
            if (rh_zero_count(o->piece_id) != (fb+ft+fl+fr) ||
                (fb && o->bottom != 0) || (ft && o->top   != 0) ||
                (fl && o->left   != 0) || (fr && o->right != 0)) {
                why = "a piece in it does not seat legally"; bad_r = r; bad_c = c; break;
            }
            if (c+1 < PUZZLE_SIDE && g_has[r][c+1] && o->right != g_grid[r][c+1].left) {
                why = "it holds a break"; bad_r = r; bad_c = c; break;
            }
            if (r+1 < PUZZLE_SIDE && g_has[r+1][c] && o->top != g_grid[r+1][c].bottom) {
                why = "it holds a break"; bad_r = r; bad_c = c; break;
            }
        }

    if (!why && n && n % W) why = "it holds a partial chain level";
    if (!why) {
        if (n) {
            printf("[hold] line %u: holding %d piece(s) = %d chain level(s) in the %s band; "
                   "round %d will fill up to meet them\n",
                   line, n, n / W, round_side(g_rounds), g_rounds);
        }
        return true;
    }

    int ar, ac; frame_to_input(bad_r < 0 ? 0 : bad_r, bad_c < 0 ? 0 : bad_c,
                               g_rot_applied, &ar, &ac);
    if (bad_r >= 0)
        printf("[hold] line %u: the %s band is not holdable -- %s at (%d,%d). Freeing it and "
               "searching it from nothing, as without --hold_band\n",
               line, round_side(g_rounds), why, ar, ac);
    else
        printf("[hold] line %u: the %s band is not holdable -- %s (%d piece(s), width %d). "
               "Freeing it and searching it from nothing, as without --hold_band\n",
               line, round_side(g_rounds), why, n, W);

    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            if (!cell_kept(r, c, W)) g_has[r][c] = false;
    return true;
}

/* Load one CSV line into the frame, apply --rotate, choose the strip width and
   free everything outside the core. */
static bool load_and_cut(const char *path, uint32_t line, int forced_W) {
    int pos[NUM_PIECES], rot[NUM_PIECES];
    if (!read_line(path, line, g_in_id, pos, rot)) return false;
    /* A board with no id of its own still needs one to carry. */
    if (!g_in_id[0]) snprintf(g_in_id, sizeof g_in_id, "p%u", line);

    /* --reverse: into mirror space first, so everything below -- the clue
       orientation included -- reads a board that agrees with the mirrored seed
       and the mirrored clue table. */
    if (g_reverse) mirror_line(pos, rot);

    /* Read the board's committed orientation before anything is rotated or
       freed: the clue table is in unturned coordinates, and this is the only
       moment the board is still in them. */
    g_rh_orient = g_clue_mask ? rh_clue_orient_of(pos, rot) : -1;
    if (g_clue_mask && g_rh_orient < 0) {
        printf("[skip] line %u: carries none of the enabled clue pieces, so there is\n"
               "       nothing to read the board's orientation from\n", line);
        return false;
    }

    memset(g_has, 0, sizeof g_has);
    memset(g_placed, 0, sizeof g_placed);
    g_rot_applied = 0;
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (pos[pid] == 999) continue;
        if (pos[pid] < 0 || pos[pid] >= NUM_PIECES || rot[pid] < 0 || rot[pid] > 3) {
            printf("[skip] line %u: piece %d has pos %d / rot %d out of range\n",
                   line, pid, pos[pid], rot[pid]);
            return false;
        }
        int r = pos[pid]/PUZZLE_SIDE, c = pos[pid]%PUZZLE_SIDE;
        if (g_has[r][c]) {
            printf("[skip] line %u: two pieces on cell (%d,%d)\n", line, r, c);
            return false;
        }
        g_grid[r][c] = rh_oriented((uint16_t)pid, (uint8_t)rot[pid]);
        g_has[r][c] = true;
    }

    for (int i = 0; i < (g_rotate & 3); i++) rotate_cw();
    g_rot_applied = g_rotate & 3;

    /* Width. An explicit --strip_width is validated as given; --strip_width 0
       takes the narrowest usable one, which keeps the most proven structure. */
    char why[128] = "";
    if (forced_W) {
        g_W = forced_W;
        if (!core_usable(g_W, why)) {
            printf("[skip] line %u: --strip_width %d is unusable -- %s\n", line, g_W, why);
            return false;
        }
    } else {
        int chosen = 0;
        for (int W = 2; W <= MAX_W && !chosen; W++)
            if (core_usable(W, NULL)) chosen = W;
        if (!chosen) {
            core_usable(MAX_W, why);
            printf("[skip] line %u: no strip width 2..%d leaves a complete, break-free "
                   "core (at W=%d, %s). Aim --rotate at the side the breaks are on.\n",
                   line, MAX_W, MAX_W, why);
            return false;
        }
        g_W = chosen;
    }

    /* Free everything outside the core -- except, under --hold_band, whatever is
       already standing in the band the LAST round refills. Those pieces are the
       previous pass's work at the far end of this pass's strip, and holding them
       is the whole point of the flag: the search fills up to MEET them. */
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++) {
            if (cell_kept(r, c, g_W)) continue;
            if (g_hold_band && g_has[r][c] && band_of(r, c, g_W) == g_rounds) continue;
            g_has[r][c] = false;
        }

    if (g_hold_band && !hold_band_prepare(line)) return false;

    /* Clue viability. A clue whose cell survives the cut is never re-placed by
       any strip, so if it is wrong there it is wrong for good and searching this
       board is wasted work. The centre clue is always in this case -- the core
       spans the middle of the board at every W -- which is why --clue_center
       verifies here rather than pinning anything. */
    for (int k = 0; k < CLUE_N && g_clue_mask; k++) {
        if (!clue_on(k)) continue;
        const ClueCell *cc = &g_rh_clue[g_rh_orient][k];
        int R, C, S;
        orig_to_frame(cc->row, cc->col, cc->spin, g_rot_applied, &R, &C, &S);
        if (!g_has[R][C]) continue;                 /* freed: a strip will pin it */
        if (g_grid[R][C].piece_id != cc->piece || g_grid[R][C].rotation != (uint8_t)S) {
            /* The cell is named from the UNMIRRORED table, so the message points
               at the board the user handed in rather than at mirror space. */
            const ClueCell *say = &g_clue[g_rh_orient][k];
            printf("[skip] line %u: clue %d of orientation %d needs piece %u at board cell\n"
                   "       (%d,%d), but that cell is inside the retained core holding piece %u,\n"
                   "       and no strip can re-place it\n",
                   line, k, g_rh_orient, cc->piece, say->row, say->col, g_grid[R][C].piece_id);
            return false;
        }
    }

    g_n_placed = 0;
    memset(g_placed, 0, sizeof g_placed);
    for (int c = 0; c < NUM_COLORS_TOTAL; c++) g_avail[c] = 0;
    for (int r = 0; r < PUZZLE_SIDE; r++)
        for (int c = 0; c < PUZZLE_SIDE; c++)
            if (g_has[r][c]) { used_set(g_placed, g_grid[r][c].piece_id); g_n_placed++; }
    for (int pid = 0; pid < NUM_PIECES; pid++) {
        if (used_test(g_placed, (uint16_t)pid)) continue;
        int e[4] = { g_seed_top[pid], g_seed_right[pid], g_seed_bottom[pid], g_seed_left[pid] };
        for (int k = 0; k < 4; k++) if (color_is_inner(e[k])) g_avail[e[k]]++;
    }
    return true;
}

/* -- Summary ---------------------------------------------------------------- */

static void print_summary(double wall) {
    printf("\n================= run summary =================\n");
    printf("[sum] wall %.1fs   threads %d   deterministic: this tool holds no randomness\n",
           wall, g_nthreads);
    printf("[sum] input: %u line(s) read -> %u board(s) searched, %u duplicate core(s), "
           "%u unusable\n", g_stats.lines_read, g_stats.lines_used,
           g_stats.lines_dup, g_stats.lines_bad);
    printf("[sum] proof: %u board(s) searched EXHAUSTIVELY, %u stopped by a budget\n",
           g_stats.lines_exhausted, g_stats.lines_truncated);
    printf("[sum] strips: %u searched, %u refuted by the oracle before any piece was "
           "tried (%" PRIu64 " node(s))\n",
           g_stats.strips, g_stats.strips_refuted, g_stats.nodes);
    if (g_stats.strips_refuted) {
        printf("[sum] oracle refutations, by the level colour alone dies at: ");
        for (int r = 0; r <= MAX_LEVEL+1; r++)
            if (g_stats.refuted[r]) printf(" L%d:%u", r, g_stats.refuted[r]);
        putchar('\n');
    }
    printf("[sum] emitted %" PRIu64 " board(s): %" PRIu64 " solved, %" PRIu64
           " deepest, %" PRIu64 " break-filled\n", g_emitted,
           g_stats.emit_solved, g_stats.emit_deepest, g_stats.emit_filled);
    if (g_out_fp[OUT_CLEAN])
        printf("[sum]   break-free  -> %s\n", g_out_path[OUT_CLEAN]);
    if (g_out_fp[OUT_BROKEN])
        printf("[sum]   with breaks -> %s\n", g_out_path[OUT_BROKEN]);
    if (g_stats.emit_deepest)
        printf("[sum] a break-free partial scores 480 minus the junctions its EMPTY cells "
               "leave open, never a mismatch: compare it with another partial, not with the "
               "complete board you fed in.\n");
    if (g_stats.lines_exhausted && !g_stats.emit_solved)
        printf("[sum] every exhaustively searched board is PROVED to admit no break-free "
               "refill of these bands. Change the geometry (--rotate, --strip_width, "
               "--rounds), or take a complete board with --max_breaks.\n");
    if (g_stats.lines_truncated)
        printf("[sum] the truncated boards are NOT proofs. Raise the budget, or narrow the "
               "cut: --rounds 3 --strip_width 5 keeps only 66 pieces and is the one cut too "
               "wide to exhaust.\n");
    fflush(stdout);
}

/* -- Usage and entry point -------------------------------------------------- */

static const char *k_usage =
"E555_roundhouse -- exhaustive strip solver over a width-W chain database\n"
"\n"
"  bin/E555_roundhouse seed.txt boards.csv [options]\n"
"\n"
"GEOMETRY -- what gets freed and refilled\n"
"  --rounds N             1..3 W-wide bands to free and refill (default 3):\n"
"                         right, then top, then left IN THE FRAME\n"
"  --strip_width W        2..5, the chain length and the band width (default 5;\n"
"                         0 = the narrowest width with a usable core)\n"
"  --rotate K             -3..3 quarter-turns before the cut (default 1, which\n"
"                         makes round 1 the INPUT's top band)\n"
"  --reverse              spiral the other way round: right/top/left becomes\n"
"                         left/top/right on the input board. Implemented by\n"
"                         mirroring the seed, so the strip is traversed the\n"
"                         other way over the same cells -- a second exhaustive\n"
"                         attack, not a new region\n"
"  --stop_row R           stop each strip at level R instead of its last\n"
"  --hold_band            do not free what is already standing in the band the LAST\n"
"                         round refills. Those pieces must form complete chain levels\n"
"                         stacked at the far end of the strip -- the shape the other\n"
"                         spiral leaves behind, having filled that band from the\n"
"                         opposite side until it stopped -- and the search then fills\n"
"                         up to MEET them instead of starting the band from nothing.\n"
"                         This is what lets CCW and CW compound: run one, feed its\n"
"                         board to the other with this flag, and the second pass keeps\n"
"                         the first pass's work instead of tearing it out. A band that\n"
"                         holds a partial level, or a gap under the block, is refused\n"
"                         rather than silently freed. Ignored by rounds 1..N-1, whose\n"
"                         bands are freed and re-searched as usual (default off)\n"
"  --BL/--BR/--TL/--TR P  pin a corner piece by its role on the INPUT board\n"
"\n"
"SEARCH -- exhaustive unless a budget bites\n"
"  --max_breaks B         after the exhaustive search, greedily fill the rest of\n"
"                         the deepest board, spending at most B mismatches\n"
"                         (default 0 = off, so the output stays break-free)\n"
"  --clue_center          verify the published centre clue is on its cell. The core\n"
"                         spans the middle of the board at every width, so this tool\n"
"                         never frees that cell -- the flag checks, it cannot pin\n"
"  --clue_corners         hold the four corner clues on their cells and spins. These\n"
"                         DO fall in the freed bands, and without the flag a --rounds 3\n"
"                         cut re-places all four somewhere else. Both flags read the\n"
"                         orientation off each input board and skip a board whose core\n"
"                         already contradicts a clue; --max_breaks respects them too\n"
"  --max_nodes N          node budget per input board (0 = unlimited)\n"
"  --config_time_sec S    wall-clock budget per input board (default 600)\n"
"  --max_wall_sec S       wall-clock budget for the whole run (0 = unlimited)\n"
"  --max_boards N         stop after N boards have been written\n"
"\n"
"INPUT / OUTPUT\n"
"  --border_row N         first data line of the boards CSV (default 0)\n"
"  --border_row_N N       consecutive lines to process (default 1)\n"
"  --out_dir DIR          output directory (default round_out). Names carry the\n"
"                         geometry, and break-free boards are filed separately\n"
"                         from break-bought ones: ..._W5_miss0.csv and _miss12.csv\n"
"  --ties N               boards to emit at the deepest reach (default 1). Extra\n"
"                         ones are kept only when they differ by more than a\n"
"                         single frontier piece\n"
"  --only_complete        emit only boards with all 256 pieces placed\n"
"\n"
"MISC\n"
"  --threads N            OpenMP threads (used by the oracle sweep)\n"
"  --selfcheck            check the oracle against brute force, then exit\n"
"  --verbose              per-round geometry, the oracle's live counts, the\n"
"                         database build line\n"
"\n"
"RETIRED, accepted with a warning so older scripts keep running: --beam_width,\n"
"--mode, --frac_rand, --repeats, --finalize_repeats, --lambda_Mahalanobis,\n"
"--top_bottoms, --emit_each_round, --emit_deepest, --seed, --max_partials,\n"
"--free_edges. The search is exhaustive and deterministic, so none of them have\n"
"anything left to do, and the deepest board is emitted by default.\n";

/* --BL/--BR/--TL/--TR -> the role index used by g_pin_corner. */
static int corner_index(const char *flag) {
    if (!strcmp(flag, "--BL")) return 0;
    if (!strcmp(flag, "--BR")) return 1;
    if (!strcmp(flag, "--TL")) return 2;
    if (!strcmp(flag, "--TR")) return 3;
    return -1;
}

/* Flags this version no longer has. Accepting them keeps a cluster script from
   dying on a stale argument, and the warning says why they went. */
static bool retired_flag(const char *a, int *i, int argc) {
    static const char *takes_value[] = {
        "--beam_width", "--mode", "--frac_rand", "--repeats", "--finalize_repeats",
        "--lambda_Mahalanobis", "--top_bottoms", "--emit_deepest", "--seed",
        "--max_partials", NULL };
    static const char *no_value[] = { "--emit_each_round", "--free_edges", NULL };
    for (int k = 0; takes_value[k]; k++)
        if (!strcmp(a, takes_value[k])) {
            if (*i+1 < argc) (*i)++;
            printf("[warn] %s was retired: the search is exhaustive and deterministic, so "
                   "it has nothing left to do. Ignored.\n", a);
            return true;
        }
    for (int k = 0; no_value[k]; k++)
        if (!strcmp(a, no_value[k])) {
            printf("[warn] %s was retired: the border is always free and the deepest "
                   "board is emitted by default. Ignored.\n", a);
            return true;
        }
    return false;
}

/* Parse the CLI, set up the seed and catalog once, then for each input line:
   load and cut, skip duplicate cores, build the width-W database for what is
   left, search the spiral exhaustively, and report the furthest it got. */
int main(int argc, char **argv) {
    if (argc < 3) { fputs(k_usage, stdout); return argc < 2 ? 1 : 0; }
    const char *seed_path = argv[1];
    const char *csv_path  = argv[2];
    int nthreads = 0;

    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        int ci = corner_index(a);
        if (ci >= 0 && i+1 < argc) { g_pin_corner[ci] = atoi(argv[++i]); }
        else if (retired_flag(a, &i, argc)) { /* warned above */ }
        else if (!strcmp(a, "--out_dir") && i+1 < argc) g_out_dir = argv[++i];
        else if (!strcmp(a, "--border_row") && i+1 < argc) g_line_first = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--border_row_N") && i+1 < argc) g_line_count = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--strip_width") && i+1 < argc) g_opt_W = atoi(argv[++i]);
        else if (!strcmp(a, "--rounds") && i+1 < argc) g_rounds = atoi(argv[++i]);
        else if (!strcmp(a, "--rotate") && i+1 < argc) g_rotate = atoi(argv[++i]);
        else if (!strcmp(a, "--reverse"))             g_reverse = true;
        else if (!strcmp(a, "--stop_row") && i+1 < argc) g_stop_level = atoi(argv[++i]);
        else if (!strcmp(a, "--hold_band")) g_hold_band = true;
        else if (!strcmp(a, "--max_breaks") && i+1 < argc) g_max_breaks = atoi(argv[++i]);
        else if (!strcmp(a, "--clue_center"))  g_clue_mask |= CLUE_CENTER;
        else if (!strcmp(a, "--clue_corners")) g_clue_mask |= CLUE_CORNERS;
        else if (!strcmp(a, "--max_nodes") && i+1 < argc) g_max_nodes = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--ties") && i+1 < argc) g_ties = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--only_complete")) g_only_complete = true;
        else if (!strcmp(a, "--config_time_sec") && i+1 < argc) g_config_time_sec = atof(argv[++i]);
        else if (!strcmp(a, "--max_wall_sec") && i+1 < argc) g_max_wall_sec = atof(argv[++i]);
        else if (!strcmp(a, "--max_boards") && i+1 < argc) g_max_boards = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--threads") && i+1 < argc) nthreads = atoi(argv[++i]);
        else if (!strcmp(a, "--selfcheck")) g_selfcheck = true;
        else if (!strcmp(a, "--verbose")) g_verbose = true;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { fputs(k_usage, stdout); return 0; }
        else fatal("unknown option %s (try --help)", a);
    }
    if (g_rounds < 1 || g_rounds > MAX_ROUNDS) fatal("--rounds must be 1..%d", MAX_ROUNDS);
    if (g_opt_W && (g_opt_W < 2 || g_opt_W > MAX_W)) fatal("--strip_width must be 0 or 2..%d", MAX_W);
    if (g_rotate < -3 || g_rotate > 3) fatal("--rotate must be -3..3");
    g_rotate = (g_rotate + 4) & 3;      /* -1 == 3, one turn anticlockwise */
    if (g_ties < 1) fatal("--ties must be >= 1");
    if (g_hold_band && g_stop_level >= 0)
        fatal("--hold_band and --stop_row both set where the strip ends: the held "
              "block already fixes it. Drop one.");
    if (g_max_breaks < 0) fatal("--max_breaks must be >= 0");

    if (nthreads > 0) { omp_set_num_threads(nthreads); g_nthreads = nthreads; }
    else g_nthreads = omp_get_max_threads();
    signal(SIGINT, handle_stop);
    signal(SIGTERM, handle_stop);

    /* The names carry the settings that decide what a board IS, so runs with
       different geometry never land in one file and a directory of results reads
       without the logs. W is "auto" when --strip_width 0 lets each input board
       pick its own; miss0 always holds the break-free boards. */
    char wtag[8];
    if (g_opt_W) snprintf(wtag, sizeof wtag, "%d", g_opt_W);
    else         snprintf(wtag, sizeof wtag, "auto");
    const char *rtag = g_reverse ? "rev" : "";
    snprintf(g_out_path[OUT_CLEAN], sizeof g_out_path[0],
             "%s/roundhouse_round%d_rot%d%s_W%s_miss0.csv",
             g_out_dir, g_rounds, g_rotate, rtag, wtag);
    snprintf(g_out_path[OUT_BROKEN], sizeof g_out_path[0],
             "%s/roundhouse_round%d_rot%d%s_W%s_miss%d.csv",
             g_out_dir, g_rounds, g_rotate, rtag, wtag, g_max_breaks);

    uint32_t csv_lines = count_data_lines(csv_path);
    char sbuf[24];
    if (g_stop_level >= 0) snprintf(sbuf, sizeof sbuf, "%d", g_stop_level);
    else                   snprintf(sbuf, sizeof sbuf, "last");

    /* Banner first, as the beamer does, so the [init] lines the shared database
       module prints land UNDER the settings that produced them. Every setting is
       echoed, in four lines rather than six -- and the one thing no reader can
       infer, which side of THEIR board each round tears up, is the [plan] line. */
    printf("\n=== E555 roundhouse ===\n\n");
    printf("[cfg] seed=%s boards=%s out_dir=%s\n", seed_path, csv_path, g_out_dir);
    printf("[cfg] rounds=%d rotate=%d reverse=%d strip_width=%s stop_row=%s ties=%u "
           "only_complete=%d hold_band=%d\n", g_rounds, g_rotate, g_reverse?1:0, wtag, sbuf,
           g_ties, g_only_complete?1:0, g_hold_band?1:0);
    printf("[cfg] max_breaks=%d max_nodes=%" PRIu64 " config_time=%.0fs max_wall=%.0fs "
           "max_boards=%" PRIu64 " threads=%d\n", g_max_breaks, g_max_nodes,
           g_config_time_sec, g_max_wall_sec, g_max_boards, g_nthreads);
    printf("[cfg] border_row=%u border_row_N=%u of %u data line(s)  "
           "corners BL/BR/TL/TR=%d/%d/%d/%d\n", g_line_first, g_line_count, csv_lines,
           g_pin_corner[0], g_pin_corner[1], g_pin_corner[2], g_pin_corner[3]);
    if (g_clue_mask)
        printf("[cfg] clue_center=%d clue_corners=%d (orientation read from each input board;\n"
               "      the centre always survives the cut, so --clue_center only verifies it)\n",
               (g_clue_mask & CLUE_CENTER) ? 1 : 0, (g_clue_mask & CLUE_CORNERS) ? 1 : 0);
    printf("[plan] rounds refill the input's %s", round_side(1));
    for (int rd = 2; rd <= g_rounds; rd++) printf(" -> %s", round_side(rd));
    printf(" band; a break outside them makes a board unusable%s\n",
           (g_rounds == 3 && g_opt_W == 5)
               ? ". This cut keeps only 66 pieces: expect a budget, not a proof" : "");
    if (g_hold_band)
        printf("[plan] --hold_band: the %s band keeps whatever is already standing in\n"
               "       it, and round %d fills up to meet it. Rounds 1..%d free their\n"
               "       bands and re-search them as usual\n",
               round_side(g_rounds), g_rounds, g_rounds-1);
    if (g_line_first >= csv_lines)
        printf("[warn] --border_row %u is past the end of the CSV: nothing to do\n", g_line_first);
    fflush(stdout);

    g_free_edges = true;                 /* the roundhouse always re-chooses its border */
    load_seed_and_catalog(seed_path);
    if (g_reverse) mirror_seed();        /* before anything is derived from it */
    init_clue_table();
    build_catalog_indices();
    build_inner_color_totals();
    if (g_clue_mask) check_frame_maps();

    for (int k = 0; k < 4; k++)
        if (g_pin_corner[k] >= 0) {
            if (g_pin_corner[k] >= NUM_PIECES || rh_zero_count(g_pin_corner[k]) != 2)
                fatal("--%s %d is not a corner piece", k_corner_name[k], g_pin_corner[k]);
        }

    ensure_dir(g_out_dir);
    printf("[out] break-free -> %s\n", g_out_path[OUT_CLEAN]);
    if (g_max_breaks > 0) printf("[out] with breaks -> %s\n", g_out_path[OUT_BROKEN]);
    fflush(stdout);

    g_t_start = omp_get_wtime();

    for (uint32_t li = g_line_first; li < g_line_first + g_line_count; li++) {
        if (li >= csv_lines || g_stop) break;
        if (g_max_wall_sec > 0.0 && omp_get_wtime() - g_t_start >= g_max_wall_sec) break;
        if (g_max_boards > 0 && g_emitted >= g_max_boards) break;
        g_stats.lines_read++;
        if (!load_and_cut(csv_path, li, g_opt_W)) { g_stats.lines_bad++; continue; }
        if (!dup_insert(core_hash())) {
            printf("[skip] line %u: duplicate core -- an earlier line keeps exactly the "
                   "same pieces here, so it would seed an identical search\n", li);
            g_stats.lines_dup++;
            continue;
        }
        g_stats.lines_used++;
        g_line_id = li;

        /* Sizes depend on W, which is per line: (re)size the index space. */
        s_nsig = MAX_EDGE_SIDE_COLOR;
        for (int i = 0; i < chain_w() - 1; i++) s_nsig *= DIM_INNER;
        s_ncell = (uint64_t)DIM_INNER * s_nsig;
        if (s_db) { free(s_db); s_db = NULL; }

        memcpy(g_db_exclude, g_placed, sizeof g_db_exclude);
        build_chain_db();

        if (g_verbose) {
            int kr0, kr1, kc0, kc1;
            core_box_orig(g_W, &kr0, &kr1, &kc0, &kc1);
            printf("[core] line %u: W=%d keeps %d piece(s) -- the input's rows %d..%d x "
                   "cols %d..%d -- and frees %d; db=%" PRIu64 " chain(s)\n",
                   li, g_W, g_n_placed, kr0, kr1, kc0, kc1,
                   NUM_PIECES - g_n_placed, s_records);
            fflush(stdout);
        }
        int core_kept = g_n_placed;

        Snap core; snapshot(&core);              /* the fallback, and the fill's base */
        best_reset();
        g_nodes = 0; g_truncated = false;
        g_t_config = omp_get_wtime();
        run_round(1);
        g_stats.nodes += g_nodes;
        if (g_selfcheck) break;

        if (g_truncated) g_stats.lines_truncated++; else g_stats.lines_exhausted++;
        printf("[line %u] W=%d core=%d -> deepest %d/%d in %u board(s), %.1fM nodes, "
               "%.1fs, %s\n", li, g_W, core_kept,
               g_best_depth < 0 ? core.placed : g_best_depth, NUM_PIECES, g_best_n,
               (double)g_nodes/1e6, omp_get_wtime()-g_t_config,
               g_truncated ? "TRUNCATED (a budget, not a proof)" : "EXHAUSTED (a proof)");

        /* The break-free result: one board, or --ties of them. A reach no deeper
           than the core itself carries nothing the input did not already have. */
        bool any_out = false;
        for (uint32_t k = 0; k < g_best_n; k++) {
            if (g_best[k].placed <= core.placed) continue;
            emit_snap(&g_best[k], g_best[k].placed == NUM_PIECES ? "s" : "d");
            any_out = true;
        }
        if (!any_out)
            printf("[line %u] nothing to emit: not one chain level was filled\n", li);

        /* And, if asked, a complete board bought with breaks. */
        if (g_max_breaks > 0) {
            restore_snap(g_best_n ? &g_best[0] : &core);
            int spent = greedy_fill();
            if (spent > g_max_breaks)
                printf("[line %u] fill needs %d break(s), over --max_breaks %d: not written\n",
                       li, spent, g_max_breaks);
            else {
                Snap filled; snapshot(&filled);
                emit_snap(&filled, "f");
            }
        }
    }

    print_summary(omp_get_wtime()-g_t_start);
    for (int k = 0; k < 2; k++) if (g_out_fp[k]) fclose(g_out_fp[k]);
    return 0;
}
