/*
 * GPU scheduler for the context-mixing coder. Many independent streams ("units") live in
 * slots whose model memory sits in VRAM; the GPU advances every active slot a few KB per
 * launch, and finished slots are refilled from a job source (CPU threads can share it).
 * Needs the OpenCL loader and device list from gpz.c.
 */
typedef struct CmJob {
    const u8 *in;        /* encode: raw data;  decode: compressed data */
    u32 n;               /* raw length */
    u32 cn;              /* decode: compressed length */
    u32 tbits;           /* model size */
    u8 *out;             /* encode: capacity n;  decode: capacity n */
    u32 outlen;          /* result */
    int status;          /* 0 ok, 1 stream error/did not fit (encode: store raw), 2 GPU failure: redo on CPU */
    void *user;
} CmJob;

/* next(): 1 = job returned, 0 = none right now (only when block==0), -1 = no more jobs ever */
typedef int (*CmNext)(void *ctx, int block, CmJob **job);
typedef void (*CmDone)(void *ctx, CmJob *job);

typedef struct {
    cl_context ctx; cl_queue q; cl_program prog; cl_kernel k;
    cl_mem *grp, *state, *jobs, stt, dtt;
    u32 ngrp, *gslots;               /* groups and slots per group */
    u32 nslots, slot_bytes, arena_bytes, capA, maxtbits;
    CmJob **cur; u32 *jobs_h;        /* per-slot job and host copy of the job words */
    u32 maxb; double ktime; u32 launches;
    cl_ulong used;
} CmGpu;

#define CL_MEM_COPY_HOST_PTR_ 32ull

static void cmgpu_close(CmGpu *g) {
    u32 i;
    for (i = 0; i < g->ngrp; i++) {
        if (g->grp && g->grp[i]) cl.ReleaseMemObject(g->grp[i]);
        if (g->state && g->state[i]) cl.ReleaseMemObject(g->state[i]);
        if (g->jobs && g->jobs[i]) cl.ReleaseMemObject(g->jobs[i]);
    }
    if (g->stt) cl.ReleaseMemObject(g->stt);
    if (g->dtt) cl.ReleaseMemObject(g->dtt);
    free(g->grp); free(g->state); free(g->jobs); free(g->gslots); free(g->cur); free(g->jobs_h);
    memset(g, 0, sizeof *g);
}

