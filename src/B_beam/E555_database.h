/*
 * E555_database.h -- E555 5-5-5 single-database module: public interface.
 *
 * This module owns everything that is NOT specific to the beam search: the
 * seed/catalog, the oriented-piece tables, the edge/border pools, the border
 * (bottom-row and left-column) enumerations, and -- the heart of the design --
 * a SINGLE 5-piece chain database, DB_5pieces, reused for all three horizontal
 * segments of every inner row.
 *
 * THE 5-5-5 DECOMPOSITION
 *   Every inner row is split into a fixed left-edge column and three 5-cell
 *   segments:
 *       col 0        cols 1-5        cols 6-10       cols 11-15
 *     [left edge]  [seg A: 5 in]  [seg B: 5 in]  [seg C: 4 in + right edge]
 *   The left-edge column (col 0) is fixed by the border configuration; the
 *   right edge (col 15) emerges automatically from the database (see below).
 *
 * THE SINGLE DATABASE
 *   DB_5pieces[left_color][b1][b2][b3][b4][b5] is a 6-D direct-index pointer
 *   array of packed chain records (a NULL cell = no legal chain for that
 *   key). The key is the inner color exposed to the LEFT of the chain's first
 *   piece, plus the five colors the chain exposes toward the BOTTOM. A record is
 *   either five inner pieces (when b5 is an inner color) OR four inner pieces and
 *   a final frame-right edge piece (when b5 is an edge-interface color). The two
 *   families never share a cell because inner colors (6..22) and edge-interface
 *   colors (1..5) are disjoint, so the lookup site always knows which decode to
 *   use from b5 alone.
 *
 *   The same array serves all three segments by feeding the left-neighbour's
 *   exposed right color as the key's left_color:
 *     seg A: left_color = (col-0 left-edge piece).right,  b = bottoms[1..5]
 *     seg B: left_color = (seg A last piece).right,        b = bottoms[6..10]
 *     seg C: left_color = (seg B last piece).right,        b = bottoms[11..15]
 *   In seg C, b5 = bottoms[15] is an edge-interface color (seeded by the
 *   bottom-right corner's top and perpetuated by each right edge's exposed top),
 *   so that cell holds only 4-inner+edge chains -- the right edge appears for
 *   free, with no separate right database.
 *
 * BUILD COST AND THE DISK CACHE
 *   Inner cells (b5 inner) are border-row-independent and built ONCE (the
 *   dominant arena, several GB). Because they depend only on the seed file,
 *   they can be cached on disk with --db_file: the first run builds, sorts and
 *   writes the arena; later runs mmap it back in seconds (db_cache_load /
 *   db_cache_save). Edge cells (b5 edge-interface) depend on which edges may
 *   terminate a chain, decided by --free_edges: free mode admits all 56
 *   non-corner edges (border-independent -> built once per run), fixed mode
 *   admits only the border row's pre-rotated right edges (built per border row,
 *   tiny). A bottom/left ORDERING change never rebuilds anything.
 *
 * THE FAN-OUT TABLE
 *   g_fanout[b1][b2][b3][b4][b5raw] caches, for every 5-color bottom signature,
 *   the total number of DB records summed over all 17 possible left neighbours.
 *   It is the left-agnostic "how continuable are these five exposed tops"
 *   measure used by the in-cell promise sort, the border ranking, and the beam's
 *   one-row lookahead -- replacing 17 scattered pointer dereferences per query
 *   with one lookup into a 15 MB table.
 *
 * See E555_beamer.c for the Stage B search architecture.
 */

#ifndef E555_DATABASE_V1_H
#define E555_DATABASE_V1_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* -- Constants ----------------------------------------------------------- */

#define PUZZLE_SIDE         16
#define NUM_PIECES          256
#define EDGE_LEN            14                       /* inner cells per row / side */
#define EXPECTED_INNER      196
#define CATALOG_SIZE        (EXPECTED_INNER * 4)     /* 784 oriented inner pieces */
#define NUM_COLORS_TOTAL    23
#define COLOR_MIN           6
#define COLOR_MAX           22
#define MAX_EDGE_SIDE_COLOR 5
#define NUM_EDGE_COLORS     (MAX_EDGE_SIDE_COLOR + 1)
#define NUM_INNER_COLORS    (COLOR_MAX - COLOR_MIN + 1)   /* 17 */
#define DIM_INNER           NUM_INNER_COLORS              /* 17 */
#define DIM_EDGE            NUM_EDGE_COLORS               /* 6  */

