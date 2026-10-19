/*
 * gpz - block compressor that can run entirely on the CPU or on any
 * OpenCL GPU (AMD, NVIDIA, Intel, ...). OpenCL is loaded at run time, so
 * the binary starts fine on machines without a GPU or OpenCL driver.
 * Plain C99, 32-bit safe, no SIMD, no unaligned access, endian-neutral.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

#include "codec.h"
#include "codec_cl.inc"          /* generated: GPZ_CL_SRC (codec.h as a string) */

#define MAGIC "GPZ1"
#define HDR 20
#define BATCH 256u

static int verbose;

/* ---------------- helpers ---------------- */
static void die(const char *m) { fprintf(stderr, "gpz: %s\n", m); exit(1); }
static void put32(u8 *p, u32 v) { p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }
static u32 get32(const u8 *p) { return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }

static u32 crc32(const u8 *d, size_t n) {
    static u32 t[256]; u32 c = 0xFFFFFFFFu; size_t i; int k;
    if (!t[1]) for (i = 0; i < 256; i++) {
        u32 x = (u32)i; for (k = 0; k < 8; k++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
        t[i] = x;
    }
    for (i = 0; i < n; i++) c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static double now(void) {   /* wall clock: clock() would miss time spent waiting on the GPU */
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* ---------------- OpenCL, loaded dynamically ---------------- */
typedef int cl_int; typedef unsigned int cl_uint; typedef unsigned long long cl_ulong;
typedef struct _p *cl_platform; typedef struct _d *cl_device; typedef struct _c *cl_context;
typedef struct _q *cl_queue; typedef struct _m *cl_mem; typedef struct _g *cl_program;
typedef struct _k *cl_kernel;
#define CL_DEVICE_TYPE_GPU 4ull
#define CL_DEVICE_TYPE_ALL 0xFFFFFFFFull
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_VENDOR 0x102C
#define CL_DEVICE_TYPE 0x1000
#define CL_DEVICE_GLOBAL_MEM_SIZE 0x101F
#define CL_PROGRAM_BUILD_LOG 0x1183
#define CL_MEM_READ_WRITE 1ull
#define CL_MEM_WRITE_ONLY 2ull
#define CL_MEM_READ_ONLY 4ull

static struct {
    cl_int (*GetPlatformIDs)(cl_uint, cl_platform *, cl_uint *);
    cl_int (*GetDeviceIDs)(cl_platform, cl_ulong, cl_uint, cl_device *, cl_uint *);
    cl_int (*GetDeviceInfo)(cl_device, cl_uint, size_t, void *, size_t *);
    cl_context (*CreateContext)(const void *, cl_uint, const cl_device *, void *, void *, cl_int *);
    cl_queue (*CreateCommandQueue)(cl_context, cl_device, cl_ulong, cl_int *);
    cl_program (*CreateProgramWithSource)(cl_context, cl_uint, const char **, const size_t *, cl_int *);
    cl_int (*BuildProgram)(cl_program, cl_uint, const cl_device *, const char *, void *, void *);
    cl_int (*GetProgramBuildInfo)(cl_program, cl_device, cl_uint, size_t, void *, size_t *);
    cl_kernel (*CreateKernel)(cl_program, const char *, cl_int *);
    cl_mem (*CreateBuffer)(cl_context, cl_ulong, size_t, void *, cl_int *);
    cl_int (*SetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
    cl_int (*EnqueueWriteBuffer)(cl_queue, cl_mem, cl_uint, size_t, size_t, const void *, cl_uint, const void *, void *);
    cl_int (*EnqueueReadBuffer)(cl_queue, cl_mem, cl_uint, size_t, size_t, void *, cl_uint, const void *, void *);
    cl_int (*EnqueueNDRangeKernel)(cl_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const void *, void *);
    cl_int (*Finish)(cl_queue);
    cl_int (*ReleaseMemObject)(cl_mem);
} cl;

static int cl_load(void) {
    void *h; static int tried, ok;
    if (tried) return ok;
    tried = 1;
#ifdef _WIN32
    h = (void *)LoadLibraryA("OpenCL.dll");
  #define SYM(f) (void *)GetProcAddress((HMODULE)h, "cl" #f)
#else
    h = dlopen("libOpenCL.so.1", RTLD_LAZY);
    if (!h) h = dlopen("libOpenCL.so", RTLD_LAZY);
    if (!h) h = dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_LAZY);
  #define SYM(f) dlsym(h, "cl" #f)
#endif
    if (!h) return 0;
#define LOAD(f) do { *(void **)&cl.f = SYM(f); if (!cl.f) return 0; } while (0)
    LOAD(GetPlatformIDs); LOAD(GetDeviceIDs); LOAD(GetDeviceInfo); LOAD(CreateContext);
    LOAD(CreateCommandQueue); LOAD(CreateProgramWithSource); LOAD(BuildProgram);
    LOAD(GetProgramBuildInfo); LOAD(CreateKernel); LOAD(CreateBuffer); LOAD(SetKernelArg);
    LOAD(EnqueueWriteBuffer); LOAD(EnqueueReadBuffer); LOAD(EnqueueNDRangeKernel);
    LOAD(Finish); LOAD(ReleaseMemObject);
    ok = 1; return 1;
}

#define MAXDEV 16
typedef struct { cl_device dev; char name[128], vendor[128]; cl_ulong type, mem; } DevInfo;

static int list_devices(DevInfo *out, int gpu_only) {
    cl_platform pl[8]; cl_uint np = 0, p, nd, d; int n = 0;
    if (!cl_load() || cl.GetPlatformIDs(8, pl, &np) != 0) return 0;
    for (p = 0; p < np; p++) {
        cl_device dv[MAXDEV];
        if (cl.GetDeviceIDs(pl[p], gpu_only ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_ALL, MAXDEV, dv, &nd) != 0) continue;
        for (d = 0; d < nd && n < MAXDEV; d++) {
            DevInfo *o = &out[n++];
            o->dev = dv[d]; o->name[0] = o->vendor[0] = 0; o->type = 0; o->mem = 0;
            cl.GetDeviceInfo(dv[d], CL_DEVICE_NAME, sizeof o->name, o->name, NULL);
            cl.GetDeviceInfo(dv[d], CL_DEVICE_VENDOR, sizeof o->vendor, o->vendor, NULL);
            cl.GetDeviceInfo(dv[d], CL_DEVICE_TYPE, sizeof o->type, &o->type, NULL);
            cl.GetDeviceInfo(dv[d], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof o->mem, &o->mem, NULL);
        }
    }
    return n;
}

typedef struct {
    cl_context ctx; cl_queue q; cl_program prog; cl_kernel kc, kd;
    cl_mem src, out, osz, P, H, ioff, tab, st;   /* batch buffers */
    u32 bs, cap;
} Gpu;

static void clcheck(cl_int e, const char *what) {
    if (e != 0) { fprintf(stderr, "gpz: OpenCL error %d in %s\n", e, what); exit(1); }
}
static cl_mem mkbuf(Gpu *g, cl_ulong fl, size_t sz, const char *w) {
    cl_int e; cl_mem m = cl.CreateBuffer(g->ctx, fl, sz ? sz : 4, NULL, &e);
    clcheck(e, w); return m;
}

static void gpu_open(Gpu *g, cl_device dev, u32 bs) {
    cl_int e; const char *srcs[1]; size_t len; char *log; size_t ll = 0;
    memset(g, 0, sizeof *g);
    g->bs = bs; g->cap = bs;
    g->ctx = cl.CreateContext(NULL, 1, &dev, NULL, NULL, &e); clcheck(e, "context");
    g->q = cl.CreateCommandQueue(g->ctx, dev, 0, &e); clcheck(e, "queue");
    srcs[0] = GPZ_CL_SRC; len = strlen(GPZ_CL_SRC);
    g->prog = cl.CreateProgramWithSource(g->ctx, 1, srcs, &len, &e); clcheck(e, "program");
    e = cl.BuildProgram(g->prog, 1, &dev, "", NULL, NULL);
    if (e != 0) {
        cl.GetProgramBuildInfo(g->prog, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &ll);
        log = (char *)calloc(ll + 1, 1);
        cl.GetProgramBuildInfo(g->prog, dev, CL_PROGRAM_BUILD_LOG, ll, log, NULL);
        fprintf(stderr, "gpz: kernel build failed:\n%s\n", log); exit(1);
    }
    g->kc = cl.CreateKernel(g->prog, "gpz_compress_k", &e); clcheck(e, "kernel c");
    g->kd = cl.CreateKernel(g->prog, "gpz_decompress_k", &e); clcheck(e, "kernel d");
    g->src  = mkbuf(g, CL_MEM_READ_WRITE, (size_t)BATCH * bs, "buf src");
    g->out  = mkbuf(g, CL_MEM_READ_WRITE, (size_t)BATCH * g->cap, "buf out");
    g->osz  = mkbuf(g, CL_MEM_READ_WRITE, BATCH * 4, "buf osz");
    g->P    = mkbuf(g, CL_MEM_READ_WRITE, (size_t)BATCH * GPZ_PSTRIDE * 2, "buf P");
    g->H    = mkbuf(g, CL_MEM_READ_WRITE, (size_t)BATCH * GPZ_HSIZE * 4, "buf H");
    g->ioff = mkbuf(g, CL_MEM_READ_ONLY, BATCH * 4, "buf ioff");
    g->tab  = mkbuf(g, CL_MEM_READ_ONLY, BATCH * 4, "buf tab");
    g->st   = mkbuf(g, CL_MEM_READ_WRITE, BATCH * 4, "buf st");
}

#define ARG(k, i, v) clcheck(cl.SetKernelArg(k, i, sizeof(v), &(v)), "arg")
#define ARGM(k, i, m) clcheck(cl.SetKernelArg(k, i, sizeof(cl_mem), &(m)), "arg")

/* Compress `nblk` blocks (batch_len bytes at src). osz[i] = size or GPZ_FAIL. */
static void gpu_compress(Gpu *g, const u8 *src, u32 batch_len, u32 nblk, u8 *out, u32 *osz, u32 lazy) {
    size_t gws = nblk; u32 bl = batch_len, bs = g->bs, cap = g->cap, nb = nblk;
    clcheck(cl.EnqueueWriteBuffer(g->q, g->src, 1, 0, batch_len, src, 0, NULL, NULL), "write");
    ARGM(g->kc, 0, g->src); ARG(g->kc, 1, bl); ARG(g->kc, 2, bs); ARGM(g->kc, 3, g->out);
    ARG(g->kc, 4, cap); ARGM(g->kc, 5, g->osz); ARGM(g->kc, 6, g->P); ARGM(g->kc, 7, g->H);
    ARG(g->kc, 8, nb); ARG(g->kc, 9, lazy);
    clcheck(cl.EnqueueNDRangeKernel(g->q, g->kc, 1, NULL, &gws, NULL, 0, NULL, NULL), "run compress");
    clcheck(cl.Finish(g->q), "finish");
    clcheck(cl.EnqueueReadBuffer(g->q, g->osz, 1, 0, (size_t)nblk * 4, osz, 0, NULL, NULL), "read sizes");
    clcheck(cl.EnqueueReadBuffer(g->q, g->out, 1, 0, (size_t)nblk * cap, out, 0, NULL, NULL), "read out");
}

/* Decompress nblk blocks. `in` = their concatenated payload (in_len bytes). */
static int gpu_decompress(Gpu *g, const u8 *in, u32 in_len, const u32 *tab, u32 nblk,
                          u32 raw_len, u8 *out) {
    u32 ioff[BATCH], st[BATCH], i, o = 0, bs = g->bs, nb = nblk, rl = raw_len; size_t gws = nblk;
    for (i = 0; i < nblk; i++) { ioff[i] = o; o += tab[i] & 0x7FFFFFFFu; }
    if (o != in_len) return 1;
    clcheck(cl.EnqueueWriteBuffer(g->q, g->src, 1, 0, in_len ? in_len : 1, in, 0, NULL, NULL), "write in");
    clcheck(cl.EnqueueWriteBuffer(g->q, g->ioff, 1, 0, (size_t)nblk * 4, ioff, 0, NULL, NULL), "write ioff");
    clcheck(cl.EnqueueWriteBuffer(g->q, g->tab, 1, 0, (size_t)nblk * 4, tab, 0, NULL, NULL), "write tab");
    ARGM(g->kd, 0, g->src); ARGM(g->kd, 1, g->ioff); ARGM(g->kd, 2, g->tab); ARG(g->kd, 3, rl);
    ARG(g->kd, 4, bs); ARGM(g->kd, 5, g->out); ARGM(g->kd, 6, g->st); ARGM(g->kd, 7, g->P);
    ARG(g->kd, 8, nb);
    clcheck(cl.EnqueueNDRangeKernel(g->q, g->kd, 1, NULL, &gws, NULL, 0, NULL, NULL), "run decompress");
    clcheck(cl.Finish(g->q), "finish");
    clcheck(cl.EnqueueReadBuffer(g->q, g->st, 1, 0, (size_t)nblk * 4, st, 0, NULL, NULL), "read st");
    clcheck(cl.EnqueueReadBuffer(g->q, g->out, 1, 0, raw_len, out, 0, NULL, NULL), "read raw");
    for (i = 0; i < nblk; i++) if (st[i]) return 1;
    return 0;
}

/* ---------------- files ---------------- */
static u8 *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); u8 *b; long sz;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) die("cannot size input");
    b = (u8 *)malloc((size_t)sz + 1);
    if (!b) die("out of memory");
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) die("read error");
    fclose(f); *n = (size_t)sz; return b;
}
static void spit(const char *path, const u8 *a, size_t an, const u8 *b, size_t bn,
                 const u8 *c, size_t cn) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (fwrite(a, 1, an, f) != an || (bn && fwrite(b, 1, bn, f) != bn) ||
        (cn && fwrite(c, 1, cn, f) != cn)) die("write error");
    if (fclose(f)) die("write error");
}

