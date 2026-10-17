/* SPDX-License-Identifier: GPL-3.0-only */
/* RIFT 裂 — a monophonic bass firmware for the M-VAVE FM-1 (core release).
 * Mr.V / PUZAOSI. Built on Dinghy (zvenson), Felucca (Leo Kuroshita, Hügelton Instruments) and SLOOP (isod89).
 *
 *   ACID engine: 303-style ladder with HP in the resonance loop, CUT/ENV-MOD coupling, accent capacitor,
 *   constant-time slide; post-VCA "爆" BURST macro on the band above 120 Hz; tape delay on the high band.
 *   Generative 8-lane sequencer (DNA), throws (RISE ROLL WOW STOP BUILD SHRED VOID), MIDI out for visuals.
 *
 * Integer only: Q24 signals (1.0 = 1 << 24), Q30 coefficients, Q16 knobs. Tables: rift_tables.h. */
#define APP_NAME "RIFT"
#include "platform.c"
#include "rift_tables.h"
#ifndef RING_PUBLISH
#define RING_PUBLISH() ((void)0)              /* host: no ISR to order against */
#endif

/* ================================================================== fixed point */
#define ONE24 16777216
#define ONE16 65536
static inline int32_t m30(int32_t a, int32_t c) { return (int32_t)(((int64_t)a * c) >> 30); }
static inline int32_t m24(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 24); }
static inline int32_t m16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static inline int32_t clampi(int32_t x, int32_t a, int32_t b) { return x < a ? a : x > b ? b : x; }
static inline int32_t lerp16(int32_t a, int32_t b, int32_t t) { return a + m16(b - a, t); }
static inline int32_t iabs(int32_t x) { return x < 0 ? -x : x; }

#define HOT_INL static inline __attribute__((always_inline))
#define HOT_FN static __attribute__((noinline))   /* tools/build.py HOT_FNS: in RAM */
HOT_INL int32_t tanh24(int32_t x)                /* Q24 in, Q24 out */
{
    uint32_t u, i, f;
    x = clampi(x, -(8 * ONE24) + 1, 8 * ONE24 - 1);
    u = (uint32_t)(x + 8 * ONE24);
    i = u >> 18;
    f = (u >> 2) & 0xFFFFu;
    return TANH_T[i] + (int32_t)(((int64_t)(TANH_T[i + 1] - TANH_T[i]) * f) >> 16);
}
HOT_INL int32_t sin24(uint32_t ph)               /* one cycle over 2^32 */
{
    uint32_t i = ph >> 22, f = (ph >> 6) & 0xFFFFu;
    return SIN_T[i] + (int32_t)(((int64_t)(SIN_T[i + 1] - SIN_T[i]) * f) >> 16);
}
static int32_t exp2_16(int32_t x)               /* 2^x, x Q16 (-15..14) -> Q16 */
{
    int32_t i = x >> 16;
    uint32_t f = (uint32_t)x & 0xFFFFu, k = (f * 768u) >> 16;
    int32_t r = EXP2_T[k] >> 14;
    if (i >= 0) return r << (i > 14 ? 14 : i);
    i = -i;
    return i > 30 ? 0 : r >> i;
}
static uint32_t isqrt64(uint64_t v)               /* integer square root, shifts only */
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (uint32_t)r;
}
HOT_FN int32_t tabi(const int32_t *t, int32_t n, int32_t c16)   /* table at a Q16 index, linear */
{
    int32_t i, f;
    if (c16 < 0) return t[0];
    i = c16 >> 16;
    if (i >= n - 1) return t[n - 1];
    f = c16 & 0xFFFF;
    return t[i] + m16(t[i + 1] - t[i], f);
}
static uint32_t pitch_inc(int32_t n8, int32_t os)   /* MIDI note Q8 -> Q32 phase step (OS or base rate) */
{
    int32_t oct, k, e;
    if (n8 < 0) n8 = 0;
    oct = n8 / 3072;
    k = (n8 - oct * 3072) >> 2;
    e = EXP2_T[k];
    return (uint32_t)(((((int64_t)(os ? INC_C0_OS_Q40 : INC_C0_BR_Q40) * e) >> 30) << oct) >> 8);
}

/* ================================================================== generator (main loop) */
enum { L_RHY, L_ACC, L_SLD, L_PIT, L_OCT, L_MOD, L_CHN, L_TRN, NL };
enum { REST = 0, NOTE = 1, TIE = 2 };
static const char *const LANE_NAME[NL] = {"RHY", "ACC", "SLD", "PIT", "OCT", "MOD", "CHN", "TRN"};
static const uint32_t SALT[NL] = {4097, 8194, 12291, 16388, 20485, 24582, 28679, 32776};
typedef struct { uint8_t len, dir, off, link, lock; int8_t v[32]; } lane_t;
typedef struct {
    uint32_t seed; int32_t gen;
    uint8_t type, dens, slide, acc, motif, root, scale, pad;
    lane_t ln[NL];
    uint8_t rat[32], gate[32];
    struct { uint8_t id[4]; uint16_t v[4][32]; } mo;   /* motion: up to 4 knobs, a value per RHY step (0xFFFF none) */
} genome_t;