#define CHAIN_LEN           5     /* pieces in one DB chain / one row segment */
#define NUM_SEG             3     /* segments per inner row (A, B, C) */
#define DIM_B5              NUM_COLORS_TOTAL  /* 5th bottom: inner OR edge-iface, raw color */

/* Record format: each chain field is an index into the tiny (left_color,
 * bottom_color) bucket of the catalog (max ~7 pieces), so a field is g_lb_bits
 * (~3) wide. Record widths are chosen at build time from the actual bucket
 * sizes; see g_lb_bits / g_term_bits / g_rec_bytes_inner / g_rec_bytes_edge. */
#define MAX_LB_BUCKET       64    /* cap on a single (left,bottom) bucket (assert) */
#define MAX_EDGE_TERMINALS  56    /* non-corner edge pieces (free mode upper bound) */
#define SOFT_CENTER_139_PIECE  139

/* Stop-row ceiling. DB_5pieces is NOT dead-end pruned (a 5-chain's tops do not
   determine the next cell -- that needs the next row's left-neighbour color), so
   there is no row-14 under-representation: rows 1..14 are all buildable. Rows
   14-15 belong to Stage C by design, so we keep stop_row <= 13. */
#define MAX_DRILL_DEPTH     (EDGE_LEN - 1)   /* 13 */

#define INNER_IDX(c)        ((c) - COLOR_MIN)

#define NCELL_DB ((uint64_t)DIM_INNER*DIM_INNER*DIM_INNER*DIM_INNER*DIM_INNER*DIM_B5)

/* Fan-out table: one slot per (b1,b2,b3,b4) inner-color suffix x b5 raw color.
   Also the per-left_color stride of the flat DB index (la is the outermost key
   dimension), which build_fanout exploits. */
#define FANOUT_N ((uint64_t)DIM_INNER*DIM_INNER*DIM_INNER*DIM_INNER*DIM_B5)

/* -- Core typedefs -------------------------------------------------------- */

typedef struct {
    uint16_t piece_id;
    uint8_t  rotation;
    uint8_t  top, right, bottom, left;
} Oriented;

/* One DB cell: a flat array of n packed chain records. Record stride is
 * g_rec_bytes_inner for inner cells, g_rec_bytes_edge for edge cells (set at
 * build time). */
typedef struct {
    uint32_t n;
    uint32_t _pad;
    uint8_t  rec[];
} Cell;

/* Byte stride from one Cell to the next in an arena: the header plus n packed
 * records, rounded up so every Cell keeps its own 4-byte alignment. The records
 * themselves are read and written by rec_load/rec_store, which go through
 * memcpy and do not care where they land -- but `n` is a uint32_t, so without
 * this round-up a cell holding an odd number of 2-byte records (which is what
 * both shipped seeds produce: g_lb_bits=3 -> g_rec_bytes_inner=2) leaves the
 * NEXT cell at 2 mod 4, and reading its `n` is a misaligned load.
 *
 * Every arena size calculation, construction loop, cache write and cache load
 * has to use this one function: they all encode the same layout, and an offset
 * computed one way against an arena laid out the other way points into the
 * middle of a record. */
static inline uint64_t cell_stride(uint32_t n, int rec_bytes) {
    uint64_t b = sizeof(Cell) + (uint64_t)n * (uint64_t)rec_bytes;
    return (b + _Alignof(Cell) - 1) / _Alignof(Cell) * _Alignof(Cell);
}

typedef struct { uint64_t s[2]; } RNG;

/* One enumerated bottom-row ordering (p[0]=cBL .. p[15]=cBR). rtop0[c] is that
   row's exposed top color at column c; used[] covers the 14 bottom edges plus
   cBL and cBR; rank is the row-1 fan-out used to pick the most productive ones. */
typedef struct {
    const Oriented *p[PUZZLE_SIDE];
    int             rtop0[PUZZLE_SIDE];
    uint64_t        used[4];
    double          rank;
} BottomOrder;