/* ---------------- compress / decompress ---------------- */
typedef struct { int use_gpu, force_gpu, force_cpu, dev, lazy, bslog; } Opt;

static Gpu *pick_gpu(Gpu *store, const Opt *o, u32 bs) {
    DevInfo dv[MAXDEV]; int n;
    if (o->force_cpu) return NULL;
    n = list_devices(dv, getenv("GPZ_ANY_DEVICE") == NULL);   /* ANY: also CPU OpenCL (testing) */
    if (o->dev >= n || n == 0) {
        if (o->force_gpu) die("no usable OpenCL GPU found (try: gpz list)");
        return NULL;
    }
    if (verbose) fprintf(stderr, "gpz: using GPU %d: %s (%s)\n", o->dev, dv[o->dev].name, dv[o->dev].vendor);
    gpu_open(store, dv[o->dev].dev, bs);
    return store;
}

static void do_compress(const char *in, const char *outp, const Opt *o) {
    size_t n; u8 *src = slurp(in, &n), *cbuf, *tabb, hdr[HDR];
    u32 bs = 1u << o->bslog, cap = bs, nb, i, *osz, total = 0; Gpu gs, *g; double t0 = now();
    if (n > 0xFFFFFFFFu) die("file too large for this build");
    nb = (u32)((n + bs - 1) / bs);
    cbuf = (u8 *)malloc((size_t)nb * cap + 1); osz = (u32 *)malloc((size_t)nb * 4 + 4);
    tabb = (u8 *)malloc((size_t)nb * 4 + 4);
    if (!cbuf || !osz || !tabb) die("out of memory");
    g = pick_gpu(&gs, o, bs);
    if (g) {
        for (i = 0; i < nb; i += BATCH) {
            u32 cnt = nb - i < BATCH ? nb - i : BATCH;
            size_t off = (size_t)i * bs, len = n - off;
            if (len > (size_t)cnt * bs) len = (size_t)cnt * bs;
            gpu_compress(g, src + off, (u32)len, cnt, cbuf + (size_t)i * cap, osz + i, (u32)o->lazy);
        }
    } else {
        GP u16 *P = (u16 *)malloc(GPZ_PSTRIDE * 2); u32 *H = (u32 *)malloc(GPZ_HSIZE * 4);
        if (!P || !H) die("out of memory");
        for (i = 0; i < nb; i++) {
            size_t off = (size_t)i * bs; u32 len = (u32)(n - off > bs ? bs : n - off);
            osz[i] = gpz_enc_block(src + off, len, cbuf + (size_t)i * cap, cap, P, H, (u32)o->lazy);
        }
        free(P); free(H);
    }
    for (i = 0; i < nb; i++) {
        size_t off = (size_t)i * bs; u32 len = (u32)(n - off > bs ? bs : n - off);
        if (osz[i] == GPZ_FAIL || osz[i] >= len) { put32(tabb + 4 * i, len | 0x80000000u); osz[i] = GPZ_FAIL; total += len; }
        else { put32(tabb + 4 * i, osz[i]); total += osz[i]; }
    }
    memcpy(hdr, MAGIC, 4); hdr[4] = 0; hdr[5] = (u8)o->bslog; hdr[6] = hdr[7] = 0;
    put32(hdr + 8, (u32)n); put32(hdr + 12, 0); put32(hdr + 16, crc32(src, n));
    {
        FILE *f = fopen(outp, "wb");
        if (!f) { perror(outp); exit(1); }
        fwrite(hdr, 1, HDR, f); fwrite(tabb, 1, (size_t)nb * 4, f);
        for (i = 0; i < nb; i++) {
            size_t off = (size_t)i * bs; u32 len = (u32)(n - off > bs ? bs : n - off);
            if (osz[i] == GPZ_FAIL) fwrite(src + off, 1, len, f);
            else fwrite(cbuf + (size_t)i * cap, 1, osz[i], f);
        }
        if (fclose(f)) die("write error");
    }
    fprintf(stderr, "gpz: %s -> %s: %lu -> %lu bytes (%.1f%%) on %s in %.2fs\n", in, outp,
                (unsigned long)n, (unsigned long)(HDR + (size_t)nb * 4 + total),
                n ? 100.0 * (HDR + (size_t)nb * 4 + total) / n : 100.0, g ? "GPU" : "CPU", now() - t0);
    free(src); free(cbuf); free(osz); free(tabb);
}

