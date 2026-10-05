/*
 * E555_dive.c -- the end-dive engine. See E555_dive.h for the model and the API.
 *
 * A dive is the stuck mode of E555_backtracker.c: the most-constrained open
 * cell first, an exact fit when one exists, otherwise a placement from the
 * minimal-break class; within a class the least-constraining value (LCV), ties
 * at random. A dive never backtracks and cannot fail (piece types, and when
 * edges are held to their sides each frame side, are exactly balanced), so it
 * is cheap and every dive is a different board. Score = connected edges of 480.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <omp.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "E555_dive.h"

/* ============================================================================
 * Tuning (measured; see PROJECT_E555.md, "Finishing boards")
 * ========================================================================== */
#define DV_S1_SHARE      0.10   /* share of M spent in stage 1 */
#define DV_ROUNDS        18     /* stage-2 cross-entropy rounds */
#define DV_S2_BETA       2.0    /* stage-2 weight on the learned w */
#define DV_ELITE         0.05   /* share of a round's dives that vote */
#define DV_GAMMA         0.3f   /* learning rate */
#define DV_CLIP          2.0f   /* |vote| and |w| cap */
#define DV_LCV_CAP       8      /* candidates the LCV plays out */
#define DV_S2_MARGIN     4      /* stage 2 for stage-1 best >= S - this... */
#define DV_S2_MIN_SHARE  0.20   /* ...and if that promotes under this share, */
#define DV_S2_TOP_SHARE  0.10   /* also this top share by stage-1 best */
#define DV_POLISH_TOP    32     /* K: dives polished per board; K/2 walks (rounded up) */
#define DV_POLISH_MAX    256    /* largest K (diver --polish_top) */
#define DV_POLISH_MARGIN 6      /* on boards whose best is >= S - this */
#define DV_KICK          3      /* random swaps per kick */

#define DV_WORDS      ((NUM_PIECES * 4) / 64)
#define DV_CLASSES    9
#define DV_PLACED     0xFFFFu
#define DV_BREAK_CAP  4          /* a cell has 4 neighbours: this is "unlimited" */
#define DV_SEED_OPTS  256

static DvParams g_p;

/* Gumbel noise in single precision for the dive engine's tie-breaks and
   policies: the same distribution to float resolution (24-bit uniform, tail cut
   past 17, probability 3e-8) at a fraction of two double logs.  The beam keeps
   its own double gumbel_noise() in E555_database.h. */
static inline double dv_gumbel(RNG *r) {
    const float u = ((float)(rng_next(r) >> 40) + 0.5f) * (1.0f / 16777216.0f);
    return (double)(-logf(-logf(u)));
}

static inline bool dv_time_up(void) {
    return (g_p.stop && *g_p.stop) || (g_p.deadline > 0.0 && omp_get_wtime() >= g_p.deadline);
}

/* ============================================================================
 * Problem: orientations, fit masks, frame classes
 * ========================================================================== */
typedef struct { uint64_t w[DV_WORDS]; } DvMask;
typedef struct { Oriented c[NUM_PIECES]; } DvBoard;   /* piece_id DV_EMPTY = open */
typedef struct { uint16_t pid; uint8_t spin, breaks; } DvCand;

static Oriented g_dv_or[NUM_PIECES][4];
static uint8_t  g_dv_spins[NUM_PIECES];        /* distinct orientations, one bit each */
static DvMask   g_dv_fit[4][NUM_COLORS_TOTAL]; /* side d (t,r,b,l) has colour c */
static DvMask   g_dv_base[DV_CLASSES];         /* frame-legal orientations per class */
static uint16_t g_dv_pclass[NUM_PIECES];       /* classes a piece may occupy */

static inline int dv_piece_kind(int p) {       /* grey sides: 0 inner, 1 edge, 2 corner */
    return (g_seed_top[p] == 0) + (g_seed_right[p] == 0)
         + (g_seed_bottom[p] == 0) + (g_seed_left[p] == 0);
}
static uint8_t g_dv_clsof[NUM_PIECES];
static inline int dv_cls(int x) { return g_dv_clsof[x]; }
/* Neighbour of x toward d (0 up, 1 right, 2 down, 3 left), or -1. */
static inline int dv_nb(int x, int d) {
    int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
    switch (d) {
        case 0:  return r < PUZZLE_SIDE - 1 ? x + PUZZLE_SIDE : -1;
        case 1:  return c < PUZZLE_SIDE - 1 ? x + 1 : -1;
        case 2:  return r > 0 ? x - PUZZLE_SIDE : -1;
        default: return c > 0 ? x - 1 : -1;
    }
}
static inline int dv_side(const Oriented *o, int d) {
    return d == 0 ? o->top : d == 1 ? o->right : d == 2 ? o->bottom : o->left;
}
static inline void dv_set(DvMask *m, int bit) { m->w[bit >> 6] |= 1ULL << (bit & 63); }
static inline void dv_and(DvMask *d, const DvMask *s) {
    for (int i = 0; i < DV_WORDS; i++) d->w[i] &= s->w[i];
}
static inline int dv_ipop(const DvMask *a, const DvMask *b) {
    int n = 0;
    for (int i = 0; i < DV_WORDS; i++) n += __builtin_popcountll(a->w[i] & b->w[i]);
    return n;
}
/* May piece p sit at spin s in a cell of class cls? */
static inline bool dv_legal(int p, int s, int cls) {
    return (g_dv_spins[p] >> s & 1u) &&
           (g_dv_base[cls].w[(p * 4 + s) >> 6] >> ((p * 4 + s) & 63) & 1ULL);
}

static void dv_build_static(void) {
    for (int x = 0; x < NUM_PIECES; x++) {
        const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
        g_dv_clsof[x] = (uint8_t)(((r == 0) ? 1 : (r == PUZZLE_SIDE - 1) ? 2 : 0) * 3
                                + ((c == 0) ? 1 : (c == PUZZLE_SIDE - 1) ? 2 : 0));
    }
    memset(g_dv_fit, 0, sizeof g_dv_fit);
    for (int p = 0; p < NUM_PIECES; p++) {
        const int e[4] = { g_seed_top[p], g_seed_right[p], g_seed_bottom[p], g_seed_left[p] };
        g_dv_spins[p] = 0;
        for (int s = 0; s < 4; s++) {
            Oriented o;
            o.piece_id = (uint16_t)p; o.rotation = (uint8_t)s;
            o.top    = (uint8_t)e[(0 + s) & 3]; o.right = (uint8_t)e[(1 + s) & 3];
            o.bottom = (uint8_t)e[(2 + s) & 3]; o.left  = (uint8_t)e[(3 + s) & 3];
            g_dv_or[p][s] = o;
            bool dup = false;
            for (int t = 0; t < s; t++)
                if ((g_dv_spins[p] >> t & 1u) && g_dv_or[p][t].top == o.top &&
                    g_dv_or[p][t].right == o.right && g_dv_or[p][t].bottom == o.bottom)
                    dup = true;
            if (dup) continue;
            g_dv_spins[p] |= (uint8_t)(1u << s);
            const int bit = p * 4 + s;
            dv_set(&g_dv_fit[0][o.top], bit);    dv_set(&g_dv_fit[1][o.right], bit);
            dv_set(&g_dv_fit[2][o.bottom], bit); dv_set(&g_dv_fit[3][o.left], bit);
        }
    }
}

/* The nine cell classes of the frame rule (grey exactly on the outward sides).
   by_side further holds an edge piece to the side g_spin gives it. */
void dv_frame(bool by_side) {
    memset(g_dv_base, 0, sizeof g_dv_base);
    memset(g_dv_pclass, 0, sizeof g_dv_pclass);
    for (int p = 0; p < NUM_PIECES; p++) {
        int pside = -1;
        if (by_side && dv_piece_kind(p) == 1) {
            const Oriented *o = &g_dv_or[p][g_spin[p] & 3];
            for (int d = 0; d < 4; d++) if (dv_side(o, d) == 0) pside = d;
        }
        for (int s = 0; s < 4; s++) {
            if (!(g_dv_spins[p] >> s & 1u)) continue;
            const Oriented *o = &g_dv_or[p][s];
            for (int k = 0; k < DV_CLASSES; k++) {
                const int rk = k / 3, ck = k % 3;
                const bool out[4] = { rk == 2, ck == 2, rk == 1, ck == 1 };
                bool ok = true;
                int cside = -1, nout = 0;
                for (int d = 0; d < 4; d++) {
                    if ((dv_side(o, d) == 0) != out[d]) ok = false;
                    if (out[d]) { cside = d; nout++; }
                }
                if (ok && pside >= 0 && nout == 1 && cside != pside) ok = false;
                if (!ok) continue;
                dv_set(&g_dv_base[k], p * 4 + s);
                g_dv_pclass[p] |= (uint16_t)(1u << k);
            }
        }
    }
}

/* Connected edges of a full board, out of 480. */
static int dv_score(const DvBoard *b) {
    int broken = 0;
    for (int x = 0; x < NUM_PIECES; x++) {
        const int r = x / PUZZLE_SIDE, c = x % PUZZLE_SIDE;
        if (c < PUZZLE_SIDE - 1 && b->c[x].right != b->c[x + 1].left) broken++;
        if (r < PUZZLE_SIDE - 1 && b->c[x].top != b->c[x + PUZZLE_SIDE].bottom) broken++;
    }
    return DV_EDGES - broken;
}

static uint64_t dv_board_fp(const DvBoard *b) {
    uint64_t h = 14695981039346656037ULL;
    for (int x = 0; x < NUM_PIECES; x++) {
        h ^= b->c[x].piece_id; h *= 1099511628211ULL;
        h ^= b->c[x].rotation; h *= 1099511628211ULL;
    }
    return h;
}

/* ============================================================================
 * Dive: forward checking and one stuck-mode dive
 * ========================================================================== */

/* Exact-fit orientation masks per open cell and their counts against the unused
   pieces, maintained incrementally. */
typedef struct {
    DvMask   nb[NUM_PIECES];
    DvMask   unused4;
    uint16_t dom[NUM_PIECES];
    int32_t  avail[DV_CLASSES];
    uint8_t  elist[NUM_PIECES], epos[NUM_PIECES];
    int16_t  n_empty, zero;
} DvFc;

/* A dive's starting state: the prototype's, copying only what a dive reads --
   the masks of the open cells (a third of the 32 kB mask array on a typical
   board), not those of placed cells, which no dive touches. */
static inline void dv_fc_reset(DvFc *f, const DvFc *proto, const uint8_t *cells, int n) {
    f->unused4 = proto->unused4;
    memcpy(f->dom, proto->dom, sizeof f->dom);
    memcpy(f->avail, proto->avail, sizeof f->avail);
    memcpy(f->elist, proto->elist, sizeof f->elist);
    memcpy(f->epos, proto->epos, sizeof f->epos);
    f->n_empty = proto->n_empty;
    f->zero = proto->zero;
    for (int i = 0; i < n; i++) f->nb[cells[i]] = proto->nb[cells[i]];
}

static inline void dv_empty_add(DvFc *f, int x) {
    f->epos[x] = (uint8_t)f->n_empty;
    f->elist[f->n_empty++] = (uint8_t)x;
}
static inline void dv_empty_remove(DvFc *f, int x) {
    const int at = f->epos[x], last = f->elist[--f->n_empty];
    f->elist[at] = (uint8_t)last;
    f->epos[last] = (uint8_t)at;
}

static void dv_compute(const DvBoard *b, DvFc *f, int x) {
    if (f->dom[x] == 0) f->zero--;
    DvMask m = g_dv_base[dv_cls(x)];
    for (int d = 0; d < 4; d++) {
        const int y = dv_nb(x, d);
        if (y < 0 || b->c[y].piece_id == DV_EMPTY) continue;
        dv_and(&m, &g_dv_fit[d][dv_side(&b->c[y], (d + 2) & 3)]);
    }
    f->nb[x] = m;
    const int n = dv_ipop(&m, &f->unused4);
    f->dom[x] = (uint16_t)n;
    if (n == 0) f->zero++;
}

static void dv_fc_init(DvFc *f, const DvBoard *b) {
    memset(f, 0, sizeof *f);
    bool on[NUM_PIECES] = { false };
    for (int x = 0; x < NUM_PIECES; x++) {
        f->dom[x] = DV_PLACED;
        if (b->c[x].piece_id != DV_EMPTY) on[b->c[x].piece_id] = true;
    }
    for (int p = 0; p < NUM_PIECES; p++)
        if (!on[p]) f->unused4.w[(p * 4) >> 6] |= (uint64_t)g_dv_spins[p] << ((p * 4) & 63);
    for (int k = 0; k < DV_CLASSES; k++) f->avail[k] = dv_ipop(&g_dv_base[k], &f->unused4);
    for (int x = 0; x < NUM_PIECES; x++)
        if (b->c[x].piece_id == DV_EMPTY) { dv_empty_add(f, x); dv_compute(b, f, x); }
}

static inline void dv_adjust(DvFc *f, int p, int sign) {
    const int wi = (p * 4) >> 6;
    const uint64_t bits = (uint64_t)g_dv_spins[p] << ((p * 4) & 63);
    for (int k = 0; k < DV_CLASSES; k++)
        f->avail[k] += sign * __builtin_popcountll(g_dv_base[k].w[wi] & bits);
    /* Branch-free: most cells do not hold p (delta 0), but which ones is data,
       and a mispredicted skip costs more than the update it saves.  No cell here
       is DV_PLACED: dv_unplace() adjusts before its own cell rejoins the list. */
    int zero = 0;
    for (int i = 0; i < f->n_empty; i++) {
        const int x = f->elist[i];
        const int old = f->dom[x];
        const int next = old + sign * __builtin_popcountll(f->nb[x].w[wi] & bits);
        f->dom[x] = (uint16_t)next;
        zero += (next == 0) - (old == 0);
    }
    f->zero = (int16_t)(f->zero + zero);
}

static inline void dv_refresh(const DvBoard *b, DvFc *f, int x) {
    for (int d = 0; d < 4; d++) {
        const int y = dv_nb(x, d);
        if (y >= 0 && b->c[y].piece_id == DV_EMPTY) dv_compute(b, f, y);
    }
}

static inline void dv_place(DvBoard *b, DvFc *f, int x, int p, int s) {
    if (f->dom[x] == 0) f->zero--;
    f->dom[x] = DV_PLACED;
    dv_empty_remove(f, x);
    b->c[x] = g_dv_or[p][s & 3];
    f->unused4.w[(p * 4) >> 6] &= ~(0xFULL << ((p * 4) & 63));
    dv_adjust(f, p, -1);
    dv_refresh(b, f, x);
}

