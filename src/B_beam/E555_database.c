/*
 * E555_database.c -- E555 5-5-5 single-database module: implementation.
 *
 * Builds and owns the seed/catalog, the oriented-piece tables, the edge/border
 * pools, the bottom-row and left-column enumerations, the single 5-piece
 * chain database DB_5pieces, its left-agnostic fan-out table, and the optional
 * on-disk cache of the (border-independent) inner cells. See
 * E555_database.h for the design overview. A search driver
 * (E555_beamer.c) calls the build/enumerate functions, then reads the
 * read-only tables through the shared globals and inline helpers in the header.
 *
 * COMPILE (see E555_beamer.c):
 *   gcc -Wall -Wextra -O3 -march=native -fopenmp \
 *       E555_database.c E555_beamer.c -o E555_beamer -lm
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <omp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "E555_database.h"

/* -- Globals --------------------------------------------------------------- */

int g_seed_top[NUM_PIECES];
int g_seed_right[NUM_PIECES];
int g_seed_bottom[NUM_PIECES];
int g_seed_left[NUM_PIECES];

int g_inner_ids[EXPECTED_INNER];
int g_num_inner = 0;

uint64_t g_seed_file_hash = 0;

Oriented g_cat[CATALOG_SIZE];
int      g_cat_count = 0;

int g_left_bucket[NUM_COLORS_TOTAL][CATALOG_SIZE];
int g_left_count[NUM_COLORS_TOTAL];
int g_cat_to_local[CATALOG_SIZE];

int g_lb_bucket[NUM_COLORS_TOTAL][NUM_COLORS_TOTAL][MAX_LB_BUCKET];
int g_lb_count[NUM_COLORS_TOTAL][NUM_COLORS_TOTAL];
uint32_t g_lb_step[NUM_COLORS_TOTAL][NUM_COLORS_TOTAL][MAX_LB_BUCKET];
int g_cat_to_lb_local[CATALOG_SIZE];
int g_lb_bits = 0, g_term_bits = 0;
int g_rec_bytes_inner = 0, g_rec_bytes_edge = 0;

uint8_t  g_spin[NUM_PIECES];
Oriented g_bottom_pool[EDGE_LEN];
int      g_bottom_count    = 0;
Oriented g_left_pool[EDGE_LEN];
int      g_left_count_pool = 0;

Oriented g_cBL, g_cBR, g_cTL, g_cTR;
bool     g_has_cBL = false, g_has_cBR = false;
bool     g_has_cTL = false, g_has_cTR = false;
int      g_fixed_corner_pid[4] = { -1, -1, -1, -1 };  /* role 0=BL 1=BR 2=TL 3=TR */

Oriented g_edge_term[MAX_EDGE_TERMINALS];
int      g_edge_term_count = 0;
int      g_edge_term_by_left[NUM_COLORS_TOTAL][MAX_EDGE_TERMINALS];
int      g_edge_term_by_left_n[NUM_COLORS_TOTAL];

Cell *g_db[DIM_INNER][DIM_INNER][DIM_INNER][DIM_INNER][DIM_INNER][DIM_B5];

uint32_t  g_db_max_cell_n = 0;
uint64_t *g_fanout = NULL;

/* Per-left_color inner-record totals (for rank_lefts). Computed during the
   fan-out build (or from the cache index table) so ranking never has to touch
   the multi-GB inner arena. */
static uint64_t g_la_total_inner[DIM_INNER];

BottomOrder *g_bottoms = NULL;  size_t g_bottom_n = 0;
LeftOrder   *g_lefts   = NULL;  size_t g_left_n   = 0;

int  g_nthreads        = 0;
bool g_verbose         = false;
bool g_free_edges      = false;

/* Pieces barred from the database: excluded pieces never enter a chain record
   or the edge-terminal pool. All-zero (the default) means no exclusion. */
uint64_t g_db_exclude[4] = {0, 0, 0, 0};

/* Generated from the published clue data; see ClueCell in the header. Entry 0
   of each row is the center clue, 1..2 the reachable corners, 3..4 row 13. */
const ClueCell g_clue[4][CLUE_N] = {
    { { 7, 7,138,0}, { 2, 2,180,0}, { 2,13,248,3}, {13, 2,207,3}, {13,13,254,1} },   /*   0 deg */
    { { 8, 7,138,3}, { 2, 2,248,2}, { 2,13,254,0}, {13, 2,180,3}, {13,13,207,2} },   /*  90 deg */
    { { 8, 8,138,2}, { 2, 2,254,3}, { 2,13,207,1}, {13, 2,248,1}, {13,13,180,2} },   /* 180 deg */
    { { 7, 8,138,1}, { 2, 2,207,0}, { 2,13,180,1}, {13, 2,254,2}, {13,13,248,0} },   /* 270 deg */
};
uint32_t g_clue_mask    = 0;
uint8_t  g_clue_orients = 0xF;

int g_top_border_inner_count[NUM_COLORS_TOTAL];
int g_inner_color_total[NUM_COLORS_TOTAL];

uint8_t *g_inner_arena = NULL;  size_t g_inner_arena_size = 0;
uint8_t *g_edge_arena  = NULL;  size_t g_edge_arena_size  = 0;

double g_time_db_build = 0.0, g_time_db_sort = 0.0;

/* -- Utility -------------------------------------------------------------- */

void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "Fatal: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap); exit(EXIT_FAILURE);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) fatal("malloc(%zu) failed", n);
    return p;
}

void *xrealloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n);
    if (!p && n != 0) fatal("realloc(%zu) failed", n);
    return p;
}

void *arena_map(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fatal("mmap(%zu bytes) failed: %s", bytes, strerror(errno));
    madvise(p, bytes, MADV_HUGEPAGE);
    return p;
}

void ensure_dir(const char *path) {
    if (mkdir(path, 0775) != 0 && errno != EEXIST)
        fatal("cannot create directory %s: %s", path, strerror(errno));
}

uint64_t fnv1a_str(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

/* fnv1a over a whole file's bytes (the --db_file cache key for the seed). */
static uint64_t fnv1a_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open %s: %s", path, strerror(errno));
    uint64_t h = 14695981039346656037ULL;
    uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
    fclose(f);
    return h;
}

/* -- Seed / catalog -------------------------------------------------------- */

static Oriented make_oriented(uint16_t piece_id, uint8_t spin) {
    const int e[4] = { g_seed_top[piece_id], g_seed_right[piece_id],
                       g_seed_bottom[piece_id], g_seed_left[piece_id] };
    Oriented o;
    o.piece_id=piece_id; o.rotation=spin;
    o.top   =(uint8_t)e[(0+spin)&3]; o.right =(uint8_t)e[(1+spin)&3];
    o.bottom=(uint8_t)e[(2+spin)&3]; o.left  =(uint8_t)e[(3+spin)&3];
    return o;
}

/* Return the spin that places color 0 on the given side, or false if none. */
static bool orient_with_zero_side(uint16_t piece_id, int side, Oriented *out) {
    for (uint8_t spin = 0; spin < 4; spin++) {
        Oriented o = make_oriented(piece_id, spin);
        int edge = (side==0)?o.top:(side==1)?o.right:(side==2)?o.bottom:o.left;
        if (edge == 0) { *out = o; return true; }
    }
    return false;
}

static int canonical_zero_count(int i) {
    return (g_seed_top[i]==0)+(g_seed_right[i]==0)+(g_seed_bottom[i]==0)+(g_seed_left[i]==0);
}

void load_seed_and_catalog(const char *path) {
    g_seed_file_hash = fnv1a_file(path);
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open seed file %s: %s", path, strerror(errno));
    for (int i = 0; i < NUM_PIECES; i++) {
        if (fscanf(f, "%d %d %d %d", &g_seed_top[i], &g_seed_right[i],
                   &g_seed_bottom[i], &g_seed_left[i]) != 4)
            fatal("seed file must have 256 rows of 4 integers");
        int vals[4] = {g_seed_top[i],g_seed_right[i],g_seed_bottom[i],g_seed_left[i]};
        int zeros = 0, iface = 0, inner = 0;
        for (int k = 0; k < 4; k++) {
            if (vals[k]<0 || vals[k]>=NUM_COLORS_TOTAL)
                fatal("seed row %d: color %d outside 0..22", i, vals[k]);
            if (vals[k]==0) zeros++;
            else if (color_is_edge_iface(vals[k])) iface++;
            else inner++;
        }
        /* Validate the E2 color structure: inner colors (6..22) never appear on
           the frame interface and vice versa. A violation would silently corrupt
           the DB build (negative INNER_IDX), so reject the seed up front. */
        if (zeros==0 && inner!=4)
            fatal("seed row %d: inner piece carries edge-interface color(s) 1..5", i);
        if (zeros==1 && !(iface==2 && inner==1))
            fatal("seed row %d: edge piece must have 2 interface + 1 inner colors", i);
        if (zeros==2 && iface!=2)
            fatal("seed row %d: corner piece must have 2 interface colors", i);
        if (zeros>2)
            fatal("seed row %d: more than 2 frame sides", i);
        if (zeros==0) g_inner_ids[g_num_inner++] = i;
    }
    fclose(f);
    if (g_num_inner != EXPECTED_INNER)
        fatal("seed has %d inner pieces; expected %d", g_num_inner, EXPECTED_INNER);
    g_cat_count = 0;
    for (int k = 0; k < g_num_inner; k++) {
        uint16_t pid = (uint16_t)g_inner_ids[k];
        for (uint8_t spin = 0; spin < 4; spin++)
            g_cat[g_cat_count++] = make_oriented(pid, spin);
    }
    if (g_verbose) { printf("[init] seed loaded: inner=%d catalog=%d hash=%016" PRIx64 "\n",
                            g_num_inner, g_cat_count, g_seed_file_hash); fflush(stdout); }
}