/* Open a device and allocate as many slots as fit in `budget` bytes of device memory. */
static int cmgpu_open(CmGpu *g, cl_device dev, cl_ulong budget, u32 tbits, u32 umax, u32 want_slots) {
    cl_int e; const char *srcs[1]; size_t len, per; cl_ulong maxalloc = 0; u32 sg, total, i;
    memset(g, 0, sizeof *g);
    cm_tables();
    g->maxtbits = tbits; g->capA = umax;
    g->arena_bytes = (u32)((cm_arena_size(tbits) + 255) & ~(size_t)255);
    g->slot_bytes = g->arena_bytes + 2 * umax;
    cl.GetDeviceInfo(dev, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof maxalloc, &maxalloc, NULL);
    per = (size_t)g->slot_bytes + 32 * 4 + 8 * 4;
    total = (u32)(budget / per);
    if (total > want_slots) total = want_slots;
    if (total < 1) return -100;                         /* budget too small for even one slot */
    sg = maxalloc ? (u32)(maxalloc / g->slot_bytes) : total;
    if (sg < 1) return -101;                            /* one slot exceeds the largest buffer */
    g->ctx = cl.CreateContext(NULL, 1, &dev, NULL, NULL, &e); if (e) return e;
    g->q = cl.CreateCommandQueue(g->ctx, dev, 0, &e); if (e) return e;
    srcs[0] = GPZ_CL_SRC; len = strlen(GPZ_CL_SRC);
    g->prog = cl.CreateProgramWithSource(g->ctx, 1, srcs, &len, &e); if (e) return e;
    e = cl.BuildProgram(g->prog, 1, &dev, "", NULL, NULL); if (e) return e;
    g->k = cl.CreateKernel(g->prog, "cm_k", &e); if (e) return e;
    g->stt = cl.CreateBuffer(g->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR_, sizeof cm_st_h, cm_st_h, &e); if (e) return e;
    g->dtt = cl.CreateBuffer(g->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR_, sizeof cm_dt_h, cm_dt_h, &e); if (e) return e;
    for (;; total = total / 2) {                       /* allocate, halving on failure */
        u32 left = total; g->ngrp = (total + sg - 1) / sg; g->used = 0;
        g->grp = (cl_mem *)calloc(g->ngrp, sizeof(cl_mem)); g->state = (cl_mem *)calloc(g->ngrp, sizeof(cl_mem));
        g->jobs = (cl_mem *)calloc(g->ngrp, sizeof(cl_mem)); g->gslots = (u32 *)calloc(g->ngrp, 4);
        if (!g->grp || !g->state || !g->jobs || !g->gslots) return -102;
        e = 0;
        for (i = 0; i < g->ngrp && !e; i++) {
            u32 c = left < sg ? left : sg; left -= c; g->gslots[i] = c;
            g->grp[i] = cl.CreateBuffer(g->ctx, CL_MEM_READ_WRITE, (size_t)c * g->slot_bytes, NULL, &e);
            if (e) { g->grp[i] = NULL; break; }
            g->state[i] = cl.CreateBuffer(g->ctx, CL_MEM_READ_WRITE, (size_t)c * 128, NULL, &e);
            if (e) { g->state[i] = NULL; break; }
            g->jobs[i] = cl.CreateBuffer(g->ctx, CL_MEM_READ_WRITE, (size_t)c * 32, NULL, &e);
            if (e) { g->jobs[i] = NULL; break; }
            g->used += (cl_ulong)c * per;
        }
        if (!e) break;
        for (i = 0; i < g->ngrp; i++) {
            if (g->grp[i]) cl.ReleaseMemObject(g->grp[i]);
            if (g->state[i]) cl.ReleaseMemObject(g->state[i]);
            if (g->jobs[i]) cl.ReleaseMemObject(g->jobs[i]);
        }
        free(g->grp); free(g->state); free(g->jobs); free(g->gslots);
        g->grp = g->state = g->jobs = NULL; g->gslots = NULL; g->ngrp = 0;
        if (total <= 1) return e;
    }
    g->nslots = total;
    g->cur = (CmJob **)calloc(total, sizeof(CmJob *));
    g->jobs_h = (u32 *)calloc((size_t)total * 8, 4);
    if (!g->cur || !g->jobs_h) return -102;
    g->maxb = 1024;
    {   /* constant tables */
        e = cl.EnqueueWriteBuffer(g->q, g->stt, 1, 0, sizeof cm_st_h, cm_st_h, 0, NULL, NULL); if (e) return e;
        e = cl.EnqueueWriteBuffer(g->q, g->dtt, 1, 0, sizeof cm_dt_h, cm_dt_h, 0, NULL, NULL); if (e) return e;
    }
    return 0;
}

static int cmgpu_launch(CmGpu *g, u32 mode, u32 grp, u32 maxb) {
    cl_int e; size_t gws = g->gslots[grp];
    e = cl.SetKernelArg(g->k, 0, 4, &mode); if (e) return e;
    e = cl.SetKernelArg(g->k, 1, sizeof(cl_mem), &g->grp[grp]); if (e) return e;
    e = cl.SetKernelArg(g->k, 2, 4, &g->slot_bytes); if (e) return e;
    e = cl.SetKernelArg(g->k, 3, 4, &g->arena_bytes); if (e) return e;
    e = cl.SetKernelArg(g->k, 4, 4, &g->capA); if (e) return e;
    e = cl.SetKernelArg(g->k, 5, 4, &g->gslots[grp]); if (e) return e;
    e = cl.SetKernelArg(g->k, 6, sizeof(cl_mem), &g->state[grp]); if (e) return e;
    e = cl.SetKernelArg(g->k, 7, sizeof(cl_mem), &g->jobs[grp]); if (e) return e;
    e = cl.SetKernelArg(g->k, 8, sizeof(cl_mem), &g->stt); if (e) return e;
    e = cl.SetKernelArg(g->k, 9, sizeof(cl_mem), &g->dtt); if (e) return e;
    e = cl.SetKernelArg(g->k, 10, 4, &maxb); if (e) return e;
    return cl.EnqueueNDRangeKernel(g->q, g->k, 1, NULL, &gws, NULL, 0, NULL, NULL);
}

/* Process jobs until the source is exhausted. Returns 0, or a CL error (jobs in flight then
   get status 2 and are passed to done() so the caller can redo them on the CPU). */