static inline void dv_unplace(DvBoard *b, DvFc *f, int x) {
    const int p = b->c[x].piece_id;
    b->c[x].piece_id = DV_EMPTY;
    f->unused4.w[(p * 4) >> 6] |= (uint64_t)g_dv_spins[p] << ((p * 4) & 63);
    dv_adjust(f, p, +1);                     /* before x rejoins: dom[x] is DV_PLACED */
    dv_empty_add(f, x);
    dv_compute(b, f, x);
    dv_refresh(b, f, x);
}

/* Fit masks of x's PLACED neighbours; returns how many. */
static inline int dv_fit_masks(const DvBoard *b, int x, const DvMask *fit[4]) {
    int n = 0;
    for (int d = 0; d < 4; d++) {
        const int y = dv_nb(x, d);
        if (y >= 0 && b->c[y].piece_id != DV_EMPTY)
            fit[n++] = &g_dv_fit[d][dv_side(&b->c[y], (d + 2) & 3)];
    }
    return n;
}

/* out[j] = orientations of `base` breaking exactly j of the d placed
   neighbours: a bit-sliced count of the fit masks. */
static inline void dv_break_classes(const DvMask *base, const DvMask *const fit[4],
                                    int d, DvMask out[5]) {
    for (int wi = 0; wi < DV_WORDS; wi++) {
        uint64_t s0 = 0, s1 = 0, s2 = 0;
        for (int k = 0; k < d; k++) {
            const uint64_t v = fit[k]->w[wi];
            const uint64_t c0 = s0 & v; s0 ^= v;
            const uint64_t c1 = s1 & c0; s1 ^= c0;
            s2 |= c1;
        }
        for (int j = 0; j <= d; j++) {
            const int m = d - j;
            uint64_t eq = base->w[wi];
            eq &= (m & 1) ? s0 : ~s0;
            eq &= (m & 2) ? s1 : ~s1;
            eq &= (m & 4) ? s2 : ~s2;
            out[j].w[wi] = eq;
        }
    }
}

/* Break placements available at open x: frame-legal unused orientations that
   are not exact fits. */
static inline int dv_count_breaks(const DvFc *f, int x) {
    return f->avail[dv_cls(x)] - (int)f->dom[x];
}

/* Exact fits at x, or when must_break the break placements in ascending break
   class; returns the count, exact fits reported in *exact. */
static int dv_collect(const DvBoard *b, const DvFc *f, int x, bool must_break,
                      DvCand *out, int *exact) {
    int n = 0;
    for (int wi = 0; wi < DV_WORDS; wi++) {
        uint64_t w = f->nb[x].w[wi] & f->unused4.w[wi];
        while (w) {
            const int bit = wi * 64 + __builtin_ctzll(w);
            w &= w - 1;
            out[n].pid = (uint16_t)(bit >> 2); out[n].spin = (uint8_t)(bit & 3);
            out[n].breaks = 0; n++;
        }
    }
    *exact = n;
    if (must_break && n == 0) {
        const DvMask *fit[4];
        const int d = dv_fit_masks(b, x, fit);
        DvMask base = g_dv_base[dv_cls(x)];
        dv_and(&base, &f->unused4);
        DvMask cls[5];
        dv_break_classes(&base, fit, d, cls);
        for (int j = 1; j <= d && j <= DV_BREAK_CAP; j++)
            for (int wi = 0; wi < DV_WORDS; wi++) {
                uint64_t w = cls[j].w[wi];
                while (w) {
                    const int bit = wi * 64 + __builtin_ctzll(w);
                    w &= w - 1;
                    out[n].pid = (uint16_t)(bit >> 2); out[n].spin = (uint8_t)(bit & 3);
                    out[n].breaks = (uint8_t)j; n++;
                }
            }
    }
    return n;
}

/* A dive's policy: learned weights w(piece, cell) and how strongly they steer.
   Stock dives pass w = NULL or beta = 0. */
typedef struct {
    const float   *w;          /* [slot(piece) * ncell + slot(cell)] */
    const int16_t *pslot;      /* piece -> row of w, -1 = not open */
    const int16_t *xslot;      /* cell  -> column of w */
    int            ncell;
    double         beta;
} DvPolicy;

static inline bool dv_guided(const DvPolicy *pol) {
    return pol && pol->w && pol->beta > 0.0;
}
static inline float dv_w(const DvPolicy *pol, int p, int x) {
    return pol->w[(size_t)pol->pslot[p] * (size_t)pol->ncell + (size_t)pol->xslot[x]];
}

/* Largest learned weight among x's exact fits. */
static float dv_maxw(const DvFc *f, int x, const DvPolicy *pol) {
    float m = -INFINITY;
    for (int wi = 0; wi < DV_WORDS; wi++) {
        uint64_t v = f->nb[x].w[wi] & f->unused4.w[wi];
        while (v) {
            const int bit = wi * 64 + __builtin_ctzll(v);
            v &= v - 1;
            const float q = dv_w(pol, bit >> 2, x);
            if (q > m) m = q;
        }
    }
    return m;
}

/* What dv_place(x, p, s) would leave behind, read off the state without playing
   it: the zero-domain count and the room (exact fits summed over x's open
   neighbours).  Placing p removes its orientations from every open cell's
   domain and narrows x's open neighbours to the new side; so a neighbour's new
   domain is its mask AND that side's fit mask counted against the pool without
   p, and any other cell turns zero only if p's orientations were its whole
   domain -- which needs at most four fits, so only those cells are kept to
   test.  Exact: the same numbers as playing the move and undoing it, for a
   fraction of the work (no writes, no per-cell recompute). */
typedef struct {
    int     ny, y[4], dir[4];      /* x's open neighbours and the side toward each */
    int     zero_base;             /* zero count with x and its neighbours left out */
    int     ncrit;
    uint8_t crit[NUM_PIECES];      /* other open cells with 1..4 exact fits */
} DvLcvCtx;

static void dv_lcv_ctx(const DvBoard *b, const DvFc *f, int x, DvLcvCtx *c) {
    c->ny = 0;
    c->zero_base = f->zero - (f->dom[x] == 0);
    for (int d = 0; d < 4; d++) {
        const int y = dv_nb(x, d);
        if (y < 0 || b->c[y].piece_id != DV_EMPTY) continue;
        c->y[c->ny] = y; c->dir[c->ny] = d; c->ny++;
        c->zero_base -= (f->dom[y] == 0);
    }
    c->ncrit = 0;
    for (int i = 0; i < f->n_empty; i++) {
        const int y = f->elist[i];
        const int dy = f->dom[y];
        if (dy >= 1 && dy <= 4) c->crit[c->ncrit++] = (uint8_t)y;
    }
    /* drop x and its neighbours (handled exactly above) from the critical list */
    int k = 0;
    for (int i = 0; i < c->ncrit; i++) {
        const int y = c->crit[i];
        bool nb = (y == x);
        for (int d = 0; d < 4 && !nb; d++) nb = (dv_nb(x, d) == y);
        if (!nb) c->crit[k++] = (uint8_t)y;
    }
    c->ncrit = k;
}

static inline void dv_lcv_eval(const DvFc *f, const DvLcvCtx *c, int p, int s,
                               int *zd_out, int *room_out) {
    const Oriented *o = &g_dv_or[p][s & 3];
    const int wi = (p * 4) >> 6;
    const uint64_t bits = (uint64_t)g_dv_spins[p] << ((p * 4) & 63);
    DvMask un = f->unused4;
    un.w[wi] &= ~(0xFULL << ((p * 4) & 63));
    int zd = c->zero_base, room = 0;
    for (int k = 0; k < c->ny; k++) {
        const int y = c->y[k], d = c->dir[k];
        DvMask m = f->nb[y];
        dv_and(&m, &g_dv_fit[(d + 2) & 3][dv_side(o, d)]);
        const int n = dv_ipop(&m, &un);
        room += n;
        zd += (n == 0);
    }
    for (int i = 0; i < c->ncrit; i++) {
        const int y = c->crit[i];
        zd += (__builtin_popcountll(f->nb[y].w[wi] & bits) == (int)f->dom[y]);
    }
    *zd_out = zd; *room_out = room;
}

/* Least-constraining value among cand[lo..hi): evaluate each candidate's effect
   on the forward-checking state (dv_lcv_eval, exactly what playing it would
   leave). Lexicographic: fewest breaks, fewest
   zero-domain cells, then most room left around it (stock: ties uniform;
   guided: log1p(room) + beta*(w + Gumbel)). */
static int dv_lcv(DvBoard *b, DvFc *f, int x, const DvCand *cand, int lo, int hi,
                  RNG *rng, const DvPolicy *pol) {
    const bool g = dv_guided(pol);
    const int n = hi - lo, cap = n < DV_LCV_CAP ? n : DV_LCV_CAP;
    int idx[DV_LCV_CAP];
    if (n <= cap) {
        for (int k = 0; k < cap; k++) idx[k] = lo + k;
    } else if (!g) {
        for (int k = 0; k < cap; k++) idx[k] = lo + (int)rng_uniform(rng, (uint32_t)n);
    } else {                                   /* Gumbel-top-k on beta*w */
        double key[DV_LCV_CAP];
        int m = 0;
        for (int i = lo; i < hi; i++) {
            const double k = pol->beta * dv_w(pol, cand[i].pid, x) + dv_gumbel(rng);
            if (m < cap) { idx[m] = i; key[m] = k; m++; }
            else {
                int lo_k = 0;
                for (int j = 1; j < cap; j++) if (key[j] < key[lo_k]) lo_k = j;
                if (k > key[lo_k]) { idx[lo_k] = i; key[lo_k] = k; }
            }
        }
    }
    int best = -1, b_brk = 0, b_zd = 0;
    double b_key = 0.0;
    uint32_t ties = 0;
    DvLcvCtx ctx;
    dv_lcv_ctx(b, f, x, &ctx);
    for (int k = 0; k < cap; k++) {
        const int ci = idx[k];
        int zd, room;
        dv_lcv_eval(f, &ctx, cand[ci].pid, cand[ci].spin, &zd, &room);
        const int brk = cand[ci].breaks;
        const double key = g ? (double)log1pf((float)room)
                               + pol->beta * ((double)dv_w(pol, cand[ci].pid, x) + dv_gumbel(rng))
                             : (double)room;
        const bool better = best < 0 || brk < b_brk ||
                            (brk == b_brk && (zd < b_zd || (zd == b_zd && key > b_key)));
        const bool same = best >= 0 && brk == b_brk && zd == b_zd && key == b_key;
        if (better) { best = ci; b_brk = brk; b_zd = zd; b_key = key; ties = 1; }
        else if (same && rng_uniform(rng, ++ties) == 0) best = ci;
    }
    return best;
}

/* One dive over cells[0..n): never backtracks, always completes. `tpos` (per
   cell; NULL = none) is a fill order's key: among the most constrained cells,
   the one with the largest tpos + noise (+ the policy's pull) goes first. */
static void dv_dive(DvBoard *b, DvFc *f, const uint8_t *cells, int n, RNG *rng,
                    const DvPolicy *pol, const float *tpos) {
    const bool g = dv_guided(pol);
    uint8_t rem[NUM_PIECES];
    memcpy(rem, cells, (size_t)n);
    DvCand cand[NUM_PIECES * 4];
    for (int pos = 0; pos < n; pos++) {
        int sel = -1, best_ex = INT_MAX;
        uint32_t ties = 0;
        /* The most constrained open cell with an exact fit: a branch-free minimum,
           then the tied cells, then one draw among them -- uniform for a stock
           dive, Gumbel-max on the policy (and the fill order) for a guided one,
           which needs no draw at all when a single cell is tied. */
        for (int j = pos; j < n; j++) {
            const int ex = f->dom[rem[j]];
            const int v = ex ? ex : INT_MAX;
            best_ex = v < best_ex ? v : best_ex;
        }
        if (best_ex != INT_MAX) {
            uint8_t tie[NUM_PIECES];
            int nt = 0;
            for (int j = pos; j < n; j++) {
                tie[nt] = (uint8_t)j;
                nt += (f->dom[rem[j]] == best_ex);
            }
            if (nt == 1) sel = tie[0];
            else if (!g && !tpos) sel = tie[rng_uniform(rng, (uint32_t)nt)];
            else {
                double bk = -INFINITY;
                for (int t = 0; t < nt; t++) {
                    const int j = tie[t];
                    double k = dv_gumbel(rng);
                    if (g) k += pol->beta * (double)dv_maxw(f, rem[j], pol);
                    if (tpos) k += (double)tpos[rem[j]];
                    if (sel < 0 || k > bk) { bk = k; sel = j; }
                }
            }
        }
        if (sel < 0) {                 /* every cell is stuck: break where narrowest */
            int best_n = INT_MAX;
            ties = 0;
            for (int j = pos; j < n; j++) {
                const int c = dv_count_breaks(f, rem[j]);
                if (c <= 0) continue;
                if (c < best_n) { best_n = c; sel = j; ties = 1; }
                else if (c == best_n && rng_uniform(rng, ++ties) == 0) sel = j;
            }
            if (sel < 0) sel = pos;
        }
        if (sel != pos) { uint8_t t = rem[pos]; rem[pos] = rem[sel]; rem[sel] = t; }
        const int x = rem[pos];
        const bool must_break = (f->dom[x] == 0);
        int exact = 0;
        const int nc = dv_collect(b, f, x, must_break, cand, &exact);
        int lo, hi;
        if (!must_break && exact > 0) { lo = 0; hi = exact; }
        else {
            lo = exact;
            if (lo >= nc) { lo = 0; hi = nc; }
            else { hi = lo; while (hi < nc && cand[hi].breaks == cand[lo].breaks) hi++; }
        }
        if (hi <= lo)
            fatal("internal error: an end dive found no piece for cell %d "
                  "(the frame's piece types are not balanced)", x);
        const int pick = (hi - lo > 1) ? dv_lcv(b, f, x, cand, lo, hi, rng, pol) : lo;
        dv_place(b, f, x, cand[pick].pid, cand[pick].spin);
    }
}

/* ============================================================================
 * Local search: moves, polish, kick-and-polish walks
 * ========================================================================== */