/* One enumerated left-column ordering (p[0]=cBL .. p[15]=cTL). right[r] is the
   inner color the col-0 piece at row r exposes to its right (= seg A left_color
   for that row); used[] covers the 14 left edges plus cBL and cTL. */
typedef struct {
    const Oriented *p[PUZZLE_SIDE];
    int             right[PUZZLE_SIDE];
    uint64_t        used[4];
    double          rank;
} LeftOrder;

/* -- Shared globals (defined in E555_database.c) ----------------------- */

extern int g_seed_top[NUM_PIECES];
extern int g_seed_right[NUM_PIECES];
extern int g_seed_bottom[NUM_PIECES];
extern int g_seed_left[NUM_PIECES];

extern int g_inner_ids[EXPECTED_INNER];
extern int g_num_inner;

extern uint64_t g_seed_file_hash;   /* fnv1a of the seed file bytes (db cache key) */

extern Oriented g_cat[CATALOG_SIZE];
extern int      g_cat_count;

/* Oriented inner pieces bucketed by their LEFT color; g_cat_to_local maps a
   catalog index to its position inside its own left bucket. */
extern int g_left_bucket[NUM_COLORS_TOTAL][CATALOG_SIZE];
extern int g_left_count[NUM_COLORS_TOTAL];
extern int g_cat_to_local[CATALOG_SIZE];

/* Oriented inner pieces bucketed by their (LEFT, BOTTOM) colors. A chain
   record stores, per piece, the piece's index WITHIN this tiny bucket (both the
   left color, from chaining, and the bottom color, from the cell key, are known
   at decode time). g_cat_to_lb_local maps a catalog index to that position. */
extern int g_lb_bucket[NUM_COLORS_TOTAL][NUM_COLORS_TOTAL][MAX_LB_BUCKET];
extern int g_lb_count[NUM_COLORS_TOTAL][NUM_COLORS_TOTAL];
extern int g_cat_to_lb_local[CATALOG_SIZE];

/* Record field widths and byte sizes, chosen at build time from the bucket sizes.
   g_lb_bits: bits per inner-piece field. g_term_bits: bits for the edge-terminal
   field (index within its per-left terminal bucket). Inner record = 5*g_lb_bits
   bits; edge record = 4*g_lb_bits + g_term_bits bits. Both asserted <= 32. */
extern int g_lb_bits, g_term_bits;
extern int g_rec_bytes_inner, g_rec_bytes_edge;

extern uint8_t  g_spin[NUM_PIECES];
extern Oriented g_bottom_pool[EDGE_LEN];
extern int      g_bottom_count;
extern Oriented g_left_pool[EDGE_LEN];
extern int      g_left_count_pool;

extern Oriented g_cBL, g_cBR, g_cTL, g_cTR;
extern bool     g_has_cBL, g_has_cBR, g_has_cTL, g_has_cTR;

/* Optional user-pinned corners for --random_edges (indexed by role 0=BL 1=BR
   2=TL 3=TR); a seed piece index 0..255, or -1 for a free (randomly sampled)
   role. Set by the beamer CLI, resolved/validated by finalize_fixed_corners. */
extern int      g_fixed_corner_pid[4];

/* Edge-terminal pool: every edge piece that may END a chain, oriented
   frame-right (frame on the right, exposing an edge-interface top/bottom and an
   inner left). Free mode: all non-corner edges. Fixed mode: the border row's
   right edges only. Bucketed by the inner LEFT color it must match. */
extern Oriented g_edge_term[MAX_EDGE_TERMINALS];
extern int      g_edge_term_count;
extern int      g_edge_term_by_left[NUM_COLORS_TOTAL][MAX_EDGE_TERMINALS];
extern int      g_edge_term_by_left_n[NUM_COLORS_TOTAL];

/* The one database. Inner cells (b5 inner) live in the permanent arena (or the
   mmapped --db_file); edge cells (b5 edge-interface) live in g_edge_arena
   (rebuilt per border row in fixed mode, once in free mode). */
extern Cell *g_db[DIM_INNER][DIM_INNER][DIM_INNER][DIM_INNER][DIM_INNER][DIM_B5];

extern uint32_t g_db_max_cell_n;     /* sizing for the fan-out sort buffer */

