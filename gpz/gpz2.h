/*
 * GPZ2 container: context-mixing codec + optional "undeflate" transform.
 *
 * Transform: scans for gzip members and zip entries, inflates each deflate
 * stream, then searches zlib settings (level 1-9, memLevel 8/9) that
 * re-create the ORIGINAL compressed bytes exactly. If found, the stream is
 * replaced by its raw data (which the CM codec compresses far better) plus
 * the settings. Streams that cannot be reproduced are kept verbatim.
 * zlib is loaded at run time (dlopen); without it the transform is skipped.
 */
#include "cmcore.h"

typedef struct {
    const unsigned char *next_in; unsigned avail_in; unsigned long total_in;
    unsigned char *next_out; unsigned avail_out; unsigned long total_out;
    const char *msg; void *state; void *zalloc, *zfree, *opaque;
    int data_type; unsigned long adler, reserved;
} zs_t;

static struct {
    const char *(*Version)(void);
    int (*InflateInit2)(zs_t *, int, const char *, int);
    int (*Inflate)(zs_t *, int);
    int (*InflateEnd)(zs_t *);
    int (*DeflateInit2)(zs_t *, int, int, int, int, int, const char *, int);
    int (*Deflate)(zs_t *, int);
    int (*DeflateEnd)(zs_t *);
} zl;

static int zl_load(void) {
    static int tried, ok; void *h;
    if (tried) return ok;
    tried = 1;
#ifdef _WIN32
    h = (void *)LoadLibraryA("zlib1.dll");
  #define ZSYM(n) (void *)GetProcAddress((HMODULE)h, n)
#else
    h = dlopen("libz.so.1", RTLD_LAZY);
    if (!h) h = dlopen("libz.dylib", RTLD_LAZY);
    if (!h) h = dlopen("libz.1.dylib", RTLD_LAZY);
    if (!h) h = dlopen("libz.so", RTLD_LAZY);
  #define ZSYM(n) dlsym(h, n)
#endif
    if (!h) return 0;
    *(void **)&zl.Version = ZSYM("zlibVersion");
    *(void **)&zl.InflateInit2 = ZSYM("inflateInit2_");
    *(void **)&zl.Inflate = ZSYM("inflate");
    *(void **)&zl.InflateEnd = ZSYM("inflateEnd");
    *(void **)&zl.DeflateInit2 = ZSYM("deflateInit2_");
    *(void **)&zl.Deflate = ZSYM("deflate");
    *(void **)&zl.DeflateEnd = ZSYM("deflateEnd");
    if (!zl.Version || !zl.InflateInit2 || !zl.Inflate || !zl.InflateEnd ||
        !zl.DeflateInit2 || !zl.Deflate || !zl.DeflateEnd) return 0;
    ok = 1; return 1;
}

#define Z_FINISH 4
#define Z_STREAM_END 1
#define MAXRAW ((size_t)1 << 30)

/* Inflate a deflate stream (wbits -15 raw, 15 zlib-wrapped). Returns raw buffer or NULL. */
static u8 *z_inflate(const u8 *in, size_t avail, size_t *rawn, size_t *used, int wbits) {
    zs_t z; size_t cap = avail * 4 + 4096, len = 0; u8 *o; int r;
    if (cap > ((size_t)1 << 20)) cap = (size_t)1 << 20;     /* grows on demand */
    o = (u8 *)malloc(cap);
    if (!o) return NULL;
    memset(&z, 0, sizeof z);
    if (zl.InflateInit2(&z, wbits, zl.Version(), (int)sizeof z) != 0) { free(o); return NULL; }
    z.next_in = in; z.avail_in = (unsigned)(avail > 0x7FFFFFFFu ? 0x7FFFFFFFu : avail);
    for (;;) {
        if (len == cap) {
            u8 *nb; if (cap >= MAXRAW) { r = -1; break; }
            cap *= 2; nb = (u8 *)realloc(o, cap); if (!nb) { r = -1; break; } o = nb;
        }
        z.next_out = o + len; z.avail_out = (unsigned)(cap - len > 0x40000000u ? 0x40000000u : cap - len);
        r = zl.Inflate(&z, 0);
        len = cap - z.avail_out;
        if (r == Z_STREAM_END) break;
        if (r != 0 && r != -5) break;                    /* error */
        if (r == -5 && z.avail_in == 0) break;           /* truncated */
    }
    *used = (size_t)z.total_in; *rawn = len;
    zl.InflateEnd(&z);
    if (r != Z_STREAM_END || len == 0) { free(o); return NULL; }
    return o;
}

