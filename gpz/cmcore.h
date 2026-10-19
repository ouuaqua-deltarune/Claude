/*
 * GPZ-CM core: context-mixing compressor (maximum ratio). Compiled as C for the
 * CPU and as OpenCL C for the GPU, so both produce identical bytes.
 *
 * Models: orders 0-4,6,8, word, sparse, column and match models, each context with
 * a fast and a slow counter, blended by two gated logistic mixers, refined by three
 * APM/SSE stages, then a 32-bit binary arithmetic coder. Integer-only.
 *
 * The coder is resumable at byte boundaries: all model memory lives in one "arena"
 * and the few scalar registers can be saved/loaded, so a GPU can advance thousands
 * of independent streams a few KB at a time (short kernels, no watchdog trouble).
 */
#include "common.h"

#define CM_NT 11
#define CM_NI (2 * CM_NT + 2)
#define CM_LR 24

CONSTARR int cm_sqt[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,
    2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
CONSTARR u32 cm_limf[CM_NT] = {30, 4, 3, 2, 2, 2, 2, 2, 2, 3, 2};                    /* fast counters */
CONSTARR u32 cm_lims[CM_NT] = {1023, 255, 120, 60, 40, 30, 30, 60, 60, 60, 30};      /* slow counters */

FN int cm_squash(int d) {
    int w;
    if (d > 2047) return 4095;
    if (d < -2047) return 0;
    w = d & 127; d = (d >> 7) + 16;
    return (cm_sqt[d] * (128 - w) + cm_sqt[d + 1] * w + 64) >> 7;
}
FN u32 cm_hash(u32 x, u32 salt) {
    x = (x ^ (salt * 0x7FEB352Du)) * 0x9E3779B1u; x ^= x >> 15;
    x *= 0x85EBCA6Bu; x ^= x >> 13; return x;
}

typedef struct {
    u32 tbits, mb, pos, c0, c4, c8, bit, wordh, lstart, plstart, mptr, mlen, mbyte, hh[CM_NT];
    u32 x1, x2, xv, io, err, fin, wr;
    GP u8 *hist;                                  /* encoder: input; decoder: output so far */
    GP u32 *t[CM_NT], *sp[CM_NT], *mht, *msm, *msp;
    GP const int *stt, *dtt;                      /* stretch / adaptation-rate tables */
    GP int *w1, *w2, *cw1, *cw2;
    GP u16 *a1, *a2, *a3;
    int x[CM_NI], pr1, pr2, pm, ai1, ai2, ai3;
} CMP;

/* ---- memory arena: sets table pointers (when m != 0) and returns the size in bytes ---- */
FN size_t cm_layout(CMP *m, GP u8 *arena, u32 tbits) {
    size_t o = 0; int i;
#define CM_TAKE(ptr, type, count) do { if (m) (ptr) = (GP type *)(arena + o); \
        o += (size_t)(count) * sizeof(type); o = (o + 15) & ~(size_t)15; } while (0)
    for (i = 0; i < CM_NT; i++) {
        size_t cnt = i == 0 ? 2 * 256 : i == 1 ? 2 * 65536 : (size_t)2 << tbits;
        CM_TAKE(m->t[i], u32, cnt);
    }
    CM_TAKE(m->mht, u32, (size_t)1 << (tbits > 4 ? tbits - 2 : 2));
    CM_TAKE(m->msm, u32, 64);
    CM_TAKE(m->w1, int, 256 * CM_NI);
    CM_TAKE(m->w2, int, 5 * 256 * CM_NI);
    CM_TAKE(m->a1, u16, 256 * 24);
    CM_TAKE(m->a2, u16, 65536 * 24);
    CM_TAKE(m->a3, u16, 65536 * 24);
#undef CM_TAKE
    return o;
}
/* initial contents of the arena */
FN void cm_init_arena(CMP *m, u32 tbits) {
    size_t k, cnt; int i;
    for (i = 0; i < CM_NT; i++) {
        cnt = i == 0 ? 2 * 256 : i == 1 ? 2 * 65536 : (size_t)2 << tbits;
        for (k = 0; k < cnt; k++) m->t[i][k] = 0x80000000u;
    }
    cnt = (size_t)1 << (tbits > 4 ? tbits - 2 : 2);
    for (k = 0; k < cnt; k++) m->mht[k] = 0;
    for (k = 0; k < 64; k++) m->msm[k] = (k & 1) ? 0xC0000000u : 0x40000000u;
    for (k = 0; k < 256 * CM_NI; k++) m->w1[k] = 6000;
    for (k = 0; k < 5 * 256 * CM_NI; k++) m->w2[k] = 6000;
    for (k = 0; k < 256 * 24; k++)
        m->a1[k] = (u16)(cm_squash((int)(k % 24) * 2 * 4096 / 48 + 4096 / 48 - 2048) * 16);
    for (k = 0; k < 65536 * 24; k++) m->a2[k] = m->a3[k] = m->a1[k % (256 * 24)];
}
/* scalar registers at a stream start */
FN void cm_begin(CMP *m, u32 tbits) {
    int i;
    m->tbits = tbits; m->mb = tbits > 4 ? tbits - 2 : 2;
    m->pos = 0; m->c0 = 1; m->c4 = m->c8 = 0; m->bit = 0; m->wordh = 0; m->lstart = m->plstart = 0;
    m->mptr = m->mlen = m->mbyte = 0;
    for (i = 0; i < CM_NT; i++) m->hh[i] = 0;
    m->x1 = 0; m->x2 = 0xFFFFFFFFu; m->xv = 0; m->io = 0; m->err = 0; m->fin = 0; m->msp = 0;
}
#ifdef __OPENCL_VERSION__
FN void cm_save(CMP *m, GP u32 *s) {
    int i;
    s[0] = m->pos; s[1] = m->c4; s[2] = m->c8; s[3] = m->wordh; s[4] = m->lstart; s[5] = m->plstart;
    s[6] = m->mptr; s[7] = m->mlen; s[8] = m->mbyte; s[9] = m->x1; s[10] = m->x2; s[11] = m->xv;
    s[12] = m->io; s[13] = m->err; s[14] = m->fin;
    for (i = 0; i < CM_NT; i++) s[15 + i] = m->hh[i];     /* 15..25 */
}
FN void cm_load(CMP *m, GP const u32 *s, u32 tbits) {
    int i;
    cm_begin(m, tbits);
    m->pos = s[0]; m->c4 = s[1]; m->c8 = s[2]; m->wordh = s[3]; m->lstart = s[4]; m->plstart = s[5];
    m->mptr = s[6]; m->mlen = s[7]; m->mbyte = s[8]; m->x1 = s[9]; m->x2 = s[10]; m->xv = s[11];
    m->io = s[12]; m->err = s[13]; m->fin = s[14];
    for (i = 0; i < CM_NT; i++) m->hh[i] = s[15 + i];
}
#endif