static void do_decompress(const char *in, const char *outp, const Opt *o) {
    size_t n; u8 *f = slurp(in, &n), *raw, *p; u32 size, bs, nb, i, *tab, bad = 0; Gpu gs, *g;
    double t0 = now();
    if (n < HDR || memcmp(f, MAGIC, 4)) die("not a GPZ file");
    if (f[5] < 10 || f[5] > 24) die("bad block size");
    bs = 1u << f[5]; size = get32(f + 8);
    if (get32(f + 12)) die("file too large for this build");
    nb = (u32)(((size_t)size + bs - 1) / bs);
    if (n < HDR + (size_t)nb * 4) die("truncated file");
    tab = (u32 *)malloc((size_t)nb * 4 + 4); raw = (u8 *)malloc((size_t)size + 1);
    if (!tab || !raw) die("out of memory");
    for (i = 0; i < nb; i++) tab[i] = get32(f + HDR + 4 * i);
    p = f + HDR + (size_t)nb * 4;
    { Opt oo = *o; g = pick_gpu(&gs, &oo, bs); }
    if (g) {
        for (i = 0; i < nb; i += BATCH) {
            u32 cnt = nb - i < BATCH ? nb - i : BATCH, j, plen = 0;
            size_t off = (size_t)i * bs, len = size - off;
            if (len > (size_t)cnt * bs) len = (size_t)cnt * bs;
            for (j = 0; j < cnt; j++) plen += tab[i + j] & 0x7FFFFFFFu;
            if ((size_t)(p - f) + plen > n) die("truncated file");
            if (gpu_decompress(g, p, plen, tab + i, cnt, (u32)len, raw + off)) bad = 1;
            p += plen;
        }
    } else {
        u16 *P = (u16 *)malloc(GPZ_PSTRIDE * 2);
        if (!P) die("out of memory");
        for (i = 0; i < nb && !bad; i++) {
            size_t off = (size_t)i * bs; u32 len = (u32)(size - off > bs ? bs : size - off);
            u32 cl_ = tab[i] & 0x7FFFFFFFu;
            if ((size_t)(p - f) + cl_ > n) die("truncated file");
            if (tab[i] >> 31) { if (cl_ != len) bad = 1; else memcpy(raw + off, p, len); }
            else bad = (int)gpz_dec_block(p, cl_, raw + off, len, P);
            p += cl_;
        }
        free(P);
    }
    if (bad) die("corrupt data");
    if (crc32(raw, size) != get32(f + 16)) die("checksum mismatch");
    spit(outp, raw, size, NULL, 0, NULL, 0);
    fprintf(stderr, "gpz: %s -> %s: %lu bytes on %s in %.2fs\n", in, outp,
            (unsigned long)size, g ? "GPU" : "CPU", now() - t0);
    free(f); free(raw); free(tab);
}