/* Smallest field width (>=1 bit) that can index n items (values 0..n-1). */
static int bits_for(int n) {
    int b = 1;
    while ((1 << b) < n) b++;
    return b;
}

/* Bucket every oriented inner piece by its LEFT color (used by the DB build's
   DFS) AND by its (LEFT,BOTTOM) colors (the record fields). Derive the
   inner-record field width and byte size from the largest (left,bottom) bucket. */
void build_catalog_indices(void) {
    memset(g_left_count, 0, sizeof(g_left_count));
    memset(g_lb_count,   0, sizeof(g_lb_count));
    for (int ci = 0; ci < g_cat_count; ci++) {
        const Oriented *o = &g_cat[ci];
        if (g_left_count[o->left] >= CATALOG_SIZE) fatal("left-color bucket overflow for color %u", o->left);
        g_cat_to_local[ci] = g_left_count[o->left];
        g_left_bucket[o->left][g_left_count[o->left]++] = ci;

        int *nlb = &g_lb_count[o->left][o->bottom];
        if (*nlb >= MAX_LB_BUCKET)
            fatal("(left,bottom)=(%u,%u) bucket overflow: raise MAX_LB_BUCKET", o->left, o->bottom);
        g_cat_to_lb_local[ci] = *nlb;
        g_lb_bucket[o->left][o->bottom][(*nlb)++] = ci;
    }

    int maxlb = 1;
    for (int L = 0; L < NUM_COLORS_TOTAL; L++)
        for (int B = 0; B < NUM_COLORS_TOTAL; B++)
            if (g_lb_count[L][B] > maxlb) maxlb = g_lb_count[L][B];
    g_lb_bits = bits_for(maxlb);
    if (CHAIN_LEN * g_lb_bits > 32)
        fatal("inner record needs %d bits (>32); widen rec_load/store to 64-bit", CHAIN_LEN * g_lb_bits);
    g_rec_bytes_inner = (CHAIN_LEN * g_lb_bits + 7) / 8;

    /* Flatten the buckets for the chain walk. A record's per-piece field is
       g_lb_bits wide, so decode_inner_chain indexes the table with values up to
       (1 << g_lb_bits) - 1 and does no bounds test of its own: that is safe only
       while the field cannot outrun the table, which is checked here once. It
       holds for every seed (bits_for caps at 6 for a 64-slot bucket) and would
       only break if MAX_LB_BUCKET were raised to a non-power of two. */
    if ((1 << g_lb_bits) > MAX_LB_BUCKET)
        fatal("g_lb_bits=%d indexes %d slots but MAX_LB_BUCKET is %d; raise it to a power of two",
              g_lb_bits, 1 << g_lb_bits, MAX_LB_BUCKET);
    for (int L = 0; L < NUM_COLORS_TOTAL; L++)
        for (int B = 0; B < NUM_COLORS_TOTAL; B++)
            for (int k = 0; k < MAX_LB_BUCKET; k++) {
                if (k >= g_lb_count[L][B]) { g_lb_step[L][B][k] = LBSTEP_DEAD; continue; }
                int ci = g_lb_bucket[L][B][k];
                const Oriented *o = &g_cat[ci];
                if (ci > 0x3FF || o->right > 0x1F || o->piece_id > 0x1FF)
                    fatal("g_lb_step packing overflow: ci=%d right=%u piece=%u",
                          ci, o->right, o->piece_id);
                g_lb_step[L][B][k] = (uint32_t)ci
                                   | ((uint32_t)o->right    << 10)
                                   | ((uint32_t)o->piece_id << 15);
            }

    if (g_verbose) {
        printf("[init] catalog=%d  max(left,bottom) bucket=%d -> g_lb_bits=%d  inner record=%d B\n",
               g_cat_count, maxlb, g_lb_bits, g_rec_bytes_inner);
        fflush(stdout);
    }
}

static bool parse_spin_token(const char *s, uint8_t *out) {
    char *end=NULL; long v=strtol(s,&end,10);
    if (end==s || *end!='\0' || v<0 || v>3) return false;
    *out=(uint8_t)v; return true;
}

bool parse_u64_token(const char *s, unsigned long long *out) {
    char *end=NULL; *out=strtoull(s,&end,10);
    return (end!=s && *end=='\0');
}

static void add_corner(Oriented o) {
    if      (o.bottom==0&&o.left ==0) { if (g_has_cBL) fatal("duplicate BL corner"); g_cBL=o; g_has_cBL=true; }
    else if (o.bottom==0&&o.right==0) { if (g_has_cBR) fatal("duplicate BR corner"); g_cBR=o; g_has_cBR=true; }
    else if (o.top   ==0&&o.left ==0) { if (g_has_cTL) fatal("duplicate TL corner"); g_cTL=o; g_has_cTL=true; }
    else if (o.top   ==0&&o.right==0) { if (g_has_cTR) fatal("duplicate TR corner"); g_cTR=o; g_has_cTR=true; }
    else fatal("corner piece %u not in a valid zero-zero corner orientation", o.piece_id);
}

/* Populate bottom/left pools and corners from the current g_spin[]. */
void classify_deal_from_rotations(void) {
    g_bottom_count = g_left_count_pool = 0;
    g_has_cBL = g_has_cBR = g_has_cTL = g_has_cTR = false;
    for (int i = 0; i < NUM_PIECES; i++) {
        int zeros = canonical_zero_count(i);
        Oriented o = make_oriented((uint16_t)i, g_spin[i]);
        if (zeros==2) add_corner(o);
        else if (zeros==1) {
            if (o.bottom==0) {
                if (g_bottom_count >= EDGE_LEN) fatal("too many bottom edges");
                g_bottom_pool[g_bottom_count++] = o;
            } else if (o.left==0) {
                if (g_left_count_pool >= EDGE_LEN) fatal("too many left edges");
                g_left_pool[g_left_count_pool++] = o;
            }
        }
    }
    if (!g_has_cBL || !g_has_cBR || !g_has_cTL || !g_has_cTR)
        fatal("border row does not provide exactly one corner in each board corner");
    if (g_bottom_count != EDGE_LEN) fatal("border row produced %d bottom edges; expected %d", g_bottom_count, EDGE_LEN);
    if (g_left_count_pool != EDGE_LEN) fatal("border row produced %d left edges; expected %d", g_left_count_pool, EDGE_LEN);
}

/* Build the edge-terminal pool that may END a chain (frame-right orientation).
 * Free mode: ALL non-corner edges (border-independent) -- call once.
 * Fixed mode: only this border row's right edges (frame-right in g_spin).
 * In both cases bucket terminals by the inner LEFT color they must match. */
void build_edge_terminal_pool(void) {
    g_edge_term_count = 0;
    memset(g_edge_term_by_left_n, 0, sizeof(g_edge_term_by_left_n));

    const bool excluding = (g_db_exclude[0] | g_db_exclude[1] |
                            g_db_exclude[2] | g_db_exclude[3]) != 0;
    if (g_free_edges) {
        for (int i = 0; i < NUM_PIECES; i++) {
            if (canonical_zero_count(i) != 1) continue;           /* edges only */
            if (used_test(g_db_exclude, (uint16_t)i)) continue;
            Oriented o;
            if (!orient_with_zero_side((uint16_t)i, 1, &o))       /* frame on right */
                fatal("edge piece %d cannot be oriented frame-right", i);
            if (g_edge_term_count >= MAX_EDGE_TERMINALS) fatal("edge terminal pool overflow");
            g_edge_term[g_edge_term_count++] = o;
        }
    } else {
        for (int i = 0; i < NUM_PIECES; i++) {
            if (canonical_zero_count(i) != 1) continue;
            if (used_test(g_db_exclude, (uint16_t)i)) continue;
            Oriented o = make_oriented((uint16_t)i, g_spin[i]);
            if (o.right != 0) continue;                           /* right edges of THIS row */
            if (g_edge_term_count >= MAX_EDGE_TERMINALS) fatal("edge terminal pool overflow");
            g_edge_term[g_edge_term_count++] = o;
        }
        if (!excluding && g_edge_term_count != EDGE_LEN)
            fatal("fixed mode produced %d right edges; expected %d", g_edge_term_count, EDGE_LEN);
    }
    for (int t = 0; t < g_edge_term_count; t++) {
        int cl = g_edge_term[t].left;
        if (!color_is_inner(cl))
            fatal("edge terminal %d (piece %u) has non-inner left color %d", t, g_edge_term[t].piece_id, cl);
        g_edge_term_by_left[cl][g_edge_term_by_left_n[cl]++] = t;
    }
    /* The edge record stores a terminal as its index WITHIN its per-left bucket, so
       its width follows the largest such bucket (recomputed here because the fixed-
       mode pool changes per border row; the inner field width g_lb_bits is fixed). */
    int maxt = 1;
    for (int c = 0; c < NUM_COLORS_TOTAL; c++)
        if (g_edge_term_by_left_n[c] > maxt) maxt = g_edge_term_by_left_n[c];
    g_term_bits = bits_for(maxt);
    if ((CHAIN_LEN-1) * g_lb_bits + g_term_bits > 32)
        fatal("edge record needs %d bits (>32); widen rec_load/store to 64-bit",
              (CHAIN_LEN-1) * g_lb_bits + g_term_bits);
    g_rec_bytes_edge = ((CHAIN_LEN-1) * g_lb_bits + g_term_bits + 7) / 8;
    if (g_verbose) {
        printf("[init] edge terminals: %d (%s)  max per-left bucket=%d -> g_term_bits=%d  edge record=%d B\n",
               g_edge_term_count, g_free_edges?"free":"fixed", maxt, g_term_bits, g_rec_bytes_edge);
        fflush(stdout);
    }
}