/* ---- probability slot: high 22 bits = P(1), low 10 bits = hit count (adaptive rate) ---- */
FN void cm_sm(GP u32 *t, int y, u32 limit, GP const int *dt) {
    u32 s = *t, n = s & 1023, p = s >> 10; int d;
    s = n < limit ? s + 1 : (s & 0xFFFFFC00u) | limit;
    d = ((int)((u32)y << 22) - (int)p) >> 3;
    *t = s + (((u32)d * (u32)dt[n]) & 0xFFFFFC00u);
}
FN int cm_apm(GP const int *st, GP u16 *t, int pr, u32 cx, int *index) {
    int w, c;
    pr = (st[pr] + 2048) * 23;
    w = pr & 0xFFF; c = (int)cx * 24 + (pr >> 12);
    *index = c + (w >> 11);
    return (t[c] * (4096 - w) + t[c + 1] * w) >> 16;
}
FN void cm_apm_up(GP u16 *t, int index, int y) {
    int g = (y << 16) + (y << 7) - y - y;
    t[index] = (u16)(t[index] + ((g - t[index]) >> 7));
}
FN int cm_clamp(int v) { return v > 2047 ? 2047 : v < -2047 ? -2047 : v; }

/* P(next bit = 1), 12 bit */
FN int cm_p(CMP *m) {
    u32 c0 = m->c0, c1 = m->c4 & 0xFF; int i, s1 = 0, s2 = 0, p, pa1, pa2, pa3;
    m->sp[0] = m->t[0] + 2 * c0;
    m->sp[1] = m->t[1] + 2 * ((c1 << 8) | c0);
    for (i = 2; i < CM_NT; i++)
        m->sp[i] = m->t[i] + 2 * (((m->hh[i] ^ (c0 * 0x2545F491u)) * 0x9E3779B1u) >> (32 - m->tbits));
    for (i = 0; i < CM_NT; i++) {
        u32 s = m->sp[i][0], s2v = m->sp[i][1];
        m->x[2 * i] = (i < 2 || (s & 1023)) ? m->stt[s >> 20] : 0;
        m->x[2 * i + 1] = (i < 2 || (s2v & 1023)) ? m->stt[s2v >> 20] : 0;
    }
    m->msp = 0; m->x[2 * CM_NT] = 0;
    if (m->mlen > 0 && ((m->mbyte | 256) >> (8 - m->bit)) == c0) {
        u32 l = m->mlen > 31 ? 31 : m->mlen, e = (m->mbyte >> (7 - m->bit)) & 1;
        m->msp = m->msm + l * 2 + e;
        m->x[2 * CM_NT] = m->stt[*m->msp >> 20];
    } else m->mlen = 0;
    m->x[2 * CM_NT + 1] = 256;
    m->cw1 = m->w1 + c0 * CM_NI;
    m->cw2 = m->w2 + ((m->mlen == 0 ? 0u : m->mlen < 8 ? 1u : m->mlen < 16 ? 2u : m->mlen < 32 ? 3u : 4u) * 256 + c1) * CM_NI;
    for (i = 0; i < CM_NI; i++) { s1 += (m->x[i] * m->cw1[i]) >> 8; s2 += (m->x[i] * m->cw2[i]) >> 8; }
    s1 = cm_clamp(s1 >> 8); s2 = cm_clamp(s2 >> 8);
    m->pr1 = cm_squash(s1); m->pr2 = cm_squash(s2);
    m->pm = cm_squash((s1 + s2) >> 1);
    pa1 = cm_apm(m->stt, m->a1, m->pm, c0, &m->ai1);
    pa2 = cm_apm(m->stt, m->a2, m->pm, c0 | (c1 << 8), &m->ai2);
    pa3 = cm_apm(m->stt, m->a3, m->pm, (cm_hash(m->c4 & 0xFFFFFF, 11) >> 16 ^ c0 * 0x9E37u) & 0xFFFF, &m->ai3);
    p = (2 * m->pm + pa1 + pa2 + 2 * pa3 + 3) / 6;
    return p < 1 ? 1 : p > 4095 ? 4095 : p;
}