/* Left-agnostic fan-out: g_fanout[suffix] = sum over the 17 left colors of the
   record count of DB[la][suffix]. Built once for the inner b5 slots, refreshed
   for the edge b5 slots after every edge-cell (re)build. */
extern uint64_t *g_fanout;

extern BottomOrder *g_bottoms;  extern size_t g_bottom_n;
extern LeftOrder   *g_lefts;    extern size_t g_left_n;

/* Shared run flags, set by the solver's main() before the builds run. */
extern int  g_nthreads;
extern bool g_verbose;
extern bool g_soft_center_139;
extern bool g_free_edges;

/* Pieces barred from the database: excluded pieces never enter a chain record
   or the edge-terminal pool (used e.g. to build a reduced database for a board
   whose lower rows are already fixed). All-zero (the default) = no exclusion. */
extern uint64_t g_db_exclude[4];

extern int g_top_border_inner_count[NUM_COLORS_TOTAL];
extern int g_inner_color_total[NUM_COLORS_TOTAL];

/* Piece-level companion to g_inner_color_total: g_color_pieces[c] is the used[]-
   style bitmask of every INNER piece carrying color c on at least one side. The
   two count different things -- a piece with two sides of color c adds 2 to
   g_inner_color_total[c] but is still one piece -- which is what makes the
   supply certificate (frontier columns demanding c vs pieces that could serve
   them) independent of the half-edge parity test. */
extern uint64_t g_color_pieces[NUM_COLORS_TOTAL][4];

/* Memory tracking for the two database arenas. */
extern uint8_t *g_inner_arena;  extern size_t g_inner_arena_size;
extern uint8_t *g_edge_arena;   extern size_t g_edge_arena_size;

/* Init timing, for the end-of-run summary (seconds). */
extern double g_time_db_build, g_time_db_sort;

/* -- Inline utilities ----------------------------------------------------- */

static inline uint64_t piece_bit(uint16_t pid) { return 1ULL << (pid & 63u); }
static inline bool used_test(const uint64_t used[4], uint16_t pid) {
    return (used[pid >> 6] & piece_bit(pid)) != 0;
}
static inline void used_set(uint64_t used[4], uint16_t pid) {
    used[pid >> 6] |= piece_bit(pid);
}
static inline void used_clear(uint64_t used[4], uint16_t pid) {
    used[pid >> 6] &= ~piece_bit(pid);
}

static inline bool color_is_inner(int c) { return c >= COLOR_MIN && c <= COLOR_MAX; }
static inline bool color_is_edge_iface(int c) { return c >= 1 && c <= MAX_EDGE_SIDE_COLOR; }

static inline bool masks_intersect4(const uint64_t a[4], const uint64_t b[4]) {
    return ((a[0] & b[0]) | (a[1] & b[1]) | (a[2] & b[2]) | (a[3] & b[3])) != 0;
}

/* Flat index into the 6-D pointer array. la,b1..b4 are INNER_IDX (0..16);
   b5 is a RAW color (0..22), so one index covers both inner and edge-iface.
   la is the outermost dimension: db_flat = la*FANOUT_N + fan_flat. */
static inline size_t fan_flat(int b1, int b2, int b3, int b4, int b5raw) {
    return ((((size_t)b1*DIM_INNER + b2)*DIM_INNER + b3)*DIM_INNER + b4)*DIM_B5 + b5raw;
}
static inline size_t db_flat(int la, int b1, int b2, int b3, int b4, int b5raw) {
    return (size_t)la*FANOUT_N + fan_flat(b1, b2, b3, b4, b5raw);
}

/* -- Record codec (variable byte width) ------------------------------------ */
/* A record is a little-endian bitstream of fixed-width fields packed into the low
   bits of a uint32 (the whole record is <= 32 bits, asserted at build time).
   Inner record: five g_lb_bits fields = the 5 inner pieces' (left,bottom)-bucket
   indices. Edge record: four g_lb_bits fields (the 4 inner pieces) + one
   g_term_bits field (the terminal's index within its per-left terminal bucket).
   The cell's b5 (inner vs edge-iface) tells the site which decode to use. */