/* Deflate raw[0..n) with (level, mem, wbits). With ref: succeed only if the output equals
   ref[0..refn) exactly (aborting at the first difference). Without ref: write into dst. */
static int z_deflate(const u8 *raw, size_t n, int level, int mem, int wbits, const u8 *ref, size_t refn,
                     u8 *dst, size_t dstn) {
    zs_t z; u8 buf[1 << 16]; size_t tot = 0; int r;
    memset(&z, 0, sizeof z);
    if (zl.DeflateInit2(&z, level, 8, wbits, mem, 0, zl.Version(), (int)sizeof z) != 0) return 0;
    z.next_in = raw; z.avail_in = (unsigned)n;
    do {
        size_t got;
        z.next_out = buf; z.avail_out = sizeof buf;
        r = zl.Deflate(&z, Z_FINISH);
        got = sizeof buf - z.avail_out;
        if (ref) {
            if (tot + got > refn || memcmp(buf, ref + tot, got)) { zl.DeflateEnd(&z); return 0; }
        } else {
            if (tot + got > dstn) { zl.DeflateEnd(&z); return 0; }
            memcpy(dst + tot, buf, got);
        }
        tot += got;
    } while (r == 0);
    zl.DeflateEnd(&z);
    if (r != Z_STREAM_END) return 0;
    if (ref && tot != refn) return 0;
    return 1;
}

typedef struct { u8 type, level, mem; u32 len, dlen, dcrc; } Seg;   /* type 0 verbatim, 1 raw deflate, 2 zlib */

typedef struct {                       /* transform builder */
    Seg *segs; size_t ns, nsc; u8 *payload; size_t pn, pcap, vstart; int nr, nf;
} TxB;

static void put_seg(u8 **p, size_t *n, size_t *cap, const u8 *d, size_t k) {
    if (*n + k > *cap) {
        *cap = (*n + k) * 2 + 1024; *p = (u8 *)realloc(*p, *cap);
        if (!*p) die("out of memory");
    }
    memcpy(*p + *n, d, k); *n += k;
}
static void txb_seg(TxB *b, Seg s) {
    if (b->ns + 1 > b->nsc) { b->nsc = b->nsc * 2 + 16; b->segs = (Seg *)realloc(b->segs, b->nsc * sizeof *b->segs); if (!b->segs) die("out of memory"); }
    b->segs[b->ns++] = s;
}

static const int tryorder[][2] = {{6,8},{9,8},{1,8},{2,8},{3,8},{4,8},{5,8},{7,8},{8,8},
                                  {6,9},{9,9},{1,9},{2,9},{3,9},{4,9},{5,9},{7,9},{8,9}};

/* Try to replace the deflate stream at in[dstart..dstart+avail) by its raw data. Returns the
   number of input bytes consumed by the stream if it was replaced, else 0. */
static size_t txb_try(TxB *b, const u8 *in, size_t dstart, size_t avail, int wbits) {
    size_t rawn, used, i; u8 *raw = z_inflate(in + dstart, avail, &rawn, &used, wbits); int found = -1;
    Seg a, c;
    if (!raw) return 0;
    for (i = 0; i < sizeof tryorder / sizeof *tryorder; i++)
        if (z_deflate(raw, rawn, tryorder[i][0], tryorder[i][1], wbits, in + dstart, used, NULL, 0)) { found = (int)i; break; }
    if (found < 0) { b->nf++; free(raw); return 0; }
    a.type = 0; a.level = a.mem = 0; a.len = (u32)(dstart - b->vstart); a.dlen = a.dcrc = 0;
    c.type = wbits > 0 ? 2 : 1; c.level = (u8)tryorder[found][0]; c.mem = (u8)tryorder[found][1];
    c.len = (u32)rawn; c.dlen = (u32)used; c.dcrc = crc32(in + dstart, used);
    txb_seg(b, a); txb_seg(b, c);
    put_seg(&b->payload, &b->pn, &b->pcap, in + b->vstart, dstart - b->vstart);
    put_seg(&b->payload, &b->pn, &b->pcap, raw, rawn);
    b->nr++; b->vstart = dstart + used; free(raw);
    return used;
}
/* Serialize: [u32 nseg][segs][payload]. Frees the builder's buffers. */
static u8 *txb_finish(TxB *b, const u8 *in, size_t n, size_t *outn) {
    Seg a; u8 *res, *q; size_t i;
    a.type = 0; a.level = a.mem = 0; a.len = (u32)(n - b->vstart); a.dlen = a.dcrc = 0;
    txb_seg(b, a);
    put_seg(&b->payload, &b->pn, &b->pcap, in + b->vstart, n - b->vstart);
    res = (u8 *)malloc(4 + b->ns * 15 + b->pn + 1);
    if (!res) die("out of memory");
    put32(res, (u32)b->ns); q = res + 4;
    for (i = 0; i < b->ns; i++) {
        *q++ = b->segs[i].type; put32(q, b->segs[i].len); q += 4;
        if (b->segs[i].type) { *q++ = b->segs[i].level; *q++ = b->segs[i].mem; put32(q, b->segs[i].dlen); q += 4; put32(q, b->segs[i].dcrc); q += 4; }
    }
    memcpy(q, b->payload, b->pn); *outn = (size_t)(q - res) + b->pn;
    free(b->segs); free(b->payload);
    return res;
}