/* Matched edges between x and its neighbours (a full board). */
static inline int dv_local(const DvBoard *b, int x) {
    int m = 0;
    for (int d = 0; d < 4; d++) {
        const int y = dv_nb(x, d);
        if (y >= 0 && dv_side(&b->c[x], d) == dv_side(&b->c[y], (d + 2) & 3)) m++;
    }
    return m;
}
/* 1 if the shared edge of adjacent x, y matches. */
static inline int dv_local_edge(const DvBoard *b, int x, int y) {
    for (int d = 0; d < 4; d++)
        if (dv_nb(x, d) == y) return dv_side(&b->c[x], d) == dv_side(&b->c[y], (d + 2) & 3);
    return 0;
}
static inline int dv_pair(const DvBoard *b, int x, int y) {
    return dv_local(b, x) + dv_local(b, y) - dv_local_edge(b, x, y);   /* shared edge once */
}
static inline bool dv_adjacent(int x, int y) {
    const int d = x > y ? x - y : y - x;
    return d == PUZZLE_SIDE || (d == 1 && x / PUZZLE_SIDE == y / PUZZLE_SIDE);
}

/* The first spin of piece p, in spin order, that matches the most edges at
   cell y, or -1 when p has no frame-legal spin there; the count in *v.

   A swap of two cells that do not touch scores as two independent choices:
   the edges y gains depend only on p's spin at y, and x's only on q's spin at
   x. So the swap's best spin pair, and the first one in the (spin at y, spin at
   x) scan order, is each cell's first best spin -- 4 + 4 evaluations for the
   16 a joint scan makes, and exactly the same move. */
static inline int dv_best_spin(DvBoard *b, int y, int p, int cls, int *v) {
    const Oriented keep = b->c[y];
    int bs = -1, bv = -1;
    for (int s = 0; s < 4; s++) {
        if (!dv_legal(p, s, cls)) continue;
        b->c[y] = g_dv_or[p][s];
        const int val = dv_local(b, y);
        if (val > bv) { bv = val; bs = s; }
    }
    b->c[y] = keep;
    *v = bv;
    return bs;
}

/* Hill-climb a finished board over cells[0..n): every re-rotation of one piece
   and every swap of two (each with its best frame-legal spins), taking any move
   that adds matched edges, until none does. Returns the edges gained. */
static int dv_polish(DvBoard *b, const uint8_t *cells, int n) {
    int gained = 0;
    for (int pass = 0; pass < 100; pass++) {
        int improved = 0;
        for (int i = 0; i < n; i++) {
            const int x = cells[i];
            {                                             /* re-rotate in place */
                const int p = b->c[x].piece_id, cls = dv_cls(x);
                const Oriented keep = b->c[x];
                int best = dv_local(b, x), bs = -1;
                for (int s = 0; s < 4; s++) {
                    if (s == keep.rotation || !dv_legal(p, s, cls)) continue;
                    b->c[x] = g_dv_or[p][s];
                    const int v = dv_local(b, x);
                    if (v > best) { best = v; bs = s; }
                }
                b->c[x] = keep;
                if (bs >= 0) { gained += best - dv_local(b, x); b->c[x] = g_dv_or[p][bs]; improved++; }
            }
            for (int j = i + 1; j < n; j++) {
                const int y = cells[j];
                const int p = b->c[x].piece_id, q = b->c[y].piece_id;
                const int cx = dv_cls(x), cy = dv_cls(y);
                if (!(g_dv_pclass[p] >> cy & 1u) || !(g_dv_pclass[q] >> cx & 1u)) continue;
                const int before = dv_pair(b, x, y);
                if (!dv_adjacent(x, y)) {
                    int va, vb;
                    const int sp = dv_best_spin(b, y, p, cy, &va);
                    const int sq = dv_best_spin(b, x, q, cx, &vb);
                    if (sp >= 0 && sq >= 0 && va + vb > before) {
                        b->c[y] = g_dv_or[p][sp]; b->c[x] = g_dv_or[q][sq];
                        gained += va + vb - before; improved++;
                    }
                    continue;
                }
                const Oriented ox = b->c[x], oy = b->c[y];
                int best = before, bsp = -1, bsq = -1;
                for (int sp = 0; sp < 4; sp++) {
                    if (!dv_legal(p, sp, cy)) continue;
                    b->c[y] = g_dv_or[p][sp];
                    for (int sq = 0; sq < 4; sq++) {
                        if (!dv_legal(q, sq, cx)) continue;
                        b->c[x] = g_dv_or[q][sq];
                        const int v = dv_pair(b, x, y);
                        if (v > best) { best = v; bsp = sp; bsq = sq; }
                    }
                }
                if (bsp >= 0) {
                    b->c[y] = g_dv_or[p][bsp]; b->c[x] = g_dv_or[q][bsq];
                    gained += best - before; improved++;
                } else { b->c[x] = ox; b->c[y] = oy; }
            }
        }
        if (!improved) break;
    }
    return gained;
}

/* The best move involving cell x: re-rotate it, or swap it with any other cell
   of the region. Applies it if it gains; returns the partner cell moved (x
   itself for a rotation), or -1. */
static int dv_best_move_at(DvBoard *b, const uint8_t *cells, int n, int x, int *gain) {
    const int p = b->c[x].piece_id, cx = dv_cls(x);
    const Oriented ox = b->c[x];
    int best_g = 0, by = -1, bsp = -1, bsq = -1;
    {
        const int base = dv_local(b, x);
        for (int s = 0; s < 4; s++) {
            if (s == ox.rotation || !dv_legal(p, s, cx)) continue;
            b->c[x] = g_dv_or[p][s];
            const int g = dv_local(b, x) - base;
            if (g > best_g) { best_g = g; by = x; bsp = s; }
        }
        b->c[x] = ox;
    }
    for (int j = 0; j < n; j++) {
        const int y = cells[j];
        if (y == x) continue;
        const int q = b->c[y].piece_id, cy = dv_cls(y);
        if (!(g_dv_pclass[p] >> cy & 1u) || !(g_dv_pclass[q] >> cx & 1u)) continue;
        const int before = dv_pair(b, x, y);
        if (!dv_adjacent(x, y)) {
            int va, vb;
            const int sp = dv_best_spin(b, y, p, cy, &va);
            const int sq = dv_best_spin(b, x, q, cx, &vb);
            if (sp >= 0 && sq >= 0 && va + vb - before > best_g) {
                best_g = va + vb - before; by = y; bsp = sp; bsq = sq;
            }
            continue;
        }
        const Oriented oy = b->c[y];
        for (int sp = 0; sp < 4; sp++) {
            if (!dv_legal(p, sp, cy)) continue;
            b->c[y] = g_dv_or[p][sp];
            for (int sq = 0; sq < 4; sq++) {
                if (!dv_legal(q, sq, cx)) continue;
                b->c[x] = g_dv_or[q][sq];
                const int g = dv_pair(b, x, y) - before;
                if (g > best_g) { best_g = g; by = y; bsp = sp; bsq = sq; }
            }
        }
        b->c[x] = ox; b->c[y] = oy;
    }
    if (by < 0) return -1;
    if (by == x) b->c[x] = g_dv_or[p][bsp];
    else { const int q = b->c[by].piece_id; b->c[by] = g_dv_or[p][bsp]; b->c[x] = g_dv_or[q][bsq]; }
    *gain = best_g;
    return by;
}

/* Polish driven by a work queue: only cells whose surroundings changed are
   re-examined (a move's gain depends only on the two cells and their
   neighbours). Seeds with queue[0..qn); returns the edges gained. */
static int dv_polish_q(DvBoard *b, const uint8_t *cells, int n, const bool *inreg,
                       int *queue, int qn, bool *inq) {
    int gained = 0, head = 0, tail = qn;   /* ring of NUM_PIECES slots */
    int cnt = qn;
    while (cnt > 0) {
        const int x = queue[head]; head = (head + 1) % NUM_PIECES; cnt--;
        inq[x] = false;
        int g = 0;
        const int y = dv_best_move_at(b, cells, n, x, &g);
        if (y < 0) continue;
        gained += g;
        const int touched[2] = { x, y };
        for (int t = 0; t < 2; t++) {
            const int z = touched[t];
            for (int d = -1; d < 4; d++) {
                const int w = d < 0 ? z : dv_nb(z, d);
                if (w < 0 || !inreg[w] || inq[w]) continue;
                inq[w] = true; queue[tail] = w; tail = (tail + 1) % NUM_PIECES; cnt++;
            }
        }
    }
    return gained;
}

/* Kick: DV_KICK random frame-legal swaps; the touched cells and their
   neighbours are queued for the re-polish. Returns the queue length. The first
   cell of each swap is drawn from the open cells with a broken edge (any open
   cell once none is left), the second from all open cells: a swap that moves
   a broken piece is the one a re-polish can turn into a gain. Measured on 177
   stop-row boards: +0.08 edges per board over uniform kicks, no extra time. */
static int dv_kick(DvBoard *b, const uint8_t *cells, int nc, const bool *inreg,
                   RNG *rng, int *queue, bool *inq) {
    int qn = 0;
    uint8_t brk[NUM_PIECES];
    int nb = 0;
    for (int i = 0; i < nc; i++) {
        const int x = cells[i];
        int nn = 0;
        for (int d = 0; d < 4; d++) nn += dv_nb(x, d) >= 0;
        if (dv_local(b, x) < nn) brk[nb++] = (uint8_t)x;
    }
    for (int kk = 0; kk < DV_KICK; kk++) {
        for (int tries = 0; tries < 32; tries++) {
            const int x = nb ? brk[rng_uniform(rng, (uint32_t)nb)]
                             : cells[rng_uniform(rng, (uint32_t)nc)];
            const int y = cells[rng_uniform(rng, (uint32_t)nc)];
            if (x == y) continue;
            const int p = b->c[x].piece_id, q = b->c[y].piece_id;
            if (!(g_dv_pclass[p] >> dv_cls(y) & 1u) || !(g_dv_pclass[q] >> dv_cls(x) & 1u)) continue;
            int sp, sq;
            do sp = (int)rng_uniform(rng, 4); while (!dv_legal(p, sp, dv_cls(y)));
            do sq = (int)rng_uniform(rng, 4); while (!dv_legal(q, sq, dv_cls(x)));
            b->c[y] = g_dv_or[p][sp]; b->c[x] = g_dv_or[q][sq];
            const int kicked[2] = { x, y };
            for (int t = 0; t < 2; t++)
                for (int d = -1; d < 4; d++) {
                    const int w = d < 0 ? kicked[t] : dv_nb(kicked[t], d);
                    if (w < 0 || !inreg[w] || inq[w]) continue;
                    inq[w] = true; queue[qn++] = w;
                }
            break;
        }
    }
    return qn;
}

/* ============================================================================
 * Batch: roots, stages, keep
 * ========================================================================== */
/* The K best distinct dives of a board, kept as polish candidates. Ordered by
   the key (score desc, dive index asc) and deduplicated by board, so the set is
   the same however the dives were split into jobs and merged: the top K of a
   union is the top K of the union of the parts' top K. */
typedef struct {
    int       n;
    uint64_t *idx, *fp;
    int      *s;
    uint8_t (*brd)[2 * NUM_PIECES];   /* pid[256] then rot[256], per cell */
} DvTop;

static inline int dv_walks(void) { return (g_p.polish_top + 1) / 2; }

static DvTop *dv_top_new(void) {
    const size_t k = (size_t)g_p.polish_top;
    DvTop *t = xmalloc(sizeof *t + k * (2 * sizeof(uint64_t) + sizeof(int) + 2 * NUM_PIECES));
    t->n = 0;
    t->idx = (uint64_t *)(t + 1); t->fp = t->idx + k;
    t->s = (int *)(t->fp + k);
    t->brd = (uint8_t (*)[2 * NUM_PIECES])(t->s + k);
    return t;
}

struct DvS2;
struct DvPol;

typedef struct {
    uint16_t base_pid[NUM_PIECES];   /* per cell; DV_EMPTY = open */
    uint8_t  base_rot[NUM_PIECES];
    uint8_t  best_pid[NUM_PIECES], best_rot[NUM_PIECES];
    uint64_t fp, seq;
    int      best;                   /* -1 = no dive yet */
    uint64_t best_idx;               /* dive index of the best: earliest wins a tie */
    uint32_t dives;
    bool     stage2;
    uint8_t  seeded;                 /* corner seed: 1 = TL, 2 = TR, 3 = both */
    int8_t   top_row;                /* highest row with every row below it full; -1 none */
    uint32_t origin;                 /* queue index of the unseeded board it came from */
    uint32_t group, copy;            /* queue index of its board's first copy; which copy */
    uint8_t  order;                  /* DvOrder of this copy */
    bool     kept_copy;              /* after dv_run: the copy its board keeps */
    int      inc_score;              /* score of the incumbent it was cut from; -1 = none */
    int      own;                    /* its own best dive or polish; -1 = none yet */
    bool     moved;                  /* best is no longer the incumbent (plateau) */
    uint8_t  inc_pid[NUM_PIECES], inc_rot[NUM_PIECES];
    /* Run state of the batch in flight (NULL / 0 between batches). */
    float        *w0;                /* prior weights from the incumbent, when set */
    DvTop        *top;               /* polish candidates, when polishing */
    struct DvS2  *s2;                /* stage-2 learning state, while live */
    struct DvPol *pol;               /* polish state, while polishing */
    int           pending;           /* jobs of the root's current step not yet done */
    bool          stopped;           /* a job ran out of time: the root takes no new step */
} DvRoot;

typedef struct {
    uint8_t  pid[NUM_PIECES], rot[NUM_PIECES];   /* per cell */
    uint64_t seq, fp;
    uint32_t cfg;
    int      score;
    uint8_t  seeded;
    int8_t   top_row;
} DvKeep;

static DvRoot  *g_dv_q = NULL;       /* this batch's roots */
static size_t   g_dv_qn = 0, g_dv_qcap = 0;
static uint64_t g_dv_seq = 0;
static DvKeep  *g_dv_keep = NULL;    /* kept boards not yet written */
static size_t   g_dv_kn = 0, g_dv_kcap = 0;
static char   **g_dv_cfg = NULL;     /* batch ids the kept boards name */
static size_t   g_dv_ncfg = 0, g_dv_cfgcap = 0;
/* The queued boards of the batch (one per dv_add/dv_queue call): the root of
   the first copy, and after dv_run the root of the copy kept. */
static uint32_t *g_dv_grp = NULL, *g_dv_grp_keep = NULL;
static size_t    g_dv_grpn = 0, g_dv_grpcap = 0;
static bool      g_dv_ran = false;   /* the next queued board starts a batch */
static bool      g_dv_multi = false; /* the batch has a board queued in copies */
static size_t    g_dv_last_n = 0;    /* roots of the last batch run */
static char      g_dv_last_id[256];
static bool      g_dv_keeping = true;