static const uint8_t SC_N[10] = {7, 7, 7, 7, 5, 6, 8, 5, 6, 12};
static const int8_t SC_S[10][12] = {
    {0, 2, 3, 5, 7, 8, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10}, {0, 2, 3, 5, 7, 8, 11}, {0, 3, 5, 7, 10},
    {0, 3, 5, 6, 7, 10}, {0, 1, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 7, 8}, {0, 2, 4, 6, 8, 10}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
static const char *const SC_NAME[10] = {"MINOR", "PHRYG", "DORIAN", "HARM", "PENT", "BLUES", "ACID", "HIRA", "WHOLE", "CHROM"};
static const uint8_t CHANCE[8] = {255, 222, 191, 158, 128, 94, 64, 31};
static const uint8_t W_POS[16] = {255, 150, 217, 178, 255, 140, 217, 170, 255, 150, 217, 178, 255, 140, 217, 185};

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
typedef struct { uint32_t s; } rng_t;
static void rng_seed(rng_t *r, uint32_t s) { r->s = s ? s : 0x9e3779b9u; }
static uint32_t rng_next(rng_t *r) { uint32_t x = r->s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; r->s = x; return x; }
static uint32_t r8(rng_t *r) { return rng_next(r) >> 24; }
static uint32_t rint_(rng_t *r, uint32_t n) { return (uint32_t)(((uint64_t)rng_next(r) * n) >> 32); }
static uint32_t pickw(rng_t *r, const uint32_t *w, uint32_t n)
{
    uint32_t t = 0, i, x;
    for (i = 0; i < n; i++) t += w[i];
    x = (uint32_t)(((uint64_t)rng_next(r) * t) >> 32);
    for (i = 0; i < n; i++) { if (x < w[i]) return i; x -= w[i]; }
    return n - 1;
}
static int32_t mix8(int32_t c, int32_t u, int32_t T) { return (c * (255 - T) + u * T) >> 8; }
static void lane_rng(rng_t *r, const genome_t *g, int l) { rng_seed(r, hash32(g->seed ^ SALT[l] ^ ((uint32_t)g->gen * 0x9e3779b9u))); }
static int32_t deg_semi(int scale, int32_t d)
{
    int32_t n = SC_N[scale], o = d >= 0 ? d / n : -((-d + n - 1) / n);
    return SC_S[scale][d - o * n] + 12 * o;
}
static int32_t semi_to_deg(int scale, int32_t s)
{
    int32_t n = SC_N[scale], o = s >= 0 ? s / 12 : -((-s + 11) / 12), pc = s - 12 * o, k = 0, i;
    for (i = 0; i < n; i++) if (SC_S[scale][i] <= pc) k = i;
    return k + n * o;
}

static void gen_rhy(genome_t *g)
{
    lane_t *L = &g->ln[L_RHY];
    rng_t r; int32_t T = g->type, len = L->len, M, i, k, j, reps, tail, pre, ptie;
    uint8_t cell[32];
    lane_rng(&r, g, L_RHY);
    M = g->motif ? g->motif : (T < 128 ? (r8(&r) < 128 ? 4 : 8) : 3 + (int32_t)rint_(&r, 14));
    if (M > len) M = len;
    for (i = 0; i < M; i++) cell[i] = r8(&r) < (uint32_t)clampi((mix8(W_POS[i & 15], 128, T) * g->dens) >> 7, 0, 255) ? NOTE : REST;
    if (T < 192) cell[0] = NOTE;
    for (i = 0; i < len; i++) L->v[i] = (int8_t)cell[i % M];
    reps = (len + M - 1) / M; tail = (M + 3) / 4; pre = mix8(140, 255, T);
    for (k = 1; k < reps; k += 2)
        for (j = M - tail; j < M; j++) {
            i = k * M + j;
            if (i < len && (int32_t)r8(&r) < pre)
                L->v[i] = r8(&r) < (uint32_t)clampi((mix8(W_POS[i & 15], 128, T) * g->dens) >> 7, 0, 255) ? NOTE : REST;
        }
    ptie = mix8(35, 80, T);
    for (i = 1; i < len; i++) if (L->v[i] == NOTE && L->v[i - 1] != REST && (i & 3) && (int32_t)r8(&r) < ptie) L->v[i] = TIE;
    for (i = 0; i < len; i++) { g->rat[i] = (L->v[i] == NOTE && (i & 1) && (int32_t)r8(&r) < mix8(6, 40, T)) ? 2 : 1; g->gate[i] = 4; }
    if (L->v[0] == TIE) L->v[0] = NOTE;
}
static int32_t onsets_of(const genome_t *g, uint8_t *pos)
{
    int32_t i, n = 0;
    for (i = 0; i < g->ln[L_RHY].len; i++) if (g->ln[L_RHY].v[i] == NOTE) pos[n++] = (uint8_t)i;
    return n;
}
static uint32_t pc_weight(int32_t semi)
{
    int32_t pc = semi - 12 * (semi >= 0 ? semi / 12 : -((-semi + 11) / 12));
    if (semi == 12) return 20;
    switch (pc) { case 0: return 110; case 7: return 35; case 10: return 30; case 3: return 25; case 5: return 15; case 1: return 10; default: return 4; }
}
static void gen_pit(genome_t *g)
{
    lane_t *P = &g->ln[L_PIT], *O = &g->ln[L_OCT];
    rng_t r, ro; int32_t T = g->type, n = SC_N[g->scale], len, j, prev = 0, plus = 0, nc = 0, c;
    uint8_t pos[32]; int32_t cands[24]; uint32_t ws[24];
    int32_t no = onsets_of(g, pos);
    lane_rng(&r, g, L_PIT); lane_rng(&ro, g, L_OCT);
    len = T < 128 ? clampi(no, 1, 16) : 3 + (int32_t)rint_(&r, 14);
    if (P->lock) len = P->len;
    P->len = O->len = (uint8_t)len;
    for (c = -((n + 1) / 2); c <= n + 2 && nc < 24; c++) cands[nc++] = c;
    for (j = 0; j < len; j++) {
        int32_t p0 = no ? pos[j % no] : j, onb = (p0 & 3) == 0, d, o, k;
        uint32_t wo[3] = {15, 170, 70};
        if (j > 0 && (int32_t)r8(&r) < mix8(70, 15, T)) d = prev;
        else {
            for (k = 0; k < nc; k++) {
                int32_t semi = deg_semi(g->scale, cands[k]), pc = semi - 12 * (semi >= 0 ? semi / 12 : -((-semi + 11) / 12));
                uint32_t w = pc_weight(semi);
                if (semi < -5 || semi > 14) w >>= 1;
                if ((onb || j == 0) && semi == 0) w *= 2;
                if (iabs(cands[k] - prev) > 2 && !(pc == 0 || pc == 7)) w >>= 2;
                ws[k] = (uint32_t)mix8((int32_t)w, 30, T) + 1u;
            }
            d = cands[pickw(&r, ws, (uint32_t)nc)];
        }
        if (P->lock) d = P->v[j];
        P->v[j] = (int8_t)d;
        if (!onb && j > 0 && P->v[j - 1] == 0) wo[2] = 140;
        if (plus >= 2) wo[2] = 0;
        o = (int32_t)pickw(&ro, wo, 3) - 1;
        if (T > 200 && r8(&ro) < 40) o = 2;
        if (O->lock) o = O->v[j];
        plus = o > 0 ? plus + 1 : 0;
        O->v[j] = (int8_t)o;
        prev = d;
    }
}
static void gen_acc(genome_t *g)
{
    lane_t *L = &g->ln[L_ACC], *R = &g->ln[L_RHY], *O = &g->ln[L_OCT];
    rng_t r; int32_t i, count = 0, on = 0;
    lane_rng(&r, g, L_ACC);
    L->len = R->len;
    for (i = 0; i < R->len; i++) {
        int32_t w;
        L->v[i] = 0;
        if (R->v[i] != NOTE) continue;
        w = g->acc >> 1;
        if (i > 0 && R->v[i - 1] == REST) w += 80;
        if ((i & 3) == 0) w += 40;
        if (O->v[on % (O->len ? O->len : 1)] > 0) w += 50;
        if (i > 0 && L->v[i - 1]) w -= 90;
        on++;
        if (count < 5 && (int32_t)(r8(&r) * 255u * 10u) < clampi(w, 0, 255) * g->acc * 16) { L->v[i] = 1; count++; }
    }
}
static void gen_sld(genome_t *g)
{
    lane_t *L = &g->ln[L_SLD], *R = &g->ln[L_RHY], *P = &g->ln[L_PIT], *O = &g->ln[L_OCT];
    rng_t r; int32_t i, on = 0, run = 0;
    int32_t pl = P->len ? P->len : 1, ol = O->len ? O->len : 1;
    lane_rng(&r, g, L_SLD);
    L->len = R->len;
    for (i = 0; i < R->len; i++) {
        int32_t isn = R->v[i] == NOTE, nxt = R->v[(i + 1) % R->len], k, w;
        L->v[i] = 0;
        if (R->v[i] == REST) { run = 0; continue; }
        k = isn ? on : on - 1;
        if (isn) on++;
        if (k < 0) k = 0;
        if (nxt != NOTE) { run = 0; continue; }
        if (P->v[k % pl] + 7 * O->v[k % ol] == P->v[on % pl] + 7 * O->v[on % ol]) { run = 0; continue; }
        w = g->slide * 45 / 100;
        if (O->v[k % ol] != O->v[on % ol]) w += 60;
        if ((i & 3) == 3) w += 40;
        if (run < 3 && (int32_t)r8(&r) < clampi(w, 0, 255)) { L->v[i] = 1; run++; } else run = 0;
    }
}
static void gen_mod(genome_t *g)
{
    lane_t *L = &g->ln[L_MOD];
    rng_t r; int32_t i, v = 0;
    lane_rng(&r, g, L_MOD);
    L->len = (uint8_t)(g->type < 128 ? 12 : 5 + (int32_t)rint_(&r, 12));
    for (i = 0; i < L->len; i++) {
        v = clampi(v + (int32_t)rint_(&r, 41) - 20, -60, 60);
        L->v[i] = (int8_t)(g->type < 128 ? ((30 * (sin24((uint32_t)i * (0xFFFFFFFFu / L->len)) >> 8)) >> 16) + (v >> 2) : v);
    }
}
static void gen_chn(genome_t *g)
{
    lane_t *L = &g->ln[L_CHN];
    rng_t r; int32_t i;
    lane_rng(&r, g, L_CHN);
    L->len = 16;
    for (i = 0; i < 16; i++) L->v[i] = (int8_t)(g->type > 160 && (int32_t)r8(&r) < g->type - 160 ? 1 + (int32_t)rint_(&r, 4) : 0);
}
static const int8_t TRN_SETS[6][4] = {{0, 0, 0, 0}, {0, 0, 3, 4}, {0, 0, -2, -1}, {0, 5, 3, 4}, {0, 0, 0, 3}, {0, -1, 0, 2}};
static void gen_trn(genome_t *g)
{
    lane_t *L = &g->ln[L_TRN];
    rng_t r; int32_t s, i;
    lane_rng(&r, g, L_TRN);
    s = g->gen == 0 ? 0 : 1 + (int32_t)rint_(&r, 5);
    L->len = s == 0 ? 1 : 4;
    for (i = 0; i < 4; i++) L->v[i] = TRN_SETS[s][i];
}
static void generate(genome_t *g)
{
    if (!g->ln[L_RHY].lock) gen_rhy(g);
    if (!g->ln[L_PIT].lock || !g->ln[L_OCT].lock) gen_pit(g);
    if (!g->ln[L_ACC].lock) gen_acc(g);
    if (!g->ln[L_SLD].lock) gen_sld(g);
    if (!g->ln[L_MOD].lock) gen_mod(g);
    if (!g->ln[L_CHN].lock) gen_chn(g);
}
static void hard_lane(genome_t *g, int l)
{
    uint8_t was = g->ln[l].lock;
    g->gen++;
    g->ln[l].lock = 0;
    switch (l) {
    case L_TRN: gen_trn(g); break;
    case L_RHY: gen_rhy(g); gen_acc(g); gen_sld(g); break;
    case L_PIT: case L_OCT: gen_pit(g); gen_sld(g); break;
    case L_ACC: gen_acc(g); break;
    case L_SLD: gen_sld(g); break;
    case L_MOD: gen_mod(g); break;
    default: gen_chn(g); break;
    }
    g->ln[l].lock = was;
}
static void soft_mutate(genome_t *g, uint32_t salt)
{
    rng_t r; int32_t t, n = 1;
    static const uint8_t FREE[5] = {L_RHY, L_ACC, L_SLD, L_PIT, L_OCT};
    rng_seed(&r, hash32(g->seed ^ 43981u ^ (salt * 0x9e3779b9u)));
    n += (int32_t)(r8(&r) & 1u);
    for (t = 0; t < n; t++) {
        int l = FREE[rint_(&r, 5)];
        lane_t *L = &g->ln[l];
        int32_t i = (int32_t)rint_(&r, L->len ? L->len : 1);
        if (L->lock) continue;
        if (l == L_RHY) {
            int32_t j = (i + 1) % L->len;
            if (L->v[i] != L->v[j] && i != 0) { int8_t x = L->v[i]; L->v[i] = L->v[j]; L->v[j] = x; }
            if (L->v[0] == TIE) L->v[0] = NOTE;
        } else if (l == L_PIT) L->v[i] = (int8_t)clampi(L->v[i] + (r8(&r) < 128 ? -1 : 1), -7, 14);
        else if (l == L_OCT) L->v[i] = (int8_t)(L->v[i] == 0 ? (r8(&r) < 160 ? 1 : -1) : 0);
        else L->v[i] = (int8_t)!L->v[i];
    }
}
static genome_t drift_tmp;
static int drift_lane(genome_t *g, int l, uint32_t cycle, int32_t lock)
{
    rng_t r; int32_t i, p, ch = 0;
    lane_t *L = &g->ln[l];
    if (lock >= 100 || L->lock || l > L_MOD) return 0;
    drift_tmp = *g;
    drift_tmp.gen = g->gen + 1000 + (int32_t)cycle;
    if (l == L_RHY) gen_rhy(&drift_tmp);
    else if (l == L_PIT || l == L_OCT) gen_pit(&drift_tmp);
    else if (l == L_ACC) gen_acc(&drift_tmp);
    else if (l == L_SLD) gen_sld(&drift_tmp);
    else gen_mod(&drift_tmp);
    rng_seed(&r, hash32(g->seed ^ SALT[l] ^ ((cycle + 7u) * 0x85ebca6bu)));
    p = (100 - lock) * 255 / 100 * 35 / 100;
    for (i = 0; i < L->len; i++)
        if ((int32_t)r8(&r) < p) {
            int8_t nv = drift_tmp.ln[l].v[i % (drift_tmp.ln[l].len ? drift_tmp.ln[l].len : 1)];
            if (nv != L->v[i]) { L->v[i] = nv; ch = 1; }
        }
    if (l == L_RHY && L->v[0] == TIE) L->v[0] = NOTE;
    return ch;
}
static void genome_init(genome_t *g, uint32_t seed, uint8_t type, uint8_t dens, uint8_t slide, uint8_t acc, uint8_t scale)
{
    int i;
    memset(g, 0, sizeof *g);
    g->seed = seed; g->type = type; g->dens = dens; g->slide = slide; g->acc = acc; g->scale = scale;
    for (i = 0; i < NL; i++) g->ln[i].len = 16;
    g->ln[L_ACC].link = g->ln[L_SLD].link = 1;
    g->ln[L_PIT].len = g->ln[L_OCT].len = 8; g->ln[L_MOD].len = 12; g->ln[L_TRN].len = 1;
    for (i = 0; i < 32; i++) { g->ln[L_RHY].v[i] = NOTE; g->rat[i] = 1; g->gate[i] = 4; }
    generate(g);
}
static const struct { const char *name; uint32_t seed; uint8_t type, dens, slide, acc, scale; } FACT[8] = {
    {"PHUTURE", 0x5eed0303u, 30, 165, 110, 120, 0}, {"SQUELCH", 0x7f3ac219u, 60, 150, 140, 150, 1},
    {"BUNKER", 0x00b0c4e2u, 20, 120, 40, 90, 0},    {"BREAK", 0x2a2a1996u, 90, 175, 80, 130, 5},
    {"BLOCK", 0x1997b10cu, 50, 140, 60, 160, 4},    {"SIREN", 0x51e7e7a5u, 110, 185, 170, 140, 6},
    {"DRIFT", 0x0dd5eed5u, 170, 150, 100, 110, 2},  {"CHAOS", 0xc4a05fffu, 230, 190, 120, 130, 1},
};
static uint32_t lane_index(const lane_t *L, uint32_t c, uint32_t seed, uint32_t salt)
{
    uint32_t len = L->len ? L->len : 1u, i;
    switch (L->dir) {
    case 1: i = len - 1u - (c % len); break;
    case 2: { uint32_t per = len > 1u ? 2u * len - 2u : 1u, m = c % per; i = m < len ? m : per - m; break; }
    case 3: case 4: i = hash32(seed ^ salt ^ ((c + 1u) * 0x9e3779b9u)) % len; break;
    default: i = c % len;
    }
    return (i + L->off) % len;
}

/* ================================================================== presets (sound) */
/* the engines. CHR, the VOICE page's third knob, is each one's own: ACID the saw -> square wave, RUMBLE the
 * diffuser loop, REESE the detune, GROWL the FM index, SCREAM the split of its two ladders */
enum { E_ACID, E_RUMBLE, E_REESE, E_GROWL, E_SCREAM, NENG };
static const char *const ENG_NAME[NENG] = {"ACID", "RUMBLE", "REESE", "GROWL", "SCREAM"};
static const char *const CHR_NAME[NENG] = {"WAVE", "RUMBLE", "DETUNE", "FM", "SPLIT"};
typedef struct {
    const char *name; uint8_t eng, dly_time, ratio, rel;   /* rel: amp release, ms */
    uint16_t cut, res, env, dec, acc, burst, chr, dly_send, dly_fb, sub, drive, duck, fold, formant, phaser;
} preset_t;
#define K(x) ((uint16_t)((x) * 65535))
static const preset_t PRESETS[] = {
    {"PHUTURE 88", E_ACID, 2, 2, 8, K(.46), K(.72), K(.55), K(.37), K(.70), K(.15), K(0), K(0), K(.40), 0, 0, 0, 0, 0, 0},
    {"SQUELCH", E_ACID, 2, 2, 8, K(.28), K(.86), K(.80), K(.18), K(1.0), K(.55), K(1), K(.25), K(.45), 0, 0, 0, 0, 0, K(.20)},
    {"RUBBER", E_ACID, 3, 2, 8, K(.36), K(.55), K(.45), K(.62), K(.50), K(.25), K(.85), K(.10), K(.35), K(.25), 0, 0, 0, 0, 0},
    {"DIRT", E_ACID, 2, 2, 8, K(.40), K(.80), K(.65), K(.30), K(.80), K(.85), K(.30), K(.20), K(.40), K(.15), K(.3), 0, 0, 0, 0},
    {"BUNKER", E_RUMBLE, 2, 2, 30, K(.19), K(.20), K(.20), K(.50), K(.40), K(.30), K(.70), 0, K(.40), K(.30), K(.6), K(.75), 0, 0, 0},
    {"PRESSURE", E_RUMBLE, 2, 2, 30, K(.13), K(.25), K(.25), K(.45), K(.50), K(.45), K(.85), 0, K(.40), K(.50), K(.7), K(.90), 0, 0, 0},
    {"BREAK REESE", E_REESE, 2, 2, 80, K(.48), K(.15), K(.30), K(.50), K(.50), K(.60), K(.50), 0, K(.40), K(.50), 0, 0, 0, 0, K(.35)},
    {"FIRESTORM", E_REESE, 2, 2, 80, K(.58), K(.20), K(.45), K(.30), K(.60), K(.85), K(.75), K(.20), K(.30), K(.45), 0, 0, 0, 0, 0},
    {"BLOCK ROCK", E_GROWL, 2, 2, 20, K(.56), K(.40), K(.40), K(.32), K(.60), K(.70), K(.70), 0, K(.40), 0, 0, 0, K(.40), K(.65), 0},
    {"ACID SIREN", E_SCREAM, 4, 2, 8, K(.42), K(.93), K(.70), K(.40), K(1.0), K(.85), K(.50), K(.30), K(.50), 0, 0, 0, 0, 0, 0},
};
#define NPRESET (sizeof PRESETS / sizeof PRESETS[0])

/* ================================================================== shared state */
enum { T_RISE, T_ROLL, T_WOW, T_STOP, T_BUILD, T_SHRED, T_FREEZE, T_VOID, T_DIVE, T_GATE, T_STACK, T_REV, NTH };
static const char *const TH_NAME[NTH] = {"RISE", "ROLL", "WOW", "STOP", "BUILD", "SHRED", "FREEZE", "VOID", "DIVE", "GATE", "STACK", "REV"};
static const uint16_t GATE_PATS[4] = {0x5555u, 0x6D6Du, 0x4925u, 0x7777u};
typedef struct {
    int32_t cut, res, env, dec, acc, burst, chr, glide, sub, drive, dly_send, dly_fb, vol, duck, drum_lvl, mod_depth;
    int32_t eng, ratio, rel, fold, formant, harm;     /* the engine; GROWL's FM ratio; amp release (ms); fold; formant */
    int32_t phaser, ph_rate, squash;                 /* the phaser's mix and rate; OTT's depth */
    int32_t sync;                                    /* 0 INT, 1 AUTO (follow a clock while one comes), 2 EXT */
    int32_t bpm10, swing, dly_time, tension, lock, xfade, pres, vary;
    int32_t drums, kick_tune, root, scale;
    int32_t tp[NTH][4];
} params_t;
static volatile params_t P;
/* the knobs motion can record (genome_t.mo.id = M_x + 1), and what the sound engine reads: the locked value, or P's */
enum { M_CUT, M_RES, M_ENV, M_BURST, M_DEC, M_ACC, M_CHR, M_DRIVE, M_DLY_SEND, M_DLY_FB, M_SUB, M_DUCK, M_PHASER, M_PH_RATE,
       M_SQUASH, M_HARM, NMP };
static volatile int32_t *const MP[NMP] = {&P.cut, &P.res, &P.env, &P.burst, &P.dec, &P.acc, &P.chr, &P.drive, &P.dly_send,
                                          &P.dly_fb, &P.sub, &P.duck, &P.phaser, &P.ph_rate, &P.squash, &P.harm};
static int32_t lk[NMP];                          /* (ISR) this step's locks, -1 none */
#define EP(m) (lk[m] >= 0 ? lk[m] : *MP[m])
static int mo_lane_of(const genome_t *g, int m) { int l; for (l = 0; l < 4; l++) if (g->mo.id[l] == m + 1) return l; return -1; }
static int mp_of(volatile int32_t *p) { int m; for (m = 0; m < NMP; m++) if (MP[m] == p) return m; return -1; }


/* main -> ISR commands (single producer, single consumer) */
enum { OP_ON = 1, OP_OFF, OP_PLAY, OP_ENGAGE, OP_RELEASE, OP_ABORT, OP_KEEP, OP_PANIC, OP_FILL, OP_DROP, OP_GENOME, OP_GENOME_B, OP_KEYDEG };
typedef struct { uint8_t op, a; int16_t b; } cmd_t;
static cmd_t cq[64];
static volatile uint32_t cq_w, cq_r;
static void cmd(uint32_t op, uint32_t a, int32_t b)
{
    if (cq_w - cq_r >= 64u) return;
    cq[cq_w & 63u].op = (uint8_t)op; cq[cq_w & 63u].a = (uint8_t)a; cq[cq_w & 63u].b = (int16_t)b;
    RING_PUBLISH();
    cq_w++;
}
/* ISR -> main events */
enum { EV_NOTE = 1, EV_OFF, EV_STEP, EV_BEAT, EV_ENGAGE, EV_LAND, EV_ROLL, EV_DROP, EV_WRAP, EV_PANIC };
typedef struct { uint8_t ev, a, b, c; } evt_t;
static evt_t eq[128];
static volatile uint32_t eq_w, eq_r;
static void emit(uint32_t ev, uint32_t a, uint32_t b, uint32_t c)
{
    if (eq_w - eq_r >= 128u) return;
    eq[eq_w & 127u].ev = (uint8_t)ev; eq[eq_w & 127u].a = (uint8_t)a; eq[eq_w & 127u].b = (uint8_t)b; eq[eq_w & 127u].c = (uint8_t)c;
    RING_PUBLISH();
    eq_w++;
}

#define NG 6
static genome_t gbuf[NG];
static volatile int32_t g_a = 0, g_b = -1, g_pend = -1;
static volatile uint8_t dsp_ready;

/* what the screen and MIDI read from the ISR (single 32-bit values) */
static volatile int32_t s_dive, s_gate, s_stack, s_ext, s_bpm_ext;   /* B throws for the screen; following an external clock */
static volatile int32_t s_fcidx, s_env, s_acccap, s_burst, s_frac, s_prog, s_rate, s_amp, s_dfb, s_rms, s_wow;
static volatile uint32_t s_abs, s_onsets, s_bar, s_gstep, s_clk24, s_playing, s_cpu, s_c1;
static volatile uint8_t s_th[NTH];
static int16_t scope[512];
static volatile uint32_t scope_w;

/* ================================================================== DSP state (ISR only) */
typedef struct { int32_t s, G; } op1_t;     /* TPT one-pole */
static inline int32_t op_lp(op1_t *f, int32_t x) { int32_t v = m30(x - f->s, f->G), y = v + f->s; f->s = y + v; return y; }
static inline int32_t op_hp(op1_t *f, int32_t x) { return x - op_lp(f, x); }
typedef struct { int32_t i1, i2, a1, a2, a3; } svf_t;   /* Cytomic SVF, Butterworth k */
static void svf_set(svf_t *f, int32_t c) { c = clampi(c, 0, 704); f->a1 = SVF_A1_T[c]; f->a2 = SVF_A2_T[c]; f->a3 = SVF_A3_T[c]; }
static void svfb_set(svf_t *f, int32_t c) { c = clampi(c, 0, 704); f->a1 = SVFB_A1_T[c]; f->a2 = SVFB_A2_T[c]; f->a3 = SVFB_A3_T[c]; }   /* base rate */
static inline void svf_run(svf_t *f, int32_t x, int32_t *lp, int32_t *bp)
{
    int32_t v3 = x - f->i2, v1 = m30(f->i1, f->a1) + m30(v3, f->a2), v2 = f->i2 + m30(f->i1, f->a2) + m30(v3, f->a3);
    f->i1 = 2 * v1 - f->i1; f->i2 = 2 * v2 - f->i2;
    *lp = v2; *bp = v1;
}
static inline int32_t svf_lp(svf_t *f, int32_t x) { int32_t lp, bp; svf_run(f, x, &lp, &bp); return lp; }
static inline int32_t svf_hp(svf_t *f, int32_t x) { int32_t lp, bp; svf_run(f, x, &lp, &bp); return x - m30(bp, K_BW_Q30) - lp; }

typedef struct { int32_t b[HB_N * 2]; uint32_t p; } hb_t;
/* the ladder (ZDF 4-pole, the HP in the resonance loop, saturation at the loop input, tilted to ~18 dB) */
typedef struct { int32_t G, G2, G3, G4, om, recip, comp16, k24, hG, a_hp; } ladc_t;
typedef struct { int32_t s0, s1, s2, s3, hs; } lads_t;
static void lad_coef(ladc_t *c, int32_t c16, int32_t k24, int32_t hG)
{
    int32_t den16;
    c->G = tabi(G_OS_T, 705, c16);
    c->G2 = m30(c->G, c->G); c->G3 = m30(c->G2, c->G); c->G4 = m30(c->G2, c->G2); c->om = (1 << 30) - c->G;
    c->hG = hG; c->a_hp = (1 << 30) - hG; c->k24 = k24;
    c->comp16 = LCOMP_T[clampi(k24 >> 18, 0, 320)];
    den16 = ONE16 + (int32_t)(((int64_t)m30(c->a_hp, c->G4) * k24) >> 38);   /* 1 / (1 + k a G^4) */
    c->recip = (int32_t)((0x40000000u / (uint32_t)den16) << 16);
}
HOT_INL int32_t lad_run(lads_t *L, const ladc_t *c, int32_t x)
{
    int32_t S = m30(m30(L->s0, c->G3) + m30(L->s1, c->G2) + m30(L->s2, c->G) + L->s3, c->om);
    int32_t fb = m30(S, c->a_hp) - m30(L->hs, c->a_hp), u, v, y1, y2, y3, y4;
    u = m30(x - m24(fb, c->k24), c->recip);
    u = m16(tanh24(m16(u, 98304)), 43691);
    v = m30(u - L->s0, c->G); y1 = v + L->s0; L->s0 = y1 + v;
    v = m30(y1 - L->s1, c->G); y2 = v + L->s1; L->s1 = y2 + v;
    v = m30(y2 - L->s2, c->G); y3 = v + L->s2; L->s2 = y3 + v;
    v = m30(y3 - L->s3, c->G); y4 = v + L->s3; L->s3 = y4 + v;
    v = m30(y4 - L->hs, c->hG); L->hs = v + L->hs + v;
    return m16(y4 + m16(y3 - y4, 9830), c->comp16);
}
/* polyBLEP: the correction at a wrap of phase ph (step dt, invdt = 2^31 / (dt >> 12)) */
HOT_INL int32_t blep(uint32_t ph, uint32_t dt, uint32_t invdt)
{
    int32_t u;
    if (ph < dt) { u = (int32_t)(((uint64_t)(ph >> 12) * invdt) >> 7); return m24(ONE24 - u, ONE24 - u); }
    if ((uint32_t)-ph < dt) { u = (int32_t)(((uint64_t)((uint32_t)-ph >> 12) * invdt) >> 7); return -m24(ONE24 - u, ONE24 - u); }
    return 0;
}
HOT_INL int32_t saw24(uint32_t ph, uint32_t dt, uint32_t invdt) { return (int32_t)(ph >> 7) - ONE24 + blep(ph, dt, invdt); }
static uint32_t inv_of(uint32_t dt) { return (uint32_t)(0x80000000u / ((dt >> 12) ? (dt >> 12) : 1u)); }
static inline void hb_push(hb_t *h, int32_t x) { h->b[h->p] = x; h->b[h->p + HB_N] = x; h->p = h->p + 1u == HB_N ? 0u : h->p + 1u; }
HOT_INL int32_t hb_out(const hb_t *h)
{
    const int32_t *b = h->b + h->p;
    int64_t a = (int64_t)HB_T[23] * b[23];
    uint32_t k;
    for (k = 1; k < 24u; k += 2u) a += (int64_t)HB_T[23 - k] * ((int64_t)b[23 - k] + b[23 + k]);
    return (int32_t)(a >> 30);
}

static struct {
    /* voice */
    int32_t note8, tgt8, envF, amp, accEnv, accCap, wow, vca;
    uint8_t gate, att, ampAtt, acc;
    uint32_t ph, phS, inc, incS, invdt;
    lads_t lad, lad2; ladc_t lc, lc2;                /* SCREAM: the second ladder */
    int32_t dive8, kDive, gate_env, stack_sm, stack_t; uint32_t phA, phB, incA, incB, invA, invB;   /* DIVE, GATE, STACK */
    op1_t stHP1, stHP2;
    uint32_t ph2, ph3, phM, inc2, inc3, incM, inv2, inv3;   /* REESE / SCREAM's other saws, GROWL's modulator */
    int32_t drift, idx, kIdx, fmI24, eng;           /* REESE's drift; GROWL's index envelope and depth */
    op1_t rHP1, rHP2;                               /* REESE: the side saws' 120 Hz HP */
    svf_t gsvf, bp700;                              /* GROWL's resonant SVF; RUMBLE's small speaker */
    int32_t cA[256], cB[256], clpA, clpB, cda8, cdb8; uint32_t cp, formPh;   /* GROWL's formant combs */
    op1_t preHP, sqHP, sqLP, hump, harmHP, harmLP;
    svf_t xl1, xh2, cab, riseHP;          /* (cab: the coefficients only; its state is stage 2's) */
    int32_t l1sum;                      /* the low band's first LP stage, summed over the two OS samples */
    int32_t kF, kRel, kGlide, kCapC, kCapD, kKick, kDuck;
    int32_t preG, bias, tbias, fold16, gain16, wet16, chr16, hump16, low16, sub16, comp16b, post16, foldComp16;
    int32_t rmsIn;
    /* transport + sequencer */
    int32_t step_len16;                 /* samples per 1/16, Q16 */
    uint32_t inv_sl, frac16;            /* 2^32 / (step_len16 >> 8); position inside the step, Q16 */
    int32_t roll_acc16, roll_per16;
    int32_t gpos16, spos16;             /* position inside the global / sequencer step, Q16 samples */
    uint32_t gstep, abs, onsets, bar, clk_acc;
    uint8_t playing, trig_done, prev_slide, fill, drop_arm, drop_state;
    int32_t key_deg, key_pend, mod_oct16, vary_c16;
    struct { int32_t at16; uint8_t ty, note, acc; } ev[10];
    uint32_t nev;
    uint8_t live[8]; uint32_t nlive;
    /* throws */
    struct { uint8_t st, latched, rel, auto_land; uint32_t s0, send, land_at; } th[NTH];
    int32_t md_cut16, md_res16, md_fbov, md_send16, md_sub16, md_pitch8, md_drive16, md_burst16, md_noise, md_force_acc, md_hpf;
    int32_t roll_steps16, roll_idx, roll_gate_on, roll_gate_env, roll_cut16, roll_hits, shred_div, shred_bits;
    int32_t void_tgt, void_lp, vlp, drop_burst16, duck_extra;
    /* fx */
    int32_t dtime16, dlyfb;
    int32_t tape_mode, tape_rate16, spin_from;
    int32_t duckEnv;
    uint32_t noise;
    int32_t swing16;                        /* the odd steps' swing (this block) */
    uint32_t blk, cur_i;                    /* blocks made by stage 1; the sample being made */
    int32_t frz_ev, frz_len;                /* FREEZE: an event for stage 2 */
    int32_t ott16, apL, apR; uint32_t phlfo;  /* OTT's depth; the phaser's coefficients and LFO */
    uint32_t ctr;
} D;

#define DLY_N 30870u                    /* 0.7 s per channel */
#define TAPE_N 26460u                   /* 1.2 s mono at half rate (the tape stop) */
#define FRZ_N 24000u                    /* 1.09 s at half rate: FREEZE's loop of the high band */
#define RMB_N (573u + 838u + 1279u + 1808u)   /* RUMBLE's diffuser: 13, 19, 29, 41 ms */
static int16_t dlyL[DLY_N] __attribute__((section(".pool")));
static int16_t dlyR[DLY_N] __attribute__((section(".pool")));
static int16_t tape[TAPE_N] __attribute__((section(".pool")));
static int16_t frz[FRZ_N] __attribute__((section(".pool")));
static int16_t rmb[RMB_N] __attribute__((section(".lowbuf")));
static const uint16_t RMB_AT[5] = {0, 573, 573 + 838, 573 + 838 + 1279, RMB_N};
/* ---------------------------------------------------------------- the block, in two stages
 * Stage 1 (core 0): commands, the sequencer, the voice at 2x, BURST -> the high band at 2x and the low band's first
 *   stage into BK[b & 1], with what stage 2 needs of the control values (every 16 samples) and the drum hits.
 * Stage 2 (core 1, hal/fm1_multicore.h): the decimator, the cabinet, crush, the low band, the tape delay, the
 *   reference drums, the sum, the tape stop, the master -> the output. With two cores it renders block b - 1 while
 *   core 0 makes block b (one block, 5.8 ms, later); with one, block b right after stage 1. */
#define FO (2u * FS)                            /* the oversampled core's rate */
#define NB 256u                                 /* frames a block (platform.c HALF_FRAMES) */
#define NC (NB / 16u)                           /* control ticks a block */
typedef struct {
    int32_t low16, msub16, burst, crdiv, crbits, void_tgt, send16, dt8, fb, vlp, tr, rmsIn, a1, a2, a3;
    int32_t rmb, harm, frz_len, frz_mix, frz_dry, frz_fade;   /* RUMBLE's loop; the low band's harmonics; FREEZE */
    int32_t ott16, ph16, apL, apR;                  /* OTT's depth; the phaser's mix and all-pass coefficients (Q30) */
    uint8_t tm, frz_ev, pad[2];                     /* frz_ev: 1 capture frz_len samples, 2 fade out */
} seg_t;
typedef struct {
    int32_t hos[2u * NB];                       /* the high band after BURST, at the OS rate (the decimator's input) */
    int32_t l1[NB], sub[NB], rg[NB], bg[NB];    /* low band 1st stage, sub, roll gate, duck gain */
    seg_t sg[NC];
    uint32_t ntrig; uint16_t at[8]; uint32_t a[8];   /* the drum hits: sample, step */
    int32_t drum_lvl; uint8_t drums, playing, kick_tune, root;
} blk_t;
static blk_t BK[2];
static struct {                                 /* stage 2's state (core 1's while it runs) */
    hb_t decH; svf_t xl2, cab, hhp;
    int32_t corr16, rmsOut, crush_acc, crush_hold, crush_lp, void_mute;
    uint32_t dw, wowph, noise, kPh, kInc, tw, tr16, us;
    int32_t dlpL, dlpR; op1_t dly[4], hHP;
    int32_t kEnv, kP, click, hEnv, kF8;
    int32_t dcx, dcy, dcxr, dcyr, tape_lp, tape_xf, rms, tacc; uint8_t tm_prev;
    svf_t hHP1; op1_t hLP, rLP; int32_t rfb, kout; uint32_t rp[4];   /* harmonics; the rumble loop */
    uint32_t fw, fstart, flen, fpos; int32_t facc, fgain, fon, ffade, fkf;   /* FREEZE (half-rate positions) */
    int32_t ott_env, ott_g, ott_g0, ap[2][6], apfb[2];   /* OTT; the phaser */
} S2;
static int32_t *s2_out;

static const uint8_t DLY_STEPS[5] = {1, 2, 3, 4, 6};
static const char *const DLY_NAMES[5] = {"1/16", "1/8", "1/8D", "1/4", "1/4D"};

static genome_t *GA(void) { return &gbuf[g_a]; }

/* ---------------------------------------------------------------- voice */
static void voice_on(int32_t note, int acc, int slide)
{
    D.tgt8 = note << 8;
    if (!slide) {
        D.note8 = D.tgt8; D.att = 1; D.ampAtt = 1; D.idx = ONE24;
        if (D.th[T_DIVE].st >= 2) D.dive8 = m16(m16(12 << 8, ONE16 / 2 + P.tp[T_DIVE][1]), ONE16 + P.pres);   /* semitones above, diving */
        if (D.th[T_WOW].st == 2 && acc) D.wow = clampi(D.wow + (ONE16 * 15 / 100), 0, ONE16 * 6 / 10);
    }
    D.acc = (uint8_t)acc;
    if (acc && !slide) D.accEnv = ONE24;
    D.gate = 1;
}
static void gate_off(void) { D.gate = 0; }

/* ---------------------------------------------------------------- throws */
static void th_land(int t, int now)
{
    if (!D.th[t].st) return;
    D.th[t].st = 0; D.th[t].rel = 0; D.th[t].latched = 0;
    if (t == T_STOP || t == T_REV) { D.tape_mode = 0; D.tape_rate16 = ONE16; }
    if (t == T_FREEZE) D.frz_ev = 2;
    if (t == T_VOID && !now) { D.drop_burst16 = ONE16 * 4 / 10; D.duck_extra = ONE16; emit(EV_DROP, 0, 0, 0); }
    emit(EV_LAND, (uint32_t)t, 0, 0);
}
static void th_engage(int t, int latched)
{
    {   /* STOP, REV and VOID share the tape */
        int busy_stop = D.th[T_STOP].st != 0, busy_rev = D.th[T_REV].st != 0, busy_void = D.th[T_VOID].st != 0;
        if ((t == T_STOP && (busy_void || busy_rev)) || (t == T_REV && (busy_stop || busy_void)) || (t == T_VOID && (busy_stop || busy_rev))) return;
    }
    D.th[t].latched = (uint8_t)latched; D.th[t].rel = 0;
    if (!D.th[t].st) D.th[t].st = 1; else if (D.th[t].st == 3) D.th[t].st = 2;
}
HOT_FN void step_boundary(void)
{
    uint32_t s = D.gstep; int t;
    for (t = 0; t < NTH; t++) {
        if (D.th[t].st == 1) {
            uint32_t PL = (t == T_RISE || t == T_BUILD) && P.tp[t][0] > ONE16 / 2 ? 128u : 64u;
            D.th[t].st = 2; D.th[t].s0 = s;
            D.th[t].send = (s + PL) / PL * PL;
            if (D.th[t].send - s < 16u) D.th[t].send += PL;
            if (t == T_STOP) { D.tape_mode = 1; D.tape_rate16 = ONE16; }
            if (t == T_REV) { D.tape_mode = 2; D.tape_rate16 = ONE16; }
            if (t == T_FREEZE) {   /* the last 2 / 4 / 8 steps of the high band, looped (shorter if they do not fit) */
                int32_t n = P.tp[T_FREEZE][0] < ONE16 / 3 ? 2 : P.tp[T_FREEZE][0] < ONE16 * 2 / 3 ? 4 : 8;
                while (n > 1 && (int32_t)(((int64_t)D.step_len16 * n) >> 16) > (int32_t)(2u * FRZ_N - 512u)) n /= 2;
                D.frz_ev = 1; D.frz_len = (int32_t)(((int64_t)D.step_len16 * n) >> 16);
            }
            emit(EV_ENGAGE, (uint32_t)t, 0, 0);
        }
        if (D.th[t].st == 2 && (D.th[t].rel || ((t == T_RISE || t == T_BUILD) && D.th[t].latched && s >= D.th[t].send))) {
            uint32_t q = (t == T_RISE || t == T_BUILD || t == T_VOID) ? 16u : (t == T_STOP || t == T_FREEZE || t == T_REV) ? 4u : 1u;
            D.th[t].st = 3; D.th[t].rel = 0;
            D.th[t].land_at = (s + q - 1u) / q * q;
            if (t == T_STOP) { D.th[t].land_at = (s + 2u + 3u) / 4u * 4u; D.spin_from = (int32_t)D.th[t].land_at - 2; }
        }
        if (D.th[t].st == 3 && s >= D.th[t].land_at) th_land(t, 0);
    }
}

/* ---------------------------------------------------------------- sequencer */
static int32_t lane_val(int l, uint32_t c, uint32_t *idx)
{
    genome_t *A = GA(), *G = A;
    uint32_t i;
    if (g_b >= 0 && P.xfade > 0) {
        uint32_t i0 = lane_index(&A->ln[l], c, A->seed, SALT[l]);
        if ((int32_t)(hash32((SALT[l] << 8) ^ i0) & 255u) < P.xfade) G = &gbuf[g_b];
    }
    i = lane_index(&G->ln[l], c, G->seed, SALT[l]);
    if (idx) *idx = i;
    return G->ln[l].v[i];
}
static int32_t lane_pure(int l, uint32_t c)
{
    const lane_t *L = &GA()->ln[l];
    uint32_t len = L->len ? L->len : 1u;
    return L->v[(c % len + L->off) % len];
}
static int chance_ok(uint32_t a)
{
    int32_t v = clampi(lane_pure(L_CHN, a), 0, 7);
    return (hash32(GA()->seed ^ 28679u ^ ((a + 3u) * 0x27d4eb2du)) >> 24) < CHANCE[v];
}
static int32_t rhy_at(uint32_t a, uint32_t *idx)
{
    const lane_t *L = &GA()->ln[L_RHY];
    uint32_t i = lane_index(L, a, GA()->seed, SALT[L_RHY]);
    int32_t v = L->v[i];
    if (D.fill && v == REST) v = NOTE;
    if (D.drop_state == 1) v = (a & 15u) == 0 ? NOTE : REST;
    if (idx) *idx = i;
    return v;
}
static void wrap_check(int l, uint32_t c)
{
    uint32_t len = GA()->ln[l].len ? GA()->ln[l].len : 1u;
    if (c > 0 && c % len == 0 && P.lock < 100) emit(EV_WRAP, (uint32_t)l, (c / len) & 255u, 0);
}
static void drum_hit(uint32_t a)                /* stage 1's part of a step: the duck, the beat event, the hit for stage 2 */
{
    uint32_t inb = a & 15u;
    blk_t *B = &BK[D.blk & 1u];
    if ((inb & 3u) == 0) { D.duckEnv = ONE24; emit(EV_BEAT, inb == 0, 0, 0); }
    if (B->ntrig < 8u) { B->at[B->ntrig] = (uint16_t)D.cur_i; B->a[B->ntrig++] = a; }
}
static void drum_trig(const blk_t *B, uint32_t a)   /* stage 2's: the hit itself */
{
    uint32_t inb = a & 15u;
    if ((inb & 3u) == 0) {
        int32_t f8 = ((B->root + 24) << 8);
        S2.kEnv = ONE24; S2.kP = ONE24; S2.kPh = 0; S2.click = ONE24;
        while (f8 > (31 << 8) + 128) f8 -= 12 << 8;   /* 41..82 Hz: MIDI 28..39 */
        while (f8 < (28 << 8)) f8 += 12 << 8;
        S2.kF8 = B->kick_tune ? f8 : (31 << 8);
        S2.kInc = pitch_inc(S2.kF8, 0);
    }
    if ((inb & 3u) == 2) S2.hEnv = ONE24; else if (S2.hEnv < ONE24 / 3) S2.hEnv = ONE24 * 35 / 100;
}
static uint32_t hr(uint32_t salt, uint32_t a) { return hash32(GA()->seed ^ salt ^ ((a + 11u) * 0x165667b1u)) >> 24; }
HOT_FN void seq_step(void)
{
    genome_t *g = GA();
    uint32_t a = D.abs, inb = a & 15u, ri, nri;
    int32_t rv, sl16 = D.step_len16, ten = P.tension;
    if (inb == 0 && a > 0) {
        D.bar++;
        if (D.drop_state == 1) D.drop_state = 0;
        if (D.drop_arm) { D.drop_state = 1; D.drop_arm = 0; }
        if (g_pend >= 0) { g_a = g_pend; g_pend = -1; g = GA(); }
    }
    drum_hit(a);
    wrap_check(L_RHY, a);
    rv = rhy_at(a, &ri);
    {   /* motion: this step's recorded knob values (p-locks); a step without one plays the knob */
        int l;
        for (l = 0; l < NMP; l++) lk[l] = -1;
        for (l = 0; l < 4; l++)
            if (g->mo.id[l] && g->mo.id[l] <= NMP && g->mo.v[l][ri & 31u] != 0xFFFFu) lk[g->mo.id[l] - 1] = g->mo.v[l][ri & 31u];
    }
    D.mod_oct16 = m16(lane_val(L_MOD, a, 0) * (ONE16 / 64) * 5 / 2, P.mod_depth);
    emit(EV_STEP, ri, inb, 0);
    D.nev = 0;
    if (rv == NOTE && chance_ok(a)) {
        int32_t deg, oct, note, acc, sld, slide_in, slide_out, hold, rat, nx, k;
        uint32_t sub16;
        D.onsets++;
        if (D.key_pend != 99) { D.key_deg = D.key_pend; D.key_pend = 99; }
        wrap_check(L_PIT, D.onsets);
        deg = lane_val(L_PIT, D.onsets, 0); oct = lane_val(L_OCT, D.onsets, 0);
        if (D.fill) oct = (int32_t)(a & 1u);
        if (ten > 0 && (int32_t)hr(81, a) * ONE16 < ten * 77 && oct < 1) oct++;
        deg += lane_pure(L_TRN, D.bar) + D.key_deg;
        if (D.drop_state == 1) { deg = 0; oct = 0; }
        note = 36 + P.root + deg_semi(g->scale, deg) + 12 * oct;
        acc = g->ln[L_ACC].link ? g->ln[L_ACC].v[ri] : lane_val(L_ACC, a, 0);
        if (D.fill || D.md_force_acc || (ten > 0 && (int32_t)hr(82, a) * ONE16 < ten * 102)) acc = 1;
        slide_in = D.prev_slide && D.gate;
        if (!slide_in && P.vary > 0) {
            uint32_t h = hash32(g->seed ^ ((a + 1u) * 0x9e3779b1u));
            D.vary_c16 = m16(((int32_t)(h >> 16) - 32768) * 2 * 7 / 10 * 64, P.vary);   /* +-0.7 oct */
            if ((int32_t)hr(84, a) * ONE16 < P.vary * 38) acc = !acc;
        }
        sld = g->ln[L_SLD].link ? g->ln[L_SLD].v[ri] : lane_val(L_SLD, a, 0);
        if (ten > 0 && (int32_t)hr(83, a) * ONE16 < ten * 89) sld = 1;
        nx = rhy_at(a + 1u, &nri);
        slide_out = sld && nx == NOTE && chance_ok(a + 1u);
        hold = nx == TIE;
        voice_on(note, acc, slide_in);
        emit(EV_NOTE, (uint32_t)note, (uint32_t)acc, (uint32_t)slide_in);
        rat = slide_in ? 1 : g->rat[ri];
        if (rat < 1) rat = 1;
        sub16 = (uint32_t)(sl16 / rat);
        for (k = 1; k < rat && D.nev < 8u; k++) {
            D.ev[D.nev].at16 = (int32_t)(k * sub16 - sub16 * 45 / 100); D.ev[D.nev++].ty = 0;
            D.ev[D.nev].at16 = (int32_t)(k * sub16); D.ev[D.nev].ty = 1; D.ev[D.nev].note = (uint8_t)note; D.ev[D.nev++].acc = 0;
        }
        if (!slide_out && !hold) { D.ev[D.nev].at16 = (int32_t)((rat - 1) * sub16 + sub16 / 8u * g->gate[ri]); D.ev[D.nev++].ty = 0; }
        D.prev_slide = (uint8_t)slide_out;
    } else if (rv == TIE && D.gate) {
        int32_t sld = g->ln[L_SLD].link ? g->ln[L_SLD].v[ri] : 0, nx = rhy_at(a + 1u, &nri);
        int32_t slide_out = sld && nx == NOTE && chance_ok(a + 1u);
        if (!slide_out && nx != TIE) { D.ev[0].at16 = sl16 / 8 * g->gate[ri]; D.ev[0].ty = 0; D.nev = 1; }
        D.prev_slide = (uint8_t)slide_out;
    } else {
        if (D.gate && !D.nlive) { gate_off(); emit(EV_OFF, 0, 0, 0); }
        D.prev_slide = 0;
    }
}

/* ---------------------------------------------------------------- control rate (every 8 samples) */
HOT_FN void control(void)
{
    int32_t sl16 = D.step_len16, gpos16, q16, bx, c16, cut, EM, res, k24, eng, acid, kt;
    int t;
    /* throws -> modulation bus */
    D.md_cut16 = D.md_res16 = D.md_send16 = D.md_pitch8 = D.md_drive16 = D.md_noise = D.md_force_acc = D.md_hpf = 0;
    D.md_fbov = -1; D.md_sub16 = ONE16; D.roll_steps16 = 0;
    D.frac16 = (uint32_t)(((uint64_t)(uint32_t)(D.gpos16 >> 8) * D.inv_sl) >> 16);
    if (D.frac16 > 65535u) D.frac16 = 65535u;
    (void)gpos16; (void)sl16;
    for (t = 0; t < NTH; t++) s_th[t] = D.th[t].st;
#define REL16(s_) ((int32_t)((D.gstep - (s_)) << 16) + (int32_t)D.frac16)      /* steps since s_, Q16 (small) */
#define PROG(T_) clampi(REL16(D.th[T_].s0) / (int32_t)(D.th[T_].send - D.th[T_].s0 ? D.th[T_].send - D.th[T_].s0 : 1), 0, ONE16)
    if (D.th[T_RISE].st >= 2 && !D.th[T_BUILD].st) {
        int32_t q = PROG(T_RISE), q3 = m16(m16(q, q), q), depth = ONE16 + 5 * P.tp[T_RISE][1];
        D.md_cut16 += m16(depth, q3) * 64;
        D.md_res16 += m16(m16(ONE16 * 3 / 10, q), ONE16 * 4 / 10 + P.tp[T_RISE][2]);
        D.md_hpf = 120 + ((480 * m16(q, q)) >> 16);
        D.md_fbov = lerp16(ONE16 * 35 / 100, ONE16 * 80 / 100 + P.tp[T_RISE][3] / 10, q);
        D.md_send16 = m16(ONE16 * 55 / 100, q); D.md_noise = m16(ONE16 * 12 / 100, m16(q, q));
        if (-REL16(D.th[T_RISE].send) < 4 * ONE16) D.md_sub16 = clampi(-REL16(D.th[T_RISE].send) / 4, 0, ONE16);
        s_prog = q;
    }
    if (D.th[T_BUILD].st >= 2) {
        int32_t q = PROG(T_BUILD), q3 = m16(m16(q, q), q);
        D.md_cut16 += m16(ONE16 * 7 / 2, q3) * 64; D.md_hpf = 120 + ((380 * m16(q, q)) >> 16);
        D.md_fbov = lerp16(ONE16 * 35 / 100, ONE16 * 84 / 100, q); D.md_send16 = m16(ONE16 * 4 / 10, q);
        D.md_pitch8 = m16(q, 12 << 8);
        D.roll_steps16 = q < ONE16 * 4 / 10 ? 2 * ONE16 : q < ONE16 * 7 / 10 ? ONE16 : q < ONE16 * 9 / 10 ? ONE16 / 2 : ONE16 / 4;
        if (q >= ONE16 * 9 / 10) D.md_sub16 = 0;
        s_prog = q;
    }
    if (D.th[T_ROLL].st >= 2 && !D.th[T_BUILD].st && !D.th[T_SHRED].st) {
        int32_t beats = ((int32_t)D.gstep - (int32_t)D.th[T_ROLL].s0) / 4;
        D.roll_steps16 = beats <= 0 ? ONE16 : beats == 1 ? ONE16 * 2 / 3 : ONE16 / 2;
        if (P.tp[T_ROLL][0] > ONE16 * 7 / 10) D.roll_steps16 /= 2;
    }
    if (D.th[T_SHRED].st >= 2) D.roll_steps16 = ONE16 / 2;
    if (D.th[T_WOW].st >= 2) { D.md_force_acc = 1; D.md_drive16 = 6 * (ONE16 / 2 + P.tp[T_WOW][1]); }
    if (D.th[T_VOID].st >= 2) {
        D.void_tgt = 0; D.md_fbov = m16(ONE16 * 95 / 100, ONE16 * 85 / 100 + m16(P.tp[T_VOID][3], ONE16 * 18 / 100));
        D.void_lp = 541 - clampi(REL16(D.th[T_VOID].s0) / 32, 0, ONE16) * 211 / ONE16;   /* 8 k -> 800 Hz (cutoff index) */
    } else { D.void_tgt = ONE16; D.void_lp = 640; }
    if (!D.th[T_RISE].st && !D.th[T_BUILD].st) s_prog = 0;
    D.drop_burst16 = m30(D.drop_burst16, 1073000000);
    D.md_burst16 = D.drop_burst16 + m16(P.pres, ONE16 * 35 / 100);
    D.md_cut16 += m16(P.pres, ONE16 * 22 / 10) * 64;
    /* burst tables */
    bx = clampi(EP(M_BURST) + D.md_burst16, 0, ONE16);
    s_burst = bx;
    q16 = bx * 64;                                           /* table index Q16 over 0..64 */
    D.preG = tabi(P.eng == E_ACID ? BT_PREGA : BT_PREG, 65, q16) + m16(EP(M_DRIVE), 10 * ONE16) / 4 + D.md_drive16 / 4;
    if (P.eng == E_GROWL) D.preG = m16(D.preG, ONE16 * 28 / 10); else if (P.eng == E_RUMBLE) D.preG = m16(D.preG, ONE16 * 5 / 2);
    D.bias = tabi(BT_BIAS, 65, q16) + (P.eng == E_GROWL ? ONE24 * 8 / 100 : 0); D.tbias = tanh24(D.bias);
    D.fold16 = clampi(m16(tabi(BT_FOLD, 65, q16), P.eng == E_ACID ? ONE16 * 6 / 10 : ONE16) + P.fold, 0, ONE16 * 3 / 2);
    D.gain16 = tabi(BT_GAIN, 65, q16); D.wet16 = tabi(BT_WET, 65, q16); D.chr16 = tabi(BT_CHR, 65, q16);
    D.hump16 = tabi(BT_HUMP, 65, q16); D.low16 = tabi(BT_LOW, 65, q16); D.sub16 = tabi(BT_SUB, 65, q16);
    D.comp16b = tabi(BT_COMP, 65, q16); D.post16 = tabi(BT_POST, 65, q16);
    D.ott16 = m16(tabi(BT_OTT, 65, q16), ONE16 * 4 / 10 + EP(M_SQUASH));
    if (EP(M_PHASER) > 0) {   /* the phaser: 220 Hz .. 2.5 kHz, L and R a quarter turn apart; twice as fast under FREEZE */
        uint32_t step = ((uint32_t)m16(EP(M_PH_RATE), 97391) + 1948u) * 16u * (D.th[T_FREEZE].st >= 2 ? 2u : 1u);   /* 0.02 .. 1 Hz */
        int32_t uL, uR;
        D.phlfo += step;
        uL = ONE16 / 2 + (sin24(D.phlfo) >> 9); uR = ONE16 / 2 + (sin24(D.phlfo + 0x40000000u) >> 9);
        D.apL = 2 * G_BR_T[clampi(221 + (224 * uL >> 16), 0, 704)] - (1 << 30);
        D.apR = 2 * G_BR_T[clampi(221 + (224 * uR >> 16), 0, 704)] - (1 << 30);
    }
    svfb_set(&D.cab, tabi(BT_CABC, 65, q16));
    if (D.md_hpf) svf_set(&D.riseHP, CI_120 + (D.md_hpf - 120) * 64 / 400);
    /* pitch */
    eng = D.eng = clampi(P.eng, 0, NENG - 1);
    acid = eng == E_ACID || eng == E_SCREAM;
    {
        int32_t nn8 = D.note8 + D.md_pitch8 + D.dive8;
        D.inc = pitch_inc(nn8, 1); D.incS = D.inc >> 1;
        if (D.th[T_STACK].st >= 2 || D.stack_sm > 64) {   /* STACK: a fifth and an octave above */
            D.incA = (uint32_t)(((uint64_t)D.inc * 98193u) >> 16); D.incB = D.inc << 1;
            D.invA = inv_of(D.incA); D.invB = inv_of(D.incB);
        }
        D.invdt = inv_of(D.inc);
        D.sqHP.G = G_OS_T[clampi(nn8 / 48 - 211, 0, 704)];
        D.foldComp16 = nn8 > (48 << 8) ? exp2_16(-((nn8 - (48 << 8)) * 33722 / 3072)) : ONE16;  /* 0.7^(oct) */
        kt = (eng == E_SCREAM ? ONE16 / 2 : 0);
        if (eng == E_REESE) {   /* the side saws: -13 / +11 cents x (0.25 + DETUNE), and the drift */
            int32_t det = ONE16 / 4 + EP(M_CHR);
            D.inc2 = (uint32_t)(((uint64_t)D.inc * (uint32_t)exp2_16(m16(det, -13 * ONE16 / 1200) + D.drift)) >> 16);
            D.inc3 = (uint32_t)(((uint64_t)D.inc * (uint32_t)exp2_16(m16(det, 11 * ONE16 / 1200) - m16(D.drift, ONE16 * 7 / 10))) >> 16);
            D.inv2 = inv_of(D.inc2); D.inv3 = inv_of(D.inc3);
        } else if (eng == E_SCREAM) {   /* two saws, +14 / -7 cents, each into its own ladder */
            D.inc2 = (uint32_t)(((uint64_t)D.inc * 66066u) >> 16); D.inc3 = (uint32_t)(((uint64_t)D.inc * 65271u) >> 16);
            D.inv2 = inv_of(D.inc2); D.inv3 = inv_of(D.inc3);
        } else if (eng == E_GROWL) {   /* FM: ratio, index from the envelope, less up high (0.75 an octave above C3) */
            int32_t I16 = m16(ONE16 * 6 / 10 + m16(ONE16 * 29 / 10, D.idx >> 8), ONE16 * 3 / 10 + EP(M_CHR));
            if (nn8 > (48 << 8)) I16 = m16(I16, exp2_16(-((nn8 - (48 << 8)) * 27199 / 3072)));
            D.incM = D.inc * (uint32_t)clampi(P.ratio, 1, 4);
            D.fmI24 = m16(I16, 2670177);               /* I / 2 pi, Q24 */
        }
    }
    /* cutoff (index: 1/64 octave above 20 Hz) */
    EM = EP(M_ENV);
    cut = EP(M_CUT);
    if (acid)
        c16 = (250 << 16) + 192 * cut - m16(EM, 83 << 16) + m16(m16(m16(EM, 352 << 16), D.envF >> 8), ONE16 + D.wow)
            + m16(m16(EP(M_ACC), D.accCap >> 8), 141 << 16);
    else
        c16 = (64 << 16) + 544 * cut + m16(m16(m16(EM, 384 << 16), D.envF >> 8), ONE16 + D.wow) + m16(m16(EP(M_ACC), D.accCap >> 8), 85 << 16);
    c16 += D.mod_oct16 * 64 + D.md_cut16 * 1 + D.roll_cut16 * 64 + D.vary_c16 + m16(kt, (D.note8 - (48 << 8)) * 1365);
    c16 = clampi(c16, 0, 638 << 16);
    s_fcidx = c16 >> 16;
    res = clampi(EP(M_RES) + D.md_res16, 0, eng == E_SCREAM ? ONE16 * 99 / 100 : ONE16 * 95 / 100);
    k24 = res * 256 * (eng == E_SCREAM ? 5 : 4);            /* k = 4 res (SCREAM 5), Q24 */
    if (eng == E_GROWL) {   /* resonant SVF, k = 2 - 1.95 res */
        int32_t g16 = tabi(TAN_OS_T, 705, c16), k16 = 2 * ONE16 - m16(res, ONE16 * 195 / 100);
        int32_t dn16 = ONE16 + m16(g16, g16 + k16), a1 = (int32_t)((0x40000000u / (uint32_t)dn16) << 16);
        D.gsvf.a1 = a1; D.gsvf.a2 = (int32_t)(((int64_t)a1 * g16) >> 16); D.gsvf.a3 = (int32_t)(((int64_t)D.gsvf.a2 * g16) >> 16);
        /* the formant combs: 730 -> 570 and 1090 -> 840 Hz, swept by an LFO on the eighths */
        {
            int32_t ph = ONE16 / 2 + (sin24(D.formPh) >> 9), f1 = lerp16(730, 570, ph), f2 = lerp16(1090, 840, ph);
            D.cda8 = (int32_t)((FO << 8) / (uint32_t)f1); D.cdb8 = (int32_t)((FO << 8) / (uint32_t)f2);
        }
    } else {
        lad_coef(&D.lc, c16, k24, G_OS_T[eng == E_SCREAM ? CI_40 : CI_150]);
        if (eng == E_SCREAM) lad_coef(&D.lc2, clampi(c16 + m16(EP(M_CHR), 54 << 16), 0, 638 << 16), k24, G_OS_T[CI_40]);
    }
    D.formPh += (uint32_t)clampi(P.bpm10, 600, 2000) * 325u * 16u;   /* 2 x bpm / 60 Hz, 16 samples a tick */
    /* envelopes */
    {
        int32_t di = (m16(EP(M_DEC), 255 << 16) >> 16) + (P.vary > 0 ? (D.vary_c16 >> 16) / 2 : 0);
        D.kF = D.acc && acid ? KF_ACC : acid ? KF_ACID_T[clampi(di, 0, 255)] : KF_WIDE_T[clampi(di, 0, 255)];
        D.kIdx = K_IDX_T[clampi(EP(M_DEC) >> 10, 0, 63)];
        D.kRel = K_REL_T[clampi((P.rel - 1) * 63 / 199, 0, 63)];
        D.kDive = K_DIVE_T[clampi(P.tp[T_DIVE][0] >> 10, 0, 63)];
        D.stack_t = D.th[T_STACK].st >= 2 ? ONE16 / 2 + P.tp[T_STACK][1] / 2 : 0;
    }
    D.kGlide = K_GLIDE_T[clampi(P.glide >> 10, 0, 63)];
    D.kCapC = K_CAPC_T[clampi(res >> 10, 0, 63)]; D.kCapD = K_CAPD_T[clampi(res >> 10, 0, 63)];
    D.kKick = K_KICK_T[32]; D.kDuck = K_DUCK_T[clampi(EP(M_DUCK) >> 10, 0, 63)];
    /* delay */
    {
        int32_t tgt = (int32_t)(((int64_t)sl16 * DLY_STEPS[clampi(P.dly_time, 0, 4)]) >> 16);
        int32_t fbt;
        if (tgt > (int32_t)DLY_N - 4) tgt = (int32_t)DLY_N - 4;
        D.dtime16 += ((tgt << 8) - D.dtime16) / 50;           /* Q8 samples, smoothed: tape-like pitch on change */
        fbt = D.md_fbov >= 0 ? (D.md_fbov > EP(M_DLY_FB) ? D.md_fbov : EP(M_DLY_FB)) : EP(M_DLY_FB);
        D.dlyfb += (fbt - D.dlyfb) / (D.md_fbov >= 0 ? 20 : 250);
        s_dfb = D.dlyfb;
    }
    /* tape stop rate */
    if (D.tape_mode == 1) {
        int32_t dur = P.tp[T_STOP][0] < ONE16 / 3 ? 4 : P.tp[T_STOP][0] < ONE16 * 2 / 3 ? 8 : 16, u;
        if (D.th[T_STOP].st == 3 && (int32_t)(D.gstep - (uint32_t)D.spin_from) >= 0) { u = clampi(REL16((uint32_t)D.spin_from) / 2, 0, ONE16); D.tape_rate16 = (int32_t)isqrt64((uint64_t)u << 16); }
        else { u = clampi(REL16(D.th[T_STOP].s0) / dur, 0, ONE16); D.tape_rate16 = m16(ONE16 - u, ONE16 - u); }
    }
    s_rate = D.tape_mode ? D.tape_rate16 : ONE16;
    s_env = D.envF; s_acccap = D.accCap; s_amp = D.amp; s_wow = D.wow;
    s_dive = D.dive8; s_gate = D.gate_env; s_stack = D.stack_sm;
    s_frac += (clampi(m16(bx, ONE16 * 8 / 10) + m16(m16(D.amp >> 8, bx), ONE16 * 3 / 10) + D.drop_burst16, 0, ONE16) - s_frac) / 20;
}

/* log2 of a Q24 value, Q16 (to ~0.005); shifts only */
static int32_t log2_24(int32_t x)
{
    int32_t n = 31, f;
    uint32_t m;
    if (x <= 0) return -24 * ONE16;
    m = (uint32_t)x;
    while (!(m & 0x80000000u)) { m <<= 1; n--; }
    f = (int32_t)(m >> 15) - ONE16;                 /* the mantissa's fraction, Q16 */
    return (n - 24) * ONE16 + f + m16(m16(f, ONE16 - f), 22715);   /* + 0.3466 f (1 - f) */
}

/* ---------------------------------------------------------------- stage 2 */
HOT_FN void c1_job(uint32_t b)
{
    const blk_t *B = &BK[b & 1u];
    int32_t *out = s2_out;
    uint32_t i, t = 0;
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_mc_ticks();
#endif
    if (!b) return;                                 /* (two cores, the first block: nothing made yet) */
    for (i = 0; i < NB; i++) {
        const seg_t *G = &B->sg[i >> 4];
        int32_t hi, low, yl, yr, kick, hat, L, R, hL, hR;
        /* the high band: decimated, then the cabinet and the 100 Hz HP (linear: at the base rate) */
        hb_push(&S2.decH, B->hos[2u * i]); hb_push(&S2.decH, B->hos[2u * i + 1u]);
        hi = hb_out(&S2.decH);
        S2.cab.a1 = G->a1; S2.cab.a2 = G->a2; S2.cab.a3 = G->a3;
        hi = svf_hp(&S2.hhp, svf_lp(&S2.cab, hi));
        if ((i & 15u) == 0) {   /* slow loudness correction, +-6 dB, target +3 dB at full BURST */
            int32_t ri = G->rmsIn > 64 ? G->rmsIn : 64, ro = S2.rmsOut > 64 ? S2.rmsOut : 64;
            int32_t tgt = m16((int32_t)(((uint32_t)ri << 4) / (((uint32_t)ro >> 12) | 1u)), ONE16 + G->burst);   /* power ratio, Q16 */
            int32_t g16 = tgt < ONE16 / 4 ? ONE16 / 2 : tgt > 4 * ONE16 ? 2 * ONE16 : (int32_t)isqrt64((uint64_t)tgt << 16);
            S2.corr16 += (g16 - S2.corr16) / 200;
        }
        hi = m16(hi, S2.corr16);
        S2.rmsOut += (m24(hi, hi) - S2.rmsOut) >> 13;
        hi = m16(hi, B->rg[i]);
        /* the low band: second LP stage, the sub */
        low = m16(m16(svf_lp(&S2.xl2, B->l1[i]), G->low16) + B->sub[i], G->msub16);
        if (G->rmb > 0) {   /* RUMBLE: the low band, a little of the high and the kick through four diffusers, fed back */
            int32_t xin = low + m16(hi, ONE16 * 3 / 10) + (S2.kout >> 1) + S2.rfb, k;
            for (k = 0; k < 4; k++) {
                uint32_t n = (uint32_t)(RMB_AT[k + 1] - RMB_AT[k]), pp = S2.rp[k];
                int32_t d = (int32_t)rmb[RMB_AT[k] + pp] * 512, w = xin + m16(d, ONE16 * 6 / 10);
                rmb[RMB_AT[k] + pp] = (int16_t)clampi(w >> 9, -32767, 32767);
                xin = d - m16(w, ONE16 * 6 / 10);
                S2.rp[k] = pp + 1u == n ? 0u : pp + 1u;
            }
            S2.rfb = tanh24(m16(op_lp(&S2.rLP, xin), ONE16 * 82 / 100 + m16(G->rmb, ONE16 / 10)));
            low += m16(S2.rfb, m16(G->rmb, ONE16 * 9 / 10));
        }
        low = m16(tanh24(m16(low, 92560)), 46396);           /* 0.708 tanh(low / 0.708) */
        if (G->harm > 0) {   /* the low band's harmonics, for small speakers: 160 Hz HP, 700 Hz LP of tanh(6 low) */
            int32_t hg = op_lp(&S2.hLP, svf_hp(&S2.hHP1, tanh24(low * 6)));
            low += m16(hg, m16(G->harm, ONE16 * 45 / 100));
        }
        /* crush: BURST above .85 or SHRED */
        if (G->crdiv > 1 || G->crbits < 16) {
            int32_t sh2 = 24 - (G->crbits - 1);
            if (++S2.crush_acc >= G->crdiv) { S2.crush_acc = 0; S2.crush_hold = (int32_t)((uint32_t)(hi >> sh2) << sh2); }
            S2.crush_lp += (S2.crush_hold - S2.crush_lp) * 3 / 4; hi = S2.crush_lp;
        }
        /* OTT on the high band: down above -16 dB, up below -34 dB, by BURST (x SQUASH) */
        {
            int32_t ab = iabs(hi);
            S2.ott_env += ab > S2.ott_env ? m16(ab - S2.ott_env, 3277) : m16(ab - S2.ott_env, 39);
            if ((i & 15u) == 0) {
                int32_t edb = m16(log2_24(S2.ott_env + 16), 394566), gdb = 0;   /* x 6.0206: dB, Q16 */
                if (edb > -16 * ONE16) gdb = -m16(edb + 16 * ONE16, ONE16 * 72 / 100);
                else if (edb < -34 * ONE16 && edb > -70 * ONE16) gdb = clampi(m16(-34 * ONE16 - edb, ONE16 / 2), 0, 12 * ONE16);
                S2.ott_g0 = S2.ott_g;
                S2.ott_g = G->ott16 > 0 ? exp2_16(m16(m16(gdb, G->ott16) + 5 * G->ott16, 10885)) : ONE16;   /* / 6.0206 */
            }
            if (G->ott16 > 0 || S2.ott_g != ONE16) hi = m16(hi, S2.ott_g0 + ((S2.ott_g - S2.ott_g0) * (int32_t)(i & 15u) >> 4));
        }
        {   /* FREEZE: the high band recorded at half rate, all the time; a loop of it played over the live one */
            int32_t fz = 0;
            if (G->frz_ev == 1 && (i & 15u) == 0) {
                uint32_t n2 = (uint32_t)G->frz_len >> 1;
                S2.flen = n2 < 2u ? 2u : n2; S2.fstart = (S2.fw + FRZ_N - S2.flen) % FRZ_N; S2.fpos = 0; S2.fon = 1; S2.fgain = ONE24;
            }
            if (G->frz_ev == 2 && (i & 15u) == 0) S2.fon = 2;   /* landed: fade */
            if (i & 1u) { frz[S2.fw] = (int16_t)clampi((S2.facc + (hi >> 1)) >> 9, -32767, 32767); S2.fw = S2.fw + 1u == FRZ_N ? 0u : S2.fw + 1u; }
            else S2.facc = hi >> 1;
            if (S2.fon) {
                uint32_t a = (S2.fstart + (S2.fpos >> 1)) % FRZ_N, b2 = (a + 1u) % FRZ_N;
                int32_t v = (int32_t)frz[a] * 512;
                if (S2.fpos & 1u) v = (v + (int32_t)frz[b2] * 512) >> 1;
                fz = m16(m24(v, S2.fgain), G->frz_mix);
                S2.fpos = S2.fpos + 1u >= 2u * S2.flen ? 0u : S2.fpos + 1u;
                if (S2.fon == 2) { S2.fgain = m30(S2.fgain, G->frz_fade); if (S2.fgain < 4096) S2.fon = 0; }
                hi = m16(hi, S2.fon == 1 ? G->frz_dry : ONE16) + fz;
            }
        }
        /* void: dry and sub out over 20 ms */
        S2.void_mute += (G->void_tgt - S2.void_mute) / 600;
        hi = m16(hi, S2.void_mute); low = m16(low, S2.void_mute);
        hL = hR = hi;
        if (G->ph16 > 0) {   /* phaser: 6 all-passes a side, a little feedback */
            int c, k;
            for (c = 0; c < 2; c++) {
                int32_t A = c ? G->apR : G->apL, xx = hi + m16(S2.apfb[c], ONE16 * 4 / 10), yy;
                for (k = 0; k < 6; k++) { yy = m30(xx, A) + S2.ap[c][k]; S2.ap[c][k] = xx - m30(yy, A); xx = yy; }
                S2.apfb[c] = xx;
                yy = m16(hi, ONE16 - G->ph16 / 2) + m16(xx, G->ph16 / 2);
                if (c) hR = yy; else hL = yy;
            }
        }
        {   /* tape delay: ping-pong, high band only */
            int32_t dtm = G->dt8, dt8 = dtm + m24(m16(sin24(S2.wowph), ONE16 * 15 / 10000), dtm), di = dt8 >> 8, df = (dt8 & 255) << 8;
            uint32_t r0 = (S2.dw + DLY_N - (uint32_t)di) % DLY_N, r1 = (r0 + DLY_N - 1u) % DLY_N;
            int32_t send = m16((hL + hR) >> 1, G->send16), fl, fr;
            yl = ((int32_t)dlyL[r0] * 512) + m16(((int32_t)dlyL[r1] - dlyL[r0]) * 512, df);
            yr = ((int32_t)dlyR[r0] * 512) + m16(((int32_t)dlyR[r1] - dlyR[r0]) * 512, df);
            S2.wowph += 58434u;                              /* ~0.6 Hz */
            fl = m16(op_lp(&S2.dly[1], op_hp(&S2.dly[0], yr)), G->fb);
            fr = m16(op_lp(&S2.dly[3], op_hp(&S2.dly[2], yl)), G->fb);
            fl = m16(tanh24(m16(fl, 78643)), 54613); fr = m16(tanh24(m16(fr, 78643)), 54613);
            dlyL[S2.dw] = (int16_t)clampi((send + fl) >> 9, -32767, 32767);
            dlyR[S2.dw] = (int16_t)clampi(fr >> 9, -32767, 32767);
            S2.dw = S2.dw + 1u == DLY_N ? 0u : S2.dw + 1u;
            if (G->vlp >= 0) {
                int32_t gg = G_BR_T[clampi(G->vlp, 0, 704)];
                S2.dlpL += m30(yl - S2.dlpL, gg); S2.dlpR += m30(yr - S2.dlpR, gg); yl = S2.dlpL; yr = S2.dlpR;
            }
        }
        /* duck + sum */
        L = m16(low + hL + yl, B->bg[i]); R = m16(low + hR + yr, B->bg[i]);
        /* scope: the bass, every 2nd sample */
        if (i & 1u) { scope[scope_w & 511u] = (int16_t)clampi((L + R) >> 10, -32767, 32767); scope_w++; }
        /* reference drums */
        while (t < B->ntrig && B->at[t] == i) drum_trig(B, B->a[t++]);
        S2.kP = m30(S2.kP, K_KICKP);
        S2.kPh += S2.kInc + (uint32_t)m24((int32_t)(S2.kInc >> 1), m24(S2.kP, S2.kP)) * 4u;
        S2.kEnv = m30(S2.kEnv, K_KICK_T[48]); S2.click = m30(S2.click, K_CLICK);
        S2.noise = S2.noise * 1103515245u + 12345u;
        kick = m24(sin24(S2.kPh), m16(S2.kEnv, 72090)) + m16(m24((int32_t)S2.noise >> 7, S2.click), ONE16 / 4);
        kick = tanh24(m16(kick, 91750));
        S2.kout = kick;
        S2.hEnv = m30(S2.hEnv, K_HAT);
        hat = m16(m24(op_hp(&S2.hHP, (int32_t)S2.noise >> 7), S2.hEnv), ONE16 * 35 / 100);
        {
            int32_t dl = 0, dr = 0;
            if (B->drums && B->playing) { dl = m16(kick + m16(hat, ONE16 * 8 / 10), B->drum_lvl); dr = m16(kick + m16(hat, ONE16 * 11 / 10), B->drum_lvl); }
            if (usb_stems) {   /* SYS > USB STEMS: bass, drums, delay return, at the volume, before the master's ceiling */
                int16_t *q = &stem_q[6u * i];
                int32_t bl = m16(low + hL, B->bg[i]), br = m16(low + hR, B->bg[i]), v = P.vol;
                q[0] = (int16_t)clampi(m16(bl, v) >> 9, -32767, 32767); q[1] = (int16_t)clampi(m16(br, v) >> 9, -32767, 32767);
                q[2] = (int16_t)clampi(m16(dl, v) >> 9, -32767, 32767); q[3] = (int16_t)clampi(m16(dr, v) >> 9, -32767, 32767);
                q[4] = (int16_t)clampi(m16(L - bl, v) >> 9, -32767, 32767); q[5] = (int16_t)clampi(m16(R - br, v) >> 9, -32767, 32767);
            }
            L += dl; R += dr;
        }
        /* DC block */
        { int32_t a = L - S2.dcx + m30(S2.dcy, 1072235000), c = R - S2.dcxr + m30(S2.dcyr, 1072235000); S2.dcx = L; S2.dcy = a; S2.dcxr = R; S2.dcyr = c; L = a; R = c; }
        /* tape stop (mono varispeed) */
        if (i & 1u) { tape[S2.tw] = (int16_t)clampi((S2.tacc + ((L + R) >> 1)) >> 10, -32767, 32767); S2.tw = S2.tw + 1u == TAPE_N ? 0u : S2.tw + 1u; }
        else S2.tacc = (L + R) >> 1;
        if (G->tm && G->tm != S2.tm_prev) S2.tr16 = S2.tw << 16;   /* engaged: the head starts where the tape is now */
        S2.tm_prev = G->tm;
        if (G->tm) {
            uint32_t ti, t2; int32_t tv, g8, tr = G->tr;
            if (G->tm == 2) {                              /* REV: backwards at speed (half-rate positions) */
                S2.tr16 = S2.tr16 >= 0x8000u ? S2.tr16 - 0x8000u : S2.tr16 + (TAPE_N << 16) - 0x8000u;
                tr = ONE16;
            } else
                S2.tr16 += (uint32_t)tr >> 1;              /* (half-rate positions) */
            if ((S2.tr16 >> 16) >= TAPE_N) S2.tr16 -= TAPE_N << 16;
            ti = S2.tr16 >> 16; t2 = ti + 1u == TAPE_N ? 0u : ti + 1u;
            tv = ((int32_t)tape[ti] * 512) + m16(((int32_t)tape[t2] - tape[ti]) * 512, (int32_t)(S2.tr16 & 0xFFFFu));
            g8 = clampi(tr * 4, 0, ONE16);                         /* silent when the tape stands still */
            S2.tape_lp += m30(m16(tv, g8) - S2.tape_lp, G_BR_T[clampi(CI_150 + m16(tr, 500), 0, 704)]);   /* (REV: open) */
            if (S2.tape_xf < ONE16) S2.tape_xf += ONE16 / 100;
            L = lerp16(L, S2.tape_lp, S2.tape_xf); R = lerp16(R, S2.tape_lp, S2.tape_xf);
        } else if (S2.tape_xf > 0) { S2.tape_xf -= ONE16 / 220; if (S2.tape_xf < 0) S2.tape_xf = 0; L = lerp16(L, S2.tape_lp, S2.tape_xf); R = lerp16(R, S2.tape_lp, S2.tape_xf); }
        /* master: soft ceiling at -1 dBFS, out in Q15 */
        L = m16(tanh24(m16(m16(L, P.vol), 73550)), 58395);
        R = m16(tanh24(m16(m16(R, P.vol), 73550)), 58395);
        S2.rms += (m24(L, L) - S2.rms) >> 11;
        out[2u * i] += L >> 9;
        out[2u * i + 1u] += R >> 9;
    }
#ifndef DINGHY_HOST
    S2.us = (fm1_mc_ticks() - t0) / FM1_TICKS_PER_US;
#endif
}

/* ---------------------------------------------------------------- following an external MIDI clock (24 ppqn)
 * The ticks come stamped (TIMER4, 24 MHz) from USB and the TRS input (platform.c clk_q). Their period gives the tempo;
 * the count since Start, with the time since the last tick, gives where the step grid should be: the sequencer is
 * pulled there a quarter of the way each block, or put there at once when it is more than two steps off */
static struct { uint32_t n, t_last, per8, t_any; int started, following, hard; } XC;
static void ext_clock(void)
{
    uint32_t now = clk_now();
    while (ck_r != ck_w) {
        uint32_t t = clk_q[ck_r % CKN].t, st = clk_q[ck_r % CKN].st;
        ck_r++;
        if (!P.sync) continue;
        XC.t_any = t;
        if (st == 0xFAu) {   /* Start: from the top, at the next tick */
            XC.n = 0; XC.started = 1; XC.hard = 1;
            D.abs = 0; D.spos16 = 0; D.gstep = 0; D.gpos16 = 0; D.onsets = (uint32_t)-1; D.bar = 0; D.trig_done = 0; D.prev_slide = 0; D.clk_acc = 0;
            D.playing = 1; D.nlive = 0;
        } else if (st == 0xFBu) { D.playing = 1; XC.started = 1; }
        else if (st == 0xFCu) { if (D.playing) gate_off(); D.playing = 0; XC.started = 0; }
        else {
            uint32_t dt = t - XC.t_last;
            if (XC.n && dt > 2400u && dt < 2400000u) {   /* 0.1 .. 100 ms: 25 .. 25000 BPM */
                if (!XC.per8 || dt * 2u < XC.per8 >> 8 || dt > (XC.per8 >> 8) * 2u) XC.per8 = dt << 8;
                else XC.per8 += (int32_t)((dt << 8) - XC.per8) / 8;
            }
            XC.t_last = t; XC.n++;
        }
    }
    XC.following = P.sync == 2 || (P.sync == 1 && XC.per8 && now - XC.t_any < 7200000u);   /* AUTO: a tick in 0.3 s */
    if (!XC.following || !XC.per8) return;
    {   /* the tempo: 6 ticks a step */
        int32_t sl16 = (int32_t)(((uint64_t)XC.per8 * 184971u) >> 16);
        D.step_len16 = clampi(sl16, (int32_t)(FS * 60u / 4u / 300u) << 16, (int32_t)(FS * 60u / 4u / 30u) << 16);
        D.inv_sl = 0xFFFFFFFFu / (uint32_t)(D.step_len16 >> 8);
    }
    if (XC.started && XC.n && D.playing) {   /* the phase */
        uint32_t per = XC.per8 >> 8, el = now - XC.t_last, k = XC.n - 1u, f16;
        int32_t ext, in, err, d;
        if (el > per) el = per;
        f16 = per ? ((el << 11) / per) << 5 : 0u;        /* the tick's fraction, Q16 */
        ext = (int32_t)((k / 6u) << 16) + (int32_t)(((k % 6u) * 65536u + f16) / 6u);
        in = (int32_t)(D.gstep << 16) + (int32_t)((((uint32_t)D.gpos16 >> 8) * (uint64_t)D.inv_sl) >> 16);
        err = ext - in;
        if (XC.hard || err > 2 * ONE16 || err < -2 * ONE16) {
            uint32_t sq = (uint32_t)ext >> 16, fr = (uint32_t)ext & 0xFFFFu;
            int32_t pos = (int32_t)(((uint64_t)fr * (uint32_t)D.step_len16) >> 16);
            D.gstep = sq; D.gpos16 = pos; D.abs = sq; D.spos16 = pos; D.trig_done = pos > 0;
            XC.hard = 0;
        } else {
            d = (int32_t)(((int64_t)err * D.step_len16) >> 16) / 4;
            d = clampi(d, -(D.step_len16 >> 4), D.step_len16 >> 4);
            D.gpos16 = clampi(D.gpos16 + d, 0, D.step_len16 - 1);
            D.spos16 = clampi(D.spos16 + d, 0, D.step_len16 - 1);
        }
    }
}

/* ---------------------------------------------------------------- the audio ISR */
HOT_FN void app_audio(int32_t *out, uint32_t n)
{
#ifndef DINGHY_HOST
    uint32_t t_start = fm1_ticks();
#endif
    uint32_t i;
    int mc;
    /* commands from the main loop */
    while (cq_r != cq_w) {
        cmd_t c = cq[cq_r & 63u];
        cq_r++;
        switch (c.op) {
        case OP_ON:
            if (D.playing) { D.key_pend = semi_to_deg(P.scale, c.a - 60); break; }
            if (D.nlive < 8u) D.live[D.nlive++] = c.a;
            voice_on(c.a - 24, c.b & 1, D.nlive > 1 && D.gate);
            emit(EV_NOTE, (uint32_t)(c.a - 24), (uint32_t)(c.b & 1), 0);
            break;
        case OP_OFF: {
            uint32_t k, j = 0;
            if (D.playing) break;
            for (k = 0; k < D.nlive; k++) if (D.live[k] != c.a) D.live[j++] = D.live[k];
            D.nlive = j;
            if (j) voice_on(D.live[j - 1] - 24, 0, 1); else { gate_off(); emit(EV_OFF, 0, 0, 0); }
            break;
        }
        case OP_PLAY:
            if (c.a && !D.playing) { D.abs = 0; D.spos16 = 0; D.gstep = 0; D.gpos16 = 0; D.onsets = (uint32_t)-1; D.bar = 0; D.trig_done = 0; D.prev_slide = 0; D.clk_acc = 0; }
            if (!c.a && D.playing) { int l; gate_off(); for (l = 0; l < NMP; l++) lk[l] = -1; }
            D.playing = c.a; D.nlive = 0;
            break;
        case OP_ENGAGE: th_engage(c.a, c.b); break;
        case OP_RELEASE: if (D.th[c.a].st) { D.th[c.a].rel = 1; D.th[c.a].latched = 0; } break;
        case OP_ABORT: th_land(c.a, 1); break;
        case OP_KEEP: if (c.a == T_RISE || c.a == T_BUILD) P.cut = clampi(EP(M_CUT) + D.md_cut16 / 192, 0, ONE16); th_land(c.a, 1); break;
        case OP_PANIC: {
            int t;
            for (t = 0; t < NTH; t++) th_land(t, 1);
            D.dlyfb = 0; memset(dlyL, 0, sizeof dlyL); memset(dlyR, 0, sizeof dlyR);
            for (t = 0; t < (int)NC; t++) BK[0].sg[t].fb = BK[1].sg[t].fb = 0;   /* (core 1 is idle here: block start) */
            emit(EV_PANIC, 0, 0, 0);
            break;
        }
        case OP_FILL: D.fill = c.a; break;
        case OP_DROP: D.drop_arm = 1; break;
        case OP_GENOME: if (c.b && D.playing) g_pend = c.a; else { g_a = c.a; g_pend = -1; } break;
        case OP_GENOME_B: g_b = c.b < 0 ? -1 : c.a; break;
        }
    }
    if (!dsp_ready) return;
    {
        uint32_t bpm = (uint32_t)clampi(P.bpm10, 600, 2000), num = FS * 600u / 4u, q = num / bpm, r = num - q * bpm;
        D.step_len16 = (int32_t)((q << 16) + (r << 16) / bpm);
        D.inv_sl = 0xFFFFFFFFu / (uint32_t)(D.step_len16 >> 8);
    }
    ext_clock();
    s_ext = XC.following;
    s_bpm_ext = XC.following ? (int32_t)((FS * 600u / 4u) * 256u / ((uint32_t)D.step_len16 >> 8)) : 0;
    (void)n;                                        /* (always NB: platform.c HALF_FRAMES) */
    D.swing16 = m16(P.swing, D.step_len16);
    s2_out = out;
    mc = fm1_mc_run(c1_job, D.blk);                 /* stage 2 of the block before on core 1, beside stage 1 */
    D.blk++;
    {
        blk_t *B = &BK[D.blk & 1u];
        B->ntrig = 0; B->playing = D.playing; B->drums = (uint8_t)(P.drums != 0); B->drum_lvl = P.drum_lvl;
        B->kick_tune = (uint8_t)(P.kick_tune != 0); B->root = (uint8_t)P.root;
    }
    for (i = 0; i < NB; i++) {
        int32_t sl16 = D.step_len16, k;
        uint32_t o;
        blk_t *B = &BK[D.blk & 1u];
        D.cur_i = i;
        /* ---- clocks */
        D.gpos16 += ONE16;
        if (D.gpos16 >= sl16) {
            D.gpos16 -= sl16; D.gstep++; step_boundary();
            if (D.roll_steps16) {   /* realign the roll grid: every step, every 2nd for 1/8, every beat for triplets */
                int32_t al = D.roll_steps16 > ONE16 ? 2 : (D.roll_steps16 == ONE16 * 2 / 3 ? 4 : 1);
                if ((D.gstep % (uint32_t)al) == 0) { D.roll_acc16 = D.roll_per16; }
            }
        }
        if (D.playing) {
            int32_t swing16 = (D.abs & 1u) ? D.swing16 : 0;
            D.clk_acc += 6u * ONE16;                          /* 24 ppqn = 6 per 1/16 */
            while (D.clk_acc >= (uint32_t)sl16) { D.clk_acc -= (uint32_t)sl16; s_clk24++; }
            if (!D.trig_done && D.spos16 >= swing16) { D.trig_done = 1; D.ev[0].at16 = 0; D.nev = 0; seq_step(); D.ev[9].at16 = D.spos16; }
            if (D.nev) {
                uint32_t e; int32_t rel = D.spos16 - D.ev[9].at16;
                for (e = 0; e < D.nev; e++)
                    if (D.ev[e].ty != 255 && rel >= D.ev[e].at16) {
                        if (D.ev[e].ty == 1) voice_on(D.ev[e].note, D.ev[e].acc, 0); else { gate_off(); emit(EV_OFF, 0, 0, 0); }
                        D.ev[e].ty = 255;
                    }
            }
            D.spos16 += ONE16;
            if (D.spos16 >= sl16) { D.spos16 -= sl16; D.abs++; D.trig_done = 0; }
        }
        /* ---- roll / shred retrigger grid */
        if (D.roll_steps16) {
            int32_t per16 = m16(sl16, D.roll_steps16);
            if (per16 < ONE16) per16 = ONE16;
            if (D.roll_per16 != per16) { D.roll_per16 = per16; if (D.roll_idx < 0) D.roll_acc16 = per16; }
            D.roll_acc16 += ONE16;
            if (D.roll_acc16 >= per16) {
                D.roll_acc16 -= per16; if (D.roll_acc16 >= per16) D.roll_acc16 = 0;
                D.roll_idx = 1; D.roll_hits++;
                if (D.envF < ONE24 * 85 / 100) D.envF = ONE24 * 85 / 100;
                D.att = 1;
                if (D.th[T_SHRED].st >= 2) {
                    static const uint8_t PAT[4][4] = {{1, 2, 4, 8}, {8, 4, 2, 1}, {1, 4, 2, 8}, {2, 8, 1, 4}};
                    int32_t pk = (D.roll_hits - 1) & 3, dv = PAT[clampi(P.tp[T_SHRED][2] >> 14, 0, 3)][pk];
                    D.shred_div = dv; D.shred_bits = dv == 1 ? 12 : dv == 2 ? 8 : dv == 4 ? 6 : 4;
                    if (pk == 0) D.accEnv = ONE24;
                }
                if (!D.th[T_BUILD].st) D.roll_cut16 = clampi(D.roll_cut16 + ONE16 * 6 / 100, 0, 2 * ONE16);
                D.roll_gate_on = (int32_t)((int64_t)per16 * (ONE16 * 35 / 100 + m16(ONE16 * 3 / 10, P.tp[T_ROLL][1])) >> 32);
                emit(EV_ROLL, 0, 0, 0);
            }
            D.roll_gate_on--;
            if (D.playing && !D.gate) D.gate = 1;
        } else { D.roll_hits = 0; D.roll_idx = -1; D.roll_per16 = 0; D.roll_cut16 = m16(D.roll_cut16, 65500); D.roll_gate_on = 1 << 30; D.shred_div = 1; D.shred_bits = 16; }
        k = D.roll_gate_on > 0 ? ONE16 : 0;
        D.roll_gate_env += (k - D.roll_gate_env) / (k ? 2 : 50);

        if (++D.ctr >= 16u) { D.ctr = 0; control(); }
        if ((i & 15u) == 15u) {                     /* this segment's control values for stage 2 */
            seg_t *G = &B->sg[i >> 4];
            int32_t div = 1, bits = 16;
            if (s_burst > ONE16 * 85 / 100) { int32_t uu = (s_burst - ONE16 * 85 / 100) * 100 / 15; div = 1 + (uu > ONE16 / 2); bits = 16 - (8 * uu >> 16); }
            if (D.th[T_SHRED].st >= 2) { if (D.shred_div > div) div = D.shred_div; if (D.shred_bits < bits) bits = D.shred_bits; }
            G->crdiv = div; G->crbits = bits;
            G->low16 = D.low16; G->msub16 = D.md_sub16; G->burst = s_burst; G->void_tgt = D.void_tgt;
            G->send16 = clampi(EP(M_DLY_SEND) + D.md_send16, 0, ONE16);
            G->dt8 = D.dtime16; G->fb = D.dlyfb; G->vlp = D.th[T_VOID].st ? D.void_lp : -1;
            G->tr = D.tape_rate16; G->tm = (uint8_t)D.tape_mode; G->rmsIn = D.rmsIn;
            G->a1 = D.cab.a1; G->a2 = D.cab.a2; G->a3 = D.cab.a3;
            G->rmb = D.eng == E_RUMBLE ? EP(M_CHR) : 0; G->harm = EP(M_HARM);
            G->ott16 = D.ott16; G->ph16 = EP(M_PHASER); G->apL = D.apL; G->apR = D.apR;
            G->frz_ev = D.frz_ev; G->frz_len = D.frz_len; D.frz_ev = 0;
            G->frz_mix = m16(P.tp[T_FREEZE][1], ONE16 * 12 / 10) + ONE16 * 4 / 10; G->frz_dry = clampi(P.tp[T_FREEZE][2] * 2, 0, ONE16);
            G->frz_fade = (1 << 30) - (int32_t)((1u << 30) / (uint32_t)(4410 + m16(P.tp[T_FREEZE][3], 84000)));   /* 0.1 .. 2 s */
        }

        /* ---- envelopes (base rate) */
        if (D.att) { D.envF += ATT_F_STEP; if (D.envF >= ONE24) { D.envF = ONE24; D.att = 0; } } else D.envF = m30(D.envF, D.kF);
        if (D.gate) { if (D.ampAtt) { D.amp += ATT_A_STEP; if (D.amp >= ONE24) { D.amp = ONE24; D.ampAtt = 0; } } else D.amp = m30(D.amp, K_AMPHOLD); }
        else { D.amp = m30(D.amp, D.kRel); D.ampAtt = 0; }
        D.accEnv = m30(D.accEnv, K_ACCENV);
        D.accCap += m30(D.accEnv - D.accCap, D.accEnv > D.accCap ? D.kCapC : D.kCapD);
        D.wow = m30(D.wow, 1073698000);
        D.note8 = D.tgt8 + m30(D.note8 - D.tgt8, D.kGlide);
        D.vca = D.acc ? m16(D.amp, ONE16 + m16(EP(M_ACC), ONE16 * 9 / 10)) : D.amp;

        if (D.dive8) { D.dive8 = m30(D.dive8, D.kDive); if (D.dive8 < 3) D.dive8 = 0; }
        D.stack_sm += (D.stack_t - D.stack_sm) >> 9;
        {   /* GATE: a 16-step pattern on the global grid, the high band only */
            int32_t gt = ONE16;
            if (D.th[T_GATE].st >= 2) {
                uint32_t pat = GATE_PATS[clampi(P.tp[T_GATE][2] >> 14, 0, 3)], stp = D.gstep & 15u;
                int on = ((pat >> (15u - stp)) & 1u) && D.frac16 < 47186u;   /* 0.72 of the step */
                gt = on ? ONE16 : ONE16 - (ONE16 / 2 + P.tp[T_GATE][1] / 2);
            }
            D.gate_env += m16(gt - D.gate_env, gt > D.gate_env ? ONE16 * 35 / 100 : ONE16 * 6 / 100);
        }
        if (D.eng == E_GROWL) D.idx = m30(D.idx, D.kIdx);
        else if (D.eng == E_REESE) { D.noise = D.noise * 1103515245u + 12345u; D.drift += (((int32_t)(D.noise >> 22) - 512) >> 4) - (D.drift >> 13); D.drift = clampi(D.drift, -ONE16 / 400, ONE16 / 400); }

        /* ---- 2x oversampled core */
        for (o = 0; o < 2u; o++) {
            int32_t x, x2 = 0, y, s, hiS, h, hg, sh, par = 0;
            uint32_t ph, dt = D.inc;
            D.ph += dt;
            ph = D.ph;
            switch (D.eng) {
            default:                                    /* ACID (and SCREAM's first saw) */
                s = saw24(ph, dt, D.invdt);
                s += m16(m24(s, s) - ONE24 / 3, 5243);       /* + 0.08 (s^2 - 1/3) */
                x = s;
                if (D.eng == E_ACID && EP(M_CHR) > 0) {
                    uint32_t pw = 2018634629u, p2 = ph - pw;    /* 0.47 */
                    int32_t pu = (ph < pw ? ONE24 : -ONE24) - blep(ph, dt, D.invdt) + blep(p2, dt, D.invdt);
                    pu = m16(op_lp(&D.sqLP, op_hp(&D.sqHP, pu)), ONE16 * 104 / 100);
                    x = lerp16(s, pu, EP(M_CHR));
                }
                if (D.eng == E_SCREAM) {
                    D.ph2 += D.inc2; D.ph3 += D.inc3;
                    x = saw24(D.ph3, D.inc3, D.inv3); x2 = saw24(D.ph2, D.inc2, D.inv2);
                }
                break;
            case E_RUMBLE:                              /* sine + a little triangle */
                x = sin24(ph) + m16((int32_t)((ph >> 31) ? ~ph : ph) >> 6, ONE16 * 15 / 100) - ONE24 * 15 / 100;
                break;
            case E_REESE: {                             /* the centre saw + the two detuned ones, their lows cut */
                int32_t side;
                D.ph2 += D.inc2; D.ph3 += D.inc3;
                side = op_hp(&D.rHP2, op_hp(&D.rHP1, saw24(D.ph2, D.inc2, D.inv2) + saw24(D.ph3, D.inc3, D.inv3)));
                x = m16(saw24(ph, dt, D.invdt) + side, ONE16 * 62 / 100);
                break;
            }
            case E_GROWL: {                             /* 2-op FM + a little saw */
                uint32_t pm;
                D.phM += D.incM;
                pm = ph + (uint32_t)(((int64_t)D.fmI24 * sin24(D.phM)) >> 16);
                x = m16(sin24(pm), ONE16 * 85 / 100) + m16(saw24(ph, dt, D.invdt), ONE16 * 3 / 10);
                break;
            }
            }
            if (D.stack_sm > 64) {   /* STACK: fifth + octave saws, their lows cut */
                int32_t st;
                D.phA += D.incA; D.phB += D.incB;
                st = m16(saw24(D.phA, D.incA, D.invA) + saw24(D.phB, D.incB, D.invB), ONE16 * 45 / 100);
                st = m16(op_hp(&D.stHP2, op_hp(&D.stHP1, st)), D.stack_sm);
                x += st; if (D.eng == E_SCREAM) x2 += st;
            }
            if (D.md_noise) { D.noise = D.noise * 1103515245u + 12345u; x += m16((int32_t)D.noise >> 7, D.md_noise); }
            /* pre drive (asymmetric) */
            if (D.preG > ONE16 * 102 / 100 || D.bias) {
                x = op_hp(&D.preHP, tanh24(m16(x, D.preG) + D.bias) - D.tbias);
                if (D.eng == E_SCREAM) x2 = tanh24(m16(x2, D.preG) + D.bias) - D.tbias;
            }
            /* the filter */
            if (D.eng == E_GROWL) { int32_t lp, bp; svf_run(&D.gsvf, x, &lp, &bp); y = m16(lp, ONE16 * 12 / 10); }
            else if (D.eng == E_SCREAM) y = m16(lad_run(&D.lad, &D.lc, x) + lad_run(&D.lad2, &D.lc2, x2), ONE16 * 62 / 100);
            else y = lad_run(&D.lad, &D.lc, x);
            y = m24(y, D.vca);
            /* fold */
            {
                int32_t fa = m16(D.fold16, D.foldComp16);
                if (fa > 64) {
                    int32_t fg = ONE16 + m16(fa * 5, ONE16 * 4 / 10 + m16(ONE16 * 6 / 10, D.envF >> 8));
                    int32_t z = m16(y, fg);
                    y = lerp16(y, m16(sin24((uint32_t)z << 6), ONE16 * 9 / 10), clampi(m16(fa, ONE16 * 14 / 10), 0, ONE16));
                }
            }
            if (D.eng == E_GROWL && P.formant > 0) {   /* two formant combs */
                int32_t ca, cb, k;
                uint32_t ra, rb, fa_, fb_;
                ra = (D.cp - (uint32_t)(D.cda8 >> 8)) & 255u; fa_ = (uint32_t)(D.cda8 & 255) << 8;
                rb = (D.cp - (uint32_t)(D.cdb8 >> 8)) & 255u; fb_ = (uint32_t)(D.cdb8 & 255) << 8;
                ca = D.cA[ra] + m16(D.cA[(ra - 1u) & 255u] - D.cA[ra], (int32_t)fa_);
                cb = D.cB[rb] + m16(D.cB[(rb - 1u) & 255u] - D.cB[rb], (int32_t)fb_);
                D.clpA += m16(ca - D.clpA, ONE16 / 5); D.clpB += m16(cb - D.clpB, ONE16 / 5);
                D.cA[D.cp] = y + m16(D.clpA, ONE16 * 3 / 4); D.cB[D.cp] = y + m16(D.clpB, ONE16 * 3 / 4);
                D.cp = (D.cp + 1u) & 255u;
                k = m16(y, ONE16 / 2) + m16(ca + cb, ONE16 * 35 / 100);
                y = lerp16(y, clampi(k, -8 * ONE24, 8 * ONE24), P.formant);
            }
            if (D.eng == E_RUMBLE) { int32_t lp, bp; svf_run(&D.bp700, tanh24(y * 3), &lp, &bp); par = m16(m16(bp, BP700_K16), ONE16 / 10); }
            /* split at 120 Hz (Linkwitz-Riley 4): the first stage here, the low band's second stage at the base rate
             * (its first LP stage is 90 dB down at the OS Nyquist: summing the two samples is all the decimation needs) */
            {
                int32_t l1, b1, h1;
                svf_run(&D.xl1, y, &l1, &b1);
                h1 = y - m30(b1, K_BW_Q30) - l1;
                D.l1sum += l1 >> 1;
                hiS = svf_hp(&D.xh2, h1) + par;
            }
            if (D.md_hpf) hiS = svf_hp(&D.riseHP, hiS);
            if (o) D.rmsIn += (m24(hiS, hiS) - D.rmsIn) >> 13;
            /* BURST on the high band */
            if (D.wet16 > 0) {
                h = hiS + m16(op_hp(&D.hump, hiS), D.hump16);
                hg = clampi(m16(h, D.gain16), -(64 * ONE24), 64 * ONE24);
                if (D.chr16 < ONE16) {
                    int32_t c = clampi(hg, -ONE24, ONE24), sc = m16(c - m24(m24(c, c), c) / 3, 98304);
                    sh = lerp16(m16(sc, 43909), tanh24(hg), D.chr16);
                } else sh = lerp16(tanh24(hg), clampi(hg, -(ONE24 * 72 / 100), ONE24), D.chr16 - ONE16);
                h = m16(lerp16(hiS, m16(sh, 62259), D.wet16), D.comp16b);
            } else h = m16(hiS, D.comp16b);
            B->hos[2u * i + o] = h;
        }
        /* the low band's first stage and the sub, the roll gate, the duck gain: for stage 2 */
        B->l1[i] = D.l1sum; D.l1sum = 0;
        D.phS += D.inc;                                   /* an octave below the OS increment's pitch */
        B->sub[i] = EP(M_SUB) ? m16(m24(m16(m16(sin24(D.phS), EP(M_SUB)), m16(D.sub16, D.md_sub16)), D.amp), ONE16 * 55 / 100) : 0;
        B->rg[i] = m16(D.roll_gate_env, D.gate_env);
        D.duckEnv = m30(D.duckEnv, D.kDuck);
        {
            int32_t ddb = m16(m16(EP(M_DUCK) * 96, D.duckEnv >> 8) + m16(D.duck_extra * 48, D.duckEnv >> 8), ONE16);
            int32_t dg = DUCK_DB_T[clampi(ddb >> 16, 0, 192)];
            B->bg[i] = m16(m16(dg, D.post16), ONE16 * 9 / 10);
            D.duck_extra = m30(D.duck_extra, 1073688000);
        }
    }
    if (mc) {
        if (fm1_mc_wait() < 0) {                    /* core 1 given up (hal/fm1_multicore.h): that block is lost */
#ifndef DINGHY_HOST
            fm1_mc_bar_set();                       /* one core until power-off */
#endif
        }
    } else
        c1_job(D.blk);                              /* one core: stage 2 of this block, now */
    s_abs = D.abs; s_onsets = D.onsets; s_bar = D.bar; s_gstep = D.gstep; s_playing = D.playing; s_rms = S2.rms;
#ifndef DINGHY_HOST
    s_cpu = (fm1_ticks() - t_start) / FM1_TICKS_PER_US;
    s_c1 = S2.us;
#endif
}

static void dsp_init(void)
{
    D.rHP1.G = D.rHP2.G = G_OS_T[CI_120]; D.stHP1.G = D.stHP2.G = G_OS_T[CI_150]; D.gate_env = ONE16;
    D.bp700.a1 = BP700_A1; D.bp700.a2 = BP700_A2; D.bp700.a3 = BP700_A3;
    lad_coef(&D.lc, 300 << 16, 0, G_OS_T[CI_150]); D.lc2 = D.lc;
    D.preHP.G = G_OS_T[CI_20]; D.sqLP.G = G_OS_T[CI_9K]; D.hump.G = G_OS_T[CI_720];
    svf_set(&D.xl1, CI_120); svf_set(&D.xh2, CI_120); svf_set(&D.riseHP, CI_120); svfb_set(&D.cab, 704);
    svfb_set(&S2.xl2, CI_120); svfb_set(&S2.hhp, CI_100); svfb_set(&S2.cab, 704);
    S2.hHP.G = G_BR_T[CI_7K];
    S2.dly[0].G = S2.dly[2].G = G_BR_T[CI_250]; S2.dly[1].G = S2.dly[3].G = G_BR_T[CI_3K2];
    svfb_set(&S2.hHP1, CI_160); S2.hLP.G = G_BR_T[CI_700]; S2.rLP.G = G_BR_T[CI_140];
    S2.noise = 33333; S2.kF8 = 31 << 8; S2.ott_g = S2.ott_g0 = ONE16; S2.kInc = pitch_inc(S2.kF8, 0); S2.corr16 = ONE16; S2.void_mute = ONE16;
    D.void_tgt = ONE16; D.void_lp = 640; D.tape_rate16 = ONE16;
    { int l; for (l = 0; l < NMP; l++) lk[l] = -1; }
    D.roll_gate_env = ONE16; D.roll_gate_on = 1 << 30; D.roll_idx = -1; D.shred_div = 1; D.shred_bits = 16;
    D.key_pend = 99; D.note8 = D.tgt8 = 36 << 8; D.noise = 22222;
    D.step_len16 = (int32_t)((5011u << 16) + (480u << 16) / 1320u);   /* 132 BPM */
    D.dtime16 = (D.step_len16 >> 8) * 3;
}

/* ================================================================== UI (main loop) */
#define C_HEAT RGB(255, 90, 31)
#define C_HEATD RGB(122, 42, 14)
#define C_ACID RGB(198, 255, 31)
#define C_INK RGB(239, 238, 230)
#define C_MUTE RGB(142, 141, 134)
#define C_LN RGB(30, 30, 34)
#define C_DIMC RGB(58, 58, 64)
#define C_CELL RGB(48, 48, 54)
#define C_REST RGB(17, 17, 20)

enum { PG_PLAY, PG_SEQ, PG_VOICE, PG_FX, PG_SYS };
static struct {
    int page, seqmode, preset, pat, oct, dirty_all;
    genome_t g, gB, undo[6]; int nundo, hasB;
    uint32_t salt, next_buf;
    /* settings */
    struct { uint32_t seed; int32_t gen; uint8_t type, dens, slide, acc, root, scale, pad[2]; } slot[8];
    uint32_t key_t[27], key_up_t[27]; uint32_t prev_keys;
    int32_t tapdeg[16]; int ntap, rec_held, rec_used;
    /* throws gestures */
    uint32_t tdown[4], tlast[4]; uint8_t tconsumed[4], tdown_on[4]; int tactive[4]; int latch[NTH]; int combo, combo_a, combo_b;
    uint32_t home_t, home_last; int home_panic;
    int seq_used, edit_used;
    uint32_t enc_t[7];
    /* drawing caches */
    uint32_t drawn_hash[8]; uint32_t last_frame; int drop_flash, acc_flash, roll_flash, beat_flash, msg_t; const char *msg;
    int last_note; uint32_t last_step_ri; uint32_t clk_sent;
    int bank;                             /* the throw buttons' bank: 0 A (RISE ROLL WOW STOP), 1 B (DIVE GATE STACK REV) */
    uint32_t mo_next;                     /* motion: the lane to replace next */
    __typeof__(((genome_t *)0)->mo) mot[8];   /* each slot's motion */
    int chan, sync, stems;                /* MIDI channel - 1; SYNC 0 INT 1 AUTO 2 EXT; USB audio 0 stereo 1 stems */
    int fxl;                              /* the FX page's knobs: 0 SEND FBACK SUB DUCK, 1 PHASR RATE SQUASH HARM */
    int light, light_set; uint32_t cpu_pk, c1_pk, pk_t, sel_t, dice_n;
    int32_t dice_mem[16]; int has_dice;   /* the sound before the last dice */   /* key backlight 0..3; CPU peaks (us a block) */
    int lastcc[8];
    int32_t snap[4];
} U;

static const int THROW_BTN[4] = {BTN_FX, BTN_LFO, BTN_ENV, BTN_ARP};
static const int THROW_OF_AB[2][4] = {{T_RISE, T_ROLL, T_WOW, T_STOP}, {T_DIVE, T_GATE, T_STACK, T_REV}};
#define THROW_OF THROW_OF_AB[U.bank & 1]
static int combo_of(int a, int b)
{
    if (U.bank) return -1;                      /* (bank B: no combos) */
    if (a > b) { int x = a; a = b; b = x; }
    if (a == 0 && b == 1) return T_BUILD;
    if (a == 1 && b == 2) return T_SHRED;
    if (a == 2 && b == 3) return T_FREEZE;
    if (a == 0 && b == 3) return T_VOID;
    return -1;
}

static void publish(int queue)
{
    int i, pick = -1;
    for (i = 0; i < NG; i++) {
        uint32_t c = (U.next_buf + (uint32_t)i) % NG;
        if ((int32_t)c != g_a && (int32_t)c != g_b && (int32_t)c != g_pend) { pick = (int)c; break; }
    }
    if (pick < 0) return;
    U.next_buf = (uint32_t)pick + 1u;
    gbuf[pick] = U.g;
    cmd(OP_GENOME, (uint32_t)pick, queue);
}
static void publish_b(void)
{
    int i;
    for (i = 0; i < NG; i++) {
        uint32_t c = (U.next_buf + (uint32_t)i) % NG;
        if ((int32_t)c != g_a && (int32_t)c != g_pend && (int32_t)c != g_b) { U.next_buf = c + 1u; gbuf[c] = U.gB; cmd(OP_GENOME_B, c, 0); return; }
    }
}
static void push_undo(void)
{
    if (U.nundo == 6) { int k; for (k = 0; k < 5; k++) U.undo[k] = U.undo[k + 1]; U.nundo = 5; }
    U.undo[U.nundo++] = U.g;
    U.gB = U.g; U.hasB = 1; publish_b();
}
static void save_slot(void)
{
    U.mot[U.pat] = U.g.mo;
    U.slot[U.pat].seed = U.g.seed; U.slot[U.pat].gen = U.g.gen; U.slot[U.pat].type = U.g.type; U.slot[U.pat].dens = U.g.dens;
    U.slot[U.pat].slide = U.g.slide; U.slot[U.pat].acc = U.g.acc; U.slot[U.pat].root = U.g.root; U.slot[U.pat].scale = U.g.scale;
}
static void load_slot(int i, int queue)
{
    U.pat = (i + 8) % 8;
    genome_init(&U.g, U.slot[U.pat].seed, U.slot[U.pat].type, U.slot[U.pat].dens, U.slot[U.pat].slide, U.slot[U.pat].acc, U.slot[U.pat].scale);
    if (U.slot[U.pat].gen) { U.g.gen = U.slot[U.pat].gen; generate(&U.g); }
    U.g.root = U.slot[U.pat].root; P.root = U.g.root; P.scale = U.g.scale;
    U.g.mo = U.mot[U.pat];
    publish(queue);
}
static void say(const char *m) { U.msg = m; U.msg_t = (int)fm1_ms; U.dirty_all = 1; }
static void hard_mutate(void)
{
    push_undo();
    U.g.seed = hash32(U.g.seed ^ fm1_ms ^ (U.salt++ * 2654435761u));
    U.g.gen = 0;
    generate(&U.g);
    save_slot(); publish(1); say("MUTATE");
}
static void load_preset(int i)
{
    const preset_t *p = &PRESETS[(i + (int)NPRESET) % (int)NPRESET];
    U.preset = (i + (int)NPRESET) % (int)NPRESET;
    P.cut = p->cut; P.res = p->res; P.env = p->env; P.dec = p->dec; P.acc = p->acc; P.burst = p->burst; P.chr = p->chr;
    P.dly_send = p->dly_send; P.dly_fb = p->dly_fb; P.dly_time = p->dly_time; P.sub = p->sub; P.drive = p->drive;
    P.duck = p->duck; P.fold = p->fold; P.formant = p->formant; P.ratio = p->ratio; P.rel = p->rel; P.eng = p->eng; P.phaser = p->phaser;
    U.snap[0] = P.cut; U.snap[1] = P.res; U.snap[2] = P.env; U.snap[3] = P.burst;
    U.dirty_all = 1;
}

/* the sound dice: a new voice for the engine, inside the ranges that engine sounds good in */
static volatile int32_t *const DICE_P[16] = {&P.cut, &P.res, &P.env, &P.dec, &P.acc, &P.burst, &P.chr, &P.drive, &P.sub,
                                              &P.fold, &P.formant, &P.ratio, &P.rel, &P.glide, &P.dly_send, &P.eng};
static void dice(void)
{
    static const uint8_t RNG[NENG][9][2] = {   /* cut res env dec acc burst chr drive sub, in percent */
        {{20, 60}, {45, 90}, {30, 85}, {15, 65}, {40, 100}, {10, 85}, {0, 100}, {0, 40}, {0, 30}},
        {{8, 35}, {10, 40}, {10, 45}, {30, 70}, {30, 70}, {20, 70}, {40, 95}, {30, 80}, {20, 60}},
        {{30, 70}, {5, 35}, {20, 60}, {25, 65}, {40, 70}, {35, 95}, {25, 95}, {0, 30}, {30, 60}},
        {{35, 75}, {20, 60}, {25, 65}, {20, 50}, {40, 80}, {40, 90}, {35, 95}, {0, 30}, {0, 30}},
        {{25, 60}, {75, 97}, {40, 85}, {20, 60}, {60, 100}, {50, 95}, {20, 90}, {0, 25}, {0, 20}},
    };
    uint32_t h = hash32(fm1_ms * 2654435761u ^ (U.dice_n++ * 40503u)), k, e = (uint32_t)clampi(P.eng, 0, NENG - 1);
    for (k = 0; k < 16; k++) U.dice_mem[k] = *DICE_P[k];
    U.has_dice = 1;
    for (k = 0; k < 9; k++) {
        uint32_t lo = RNG[e][k][0], hi = RNG[e][k][1], r;
        h = hash32(h + k * 0x9E3779B9u); r = h >> 16;
        *DICE_P[k] = clampi((int32_t)((lo * 65536u + (hi - lo) * r) / 100u), 0, 65535);
    }
    if (e == E_ACID) { h = hash32(h); P.chr = (h & 3u) == 0 ? ONE16 - 1 : (h & 3u) == 1 ? P.chr : 0; }   /* saw or square, mostly */
    h = hash32(h); P.fold = e == E_GROWL ? (int32_t)((h >> 16) * 6 / 10) : 0;
    h = hash32(h); P.formant = e == E_GROWL ? ONE16 * 3 / 10 + (int32_t)((h >> 16) * 6 / 10) : 0;
    h = hash32(h); P.ratio = e == E_GROWL ? 1 + (int32_t)(h >> 30 > 2 ? 2 : h >> 30) : 2;
    P.rel = e == E_REESE ? 80 : e == E_RUMBLE ? 30 : e == E_GROWL ? 20 : 8;
    U.snap[0] = P.cut; U.snap[1] = P.res; U.snap[2] = P.env; U.snap[3] = P.burst;
    say("DICE");
}
static void dice_undo(void)
{
    uint32_t k;
    if (!U.has_dice) return;
    for (k = 0; k < 16; k++) *DICE_P[k] = U.dice_mem[k];
    U.has_dice = 0;
    say("UNDO DICE");
}

/* settings: the sound + 8 pattern DNAs (regenerated on boot) */
typedef struct {
    uint8_t ver, preset, pat, drums;
    uint16_t cut, res, env, dec, acc, burst, wave, glide, sub, drive, dly_send, dly_fb, duck, vol;
    uint8_t dly_time, lock, pad[2];
    uint16_t bpm10, swing;
    struct { uint32_t seed; int32_t gen; uint8_t type, dens, slide, acc, root, scale, pad[2]; } slot[8];
    /* v4 (0.2) */
    uint8_t eng, ratio, rel, light;
    uint16_t fold, formant, harm, pad4;
    /* v5 (0.3) */
    uint16_t phaser, ph_rate, squash, chan;          /* chan: MIDI channel - 1 | sync << 4 | usb stems << 6 */
    struct { uint8_t id[4]; uint16_t v[4][32]; } mo[8];   /* each slot's motion */
} save_t;
#define SAVE_V4_N ((uint32_t)__builtin_offsetof(save_t, phaser))
#define SAVE_V3_N ((uint32_t)__builtin_offsetof(save_t, eng))   /* 0.1's settings: the same fields, up to here */
static save_t SV __attribute__((section(".lowbuf")));
static void settings_store(void)
{
    crumb('W', 0, 0);
    SV.phaser = (uint16_t)clampi(P.phaser, 0, 65535); SV.ph_rate = (uint16_t)clampi(P.ph_rate, 0, 65535); SV.squash = (uint16_t)clampi(P.squash, 0, 65535);
    SV.chan = (uint16_t)((U.chan & 15) | (U.sync & 3) << 4 | (U.stems & 1) << 6);
    SV.ver = 5; SV.eng = (uint8_t)P.eng; SV.ratio = (uint8_t)P.ratio; SV.rel = (uint8_t)P.rel; SV.light = (uint8_t)(U.light + 1);
    SV.fold = (uint16_t)clampi(P.fold, 0, 65535); SV.formant = (uint16_t)clampi(P.formant, 0, 65535); SV.harm = (uint16_t)clampi(P.harm, 0, 65535);
    SV.preset = (uint8_t)U.preset; SV.pat = (uint8_t)U.pat; SV.drums = (uint8_t)P.drums;
    SV.cut = (uint16_t)clampi(P.cut, 0, 65535); SV.res = (uint16_t)clampi(P.res, 0, 65535); SV.env = (uint16_t)clampi(P.env, 0, 65535);
    SV.dec = (uint16_t)clampi(P.dec, 0, 65535); SV.acc = (uint16_t)clampi(P.acc, 0, 65535); SV.burst = (uint16_t)clampi(P.burst, 0, 65535);
    SV.wave = (uint16_t)clampi(P.chr, 0, 65535); SV.glide = (uint16_t)clampi(P.glide, 0, 65535); SV.sub = (uint16_t)clampi(P.sub, 0, 65535);
    SV.drive = (uint16_t)clampi(P.drive, 0, 65535); SV.dly_send = (uint16_t)clampi(P.dly_send, 0, 65535); SV.dly_fb = (uint16_t)clampi(P.dly_fb, 0, 65535);
    SV.duck = (uint16_t)clampi(P.duck, 0, 65535); SV.vol = (uint16_t)clampi(P.vol, 0, 65535);
    SV.dly_time = (uint8_t)P.dly_time; SV.lock = (uint8_t)P.lock; SV.bpm10 = (uint16_t)P.bpm10; SV.swing = (uint16_t)P.swing;
    save_slot();
    memcpy(SV.slot, U.slot, sizeof SV.slot);
    memcpy(SV.mo, U.mot, sizeof SV.mo);
    if (settings_save(&SV, sizeof SV) == 0) say("SAVED"); else say("SAVE FAILED");
}

/* ---------------------------------------------------------------- drawing */
static void glyph_draw(const uint32_t *bits, int32_t sz, int32_t x, int32_t y, uint16_t c)
{
    int32_t r, k;
    for (r = 0; r < sz; r++) for (k = 0; k < sz; k++) if (bits[r] >> (sz - 1 - k) & 1u) cv_pset(x + k, y + r, c);
}
static void num_str(char *s, int32_t v)
{
    char t[12]; int n = 0, i = 0;
    if (v < 0) { s[i++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) s[i++] = t[--n];
    s[i] = 0;
}
static void top_bar(void)
{
    char b[8];
    cv_begin(240, 24, C_BLACK);
    glyph_draw(GLYPH_LIE16, 16, 6, 4, C_HEAT);
    cv_text(26, 4, &FONT_S, "RIFT", C_HEAT);
    cv_rect(70, 4, 72, 17, U.page == PG_SEQ ? C_HEAT : C_ACID);
    cv_text(74, 4, &FONT_S, U.page == PG_SEQ ? (U.seqmode ? "SEQ PERF" : "SEQ GEN") : U.page == PG_VOICE ? "VOICE" : U.page == PG_FX ? (U.fxl ? "FX 2" : "FX") : U.page == PG_SYS ? "SYS" : ENG_NAME[clampi(P.eng, 0, NENG - 1)], C_BLACK);
    if (U.bank) { cv_rect(146, 4, 14, 17, C_ACID); cv_text(149, 4, &FONT_S, "B", C_BLACK); }
    num_str(b, s_ext ? (s_bpm_ext + 5) / 10 : P.bpm10 / 10);
    cv_text(196 - text_w(&FONT_S, b), 4, &FONT_S, b, s_ext ? C_ACID : C_INK);
    cv_rect(0, 23, 240, 1, C_LN);
    cv_blit(0, 0);
}
static void dot(void)
{
    cv_begin(24, 20, C_BLACK);
    cv_rect(8, 6, 8, 8, s_playing ? (U.beat_flash ? C_ACID : RGB(77, 107, 12)) : C_DIMC);
    cv_blit(210, 2);
}
static int32_t scope_trigger(void)
{
    uint32_t w = scope_w, k;
    for (k = 500; k > 260; k--) { int16_t a = scope[(w - k - 1u) & 511u], b = scope[(w - k) & 511u]; if (a < 0 && b >= 0) return (int32_t)(w - k); }
    return (int32_t)(w - 300u);
}
static void draw_scope(void)
{
    int32_t st = scope_trigger(), x, prev = 0, pk = 400, i, frac = s_frac, dy = 0, hold = 1, flip = 0;
    uint16_t col = C_HEAT;
    int thr = -1, t;
    for (t = 0; t < NTH; t++) if (s_th[t]) thr = t;
    cv_begin(240, 122, C_BLACK);
    cv_rect(60, 0, 1, 122, C_LN); cv_rect(120, 0, 1, 122, C_LN); cv_rect(180, 0, 1, 122, C_LN); cv_rect(0, 61, 240, 1, C_LN);
    for (i = 0; i < 240; i++) { int32_t v = iabs(scope[(uint32_t)(st + i) & 511u]); if (v > pk) pk = v; }
    if (s_th[T_RISE] >= 2 || s_th[T_BUILD] >= 2) dy = -(s_prog * 25 >> 16);
    if (s_th[T_STOP] >= 2) { dy = (ONE16 - s_rate) * 70 >> 16; if (s_rate < ONE16 * 4 / 10) col = C_HEATD; }
    if (s_th[T_SHRED] >= 2) hold = 2 + (U.roll_flash & 3);
    if (s_dive) dy -= clampi(s_dive / 64, 0, 40);
    if (s_th[T_GATE] >= 2 && s_gate < ONE16 * 8 / 10) col = C_HEATD;
    flip = s_th[T_REV] >= 2;
    if (s_stack > ONE16 / 50) {   /* STACK: two ghosts above */
        int32_t gk, gy;
        for (gk = 1; gk <= 2; gk++)
            for (x = 0; x < 240; x += 2) {
                int32_t v = scope[(uint32_t)(st + x) & 511u];
                gy = 61 + dy - gk * (17 * s_stack >> 16) - (v * 50 / pk);
                if (gy >= 0 && gy < 122) cv_pset(x, gy, C_ACID);
            }
    }
    for (x = 0; x < 240; x++) {
        int32_t k = hold > 1 ? x - x % (hold * 4) : x, v = scope[(uint32_t)(st + (flip ? 239 - k : k)) & 511u];
        int32_t y = 61 + dy - (v * 50 / pk);
        y = clampi(y, 0, 121);
        if (x) { cv_line(x - 1, prev, x, y, col); cv_line(x - 1, prev + 1, x, y + 1, col); }
        prev = y;
    }
    if ((s_th[T_ROLL] >= 2 || s_th[T_BUILD] >= 2) && U.roll_flash > 0)
        for (x = 6; x < 240; x += 3) { int32_t v = scope[(uint32_t)(st + x - 6) & 511u]; cv_pset(x, clampi(61 + dy - (v * 50 / pk), 0, 121), C_HEATD); }
    for (i = 0; i < NCRACK; i++) {
        const uint8_t *c = &CRACK_T[i * 6];
        if ((int32_t)c[4] * ONE16 / 255 < frac * 115 / 100) {
            uint16_t cc = U.acc_flash ? RGB(230, 255, 150) : C_ACID;
            cv_line(c[0], c[1], c[2], c[3], cc);
            if (c[5] > 1) cv_line(c[0] + 1, c[1], c[2] + 1, c[3], cc);
        }
    }
    if (s_th[T_RISE] >= 2 || s_th[T_BUILD] >= 2) {
        int32_t wy = 121 - (s_prog * 121 >> 16);
        cv_rect(0, wy, 240, 1, C_ACID);
        for (i = 0; i < 16; i++) cv_rect(8 + i * 14, 3, 9, 2, i < (s_prog * 16 >> 16) ? C_ACID : C_DIMC);
    }
    if (s_th[T_WOW] >= 2) { int32_t h = clampi(s_acccap >> 18, 0, 121); cv_rect(0, 121 - h, 4, h, C_HEAT); }
    if (thr >= 0) {
        int32_t w = text_w(&FONT_S, TH_NAME[thr]) + 10;
        cv_rect(234 - w, 6, w, 18, s_th[thr] == 1 ? C_HEATD : C_HEAT);
        cv_text(239 - w, 7, &FONT_S, TH_NAME[thr], C_BLACK);
    } else if (U.rec_held) {
        cv_text(6, 4, &FONT_S, "REC TAP", C_HEAT);
    } else if (U.msg && (int)fm1_ms - U.msg_t < 1200) {
        cv_text(6, 4, &FONT_S, U.msg, C_ACID);
    } else {
        char b[12] = "FRAC ";
        num_str(b + 5, frac * 100 >> 16);
        cv_text(236 - text_w(&FONT_S, b), 102, &FONT_S, b, C_ACID);
    }
    if (s_th[T_VOID] >= 2) {
        cv_rect(0, 0, 240, 52, C_BLACK); cv_rect(0, 70, 240, 52, C_BLACK);
        cv_rect(186, 6, 50, 18, C_HEAT); cv_text(191, 7, &FONT_S, "VOID", C_BLACK);
        cv_text(6, 100, &FONT_S, "DROP ON THE BAR", C_HEAT);
    }
    if (U.drop_flash) { uint32_t k; for (k = 0; k < cv_w * cv_h; k++) cv_px[k] = (uint16_t)~cv_px[k]; U.drop_flash--; }
    cv_blit(0, 26);
}
static void draw_strip(void)
{
    const genome_t *g = &U.g;
    const lane_t *R = &g->ln[L_RHY];
    uint32_t cur = s_playing ? lane_index(R, s_abs, g->seed, SALT[L_RHY]) : 99u, base = cur != 99u ? cur / 16u * 16u : 0u, i;
    cv_begin(240, 24, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LN);
    for (i = 0; i < 16u; i++) {
        uint32_t idx = base + i; int32_t x = 6 + (int32_t)i * 14, on = idx < R->len, v = on ? R->v[idx] : REST;
        int32_t acc = on && g->ln[L_ACC].v[idx], sld = on && g->ln[L_SLD].v[idx];
        uint16_t c = !on ? RGB(8, 8, 10) : v == NOTE ? (acc ? C_HEAT : C_CELL) : v == TIE ? RGB(29, 29, 33) : C_REST;
        cv_rect(x, 5, 12, 14, c);
        if (v == TIE && on) cv_rect(x - 2, 10, 14, 4, C_CELL);
        if (sld) cv_rect(x, 19, 12, 2, C_ACID);
        if (idx == cur) { cv_rect(x, 4, 12, 1, C_INK); cv_rect(x, 19, 12, 1, C_INK); cv_rect(x, 4, 1, 16, C_INK); cv_rect(x + 11, 4, 1, 16, C_INK); }
    }
    cv_blit(0, 150);
}
typedef struct { const char *label; int32_t v16; int disp; int hot, mo; const char *txt; } knob_t;
static int knob_page(knob_t *k);
static void draw_macros(void)
{
    knob_t k[4]; int i;
    char b[12];
    knob_page(k);
    cv_begin(240, 64, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LN);
    for (i = 0; i < 4; i++) {
        int32_t x = i * 60;
        uint16_t fg = k[i].hot ? C_BLACK : C_INK;
        if (k[i].hot) cv_rect(x, 1, 60, 63, C_HEAT); else if (i) cv_rect(x, 0, 1, 64, C_LN);
        if (k[i].label[0] == '*') { glyph_draw(GLYPH_BAO16, 16, x + 6, 5, C_BLACK); cv_text(x + 24, 5, &FONT_S, k[i].label + 1, C_BLACK); }
        else cv_text(x + 6, 5, &FONT_S, k[i].label, k[i].hot ? C_BLACK : C_MUTE);
        num_str(b, k[i].disp);
        if (k[i].txt) cv_text(x + 6, 30, &FONT_S, k[i].txt, fg); else cv_text(x + 6, 24, &FONT_L, b, fg);
        if (k[i].mo) { cv_rect(x + 50, 6, 6, 6, C_ACID); }   /* motion recorded on this knob */
        cv_rect(x + 6, 58, 48, 3, k[i].hot ? C_HEATD : C_LN);
        cv_rect(x + 6, 58, clampi(m16(48, k[i].v16), 0, 48), 3, fg);
    }
    cv_blit(0, 176);
}
static void draw_seq_page(void)
{
    const genome_t *g = &U.g;
    int l; char b[24];
    uint32_t i;
    cv_begin(240, 124, C_BLACK);
    {   /* DNA line */
        uint32_t hi = g->seed >> 16, lo = g->seed & 0xFFFFu; int p = 0, s;
        const char *hx = "0123456789ABCDEF";
        b[p++] = 'D'; b[p++] = 'N'; b[p++] = 'A'; b[p++] = ' ';
        for (s = 12; s >= 0; s -= 4) b[p++] = hx[(hi >> s) & 15u];
        b[p++] = '-';
        for (s = 12; s >= 0; s -= 4) b[p++] = hx[(lo >> s) & 15u];
        b[p] = 0;
        cv_text(4, 0, &FONT_S, b, C_ACID);
        cv_text(236 - text_w(&FONT_S, SC_NAME[g->scale]), 0, &FONT_S, SC_NAME[g->scale], C_MUTE);
    }
    for (l = 0; l < NL; l++) {
        const lane_t *L = &g->ln[l];
        int32_t y = 18 + l * 13, len = (l == L_ACC || l == L_SLD) && L->link ? g->ln[L_RHY].len : L->len;
        uint32_t c = l == L_PIT || l == L_OCT ? s_onsets : l == L_TRN ? s_bar : s_abs;
        uint32_t cur = s_playing ? lane_index((l == L_ACC || l == L_SLD) && L->link ? &g->ln[L_RHY] : L, c, g->seed, SALT[l]) : 99u;
        if (L->lock) { cv_rect(1, y, 30, 12, C_ACID); cv_text(3, y - 3, &FONT_S, LANE_NAME[l], C_BLACK); }
        else cv_text(3, y - 3, &FONT_S, LANE_NAME[l], C_MUTE);
        for (i = 0; i < 16u; i++) {
            uint32_t idx = (cur != 99u && cur >= 16u ? 16u : 0u) + i; int32_t x = 36 + (int32_t)i * 12, v = L->v[idx];
            uint16_t c2 = idx < (uint32_t)len ? RGB(28, 28, 32) : RGB(7, 7, 8);
            if (idx < (uint32_t)len) {
                if (l == L_RHY) c2 = v == NOTE ? RGB(217, 216, 207) : v == TIE ? RGB(94, 93, 88) : RGB(28, 28, 32);
                if (l == L_ACC && v) c2 = C_HEAT;
                if (l == L_SLD && v) c2 = C_ACID;
                if ((l == L_PIT || l == L_TRN) && v) c2 = v > 0 ? RGB(80 + v * 10, 80 + v * 10, 70) : RGB(60, 60, 110 - v * 6);
                if (l == L_OCT && v) c2 = v > 0 ? C_INK : RGB(90, 90, 160);
                if (l == L_MOD) c2 = v > 0 ? RGB(clampi(60 + v * 3, 0, 255), 60, 30) : RGB(30, 40, clampi(60 - v * 3, 0, 255));
                if (l == L_CHN && v) c2 = RGB(60, clampi(255 - v * 30, 0, 255), 30);
            }
            cv_rect(x, y, 10, 10, c2);
            if (idx == cur) { cv_rect(x, y, 10, 1, C_INK); cv_rect(x, y + 9, 10, 1, C_INK); }
        }
        num_str(b, len);
        cv_text(236 - text_w(&FONT_S, b), y - 3, &FONT_S, b, len != g->ln[L_RHY].len && !((l == L_ACC || l == L_SLD) && L->link) ? C_HEAT : C_MUTE);
    }
    cv_blit(0, 26);
}
static void draw_info_page(void)
{
    char b[24];
    cv_begin(240, 124, C_BLACK);
    if (U.page == PG_VOICE) {
        int32_t x, prev = 0, fci = s_fcidx;
        cv_text(4, 0, &FONT_S, PRESETS[U.preset].name, C_ACID);
        cv_text(170, 0, &FONT_S, ENG_NAME[clampi(P.eng, 0, NENG - 1)], C_MUTE);
        for (x = 0; x < 240; x += 2) {   /* the ladder's response, drawn from the cutoff index (approximate) */
            int32_t c = x * 640 / 240, d = c - fci, db = d < 0 ? 0 : -(d * 24 / 64), pk = P.res * 12 >> 16, y;
            if (iabs(d) < 16) db += pk * (16 - iabs(d)) / 16;
            y = clampi(60 - db * 2, 20, 120);
            if (x) cv_line(x - 2, prev, x, y, C_ACID);
            prev = y;
        }
        cv_rect(clampi(fci * 240 / 640, 0, 239), 20, 1, 100, C_ACID);
        {
            static const char *const ED[NENG] = {"DIODE LADDER 2X", "SINE + DIFFUSER", "3 SAWS DETUNED", "FM + FORMANTS", "TWIN LADDER"};
            cv_text(4, 104, &FONT_S, ED[clampi(P.eng, 0, NENG - 1)], C_MUTE);
            cv_text(150, 104, &FONT_S, "SEL DICE", C_DIMC);
        }
    } else if (U.page == PG_SYS) {
        cv_text(4, 0, &FONT_S, "CLOCK", C_MUTE);
        if (s_ext) { num_str(b, (s_bpm_ext + 5) / 10); cv_text(70, 0, &FONT_S, "FOLLOWING", C_ACID); cv_text(190, 0, &FONT_S, b, C_ACID); }
        else cv_text(70, 0, &FONT_S, U.sync == 2 ? "WAITING" : "OWN TEMPO", U.sync == 2 ? C_HEAT : C_INK);
        cv_text(4, 40, &FONT_S, "NOTES CC: CH", C_MUTE); num_str(b, U.chan + 1); cv_text(120, 40, &FONT_S, b, C_INK);
        cv_text(4, 60, &FONT_S, "SEQ+SAVE  SEND DNA", C_MUTE);
        cv_text(4, 80, &FONT_S, U.stems ? "USB AUDIO 8 CH STEMS" : "USB AUDIO STEREO", C_MUTE);
#ifndef DINGHY_HOST
        if (fm1_crash.magic == FM1_CRASH_MAGIC) {      /* the last crash since power-on: PC, and when after its boot */
            hex8(b, fm1_crash.pc); cv_text(4, 20, &FONT_S, "CRASH", C_HEAT); cv_text(60, 20, &FONT_S, b, C_HEAT);
            num_str(b, (int32_t)(fm1_crash.uptime_ms / 1000u)); cv_text(150, 20, &FONT_S, b, C_HEAT); cv_text(190, 20, &FONT_S, "S", C_HEAT);
        } else
#endif
        cv_text(4, 20, &FONT_S, "IN: USB + TRS JACK", C_MUTE);
        cv_text(4, 104, &FONT_S, mc_status(), C_DIMC);
    } else {
        int t;
        cv_text(4, 0, &FONT_S, "DELAY", C_MUTE);
        cv_text(64, 0, &FONT_S, DLY_NAMES[clampi(P.dly_time, 0, 4)], C_ACID);
        for (t = 0; t < NTH; t++) {
            int32_t x = 4 + (t % 4) * 59, y = 20 + (t / 4) * 27;
            uint16_t on = t >= T_DIVE ? C_ACID : C_HEAT;
            cv_rect(x, y, 55, 24, s_th[t] >= 2 ? on : s_th[t] ? C_HEATD : RGB(22, 22, 26));
            cv_text(x + 4, y + 4, &FONT_S, TH_NAME[t], s_th[t] >= 2 ? C_BLACK : t >= T_DIVE && !U.bank ? C_DIMC : C_INK);
        }
        {   /* the load: core 0 (stage 1 and the wait), core 1 (stage 2), peaks over 2 s */
            uint32_t c0 = s_cpu, c1 = s_c1;
            if (c0 > U.cpu_pk) U.cpu_pk = c0;
            if (c1 > U.c1_pk) U.c1_pk = c1;
            if (fm1_ms - U.pk_t > 2000u) { U.pk_t = fm1_ms; U.cpu_pk = c0; U.c1_pk = c1; }
            cv_text(112, 0, &FONT_S, mc_status(), C_MUTE);
            cv_text(4, 104, &FONT_S, "CPU", C_MUTE);
            num_str(b, (int32_t)(c0 * 100u / 5805u)); cv_text(40, 104, &FONT_S, b, U.cpu_pk > 4600u ? C_HEAT : C_ACID);
            num_str(b, (int32_t)(U.cpu_pk * 100u / 5805u)); cv_text(70, 104, &FONT_S, b, C_MUTE);
            cv_text(104, 104, &FONT_S, "C1", C_MUTE);
            num_str(b, (int32_t)(c1 * 100u / 5805u)); cv_text(128, 104, &FONT_S, b, U.c1_pk > 4600u ? C_HEAT : C_ACID);
            num_str(b, (int32_t)(U.c1_pk * 100u / 5805u)); cv_text(158, 104, &FONT_S, b, C_MUTE);
        }
    }
    (void)b;
    cv_blit(0, 26);
}

/* ---------------------------------------------------------------- knobs per page */
static int held_throw(void)
{
    int i;
    if (U.combo >= 0) return U.combo;
    for (i = 0; i < 4; i++) if (U.tdown_on[i] && U.tactive[i] >= 0) return U.tactive[i];
    return -1;
}
static volatile int32_t *knob_ptr(int i, int *kind)
{
    int ht = held_throw();
    *kind = 0;
    if (ht >= 0) return &P.tp[ht][i];
    switch (U.page) {
    case PG_SEQ:
        if (U.seqmode) { volatile int32_t *p[4] = {&P.tension, &P.lock, &P.xfade, &P.swing}; *kind = i == 1 ? 2 : i == 2 ? 3 : 0; return p[i]; }
        *kind = 1; return 0;
    case PG_VOICE: { volatile int32_t *p[4] = {&P.dec, &P.acc, &P.chr, &P.drive}; return p[i]; }
    case PG_FX:
        if (U.fxl) { volatile int32_t *p[4] = {&P.phaser, &P.ph_rate, &P.squash, &P.harm}; return p[i]; }
        { volatile int32_t *p[4] = {&P.dly_send, &P.dly_fb, &P.sub, &P.duck}; return p[i]; }
    case PG_SYS: *kind = 4; return 0;
    default: { volatile int32_t *p[4] = {&P.cut, &P.res, &P.env, &P.burst}; return p[i]; }
    }
}
static int knob_page(knob_t *k)
{
    static const char *const LB[4][4] = {{"CUT", "RES", "ENV", "*BRST"}, {"TYPE", "DENS", "SLIDE", "ACC"}, {"DECAY", "ACCNT", "WAVE", "DRIVE"}, {"SEND", "FBACK", "SUB", "DUCK"}};
    static const char *const LB2[4] = {"TENS", "LOCK", "XFADE", "SWING"};
    static const char *const TL[NTH][4] = {{"PHRS", "DEPTH", "RES", "FB"}, {"RATE", "GATE", "-", "-"}, {"-", "DRIVE", "-", "-"}, {"TIME", "-", "-", "-"},
                                           {"PHRS", "-", "-", "-"}, {"-", "-", "PATT", "-"}, {"LEN", "LOOP", "LIVE", "FADE"}, {"-", "-", "-", "TAIL"},
                                           {"TIME", "DEPTH", "-", "-"}, {"-", "DEPTH", "PATT", "-"}, {"-", "AMT", "-", "-"}, {"-", "-", "-", "-"}};
    int i, ht = held_throw();
    for (i = 0; i < 4; i++) {
        int kind; volatile int32_t *p = knob_ptr(i, &kind);
        k[i].hot = 0; k[i].mo = 0; k[i].txt = 0;
        if (ht >= 0) { k[i].label = TL[ht][i]; k[i].v16 = *p; k[i].disp = *p * 99 >> 16; k[i].hot = 1; continue; }
        k[i].mo = p && ht < 0 && mp_of(p) >= 0 && mo_lane_of(&U.g, mp_of(p)) >= 0;
        if (U.page == PG_VOICE && i == 2) { k[i].label = CHR_NAME[clampi(P.eng, 0, NENG - 1)]; k[i].v16 = *p; k[i].disp = *p * 99 >> 16; continue; }
        if (U.page == PG_SYS) {
            static const char *const SL[4] = {"SYNC", "CH", "USB", "LIGHT"}, *const SY[3] = {"INT", "AUTO", "EXT"};
            static const char *const UA[2] = {"STEREO", "STEMS"}, *const LT[4] = {"OFF", "LOW", "MID", "HIGH"};
            k[i].label = SL[i];
            if (i == 0) { k[i].txt = SY[U.sync % 3]; k[i].v16 = U.sync * 32767; }
            else if (i == 1) { k[i].disp = U.chan + 1; k[i].v16 = U.chan * 4369; }
            else if (i == 2) { k[i].txt = UA[U.stems & 1]; k[i].v16 = U.stems ? ONE16 - 1 : 0; }
            else { k[i].txt = LT[U.light & 3]; k[i].v16 = U.light * 21845; }
            continue;
        }
        if (U.page == PG_SEQ && !U.seqmode) {
            uint8_t v = i == 0 ? U.g.type : i == 1 ? U.g.dens : i == 2 ? U.g.slide : U.g.acc;
            k[i].label = LB[1][i]; k[i].v16 = v * 257; k[i].disp = v; continue;
        }
        if (U.page == PG_SEQ) { k[i].label = LB2[i]; k[i].v16 = kind == 2 ? *p * ONE16 / 127 : kind == 3 ? *p * 257 : i == 3 ? *p * 2 : *p; k[i].disp = kind ? *p : (i == 3 ? *p * 100 >> 16 : *p * 99 >> 16); continue; }
        k[i].label = LB[U.page == PG_VOICE ? 2 : U.page == PG_FX ? 3 : 0][i];
        if (U.page == PG_FX && U.fxl) { static const char *const L2[4] = {"PHASR", "RATE", "SQUASH", "HARM"}; k[i].label = L2[i]; }
        k[i].v16 = *p; k[i].disp = *p * 99 >> 16; k[i].hot = U.page == PG_PLAY && i == 3;
    }
    return 4;
}
/* motion: REC held while playing, a knob turned -> its value into the step playing, in one of the pattern's 4 lanes */
static void motion_write(volatile int32_t *p)
{
    int m = mp_of(p), l;
    uint32_t k;
    if (m < 0) return;
    l = mo_lane_of(&U.g, m);
    if (l < 0) {
        for (l = 0; l < 4 && U.g.mo.id[l]; l++) ;
        if (l == 4) l = (int)(U.mo_next++ & 3u);   /* (all four taken: the oldest goes) */
        U.g.mo.id[l] = (uint8_t)(m + 1);
        for (k = 0; k < 32u; k++) U.g.mo.v[l][k] = 0xFFFFu;
    }
    U.g.mo.v[l][U.last_step_ri & 31u] = (uint16_t)clampi(*p, 0, 65534);
    U.rec_used = 1;
    publish(0);
}
static void knob_turn(int i, int32_t det)
{
    int kind; volatile int32_t *p = knob_ptr(i, &kind);
    if (kind == 4) {   /* SYS: a detent, a choice */
        int dd = det > 0 ? 1 : -1;
        if (i == 0) { U.sync = clampi(U.sync + dd, 0, 2); P.sync = U.sync; }
        else if (i == 1) U.chan = clampi(U.chan + dd, 0, 15);
        else if (i == 2) { int v = clampi(U.stems + dd, 0, 1); if (v != U.stems) { U.stems = v; usb_stems_set(v); } }
        else U.light = clampi(U.light + dd, 0, 3);
        return;
    }
    if (U.page == PG_SEQ && !U.seqmode && held_throw() < 0) {
        uint8_t *v = i == 0 ? &U.g.type : i == 1 ? &U.g.dens : i == 2 ? &U.g.slide : &U.g.acc;
        *v = (uint8_t)clampi(*v + det * 4, 0, 255);
        generate(&U.g); save_slot(); publish(0); return;
    }
    if (kind == 2) { *p = clampi(*p + det, 0, 127); return; }
    if (kind == 3) { *p = clampi(*p + det * 4, 0, 255); if (*p && !U.hasB) { U.gB = U.g; U.hasB = 1; publish_b(); } return; }
    if (U.page == PG_SEQ && i == 3 && held_throw() < 0) { *p = clampi(*p + det * 512, 0, ONE16 / 2); return; }
    *p = clampi(*p + det * 1024, 0, ONE16);
    if (U.rec_held && s_playing && held_throw() < 0) motion_write(p);
}

/* ---------------------------------------------------------------- input */
static void throw_down(int b)
{
    int other = -1, i, name = THROW_OF[b], dbl;
    uint32_t now = fm1_ms;
    U.tdown[b] = now; U.tdown_on[b] = 1;
    for (i = 0; i < 4; i++) if (i != b && U.tdown_on[i] && !U.tconsumed[i]) other = i;
    if (other >= 0) {
        int c = combo_of(other, b);
        if (c >= 0) {
            if (U.tactive[other] >= 0 && !U.latch[U.tactive[other]]) cmd(OP_ABORT, (uint32_t)U.tactive[other], 0);
            U.tactive[other] = -1;
            dbl = U.tlast[b] && now - U.tlast[b] < 260u;
            cmd(OP_ENGAGE, (uint32_t)c, dbl); if (dbl) U.latch[c] = 1;
            U.combo = c; U.combo_a = other; U.combo_b = b; U.tconsumed[other] = U.tconsumed[b] = 1;
            return;
        }
    }
    for (i = T_BUILD; i < NTH; i++)
        if (U.latch[i]) {
            int a = -1, c2 = -1, j, k2;
            for (j = 0; j < 4; j++) for (k2 = 0; k2 < 4; k2++) if (j < k2 && combo_of(j, k2) == i) { a = j; c2 = k2; }
            if (b == a || b == c2) { U.latch[i] = 0; cmd(OP_RELEASE, (uint32_t)i, 0); U.tconsumed[b] = 1; return; }
        }
    if (U.latch[name]) { U.latch[name] = 0; cmd(OP_RELEASE, (uint32_t)name, 0); U.tconsumed[b] = 1; return; }
    dbl = U.tlast[b] && now - U.tlast[b] < 260u;
    cmd(OP_ENGAGE, (uint32_t)name, dbl); U.tactive[b] = name; if (dbl) U.latch[name] = 1;
}
static void throw_up(int b)
{
    uint32_t now = fm1_ms;
    U.tlast[b] = now - U.tdown[b] < 260u ? now : 0u;
    U.tdown_on[b] = 0;
    if (U.tconsumed[b]) {
        U.tconsumed[b] = 0;
        if (U.combo >= 0 && (U.combo_a == b || U.combo_b == b)) { if (!U.latch[U.combo]) cmd(OP_RELEASE, (uint32_t)U.combo, 0); U.combo = -1; }
        return;
    }
    if (U.tactive[b] >= 0 && !U.latch[U.tactive[b]]) cmd(OP_RELEASE, (uint32_t)U.tactive[b], 0);
    U.tactive[b] = -1;
}
static void apply_tap(void)
{
    int i;
    if (!U.ntap) return;
    push_undo();
    for (i = 0; i < U.ntap && i < 16; i++) { U.g.ln[L_PIT].v[i] = (int8_t)clampi(U.tapdeg[i], -7, 14); U.g.ln[L_OCT].v[i] = 0; }
    U.g.ln[L_PIT].len = U.g.ln[L_OCT].len = (uint8_t)(U.ntap > 16 ? 16 : U.ntap);
    U.g.ln[L_PIT].lock = 1;
    publish(0); say("TAP PITCH");
}
/* DNA over MIDI: the pattern (its lanes, ratchets, gates and motion) as SysEx, F0 7D 'R' 'I' 'F' 'T' 1, then the
 * genome 7 bits at a time (a byte of high bits before each 7), a checksum, F7. Any unit on the same firmware takes it
 * into the slot playing, at the next bar */
static void dna_send(void)
{
    static uint8_t f[8 + (sizeof(genome_t) + 6u) / 7u * 8u + 2u];
    const uint8_t *g = (const uint8_t *)&U.g;
    uint32_t n = 0, i, k, sum = 0;
    f[n++] = 0xF0; f[n++] = 0x7D; f[n++] = 'R'; f[n++] = 'I'; f[n++] = 'F'; f[n++] = 'T'; f[n++] = 1;
    for (i = 0; i < sizeof(genome_t); i += 7u) {
        uint32_t hi = 0, m = sizeof(genome_t) - i < 7u ? sizeof(genome_t) - i : 7u;
        for (k = 0; k < m; k++) hi |= (uint32_t)(g[i + k] >> 7) << k;
        f[n++] = (uint8_t)hi;
        for (k = 0; k < m; k++) { f[n++] = g[i + k] & 0x7Fu; sum += g[i + k]; }
    }
    f[n++] = (uint8_t)(sum & 0x7Fu); f[n++] = 0xF7;
    say(ota_wire_send(f, n) == 0 ? "DNA OUT" : "DNA NOT SENT");
}
/* the last crash (hal/fm1_irq.h: kept in .noinit until power-off) for the host: F0 7D 'R' 'C' 2, then 32-bit words, 8 hex
 * digits (0..15) each, F7. Words: fm1_crash (42), core 1's fault (count cpu dbg pc), mc_why | lost << 8 | on << 16, the
 * dual-core bar (2), the boot guard (3), the build, the breadcrumbs before the crash (n, then 16 x ms code) and this
 * run's (the same) */
#ifdef DINGHY_HOST
static void crash_send(void) {}
static volatile uint8_t crash_q;
#else
static void crash_send(void)
{
#define CW (sizeof(fm1_crash_t) / 4u + 11u + 2u * 17u)
    static uint8_t f[6u + CW * 8u + 1u];
    static uint32_t w[CW];
    uint32_t n = 0, i, k, o = sizeof(fm1_crash_t) / 4u;
    memcpy(w, &fm1_crash, sizeof(fm1_crash_t));
    w[o++] = fm1_core1_fault.count; w[o++] = fm1_core1_fault.cpu; w[o++] = fm1_core1_fault.dbg; w[o++] = fm1_core1_fault.pc;
    w[o++] = (uint32_t)mc_why | (uint32_t)fm1_mc_lost << 8 | (uint32_t)fm1_mc_on << 16;
    memcpy(&w[o], &fm1_mc_bar, 8u); o += 2u;
    memcpy(&w[o], &bootguard, 12u); o += 3u;
    w[o++] = 0x00000302u;                                   /* this build: 0.3.2 */
    w[o++] = crumbs_crash.magic == CRUMB_MAGIC ? crumbs_crash.n : 0xFFFFFFFFu;
    memcpy(&w[o], crumbs_crash.e, 128u); o += 32u;
    w[o++] = crumbs.n;
    memcpy(&w[o], crumbs.e, 128u); o += 32u;
    f[n++] = 0xF0; f[n++] = 0x7D; f[n++] = 'R'; f[n++] = 'C'; f[n++] = 2;
    for (i = 0; i < o; i++)
        for (k = 0; k < 8u; k++) f[n++] = (uint8_t)((w[i] >> (28u - 4u * k)) & 15u);
    f[n++] = 0xF7;
    say(ota_wire_send(f, n) == 0 ? "CRASH LOG OUT" : "LOG NOT SENT");
}
#endif
static void dna_poll(void)
{
    if (crash_q) { crash_q = 0; crash_send(); }
    {
    static genome_t t;
    uint8_t *g = (uint8_t *)&t;
    uint32_t n = dna_rx_len, p = 6, i = 0, k, sum = 0;
    if (!n) return;
    if (n >= 7u && dna_rx[5] == 1) {
        while (i < sizeof(genome_t) && p < n) {
            uint32_t hi = dna_rx[p++], m = sizeof(genome_t) - i < 7u ? sizeof(genome_t) - i : 7u;
            for (k = 0; k < m && p < n; k++, i++) { g[i] = (uint8_t)(dna_rx[p++] | ((hi >> k) & 1u) << 7); sum += g[i]; }
        }
        if (i == sizeof(genome_t) && p + 1u == n && (sum & 0x7Fu) == dna_rx[p]) {
            push_undo(); U.g = t; save_slot(); publish(1); P.root = U.g.root; P.scale = U.g.scale; say("DNA IN");
        } else say("DNA BAD");
    }
    RING_PUBLISH();
    dna_rx_len = 0;
    }
}
static void button_down(int id)
{
    uint32_t held = fm1_in.buttons;
    int seq = (held >> BTN_SEQ & 1u) && id != BTN_SEQ, i, ht = held_throw();
    crumb('B', (uint32_t)id, (uint32_t)U.page << 8 | (uint32_t)P.eng);
    if (seq) {
        U.seq_used = 1;
        switch (id) {
        case BTN_PLAY: hard_mutate(); break;
        case BTN_REC: push_undo(); soft_mutate(&U.g, U.salt++); publish(0); say("SOFT"); break;
        case BTN_HOME: if (U.nundo) { U.g = U.undo[--U.nundo]; publish(0); say("UNDO"); } break;
        case BTN_OCT_DOWN: cmd(OP_DROP, 0, 0); say("DROP"); break;
        case BTN_FX: cmd(OP_FILL, 1, 0); break;
        case BTN_EDIT: if (U.hasB) { genome_t t = U.g; U.g = U.gB; U.gB = t; publish_b(); publish(1); say("A/B"); } break;
        case BTN_SAVE: dna_send(); break;
        }
        return;
    }
    if (U.rec_held && id == BTN_HOME) { memset(&U.g.mo, 0, sizeof U.g.mo); publish(0); U.rec_used = 1; say("MOTION CLEAR"); return; }
    for (i = 0; i < 4; i++) if (THROW_BTN[i] == id) { throw_down(i); return; }
    switch (id) {
    case BTN_PLAY: cmd(OP_PLAY, !s_playing, 0); midi_send(s_playing ? 0xFC : 0xFA, 0, 0); U.clk_sent = s_clk24; break;
    case BTN_OCT_UP: if (ht >= 0) cmd(OP_KEEP, (uint32_t)ht, 0); else if (U.oct < 2) U.oct++; break;
    case BTN_OCT_DOWN: if (ht >= 0) cmd(OP_ABORT, (uint32_t)ht, 0); else if (U.oct > -2) U.oct--; break;
    case BTN_HOME: U.home_t = fm1_ms; U.home_panic = 0; break;
    case BTN_SEQ: U.seq_used = 0; break;
    case BTN_EDIT: U.page = U.page == PG_VOICE ? PG_FX : U.page == PG_FX ? PG_SYS : PG_VOICE; break;
    case BTN_GLO: P.drums = !P.drums; say(P.drums ? "DRUMS ON" : "DRUMS OFF"); break;
    case BTN_SEL:   /* the throw bank on PLAY / SEQ; the key backlight on FX; the sound dice on VOICE (on release) */
        if (U.page == PG_PLAY || U.page == PG_SEQ) {
            for (i = 0; i < 4; i++) if (U.tdown_on[i]) return;   /* (not while a throw is held) */
            U.bank ^= 1; say(U.bank ? "BANK B" : "BANK A");
        }
        if (U.page == PG_FX) { U.fxl ^= 1; say(U.fxl ? "PHASER OTT HARM" : "DELAY SUB DUCK"); }
        U.sel_t = fm1_ms;
        break;
    case BTN_SAVE: settings_store(); break;
    case BTN_REC: U.rec_held = 1; U.ntap = 0; break;
    }
    U.dirty_all = 1;
}
static void button_up(int id)
{
    int i;
    for (i = 0; i < 4; i++) if (THROW_BTN[i] == id) { if (U.tdown_on[i]) throw_up(i); U.dirty_all = 1; break; }
    if (id == BTN_FX) cmd(OP_FILL, 0, 0);
    switch (id) {
    case BTN_HOME:
        if (!U.home_panic) {
            if (fm1_ms - U.home_last < 320u) { P.cut = U.snap[0]; P.res = U.snap[1]; P.env = U.snap[2]; P.burst = U.snap[3]; say("RETURN"); }
            else U.page = PG_PLAY;
            U.home_last = fm1_ms;
        }
        break;
    case BTN_SEQ: if (!U.seq_used) { if (U.page != PG_SEQ) U.page = PG_SEQ; else U.seqmode = !U.seqmode; } break;
    case BTN_REC: U.rec_held = 0; apply_tap(); break;
    case BTN_SEL: if (U.page == PG_VOICE) { if (fm1_ms - U.sel_t >= 600u) dice_undo(); else dice(); } break;
    }
    U.dirty_all = 1;
}
static void key_down(int k)
{
    uint32_t held = fm1_in.buttons;
    int n = 53 + k + 12 * U.oct, acc;
    if (held >> BTN_SEQ & 1u) {
        static const int8_t WL[8] = {0, 2, 4, 6, 7, 9, 11, 12};   /* F3 G3 A3 B3 C4 D4 E4 F4 */
        static const int8_t WP[8] = {14, 16, 18, 19, 21, 23, 24, 26}; /* G4 .. G5 */
        int i;
        U.seq_used = 1;
        for (i = 0; i < 8; i++) if (WL[i] == k) { push_undo(); hard_lane(&U.g, i); publish(0); say(LANE_NAME[i]); return; }
        for (i = 0; i < 8; i++) if (WP[i] == k) { save_slot(); load_slot(i, 1); say("PATTERN"); return; }
        return;
    }
    if (U.rec_held && s_playing) { if (U.ntap < 16) U.tapdeg[U.ntap++] = semi_to_deg(U.g.scale, n - 60); return; }
    acc = fm1_ms - U.key_up_t[k] < 220u;
    U.key_t[k] = fm1_ms;
    cmd(OP_ON, (uint32_t)n, acc);
    if (!s_playing) midi_send(0x90u | (uint32_t)U.chan, (uint32_t)n - 24u, acc ? 127u : 96u);
}
static void key_up(int k)
{
    int n = 53 + k + 12 * U.oct;
    U.key_up_t[k] = fm1_ms; U.key_t[k] = 0;
    cmd(OP_OFF, (uint32_t)n, 0);
    if (!s_playing) midi_send(0x80u | (uint32_t)U.chan, (uint32_t)n - 24u, 0);
}

/* ---------------------------------------------------------------- events -> MIDI + screen */
static void drain_events(void)
{
    while (eq_r != eq_w) {
        evt_t e = eq[eq_r & 127u];
        eq_r++;
        switch (e.ev) {
        case EV_NOTE: if (s_playing) { if (U.last_note >= 0 && U.last_note != e.a) midi_send(0x80u | (uint32_t)U.chan, (uint32_t)U.last_note, 0); midi_send(0x90u | (uint32_t)U.chan, e.a, e.b ? 127u : 90u); } U.last_note = e.a; if (e.b) U.acc_flash = 3; break;
        case EV_OFF: if (s_playing && U.last_note >= 0) midi_send(0x80u | (uint32_t)U.chan, (uint32_t)U.last_note, 0); U.last_note = -1; break;
        case EV_BEAT: U.beat_flash = 2; midi_send(0x9F, e.a ? 51u : 52u, 100); break;
        case EV_ENGAGE: midi_send(0x9F, 36u + e.a, 110); break;
        case EV_LAND: midi_send(0x8F, 36u + e.a, 0); midi_send(0x9F, 49, 100); if (U.latch[e.a]) U.latch[e.a] = 0; break;
        case EV_DROP: U.drop_flash = 2; midi_send(0x9F, 48, 127); break;
        case EV_ROLL: U.roll_flash++; break;
        case EV_PANIC: midi_send(0x9F, 55, 127); break;
        case EV_STEP: U.last_step_ri = e.a; break;
        case EV_WRAP:
            if (drift_lane(&U.g, e.a, e.b, P.lock)) publish(0);
            break;
        }
    }
}
static void send_cc(void)
{
    int32_t v[6], i;
    static const uint8_t CC[6] = {20, 21, 22, 23, 26, 27};
    v[0] = clampi(s_fcidx * 127 / 560, 0, 127); v[1] = s_burst * 127 >> 16; v[2] = s_env >> 17; v[3] = s_acccap >> 17;
    v[4] = s_prog * 127 >> 16; v[5] = s_frac * 127 >> 16;
    for (i = 0; i < 6; i++) if (U.lastcc[i] != v[i]) { U.lastcc[i] = v[i]; midi_send(0xB0u | (uint32_t)U.chan, CC[i], (uint32_t)clampi(v[i], 0, 127)); }
}

static void app_main(void)
{
    uint32_t prev_keys = 0, last_draw = 0, last_cc = 0, last_strip = 0xFFFFFFFFu;
    int i;
    memset(&U, 0, sizeof U);
    U.combo = -1; for (i = 0; i < 4; i++) U.tactive[i] = -1; U.last_note = -1;
    P.vol = ONE16 * 85 / 100; P.bpm10 = 1320; P.lock = 100; P.drums = 1; P.drum_lvl = ONE16 * 65 / 100; P.glide = ONE16 / 2;
    P.mod_depth = ONE16 * 35 / 100; P.kick_tune = 1; P.harm = ONE16 / 4; P.squash = ONE16 * 6 / 100 * 10; P.ph_rate = ONE16 * 6 / 100;
    for (i = 0; i < NTH; i++) { P.tp[i][0] = P.tp[i][1] = P.tp[i][2] = P.tp[i][3] = ONE16 / 2; }
    P.tp[T_RISE][1] = ONE16 * 67 / 100; P.tp[T_RISE][3] = ONE16 * 7 / 10; P.tp[T_SHRED][2] = 0; P.tp[T_VOID][3] = ONE16 * 8 / 10;
    for (i = 0; i < 8; i++) { U.slot[i].seed = FACT[i].seed; U.slot[i].type = FACT[i].type; U.slot[i].dens = FACT[i].dens; U.slot[i].slide = FACT[i].slide; U.slot[i].acc = FACT[i].acc; U.slot[i].scale = FACT[i].scale; }
    load_preset(0);
    U.light = 2;
    if ((settings_load(&SV, sizeof SV) == 0 && SV.ver == 5) || (settings_load(&SV, SAVE_V4_N) == 0 && SV.ver == 4) ||
        (settings_load(&SV, SAVE_V3_N) == 0 && SV.ver == 3)) {
        load_preset(SV.preset);
        P.cut = SV.cut; P.res = SV.res; P.env = SV.env; P.dec = SV.dec; P.acc = SV.acc; P.burst = SV.burst; P.chr = SV.wave;
        P.glide = SV.glide; P.sub = SV.sub; P.drive = SV.drive; P.dly_send = SV.dly_send; P.dly_fb = SV.dly_fb; P.duck = SV.duck;
        P.vol = SV.vol ? SV.vol : P.vol; P.dly_time = SV.dly_time; P.lock = SV.lock; P.bpm10 = SV.bpm10 ? SV.bpm10 : 1320; P.swing = SV.swing; P.drums = SV.drums;
        memcpy(U.slot, SV.slot, sizeof U.slot);
        U.pat = SV.pat & 7;
        if (SV.ver == 5) {
            P.phaser = SV.phaser; P.ph_rate = SV.ph_rate; P.squash = SV.squash;
            U.chan = SV.chan & 15; U.sync = (SV.chan >> 4) & 3; U.stems = (SV.chan >> 6) & 1;
            memcpy(U.mot, SV.mo, sizeof U.mot);
        }
        if (SV.ver >= 4) {
            P.eng = SV.eng < NENG ? SV.eng : 0; P.ratio = SV.ratio ? SV.ratio : 2; P.rel = SV.rel ? SV.rel : 8;
            P.fold = SV.fold; P.formant = SV.formant; P.harm = SV.harm; if (SV.light) U.light = (SV.light - 1) & 3;
        }
    }
    U.light_set = -1;
    P.sync = U.sync;
    if (U.stems) usb_stems_set(1);              /* (USB re-enumerates once, as the stems' device) */
    dsp_init();
    dsp_ready = 1;
    genome_init(&U.g, U.slot[U.pat].seed, U.slot[U.pat].type, U.slot[U.pat].dens, U.slot[U.pat].slide, U.slot[U.pat].acc, U.slot[U.pat].scale);
    if (U.slot[U.pat].gen) { U.g.gen = U.slot[U.pat].gen; generate(&U.g); }
    gbuf[0] = U.g; g_a = 0; U.next_buf = 1;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    cv_begin(240, 120, C_BLACK);
    glyph_draw(GLYPH_LIE32, 32, 104, 20, C_HEAT);
    cv_text(120 - text_w(&FONT_L, "RIFT") / 2, 60, &FONT_L, "RIFT", C_INK);
    cv_text(120 - text_w(&FONT_S, "MR.V  " APP_VER) / 2, 96, &FONT_S, "MR.V  " APP_VER, C_MUTE);
    cv_blit(0, 60);
    U.dirty_all = 1;
    {
        uint32_t t0 = fm1_ms;
        while (fm1_ms - t0 < 700u) platform_poll();
    }
    for (;;) {
        uint32_t keys, down, up, rel, b, k;
        uint8_t st, d1, d2;
        platform_poll();
        /* keys */
        keys = fm1_in.notes; down = keys & ~prev_keys; up = prev_keys & ~keys; prev_keys = keys;
        for (k = 0; k < 27u; k++) { if (down >> k & 1u) key_down((int)k); if (up >> k & 1u) key_up((int)k); }
        /* pressure: the longest held key, 0.15 s .. 1.65 s -> 0..1 */
        {
            uint32_t hold = 0;
            for (k = 0; k < 27u; k++) if (keys >> k & 1u && U.key_t[k] && fm1_ms - U.key_t[k] > hold) hold = fm1_ms - U.key_t[k];
            P.pres += (clampi(((int32_t)hold - 150) * ONE16 / 1500, 0, ONE16) - P.pres) / 8;
        }
        /* buttons */
        b = fm1_input_edges(&rel);
        for (k = 0; k < 14u; k++) { if (b >> k & 1u) button_down((int)k); if (rel >> k & 1u) button_up((int)k); }
        if ((fm1_in.buttons >> BTN_HOME & 1u) && !U.home_panic && fm1_ms - U.home_t > 500u && U.home_t) {
            U.home_panic = 1; cmd(OP_PANIC, 0, 0); for (i = 0; i < NTH; i++) U.latch[i] = 0; U.combo = -1;
            P.cut = U.snap[0]; P.res = U.snap[1]; P.env = U.snap[2]; P.burst = U.snap[3]; say("PANIC");
        }
        /* encoders */
        for (k = 0; k < 7u; k++) {
            int32_t d = fm1_enc_take(k);
            if (!d) continue;
            if (fm1_ms - U.enc_t[k] < 40u) d *= 3;
            U.enc_t[k] = fm1_ms;
            crumb('E', k, (uint32_t)U.page << 8 | (uint32_t)P.eng);
            switch (k) {
            case ENC_SELECT:
                if (U.page == PG_SEQ) { U.g.scale = (uint8_t)((U.g.scale + (d > 0 ? 1 : 9)) % 10); P.scale = U.g.scale; generate(&U.g); save_slot(); publish(0); }
                else if (U.page == PG_FX) P.dly_time = clampi(P.dly_time + (d > 0 ? 1 : -1), 0, 4);
                else if (U.page == PG_VOICE) { P.eng = (P.eng + (d > 0 ? 1 : NENG - 1)) % NENG; say(ENG_NAME[P.eng]); }
                else P.bpm10 = clampi(P.bpm10 + d * 10, 600, 2000);
                break;
            case ENC_ALGORITHM: load_preset(U.preset + (d > 0 ? 1 : -1)); break;
            case ENC_PRESETS: save_slot(); load_slot(U.pat + (d > 0 ? 1 : -1), 1); break;
            default: knob_turn((int)k - ENC_KNOB1, d); break;
            }
            U.dirty_all = 1;
        }
        /* MIDI in */
        while (midi_get(&st, &d1, &d2)) {
            if ((st & 0x0Fu) != (uint32_t)U.chan) continue;   /* this unit's channel only */
            if ((st & 0xF0u) == 0x90u && d2) cmd(OP_ON, d1, 0);
            else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) cmd(OP_OFF, d1, 0);
            else if ((st & 0xF0u) == 0xB0u) { if (d1 == 74) P.cut = d2 * 516; else if (d1 == 71) P.res = d2 * 516; else if (d1 == 1) P.burst = d2 * 516; }
        }
        drain_events();
        dna_poll();
        P.sync = U.sync;
        /* clock out */
        while (U.clk_sent != s_clk24) { U.clk_sent++; if (!s_ext) midi_send(0xF8, 0, 0); }   /* (a follower sends none: no loops) */
        if (fm1_ms - last_cc >= 8u) { last_cc = fm1_ms; send_cc(); }
        /* LEDs */
        for (i = 0; i < 4; i++) {
            int t = THROW_OF[i], s = s_th[t], j;
            for (j = 0; j < 4; j++) { int c = combo_of(i, j); if (j != i && c >= 0 && s_th[c] > s) s = s_th[c]; }
            fm1_led_key((uint32_t)THROW_BTN[i], s == 1 ? (fm1_ms / 60u) & 1u : s >= 2 ? (U.latch[t] ? (fm1_ms / 300u) & 1u : 1) : 0);
        }
        fm1_led_key(BTN_PLAY, s_playing && U.beat_flash);
        fm1_led_key(BTN_SEQ, U.page == PG_SEQ); fm1_led_key(BTN_HOME, U.page == PG_PLAY); fm1_led_key(BTN_EDIT, U.page >= PG_VOICE);
        fm1_led_key(BTN_REC, U.rec_held); fm1_led_key(BTN_GLO, P.drums);
        fm1_led_key(BTN_OCT_UP, U.oct > 0); fm1_led_key(BTN_OCT_DOWN, U.oct < 0);
        if (U.light_set != U.light * 16 + U.oct + 2 + P.root * 64) {   /* the backlight: every key; the root notes and the throws glow */
            static const uint16_t BG_NS[4] = {0, 700, 1800, 4000};
            int k;
            U.light_set = U.light * 16 + U.oct + 2 + P.root * 64;
            fm1_led_bg_ns = BG_NS[U.light & 3];
            fm1_led_bg_all(U.light > 0);
            for (k = 0; k < 27; k++) fm1_led_glow_key(KEY_LED(k), U.light > 0 && (53 + k + 12 * U.oct - P.root + 120) % 12 == 0);
            for (k = 0; k < 4; k++) fm1_led_glow_key((uint32_t)THROW_BTN[k], U.light > 0);
        }
        /* screen: ~25 fps for the live parts, the rest on change */
        if (fm1_ms - last_draw >= 40u) {
            last_draw = fm1_ms;
            if (U.dirty_all) { U.dirty_all = 0; top_bar(); draw_macros(); last_strip = 0xFFFFFFFFu; }
            dot();
            if (U.page == PG_PLAY) {
                draw_scope();
                if (s_abs != last_strip) { last_strip = s_abs; draw_strip(); }
            } else if (U.page == PG_SEQ) { draw_seq_page(); if (s_abs != last_strip) { last_strip = s_abs; draw_strip(); } }
            else { draw_info_page(); if (s_abs != last_strip) { last_strip = s_abs; draw_strip(); } }
            if (held_throw() >= 0 || U.page != PG_PLAY) draw_macros();
            else { static int32_t lc, lr, le, lb; if (lc != P.cut || lr != P.res || le != P.env || lb != P.burst) { lc = P.cut; lr = P.res; le = P.env; lb = P.burst; draw_macros(); } }
            if (U.beat_flash) U.beat_flash--;
            if (U.acc_flash) U.acc_flash--;
        }
    }
}
