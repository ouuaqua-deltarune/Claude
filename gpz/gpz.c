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
#define CL_DEVICE_MAX_MEM_ALLOC_SIZE 0x1010
#define CL_DEVICE_HOST_UNIFIED_MEMORY 0x1035
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
typedef struct { cl_device dev; char name[128], vendor[128]; cl_ulong type, mem, maxalloc; cl_uint unified; } DevInfo;

/* shared-memory GPU/CPU (Apple Silicon, integrated GPUs): VRAM is really system RAM */
static int dev_unified(cl_device d, cl_ulong type, const char *vendor) {
    cl_uint u = 0;
    cl.GetDeviceInfo(d, CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof u, &u, NULL);
    return u || (type & 2) || strstr(vendor, "Apple") != NULL;
}

static int list_devices(DevInfo *out, int gpu_only) {
    cl_platform pl[8]; cl_uint np = 0, p, nd, d; int n = 0;
    if (!cl_load() || cl.GetPlatformIDs(8, pl, &np) != 0) return 0;
    for (p = 0; p < np; p++) {
        cl_device dv[MAXDEV];
        if (cl.GetDeviceIDs(pl[p], gpu_only ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_ALL, MAXDEV, dv, &nd) != 0) continue;
        for (d = 0; d < nd && n < MAXDEV; d++) {
            DevInfo *o = &out[n++];
            o->dev = dv[d]; o->name[0] = o->vendor[0] = 0; o->type = 0; o->mem = 0; o->maxalloc = 0;
            cl.GetDeviceInfo(dv[d], CL_DEVICE_NAME, sizeof o->name, o->name, NULL);
            cl.GetDeviceInfo(dv[d], CL_DEVICE_VENDOR, sizeof o->vendor, o->vendor, NULL);
            cl.GetDeviceInfo(dv[d], CL_DEVICE_TYPE, sizeof o->type, &o->type, NULL);
            cl.GetDeviceInfo(dv[d], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof o->mem, &o->mem, NULL);
            cl.GetDeviceInfo(dv[d], CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof o->maxalloc, &o->maxalloc, NULL);
            o->unified = (cl_uint)dev_unified(dv[d], o->type, o->vendor);
        }
    }
    return n;
}

typedef struct {
    cl_context ctx; cl_queue q; cl_program prog; cl_kernel kc, kd;
    cl_mem src, out, osz, P, H, ioff, tab, st;   /* batch buffers, live in device memory (VRAM) */
    u32 bs, cap, maxb;       /* block size; max blocks per batch (sized from VRAM) */
    u32 chunk;               /* blocks per launch, adapted to keep each kernel short (GPU watchdogs) */
    double ktime; u32 launches;
    cl_ulong vram, used; int unified;
} Gpu;

static void gpu_free_bufs(Gpu *g) {
    cl_mem *m[8]; int i;
    m[0] = &g->src; m[1] = &g->out; m[2] = &g->osz; m[3] = &g->P; m[4] = &g->H;
    m[5] = &g->ioff; m[6] = &g->tab; m[7] = &g->st;
    for (i = 0; i < 8; i++) if (*m[i]) { cl.ReleaseMemObject(*m[i]); *m[i] = NULL; }
}
static int gpu_alloc(Gpu *g, u32 maxb) {
    cl_int e = 0; cl_mem *m[8]; size_t sz[8]; cl_ulong fl[8]; int i;
    m[0] = &g->src; sz[0] = (size_t)maxb * g->bs;               fl[0] = CL_MEM_READ_WRITE;
    m[1] = &g->out; sz[1] = (size_t)maxb * g->cap;              fl[1] = CL_MEM_READ_WRITE;
    m[2] = &g->osz; sz[2] = (size_t)maxb * 4;                   fl[2] = CL_MEM_READ_WRITE;
    m[3] = &g->P;   sz[3] = (size_t)maxb * GPZ_PSTRIDE * 2;     fl[3] = CL_MEM_READ_WRITE;
    m[4] = &g->H;   sz[4] = (size_t)maxb * GPZ_HSIZE * 4;       fl[4] = CL_MEM_READ_WRITE;
    m[5] = &g->ioff; sz[5] = (size_t)maxb * 4;                  fl[5] = CL_MEM_READ_ONLY;
    m[6] = &g->tab;  sz[6] = (size_t)maxb * 4;                  fl[6] = CL_MEM_READ_ONLY;
    m[7] = &g->st;   sz[7] = (size_t)maxb * 4;                  fl[7] = CL_MEM_READ_WRITE;
    g->used = 0;
    for (i = 0; i < 8 && !e; i++) {
        *m[i] = cl.CreateBuffer(g->ctx, fl[i], sz[i], NULL, &e);
        if (e) *m[i] = NULL; else g->used += sz[i];
    }
    if (e) { gpu_free_bufs(g); return e; }
    g->maxb = maxb;
    return 0;
}