/* Fixed-mode top-border inner demands: each frame-top edge's inner bottom is a
 * color the search must reserve (Stage C places the top border). Free mode: the
 * top assignment is unknown, so the demand is relaxed to zero. */
void build_top_border_demands(void) {
    memset(g_top_border_inner_count, 0, sizeof(g_top_border_inner_count));
    if (g_free_edges) return;
    int top_n = 0;
    for (int i = 0; i < NUM_PIECES; i++) {
        if (canonical_zero_count(i) != 1) continue;
        Oriented o = make_oriented((uint16_t)i, g_spin[i]);
        if (o.top != 0) continue;
        top_n++;
        if (!color_is_inner(o.bottom))
            fatal("top-border piece %d has non-inner bottom color %u", i, o.bottom);
        g_top_border_inner_count[o.bottom]++;
    }
    if (top_n != EDGE_LEN) fatal("border row produced %d top edges; expected %d", top_n, EDGE_LEN);
}

void validate_color_constants(void) {
    for (int t = 0; t < g_edge_term_count; t++) {
        int b = g_edge_term[t].bottom, tp = g_edge_term[t].top;
        if (!color_is_edge_iface(b) || !color_is_edge_iface(tp))
            fatal("edge terminal %d (piece %u) interface colors (b=%d t=%d) not in 1..%d",
                  t, g_edge_term[t].piece_id, b, tp, MAX_EDGE_SIDE_COLOR);
    }
    if (!color_is_edge_iface(g_cBR.top))
        fatal("bottom-right corner top color %u is not an edge-interface color", g_cBR.top);
}

void build_inner_color_totals(void) {
    memset(g_inner_color_total, 0, sizeof(g_inner_color_total));
    for (int k = 0; k < g_num_inner; k++) {
        int pid = g_inner_ids[k];
        const int e[4] = { g_seed_top[pid], g_seed_right[pid],
                           g_seed_bottom[pid], g_seed_left[pid] };
        for (int s = 0; s < 4; s++)
            g_inner_color_total[e[s]]++;             /* half-edges: counts twice */
    }
}

/* -- DB_5pieces: inner cells (5 inner pieces) ------------------------------ */

/* DFS placing inner pieces depth..CHAIN_LEN-1 of a chain rooted at piece 0.
 * la_idx = INNER_IDX(root.left) is the cell's first key dim. locs[] holds each
 * piece's (left,bottom)-bucket index -- the packed record fields. */
static void dfs_inner(int depth, int la_idx, int next_left, int bidx[CHAIN_LEN],
                      uint8_t locs[CHAIN_LEN], uint64_t used[4], uint32_t *work,
                      bool store) {
    const int n = g_left_count[next_left];
    for (int i = 0; i < n; i++) {
        int ci = g_left_bucket[next_left][i];
        uint16_t pid = g_cat[ci].piece_id;
        if (used_test(used, pid)) continue;
        int b = g_cat[ci].bottom;                 /* inner pieces always have inner bottom */
        bidx[depth] = INNER_IDX(b);
        locs[depth] = (uint8_t)g_cat_to_lb_local[ci];
        if (depth == CHAIN_LEN - 1) {
            size_t fi = db_flat(la_idx, bidx[0],bidx[1],bidx[2],bidx[3], bidx[4] + COLOR_MIN);
            if (!store) {
                #pragma omp atomic
                work[fi]++;
            } else {
                Cell *cell = g_db[la_idx][bidx[0]][bidx[1]][bidx[2]][bidx[3]][bidx[4]+COLOR_MIN];
                uint32_t pos;
                #pragma omp atomic capture
                pos = work[fi]++;
                rec_store(cell->rec, pos, g_rec_bytes_inner, pack_inner(locs, g_lb_bits));
            }
        } else {
            used_set(used, pid);
            dfs_inner(depth+1, la_idx, g_cat[ci].right, bidx, locs, used, work, store);
            used_clear(used, pid);
        }
    }
}

static void db_inner_pass(uint32_t *work, bool store) {
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    #pragma omp parallel for schedule(dynamic, 4) num_threads(nt)
    for (int ci0 = 0; ci0 < g_cat_count; ci0++) {
        uint16_t pid0 = g_cat[ci0].piece_id;
        if (used_test(g_db_exclude, pid0)) continue;
        uint64_t used[4];                 /* excluded pieces start as "used" */
        memcpy(used, g_db_exclude, sizeof used);
        uint8_t locs[CHAIN_LEN]; int bidx[CHAIN_LEN];
        int la_idx = INNER_IDX(g_cat[ci0].left);
        bidx[0] = INNER_IDX(g_cat[ci0].bottom);
        locs[0] = (uint8_t)g_cat_to_lb_local[ci0];
        used_set(used, pid0);
        dfs_inner(1, la_idx, g_cat[ci0].right, bidx, locs, used, work, store);
    }
}

/* -- DB_5pieces: edge cells (4 inner + frame-right terminal) --------------- */

static void dfs_edge(int depth, int la_idx, int next_left, int bidx[CHAIN_LEN],
                     uint8_t locs[CHAIN_LEN], uint64_t used[4], uint32_t *work,
                     bool store) {
    if (depth == CHAIN_LEN - 1) {
        /* Four inner pieces placed (bidx[0..3]); attach a terminal edge whose
         * left color matches the last inner piece's exposed right color. The
         * record stores k, the terminal's index WITHIN its per-left bucket. */
        int cl = next_left;
        if (!color_is_inner(cl)) return;
        for (int k = 0; k < g_edge_term_by_left_n[cl]; k++) {
            int t = g_edge_term_by_left[cl][k];
            uint16_t pid = g_edge_term[t].piece_id;
            if (used_test(used, pid)) continue;
            int b5raw = g_edge_term[t].bottom;          /* edge-interface color */
            size_t fi = db_flat(la_idx, bidx[0],bidx[1],bidx[2],bidx[3], b5raw);
            if (!store) {
                #pragma omp atomic
                work[fi]++;
            } else {
                Cell *cell = g_db[la_idx][bidx[0]][bidx[1]][bidx[2]][bidx[3]][b5raw];
                uint32_t pos;
                #pragma omp atomic capture
                pos = work[fi]++;
                uint8_t f4[CHAIN_LEN-1] = { locs[0], locs[1], locs[2], locs[3] };
                rec_store(cell->rec, pos, g_rec_bytes_edge,
                          pack_edge(f4, k, g_lb_bits, g_term_bits));
            }
        }
        return;
    }
    const int n = g_left_count[next_left];
    for (int i = 0; i < n; i++) {
        int ci = g_left_bucket[next_left][i];
        uint16_t pid = g_cat[ci].piece_id;
        if (used_test(used, pid)) continue;
        bidx[depth] = INNER_IDX(g_cat[ci].bottom);
        locs[depth] = (uint8_t)g_cat_to_lb_local[ci];
        used_set(used, pid);
        dfs_edge(depth+1, la_idx, g_cat[ci].right, bidx, locs, used, work, store);
        used_clear(used, pid);
    }
}

static void db_edge_pass(uint32_t *work, bool store) {
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    #pragma omp parallel for schedule(dynamic, 4) num_threads(nt)
    for (int ci0 = 0; ci0 < g_cat_count; ci0++) {
        uint16_t pid0 = g_cat[ci0].piece_id;
        if (used_test(g_db_exclude, pid0)) continue;
        uint64_t used[4];                 /* excluded pieces start as "used" */
        memcpy(used, g_db_exclude, sizeof used);
        uint8_t locs[CHAIN_LEN]; int bidx[CHAIN_LEN];
        int la_idx = INNER_IDX(g_cat[ci0].left);
        bidx[0] = INNER_IDX(g_cat[ci0].bottom);
        locs[0] = (uint8_t)g_cat_to_lb_local[ci0];
        used_set(used, pid0);
        dfs_edge(1, la_idx, g_cat[ci0].right, bidx, locs, used, work, store);
    }
}

/* -- Cell-count statistics & arena layout ---------------------------------- */

/* Lay out the cells of one phase (inner or edge) into a fresh hugepage arena.
 * Cells are selected by their b5 dimension; the other phase's cells are left
 * untouched in the shared pointer array.
 *
 * This used to be three separate serial sweeps of all 32.6 M cells -- one to
 * total the bytes, one for the n-distribution printed below, one to hand out
 * the offsets. It is now two parallel ones. The n-distribution is folded into
 * the sizing pass, where the counts are already in registers, and accumulated
 * in integers rather than doubles so the printed mean and stdev do not depend
 * on how the work was divided.
 *
 * The offsets look inherently sequential -- each cell starts where the previous
 * one ended -- but if the range is cut into chunks that partition it IN ORDER,
 * every chunk's base is just the total size of the chunks before it. Summing
 * per-chunk sizes first and then filling gives exactly the layout the single
 * running sum produced, byte for byte. */