FN void cm_up(CMP *m, int y) {
    int i, e1, e2;
    for (i = 0; i < CM_NT; i++) {
        cm_sm(m->sp[i], y, cm_limf[i], m->dtt);
        cm_sm(m->sp[i] + 1, y, cm_lims[i], m->dtt);
    }
    if (m->msp) cm_sm(m->msp, y, 1023, m->dtt);
    e1 = ((y << 12) - m->pr1) * CM_LR; e2 = ((y << 12) - m->pr2) * CM_LR;
    for (i = 0; i < CM_NI; i++) {
        m->cw1[i] += (m->x[i] * e1 + 0x8000) >> 16;
        m->cw2[i] += (m->x[i] * e2 + 0x8000) >> 16;
    }
    cm_apm_up(m->a1, m->ai1, y); cm_apm_up(m->a2, m->ai2, y); cm_apm_up(m->a3, m->ai3, y);
    m->c0 = (m->c0 << 1) | (u32)y; m->bit++;
    if (m->bit == 8) {
        u32 c = m->c0 & 255, lc = c | 32, h;
        if (m->wr) m->hist[m->pos] = (u8)c;
        m->pos++;
        m->c8 = (m->c8 << 8) | (m->c4 >> 24); m->c4 = (m->c4 << 8) | c;
        m->wordh = (lc >= 'a' && lc <= 'z') ? (m->wordh + lc + 1) * 0x2F0B4A27u : 0;
        m->hh[2] = cm_hash(m->c4 & 0xFFFF, 2);
        m->hh[3] = cm_hash(m->c4 & 0xFFFFFF, 3);
        m->hh[4] = cm_hash(m->c4, 4);
        m->hh[5] = cm_hash(m->c4 ^ ((m->c8 & 0xFFFF) * 0x9E3779B1u), 5);
        m->hh[6] = cm_hash(m->c4 ^ (m->c8 * 0x2F0B4A27u), 6);
        m->hh[7] = cm_hash(m->wordh ? m->wordh : (m->c4 & 0xFFFF) * 0x9E3779B1u + 1, 7);
        m->hh[8] = cm_hash(m->c4 & 0xFF00FF00u, 8);
        m->hh[10] = cm_hash(m->c4 ^ ((m->c8 & 0xFF) * 0x9E3779B1u), 10);
        if (c == 10) { m->plstart = m->lstart; m->lstart = m->pos; }
        {
            u32 col = m->pos - m->lstart, above = 0;
            if (m->plstart + col < m->lstart) above = m->hist[m->plstart + col];
            m->hh[9] = cm_hash(above | ((m->c4 & 0xFF) << 8) | (col > 255 ? 255u << 16 : col << 16), 9);
        }
        /* match model */
        if (m->mlen > 0 && m->hist[m->mptr] == c) { if (m->mlen < 65535) m->mlen++; m->mptr++; }
        else m->mlen = 0;
        if (m->pos >= 5) {
            h = cm_hash(m->c4 ^ (m->hist[m->pos - 5] * 0x9E3779B1u), 9) >> (32 - m->mb);
            if (m->mlen == 0) {
                u32 cand = m->mht[h], l = 0;
                if (cand > 0)
                    while (l < 32 && l < cand && m->hist[cand - 1 - l] == m->hist[m->pos - 1 - l]) l++;
                if (l >= 5) { m->mlen = l; m->mptr = cand; }
            }
            m->mht[h] = m->pos;
        }
        m->mbyte = m->mlen > 0 ? m->hist[m->mptr] : 0;
        m->c0 = 1; m->bit = 0;
    }
}