/* gzip members and zip entries: scan for signatures. */
static u8 *tx_build(const u8 *in, size_t n, size_t *outn, int *nrecomp, int *nfail) {
    TxB b; size_t pos = 0; u8 *res;
    memset(&b, 0, sizeof b);
    while (pos < n) {
        size_t dstart = 0, used;
        if (in[pos] == 0x1F && pos + 18 <= n && in[pos + 1] == 0x8B && in[pos + 2] == 8) {
            u8 flg = in[pos + 3]; size_t p = pos + 10;
            if (flg & 4) { if (p + 2 <= n) p += 2 + (in[p] | (in[p + 1] << 8)); }
            if (flg & 8) { while (p < n && in[p]) p++; p++; }
            if (flg & 16) { while (p < n && in[p]) p++; p++; }
            if (flg & 2) p += 2;
            if (p < n) dstart = p;
        } else if (in[pos] == 'P' && pos + 30 <= n && in[pos + 1] == 'K' && in[pos + 2] == 3 &&
                   in[pos + 3] == 4 && (in[pos + 8] | (in[pos + 9] << 8)) == 8) {
            dstart = pos + 30 + (in[pos + 26] | (in[pos + 27] << 8)) + (in[pos + 28] | (in[pos + 29] << 8));
            if (dstart >= n) dstart = 0;
        }
        if (dstart && (used = txb_try(&b, in, dstart, n - dstart, -15)) != 0) { pos = dstart + used; continue; }
        pos++;
    }
    *nrecomp = b.nr; *nfail = b.nf;
    res = txb_finish(&b, in, n, outn);
    return res;
}

/* Minecraft Anvil region files (.mca): chunks are zlib streams inside 4 KiB sectors. */
static int mca_cmp(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}
static u8 *tx_build_mca(const u8 *in, size_t n, size_t *outn, int *nrecomp, int *nfail) {
    TxB b; size_t starts[1024], avail[1024], nc = 0, i, idx[1024], last_end = 0; u8 *res;
    memset(&b, 0, sizeof b);
    for (i = 0; i < 1024 && n >= 8192; i++) {
        u32 loc = ((u32)in[4 * i] << 24) | ((u32)in[4 * i + 1] << 16) | ((u32)in[4 * i + 2] << 8) | in[4 * i + 3];
        size_t off = (size_t)(loc >> 8) * 4096, len;
        if (!loc || off < 8192 || off + 5 > n) continue;
        len = ((size_t)in[off] << 24) | ((size_t)in[off + 1] << 16) | ((size_t)in[off + 2] << 8) | in[off + 3];
        if (len < 2 || off + 4 + len > n || in[off + 4] != 2) continue;          /* 2 = zlib */
        starts[nc] = off + 5; avail[nc] = len - 1; nc++;
    }
    for (i = 0; i < nc; i++) idx[i] = starts[i];
    qsort(idx, nc, sizeof idx[0], mca_cmp);
    for (i = 0; i < nc; i++) {                          /* map back to avail[] via search */
        size_t j, av = 0;
        for (j = 0; j < nc; j++) if (starts[j] == idx[i]) { av = avail[j]; break; }
        if (idx[i] < last_end || idx[i] < b.vstart) continue;
        {
            size_t used = txb_try(&b, in, idx[i], av, 15);
            last_end = used ? idx[i] + used : idx[i] + av;
        }
    }
    *nrecomp = b.nr; *nfail = b.nf;
    res = txb_finish(&b, in, n, outn);
    return res;
}