static int cmgpu_run(CmGpu *g, int decode, CmNext next, CmDone done, void *ctx) {
    int eof = 0, err = 0; u32 s, gi, active = 0;
    while (!err) {
        u32 fresh_mask_any = 0;
        char *fresh = NULL;
        /* 1. fill free slots */
        for (s = 0; s < g->nslots && !eof; s++) {
            CmJob *j; int r;
            if (g->cur[s]) continue;
            r = next(ctx, active == 0 && !fresh_mask_any, &j);
            if (r < 0) { eof = 1; break; }
            if (r == 0) break;
            {
                u32 sgi = 0, loc = s; size_t off;
                while (loc >= g->gslots[sgi]) { loc -= g->gslots[sgi]; sgi++; }
                off = (size_t)loc * g->slot_bytes;
                if (j->n > g->capA || j->tbits > g->maxtbits) { j->status = 2; j->outlen = 0; done(ctx, j); continue; }
                if (decode) err = cl.EnqueueWriteBuffer(g->q, g->grp[sgi], 1, off + g->arena_bytes + g->capA, j->cn ? j->cn : 1, j->in, 0, NULL, NULL);
                else err = cl.EnqueueWriteBuffer(g->q, g->grp[sgi], 1, off + g->arena_bytes, j->n ? j->n : 1, j->in, 0, NULL, NULL);
                if (err) { j->status = 2; done(ctx, j); break; }
                g->cur[s] = j; active++; fresh_mask_any = 1;
                g->jobs_h[8 * s + 0] = j->n; g->jobs_h[8 * s + 1] = decode ? j->cn : j->n;
                g->jobs_h[8 * s + 2] = 4; g->jobs_h[8 * s + 3] = 0; g->jobs_h[8 * s + 4] = j->tbits;
            }
        }
        (void)fresh;
        if (err) break;
        if (active == 0) { if (eof) break; continue; }
        /* 2. upload job words, init new slots, advance everything one step */
        {
            double t0 = now(); u32 base = 0;
            for (gi = 0; gi < g->ngrp && !err; gi++) {
                err = cl.EnqueueWriteBuffer(g->q, g->jobs[gi], 1, 0, (size_t)g->gslots[gi] * 32, g->jobs_h + (size_t)base * 8, 0, NULL, NULL);
                if (!err) err = cmgpu_launch(g, decode ? 3 : 0, gi, 0);
                if (!err) err = cmgpu_launch(g, decode ? 2 : 1, gi, g->maxb);
                base += g->gslots[gi];
            }
            if (!err) err = cl.Finish(g->q);
            {
                double t = now() - t0;
                g->ktime += t; g->launches++;
                if (t > 0.6 && g->maxb > 64) g->maxb /= 2;
                else if (t < 0.15 && g->maxb < (1u << 20)) g->maxb *= 2;
            }
        }
        if (err) break;
        /* 3. collect finished slots */
        {
            u32 base = 0;
            for (gi = 0; gi < g->ngrp && !err; gi++) {
                err = cl.EnqueueReadBuffer(g->q, g->jobs[gi], 1, 0, (size_t)g->gslots[gi] * 32, g->jobs_h + (size_t)base * 8, 0, NULL, NULL);
                for (s = base; s < base + g->gslots[gi] && !err; s++) {
                    CmJob *j = g->cur[s]; u32 st = g->jobs_h[8 * s + 2]; size_t off;
                    if (!j || (st != 2 && st != 3)) continue;
                    off = (size_t)(s - base) * g->slot_bytes;
                    if (st == 3) { j->status = 1; j->outlen = 0; }
                    else {
                        j->outlen = g->jobs_h[8 * s + 3]; j->status = 0;
                        if (j->outlen > j->n && !decode) j->status = 1;
                        else if (j->outlen) {
                            if (decode) err = cl.EnqueueReadBuffer(g->q, g->grp[gi], 1, off + g->arena_bytes, j->outlen, j->out, 0, NULL, NULL);
                            else err = cl.EnqueueReadBuffer(g->q, g->grp[gi], 1, off + g->arena_bytes + g->capA, j->outlen, j->out, 0, NULL, NULL);
                        }
                        if (decode && j->outlen != j->n) j->status = 1;
                    }
                    if (err) break;
                    g->cur[s] = NULL; g->jobs_h[8 * s + 2] = 0; active--;
                    done(ctx, j);
                }
                base += g->gslots[gi];
            }
        }
    }
    if (err) {                                  /* hand back whatever was in flight */
        for (s = 0; s < g->nslots; s++) if (g->cur[s]) { g->cur[s]->status = 2; done(ctx, g->cur[s]); g->cur[s] = NULL; }
    }
    return err;
}