/* ---- arithmetic coder steps ---- */
/* Encode up to maxb more bytes of in[0..n). Returns 1 when finished (flushed) or on overflow (err). */
FN u32 cm_enc_step(CMP *m, GP const u8 *in, u32 n, GP u8 *out, u32 cap, u32 maxb) {
    u32 end = m->pos + maxb; int b;
    if (end > n || end < m->pos) end = n;
    while (m->pos < end && !m->err) {
        u32 c = in[m->pos];
        for (b = 7; b >= 0; b--) {
            int y = (int)((c >> b) & 1), p = cm_p(m);
            u32 xm = m->x1 + ((m->x2 - m->x1) >> 12) * (u32)p;
            if (y) m->x2 = xm; else m->x1 = xm + 1;
            cm_up(m, y);
            while (((m->x1 ^ m->x2) & 0xFF000000u) == 0) {
                if (m->io >= cap) { m->err = 1; return 1; }
                out[m->io++] = (u8)(m->x2 >> 24); m->x1 <<= 8; m->x2 = (m->x2 << 8) | 255;
            }
        }
    }
    if (m->err) return 1;
    if (m->pos >= n && !m->fin) {
        if (m->io + 4 > cap) { m->err = 1; return 1; }
        for (b = 0; b < 4; b++) { out[m->io++] = (u8)(m->x1 >> 24); m->x1 <<= 8; }
        m->fin = 1;
    }
    return m->fin;
}
FN void cm_dec_begin(CMP *m, GP const u8 *cin, u32 cn) {
    int b;
    m->xv = 0;
    for (b = 0; b < 4; b++) m->xv = (m->xv << 8) | (m->io < cn ? cin[m->io++] : 0);
}
/* Decode up to maxb more bytes into hist (the output). Returns 1 when n bytes are done. */
FN u32 cm_dec_step(CMP *m, GP const u8 *cin, u32 cn, u32 n, u32 maxb) {
    u32 end = m->pos + maxb; int b;
    if (end > n || end < m->pos) end = n;
    while (m->pos < end) {
        for (b = 0; b < 8; b++) {
            int p = cm_p(m), y; u32 xm = m->x1 + ((m->x2 - m->x1) >> 12) * (u32)p;
            y = m->xv <= xm;
            if (y) m->x2 = xm; else m->x1 = xm + 1;
            cm_up(m, y);
            while (((m->x1 ^ m->x2) & 0xFF000000u) == 0) {
                m->x1 <<= 8; m->x2 = (m->x2 << 8) | 255;
                m->xv = (m->xv << 8) | (m->io < cn ? cin[m->io++] : 0);
            }
        }
    }
    return m->pos >= n;
}