/* Read/write the j-th record (exactly nbytes bytes -- never over-reads). */
static inline uint32_t rec_load(const uint8_t *base, size_t j, int nbytes) {
    uint32_t w = 0; memcpy(&w, base + j * (size_t)nbytes, (size_t)nbytes); return w;
}
static inline void rec_store(uint8_t *base, size_t j, int nbytes, uint32_t w) {
    memcpy(base + j * (size_t)nbytes, &w, (size_t)nbytes);
}

static inline uint32_t pack_inner(const uint8_t f[CHAIN_LEN], int bits) {
    uint32_t w = 0;
    for (int i = 0; i < CHAIN_LEN; i++) w |= (uint32_t)f[i] << (bits * i);
    return w;
}
static inline void unpack_inner(uint32_t w, uint8_t f[CHAIN_LEN], int bits) {
    uint32_t m = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    for (int i = 0; i < CHAIN_LEN; i++) f[i] = (uint8_t)((w >> (bits * i)) & m);
}
static inline uint32_t pack_edge(const uint8_t f4[CHAIN_LEN-1], int term_idx,
                                 int bits, int tbits) {
    uint32_t w = 0; (void)tbits;
    for (int i = 0; i < CHAIN_LEN-1; i++) w |= (uint32_t)f4[i] << (bits * i);
    w |= (uint32_t)term_idx << (bits * (CHAIN_LEN-1));
    return w;
}
static inline void unpack_edge(uint32_t w, uint8_t f4[CHAIN_LEN-1], int *term_idx,
                               int bits, int tbits) {
    uint32_t m  = (1u << bits)  - 1u;
    uint32_t tm = (1u << tbits) - 1u;
    for (int i = 0; i < CHAIN_LEN-1; i++) f4[i] = (uint8_t)((w >> (bits * i)) & m);
    *term_idx = (int)((w >> (bits * (CHAIN_LEN-1))) & tm);
}

/* Walk an inner chain of `count` pieces from `start_left` color, indexing the
   (left,bottom) bucket at each step. bottoms[i] is the RAW bottom color of piece i
   (= the cell key). Fills ci_out[] and OR-s the pieces into mask_inout[]. Returns
   false if a local index is out of range for its bucket (reliability). */
static inline bool decode_inner_chain(const uint8_t loc[], int count, int start_left,
                                      const uint8_t bottoms[], uint16_t ci_out[],
                                      uint64_t mask_inout[4], const uint64_t forbid[4]) {
    int cl = start_left;
    for (int i = 0; i < count; i++) {
        int b = bottoms[i];
        if (cl < 0 || cl >= NUM_COLORS_TOTAL || b < 0 || b >= NUM_COLORS_TOTAL) return false;
        if (loc[i] >= g_lb_count[cl][b]) return false;
        int ci = g_lb_bucket[cl][b][loc[i]];
        ci_out[i] = (uint16_t)ci;
        cl = g_cat[ci].right;
        uint16_t pid = g_cat[ci].piece_id;
        uint64_t bit = piece_bit(pid);
        /* The caller's own disjointness test, applied per piece instead of once
           per finished chain. Cells hold ~142 records and deep rows have most
           pieces placed, so most chains die on a conflict; decoding the other
           four pieces first was the bulk of the expansion's work. Callers with
           nothing to forbid pass an all-zero mask, which never fires. */
        if (forbid[pid >> 6] & bit) return false;
        mask_inout[pid >> 6] |= bit;
    }
    return true;
}

/* -- Inline RNG (xoroshiro128++ + splitmix64) ----------------------------- */

static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64-k)); }

static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static inline uint64_t rng_next(RNG *r) {
    uint64_t s0 = r->s[0], s1 = r->s[1];
    uint64_t result = rotl64(s0 + s1, 17) + s0;
    s1 ^= s0;
    r->s[0] = rotl64(s0, 49) ^ s1 ^ (s1 << 21);
    r->s[1] = rotl64(s1, 28);
    return result;
}