static void do_list(void) {
    DevInfo dv[MAXDEV]; int n, i;
    if (!cl_load()) { puts("OpenCL: not available (CPU mode only)"); return; }
    n = list_devices(dv, 0);
    if (!n) { puts("OpenCL: loaded, but no devices found (CPU mode only)"); return; }
    for (i = 0; i < n; i++)
        printf("%d: %s [%s] %s, %lu MB\n", i, dv[i].name, dv[i].vendor,
               (dv[i].type & CL_DEVICE_TYPE_GPU) ? "GPU" : "non-GPU (CPU/accelerator)",
               (unsigned long)(dv[i].mem >> 20));
    puts("(compress/decompress use GPU devices only, numbered among GPUs by -d)");
}

static void usage(void) {
    fputs("usage: gpz [options] c <in> [out]   compress (default out: <in>.gpz)\n"
          "       gpz [options] d <in> [out]   decompress\n"
          "       gpz list                     show OpenCL devices\n"
          "options: -g force GPU   -c force CPU   -d N GPU index (default 0)\n"
          "         -u ultra (lazy matching, smaller output, slower)\n"
          "         -b N block size = 2^N bytes (10..24, default 16)   -v verbose\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    Opt o; int a = 1; const char *cmd, *in, *out; char *buf = NULL;
    memset(&o, 0, sizeof o); o.bslog = 16;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        switch (argv[a][1]) {
        case 'g': o.force_gpu = 1; break;
        case 'c': o.force_cpu = 1; break;
        case 'u': o.lazy = 1; break;
        case 'v': verbose = 1; break;
        case 'd': if (++a >= argc) usage(); o.dev = atoi(argv[a]); break;
        case 'b': if (++a >= argc) usage(); o.bslog = atoi(argv[a]);
                  if (o.bslog < 10 || o.bslog > 24) die("-b must be 10..24");
                  break;
        default: usage();
        }
    }
    if (a >= argc) usage();
    cmd = argv[a++];
    if (!strcmp(cmd, "list")) { do_list(); return 0; }
    if (a >= argc || (cmd[0] != 'c' && cmd[0] != 'd') || cmd[1]) usage();
    in = argv[a++];
    if (a < argc) out = argv[a];
    else {
        size_t l = strlen(in); buf = (char *)malloc(l + 8);
        if (cmd[0] == 'c') { strcpy(buf, in); strcat(buf, ".gpz"); }
        else if (l > 4 && !strcmp(in + l - 4, ".gpz")) { memcpy(buf, in, l - 4); buf[l - 4] = 0; }
        else { strcpy(buf, in); strcat(buf, ".out"); }
        out = buf;
    }
    if (cmd[0] == 'c') do_compress(in, out, &o); else do_decompress(in, out, &o);
    free(buf);
    return 0;
}