static void db_layout(const uint32_t *cnt, bool inner_phase,
                      uint8_t **out_arena, size_t *out_size, const char *name) {
    const int rb = inner_phase ? g_rec_bytes_inner : g_rec_bytes_edge;
    uint64_t total_bytes = 0, total_recs = 0, total_cells = 0, sum_sq = 0;
    uint32_t max_n = 0, min_n = UINT32_MAX;
    uint64_t la_tot[DIM_INNER] = {0};   /* per-left_color totals, inner phase */
    Cell **flat = &g_db[0][0][0][0][0][0];

    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    int nchunk = nt * 4;
    if (nchunk < 1) nchunk = 1;
    uint64_t *chunk_off = calloc((size_t)nchunk + 1, sizeof(uint64_t));
    if (!chunk_off) fatal("calloc for %s chunk offsets failed", name);
    #define DB_CHUNK_LO(k) (NCELL_DB * (uint64_t)(k) / (uint64_t)nchunk)

    #pragma omp parallel for schedule(static) num_threads(nt) \
            reduction(+:total_bytes,total_recs,total_cells,sum_sq) \
            reduction(+:la_tot[:DIM_INNER]) \
            reduction(max:max_n) reduction(min:min_n)
    for (int k = 0; k < nchunk; k++) {
        uint64_t bytes = 0;
        for (uint64_t fi = DB_CHUNK_LO(k); fi < DB_CHUNK_LO(k + 1); fi++) {
            if (color_is_inner((int)(fi % DIM_B5)) != inner_phase) continue;
            uint32_t n = cnt[fi];
            if (!n) continue;
            bytes += cell_stride(n, rb);
            total_recs += n; total_cells++;
            sum_sq += (uint64_t)n * (uint64_t)n;
            la_tot[fi / FANOUT_N] += n;
            if (n > max_n) max_n = n;
            if (n < min_n) min_n = n;
        }
        chunk_off[k + 1] = bytes;
        total_bytes += bytes;
    }
    if (total_cells == 0) fatal("%s is empty; seed/border inconsistent", name);
    if (max_n > g_db_max_cell_n) g_db_max_cell_n = max_n;
    /* The per-left_color totals rank_lefts needs. Free here: the counts are
       already in hand. Computing them later means dereferencing every pointer
       in the arena, i.e. reading all of it a second time. */
    if (inner_phase) memcpy(g_la_total_inner, la_tot, sizeof g_la_total_inner);

    double mean = (double)total_recs / (double)total_cells;
    double var  = (double)sum_sq / (double)total_cells - mean*mean;
    printf("[init] %s: %" PRIu64 " records  %" PRIu64 " cells  %.2f GB  rec=%dB  n: min=%u mean=%.1f max=%u stdev=%.1f\n",
           name, total_recs, total_cells, (double)total_bytes/1e9, rb,
           min_n, mean, max_n, sqrt(var < 0.0 ? 0.0 : var));
    fflush(stdout);

    uint8_t *arena = arena_map(total_bytes);   /* several GB for the inner DB */
    *out_arena = arena; *out_size = total_bytes;

    for (int k = 0; k < nchunk; k++) chunk_off[k + 1] += chunk_off[k];
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int k = 0; k < nchunk; k++) {
        uint64_t off = chunk_off[k];
        for (uint64_t fi = DB_CHUNK_LO(k); fi < DB_CHUNK_LO(k + 1); fi++) {
            if (color_is_inner((int)(fi % DIM_B5)) != inner_phase) continue;
            uint32_t n = cnt[fi];
            if (!n) continue;
            Cell *c = (Cell *)(arena + off);
            c->n = n; c->_pad = 0;
            flat[fi] = c;
            off += cell_stride(n, rb);
        }
    }
    free(chunk_off);
    #undef DB_CHUNK_LO
}

void build_db_inner(void) {
    double t0 = omp_get_wtime();
    uint32_t *cnt = calloc(NCELL_DB, sizeof(uint32_t));
    if (!cnt) fatal("calloc for inner count array (%zu cells) failed", (size_t)NCELL_DB);
    db_inner_pass(cnt, false);
    db_layout(cnt, true, &g_inner_arena, &g_inner_arena_size, "DB inner");
    memset(cnt, 0, NCELL_DB * sizeof(uint32_t));
    db_inner_pass(cnt, true);
    free(cnt);
    g_time_db_build = omp_get_wtime() - t0;
    printf("[init] DB inner stored (%.1fs)\n", g_time_db_build); fflush(stdout);
}

/* NULL every edge cell (b5 edge-interface) and free the old edge arena, so the
 * edge phase can be (re)built without disturbing the permanent inner cells. */
static void clear_edge_cells(void) {
    Cell **flat = &g_db[0][0][0][0][0][0];
    /* b5 is the LAST index of g_db and the non-inner colors are exactly
       0..COLOR_MIN-1, so the edge cells are the first COLOR_MIN slots of every
       DIM_B5-slot group -- a comb, not a scattered set. Walking the comb writes
       the same 6-in-23 slots without reading the other 17 or testing 32.6 M
       colors; the finalizer runs this once per partial line. */
    for (uint64_t base = 0; base < NCELL_DB; base += DIM_B5)
        for (uint64_t b5 = 0; b5 < COLOR_MIN; b5++) flat[base + b5] = NULL;
    if (g_edge_arena) { munmap(g_edge_arena, g_edge_arena_size); g_edge_arena = NULL; g_edge_arena_size = 0; }
}

static void build_db_edge(void) {
    double t0 = omp_get_wtime();
    clear_edge_cells();
    uint32_t *cnt = calloc(NCELL_DB, sizeof(uint32_t));
    if (!cnt) fatal("calloc for edge count array failed");
    db_edge_pass(cnt, false);
    db_layout(cnt, false, &g_edge_arena, &g_edge_arena_size, "DB edge");
    memset(cnt, 0, NCELL_DB * sizeof(uint32_t));
    db_edge_pass(cnt, true);
    free(cnt);
    if (g_verbose) { printf("[init] DB edge stored (%.1fs)\n", omp_get_wtime() - t0); fflush(stdout); }
}

/* -- Fan-out table ---------------------------------------------------------- */

/* Refresh one phase's slots of g_fanout: for every (b1..b4, b5) bottom
 * signature, the record count summed over the 17 possible left neighbours.
 * la is the outermost dimension of the flat DB index, so the 17 source cells
 * of slot i are flat[la*FANOUT_N + i]. Inner slots are refreshed once per DB
 * build/load; edge slots after every edge-cell rebuild. */
static void update_fanout(bool inner_phase) {
    if (!g_fanout) {
        g_fanout = xmalloc(FANOUT_N * sizeof(uint64_t));
        memset(g_fanout, 0, FANOUT_N * sizeof(uint64_t));
    }
    Cell **flat = &g_db[0][0][0][0][0][0];
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (uint64_t i = 0; i < FANOUT_N; i++) {
        if (color_is_inner((int)(i % DIM_B5)) != inner_phase) continue;
        uint64_t s = 0;
        for (int la = 0; la < DIM_INNER; la++) {
            const Cell *c = flat[(uint64_t)la * FANOUT_N + i];
            if (c) s += c->n;
        }
        g_fanout[i] = s;
    }
}

void build_fanout_inner(void) {
    update_fanout(true);
    /* g_la_total_inner is filled by db_layout, which has the same counts in
       registers while it sizes the arena; the --db_file load path fills it from
       the cache for the same reason. Recomputing it here meant a second full
       read of the multi-GB arena for numbers already known. */
}

/* Records that can sit on five given bottom colors, summed over all left
 * neighbours (c0..c3 inner, c4 raw inner-or-edge). Left-agnostic productivity;
 * shared by border ranking and the beam's one-row lookahead. */
uint64_t db_seg_fanout(int c0, int c1, int c2, int c3, int c4raw) {
    if (!color_is_inner(c0)||!color_is_inner(c1)||!color_is_inner(c2)||!color_is_inner(c3)) return 0;
    if (c4raw < 0 || c4raw >= DIM_B5) return 0;
    return g_fanout[fan_flat(INNER_IDX(c0), INNER_IDX(c1), INNER_IDX(c2), INNER_IDX(c3), c4raw)];
}

/* -- Fan-out sort ---------------------------------------------------------- */

/* Records in a cell, keyed by promise, for the in-cell order.
 * Promise = continuations of the chain's five exposed tops summed over every
 * possible left neighbour (the next row's left-neighbour color is
 * config-dependent and unknown here) -- one g_fanout lookup per record.
 *
 * Key and record travel as ONE uint64: the promise complemented in the high
 * half, the record word in the low half. Plain ascending order on that integer
 * is exactly "promise descending, record word ascending", so the order needs no
 * comparator at all -- which matters, because 3.1 billion records at ~142 per
 * cell is some 22 billion comparisons, and reaching a comparator through a
 * function pointer costs more than the comparison. */
static inline uint64_t promise_key(uint32_t fanout, uint32_t w) {
    return ((uint64_t)~fanout << 32) | (uint64_t)w;
}

/* Median-of-three quicksort, insertion sort below the cutoff, recursing on the
 * smaller side and looping on the larger so the stack stays O(log n).
 *
 * Quicksort is normally the wrong answer for keys that repeat, and raw fan-out
 * values repeat heavily -- but these keys cannot repeat: the low half is the
 * record word, and db_dfs enumerates each chain exactly once, so every word is
 * distinct inside its cell. The keys are therefore all distinct, the order is
 * total, and the partition never meets a run of equals. */
static void sort_promise(uint64_t *v, uint32_t n) {
    while (n > 24) {
        uint64_t a = v[0], b = v[n/2], c = v[n-1], p;
        p = a < b ? (b < c ? b : (a < c ? c : a)) : (a < c ? a : (b < c ? c : b));
        uint32_t i = 0, j = n - 1;
        for (;;) {
            while (v[i] < p) i++;
            while (v[j] > p) j--;
            if (i >= j) break;
            uint64_t t = v[i]; v[i] = v[j]; v[j] = t; i++; if (j) j--;
        }
        if (j + 1 < n - j - 1) { sort_promise(v, j + 1); v += j + 1; n -= j + 1; }
        else                   { sort_promise(v + j + 1, n - j - 1); n = j + 1; }
    }
    for (uint32_t k = 1; k < n; k++) {
        uint64_t x = v[k]; uint32_t m = k;
        while (m && v[m-1] > x) { v[m] = v[m-1]; m--; }
        v[m] = x;
    }
}

