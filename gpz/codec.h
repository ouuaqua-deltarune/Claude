/*
 * GPZ block codec. This one file is compiled both as plain C (CPU path)
 * and as OpenCL C (GPU path), so both produce byte-identical output.
 *
 * Model: LZ77 (hash buckets, greedy or lazy) + LZMA-style adaptive binary
 * range coder. Integer only, no SIMD, no unaligned access, endian-neutral.
 * Every block is coded independently => one GPU work-item per block.
 */
#ifdef __OPENCL_VERSION__
  #define GP __global
  #define FN
  typedef uchar u8; typedef ushort u16; typedef uint u32; typedef ulong u64;
#else
  #define GP
  #define FN static
  typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
  typedef unsigned long long u64;
#endif

#define GPZ_FAIL      0xFFFFFFFFu
#define GPZ_MINLEN    3u
#define GPZ_MAXLEN    258u
#define GPZ_HBITS     11
#define GPZ_WAYS      4u
#define GPZ_HSIZE     ((1u << GPZ_HBITS) * GPZ_WAYS)   /* u32 entries per block */

#define GPZ_P_MATCH   0u                  /* 2   */
#define GPZ_P_LIT     2u                  /* 2048 (8 contexts x 256) */
#define GPZ_P_LEN     2050u               /* 32  */
#define GPZ_P_DIST    2082u               /* 128 (4 contexts x 32) */
#define GPZ_PTOT      2210u
#define GPZ_PSTRIDE   2304u               /* u16 entries per block */

/* ---------------- range encoder ---------------- */
typedef struct {
    GP u8 *out; u32 pos, cap; u64 low; u32 range, csize; u8 cache; int err;
} RCE;

FN void rce_byte(RCE *r, u8 b) {
    if (r->pos < r->cap) r->out[r->pos] = b; else r->err = 1;
    r->pos++;
}
FN void rce_shift(RCE *r) {
    if ((u32)r->low < 0xFF000000u || (u32)(r->low >> 32) != 0) {
        u8 carry = (u8)(r->low >> 32), t = r->cache;
        do { rce_byte(r, (u8)(t + carry)); t = 0xFF; } while (--r->csize != 0);
        r->cache = (u8)(r->low >> 24);
    }
    r->csize++;
    r->low = (r->low & 0x00FFFFFFu) << 8;
}
FN void rce_init(RCE *r, GP u8 *out, u32 cap) {
    r->out = out; r->cap = cap; r->pos = 0; r->low = 0; r->range = 0xFFFFFFFFu;
    r->csize = 1; r->cache = 0; r->err = 0;
}
FN void rce_bit(RCE *r, GP u16 *p, u32 bit) {
    u32 pr = *p, bound = (r->range >> 11) * pr;
    if (!bit) { r->range = bound; *p = (u16)(pr + ((2048u - pr) >> 5)); }
    else { r->low += bound; r->range -= bound; *p = (u16)(pr - (pr >> 5)); }
    while (r->range < (1u << 24)) { r->range <<= 8; rce_shift(r); }
}
FN void rce_direct(RCE *r, u32 bit) {
    r->range >>= 1;
    if (bit) r->low += r->range;
    while (r->range < (1u << 24)) { r->range <<= 8; rce_shift(r); }
}
FN void rce_flush(RCE *r) { int i; for (i = 0; i < 5; i++) rce_shift(r); }
FN void rce_tree(RCE *r, GP u16 *p, int nbits, u32 v) {
    u32 m = 1; int i;
    for (i = nbits - 1; i >= 0; i--) {
        u32 b = (v >> i) & 1u;
        rce_bit(r, p + m, b); m = (m << 1) | b;
    }
}
/* v >= 0, v+1 < 2^17: Elias-gamma shape, bit-length adaptive, mantissa raw */
FN void rce_num(RCE *r, GP u16 *p, u32 v) {
    u32 x = v + 1; int n = 0, i;
    while ((x >> (n + 1)) != 0) n++;
    rce_tree(r, p, 5, (u32)n);
    for (i = n - 1; i >= 0; i--) rce_direct(r, (x >> i) & 1u);
}