const char *const dv_order_names[DV_NORDERS] = {
    "mrv", "left", "right", "centre", "ends", "top", "bottom"
};

static struct {
    uint64_t roots, stage2, dives, kept, written, dup;
    uint64_t pol_gain, pol_roots;
    uint64_t brk_tl, brk_tr, brk_seam, brk_rest, clean_tl, clean_tr;
    uint64_t seed_boards, seed_roots, seed_pairs, seed_wins, seed_ties, seed_losses;
    int64_t  seed_diff;
    uint64_t seed_written, sbrk_tl, sbrk_tr, sclean_tl, sclean_tr;
    int      top_min, top_max;       /* top full rows of the written boards */
    int      seed_best, plain_best;
    double   t, t_s1, t_s2, u_s1, u_s2, jt_s2, jt_pol;
    int      best, last_best;         /* the run's best; the last dv_run batch's */
    uint64_t inc_boards, inc_better;  /* boards with an incumbent; beaten by a dive */
    int64_t  inc_gain;
    uint64_t hist[DV_EDGES + 1];
} g_dv_run = { .best = -1, .last_best = -1, .seed_best = -1, .plain_best = -1, .top_min = PUZZLE_SIDE, .top_max = -1 };

void dv_init(const DvParams *p) {
    g_p = *p;
    if (g_p.polish_top <= 0) g_p.polish_top = DV_POLISH_TOP;
    if (g_p.polish_top > DV_POLISH_MAX) fatal("polish_top must be in 1..%d", DV_POLISH_MAX);
    dv_build_static();
}

void dv_seeding(bool on) { g_p.seed_corners = on; }
void dv_keeping(bool on) { g_dv_keeping = on; }

uint64_t dv_written(void) { return g_dv_run.written; }
int      dv_last_best(void) { return g_dv_run.last_best; }
uint64_t dv_boards(void)  { return g_dv_run.roots; }
uint64_t dv_score_count(int score) {
    return score >= 0 && score <= DV_EDGES ? g_dv_run.hist[score] : 0;
}
double   dv_seconds(void) { return g_dv_run.t; }

static DvRoot *dv_push_root(void) {
    if (g_dv_qn == g_dv_qcap) {
        g_dv_qcap = g_dv_qcap ? g_dv_qcap * 2 : 256;
        g_dv_q = xrealloc(g_dv_q, g_dv_qcap * sizeof *g_dv_q);
    }
    return &g_dv_q[g_dv_qn++];
}

/* The root's random streams: its board and the master seed, and a nonzero
   salt for a copy (salt 0 = the stock streams). */
static void dv_root_key(DvRoot *r, uint64_t salt) {
    uint64_t h = 14695981039346656037ULL;
    for (int x = 0; x < NUM_PIECES; x++) {
        h ^= r->base_pid[x]; h *= 1099511628211ULL;
        h ^= r->base_rot[x]; h *= 1099511628211ULL;
    }
    r->fp = splitmix64(h ^ g_p.master_seed);
    if (salt) r->fp = splitmix64(r->fp ^ splitmix64(salt));
    r->seq = g_dv_seq++;
}

/* ============================================================================
 * Seeders: corner-seeded copies (--lambda_corners catalog)
 *
 * Dives fill the top rows at random and never aim for the top-corner blocks the
 * corner catalog still holds alive. Each board with an alive block also gets up
 * to N seeded copies: the block's pieces and one free pair of top-border
 * witnesses are placed on their cells (the clued 2x3 around the row-13 clue,
 * or the 3-cell corner), so that corner is clean by construction. The copy is
 * then dived like any other board, and its fixed cells never move.
 *
 * Cells, TL (TR mirrors the column): clued pid[0] side (14,0), pid[1] side
 * (13,0), pid[2] in_a (14,1), pid[3] in_b (14,2), pid[4] in_c (13,1), w1 (15,1),
 * w2 (15,2); unclued pid[0] side (14,0), pid[1] in_a (14,1), w1 (15,1). The TL
 * side cells hold the configuration's left column, so a TL block must name
 * those very pieces.
 * ========================================================================== */
typedef struct { uint16_t pid[7]; uint8_t rot[7], cell[7]; uint8_t n; } DvSeed;

/* Every (alive block, free witness pair) of corner k on root r, in slot s. */
static int dv_seed_options(const DvRoot *r, int s, int k, const uint64_t used[4],
                           const uint8_t rtop[PUZZLE_SIDE], bool at12, DvSeed *out) {
    int nopt = 0;
    if (at12 && g_tc_clued && rtop[k ? PUZZLE_SIDE - 3 : 2] != g_tc_clue_bottom[s][k]) return 0;
    for (int i = 0; i < g_tc_live_n[s][k] && nopt < DV_SEED_OPTS; i++) {
        if (!tc_alive(s, k, i, used, rtop, at12)) continue;
        const TcBlock *b = &g_tc_blk[s][k][g_tc_live[s][k][i].blk];
        static const uint8_t RC_CL[5][2] = { {14,0}, {13,0}, {14,1}, {14,2}, {13,1} };
        static const uint8_t RC_UN[2][2] = { {14,0}, {14,1} };
        DvSeed sd; sd.n = 0;
        bool ok = true;
        for (int q = 0; q < b->n && ok; q++) {
            const uint8_t *rc = (b->n == 5) ? RC_CL[q] : RC_UN[q];
            const int col = k ? PUZZLE_SIDE - 1 - rc[1] : rc[1];
            const int x = rc[0] * PUZZLE_SIDE + col;
            if (r->base_pid[x] != DV_EMPTY) {          /* the fixed left column */
                ok = r->base_pid[x] == b->pid[q] && r->base_rot[x] == (b->spin[q] & 3);
                continue;
            }
            if (used_test(used, b->pid[q])) { ok = false; break; }
            sd.pid[sd.n] = b->pid[q]; sd.rot[sd.n] = b->spin[q] & 3; sd.cell[sd.n] = (uint8_t)x; sd.n++;
        }
        if (!ok) continue;
        const int w1x = (PUZZLE_SIDE - 1) * PUZZLE_SIDE + (k ? PUZZLE_SIDE - 2 : 1);
        const int w2x = (PUZZLE_SIDE - 1) * PUZZLE_SIDE + (k ? PUZZLE_SIDE - 3 : 2);
        if (r->base_pid[w1x] != DV_EMPTY || (b->n == 5 && r->base_pid[w2x] != DV_EMPTY)) continue;
        for (int u = 0; u < b->nwit && nopt < DV_SEED_OPTS; u++) {
            const uint16_t w[2] = { b->wit[u][0], b->wit[u][1] };
            if (used_test(used, w[0]) || (w[1] != TC_NO_PIECE && used_test(used, w[1]))) continue;
            DvSeed o = sd;
            bool wok = true;
            for (int j = 0; j < 2 && wok; j++) {
                if (w[j] == TC_NO_PIECE) continue;
                int rot = -1;
                for (int t = 0; t < 4; t++) if (g_dv_or[w[j]][t].top == 0) { rot = t; break; }
                if (rot < 0) { wok = false; break; }
                o.pid[o.n] = w[j]; o.rot[o.n] = (uint8_t)rot; o.cell[o.n] = (uint8_t)(j ? w2x : w1x); o.n++;
            }
            if (wok) out[nopt++] = o;
        }
    }
    return nopt;
}

static bool dv_seeds_disjoint(const DvSeed *a, const DvSeed *b) {
    for (int i = 0; i < a->n; i++)
        for (int j = 0; j < b->n; j++)
            if (a->pid[i] == b->pid[j] || a->cell[i] == b->cell[j]) return false;
    return true;
}

static void dv_queue_copy(const DvRoot *src, const DvSeed *a, const DvSeed *b, uint8_t mask) {
    DvRoot *r = dv_push_root();
    *r = *src;
    const DvSeed *sd[2] = { a, b };
    for (int k = 0; k < 2; k++)
        if (sd[k])
            for (int j = 0; j < sd[k]->n; j++) {
                r->base_pid[sd[k]->cell[j]] = sd[k]->pid[j];
                r->base_rot[sd[k]->cell[j]] = sd[k]->rot[j];
            }
    dv_root_key(r, 0);
    r->group = (uint32_t)(g_dv_qn - 1);          /* a board of its own */
    r->copy = 0;
    r->seeded = mask;
    g_dv_run.seed_roots++;
}

/* Queue up to N distinct seedings of root qi: both corners where a disjoint
   pair is found, alternating with one corner at a time. Drawn from a stream of
   the board's own fingerprint, so the choice is reproducible. */
static void dv_seed_corners(size_t qi) {
    if (g_p.corner_seeds <= 0 || !g_p.seed_corners) return;
    const DvRoot *r = &g_dv_q[qi];
    uint64_t used[4] = { 0, 0, 0, 0 };
    for (int x = 0; x < NUM_PIECES; x++)
        if (r->base_pid[x] != DV_EMPTY) used_set(used, r->base_pid[x]);
    if (r->top_row < 0 || r->top_row > PUZZLE_SIDE - 4) return;   /* the blocks' rows are full */
    uint8_t rtop[PUZZLE_SIDE];
    for (int c = 0; c < PUZZLE_SIDE; c++) {
        const int x = r->top_row * PUZZLE_SIDE + c;
        rtop[c] = r->base_pid[x] == DV_EMPTY ? 0 : g_dv_or[r->base_pid[x]][r->base_rot[x]].top;
    }
    const bool at12 = r->top_row == PUZZLE_SIDE - 4;
    /* The board's clue frame: the slot whose row-13 TL clue sits on (13,2). */
    int s = -1;
    for (int t = 0; t < 4 && s < 0; t++) {
        if (!(g_tc_slots & (1u << t))) continue;
        if (!g_tc_clued) { s = t; break; }
        const int x = (PUZZLE_SIDE - 3) * PUZZLE_SIDE + 2;
        if (r->base_pid[x] == g_clue[t][3].piece) s = t;
    }
    if (s < 0) return;
    DvSeed *opt0 = xmalloc(2 * DV_SEED_OPTS * sizeof *opt0), *opt1 = opt0 + DV_SEED_OPTS;
    const int n0 = dv_seed_options(r, s, TC_TL, used, rtop, at12, opt0);
    const int n1 = dv_seed_options(r, s, TC_TR, used, rtop, at12, opt1);
    if (n0 || n1) {
        g_dv_run.seed_boards++;
        RNG rng = rng_for(r->fp, 0xC0u, 0x5EED5u, 0);
        uint64_t seen[64]; int nseen = 0;
        const DvRoot parent = *r;                  /* the queue may move */
        for (int v = 0, tries = 0; v < g_p.corner_seeds && tries < 8 * g_p.corner_seeds; tries++) {
            int a = -1, b = -1;
            const bool want_both = n0 && n1 && (v % 2 == 0);
            if (want_both) {
                a = (int)rng_uniform(&rng, (uint32_t)n0);
                for (int t = 0; t < 8 && b < 0; t++) {
                    const int c = (int)rng_uniform(&rng, (uint32_t)n1);
                    if (dv_seeds_disjoint(&opt0[a], &opt1[c])) b = c;
                }
                if (b < 0) continue;
            } else if (n0 && (!n1 || (v / 2) % 2 == 0)) {
                a = (int)rng_uniform(&rng, (uint32_t)n0);
            } else {
                b = (int)rng_uniform(&rng, (uint32_t)n1);
            }
            const uint64_t key = ((uint64_t)(a + 1) << 32) | (uint64_t)(b + 1);
            bool dup = false;
            for (int j = 0; j < nseen; j++) if (seen[j] == key) dup = true;
            if (dup) continue;
            if (nseen < 64) seen[nseen++] = key;
            dv_queue_copy(&parent, a >= 0 ? &opt0[a] : NULL, b >= 0 ? &opt1[b] : NULL,
                          (uint8_t)((a >= 0 ? 1 : 0) | (b >= 0 ? 2 : 0)));
            v++;
        }
    }
    free(opt0);
}

void dv_queue(const uint16_t pid[NUM_PIECES], const uint8_t rot[NUM_PIECES],
              const DvQueue *o) {
    static const DvQueue stock = { 0 };
    if (!o) o = &stock;
    if (g_dv_ran) { g_dv_grpn = 0; g_dv_ran = false; g_dv_multi = false; }
    if (o->copies > 1) g_dv_multi = true;
    if (g_dv_grpn == g_dv_grpcap) {
        g_dv_grpcap = g_dv_grpcap ? g_dv_grpcap * 2 : 256;
        g_dv_grp = xrealloc(g_dv_grp, g_dv_grpcap * sizeof *g_dv_grp);
        g_dv_grp_keep = xrealloc(g_dv_grp_keep, g_dv_grpcap * sizeof *g_dv_grp_keep);
    }
    const uint32_t first = (uint32_t)g_dv_qn;
    g_dv_grp[g_dv_grpn] = g_dv_grp_keep[g_dv_grpn] = first;
    g_dv_grpn++;
    int inc_score = -1;
    if (o->inc_pid) {
        DvBoard inc;
        for (int x = 0; x < NUM_PIECES; x++) {
            if (o->inc_pid[x] >= NUM_PIECES) fatal("internal error: an incumbent with an open cell");
            if (pid[x] != DV_EMPTY && (pid[x] != o->inc_pid[x] || (rot[x] & 3) != (o->inc_rot[x] & 3)))
                fatal("internal error: a board that was not cut from its incumbent");
            inc.c[x] = g_dv_or[o->inc_pid[x]][o->inc_rot[x] & 3];
        }
        inc_score = dv_score(&inc);
    }
    const uint32_t copies = o->copies ? o->copies : 1;
    for (uint32_t k = 0; k < copies; k++) {
        DvRoot *r = dv_push_root();
        memset(r, 0, sizeof *r);
        for (int x = 0; x < NUM_PIECES; x++) {
            r->base_pid[x] = pid[x];
            r->base_rot[x] = pid[x] == DV_EMPTY ? 0 : (uint8_t)(rot[x] & 3);
        }
        r->top_row = -1;
        for (int row = 0; row < PUZZLE_SIDE; row++) {
            bool full = true;
            for (int c = 0; c < PUZZLE_SIDE && full; c++)
                full = pid[row * PUZZLE_SIDE + c] != DV_EMPTY;
            if (!full) break;
            r->top_row = (int8_t)row;
        }
        dv_root_key(r, o->salt * 0x10000ULL + k);
        r->best = -1;
        r->origin = (uint32_t)(g_dv_qn - 1);
        r->group = first;
        r->copy = k;
        r->order = (uint8_t)(o->norders > 0 ? o->orders[k % (uint32_t)o->norders] % DV_NORDERS
                                            : DV_ORDER_MRV);
        r->inc_score = inc_score;
        if (inc_score >= 0)
            for (int x = 0; x < NUM_PIECES; x++) {
                r->inc_pid[x] = (uint8_t)o->inc_pid[x];
                r->inc_rot[x] = (uint8_t)(o->inc_rot[x] & 3);
            }
        if (k == 0) dv_seed_corners(g_dv_qn - 1);   /* its seeded copies follow it */
    }
}