/* Sort one phase's cells by descending promise (ties by record word). */
static void sort_phase(bool inner_phase, const char *name) {
    double t0 = omp_get_wtime();
    if (g_db_max_cell_n == 0) return;
    int nt = g_nthreads > 0 ? g_nthreads : omp_get_max_threads();
    const int rb = inner_phase ? g_rec_bytes_inner : g_rec_bytes_edge;
    #pragma omp parallel num_threads(nt)
    {
        uint64_t *buf = xmalloc((size_t)g_db_max_cell_n * sizeof(uint64_t));
        /* collapse(3): 4913 chunks for the dynamic schedule instead of 289.
           Cell populations vary by orders of magnitude, so a coarse split
           leaves threads idle waiting on whoever drew the heavy chunk. Each
           cell is sorted independently of every other, so nothing else changes
           -- and collapsed loops must be perfectly nested, which is why
           la_color is computed inside the body rather than between them. */
        #pragma omp for collapse(3) schedule(dynamic, 1)
        for (int la = 0; la < DIM_INNER; la++)
        for (int b1 = 0; b1 < DIM_INNER; b1++)
        for (int b2 = 0; b2 < DIM_INNER; b2++) {
            int la_color = la + COLOR_MIN;
            for (int b3=0;b3<DIM_INNER;b3++)
            for (int b4=0;b4<DIM_INNER;b4++) for (int b5=0;b5<DIM_B5;b5++) {
                if (color_is_inner(b5) != inner_phase) continue;
                Cell *cell = g_db[la][b1][b2][b3][b4][b5];
                if (!cell) continue;
                /* raw bottom colors of this cell's chain (b5 inner-or-edge raw). */
                int bvals[CHAIN_LEN] = { b1+COLOR_MIN, b2+COLOR_MIN, b3+COLOR_MIN, b4+COLOR_MIN, b5 };
                for (uint32_t j = 0; j < cell->n; j++) {
                    uint32_t w = rec_load(cell->rec, j, rb);
                    int tops_idx[4]; int tops4raw;
                    int cl = la_color;
                    if (inner_phase) {
                        uint8_t f[CHAIN_LEN]; unpack_inner(w, f, g_lb_bits);
                        int top4 = 0;
                        for (int k = 0; k < CHAIN_LEN; k++) {
                            int ci = g_lb_bucket[cl][bvals[k]][f[k]];
                            if (k < 4) tops_idx[k] = INNER_IDX(g_cat[ci].top);
                            else top4 = g_cat[ci].top;
                            cl = g_cat[ci].right;
                        }
                        tops4raw = top4;
                    } else {
                        uint8_t f4[CHAIN_LEN-1]; int term_k;
                        unpack_edge(w, f4, &term_k, g_lb_bits, g_term_bits);
                        for (int k = 0; k < CHAIN_LEN-1; k++) {
                            int ci = g_lb_bucket[cl][bvals[k]][f4[k]];
                            tops_idx[k] = INNER_IDX(g_cat[ci].top);
                            cl = g_cat[ci].right;
                        }
                        int t = g_edge_term_by_left[cl][term_k];
                        tops4raw = g_edge_term[t].top;          /* edge-interface */
                    }
                    uint64_t fo = g_fanout[fan_flat(tops_idx[0], tops_idx[1],
                                                    tops_idx[2], tops_idx[3], tops4raw)];
                    buf[j] = promise_key(fo > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)fo, w);
                }
                sort_promise(buf, cell->n);
                for (uint32_t j = 0; j < cell->n; j++)
                    rec_store(cell->rec, j, rb, (uint32_t)buf[j]);
            }
        }
        free(buf);
    }
    double dt = omp_get_wtime() - t0;
    if (inner_phase) g_time_db_sort = dt;
    if (g_verbose || inner_phase) { printf("[init] %s sorted by fan-out (%.1fs)\n", name, dt); fflush(stdout); }
}

void sort_db_by_fanout(void) { sort_phase(true, "DB inner"); }

/* Edge cells: build, refresh their fan-out slots, sort. Fixed mode calls this
 * once per border row; free mode once per run. */
void build_db_edge_and_sort(void) {
    build_db_edge();
    update_fanout(false);
    sort_phase(false, "DB edge");
}

/* -- Pinned segment enumeration --------------------------------------------- */

typedef struct {
    const uint8_t *bottoms;
    const uint64_t *forbid;
    uint16_t (*out)[CHAIN_LEN];
    int  len, pin_idx, pin_kind, max_out, n;
    uint16_t pin_val;
    uint16_t cur[CHAIN_LEN];
    uint64_t seg[4];                 /* pieces used inside this segment */
} SegWalk;

static void seg_walk(SegWalk *w, int i, int cl) {
    if (w->n >= w->max_out) return;
    if (i == w->len) {
        memcpy(w->out[w->n++], w->cur, (size_t)w->len * sizeof(uint16_t));
        return;
    }
    int b = w->bottoms[i];
    if (cl < 0 || cl >= NUM_COLORS_TOTAL || b < 0 || b >= NUM_COLORS_TOTAL) return;

    if (i == w->pin_idx && w->pin_kind == PIN_PIECE) {
        const Oriented *o = &g_cat[w->pin_val];
        if (o->left != cl || o->bottom != b) return;      /* the cheap early kill */
        uint16_t pid = o->piece_id; uint64_t bit = piece_bit(pid);
        if (w->seg[pid >> 6] & bit) return;               /* not forbid: see header */
        w->seg[pid >> 6] |= bit; w->cur[i] = w->pin_val;
        seg_walk(w, i + 1, o->right);
        w->seg[pid >> 6] &= ~bit;
        return;
    }
    const bool top_pin = (i == w->pin_idx && w->pin_kind == PIN_TOPCOLOR);
    const int nb = g_lb_count[cl][b];
    for (int k = 0; k < nb && w->n < w->max_out; k++) {
        int ci = g_lb_bucket[cl][b][k];
        const Oriented *o = &g_cat[ci];
        if (top_pin && o->top != w->pin_val) continue;
        uint16_t pid = o->piece_id; uint64_t bit = piece_bit(pid);
        if ((w->forbid[pid >> 6] | w->seg[pid >> 6]) & bit) continue;
        w->seg[pid >> 6] |= bit; w->cur[i] = (uint16_t)ci;
        seg_walk(w, i + 1, o->right);
        w->seg[pid >> 6] &= ~bit;
    }
}

int enumerate_pinned_segment(int la, const uint8_t bottoms[], int len,
                             int pin_idx, int pin_kind, uint16_t pin_val,
                             const uint64_t forbid[4],
                             uint16_t (*out)[CHAIN_LEN], int max_out) {
    if (len < 1 || len > CHAIN_LEN || max_out <= 0) return 0;
    SegWalk w;
    w.bottoms = bottoms; w.forbid = forbid; w.out = out;
    w.len = len; w.pin_idx = pin_idx; w.pin_kind = pin_kind;
    w.max_out = max_out; w.n = 0; w.pin_val = pin_val;
    w.seg[0] = w.seg[1] = w.seg[2] = w.seg[3] = 0;
    seg_walk(&w, 0, la);
    return w.n;
}

/* -- Inner-DB disk cache ---------------------------------------------------- */
/* The inner cells (and their promise sort) depend only on the seed file, so
 * they can be built once and reused across runs. File layout:
 *   [DbCacheHdr][ncells x DbCacheCell][pad to 4096][arena blob]
 * The per-cell (flat index, n) table lets the loader rebuild the g_db pointers
 * without touching the arena, so the blob is mapped read-only and pages fault
 * in lazily as the search visits cells. */

#define DB_CACHE_MAGIC   0x3142764244353545ULL   /* "E55DBvB1" */
/* Version 2: cell offsets are rounded up to _Alignof(Cell) (see cell_stride in
   E555_database.h). A version-1 file packs them tightly, so every cell after the
   first odd-length one sits at a different offset -- the loader must reject it
   and rebuild rather than map the arena at the wrong stride. */
/* Version 3: the header carries a hash of g_db_exclude. A clue-built cache has
   the same seed, lb_bits and rec_bytes as a normal one but different CONTENTS
   (the excluded pieces appear in no chain), so nothing else would catch it. */
#define DB_CACHE_VERSION 3u
#define DB_CACHE_ALIGN   4096u

typedef struct {
    uint64_t magic;
    uint32_t version, lb_bits, rec_bytes, max_cell_n;
    uint64_t seed_hash;
    uint64_t ncells;
    uint64_t arena_bytes;
    uint64_t arena_off;
    uint64_t exclude_hash;
} DbCacheHdr;

/* Identifies the g_db_exclude set a cache was built with. */
static uint64_t db_exclude_hash(void) {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for (int k = 0; k < 4; k++) { h ^= g_db_exclude[k]; h *= 0x100000001B3ULL; }
    return h;
}

typedef struct { uint64_t fi; uint32_t n; uint32_t _pad; } DbCacheCell;

