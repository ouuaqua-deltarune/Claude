/*
 * GPZ-CM: context-mixing compressor (maximum ratio mode). CPU only, solid.
 * Models: orders 0-4,6,8, word, sparse, and a match model, blended by two
 * gated logistic mixers, refined by two APM/SSE stages, then a 32-bit binary
 * arithmetic coder. Integer-only, so encoder and decoder agree on every
 * platform. Design follows the public lpaq/paq8 family.
 */
#define CM_NT 11
#define CM_NI (2 * CM_NT + 2)
#define CM_LR 24
#define CM_LRB 0

static int cm_st[4096], cm_dt[1024], cm_ready;

static int cm_squash(int d) {
    static const int t[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,
        2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
    int w;
    if (d > 2047) return 4095;
    if (d < -2047) return 0;
    w = d & 127; d = (d >> 7) + 16;
    return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}
static void cm_tables(void) {
    int x, i, pi = 0;
    if (cm_ready) return;
    for (x = -2047; x <= 2047; x++) {
        int v = cm_squash(x);
        for (i = pi; i <= v; i++) cm_st[i] = x;
        pi = v + 1;
    }
    for (i = pi; i < 4096; i++) cm_st[i] = 2047;
    for (i = 0; i < 1024; i++) cm_dt[i] = 16384 / (i + i + 3);
    cm_ready = 1;
}
static u32 cm_hash(u32 x, u32 salt) {
    x = (x ^ (salt * 0x7FEB352Du)) * 0x9E3779B1u; x ^= x >> 15;
    x *= 0x85EBCA6Bu; x ^= x >> 13; return x;
}
/* probability slot: high 22 bits = P(1), low 10 bits = hit count (adaptive rate) */
static void cm_sm(u32 *t, int y, u32 limit) {
    u32 s = *t, n = s & 1023, p = s >> 10; int d;
    s = n < limit ? s + 1 : (s & 0xFFFFFC00u) | limit;
    d = ((int)((u32)y << 22) - (int)p) >> 3;
    *t = s + ((u32)((long long)d * cm_dt[n]) & 0xFFFFFC00u);
}

typedef struct {
    u32 tbits, pos, c0, c4, c8, bit, wordh, hh[CM_NT], lstart, plstart;
    u8 *hist;
    u32 *t[CM_NT], *sp[CM_NT];
    u32 *mht, mb, mptr, mlen, mbyte, *msm, *msp;
    int x[CM_NI], *w1, *w2, *cw1, *cw2, pr1, pr2, pm;
    u16 *a1, *a2, *a3; int ai1, ai2, ai3;
} CMP;

static const u32 cm_limf[CM_NT] = {30, 4, 3, 2, 2, 2, 2, 2, 2, 3, 2};     /* fast counters */
static const u32 cm_lims[CM_NT] = {1023, 255, 120, 60, 40, 30, 30, 60, 60, 60, 30};  /* slow counters */

static void cm_free(CMP *m) {
    int i;
    if (!m) return;
    for (i = 0; i < CM_NT; i++) free(m->t[i]);
    free(m->hist); free(m->mht); free(m->msm); free(m->w1); free(m->w2);
    free(m->a1); free(m->a2); free(m->a3); free(m);
}
static CMP *cm_new(u32 tbits, size_t n) {
    CMP *m = (CMP *)calloc(1, sizeof *m); int i; size_t k;
    if (!m) return NULL;
    cm_tables();
    m->tbits = tbits; m->mb = tbits > 4 ? tbits - 2 : 2;
    m->hist = (u8 *)malloc(n + 8);
    m->mht = (u32 *)calloc((size_t)1 << m->mb, 4);
    m->msm = (u32 *)malloc(64 * 4);
    m->w1 = (int *)malloc(256 * CM_NI * sizeof(int));
    m->w2 = (int *)malloc(5 * 256 * CM_NI * sizeof(int));
    m->a1 = (u16 *)malloc(256 * 24 * 2); m->a2 = (u16 *)malloc(65536 * 24 * 2); m->a3 = (u16 *)malloc(65536 * 24 * 2);
    for (i = 0; i < CM_NT; i++) {
        size_t sz = 2 * (i == 0 ? 256 : i == 1 ? 65536 : (size_t)1 << tbits);
        m->t[i] = (u32 *)malloc(sz * 4);
        if (m->t[i]) for (k = 0; k < sz; k++) m->t[i][k] = 0x80000000u;
    }
    for (i = 0; i < CM_NT; i++) if (!m->t[i]) { cm_free(m); return NULL; }
    if (!m->hist || !m->mht || !m->msm || !m->w1 || !m->w2 || !m->a1 || !m->a2 || !m->a3) { cm_free(m); return NULL; }
    for (i = 0; i < 64; i++) m->msm[i] = ((i & 1) ? 0xC0000000u : 0x40000000u);
    for (k = 0; k < 256 * CM_NI; k++) m->w1[k] = 6000;
    for (k = 0; k < 5 * 256 * CM_NI; k++) m->w2[k] = 6000;
    for (k = 0; k < 256 * 24; k++) m->a1[k] = (u16)(cm_squash((int)(k % 24) * 2 * 4096 / 48 + 4096 / 48 - 2048) * 16);
    for (k = 0; k < 65536 * 24; k++) m->a2[k] = m->a3[k] = m->a1[k % (256 * 24)];
    m->c0 = 1;
    return m;
}
static int cm_apm(u16 *t, int pr, u32 cx, int *index) {
    int w, c;
    pr = (cm_st[pr] + 2048) * 23;
    w = pr & 0xFFF; c = (int)cx * 24 + (pr >> 12);
    *index = c + (w >> 11);
    return (t[c] * (4096 - w) + t[c + 1] * w) >> 16;
}
static void cm_apm_up(u16 *t, int index, int y) {
    int g = (y << 16) + (y << 7) - y - y;
    t[index] = (u16)(t[index] + ((g - t[index]) >> 7));
}
static int cm_clamp(int v) { return v > 2047 ? 2047 : v < -2047 ? -2047 : v; }

/* returns P(next bit = 1), 12 bit */
static int cm_p(CMP *m) {
    u32 c0 = m->c0, c1 = m->c4 & 0xFF; int i, s1 = 0, s2 = 0, p, pa1, pa2, pa3;
    m->sp[0] = m->t[0] + 2 * c0;
    m->sp[1] = m->t[1] + 2 * ((c1 << 8) | c0);
    for (i = 2; i < CM_NT; i++)
        m->sp[i] = m->t[i] + 2 * (((m->hh[i] ^ (c0 * 0x2545F491u)) * 0x9E3779B1u) >> (32 - m->tbits));
    for (i = 0; i < CM_NT; i++) {
        u32 s = m->sp[i][0], s2 = m->sp[i][1];
        m->x[2 * i] = (i < 2 || (s & 1023)) ? cm_st[s >> 20] : 0;
        m->x[2 * i + 1] = (i < 2 || (s2 & 1023)) ? cm_st[s2 >> 20] : 0;
    }
    m->msp = NULL; m->x[2 * CM_NT] = 0;
    if (m->mlen > 0 && ((m->mbyte | 256) >> (8 - m->bit)) == c0) {
        u32 l = m->mlen > 31 ? 31 : m->mlen, e = (m->mbyte >> (7 - m->bit)) & 1;
        m->msp = m->msm + l * 2 + e;
        m->x[2 * CM_NT] = cm_st[*m->msp >> 20];
    } else m->mlen = 0;
    m->x[2 * CM_NT + 1] = 256;
    m->cw1 = m->w1 + c0 * CM_NI; m->cw2 = m->w2 + ((m->mlen == 0 ? 0u : m->mlen < 8 ? 1u : m->mlen < 16 ? 2u : m->mlen < 32 ? 3u : 4u) * 256 + c1) * CM_NI;
    for (i = 0; i < CM_NI; i++) { s1 += (m->x[i] * m->cw1[i]) >> 8; s2 += (m->x[i] * m->cw2[i]) >> 8; }
    s1 = cm_clamp(s1 >> 8); s2 = cm_clamp(s2 >> 8);
    m->pr1 = cm_squash(s1); m->pr2 = cm_squash(s2);
    m->pm = cm_squash((s1 + s2) >> 1);
    pa1 = cm_apm(m->a1, m->pm, c0, &m->ai1);
    pa2 = cm_apm(m->a2, m->pm, c0 | (c1 << 8), &m->ai2);
    pa3 = cm_apm(m->a3, m->pm, (cm_hash(m->c4 & 0xFFFFFF, 11) >> 16 ^ c0 * 0x9E37u) & 0xFFFF, &m->ai3);
    p = (2 * m->pm + pa1 + pa2 + 2 * pa3 + 3) / 6;
    return p < 1 ? 1 : p > 4095 ? 4095 : p;
}

static void cm_up(CMP *m, int y) {
    int i, e1, e2, lr;
    for (i = 0; i < CM_NT; i++) { cm_sm(m->sp[i], y, cm_limf[i]); cm_sm(m->sp[i] + 1, y, cm_lims[i]); }
    if (m->msp) cm_sm(m->msp, y, 1023);
    lr = CM_LR + (int)(CM_LRB * 8192u / (m->pos + 8192u));
    e1 = ((y << 12) - m->pr1) * lr; e2 = ((y << 12) - m->pr2) * lr;
    for (i = 0; i < CM_NI; i++) {
        m->cw1[i] += (m->x[i] * e1 + 0x8000) >> 16;
        m->cw2[i] += (m->x[i] * e2 + 0x8000) >> 16;
    }
    cm_apm_up(m->a1, m->ai1, y); cm_apm_up(m->a2, m->ai2, y); cm_apm_up(m->a3, m->ai3, y);
    m->c0 = (m->c0 << 1) | (u32)y; m->bit++;
    if (m->bit == 8) {
        u32 c = m->c0 & 255, lc = c | 32, h;
        m->hist[m->pos++] = (u8)c;
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

/* returns compressed size, or 0 if it did not fit in cap / out of memory */
static size_t cm_compress(const u8 *in, size_t n, u8 *out, size_t cap, u32 tbits) {
    CMP *m = cm_new(tbits, n); size_t i, op = 0; u32 x1 = 0, x2 = 0xFFFFFFFFu; int b;
    if (!m) return 0;
    for (i = 0; i < n; i++)
        for (b = 7; b >= 0; b--) {
            int y = (in[i] >> b) & 1, p = cm_p(m);
            u32 xm = x1 + ((x2 - x1) >> 12) * (u32)p;
            if (y) x2 = xm; else x1 = xm + 1;
            cm_up(m, y);
            while (((x1 ^ x2) & 0xFF000000u) == 0) {
                if (op >= cap) { cm_free(m); return 0; }
                out[op++] = (u8)(x2 >> 24); x1 <<= 8; x2 = (x2 << 8) | 255;
            }
        }
    if (op + 4 > cap) { cm_free(m); return 0; }
    for (b = 0; b < 4; b++) { out[op++] = (u8)(x1 >> 24); x1 <<= 8; }
    cm_free(m);
    return op;
}
/* returns 0 on success */
static int cm_decompress(const u8 *in, size_t inn, u8 *out, size_t n, u32 tbits) {
    CMP *m = cm_new(tbits, n); size_t i, ip = 0; u32 x1 = 0, x2 = 0xFFFFFFFFu, x = 0; int b;
    if (!m) return 1;
    for (b = 0; b < 4; b++) x = (x << 8) | (ip < inn ? in[ip++] : 0);
    for (i = 0; i < n; i++) {
        u32 c = 0;
        for (b = 0; b < 8; b++) {
            int p = cm_p(m), y; u32 xm = x1 + ((x2 - x1) >> 12) * (u32)p;
            y = x <= xm;
            if (y) x2 = xm; else x1 = xm + 1;
            cm_up(m, y); c = (c << 1) | (u32)y;
            while (((x1 ^ x2) & 0xFF000000u) == 0) {
                x1 <<= 8; x2 = (x2 << 8) | 255; x = (x << 8) | (ip < inn ? in[ip++] : 0);
            }
        }
        out[i] = (u8)c;
    }
    cm_free(m);
    return 0;
}