void dv_add(const uint16_t pid[NUM_PIECES], const uint8_t rot[NUM_PIECES]) {
    dv_queue(pid, rot, NULL);
}

/* Can every dive of this board complete under the current frame? A stuck cell
   takes any unused piece its class allows, so a dive fails only when some class
   of open cells has fewer candidates than cells: inner cells need the unused
   inner pieces, corner cells the unused corners, and border cells the unused
   edge pieces -- side by side when edges are held to their sides. */
bool dv_fits(const uint16_t pid[NUM_PIECES]) {
    bool used[NUM_PIECES] = { false };
    for (int x = 0; x < NUM_PIECES; x++) {
        if (pid[x] == DV_EMPTY) continue;
        if (pid[x] >= NUM_PIECES || used[pid[x]]) return false;
        used[pid[x]] = true;
    }
    int open[DV_CLASSES] = { 0 }, have[DV_CLASSES] = { 0 };
    int open_edge = 0, have_edge = 0, open_corner = 0, have_corner = 0;
    bool pooled = false;
    for (int x = 0; x < NUM_PIECES; x++) if (pid[x] == DV_EMPTY) open[dv_cls(x)]++;
    for (int p = 0; p < NUM_PIECES; p++) {
        if (used[p]) continue;
        const int kind = dv_piece_kind(p);
        if (kind == 0) { have[0]++; continue; }
        if (kind == 2) { have_corner++; continue; }
        have_edge++;
        int nk = 0, k1 = -1;
        for (int k = 1; k < DV_CLASSES; k++)
            if (g_dv_pclass[p] >> k & 1u) { nk++; k1 = k; }
        if (nk == 1) have[k1]++; else pooled = true;
    }
    for (int k = 1; k < DV_CLASSES; k++) {
        const bool corner = (k / 3) != 0 && (k % 3) != 0;
        if (corner) open_corner += open[k]; else open_edge += open[k];
    }
    if (open[0] != have[0] || open_corner != have_corner || open_edge != have_edge) return false;
    if (pooled) return true;
    for (int k = 1; k < DV_CLASSES; k++) {
        const bool corner = (k / 3) != 0 && (k % 3) != 0;
        if (!corner && open[k] != have[k]) return false;
    }
    return true;
}

/* -- Per-thread workspace: one root's board, forward-checking prototype and the
      open cells and unused pieces, rebuilt at the start of every job. -------- */
typedef struct {
    DvBoard base, b;
    DvFc    proto, f;
    uint8_t cells[NUM_PIECES], up[NUM_PIECES];
    int     ncell, nup;
    int16_t pslot[NUM_PIECES], xslot[NUM_PIECES];
    float   tpos[NUM_PIECES];        /* the root's fill-order key, per cell */
    bool    ordered;                 /* tpos is live: the root's order is not MRV */
    DvTop  *top;                     /* the job's own polish candidates */
} DvWork;

/* A fill order's key per open cell: order_weight x a place in [0, 1] of the
   open cells' bounding box, 1 where the order starts. centre and ends run
   along the box's longer side. */
static void dv_order_key(int order, const uint8_t *cells, int n, float *tpos) {
    int r0 = PUZZLE_SIDE, r1 = -1, c0 = PUZZLE_SIDE, c1 = -1;
    for (int j = 0; j < n; j++) {
        const int r = cells[j] / PUZZLE_SIDE, c = cells[j] % PUZZLE_SIDE;
        if (r < r0) r0 = r;
        if (r > r1) r1 = r;
        if (c < c0) c0 = c;
        if (c > c1) c1 = c;
    }
    const float tw = g_p.order_weight > 0.0f ? g_p.order_weight : 6.0f;
    const float h = (float)(r1 - r0), w = (float)(c1 - c0);
    for (int j = 0; j < n; j++) {
        const int x = cells[j];
        const float r = (float)(x / PUZZLE_SIDE - r0), c = (float)(x % PUZZLE_SIDE - c0);
        const float along = w >= h ? c : r, span = w >= h ? w : h;
        float t = 0.5f;
        switch (order) {
            case DV_ORDER_LEFT:   if (w > 0) t = 1.0f - c / w; break;
            case DV_ORDER_RIGHT:  if (w > 0) t = c / w; break;
            case DV_ORDER_TOP:    if (h > 0) t = r / h; break;
            case DV_ORDER_BOTTOM: if (h > 0) t = 1.0f - r / h; break;
            case DV_ORDER_CENTRE:
            case DV_ORDER_ENDS:
                if (span > 0) {
                    t = 1.0f - fabsf(2.0f * along / span - 1.0f);
                    if (order == DV_ORDER_ENDS) t = 1.0f - t;
                }
                break;
            default: break;
        }
        tpos[x] = tw * t;
    }
}

static void dv_work_root(DvWork *k, const DvRoot *r) {
    for (int x = 0; x < NUM_PIECES; x++) {
        if (r->base_pid[x] == DV_EMPTY) { k->base.c[x].piece_id = DV_EMPTY; continue; }
        k->base.c[x] = g_dv_or[r->base_pid[x]][r->base_rot[x]];
    }
    dv_fc_init(&k->proto, &k->base);
    k->ncell = k->proto.n_empty;
    memcpy(k->cells, k->proto.elist, (size_t)k->ncell);
    for (int x = 0; x < NUM_PIECES; x++) k->xslot[x] = -1;
    for (int j = 0; j < k->ncell; j++) k->xslot[k->cells[j]] = (int16_t)j;
    k->nup = 0;
    for (int p = 0; p < NUM_PIECES; p++) {
        k->pslot[p] = -1;
        if ((k->proto.unused4.w[(p * 4) >> 6] >> ((p * 4) & 63)) & 0xFULL) {
            k->pslot[p] = (int16_t)k->nup;
            k->up[k->nup++] = (uint8_t)p;
        }
    }
    k->ordered = r->order != DV_ORDER_MRV && k->ncell > 0;
    if (k->ordered) dv_order_key(r->order, k->cells, k->ncell, k->tpos);
}

/* -- Polish candidates --------------------------------------------------------- */
static inline bool dv_key_before(int s1, uint64_t i1, int s2, uint64_t i2) {
    return s1 > s2 || (s1 == s2 && i1 < i2);
}

static void dv_top_offer(DvTop *t, int s, uint64_t idx, uint64_t fp, const uint8_t *brd) {
    for (int i = 0; i < t->n; i++)
        if (t->fp[i] == fp) {                 /* the same board: keep its earliest dive */
            if (dv_key_before(s, idx, t->s[i], t->idx[i])) { t->s[i] = s; t->idx[i] = idx; }
            return;
        }
    int at;
    if (t->n < g_p.polish_top) at = t->n++;
    else {
        at = 0;                               /* the last in key order */
        for (int i = 1; i < t->n; i++)
            if (dv_key_before(t->s[at], t->idx[at], t->s[i], t->idx[i])) at = i;
        if (!dv_key_before(s, idx, t->s[at], t->idx[at])) return;
    }
    t->s[at] = s; t->idx[at] = idx; t->fp[at] = fp;
    memcpy(t->brd[at], brd, sizeof t->brd[at]);
}

static void dv_board_bytes(const DvBoard *b, uint8_t *brd) {
    for (int x = 0; x < NUM_PIECES; x++) {
        brd[x] = (uint8_t)b->c[x].piece_id; brd[NUM_PIECES + x] = b->c[x].rotation;
    }
}

/* A job's result for one root: its best dive, and its polish candidates. */
typedef struct {
    int      best;
    uint64_t best_idx;
    uint8_t  brd[2 * NUM_PIECES];
    uint32_t dives;
} DvLocal;

static void dv_local_take(DvLocal *l, DvWork *k, const DvBoard *b, int s, uint64_t idx, bool polish) {
    l->dives++;
    uint8_t brd[2 * NUM_PIECES];
    const bool better = l->best < 0 || dv_key_before(s, idx, l->best, l->best_idx);
    if (better || polish) dv_board_bytes(b, brd);
    if (better) { l->best = s; l->best_idx = idx; memcpy(l->brd, brd, sizeof brd); }
    if (polish) dv_top_offer(k->top, s, idx, dv_board_fp(b), brd);
}

/* ============================================================================
 * Batch: the stages as parallel jobs
 *
 * Every job is a slice of one board's work: a block of stage-1 dives, a block
 * of one stage-2 round, one polish candidate, or one kick-and-polish walk. A
 * shared FIFO of jobs feeds every thread, so a batch of a few boards still
 * keeps all threads busy -- parallelism over boards alone left most of them
 * idle, since a configuration often sends only a handful of boards and stage 2
 * spends 90% of the dives on the few it promotes.
 *
 * A job never waits for another. A board's next step (the next stage-2 round,
 * after the cross-entropy update; the walks, after the start selection) is
 * released by whichever job finishes the current one last. Results are merged
 * under the board's lock with order-independent keys, and every dive's random
 * stream is keyed by its board and its index, so the output does not depend on
 * the thread count or on the order the jobs ran in.
 * ========================================================================== */
#define DV_BLOCK 32              /* dives per job */

typedef enum { DV_JOB_S1, DV_JOB_S2, DV_JOB_P0, DV_JOB_P1 } DvJobKind;
typedef struct { uint8_t kind; uint32_t root, a, b; } DvJob;

/* Stage-2 state of a board: the learned weights and the round's record. */
typedef struct DvS2 {
    float    *w;                 /* [slot(piece) * ncell + slot(cell)] */
    int32_t  *E;                 /* elite votes, same layout */
    uint8_t  *rec;               /* the round's placements, dive x open cell */
    int      *rsc;               /* the round's scores */
    uint64_t *rord;              /* sort keys of the round */
    uint32_t  per, n2, done, nd; /* dives per round, stage total, done, this round */
    uint64_t  idx0;              /* dive index of this round's first dive */
    int       rd;                /* round in flight */
    int       ncell, nup;
    uint8_t   cells[NUM_PIECES], up[NUM_PIECES];
    int16_t   pslot[NUM_PIECES];
} DvS2;

/* Polish state of a board. */
typedef struct DvPol {
    uint8_t  cells[NUM_PIECES];
    int      nc, ncand, nst, before;
    DvBoard  *cand, *st, *walk;      /* K candidates; walk starts and ends */
    int      *cand_s, *st_s, *walk_s;
    uint64_t *cand_h;
} DvPol;

static DvPol *dv_pol_new(void) {
    const size_t k = (size_t)g_p.polish_top, w = (size_t)dv_walks();
    DvPol *pl = xmalloc(sizeof *pl);
    memset(pl, 0, sizeof *pl);
    pl->cand = xmalloc((k + 2 * w) * sizeof *pl->cand);
    pl->st = pl->cand + k; pl->walk = pl->st + w;
    pl->cand_s = xmalloc((k + 2 * w) * sizeof *pl->cand_s);
    pl->st_s = pl->cand_s + k; pl->walk_s = pl->st_s + w;
    pl->cand_h = xmalloc(k * sizeof *pl->cand_h);
    return pl;
}

static void dv_pol_free(DvPol *pl) {
    free(pl->cand); free(pl->cand_s); free(pl->cand_h); free(pl);
}

static struct {
    DvJob     *q;
    size_t     head, n, cap;
    long       outstanding;      /* queued or running */
    omp_lock_t lock;
} g_js;

static omp_lock_t *g_dv_lock = NULL;     /* one per board of the batch */
static size_t      g_dv_lock_n = 0;
static DvWork    **g_dv_work = NULL;     /* one per thread */
static int         g_dv_nwork = 0;
static uint32_t    g_dv_n1 = 0, g_dv_n2 = 0;
static size_t     *g_dv_wait = NULL;     /* stage-2 boards not yet live, in order */
static size_t      g_dv_wait_n = 0, g_dv_wait_next = 0;
static int         g_dv_live_cap = 0;
static double      g_dv_jt[4];           /* thread-seconds per job kind, this batch */
static uint64_t    g_dv_pgain = 0, g_dv_proots = 0;

static void dv_push(uint8_t kind, uint32_t root, uint32_t a, uint32_t b) {
    omp_set_lock(&g_js.lock);
    if (g_js.n == g_js.cap) {
        const size_t nc = g_js.cap ? g_js.cap * 2 : 1024;
        DvJob *q = xmalloc(nc * sizeof *q);
        for (size_t i = 0; i < g_js.n; i++) q[i] = g_js.q[(g_js.head + i) % g_js.cap];
        free(g_js.q);
        g_js.q = q; g_js.cap = nc; g_js.head = 0;
    }
    g_js.q[(g_js.head + g_js.n) % g_js.cap] = (DvJob){ kind, root, a, b };
    g_js.n++;
    g_js.outstanding++;
    omp_unset_lock(&g_js.lock);
}

/* 1 = a job, 0 = none queued but some still running, -1 = all done. */
static int dv_pop(DvJob *j) {
    omp_set_lock(&g_js.lock);
    int got = -1;
    if (g_js.n) {
        *j = g_js.q[g_js.head];
        g_js.head = (g_js.head + 1) % g_js.cap;
        g_js.n--;
        got = 1;
    } else if (g_js.outstanding > 0) got = 0;
    omp_unset_lock(&g_js.lock);
    return got;
}

static void dv_job_done(void) {
    omp_set_lock(&g_js.lock);
    g_js.outstanding--;
    omp_unset_lock(&g_js.lock);
}

/* Split `total` dives into jobs of DV_BLOCK; returns how many were pushed. */
static int dv_push_blocks(uint8_t kind, uint32_t root, uint32_t total) {
    int nj = 0;
    for (uint32_t a = 0; a < total; a += DV_BLOCK) nj++;
    g_dv_q[root].pending = nj;
    for (uint32_t a = 0; a < total; a += DV_BLOCK)
        dv_push(kind, root, a, total - a < DV_BLOCK ? total - a : DV_BLOCK);
    return nj;
}