void db_cache_save(const char *path) {
    double t0 = omp_get_wtime();
    Cell **flat = &g_db[0][0][0][0][0][0];
    uint64_t ncells = 0;
    for (uint64_t fi = 0; fi < NCELL_DB; fi++)
        if (color_is_inner((int)(fi % DIM_B5)) && flat[fi]) ncells++;

    DbCacheCell *tab = xmalloc(ncells * sizeof(DbCacheCell));
    uint64_t k = 0;
    for (uint64_t fi = 0; fi < NCELL_DB; fi++) {
        if (!color_is_inner((int)(fi % DIM_B5)) || !flat[fi]) continue;
        tab[k].fi = fi; tab[k].n = flat[fi]->n; tab[k]._pad = 0; k++;
    }

    DbCacheHdr hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = DB_CACHE_MAGIC;     hdr.version = DB_CACHE_VERSION;
    hdr.lb_bits = (uint32_t)g_lb_bits; hdr.rec_bytes = (uint32_t)g_rec_bytes_inner;
    hdr.max_cell_n = g_db_max_cell_n;
    hdr.seed_hash = g_seed_file_hash;  hdr.ncells = ncells;
    hdr.arena_bytes = g_inner_arena_size;
    hdr.exclude_hash = db_exclude_hash();
    uint64_t meta = sizeof hdr + ncells * sizeof(DbCacheCell);
    hdr.arena_off = (meta + DB_CACHE_ALIGN - 1) / DB_CACHE_ALIGN * DB_CACHE_ALIGN;

    /* Write to a temp file and rename, so an interrupted save never leaves a
       plausible-looking truncated cache behind. */
    char tmp[1152];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) fatal("cannot create %s: %s", tmp, strerror(errno));
    if (fwrite(&hdr, sizeof hdr, 1, f) != 1) fatal("db cache: header write failed");
    if (fwrite(tab, sizeof(DbCacheCell), ncells, f) != ncells) fatal("db cache: table write failed");
    free(tab);
    static const uint8_t zeros[DB_CACHE_ALIGN] = {0};
    if (fwrite(zeros, 1, hdr.arena_off - meta, f) != hdr.arena_off - meta)
        fatal("db cache: pad write failed");
    for (uint64_t off = 0; off < g_inner_arena_size; ) {
        size_t chunk = g_inner_arena_size - off;
        if (chunk > (64u<<20)) chunk = 64u<<20;
        if (fwrite(g_inner_arena + off, 1, chunk, f) != chunk)
            fatal("db cache: arena write failed at %" PRIu64 " (%s)", off, strerror(errno));
        off += chunk;
    }
    if (fclose(f) != 0) fatal("db cache: close failed: %s", strerror(errno));
    if (rename(tmp, path) != 0) fatal("db cache: rename to %s failed: %s", path, strerror(errno));
    printf("[init] DB cache written: %s (%.2f GB, %.1fs)\n",
           path, (double)(hdr.arena_off + g_inner_arena_size)/1e9, omp_get_wtime()-t0);
    fflush(stdout);
}

/* Undo a partially rebuilt pointer array. Every post-mapping failure in
   db_cache_load MUST call this before unmapping: the loop below assigns
   flat[fi] as it walks the index table, and db_layout only ever WRITES the
   cells it finds non-empty -- it never clears the others. So a stale pointer
   left here survives the rebuild that follows, and update_fanout dereferences
   every non-NULL entry it finds. A truncated cache file would then be a
   segfault at startup instead of the clean rebuild it is meant to be. */
static void db_clear_inner_cells(void) {
    Cell **flat = &g_db[0][0][0][0][0][0];
    for (uint64_t fi = 0; fi < NCELL_DB; fi++)
        if (color_is_inner((int)(fi % DIM_B5))) flat[fi] = NULL;
}

bool db_cache_load(const char *path) {
    double t0 = omp_get_wtime();
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;                       /* no cache yet: silent miss */

    DbCacheHdr hdr;
    ssize_t r = pread(fd, &hdr, sizeof hdr, 0);
    if (r != (ssize_t)sizeof hdr || hdr.magic != DB_CACHE_MAGIC || hdr.version != DB_CACHE_VERSION) {
        printf("[init] DB cache %s: bad header; rebuilding\n", path); close(fd); return false;
    }
    if (hdr.seed_hash != g_seed_file_hash) {
        printf("[init] DB cache %s: different seed file; rebuilding\n", path); close(fd); return false;
    }
    if (hdr.exclude_hash != db_exclude_hash()) {
        printf("[init] DB cache %s: built with a different clue/exclusion set; "
               "rebuilding (use a separate --db_file per clue setting)\n", path);
        close(fd); return false;
    }
    if (hdr.lb_bits != (uint32_t)g_lb_bits || hdr.rec_bytes != (uint32_t)g_rec_bytes_inner) {
        printf("[init] DB cache %s: record format mismatch; rebuilding\n", path); close(fd); return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size != hdr.arena_off + hdr.arena_bytes
        || hdr.ncells == 0 || hdr.ncells > NCELL_DB) {
        printf("[init] DB cache %s: wrong size; rebuilding\n", path); close(fd); return false;
    }

    DbCacheCell *tab = xmalloc(hdr.ncells * sizeof(DbCacheCell));
    uint64_t tab_bytes = hdr.ncells * sizeof(DbCacheCell);
    for (uint64_t off = 0; off < tab_bytes; ) {
        ssize_t got = pread(fd, (uint8_t *)tab + off, tab_bytes - off, (off_t)(sizeof hdr + off));
        if (got <= 0) { printf("[init] DB cache %s: table read failed; rebuilding\n", path);
                        free(tab); close(fd); return false; }
        off += (uint64_t)got;
    }

    uint8_t *arena = mmap(NULL, hdr.arena_bytes, PROT_READ, MAP_PRIVATE, fd, (off_t)hdr.arena_off);
    close(fd);                                      /* mapping stays valid */
    if (arena == MAP_FAILED) {
        printf("[init] DB cache %s: mmap failed (%s); rebuilding\n", path, strerror(errno));
        free(tab); return false;
    }

    /* Rebuild the pointer array, the fan-out table and the per-la totals from
       the index table alone: no arena page is touched, so the mapping stays
       lazy and the search faults in only the cells it actually visits. */
    if (!g_fanout) {
        g_fanout = xmalloc(FANOUT_N * sizeof(uint64_t));
        memset(g_fanout, 0, FANOUT_N * sizeof(uint64_t));
    }
    memset(g_la_total_inner, 0, sizeof g_la_total_inner);
    Cell **flat = &g_db[0][0][0][0][0][0];
    uint64_t off = 0, recs = 0;
    for (uint64_t j = 0; j < hdr.ncells; j++) {
        uint64_t fi = tab[j].fi; uint32_t n = tab[j].n;
        if (fi >= NCELL_DB || !color_is_inner((int)(fi % DIM_B5)) || n == 0) {
            printf("[init] DB cache %s: corrupt table; rebuilding\n", path);
            db_clear_inner_cells();
            munmap(arena, hdr.arena_bytes); free(tab); return false;
        }
        flat[fi] = (Cell *)(arena + off);
        off += cell_stride(n, g_rec_bytes_inner);
        recs += n;
        g_fanout[fi % FANOUT_N] += n;
        g_la_total_inner[fi / FANOUT_N] += n;
    }
    free(tab);
    if (off != hdr.arena_bytes) {
        printf("[init] DB cache %s: arena size mismatch; rebuilding\n", path);
        db_clear_inner_cells();
        munmap(arena, hdr.arena_bytes);
        return false;
    }

    g_inner_arena = arena; g_inner_arena_size = hdr.arena_bytes;
    if (hdr.max_cell_n > g_db_max_cell_n) g_db_max_cell_n = hdr.max_cell_n;
    printf("[init] DB inner loaded from cache %s: %" PRIu64 " records  %" PRIu64 " cells  %.2f GB  (%.1fs)\n",
           path, recs, hdr.ncells, (double)hdr.arena_bytes/1e9, omp_get_wtime()-t0);
    fflush(stdout);
    return true;
}

/* -- Border enumeration ---------------------------------------------------- */

static void push_bottom(const BottomOrder *b) {
    static size_t cap = 0;
    if (g_bottom_n == cap) { cap = cap ? cap*2 : 1024; g_bottoms = xrealloc(g_bottoms, cap*sizeof(BottomOrder)); }
    g_bottoms[g_bottom_n++] = *b;
}
static void push_left(const LeftOrder *l) {
    static size_t cap = 0;
    if (g_left_n == cap) { cap = cap ? cap*2 : 1024; g_lefts = xrealloc(g_lefts, cap*sizeof(LeftOrder)); }
    g_lefts[g_left_n++] = *l;
}

/* Bottom row: a horizontal Euler trail cBL -> ... -> cBR, each piece's left
 * matching the previous piece's right. */
static void rec_bottom(int col, int prev_right, BottomOrder *b, uint64_t used[4]) {
    if (col == PUZZLE_SIDE-1) {
        if (prev_right != g_cBR.left) return;
        b->p[col] = &g_cBR; b->rtop0[col] = g_cBR.top;
        used_set(used, g_cBR.piece_id);
        memcpy(b->used, used, sizeof b->used);
        push_bottom(b);
        used_clear(used, g_cBR.piece_id);
        return;
    }
    for (int i = 0; i < g_bottom_count; i++) {
        const Oriented *p = &g_bottom_pool[i];
        if (used_test(used, p->piece_id)) continue;
        if (p->left != prev_right) continue;
        used_set(used, p->piece_id);
        b->p[col] = p; b->rtop0[col] = p->top;
        rec_bottom(col+1, p->right, b, used);
        used_clear(used, p->piece_id);
    }
}

void enumerate_bottoms(void) {
    g_bottom_n = 0;
    BottomOrder b; memset(&b, 0, sizeof b);
    b.p[0] = &g_cBL; b.rtop0[0] = g_cBL.top;
    uint64_t used[4] = {0,0,0,0};
    used_set(used, g_cBL.piece_id);
    rec_bottom(1, g_cBL.right, &b, used);
}

/* Left column: a vertical Euler trail cBL -> ... -> cTL, each piece's bottom
 * matching the piece-below's top. right[r] is the inner color exposed to col 1. */
static void rec_left(int row, int prev_top, LeftOrder *l, uint64_t used[4]) {
    if (row == PUZZLE_SIDE-1) {
        if (prev_top != g_cTL.bottom) return;
        l->p[row] = &g_cTL; l->right[row] = g_cTL.right;
        used_set(used, g_cTL.piece_id);
        memcpy(l->used, used, sizeof l->used);
        push_left(l);
        used_clear(used, g_cTL.piece_id);
        return;
    }
    for (int i = 0; i < g_left_count_pool; i++) {
        const Oriented *p = &g_left_pool[i];
        if (used_test(used, p->piece_id)) continue;
        if (p->bottom != prev_top) continue;
        used_set(used, p->piece_id);
        l->p[row] = p; l->right[row] = p->right;
        rec_left(row+1, p->top, l, used);
        used_clear(used, p->piece_id);
    }
}