/* Returns 0 on success. Never exits: the caller falls back to the CPU. */
static int gpu_open(Gpu *g, cl_device dev, u32 bs) {
    cl_int e; const char *srcs[1]; size_t len; size_t ll = 0, per;
    cl_ulong maxalloc = 0, gmem = 0, type = 0, budget, lim; char vendor[128] = "";
    memset(g, 0, sizeof *g);
    g->bs = bs; g->cap = bs;
    cl.GetDeviceInfo(dev, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof maxalloc, &maxalloc, NULL);
    cl.GetDeviceInfo(dev, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof gmem, &gmem, NULL);
    cl.GetDeviceInfo(dev, CL_DEVICE_TYPE, sizeof type, &type, NULL);
    cl.GetDeviceInfo(dev, CL_DEVICE_VENDOR, sizeof vendor, vendor, NULL);
    if (getenv("GPZ_VRAM_MB")) {          /* testing: pretend the device has this much memory */
        gmem = (cl_ulong)atol(getenv("GPZ_VRAM_MB")) << 20;
        if (maxalloc > gmem / 4) maxalloc = gmem / 4;
    }
    g->vram = gmem; g->unified = dev_unified(dev, type, vendor);
    /* Discrete GPU: use up to 70% of VRAM. Unified memory is shared with the OS, so take
       a quarter of it (max 4 GiB). */
    budget = g->unified ? gmem / 4 : gmem / 10 * 7;
    if (g->unified && budget > ((cl_ulong)4 << 30)) budget = (cl_ulong)4 << 30;
    per = (size_t)bs * 2 + GPZ_PSTRIDE * 2 + GPZ_HSIZE * 4 + 16;
    lim = budget / per;
    if (maxalloc && maxalloc / GPZ_HSIZE / 4 < lim) lim = maxalloc / GPZ_HSIZE / 4;   /* biggest buffers */
    if (maxalloc && maxalloc / bs < lim) lim = maxalloc / bs;
    if (lim > 0x7FFFFFFFu / bs) lim = 0x7FFFFFFFu / bs;                                /* 32-bit offsets */
    if (lim < 16) lim = 16;
    g->ctx = cl.CreateContext(NULL, 1, &dev, NULL, NULL, &e);
    if (e) return e;
    g->q = cl.CreateCommandQueue(g->ctx, dev, 0, &e);
    if (e) return e;
    srcs[0] = GPZ_CL_SRC; len = strlen(GPZ_CL_SRC);
    g->prog = cl.CreateProgramWithSource(g->ctx, 1, srcs, &len, &e);
    if (e) return e;
    e = cl.BuildProgram(g->prog, 1, &dev, "", NULL, NULL);
    if (e != 0) {
        if (verbose) {
            char *log;
            cl.GetProgramBuildInfo(g->prog, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &ll);
            log = (char *)calloc(ll + 1, 1);
            if (log) { cl.GetProgramBuildInfo(g->prog, dev, CL_PROGRAM_BUILD_LOG, ll, log, NULL);
                       fprintf(stderr, "gpz: kernel build failed:\n%s\n", log); free(log); }
        }
        return e;
    }
    g->kc = cl.CreateKernel(g->prog, "gpz_compress_k", &e); if (e) return e;
    g->kd = cl.CreateKernel(g->prog, "gpz_decompress_k", &e); if (e) return e;
    /* take as much memory as the device will give us, halving on failure */
    for (e = gpu_alloc(g, (u32)lim); e && lim > 1; e = gpu_alloc(g, (u32)lim)) lim /= 2;
    if (e) return e;
    g->chunk = g->maxb < 1024 ? g->maxb : 1024;
    return 0;
}