/* Unbiased uniform draw in [0, n) (Lemire's method). */
static inline uint32_t rng_uniform(RNG *r, uint32_t n) {
    if (n <= 1) return 0;
    uint64_t m = (uint64_t)(uint32_t)rng_next(r) * (uint64_t)n;
    uint32_t leftover = (uint32_t)m;
    if (leftover < n) {
        uint32_t threshold = (uint32_t)(0u - n) % n;
        while (leftover < threshold) {
            m = (uint64_t)(uint32_t)rng_next(r) * (uint64_t)n;
            leftover = (uint32_t)m;
        }
    }
    return (uint32_t)(m >> 32);
}

static inline RNG rng_for(uint64_t cfg_hash, uint32_t row,
                          uint32_t parent, uint32_t slice) {
    uint64_t h = splitmix64(cfg_hash
        ^ ((uint64_t)row    * 0x9E3779B97F4A7C15ULL)
        ^ ((uint64_t)parent * 0xBF58476D1CE4E5B9ULL)
        ^ ((uint64_t)slice  * 0x94D049BB133111EBULL));
    RNG r; r.s[0] = h; r.s[1] = splitmix64(h);
    return r;
}

/* -- Process utilities (defined in E555_database.c) -------------------- */

void  fatal(const char *fmt, ...);
void *xmalloc(size_t n);
void *xrealloc(void *ptr, size_t n);
void *arena_map(size_t bytes);
void  ensure_dir(const char *path);
uint64_t fnv1a_str(const char *s);
bool parse_u64_token(const char *s, unsigned long long *out);

/* -- Seed / catalog / pool setup ------------------------------------------ */

void load_seed_and_catalog(const char *path);   /* validates color structure */
void build_catalog_indices(void);
void build_inner_color_totals(void);
void classify_deal_from_rotations(void);
void build_edge_terminal_pool(void);     /* per border row; honours --free_edges */
void build_top_border_demands(void);
void validate_color_constants(void);

/* -- Database build / cache ------------------------------------------------ */

void build_db_inner(void);               /* inner cells (5 inner): once per seed */
void build_fanout_inner(void);           /* fan-out table, inner b5 slots */
void sort_db_by_fanout(void);            /* sort inner cells by promise (needs fan-out) */
void build_db_edge_and_sort(void);       /* edge cells + their fan-out slots + sort */

bool db_cache_load(const char *path);    /* mmap a cached inner DB; false = miss */
void db_cache_save(const char *path);    /* write the built+sorted inner DB */

/* -- Border enumeration / ranking ----------------------------------------- */

/* Records that can sit on 5 bottom colors, summed over all left neighbours
   (c0..c3 inner, c4raw inner-or-edge raw color). One fan-out table lookup. */
uint64_t db_seg_fanout(int c0, int c1, int c2, int c3, int c4raw);

void enumerate_bottoms(void);            /* fills g_bottoms / g_bottom_n */
void enumerate_lefts(void);              /* fills g_lefts / g_left_n */
void rank_bottoms(void);                 /* needs the DB; sets BottomOrder.rank, sorts */
void rank_lefts(void);                   /* needs the DB; sets LeftOrder.rank, sorts */

/* -- Random border sampling (--random_edges mode) -------------------------- */
/* Instead of a Stage A rotation row, borders are drawn directly from the
   canonical seed's edge pool: random corner roles, then a random legal chain of
   14 frame-down (bottom) or frame-left (left column) edges between them. Each
   published border is the best of RANDOM_SIDE_SAMPLES independent samples by
   the same fan-out ranks used for enumerated borders. sample_random_bottom also
   assigns all four corner roles (g_cBL/g_cBR/g_cTL/g_cTR); sample_random_left
   draws only from edges the bottom did not consume. Requires free-edges mode. */
#define RANDOM_SIDE_SAMPLES 32

bool sample_random_bottom(RNG *rng, BottomOrder *out);
bool sample_random_left(RNG *rng, const BottomOrder *bot, LeftOrder *out);

/* Validate and resolve any user-pinned corners (g_fixed_corner_pid): each fixed
   role must name a genuine, distinct corner piece; with exactly 3 fixed the 4th
   is auto-assigned. Builds the corner/edge pools if needed. Call once before
   sampling. */
void finalize_fixed_corners(void);

/* Read the want-th non-comment data row's 256 spins from a Stage A CSV. */
bool read_one_border_row(const char *csv_path, uint32_t want, uint8_t spins[NUM_PIECES]);

#endif /* E555_DATABASE_V1_H */