/* The last job of a step returns true, exactly once, and then sees what every
   other job of the step wrote. */
static bool dv_step_done(DvRoot *r) {
    int left;
    #pragma omp flush
    #pragma omp atomic capture seq_cst
    left = --r->pending;
    #pragma omp flush
    return left == 0;
}

static int dv_cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Merge a job's result into its board, under the board's lock. */
static void dv_merge(size_t ri, const DvLocal *l, const DvTop *t, bool stopped) {
    DvRoot *r = &g_dv_q[ri];
    omp_set_lock(&g_dv_lock[ri]);
    r->dives += l->dives;
    if (l->best > r->own) r->own = l->best;
    if (l->best >= 0 && (r->best < 0 || dv_key_before(l->best, l->best_idx, r->best, r->best_idx))) {
        r->moved = r->inc_score >= 0;
        r->best = l->best; r->best_idx = l->best_idx;
        memcpy(r->best_pid, l->brd, NUM_PIECES);
        memcpy(r->best_rot, l->brd + NUM_PIECES, NUM_PIECES);
    }
    if (t && r->top)
        for (int i = 0; i < t->n; i++) dv_top_offer(r->top, t->s[i], t->idx[i], t->fp[i], t->brd[i]);
    if (stopped) r->stopped = true;
    omp_unset_lock(&g_dv_lock[ri]);
}

/* -- Stage 1: plain dives ------------------------------------------------------ */
static void dv_job_s1(DvWork *k, uint32_t ri, uint32_t a, uint32_t cnt) {
    DvRoot *r = &g_dv_q[ri];
    const bool polish = r->top != NULL;
    dv_work_root(k, r);
    k->top->n = 0;
    /* Plain dives, or with an incumbent's prior dives pulled by it. */
    const DvPolicy prior = { r->w0, k->pslot, k->xslot, k->ncell, DV_S2_BETA };
    const DvPolicy *pol = r->w0 ? &prior : NULL;
    const float *tpos = k->ordered ? k->tpos : NULL;
    DvLocal l = { .best = -1 };
    bool stopped = false;
    for (uint32_t i = a; i < a + cnt; i++) {
        if (((i - a) & 15u) == 0 && dv_time_up()) { stopped = true; break; }
        RNG rng = rng_for(r->fp, 1u, i, 0u);
        k->b = k->base; dv_fc_reset(&k->f, &k->proto, k->cells, k->ncell);
        dv_dive(&k->b, &k->f, k->cells, k->ncell, &rng, pol, tpos);
        dv_local_take(&l, k, &k->b, dv_score(&k->b), i, polish);
    }
    dv_merge(ri, &l, polish ? k->top : NULL, stopped);
}

/* -- Stage 2: learning rounds -------------------------------------------------- */

/* Cross-entropy update from one round's elite dives: each (piece, cell) pair
   the elite agree on more than chance is pulled up, the rest down, the vote and
   the running weight both clipped so that nothing becomes certain. */
static void dv_cem(DvS2 *s, int nd) {
    int ne = (int)(nd * DV_ELITE);
    if (ne < 1) ne = 1;
    /* Best score first, then dive order: (480 - score) << 32 | dive. */
    for (int i = 0; i < nd; i++)
        s->rord[i] = ((uint64_t)(DV_EDGES - s->rsc[i]) << 32) | (uint64_t)i;
    qsort(s->rord, (size_t)nd, sizeof *s->rord, dv_cmp_u64);
    for (int e = 0; e < ne; e++) {
        const uint8_t *pl = s->rec + (size_t)(uint32_t)s->rord[e] * s->ncell;
        for (int j = 0; j < s->ncell; j++) s->E[(size_t)s->pslot[pl[j]] * s->ncell + j]++;
    }
    for (int i = 0; i < s->nup; i++) {
        const int p = s->up[i];
        int n = 0;
        for (int j = 0; j < s->ncell; j++) n += (g_dv_pclass[p] >> dv_cls(s->cells[j])) & 1u;
        if (!n) continue;
        const double expect = (double)ne / n;
        for (int j = 0; j < s->ncell; j++) {
            if (!(g_dv_pclass[p] >> dv_cls(s->cells[j]) & 1u)) continue;
            const size_t fi = (size_t)i * s->ncell + j;
            float ev = (float)log((s->E[fi] + 0.5) / (expect + 0.5));
            if (ev >  DV_CLIP) ev =  DV_CLIP;
            if (ev < -DV_CLIP) ev = -DV_CLIP;
            float d = s->w[fi] + DV_GAMMA * ev;
            if (d >  DV_CLIP) d =  DV_CLIP;
            if (d < -DV_CLIP) d = -DV_CLIP;
            s->w[fi] = d;
            s->E[fi] = 0;
        }
    }
}

static void dv_polish_begin(uint32_t ri);
static void dv_s2_start(uint32_t ri);

static void dv_s2_free(DvS2 *s) {
    if (!s) return;
    free(s->w); free(s->E); free(s->rec); free(s->rsc); free(s->rord); free(s);
}

/* Release the board's next round, skipping empty ones; at the end of stage 2
   free its state, start its polish, and bring the next waiting board live. */
static void dv_s2_next(uint32_t ri) {
    DvRoot *r = &g_dv_q[ri];
    DvS2 *s = r->s2;
    while (!r->stopped && s->rd < DV_ROUNDS && s->done < s->n2) {
        s->nd = (s->rd == DV_ROUNDS - 1) ? s->n2 - s->done : s->per;
        if (s->nd > 0) { dv_push_blocks(DV_JOB_S2, ri, s->nd); return; }
        s->rd++;
    }
    dv_s2_free(s);
    r->s2 = NULL;
    dv_polish_begin(ri);
    size_t next = SIZE_MAX;
    #pragma omp critical(dv_wait)
    {
        if (g_dv_wait_next < g_dv_wait_n) next = g_dv_wait[g_dv_wait_next++];
    }
    if (next != SIZE_MAX) dv_s2_start((uint32_t)next);
}

static void dv_s2_start(uint32_t ri) {
    DvRoot *r = &g_dv_q[ri];
    DvS2 *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    /* The open cells and unused pieces, exactly as dv_work_root derives them. */
    DvWork *k = xmalloc(sizeof *k);
    dv_work_root(k, r);
    s->ncell = k->ncell; s->nup = k->nup;
    memcpy(s->cells, k->cells, (size_t)k->ncell);
    memcpy(s->up, k->up, (size_t)k->nup);
    memcpy(s->pslot, k->pslot, sizeof s->pslot);
    free(k);
    const size_t nw = (size_t)s->nup * (size_t)s->ncell;
    s->w = xmalloc((nw ? nw : 1) * sizeof *s->w);
    s->E = xmalloc((nw ? nw : 1) * sizeof *s->E);
    if (r->w0) memcpy(s->w, r->w0, nw * sizeof *s->w);   /* the same layout: dv_work_root */
    else       memset(s->w, 0, nw * sizeof *s->w);
    memset(s->E, 0, nw * sizeof *s->E);
    s->n2 = g_dv_n2;
    s->per = g_dv_n2 / (uint32_t)DV_ROUNDS;
    const size_t rmax = (size_t)s->per + DV_ROUNDS;
    s->rec  = xmalloc(rmax * (size_t)(s->ncell ? s->ncell : 1));
    s->rsc  = xmalloc(rmax * sizeof *s->rsc);
    s->rord = xmalloc(rmax * sizeof *s->rord);
    s->idx0 = g_dv_n1;
    r->s2 = s;
    dv_s2_next(ri);
}

static void dv_job_s2(DvWork *k, uint32_t ri, uint32_t a, uint32_t cnt) {
    DvRoot *r = &g_dv_q[ri];
    DvS2 *s = r->s2;
    const bool polish = r->top != NULL;
    dv_work_root(k, r);
    k->top->n = 0;
    const DvPolicy pol = { s->w, k->pslot, k->xslot, k->ncell, DV_S2_BETA };
    DvLocal l = { .best = -1 };
    bool stopped = false;
    for (uint32_t i = a; i < a + cnt; i++) {
        if (((i - a) & 15u) == 0 && dv_time_up()) { stopped = true; break; }
        RNG rng = rng_for(r->fp, 3u + (uint32_t)s->rd, i, 0u);
        k->b = k->base; dv_fc_reset(&k->f, &k->proto, k->cells, k->ncell);
        dv_dive(&k->b, &k->f, k->cells, k->ncell, &rng, &pol, k->ordered ? k->tpos : NULL);
        const int sc = dv_score(&k->b);
        s->rsc[i] = sc;
        uint8_t *pl = s->rec + (size_t)i * k->ncell;
        for (int j = 0; j < k->ncell; j++) pl[j] = (uint8_t)k->b.c[k->cells[j]].piece_id;
        dv_local_take(&l, k, &k->b, sc, s->idx0 + i, polish);
    }
    dv_merge(ri, &l, polish ? k->top : NULL, stopped);
    if (!dv_step_done(r)) return;
    /* The round's last job: learn from it and release the next. */
    if (!r->stopped && s->rd < DV_ROUNDS - 1) dv_cem(s, (int)s->nd);
    s->done += s->nd; s->idx0 += s->nd; s->rd++;
    dv_s2_next(ri);
}

/* -- Polish: candidates, start selection, walks --------------------------------- */

/* Polish a board whose best dive is within DV_POLISH_MARGIN of S: its polish
   candidates in key order, one job each. */
static void dv_polish_begin(uint32_t ri) {
    DvRoot *r = &g_dv_q[ri];
    if (!r->top || !r->top->n || r->best < 0 || r->best < g_p.emit_score - DV_POLISH_MARGIN
        || dv_time_up())
        return;
    DvTop *t = r->top;
    for (int a = 1; a < t->n; a++)                 /* insertion sort by key */
        for (int b = a; b > 0 && dv_key_before(t->s[b], t->idx[b], t->s[b-1], t->idx[b-1]); b--) {
            int si = t->s[b]; t->s[b] = t->s[b-1]; t->s[b-1] = si;
            uint64_t u = t->idx[b]; t->idx[b] = t->idx[b-1]; t->idx[b-1] = u;
            u = t->fp[b]; t->fp[b] = t->fp[b-1]; t->fp[b-1] = u;
            uint8_t tmp[2 * NUM_PIECES];
            memcpy(tmp, t->brd[b], sizeof tmp); memcpy(t->brd[b], t->brd[b-1], sizeof tmp);
            memcpy(t->brd[b-1], tmp, sizeof tmp);
        }
    DvPol *pl = dv_pol_new();
    for (int x = 0; x < NUM_PIECES; x++)
        if (r->base_pid[x] == DV_EMPTY) pl->cells[pl->nc++] = (uint8_t)x;
    pl->ncand = t->n;
    pl->before = r->best;
    r->pol = pl;
    r->pending = t->n;
    for (int c = 0; c < t->n; c++) dv_push(DV_JOB_P0, ri, (uint32_t)c, 0);
}

static void dv_polish_end(uint32_t ri);

static void dv_job_p0(uint32_t ri, uint32_t c) {
    DvRoot *r = &g_dv_q[ri];
    DvPol *pl = r->pol;
    const uint8_t *d = r->top->brd[c];
    DvBoard *b = &pl->cand[c];
    for (int x = 0; x < NUM_PIECES; x++) b->c[x] = g_dv_or[d[x]][d[NUM_PIECES + x]];
    const int g = dv_polish(b, pl->cells, pl->nc);
    const int sc = dv_score(b);
    if (sc != r->top->s[c] + g)
        fatal("internal error: polish gained %d but the score moved %d -> %d",
              g, r->top->s[c], sc);
    uint64_t h = 14695981039346656037ULL;
    for (int j = 0; j < pl->nc; j++) {
        h ^= b->c[pl->cells[j]].piece_id; h *= 1099511628211ULL;
        h ^= b->c[pl->cells[j]].rotation; h *= 1099511628211ULL;
    }
    pl->cand_s[c] = sc; pl->cand_h[c] = h;
    if (!dv_step_done(r)) return;
    /* The last candidate: keep the best K/2 (rounded up) distinct polished
       boards as the walks' starts, in candidate order, then release the walks. */
    int st_t[(DV_POLISH_MAX + 1) / 2];
    uint64_t st_h[(DV_POLISH_MAX + 1) / 2];
    pl->nst = 0;
    for (int t = 0; t < pl->ncand; t++) {
        bool dup = false;
        for (int j = 0; j < pl->nst; j++) if (st_h[j] == pl->cand_h[t]) dup = true;
        if (dup) continue;
        int at = -1;
        if (pl->nst < dv_walks()) at = pl->nst++;
        else {
            int lo = 0;
            for (int j = 1; j < pl->nst; j++) if (pl->st_s[j] < pl->st_s[lo]) lo = j;
            if (pl->cand_s[t] > pl->st_s[lo]) at = lo;
        }
        if (at >= 0) { st_t[at] = t; st_h[at] = pl->cand_h[t]; pl->st_s[at] = pl->cand_s[t]; }
    }
    for (int j = 0; j < pl->nst; j++) pl->st[j] = pl->cand[st_t[j]];
    if (g_p.polish <= 0) {                           /* polish only: no walks */
        for (int j = 0; j < pl->nst; j++) { pl->walk[j] = pl->st[j]; pl->walk_s[j] = pl->st_s[j]; }
        dv_polish_end(ri);
        return;
    }
    r->pending = pl->nst;
    for (int j = 0; j < pl->nst; j++) dv_push(DV_JOB_P1, ri, (uint32_t)j, 0);
}

/* Kick-and-polish walk j from start j: kick, re-polish the touched cells, keep
   the result when it is not worse. */
static void dv_job_p1(uint32_t ri, uint32_t j) {
    DvRoot *r = &g_dv_q[ri];
    DvPol *pl = r->pol;
    bool inreg[NUM_PIECES] = { false }, inq[NUM_PIECES] = { false };
    for (int c = 0; c < pl->nc; c++) inreg[pl->cells[c]] = true;
    int queue[NUM_PIECES];
    RNG rng = rng_for(r->fp, 0x9011u, j, 0u);
    DvBoard walk = pl->st[j], b;
    int walk_s = pl->st_s[j];
    const int iters = g_p.polish / pl->nst + ((int)j < g_p.polish % pl->nst);
    for (int it = 0; it < iters; it++) {
        b = walk;
        const int qn = dv_kick(&b, pl->cells, pl->nc, inreg, &rng, queue, inq);
        dv_polish_q(&b, pl->cells, pl->nc, inreg, queue, qn, inq);
        const int sc = dv_score(&b);
        if (sc >= walk_s) { walk_s = sc; walk = b; }
    }
    pl->walk[j] = walk; pl->walk_s[j] = walk_s;
    if (dv_step_done(r)) dv_polish_end(ri);
}