#define ARG(k, i, v) do { e = cl.SetKernelArg(k, i, sizeof(v), &(v)); if (e) return e; } while (0)
#define ARGM(k, i, m) do { e = cl.SetKernelArg(k, i, sizeof(cl_mem), &(m)); if (e) return e; } while (0)
#define TRY(x) do { e = (x); if (e) return e; } while (0)

/* Run one kernel over all `nblk` blocks in short launches. A launch that fails or runs long
   makes the next ones smaller, so a slow GPU never trips the driver watchdog. */
static int gpu_launch(Gpu *g, cl_kernel k, cl_uint first_arg, u32 nblk) {
    u32 first = 0; cl_int e;
    while (first < nblk) {
        u32 cnt = nblk - first < g->chunk ? nblk - first : g->chunk; size_t gws = cnt; double t0, t;
        TRY(cl.SetKernelArg(k, first_arg, sizeof first, &first));
        t0 = now();
        e = cl.EnqueueNDRangeKernel(g->q, k, 1, NULL, &gws, NULL, 0, NULL, NULL);
        if (!e) e = cl.Finish(g->q);
        t = now() - t0;
        if (e) {
            if (g->chunk <= 1) return e;
            g->chunk = (g->chunk + 1) / 2;                       /* retry smaller */
            continue;
        }
        g->ktime += t; g->launches++; first += cnt;
        if (t > 0.8 && g->chunk > 1) g->chunk = (g->chunk + 1) / 2;
        else if (t < 0.2 && g->chunk < g->maxb) g->chunk = g->chunk * 2 > g->maxb ? g->maxb : g->chunk * 2;
    }
    return 0;
}

/* Compress `nblk` blocks (batch_len bytes at src). osz[i] = size or GPZ_FAIL. 0 = ok. */
static int gpu_compress(Gpu *g, const u8 *src, u32 batch_len, u32 nblk, u8 *out, u32 *osz, u32 lazy) {
    u32 bl = batch_len, bs = g->bs, cap = g->cap, nb = nblk; cl_int e;
    if (getenv("GPZ_TEST_GPUFAIL")) return -5;         /* testing: simulate a GPU failure */
    TRY(cl.EnqueueWriteBuffer(g->q, g->src, 1, 0, batch_len, src, 0, NULL, NULL));
    ARGM(g->kc, 0, g->src); ARG(g->kc, 1, bl); ARG(g->kc, 2, bs); ARGM(g->kc, 3, g->out);
    ARG(g->kc, 4, cap); ARGM(g->kc, 5, g->osz); ARGM(g->kc, 6, g->P); ARGM(g->kc, 7, g->H);
    ARG(g->kc, 8, nb); ARG(g->kc, 9, lazy);
    TRY(gpu_launch(g, g->kc, 10, nblk));
    TRY(cl.EnqueueReadBuffer(g->q, g->osz, 1, 0, (size_t)nblk * 4, osz, 0, NULL, NULL));
    TRY(cl.EnqueueReadBuffer(g->q, g->out, 1, 0, (size_t)nblk * cap, out, 0, NULL, NULL));
    return 0;
}