/* Inverse. out has capacity osz. Returns 0 on success. */
static int tx_restore(const u8 *t, size_t tn, u8 *out, size_t osz) {
    u32 ns, i; const u8 *q = t + 4, *pay; size_t op = 0;
    if (tn < 4) return 1;
    ns = get32(t);
    for (i = 0; i < ns; i++) {                           /* locate payload */
        if ((size_t)(q - t) + 5 > tn) return 1;
        if (*q) q += 15; else q += 5;
    }
    pay = q; q = t + 4;
    for (i = 0; i < ns; i++) {
        u8 type = *q; u32 len = get32(q + 1);
        if (type == 0) {
            q += 5;
            if ((size_t)(pay - t) + len > tn || op + len > osz) return 1;
            memcpy(out + op, pay, len); pay += len; op += len;
        } else {
            u8 lv = q[5], mem = q[6]; u32 dlen = get32(q + 7), dcrc = get32(q + 11);
            q += 15;
            if ((size_t)(pay - t) + len > tn || op + dlen > osz) return 1;
            if (!zl_load()) { fprintf(stderr, "gpz: this archive needs zlib to restore deflate streams, which is not installed\n"); return 1; }
            if (!z_deflate(pay, len, lv, mem, type == 2 ? 15 : -15, NULL, 0, out + op, dlen) || crc32(out + op, dlen) != dcrc) {
                fprintf(stderr, "gpz: zlib here does not reproduce the original deflate bytes (different zlib build)\n"); return 1;
            }
            pay += len; op += dlen;
        }
    }
    return op == osz ? 0 : 1;
}

/* ---- GPZ2 file: "GPZ2" flags(1) tbits(1) rsv(2) tsize(4) osize(4) crc(4) payload ---- */
#define F2_TX 1
#define F2_STORED 2

static void do_compress2(const char *in, const char *outp, int use_tx) {
    size_t n, tn, cn, cap; u8 *src = slurp(in, &n), *t = src, *c, hdr[20]; u32 tbits = 16, flags = 0;
    int nr = 0, nf = 0; double t0 = now();
    if (n > 0x7FFFFFFFu) die("file too large for this build");
    if (use_tx && n >= 30 && zl_load()) {
        t = tx_build(src, n, &tn, &nr, &nf);
        if (nr) flags |= F2_TX; else { free(t); t = src; }
    } else if (use_tx && !zl_load() && verbose) fprintf(stderr, "gpz: zlib not found, skipping deflate recompression\n");
    if (!(flags & F2_TX)) tn = n;
    if (tn > 0x7FFFFFFFu) die("data too large for this build");
    while (tbits < 21 && ((size_t)1 << tbits) < tn * 2) tbits++;
    cap = tn + tn / 4 + 1024; c = (u8 *)malloc(cap);
    if (!c) die("out of memory");
    cn = cm_compress(t, tn, c, cap, tbits);
    if (cn == 0 || cn >= tn) { flags |= F2_STORED; cn = tn; memcpy(c, t, tn); }
    memcpy(hdr, "GPZ2", 4); hdr[4] = (u8)flags; hdr[5] = (u8)tbits; hdr[6] = hdr[7] = 0;
    put32(hdr + 8, (u32)tn); put32(hdr + 12, (u32)n); put32(hdr + 16, crc32(src, n));
    spit(outp, hdr, 20, c, cn, NULL, 0);
    fprintf(stderr, "gpz: %s -> %s: %lu -> %lu bytes (%.1f%%) max-ratio CPU, %d deflate stream(s) recompressed, %d kept as-is, %.2fs\n",
            in, outp, (unsigned long)n, (unsigned long)(20 + cn), n ? 100.0 * (20 + cn) / n : 100.0, nr, nf, now() - t0);
    if (t != src) free(t);
    free(src); free(c);
}

static void do_decompress2(const u8 *f, size_t n, const char *outp, const char *inname) {
    u32 flags = f[4], tbits = f[5], tn = get32(f + 8), osz = get32(f + 12); u8 *t, *o; double t0 = now();
    if (n < 20 || tbits < 4 || tbits > 26) die("bad GPZ2 header");
    t = (u8 *)malloc((size_t)tn + 1);
    if (!t) die("out of memory");
    if (flags & F2_STORED) { if (n - 20 != tn) die("truncated file"); memcpy(t, f + 20, tn); }
    else if (cm_decompress(f + 20, n - 20, t, tn, tbits)) die("corrupt data");
    if (flags & F2_TX) {
        o = (u8 *)malloc((size_t)osz + 1);
        if (!o) die("out of memory");
        if (tx_restore(t, tn, o, osz)) die("corrupt data or cannot restore deflate streams");
        free(t);
    } else { o = t; if (tn != osz) die("corrupt data"); }
    if (crc32(o, osz) != get32(f + 16)) die("checksum mismatch");
    spit(outp, o, osz, NULL, 0, NULL, 0);
    fprintf(stderr, "gpz: %s -> %s: %lu bytes (max-ratio CPU) in %.2fs\n", inname, outp, (unsigned long)osz, now() - t0);
    free(o);
}