void enumerate_lefts(void) {
    g_left_n = 0;
    LeftOrder l; memset(&l, 0, sizeof l);
    l.p[0] = &g_cBL; l.right[0] = g_cBL.right;
    uint64_t used[4] = {0,0,0,0};
    used_set(used, g_cBL.piece_id);
    rec_left(1, g_cBL.top, &l, used);
}

/* -- Ranking --------------------------------------------------------------- */

/* Rank descending; ties broken by the ordering's content so equal-rank orders
   keep a stable relative order across qsort implementations. */
static int cmp_bottom_rank(const void *a, const void *b) {
    const BottomOrder *x = a, *y = b;
    if (x->rank != y->rank) return (x->rank < y->rank) ? 1 : -1;
    return memcmp(x->rtop0, y->rtop0, sizeof x->rtop0);
}
static int cmp_left_rank(const void *a, const void *b) {
    const LeftOrder *x = a, *y = b;
    if (x->rank != y->rank) return (x->rank < y->rank) ? 1 : -1;
    return memcmp(x->right, y->right, sizeof x->right);
}

/* Log fan-out of the three row-1 segments a bottom row presents (rt[15] is the
 * bottom-right corner's edge-interface top). Shared by ranking and sampling. */
static double bottom_rank_of(const int rt[PUZZLE_SIDE]) {
    uint64_t a = db_seg_fanout(rt[1],rt[2],rt[3],rt[4],  rt[5]);
    uint64_t b = db_seg_fanout(rt[6],rt[7],rt[8],rt[9],  rt[10]);
    uint64_t c = db_seg_fanout(rt[11],rt[12],rt[13],rt[14], rt[15]);
    return log1p((double)a) + log1p((double)b) + log1p((double)c);
}

/* Rank a bottom row by the log fan-out of the three row-1 segments it presents.
   At tau > 0 the stored rank is the Gumbel-perturbed key, so taking the first
   --top_bottoms entries samples that many DISTINCT bottoms without replacement
   in proportion to exp(rank/tau) instead of skimming the greedy head. */
void rank_bottoms(double tau, RNG *rng) {
    for (size_t i = 0; i < g_bottom_n; i++)
        g_bottoms[i].rank = gumbel_key(bottom_rank_of(g_bottoms[i].rtop0), tau, rng);
    qsort(g_bottoms, g_bottom_n, sizeof(BottomOrder), cmp_bottom_rank);
}

/* Per-left_color record totals: inner part precomputed at DB build/load; edge
 * part summed here (edge cells are few and change per edge-pool rebuild). */
static void compute_la_totals(uint64_t la_total[DIM_INNER]) {
    memcpy(la_total, g_la_total_inner, DIM_INNER * sizeof(uint64_t));
    Cell **flat = &g_db[0][0][0][0][0][0];
    /* b5 is the last index and the non-inner colors are exactly 0..COLOR_MIN-1,
       so the edge cells are the first COLOR_MIN slots of every DIM_B5-slot
       group. Walking that comb visits 6 slots in every 23 instead of testing
       all 32.6 M -- the same shape clear_edge_cells uses. */
    for (uint64_t base = 0; base < NCELL_DB; base += DIM_B5) {
        uint64_t la = base / FANOUT_N;      /* FANOUT_N is a multiple of DIM_B5,
                                               so one division per group */
        for (uint64_t b5 = 0; b5 < COLOR_MIN; b5++) {
            Cell *c = flat[base + b5];
            if (c) la_total[la] += c->n;
        }
    }
}

/* How connectable a left column's exposed inner colors are: the total DB
 * records keyed by each left_color it presents to col 1 (rows 1..14). This is
 * the "rotate the side to the bottom and score with the DB" heuristic. */
static double left_rank_of(const int right[PUZZLE_SIDE], const uint64_t la_total[DIM_INNER]) {
    double s = 0.0;
    for (int r = 1; r <= EDGE_LEN; r++)
        if (color_is_inner(right[r])) s += log1p((double)la_total[INNER_IDX(right[r])]);
    return s;
}

/* At tau > 0, as rank_bottoms. This measure needs the perturbation more than
   any other in the project: it is a sum over rows, so it is SYMMETRIC in the
   row index -- two columns exposing the same colour multiset score identically
   however they order it. The tie classes are therefore large, cmp_left_rank
   settles them by memcmp, and taking the top L hands the sweep L
   lexicographically adjacent columns. Perturbing the key breaks those ties
   uniformly, which is most of the benefit here. */
void rank_lefts(double tau, RNG *rng) {
    uint64_t la_total[DIM_INNER];
    compute_la_totals(la_total);
    for (size_t i = 0; i < g_left_n; i++)
        g_lefts[i].rank = gumbel_key(left_rank_of(g_lefts[i].right, la_total), tau, rng);
    qsort(g_lefts, g_left_n, sizeof(LeftOrder), cmp_left_rank);
}

/* -- Random border sampling (--random_edges mode) --------------------------- */
/* Borders are drawn directly from the canonical seed's edge pool -- no Stage A
 * rotation row is involved. A bottom sample assigns the four corner roles at
 * random and grows a random legal chain of 14 frame-down edges from BL to BR
 * (randomized greedy with restarts); a left sample grows a frame-left chain
 * from the shared BL corner to the TL corner, using only edges the bottom did
 * not consume. Each published border is the best of RANDOM_SIDE_SAMPLES
 * independent samples under the same fan-out ranks as the enumerated borders.
 * The winner's pieces are copied into static storage so BottomOrder/LeftOrder
 * pointers stay valid for the duration of the configuration. */

#define RAND_BORDER_MAX_RESTARTS 100000

static int      s_side_edge_ids[MAX_EDGE_TERMINALS];
static Oriented s_edge_down[MAX_EDGE_TERMINALS];    /* frame at the bottom */
static Oriented s_edge_leftz[MAX_EDGE_TERMINALS];   /* frame on the left  */
static int      s_side_edge_n = 0;
static int      s_corner_ids[4];
static int      s_corner_n = 0;
static int      s_tl_id = -1, s_tr_id = -1;         /* corners left for the top */
static Oriented s_rand_bottom[PUZZLE_SIDE], s_rand_left[PUZZLE_SIDE];

static void build_side_pools(void) {
    if (s_side_edge_n) return;
    for (int i = 0; i < NUM_PIECES; i++) {
        int z = canonical_zero_count(i);
        if (z == 1) {
            if (s_side_edge_n >= MAX_EDGE_TERMINALS) fatal("edge pool overflow");
            int k = s_side_edge_n++;
            s_side_edge_ids[k] = i;
            if (!orient_with_zero_side((uint16_t)i, 2, &s_edge_down[k]) ||
                !orient_with_zero_side((uint16_t)i, 3, &s_edge_leftz[k]))
                fatal("edge piece %d cannot be oriented frame-down/left", i);
        } else if (z == 2) {
            if (s_corner_n >= 4) fatal("more than 4 corner pieces");
            s_corner_ids[s_corner_n++] = i;
        }
    }
    if (s_corner_n != 4) fatal("seed has %d corner pieces; expected 4", s_corner_n);
}

/* Orient a corner piece for a board corner role. Roles: 0=BL 1=BR 2=TL 3=TR. */
static bool orient_corner_role(int pid, int role, Oriented *out) {
    for (uint8_t spin = 0; spin < 4; spin++) {
        Oriented o = make_oriented((uint16_t)pid, spin);
        bool ok = (role == 0) ? (o.bottom == 0 && o.left  == 0)
                : (role == 1) ? (o.bottom == 0 && o.right == 0)
                : (role == 2) ? (o.top    == 0 && o.left  == 0)
                :               (o.top    == 0 && o.right == 0);
        if (ok) { *out = o; return true; }
    }
    return false;
}

/* Board-corner role name for diagnostics. Roles: 0=BL 1=BR 2=TL 3=TR. */
static const char *corner_role_name(int role) {
    return role == 0 ? "BL" : role == 1 ? "BR" : role == 2 ? "TL" : "TR";
}

/* Validate/resolve any user-pinned corners (g_fixed_corner_pid). Each pinned
   role must name a genuine, distinct corner piece orientable into that role;
   with exactly three pinned the fourth is forced to the one remaining corner. */
void finalize_fixed_corners(void) {
    build_side_pools();                       /* fills s_corner_ids[4]; idempotent */
    int nfixed = 0;
    for (int r = 0; r < 4; r++) {
        int pid = g_fixed_corner_pid[r];
        if (pid < 0) continue;
        nfixed++;
        bool is_corner = false;
        for (int k = 0; k < 4; k++) if (s_corner_ids[k] == pid) { is_corner = true; break; }
        if (!is_corner)
            fatal("--%s piece %d is not a corner (a corner has two frame sides)",
                  corner_role_name(r), pid);
        Oriented tmp;
        if (!orient_corner_role(pid, r, &tmp))
            fatal("--%s piece %d cannot be oriented into the %s corner",
                  corner_role_name(r), pid, corner_role_name(r));
        for (int q = 0; q < r; q++)
            if (g_fixed_corner_pid[q] == pid)
                fatal("corner piece %d is pinned to both --%s and --%s",
                      pid, corner_role_name(q), corner_role_name(r));
    }
    if (nfixed == 3) {
        int free_role = -1;
        for (int r = 0; r < 4; r++) if (g_fixed_corner_pid[r] < 0) { free_role = r; break; }
        for (int k = 0; k < 4; k++) {
            int cid = s_corner_ids[k];
            bool taken = false;
            for (int r = 0; r < 4; r++) if (g_fixed_corner_pid[r] == cid) { taken = true; break; }
            if (!taken) { g_fixed_corner_pid[free_role] = cid; break; }
        }
    }
    if (nfixed > 0) {
        printf("[cfg] corners:");
        for (int r = 0; r < 4; r++) {
            if (g_fixed_corner_pid[r] >= 0) printf(" %s=%d", corner_role_name(r), g_fixed_corner_pid[r]);
            else                            printf(" %s=free", corner_role_name(r));
        }
        printf("\n"); fflush(stdout);
    }
}