/* The best start, then any walk that beat it, in walk order. */
static void dv_polish_end(uint32_t ri) {
    DvRoot *r = &g_dv_q[ri];
    DvPol *pl = r->pol;
    int cur_s = -1, cur = -1;
    bool from_walk = false;
    for (int j = 0; j < pl->nst; j++) if (pl->st_s[j] > cur_s) { cur_s = pl->st_s[j]; cur = j; }
    for (int j = 0; j < pl->nst; j++)
        if (pl->walk_s[j] > cur_s) { cur_s = pl->walk_s[j]; cur = j; from_walk = true; }
    omp_set_lock(&g_dv_lock[ri]);
    if (cur_s > r->own) r->own = cur_s;
    /* On the plateau a polished board that ties the incumbent, still in place,
       replaces it too. */
    const bool tie = g_p.plateau && r->inc_score >= 0 && !r->moved && cur_s == r->best;
    if (cur >= 0 && (cur_s > r->best || tie)) {
        r->moved = r->inc_score >= 0;
        const DvBoard *b = from_walk ? &pl->walk[cur] : &pl->st[cur];
        for (int x = 0; x < NUM_PIECES; x++) {
            r->best_pid[x] = (uint8_t)b->c[x].piece_id;
            r->best_rot[x] = b->c[x].rotation;
        }
        if (cur_s > pl->before) {
            #pragma omp atomic
            g_dv_pgain += (uint64_t)(cur_s - pl->before);
            #pragma omp atomic
            g_dv_proots++;
        }
        r->best = cur_s;
    }
    omp_unset_lock(&g_dv_lock[ri]);
    dv_pol_free(pl);
    r->pol = NULL;
}

/* -- Incumbents ------------------------------------------------------------------ */

/* The prior of a root cut from an incumbent: w(p, x) = +prior where the
   incumbent holds piece p on open cell x with every edge of x matched, -nogo
   where x has a broken edge, 0 elsewhere; in the layout dv_work_root gives the
   root's stage-2 weights. */
static float *dv_prior(const DvRoot *r) {
    DvWork *k = xmalloc(sizeof *k);
    dv_work_root(k, r);
    const size_t nw = (size_t)k->nup * (size_t)k->ncell;
    float *w = xmalloc((nw ? nw : 1) * sizeof *w);
    memset(w, 0, nw * sizeof *w);
    DvBoard inc;
    for (int x = 0; x < NUM_PIECES; x++) inc.c[x] = g_dv_or[r->inc_pid[x]][r->inc_rot[x]];
    const float up = g_p.prior < DV_CLIP ? g_p.prior : DV_CLIP;
    const float down = g_p.nogo < DV_CLIP ? g_p.nogo : DV_CLIP;
    for (int j = 0; j < k->ncell; j++) {
        const int x = k->cells[j], ps = k->pslot[r->inc_pid[x]];
        if (ps < 0) continue;
        int nn = 0;
        for (int d = 0; d < 4; d++) nn += dv_nb(x, d) >= 0;
        w[(size_t)ps * (size_t)k->ncell + (size_t)j] = dv_local(&inc, x) == nn ? up : -down;
    }
    free(k);
    return w;
}

int dv_result(size_t i, uint16_t pid[NUM_PIECES], uint8_t rot[NUM_PIECES]) {
    if (i >= g_dv_grpn) return -1;
    const DvRoot *r = &g_dv_q[g_dv_grp_keep[i]];
    if (r->best < 0) return -1;
    for (int x = 0; x < NUM_PIECES; x++) { pid[x] = r->best_pid[x]; rot[x] = r->best_rot[x]; }
    return r->best;
}

/* -- The scheduler ------------------------------------------------------------- */
static void dv_exec(DvWork *k, const DvJob *j) {
    const double t0 = omp_get_wtime();
    switch (j->kind) {
        case DV_JOB_S1: dv_job_s1(k, j->root, j->a, j->b); break;
        case DV_JOB_S2: dv_job_s2(k, j->root, j->a, j->b); break;
        case DV_JOB_P0: dv_job_p0(j->root, j->a); break;
        default:        dv_job_p1(j->root, j->a); break;
    }
    const double dt = omp_get_wtime() - t0;
    #pragma omp atomic
    g_dv_jt[j->kind] += dt;
}

/* Run every queued job, and every job they release, on nt threads. */
static void dv_drain(int nt) {
    #pragma omp parallel num_threads(nt)
    {
        DvWork *k = g_dv_work[omp_get_thread_num()];
        int idle = 0;
        for (;;) {
            DvJob j;
            const int got = dv_pop(&j);
            if (got < 0) break;
            if (got == 0) {                 /* others are finishing a step */
                if (++idle < 64) sched_yield();
                else { struct timespec ts = { 0, 20000 }; nanosleep(&ts, NULL); }
                continue;
            }
            idle = 0;
            dv_exec(k, &j);
            dv_job_done();
        }
    }
}