/* ---------------- range decoder ---------------- */
typedef struct { GP const u8 *in; u32 pos, len, code, range; int err; } RCD;

FN u32 rcd_byte(RCD *r) {
    if (r->pos < r->len) return r->in[r->pos++];
    r->err = 1; return 0;
}
FN void rcd_init(RCD *r, GP const u8 *in, u32 len) {
    int i;
    r->in = in; r->pos = 0; r->len = len; r->err = 0; r->code = 0;
    r->range = 0xFFFFFFFFu;
    (void)rcd_byte(r);
    for (i = 0; i < 4; i++) r->code = (r->code << 8) | rcd_byte(r);
}
FN u32 rcd_bit(RCD *r, GP u16 *p) {
    u32 pr = *p, bound = (r->range >> 11) * pr, b;
    if (r->code < bound) { r->range = bound; *p = (u16)(pr + ((2048u - pr) >> 5)); b = 0; }
    else { r->code -= bound; r->range -= bound; *p = (u16)(pr - (pr >> 5)); b = 1; }
    while (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | rcd_byte(r); }
    return b;
}
FN u32 rcd_direct(RCD *r) {
    u32 b = 0;
    r->range >>= 1;
    if (r->code >= r->range) { r->code -= r->range; b = 1; }
    while (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | rcd_byte(r); }
    return b;
}
FN u32 rcd_tree(RCD *r, GP u16 *p, int nbits) {
    u32 m = 1; int i;
    for (i = 0; i < nbits; i++) m = (m << 1) | rcd_bit(r, p + m);
    return m - (1u << nbits);
}
FN u32 rcd_num(RCD *r, GP u16 *p) {
    u32 n = rcd_tree(r, p, 5), x = 1, i;
    if (n > 16) { r->err = 1; return 0; }
    for (i = 0; i < n; i++) x = (x << 1) | rcd_direct(r);
    return x - 1;
}

/* ---------------- model / match finder ---------------- */
FN void gpz_pinit(GP u16 *P) { u32 i; for (i = 0; i < GPZ_PTOT; i++) P[i] = 1024; }

FN u32 gpz_hash(GP const u8 *s, u32 i) {
    u32 v = ((u32)s[i] << 16) | ((u32)s[i + 1] << 8) | (u32)s[i + 2];
    return (v * 2654435761u) >> (32 - GPZ_HBITS);
}
FN void gpz_ins(GP const u8 *s, u32 i, u32 n, GP u32 *H) {
    GP u32 *b;
    if (i + GPZ_MINLEN > n) return;
    b = H + gpz_hash(s, i) * GPZ_WAYS;
    b[3] = b[2]; b[2] = b[1]; b[1] = b[0]; b[0] = i + 1;
}
FN u32 gpz_find(GP const u8 *s, u32 i, u32 n, GP u32 *H, u32 *dist) {
    GP u32 *b = H + gpz_hash(s, i) * GPZ_WAYS;
    u32 best = 0, k, maxl = n - i;
    if (maxl > GPZ_MAXLEN) maxl = GPZ_MAXLEN;
    for (k = 0; k < GPZ_WAYS; k++) {
        u32 c = b[k], p, l = 0;
        if (c == 0) break;
        p = c - 1;
        while (l < maxl && s[p + l] == s[i + l]) l++;
        if (l > best) { best = l; *dist = i - p; }
    }
    if (best == GPZ_MINLEN && *dist > 4096u) best = 0;
    return best;
}
FN u32 gpz_lenctx(u32 len) { return len == 3 ? 0u : (len <= 5 ? 1u : (len <= 9 ? 2u : 3u)); }