/* Decompress nblk blocks. `in` = their concatenated payload. 0 = ok, nonzero = use the CPU. */
static int gpu_decompress(Gpu *g, const u8 *in, u32 in_len, const u32 *tab, u32 nblk,
                          u32 raw_len, u8 *out) {
    u32 *ioff = (u32 *)malloc((size_t)nblk * 8 + 8), *st, i, o = 0, bs = g->bs, nb = nblk, rl = raw_len;
    cl_int e = 0;
    if (!ioff) return -1;
    st = ioff + nblk;
    for (i = 0; i < nblk; i++) { ioff[i] = o; o += tab[i] & 0x7FFFFFFFu; }
    if (o != in_len) { free(ioff); return -1; }
#define DTRY(x) do { e = (x); if (e) { free(ioff); return e; } } while (0)
    DTRY(cl.EnqueueWriteBuffer(g->q, g->src, 1, 0, in_len ? in_len : 1, in, 0, NULL, NULL));
    DTRY(cl.EnqueueWriteBuffer(g->q, g->ioff, 1, 0, (size_t)nblk * 4, ioff, 0, NULL, NULL));
    DTRY(cl.EnqueueWriteBuffer(g->q, g->tab, 1, 0, (size_t)nblk * 4, tab, 0, NULL, NULL));
    DTRY(cl.SetKernelArg(g->kd, 0, sizeof(cl_mem), &g->src)); DTRY(cl.SetKernelArg(g->kd, 1, sizeof(cl_mem), &g->ioff));
    DTRY(cl.SetKernelArg(g->kd, 2, sizeof(cl_mem), &g->tab)); DTRY(cl.SetKernelArg(g->kd, 3, sizeof rl, &rl));
    DTRY(cl.SetKernelArg(g->kd, 4, sizeof bs, &bs)); DTRY(cl.SetKernelArg(g->kd, 5, sizeof(cl_mem), &g->out));
    DTRY(cl.SetKernelArg(g->kd, 6, sizeof(cl_mem), &g->st)); DTRY(cl.SetKernelArg(g->kd, 7, sizeof(cl_mem), &g->P));
    DTRY(cl.SetKernelArg(g->kd, 8, sizeof nb, &nb));
    DTRY(gpu_launch(g, g->kd, 9, nblk));
    DTRY(cl.EnqueueReadBuffer(g->q, g->st, 1, 0, (size_t)nblk * 4, st, 0, NULL, NULL));
    DTRY(cl.EnqueueReadBuffer(g->q, g->out, 1, 0, raw_len, out, 0, NULL, NULL));
#undef DTRY
    for (i = 0; i < nblk; i++) if (st[i]) { free(ioff); return 1; }
    free(ioff);
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

#include "gpz2.h"

/* ---------------- compress / decompress ---------------- */
typedef struct { int use_gpu, force_gpu, force_cpu, dev, lazy, bslog, max, notx; } Opt;

static Gpu *pick_gpu(Gpu *store, const Opt *o, u32 bs) {
    DevInfo dv[MAXDEV]; int n, e;
    if (o->force_cpu) return NULL;
    n = list_devices(dv, getenv("GPZ_ANY_DEVICE") == NULL);   /* ANY: also CPU OpenCL (testing) */
    if (o->dev >= n || n == 0) {
        if (o->force_gpu) die("no usable OpenCL GPU found (try: gpz list)");
        return NULL;
    }
    if (verbose) fprintf(stderr, "gpz: using GPU %d: %s (%s)\n", o->dev, dv[o->dev].name, dv[o->dev].vendor);
    e = gpu_open(store, dv[o->dev].dev, bs);
    if (e) {
        if (o->force_gpu) { fprintf(stderr, "gpz: cannot use this GPU (OpenCL error %d)\n", e); exit(1); }
        fprintf(stderr, "gpz: GPU unavailable (OpenCL error %d), using CPU\n", e);
        return NULL;
    }
    if (verbose)
        fprintf(stderr, "gpz: device memory %lu MB (%s): using %lu MB, %lu blocks per batch\n",
                (unsigned long)(store->vram >> 20), store->unified ? "unified/shared" : "dedicated VRAM",
                (unsigned long)(store->used >> 20), (unsigned long)store->maxb);
    return store;
}

static void gpu_report(const Gpu *g, u32 nb) {
    if (verbose)
        fprintf(stderr, "gpz: GPU: %lu blocks of %lu bytes, %lu launches (now %lu blocks each), %.2fs in kernels\n",
                (unsigned long)nb, (unsigned long)g->bs, (unsigned long)g->launches, (unsigned long)g->chunk, g->ktime);
}

static void cpu_comp_range(const u8 *src, size_t n, u32 bs, u32 cap, u32 first, u32 count,
                           u8 *cbuf, u32 *osz, u32 lazy) {
    u16 *P = (u16 *)malloc(GPZ_PSTRIDE * 2); u32 *H = (u32 *)malloc(GPZ_HSIZE * 4), i;
    if (!P || !H) die("out of memory");
    for (i = first; i < first + count; i++) {
        size_t off = (size_t)i * bs; u32 len = (u32)(n - off > bs ? bs : n - off);
        osz[i] = gpz_enc_block(src + off, len, cbuf + (size_t)i * cap, cap, P, H, lazy);
    }
    free(P); free(H);
}

static void do_compress(const char *in, const char *outp, const Opt *o) {
    int bslog = o->bslog ? o->bslog : 16;
    size_t n; u8 *src = slurp(in, &n), *cbuf, *tabb, hdr[HDR];
    u32 bs = 1u << bslog, cap = bs, nb, i, *osz, total = 0, step; Gpu gs, *g; double t0 = now();
    if (n > 0xFFFFFFFFu) die("file too large for this build");
    nb = (u32)((n + bs - 1) / bs);
    cbuf = (u8 *)malloc((size_t)nb * cap + 1); osz = (u32 *)malloc((size_t)nb * 4 + 4);
    tabb = (u8 *)malloc((size_t)nb * 4 + 4);
    if (!cbuf || !osz || !tabb) die("out of memory");
    g = pick_gpu(&gs, o, bs);
    step = g ? g->maxb : (nb ? nb : 1);
    for (i = 0; i < nb; i += step) {
        u32 cnt = nb - i < step ? nb - i : step;
        size_t off = (size_t)i * bs, len = n - off;
        if (len > (size_t)cnt * bs) len = (size_t)cnt * bs;
        if (g && gpu_compress(g, src + off, (u32)len, cnt, cbuf + (size_t)i * cap, osz + i, (u32)o->lazy)) {
            if (o->force_gpu) die("GPU run failed");
            fprintf(stderr, "gpz: GPU run failed, finishing on CPU\n");
            g = NULL;
        }
        if (!g) cpu_comp_range(src, n, bs, cap, i, cnt, cbuf, osz, (u32)o->lazy);
    }
    for (i = 0; i < nb; i++) {
        size_t off = (size_t)i * bs; u32 len = (u32)(n - off > bs ? bs : n - off);
        if (osz[i] == GPZ_FAIL || osz[i] >= len) { put32(tabb + 4 * i, len | 0x80000000u); osz[i] = GPZ_FAIL; total += len; }
        else { put32(tabb + 4 * i, osz[i]); total += osz[i]; }
    }
    memcpy(hdr, MAGIC, 4); hdr[4] = 0; hdr[5] = (u8)bslog; hdr[6] = hdr[7] = 0;
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
    if (g) gpu_report(g, nb);
    fprintf(stderr, "gpz: %s -> %s: %lu -> %lu bytes (%.1f%%) on %s in %.2fs\n", in, outp,
                (unsigned long)n, (unsigned long)(HDR + (size_t)nb * 4 + total),
                n ? 100.0 * (HDR + (size_t)nb * 4 + total) / n : 100.0, g ? "GPU" : "CPU", now() - t0);
    free(src); free(cbuf); free(osz); free(tabb);
}

static void do_decompress(const char *in, const char *outp, const Opt *o) {
    size_t n; u8 *f = slurp(in, &n), *raw, *p; u32 size, bs, nb, i, *tab, step; int bad = 0; Gpu gs, *g;
    u16 *P; double t0 = now();
    if (n >= 20 && !memcmp(f, "GPZ2", 4)) { do_decompress2(f, n, outp, in); free(f); return; }
    if (n < HDR || memcmp(f, MAGIC, 4)) die("not a GPZ file");
    if (f[5] < 10 || f[5] > 24) die("bad block size");
    bs = 1u << f[5]; size = get32(f + 8);
    if (get32(f + 12)) die("file too large for this build");
    nb = (u32)(((size_t)size + bs - 1) / bs);
    if (n < HDR + (size_t)nb * 4) die("truncated file");
    tab = (u32 *)malloc((size_t)nb * 4 + 4); raw = (u8 *)malloc((size_t)size + 1);
    P = (u16 *)malloc(GPZ_PSTRIDE * 2);
    if (!tab || !raw || !P) die("out of memory");
    for (i = 0; i < nb; i++) tab[i] = get32(f + HDR + 4 * i);
    p = f + HDR + (size_t)nb * 4;
    g = pick_gpu(&gs, o, bs);
    step = g ? g->maxb : (nb ? nb : 1);
    for (i = 0; i < nb && !bad; i += step) {
        u32 cnt = nb - i < step ? nb - i : step, j, plen = 0;
        size_t off = (size_t)i * bs, len = size - off;
        if (len > (size_t)cnt * bs) len = (size_t)cnt * bs;
        for (j = 0; j < cnt; j++) plen += tab[i + j] & 0x7FFFFFFFu;
        if ((size_t)(p - f) + plen > n) die("truncated file");
        if (!g || gpu_decompress(g, p, plen, tab + i, cnt, (u32)len, raw + off)) {
            /* CPU path (also used when the GPU reports a problem, so the CPU decides if data is bad) */
            const u8 *q = p;
            for (j = 0; j < cnt && !bad; j++) {
                size_t boff = (size_t)(i + j) * bs; u32 blen = (u32)(size - boff > bs ? bs : size - boff);
                u32 cl_ = tab[i + j] & 0x7FFFFFFFu;
                if (tab[i + j] >> 31) { if (cl_ != blen) bad = 1; else memcpy(raw + boff, q, blen); }
                else bad = (int)gpz_dec_block(q, cl_, raw + boff, blen, P);
                q += cl_;
            }
        }
        p += plen;
    }
    if (bad) die("corrupt data");
    if (crc32(raw, size) != get32(f + 16)) die("checksum mismatch");
    spit(outp, raw, size, NULL, 0, NULL, 0);
    if (g) gpu_report(g, nb);
    fprintf(stderr, "gpz: %s -> %s: %lu bytes on %s in %.2fs\n", in, outp,
            (unsigned long)size, g ? "GPU" : "CPU", now() - t0);
    free(f); free(raw); free(tab); free(P);
}

static void do_list(void) {
    DevInfo dv[MAXDEV]; int n, i;
    if (!cl_load()) { puts("OpenCL: not available (CPU mode only)"); return; }
    n = list_devices(dv, 0);
    if (!n) { puts("OpenCL: loaded, but no devices found (CPU mode only)"); return; }
    for (i = 0; i < n; i++)
        printf("%d: %s [%s] %s, %lu MB %s, max buffer %lu MB%s\n", i, dv[i].name, dv[i].vendor,
               (dv[i].type & CL_DEVICE_TYPE_GPU) ? "GPU" : "non-GPU (CPU/accelerator)",
               (unsigned long)(dv[i].mem >> 20), dv[i].unified ? "unified memory" : "VRAM",
               (unsigned long)(dv[i].maxalloc >> 20),
               dv[i].mem >= ((cl_ulong)3800 << 20) ? "  [>= 4 GB: full speed]" : "  [< 4 GB: smaller batches]");
    puts("(compress/decompress use GPU devices only, numbered among GPUs by -d)");
}

static void usage(void) {
    fputs("usage: gpz [options] c <in> [out]   compress (default out: <in>.gpz)\n"
          "       gpz [options] d <in> [out]   decompress\n"
          "       gpz list                     show OpenCL devices\n"
          "options: -g force GPU   -c force CPU   -d N GPU index (default 0)\n"
          "         -m MAX RATIO: context mixing + deflate recompression (CPU, slow)\n"
          "         -n with -m: do not recompress zip/gzip streams\n"
          "         -u fast mode: lazy matching, smaller output, slower\n"
          "         -b N block size = 2^N bytes (10..24, default 16; 14 = more GPU threads, worse ratio)   -v verbose\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
    Opt o; int a = 1; const char *cmd, *in, *out; char *buf = NULL;
    memset(&o, 0, sizeof o);
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        switch (argv[a][1]) {
        case 'g': o.force_gpu = 1; break;
        case 'c': o.force_cpu = 1; break;
        case 'u': o.lazy = 1; break;
        case 'm': o.max = 1; break;
        case 'n': o.notx = 1; break;
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
    if (cmd[0] == 'c') { if (o.max) do_compress2(in, out, !o.notx); else do_compress(in, out, &o); }
    else do_decompress(in, out, &o);
    free(buf);
    return 0;
}