static int dv_cmp_int(const void *a, const void *b) {
    const int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static double dv_cpu_seconds(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    return (double)ru.ru_utime.tv_sec + 1e-6 * (double)ru.ru_utime.tv_usec
         + (double)ru.ru_stime.tv_sec + 1e-6 * (double)ru.ru_stime.tv_usec;
}

/* Keep, for dv_flush, the kept copy of every board of the last batch whose
   best is >= S; returns how many. */
uint64_t dv_keep_last(void) {
    const DvRoot *q = g_dv_q;
    uint64_t kept = 0;
    bool cfg_named = false;
    uint32_t cfg = 0;
    for (size_t i = 0; i < g_dv_last_n; i++) {
        const DvRoot *r = &q[i];
        if (!r->kept_copy || r->best < 0 || r->best < g_p.emit_score) continue;
        if (!cfg_named) {
            if (g_dv_ncfg == g_dv_cfgcap) {
                g_dv_cfgcap = g_dv_cfgcap ? g_dv_cfgcap * 2 : 64;
                g_dv_cfg = xrealloc(g_dv_cfg, g_dv_cfgcap * sizeof *g_dv_cfg);
            }
            g_dv_cfg[g_dv_ncfg] = xmalloc(strlen(g_dv_last_id) + 1);
            strcpy(g_dv_cfg[g_dv_ncfg], g_dv_last_id);
            cfg = (uint32_t)g_dv_ncfg++;
            cfg_named = true;
        }
        if (g_dv_kn == g_dv_kcap) {
            g_dv_kcap = g_dv_kcap ? g_dv_kcap * 2 : 256;
            g_dv_keep = xrealloc(g_dv_keep, g_dv_kcap * sizeof *g_dv_keep);
        }
        DvKeep *kp = &g_dv_keep[g_dv_kn++];
        memcpy(kp->pid, r->best_pid, NUM_PIECES);
        memcpy(kp->rot, r->best_rot, NUM_PIECES);
        uint64_t h = 14695981039346656037ULL;
        for (int x = 0; x < NUM_PIECES; x++) {
            h ^= kp->pid[x]; h *= 1099511628211ULL;
            h ^= kp->rot[x]; h *= 1099511628211ULL;
        }
        kp->fp = h ? h : 1;
        kp->seq = r->seq; kp->cfg = cfg; kp->score = r->best; kp->seeded = r->seeded;
        kp->top_row = r->top_row;
        kept++;
    }
    g_dv_run.kept += kept;
    return kept;
}

void dv_run(const char *id) {
    const size_t n = g_dv_qn;
    g_dv_qn = 0;
    g_dv_run.last_best = -1;
    if (!n) return;
    const double t0 = omp_get_wtime();
    const int nt = g_p.threads > 0 ? g_p.threads : omp_get_max_threads();
    const bool polish = g_p.polish >= 0;
    DvRoot *q = g_dv_q;

    if (!g_js.cap) omp_init_lock(&g_js.lock);
    if (g_dv_nwork < nt) {
        g_dv_work = xrealloc(g_dv_work, (size_t)nt * sizeof *g_dv_work);
        for (int t = g_dv_nwork; t < nt; t++) {
            g_dv_work[t] = xmalloc(sizeof(DvWork));
            g_dv_work[t]->top = dv_top_new();
        }
        g_dv_nwork = nt;
    }
    if (g_dv_lock_n < n) {
        for (size_t i = 0; i < g_dv_lock_n; i++) omp_destroy_lock(&g_dv_lock[i]);
        free(g_dv_lock);
        g_dv_lock = xmalloc(n * sizeof *g_dv_lock);
        for (size_t i = 0; i < n; i++) omp_init_lock(&g_dv_lock[i]);
        g_dv_lock_n = n;
    }
    const bool priors = g_p.prior > 0.0f || g_p.nogo > 0.0f;
    for (size_t i = 0; i < n; i++) {
        q[i].top = NULL; q[i].s2 = NULL; q[i].pol = NULL; q[i].w0 = NULL;
        q[i].pending = 0; q[i].stopped = false; q[i].best = -1; q[i].best_idx = 0;
        q[i].dives = 0; q[i].own = -1; q[i].moved = false;
        if (polish) q[i].top = dv_top_new();
        if (q[i].inc_score >= 0) {
            /* The incumbent is the best so far; a dive must beat it (index 0
               wins every tie), so the board kept is never worse. On the
               plateau it loses every tie instead (the last index). */
            q[i].best = q[i].inc_score;
            q[i].best_idx = g_p.plateau ? UINT64_MAX : 0;
            memcpy(q[i].best_pid, q[i].inc_pid, NUM_PIECES);
            memcpy(q[i].best_rot, q[i].inc_rot, NUM_PIECES);
            if (priors) q[i].w0 = dv_prior(&q[i]);
        }
    }
    uint32_t n1 = (uint32_t)(g_p.dives * DV_S1_SHARE + 0.5);
    if (n1 < 1) n1 = 1;
    if (n1 > g_p.dives) n1 = g_p.dives;
    g_dv_n1 = n1;
    g_dv_n2 = g_p.dives - n1;
    memset(g_dv_jt, 0, sizeof g_dv_jt);
    g_dv_pgain = g_dv_proots = 0;

    /* Stage 1: every board's plain dives, all at once. */
    const double t_s1 = omp_get_wtime(), c_s1 = dv_cpu_seconds();
    for (size_t i = 0; i < n; i++) dv_push_blocks(DV_JOB_S1, (uint32_t)i, n1);
    dv_drain(nt);
    const double d_s1 = omp_get_wtime() - t_s1, u_s1 = dv_cpu_seconds() - c_s1;

    /* Median of the stage-1 bests. */
    int *sc = xmalloc(n * sizeof *sc);
    size_t ns = 0;
    for (size_t i = 0; i < n; i++) if (q[i].best >= 0) sc[ns++] = q[i].best;
    int median = -1, s1_best = -1;
    if (ns) {
        qsort(sc, ns, sizeof *sc, dv_cmp_int);
        median = sc[(ns - 1) / 2];
        s1_best = sc[ns - 1];
    }
    free(sc);

    /* Stage 2: every board whose stage-1 best is >= S - margin; if that is fewer
       than 20% of the boards, also the top 10% by stage-1 best (at least one;
       ties by queue order). */
    const uint32_t n2 = g_dv_n2;
    size_t n_s2 = 0, n_scored = 0;
    size_t *sel = xmalloc(n * sizeof *sel);
    for (size_t i = 0; i < n; i++) {
        q[i].stage2 = q[i].best >= 0 && n2 > 0 && q[i].best >= g_p.emit_score - DV_S2_MARGIN;
        n_scored += q[i].best >= 0;
        if (q[i].stage2) n_s2++;
    }
    if (n2 > 0 && n_scored && (double)n_s2 < DV_S2_MIN_SHARE * (double)n_scored) {
        size_t want = (size_t)ceil(DV_S2_TOP_SHARE * (double)n_scored);
        if (want < 1) want = 1;
        size_t m = 0;
        for (size_t i = 0; i < n; i++) if (q[i].best >= 0) sel[m++] = i;
        for (size_t a = 0; a < want && a < m; a++) {
            size_t b = a;
            for (size_t c = a + 1; c < m; c++)
                if (q[sel[c]].best > q[sel[b]].best) b = c;
            const size_t t = sel[b];
            memmove(&sel[a + 1], &sel[a], (b - a) * sizeof *sel);
            sel[a] = t;
            q[t].stage2 = true;
        }
    }
    n_s2 = 0;
    for (size_t i = 0; i < n; i++) if (q[i].stage2) sel[n_s2++] = i;
    if (dv_time_up()) n_s2 = 0;                      /* stopped: stage 2 never runs */

    /* Stage 2 and polish, one pool of jobs: a board is polished as soon as its
       own stage 2 ends. At most g_dv_live_cap boards learn at once, which bounds
       the memory their round records take. */
    const double t_s2 = omp_get_wtime(), c_s2 = dv_cpu_seconds();
    g_dv_wait = sel; g_dv_wait_n = n_s2; g_dv_wait_next = 0;
    g_dv_live_cap = 4 * nt < 16 ? 16 : 4 * nt;
    size_t live = n_s2 < (size_t)g_dv_live_cap ? n_s2 : (size_t)g_dv_live_cap;
    g_dv_wait_next = live;
    for (size_t j = 0; j < live; j++) dv_s2_start((uint32_t)sel[j]);
    for (size_t i = 0; i < n; i++) if (!q[i].stage2 || !n_s2) dv_polish_begin((uint32_t)i);
    dv_drain(nt);
    const double d_s2 = omp_get_wtime() - t_s2, u_s2 = dv_cpu_seconds() - c_s2;
    free(sel);
    g_dv_wait = NULL; g_dv_wait_n = g_dv_wait_next = 0;

    for (size_t i = 0; i < n; i++) {
        free(q[i].top); q[i].top = NULL;
        free(q[i].w0); q[i].w0 = NULL;
    }
    g_dv_run.t_s1 += d_s1; g_dv_run.t_s2 += d_s2;
    g_dv_run.u_s1 += u_s1; g_dv_run.u_s2 += u_s2;
    g_dv_run.jt_s2 += g_dv_jt[DV_JOB_S2];
    g_dv_run.jt_pol += g_dv_jt[DV_JOB_P0] + g_dv_jt[DV_JOB_P1];
    g_dv_run.pol_gain += g_dv_pgain; g_dv_run.pol_roots += g_dv_proots;

    /* The copy kept for each board: its best, the earliest on a tie. */
    uint32_t *gbest = xmalloc(n * sizeof *gbest);
    for (size_t i = 0; i < n; i++) gbest[i] = UINT32_MAX;
    for (size_t i = 0; i < n; i++) {
        const uint32_t g = q[i].group;
        const DvRoot *b = gbest[g] == UINT32_MAX ? NULL : &q[gbest[g]];
        if (!b || q[i].best > b->best || (q[i].best == b->best && q[i].moved && !b->moved))
            gbest[g] = (uint32_t)i;
    }
    for (size_t j = 0; j < g_dv_grpn; j++) g_dv_grp_keep[j] = gbest[g_dv_grp[j]];
    for (size_t i = 0; i < n; i++) q[i].kept_copy = gbest[q[i].group] == i;
    free(gbest);
    for (size_t j = 0; j < g_dv_grpn; j++) {
        const DvRoot *r = &q[g_dv_grp_keep[j]];
        if (r->inc_score < 0) continue;
        g_dv_run.inc_boards++;
        if (r->best > r->inc_score) {
            g_dv_run.inc_better++;
            g_dv_run.inc_gain += r->best - r->inc_score;
        }
    }
    uint64_t dives = 0;
    int best = -1;
    for (size_t i = 0; i < n; i++) {
        dives += q[i].dives;
        if (q[i].best > best) best = q[i].best;
    }
    g_dv_last_n = n;
    snprintf(g_dv_last_id, sizeof g_dv_last_id, "%s", id);
    g_dv_ran = true;
    const uint64_t kept = g_dv_keeping ? dv_keep_last() : 0;
    /* Corner seeds: each unseeded board against the best of its seeded copies,
       which follow it in the queue. */
    uint64_t c_pairs = 0, c_wins = 0, c_seeded = 0;
    for (size_t i = 0; i < n; i++) {
        if (q[i].seeded) {
            c_seeded++;
            if (q[i].best > g_dv_run.seed_best) g_dv_run.seed_best = q[i].best;
            continue;
        }
        if (q[i].best > g_dv_run.plain_best) g_dv_run.plain_best = q[i].best;
        int bs = -1;
        for (size_t j = i + 1; j < n && q[j].seeded && q[j].origin == q[i].origin; j++)
            if (q[j].best > bs) bs = q[j].best;
        if (bs < 0 || q[i].best < 0) continue;
        c_pairs++; c_wins += bs > q[i].best;
        g_dv_run.seed_pairs++;
        g_dv_run.seed_wins += bs > q[i].best;
        g_dv_run.seed_ties += bs == q[i].best;
        g_dv_run.seed_losses += bs < q[i].best;
        g_dv_run.seed_diff += bs - q[i].best;
    }
    const double dt = omp_get_wtime() - t0;
    g_dv_run.roots += n; g_dv_run.stage2 += n_s2; g_dv_run.dives += dives;
    g_dv_run.t += dt;
    if (best > g_dv_run.best) g_dv_run.best = best;
    g_dv_run.last_best = best;
    if (g_verbose) {
        printf("[dive] %s boards=%zu stage1_best=%d median=%d stage2=%zu best=%d "
               "kept>=%d:%" PRIu64 " dives=%" PRIu64 " t=%.2fs (stage 1 %.2fs, "
               "stage 2 + polish %.2fs; cpu %.0f%%)",
               id, n, s1_best, median, n_s2, best, g_p.emit_score, kept, dives, dt,
               d_s1, d_s2, 100.0 * (u_s1 + u_s2) / ((d_s1 + d_s2) * nt + 1e-9));
        if (c_seeded)
            printf(" seeded=%" PRIu64 " (seeded beats plain on %" PRIu64 " of %" PRIu64 " boards)",
                   c_seeded, c_wins, c_pairs);
        printf("\n");
        /* Boards dived in copies: every copy's own best (its dives and polish,
           the incumbent aside), then the board kept. */
        for (size_t j = 0; g_dv_multi && j < g_dv_grpn; j++) {
            const uint32_t g = g_dv_grp[j];
            printf("[copies] %s board %zu:", id, j);
            if (q[g].inc_score >= 0) printf(" incumbent %d; copies", q[g].inc_score);
            for (size_t i = g; i < n; i++)
                if (q[i].group == g) printf(" %d", q[i].own);
            printf(" -> kept %d\n", q[g_dv_grp_keep[j]].best);
        }
        fflush(stdout);
    }
}

/* ============================================================================
 * Reporting and output
 * ========================================================================== */
static int dv_cmp_keep(const void *a, const void *b) {
    const DvKeep *x = a, *y = b;
    if (x->score != y->score) return y->score - x->score;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

/* Where a written board's broken edges sit: the top-left and top-right 4x4
   corner blocks (rows 12-15), the seam between the stop row and the row above
   it, and everything else. */
static void dv_break_places(const DvKeep *kp) {
    Oriented c[NUM_PIECES];
    for (int x = 0; x < NUM_PIECES; x++) c[x] = g_dv_or[kp->pid[x]][kp->rot[x]];
    uint64_t tl = 0, tr = 0;
    for (int x = 0; x < NUM_PIECES; x++) {
        const int r = x / PUZZLE_SIDE, col = x % PUZZLE_SIDE;
        for (int d = 0; d < 2; d++) {                  /* right, then up */
            const int y = d ? (r < PUZZLE_SIDE - 1 ? x + PUZZLE_SIDE : -1)
                            : (col < PUZZLE_SIDE - 1 ? x + 1 : -1);
            if (y < 0) continue;
            const bool broken = d ? c[x].top != c[y].bottom : c[x].right != c[y].left;
            if (!broken) continue;
            const int ry = y / PUZZLE_SIDE, cy = y % PUZZLE_SIDE;
            const bool in_tl = r >= PUZZLE_SIDE - 4 && ry >= PUZZLE_SIDE - 4 && col <= 3 && cy <= 3;
            const bool in_tr = r >= PUZZLE_SIDE - 4 && ry >= PUZZLE_SIDE - 4
                            && col >= PUZZLE_SIDE - 4 && cy >= PUZZLE_SIDE - 4;
            if (in_tl) tl++;
            else if (in_tr) tr++;
            else if (d && r == kp->top_row) g_dv_run.brk_seam++;
            else g_dv_run.brk_rest++;
        }
    }
    g_dv_run.brk_tl += tl; g_dv_run.brk_tr += tr;
    g_dv_run.clean_tl += !tl; g_dv_run.clean_tr += !tr;
    if (kp->seeded) {
        g_dv_run.seed_written++;
        g_dv_run.sbrk_tl += tl; g_dv_run.sbrk_tr += tr;
        g_dv_run.sclean_tl += !tl; g_dv_run.sclean_tr += !tr;
    }
}

static inline char *dv_u32a(char *p, uint32_t v) {
    char t[10]; int k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k) *p++ = t[--k];
    return p;
}

static void (*g_dv_row_hook)(int score) = NULL;
void dv_set_row_hook(void (*hook)(int score)) { g_dv_row_hook = hook; }

void dv_flush(FILE *fp) {
    if (!g_dv_kn || !fp) return;
    qsort(g_dv_keep, g_dv_kn, sizeof *g_dv_keep, dv_cmp_keep);
    size_t hsz = 64;
    while (hsz < 2 * g_dv_kn) hsz <<= 1;
    uint64_t *hs = xmalloc(hsz * sizeof *hs);
    memset(hs, 0, hsz * sizeof *hs);
    char *line = xmalloc(4096);
    for (size_t i = 0; i < g_dv_kn; i++) {
        const DvKeep *kp = &g_dv_keep[i];
        if (g_p.max_written && g_dv_run.written >= g_p.max_written) break;
        size_t h = (size_t)kp->fp & (hsz - 1);
        bool dup = false;
        while (hs[h]) { if (hs[h] == kp->fp) { dup = true; break; } h = (h + 1) & (hsz - 1); }
        if (dup) { g_dv_run.dup++; continue; }
        hs[h] = kp->fp;
        uint32_t pos[NUM_PIECES], rot[NUM_PIECES];
        for (int p = 0; p < NUM_PIECES; p++) { pos[p] = 999; rot[p] = 0; }
        for (int x = 0; x < NUM_PIECES; x++) { pos[kp->pid[x]] = (uint32_t)x; rot[kp->pid[x]] = kp->rot[x]; }
        char *p = line;
        for (int j = 0; j < NUM_PIECES; j++) { *p++ = ','; *p++ = ' '; p = dv_u32a(p, pos[j]); }
        for (int j = 0; j < NUM_PIECES; j++) { *p++ = ','; *p++ = ' '; p = dv_u32a(p, rot[j]); }
        *p++ = '\n';
        fprintf(fp, "%s, %d", g_dv_cfg[kp->cfg], kp->score);
        fwrite(line, 1, (size_t)(p - line), fp);
        g_dv_run.written++;
        g_dv_run.hist[kp->score]++;
        if (g_dv_row_hook) g_dv_row_hook(kp->score);
        if (kp->top_row < g_dv_run.top_min) g_dv_run.top_min = kp->top_row;
        if (kp->top_row > g_dv_run.top_max) g_dv_run.top_max = kp->top_row;
        dv_break_places(kp);
    }
    free(line); free(hs);
    for (size_t c = 0; c < g_dv_ncfg; c++) free(g_dv_cfg[c]);
    g_dv_ncfg = 0;
    g_dv_kn = 0;
    fflush(fp);
}

void dv_print_summary(double wall_total) {
    if (!g_dv_run.roots) return;
    printf("[sum] end dives: M=%u boards=%" PRIu64 " stage2=%" PRIu64 " dives=%" PRIu64
           " (%.0f dives/s) best=%d kept>=%d:%" PRIu64 " written=%" PRIu64
           " duplicates=%" PRIu64 " time=%.1fs\n",
           g_p.dives, g_dv_run.roots, g_dv_run.stage2, g_dv_run.dives,
           g_dv_run.t > 0 ? (double)g_dv_run.dives / g_dv_run.t : 0.0,
           g_dv_run.best, g_p.emit_score, g_dv_run.kept, g_dv_run.written, g_dv_run.dup,
           g_dv_run.t);
    if (g_p.polish >= 0)
        printf("[sum] end dive polish: %" PRIu64 " board(s) improved, +%" PRIu64
               " edges in all (%d kick-and-polish rounds per board)\n",
               g_dv_run.pol_roots, g_dv_run.pol_gain, g_p.polish);
    {
        const double tt = g_dv_run.t_s1 + g_dv_run.t_s2;
        const double w = wall_total > 0.0 ? wall_total : 1.0;
        const int nt = g_p.threads > 0 ? g_p.threads : omp_get_max_threads();
        printf("[sum] end dive time: stage 1 %.1fs, stage 2 + polish %.1fs = %.1fs "
               "(%.0f%% of the run's %.1fs wall time)\n",
               g_dv_run.t_s1, g_dv_run.t_s2, tt, 100.0 * tt / w, wall_total);
        printf("[sum] end dive cpu use: stage 1 %.0f%%, stage 2 + polish %.0f%% of %d threads "
               "(thread-seconds: stage 2 %.1f, polish %.1f)\n",
               g_dv_run.t_s1 > 0 ? 100.0 * g_dv_run.u_s1 / (g_dv_run.t_s1 * nt) : 0.0,
               g_dv_run.t_s2 > 0 ? 100.0 * g_dv_run.u_s2 / (g_dv_run.t_s2 * nt) : 0.0,
               nt, g_dv_run.jt_s2, g_dv_run.jt_pol);
    }
    if (g_dv_run.written) {
        const double W = (double)g_dv_run.written;
        char seam[32];
        if (g_dv_run.top_min == g_dv_run.top_max)
            snprintf(seam, sizeof seam, "r%d/r%d", g_dv_run.top_min, g_dv_run.top_min + 1);
        else
            snprintf(seam, sizeof seam, "above the top full row");
        printf("[sum] breaks by place over %" PRIu64 " written board(s), per board: "
               "TL 4x4 %.2f (%.0f%% clean), TR 4x4 %.2f (%.0f%% clean), "
               "seam %s %.2f, rest %.2f\n", g_dv_run.written,
               g_dv_run.brk_tl / W, 100.0 * g_dv_run.clean_tl / W,
               g_dv_run.brk_tr / W, 100.0 * g_dv_run.clean_tr / W,
               seam, g_dv_run.brk_seam / W, g_dv_run.brk_rest / W);
        if (g_dv_run.seed_written) {
            const double S2 = (double)g_dv_run.seed_written;
            printf("[sum]   of which corner-seeded %" PRIu64 ": TL 4x4 %.2f (%.0f%% clean), "
                   "TR 4x4 %.2f (%.0f%% clean)\n", g_dv_run.seed_written,
                   g_dv_run.sbrk_tl / S2, 100.0 * g_dv_run.sclean_tl / S2,
                   g_dv_run.sbrk_tr / S2, 100.0 * g_dv_run.sclean_tr / S2);
        }
    }
    if (g_p.corner_seeds > 0 && g_p.seed_corners) {
        printf("[sum] corner seeds: %" PRIu64 " of %" PRIu64 " boards had an alive "
               "block, %" PRIu64 " seeded copies dived; best seeded %d, best unseeded %d",
               g_dv_run.seed_boards, g_dv_run.roots - g_dv_run.seed_roots,
               g_dv_run.seed_roots, g_dv_run.seed_best, g_dv_run.plain_best);
        if (g_dv_run.seed_pairs)
            printf("; per board, best seeded copy vs unseeded: better %" PRIu64 ", equal %"
                   PRIu64 ", worse %" PRIu64 ", mean %+.2f edges",
                   g_dv_run.seed_wins, g_dv_run.seed_ties, g_dv_run.seed_losses,
                   (double)g_dv_run.seed_diff / (double)g_dv_run.seed_pairs);
        printf("\n");
    }
    if (g_dv_run.inc_boards)
        printf("[sum] incumbents: %" PRIu64 " board(s) re-dived, %" PRIu64 " beaten by a dive "
               "(%+" PRId64 " edges in all), the rest kept as they were\n",
               g_dv_run.inc_boards, g_dv_run.inc_better, g_dv_run.inc_gain);
    printf("[sum] end dive scores written:");
    int shown = 0;
    for (int sc = DV_EDGES; sc >= 0 && shown < 16; sc--)
        if (g_dv_run.hist[sc]) { printf("  %d:%" PRIu64, sc, g_dv_run.hist[sc]); shown++; }
    if (!shown) printf("  none");
    printf("\n");
    fflush(stdout);
}