/* Compress one block. Returns size, or GPZ_FAIL if it does not fit in cap. */
FN u32 gpz_enc_block(GP const u8 *s, u32 n, GP u8 *out, u32 cap,
                     GP u16 *P, GP u32 *H, u32 lazy) {
    RCE r; u32 i = 0, nextins = 0, prevm = 0, prevb = 0, k;
    for (k = 0; k < GPZ_HSIZE; k++) H[k] = 0;
    gpz_pinit(P);
    rce_init(&r, out, cap);
    while (i < n && !r.err) {
        u32 len = 0, dist = 0, l2, d2 = 0;
        while (nextins < i) { gpz_ins(s, nextins, n, H); nextins++; }
        if (i + GPZ_MINLEN <= n) len = gpz_find(s, i, n, H, &dist);
        if (len >= GPZ_MINLEN && lazy && i + 1 + GPZ_MINLEN <= n) {
            while (nextins < i + 1) { gpz_ins(s, nextins, n, H); nextins++; }
            l2 = gpz_find(s, i + 1, n, H, &d2);
            if (l2 > len) len = 0;
        }
        if (len >= GPZ_MINLEN) {
            rce_bit(&r, P + GPZ_P_MATCH + prevm, 1);
            rce_num(&r, P + GPZ_P_LEN, len - GPZ_MINLEN);
            rce_num(&r, P + GPZ_P_DIST + gpz_lenctx(len) * 32u, dist - 1);
            prevb = s[i + len - 1]; prevm = 1; i += len;
        } else {
            rce_bit(&r, P + GPZ_P_MATCH + prevm, 0);
            rce_tree(&r, P + GPZ_P_LIT + (prevb >> 5) * 256u, 8, s[i]);
            prevb = s[i]; prevm = 0; i++;
        }
    }
    rce_flush(&r);
    return r.err ? GPZ_FAIL : r.pos;
}

/* Decompress one block of known raw size n. Returns 0 on success. */
FN u32 gpz_dec_block(GP const u8 *in, u32 ilen, GP u8 *o, u32 n, GP u16 *P) {
    RCD r; u32 i = 0, prevm = 0, prevb = 0;
    gpz_pinit(P);
    rcd_init(&r, in, ilen);
    while (i < n) {
        if (r.err) return 1;
        if (rcd_bit(&r, P + GPZ_P_MATCH + prevm)) {
            u32 len = rcd_num(&r, P + GPZ_P_LEN) + GPZ_MINLEN, dist, k;
            dist = rcd_num(&r, P + GPZ_P_DIST + gpz_lenctx(len) * 32u) + 1;
            if (r.err || dist > i || len > n - i) return 1;
            for (k = 0; k < len; k++) o[i + k] = o[i + k - dist];
            prevb = o[i + len - 1]; prevm = 1; i += len;
        } else {
            u8 c = (u8)rcd_tree(&r, P + GPZ_P_LIT + (prevb >> 5) * 256u, 8);
            o[i++] = c; prevb = c; prevm = 0;
        }
    }
    return r.err ? 1 : 0;
}

#ifdef __OPENCL_VERSION__
__kernel void gpz_compress_k(__global const uchar *src, uint batch_len, uint bs,
                             __global uchar *out, uint cap, __global uint *osz,
                             __global ushort *Pall, __global uint *Hall,
                             uint nblk, uint lazy, uint first) {
    uint g = first + (uint)get_global_id(0), off, n;
    if (g >= nblk) return;
    off = g * bs; n = batch_len - off; if (n > bs) n = bs;
    osz[g] = gpz_enc_block(src + off, n, out + (size_t)g * cap, cap,
                           Pall + (size_t)g * GPZ_PSTRIDE,
                           Hall + (size_t)g * GPZ_HSIZE, lazy);
}
/* tab[g]: bit31 = stored, low 31 bits = compressed length; ioff[g] = offset in `in` */
__kernel void gpz_decompress_k(__global const uchar *in, __global const uint *ioff,
                               __global const uint *tab, uint raw_len, uint bs,
                               __global uchar *out, __global uint *status,
                               __global ushort *Pall, uint nblk, uint first) {
    uint g = first + (uint)get_global_id(0), off, n, e, len, k;
    if (g >= nblk) return;
    off = g * bs; n = raw_len - off; if (n > bs) n = bs;
    e = tab[g]; len = e & 0x7FFFFFFFu;
    if (e >> 31) {
        if (len != n) { status[g] = 1; return; }
        for (k = 0; k < n; k++) out[(size_t)g * bs + k] = in[ioff[g] + k];
        status[g] = 0;
    } else {
        status[g] = gpz_dec_block(in + ioff[g], len, out + (size_t)g * bs, n,
                                  Pall + (size_t)g * GPZ_PSTRIDE);
    }
}
#endif