/* One random bottom-row attempt: BL/BR corners (the other two corners become
 * TL/TR), any user-pinned roles kept fixed and the rest assigned at random,
 * then a random legal left-right chain of 14 frame-down edges. Returns false
 * when the chain dead-ends (caller restarts). */
static bool attempt_random_bottom(RNG *rng, Oriented seq[PUZZLE_SIDE],
                                  int *tl_id, int *tr_id) {
    /* Assign corner ids to roles (0=BL 1=BR 2=TL 3=TR): pinned roles keep their
       piece; the remaining corners are shuffled into the free roles. With no pin
       this is a plain shuffle of all four corners (unchanged behaviour). */
    int rem[4], nrem = 0;
    for (int k = 0; k < 4; k++) {
        int cid = s_corner_ids[k];
        bool pinned = false;
        for (int r = 0; r < 4; r++) if (g_fixed_corner_pid[r] == cid) { pinned = true; break; }
        if (!pinned) rem[nrem++] = cid;
    }
    for (int k = 0; k + 1 < nrem; k++) {
        int j = k + (int)rng_uniform(rng, (uint32_t)(nrem - k));
        int t = rem[k]; rem[k] = rem[j]; rem[j] = t;
    }
    int role_pid[4], ri = 0;
    for (int r = 0; r < 4; r++)
        role_pid[r] = (g_fixed_corner_pid[r] >= 0) ? g_fixed_corner_pid[r] : rem[ri++];

    Oriented BL, BR;
    if (!orient_corner_role(role_pid[0], 0, &BL)) return false;
    if (!orient_corner_role(role_pid[1], 1, &BR)) return false;
    *tl_id = role_pid[2];
    *tr_id = role_pid[3];
    seq[0] = BL; seq[PUZZLE_SIDE-1] = BR;

    uint64_t used[4] = {0,0,0,0};
    used_set(used, BL.piece_id); used_set(used, BR.piece_id);
    int prev = BL.right;
    for (int col = 1; col <= EDGE_LEN; col++) {
        int cand[MAX_EDGE_TERMINALS], nc = 0;
        for (int k = 0; k < s_side_edge_n; k++) {
            const Oriented *o = &s_edge_down[k];
            if (used_test(used, o->piece_id)) continue;
            if (o->left != prev) continue;
            if (col == EDGE_LEN && o->right != BR.left) continue;
            cand[nc++] = k;
        }
        if (nc == 0) return false;
        const Oriented *o = &s_edge_down[cand[rng_uniform(rng, (uint32_t)nc)]];
        seq[col] = *o;
        used_set(used, o->piece_id);
        prev = o->right;
    }
    return true;
}

bool sample_random_bottom(RNG *rng, double tau, BottomOrder *out) {
    build_side_pools();
    /* have_best, not a negative sentinel: at tau > 0 the comparison key is
       rank/tau + Gumbel, which is below zero about a third of the time. */
    Oriented best[PUZZLE_SIDE]; double best_key = 0.0, best_rank = 0.0;
    bool have_best = false;
    int best_tl = -1, best_tr = -1;
    uint64_t restarts = 0;
    for (int got = 0; got < RANDOM_SIDE_SAMPLES; ) {
        Oriented seq[PUZZLE_SIDE]; int tl, tr;
        if (!attempt_random_bottom(rng, seq, &tl, &tr)) {
            if (++restarts > RAND_BORDER_MAX_RESTARTS) break;
            continue;
        }
        got++;
        int rt[PUZZLE_SIDE];
        for (int c = 0; c < PUZZLE_SIDE; c++) rt[c] = seq[c].top;
        double rank = bottom_rank_of(rt);
        double key  = gumbel_key(rank, tau, rng);
        if (!have_best || key > best_key) {
            memcpy(best, seq, sizeof best);
            best_key = key; best_rank = rank; have_best = true;
            best_tl = tl; best_tr = tr;
        }
    }
    if (!have_best) return false;

    /* Publish: corners to the shared globals, edges to stable static storage. */
    g_cBL = best[0];              g_has_cBL = true;
    g_cBR = best[PUZZLE_SIDE-1];  g_has_cBR = true;
    s_tl_id = best_tl; s_tr_id = best_tr;
    memset(out, 0, sizeof *out);
    for (int c = 0; c < PUZZLE_SIDE; c++) {
        s_rand_bottom[c] = best[c];
        out->p[c] = (c == 0) ? &g_cBL : (c == PUZZLE_SIDE-1) ? &g_cBR : &s_rand_bottom[c];
        out->rtop0[c] = best[c].top;
        used_set(out->used, best[c].piece_id);
    }
    out->rank = best_rank;
    return true;
}

bool sample_random_left(RNG *rng, double tau, const BottomOrder *bot, LeftOrder *out) {
    Oriented TL, TR;
    if (s_tl_id < 0 || !orient_corner_role(s_tl_id, 2, &TL) || !orient_corner_role(s_tr_id, 3, &TR))
        fatal("sample_random_left called before sample_random_bottom");
    uint64_t la_total[DIM_INNER];
    compute_la_totals(la_total);

    /* have_best: see sample_random_bottom -- the Gumbel key goes negative. */
    Oriented best[PUZZLE_SIDE]; double best_key = 0.0, best_rank = 0.0;
    bool have_best = false;
    uint64_t restarts = 0;
    for (int got = 0; got < RANDOM_SIDE_SAMPLES; ) {
        Oriented seq[PUZZLE_SIDE];
        seq[0] = g_cBL; seq[PUZZLE_SIDE-1] = TL;
        uint64_t used[4];
        memcpy(used, bot->used, sizeof used);       /* bottom's edges are taken */
        used_set(used, TL.piece_id);
        int prev_top = g_cBL.top;
        bool ok = true;
        for (int row = 1; row <= EDGE_LEN; row++) {
            int cand[MAX_EDGE_TERMINALS], nc = 0;
            for (int k = 0; k < s_side_edge_n; k++) {
                const Oriented *o = &s_edge_leftz[k];
                if (used_test(used, o->piece_id)) continue;
                if (o->bottom != prev_top) continue;
                if (row == EDGE_LEN && o->top != TL.bottom) continue;
                cand[nc++] = k;
            }
            if (nc == 0) { ok = false; break; }
            const Oriented *o = &s_edge_leftz[cand[rng_uniform(rng, (uint32_t)nc)]];
            seq[row] = *o;
            used_set(used, o->piece_id);
            prev_top = o->top;
        }
        if (!ok) {
            if (++restarts > RAND_BORDER_MAX_RESTARTS) break;
            continue;
        }
        got++;
        int right[PUZZLE_SIDE];
        for (int r = 0; r < PUZZLE_SIDE; r++) right[r] = seq[r].right;
        double rank = left_rank_of(right, la_total);
        double key  = gumbel_key(rank, tau, rng);
        if (!have_best || key > best_key) {
            memcpy(best, seq, sizeof best);
            best_key = key; best_rank = rank; have_best = true;
        }
    }
    if (!have_best) return false;

    g_cTL = TL; g_has_cTL = true;
    g_cTR = TR; g_has_cTR = true;
    memset(out, 0, sizeof *out);
    for (int r = 0; r < PUZZLE_SIDE; r++) {
        s_rand_left[r] = best[r];
        out->p[r] = (r == 0) ? &g_cBL : (r == PUZZLE_SIDE-1) ? &g_cTL : &s_rand_left[r];
        out->right[r] = best[r].right;
        used_set(out->used, best[r].piece_id);
    }
    out->rank = best_rank;
    return true;
}

/* -- Border-row reader (one Stage A rotation row) -------------------------- */

bool read_one_border_row(const char *csv_path, uint32_t want, uint8_t spins[NUM_PIECES]) {
    FILE *f = fopen(csv_path, "r");
    if (!f) fatal("cannot open rotation CSV %s: %s", csv_path, strerror(errno));
    char *line = NULL; size_t linesz = 0; ssize_t len;
    int data_idx = -1; bool found = false;
    while ((len = getline(&line, &linesz, f)) >= 0) {
        bool nonempty = false;
        for (ssize_t i = 0; i < len; i++) {
            char ch = line[i];
            if (ch == '#' || ch == '%') break;
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') { nonempty = true; break; }
        }
        if (!nonempty) continue;
        data_idx++;
        if ((uint32_t)data_idx < want) continue;
        char *fields[300]; int nf = 0;
        char *copy = strdup(line); char *p = copy; char *tok;
        while ((tok = strsep(&p, ", \t\r\n")) != NULL) { if (*tok=='\0') continue; if (nf < 300) fields[nf++] = tok; }
        int offset = -1; uint8_t tmp;
        if      (nf == NUM_PIECES+1) offset = 1;
        else if (nf == NUM_PIECES+2) offset = 2;
        else if (nf == NUM_PIECES) { if (parse_spin_token(fields[0], &tmp)) offset = 0; else fatal("border CSV row %u: field 0 not a spin", want); }
        else fatal("border CSV row %u has %d fields; expected 256/257/258", want, nf);
        for (int i = 0; i < NUM_PIECES; i++)
            if (!parse_spin_token(fields[offset+i], &spins[i]))
                fatal("border CSV row %u: invalid spin for piece %d", want, i);
        free(copy); found = true; break;
    }
    free(line); fclose(f);
    return found;
}