#ifdef __OPENCL_VERSION__
/* One work-item advances one stream. jobs[8*s..]: n, cn_or_cap, state, outlen, tbits.
   state: 4 = needs init, 1 = active, 2 = done, 3 = error.  mode: 0 init-encode, 3 init-decode,
   1 encode step, 2 decode step. */
__kernel void cm_k(uint mode, __global uchar *grp, uint slot_bytes, uint arena_bytes, uint capA,
                   uint nslots, __global uint *state, __global uint *jobs,
                   __global const int *stt, __global const int *dtt, uint maxb) {
    uint s = (uint)get_global_id(0), tbits;
    __global uint *job; __global uchar *base, *A, *B; CMP m;
    if (s >= nslots) return;
    job = jobs + 8 * s; tbits = job[4];
    if ((mode == 0 || mode == 3) ? job[2] != 4 : job[2] != 1) return;
    base = grp + (size_t)s * slot_bytes; A = base + arena_bytes; B = A + capA;
    m.stt = stt; m.dtt = dtt;
    cm_layout(&m, base, tbits);
    if (mode == 0 || mode == 3) {
        cm_init_arena(&m, tbits); cm_begin(&m, tbits);
        if (mode == 3) cm_dec_begin(&m, B, job[1]);
        cm_save(&m, state + (size_t)s * 32);
        job[2] = 1;
        return;
    }
    cm_load(&m, state + (size_t)s * 32, tbits);
    m.hist = A; m.wr = (mode == 2);
    if (mode == 1) {
        if (cm_enc_step(&m, A, job[0], B, job[1], maxb)) { job[2] = m.err ? 3 : 2; job[3] = m.io; }
    } else {
        if (cm_dec_step(&m, B, job[1], job[0], maxb)) { job[2] = 2; job[3] = m.pos; }
    }
    cm_save(&m, state + (size_t)s * 32);
}
#else
/* ---- CPU side ---- */
static int cm_st_h[4096], cm_dt_h[1024], cm_ready;
static void cm_tables(void) {
    int x, i, pi = 0;
    if (cm_ready) return;
    for (x = -2047; x <= 2047; x++) {
        int v = cm_squash(x);
        for (i = pi; i <= v; i++) cm_st_h[i] = x;
        pi = v + 1;
    }
    for (i = pi; i < 4096; i++) cm_st_h[i] = 2047;
    for (i = 0; i < 1024; i++) cm_dt_h[i] = 16384 / (i + i + 3);
    cm_ready = 1;
}
static size_t cm_arena_size(u32 tbits) { return cm_layout(0, 0, tbits); }

/* returns compressed size, or 0 if it did not fit in cap / out of memory */
static size_t cm_compress(const u8 *in, size_t n, u8 *out, size_t cap, u32 tbits) {
    CMP m; u8 *arena; size_t r;
    cm_tables();
    arena = (u8 *)malloc(cm_arena_size(tbits));
    if (!arena) return 0;
    m.stt = cm_st_h; m.dtt = cm_dt_h;
    cm_layout(&m, arena, tbits); cm_init_arena(&m, tbits); cm_begin(&m, tbits);
    m.hist = (u8 *)in; m.wr = 0;
    cm_enc_step(&m, in, (u32)n, out, (u32)cap, (u32)n);
    r = (m.err || !m.fin) ? 0 : m.io;
    free(arena);
    return r;
}
/* returns 0 on success */
static int cm_decompress(const u8 *in, size_t inn, u8 *out, size_t n, u32 tbits) {
    CMP m; u8 *arena;
    cm_tables();
    arena = (u8 *)malloc(cm_arena_size(tbits));
    if (!arena) return 1;
    m.stt = cm_st_h; m.dtt = cm_dt_h;
    cm_layout(&m, arena, tbits); cm_init_arena(&m, tbits); cm_begin(&m, tbits);
    m.hist = out; m.wr = 1;
    cm_dec_begin(&m, in, (u32)inn);
    cm_dec_step(&m, in, (u32)inn, (u32)n, (u32)n);
    free(arena);
    return 0;
}
#endif
