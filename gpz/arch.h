/*
 * GPZA: snapshot archive for a directory tree (built for Minecraft server folders).
 *
 *  - Every file's transformed bytes are appended to one logical stream S, which is cut into
 *    units (<= UMAX bytes) that are compressed independently by the context-mixing coder.
 *    Units are processed in parallel by CPU threads and, if present, a GPU whose VRAM holds
 *    one model per slot. Region files (.mca) and zip/gzip data are "un-deflated" first.
 *  - Metadata kept: mode, owner, mtime (ns), symlinks, empty dirs. Per-file CRC-32.
 *  - Layout: "GPZA" header | unit blobs (any order) | meta | unit table | 80-byte footer.
 */
#ifndef _WIN32

#define UMAX_DEFAULT (8u << 20)
#define AR_FOOT 80

typedef struct {
    int threads, use_gpu, force_gpu, force_cpu, dev, notx, force, nexcl, noverify;
    unsigned long vram_mb; u32 slots, tbits, umax;
    const char *excl[32];
} ArOpt;

static u64 get64(const u8 *p) { return (u64)get32(p) | ((u64)get32(p + 4) << 32); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

typedef struct { u8 *p; size_t n, cap; } Buf;
static void buf_add(Buf *b, const void *d, size_t k) {
    if (b->n + k > b->cap) { b->cap = (b->n + k) * 2 + 256; b->p = (u8 *)realloc(b->p, b->cap); if (!b->p) die("out of memory"); }
    memcpy(b->p + b->n, d, k); b->n += k;
}
static void buf_u8(Buf *b, u32 v) { u8 c = (u8)v; buf_add(b, &c, 1); }
static void buf_u16(Buf *b, u32 v) { u8 c[2]; c[0] = (u8)v; c[1] = (u8)(v >> 8); buf_add(b, c, 2); }
static void buf_u32(Buf *b, u32 v) { u8 c[4]; put32(c, v); buf_add(b, c, 4); }
static void buf_u64(Buf *b, u64 v) { u8 c[8]; put64(c, v); buf_add(b, c, 8); }

typedef struct {
    char *path, *target; u8 type, tx; u32 mode, uid, gid, crc, mnsec; long long mtime; u64 size, soff, tlen;
} Ent;                                    /* type: 1 dir, 2 file, 3 symlink */

typedef struct { u64 off; u32 comp, raw; u8 tbits, codec; } URec;     /* codec 0 stored, 1 context mixing */

typedef struct Unit {
    u32 id, n, tbits; u8 *data, *out; u32 outlen; int cpu_only, status; CmJob job; struct Unit *next;
} Unit;

#define MTIME_S(st) ((long long)(st).st_mtime)
#ifdef __APPLE__
  #define MTIME_NS(st) ((u32)(st).st_mtimespec.tv_nsec)
#else
  #define MTIME_NS(st) ((u32)(st).st_mtim.tv_nsec)
#endif

static u32 unit_tbits(u32 n, u32 cap) { u32 t = 16; while (t < cap && ((u32)1 << t) < n * 2) t++; return t > cap ? cap : t; }

/* ---- shared state of a pack run ---- */
typedef struct {
    ArOpt o; pthread_mutex_t mu; pthread_cond_t cv_job, cv_space;
    Unit *qh, *qt; size_t qbytes, qlimit; int eof, gpu_alive;
    FILE *out; u64 outpos; u32 crc; URec *rec; size_t nrec, caprec;
    u64 done_units, in_bytes, out_bytes, units_made, gpu_units; double t0;
    u32 cpu_tbits_cap;
} PackCtx;

static void wr(PackCtx *c, const void *d, size_t k) {       /* caller holds mu (or is single-threaded) */
    if (fwrite(d, 1, k, c->out) != k) die("write error (disk full?)");
    c->crc = crc32u(c->crc, (const u8 *)d, k); c->outpos += k;
}

static void deliver(PackCtx *c, Unit *u, int via_gpu) {
    pthread_mutex_lock(&c->mu);
    if (u->id >= c->caprec) {
        c->caprec = (size_t)u->id * 2 + 64; c->rec = (URec *)realloc(c->rec, c->caprec * sizeof(URec));
        if (!c->rec) die("out of memory");
    }
    {
        URec *r = &c->rec[u->id]; r->off = c->outpos; r->raw = u->n; r->tbits = (u8)u->tbits;
        if (u->status == 0 && u->outlen < u->n) { r->codec = 1; r->comp = u->outlen; wr(c, u->out, u->outlen); }
        else { r->codec = 0; r->comp = u->n; wr(c, u->data, u->n); }
        if (u->id >= c->nrec) c->nrec = u->id + 1;
        c->out_bytes += r->comp;
    }
    c->done_units++; c->in_bytes += u->n; if (via_gpu) c->gpu_units++;
    c->qbytes -= (size_t)u->n * 2;
    pthread_cond_broadcast(&c->cv_space);
    pthread_mutex_unlock(&c->mu);
    free(u->data); free(u->out); free(u);
}

/* CPU worker */
static void *pack_cpu_worker(void *arg) {
    PackCtx *c = (PackCtx *)arg;
    for (;;) {
        Unit *u;
        pthread_mutex_lock(&c->mu);
        while (!c->qh && !c->eof) pthread_cond_wait(&c->cv_job, &c->mu);
        u = c->qh;
        if (u) { c->qh = u->next; if (!c->qh) c->qt = NULL; }
        pthread_mutex_unlock(&c->mu);
        if (!u) return NULL;
        {
            size_t r = cm_compress(u->data, u->n, u->out, u->n, u->tbits);
            u->status = r ? 0 : 1; u->outlen = (u32)r;
        }
        deliver(c, u, 0);
    }
}

/* GPU: job source and completion */
static int pack_gpu_next(void *ctx, int block, CmJob **job) {
    PackCtx *c = (PackCtx *)ctx; Unit *u, *prev = NULL;
    pthread_mutex_lock(&c->mu);
    for (;;) {
        for (u = c->qh, prev = NULL; u && u->cpu_only; prev = u, u = u->next) ;
        if (u) break;
        if (c->eof) { pthread_mutex_unlock(&c->mu); return -1; }
        if (!block) { pthread_mutex_unlock(&c->mu); return 0; }
        pthread_cond_wait(&c->cv_job, &c->mu);
    }
    if (prev) prev->next = u->next; else c->qh = u->next;
    if (c->qt == u) c->qt = prev;
    pthread_mutex_unlock(&c->mu);
    u->job.in = u->data; u->job.n = u->n; u->job.cn = 0; u->job.tbits = u->tbits; u->job.out = u->out;
    u->job.user = u; *job = &u->job;
    return 1;
}
static void pack_gpu_done(void *ctx, CmJob *j) {
    PackCtx *c = (PackCtx *)ctx; Unit *u = (Unit *)j->user;
    if (j->status == 2) {                     /* GPU could not do it: hand to the CPU workers */
        u->cpu_only = 1; u->next = NULL;
        pthread_mutex_lock(&c->mu);
        if (c->qt) c->qt->next = u; else c->qh = u;
        c->qt = u;
        pthread_cond_broadcast(&c->cv_job);
        pthread_mutex_unlock(&c->mu);
        return;
    }
    u->status = j->status; u->outlen = j->outlen;
    deliver(c, u, 1);
}
typedef struct { PackCtx *c; CmGpu *g; } GpuArg;
static void *pack_gpu_thread(void *arg) {
    GpuArg *a = (GpuArg *)arg;
    int e = cmgpu_run(a->g, 0, pack_gpu_next, pack_gpu_done, a->c);
    if (e) fprintf(stderr, "\ngpz: GPU stopped (OpenCL error %d); CPU threads finish the rest\n", e);
    pthread_mutex_lock(&a->c->mu);
    a->c->gpu_alive = 0;
    {   /* anything still marked GPU-only is released to the CPU */
        Unit *u; for (u = a->c->qh; u; u = u->next) u->cpu_only = 1;
    }
    pthread_cond_broadcast(&a->c->cv_job);
    pthread_mutex_unlock(&a->c->mu);
    return NULL;
}

static void queue_unit(PackCtx *c, u8 *data, u32 n, u32 *next_id, int use_gpu_cap) {
    Unit *u = (Unit *)calloc(1, sizeof *u);
    if (!u) die("out of memory");
    u->data = data; u->n = n; u->id = (*next_id)++; u->out = (u8 *)malloc((size_t)n + 1);
    if (!u->out) die("out of memory");
    u->tbits = unit_tbits(n, use_gpu_cap ? c->o.tbits : c->cpu_tbits_cap);
    pthread_mutex_lock(&c->mu);
    while (c->qbytes > c->qlimit) pthread_cond_wait(&c->cv_space, &c->mu);   /* backpressure */
    c->qbytes += (size_t)n * 2; c->units_made++;
    if (c->qt) c->qt->next = u; else c->qh = u;
    c->qt = u;
    pthread_cond_broadcast(&c->cv_job);
    pthread_mutex_unlock(&c->mu);
}

/* ---- directory walk ---- */
typedef struct { Ent *e; size_t n, cap; } EntList;
static void ent_add(EntList *l, Ent e) {
    if (l->n == l->cap) { l->cap = l->cap * 2 + 64; l->e = (Ent *)realloc(l->e, l->cap * sizeof(Ent)); if (!l->e) die("out of memory"); }
    l->e[l->n++] = e;
}
static char *xstrdup(const char *s) { char *p = (char *)malloc(strlen(s) + 1); if (!p) die("out of memory"); strcpy(p, s); return p; }
static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int excluded(const ArOpt *o, const char *rel) {
    int i; const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
    for (i = 0; i < o->nexcl; i++)
        if (!fnmatch(o->excl[i], rel, 0) || !fnmatch(o->excl[i], base, 0)) return 1;
    return 0;
}

static dev_t skip_dev; static ino_t skip_ino; static int skip_set;
static unsigned long skipped_special;

static void walk(const char *root, const char *rel, const ArOpt *o, EntList *l) {
    char full[4096]; DIR *d; struct dirent *de; char **names = NULL; size_t nn = 0, cap = 0, i;
    snprintf(full, sizeof full, "%s/%s", root, rel);
    d = opendir(rel[0] ? full : root);
    if (!d) { fprintf(stderr, "gpz: cannot read directory %s\n", rel[0] ? rel : root); return; }
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (nn == cap) { cap = cap * 2 + 32; names = (char **)realloc(names, cap * sizeof(char *)); if (!names) die("out of memory"); }
        names[nn++] = xstrdup(de->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof(char *), cmp_names);
    for (i = 0; i < nn; i++) {
        char relp[4096], fp[4096]; struct stat st; Ent e;
        if (rel[0]) snprintf(relp, sizeof relp, "%s/%s", rel, names[i]); else snprintf(relp, sizeof relp, "%s", names[i]);
        snprintf(fp, sizeof fp, "%s/%s", root, relp);
        free(names[i]);
        if (excluded(o, relp)) continue;
        if (lstat(fp, &st) != 0) { fprintf(stderr, "gpz: cannot stat %s\n", relp); continue; }
        if (skip_set && st.st_dev == skip_dev && st.st_ino == skip_ino) continue;
        memset(&e, 0, sizeof e);
        e.path = xstrdup(relp); e.mode = st.st_mode & 07777; e.uid = (u32)st.st_uid; e.gid = (u32)st.st_gid;
        e.mtime = MTIME_S(st); e.mnsec = MTIME_NS(st);
        if (S_ISDIR(st.st_mode)) { e.type = 1; ent_add(l, e); walk(root, relp, o, l); }
        else if (S_ISREG(st.st_mode)) { e.type = 2; e.size = (u64)st.st_size; ent_add(l, e); }
        else if (S_ISLNK(st.st_mode)) {
            char t[4096]; ssize_t k = readlink(fp, t, sizeof t - 1);
            if (k < 0) { free(e.path); continue; }
            t[k] = 0; e.type = 3; e.target = xstrdup(t); ent_add(l, e);
        } else { skipped_special++; free(e.path); }
    }
    free(names);
}

/* ---- pack ---- */
static const char *base_of(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }
static int is_mca(const char *p) {
    size_t l = strlen(p);
    return l > 4 && (!strcmp(p + l - 4, ".mca") || !strcmp(p + l - 4, ".mcr"));
}

static void progress(PackCtx *c, size_t fi, size_t nf, u64 inb) {
    static double last; double t = now() - c->t0;
    if (t - last < 1.0 && fi != nf) return;
    last = t;
    fprintf(stderr, "\r  files %lu/%lu  read %lu MB  packed units %lu/%lu  -> %lu MB  (%.0fs)   ",
            (unsigned long)fi, (unsigned long)nf, (unsigned long)(inb >> 20), (unsigned long)c->done_units,
            (unsigned long)c->units_made, (unsigned long)(c->out_bytes >> 20), t);
}

static void do_pack(const char *root, const char *outp, const ArOpt *o0) {
    ArOpt o = *o0; PackCtx c; EntList l; pthread_t *th = NULL; pthread_t gth; GpuArg ga; CmGpu gpu; int gpu_on = 0, i;
    size_t fi; u8 *ub = NULL; u32 un = 0, umax = o.umax ? o.umax : UMAX_DEFAULT, next_id = 0;
    u64 S = 0, total_in = 0; unsigned long changed = 0, failed = 0, ntx = 0, ntxfail = 0, nfiles = 0; struct stat ost;
    u8 hdr[8] = {'G','P','Z','A',1,0,0,0};
    memset(&c, 0, sizeof c); memset(&l, 0, sizeof l);
    crc32(NULL, 0); cm_tables(); if (!o.notx) zl_load();
    pthread_mutex_init(&c.mu, NULL); pthread_cond_init(&c.cv_job, NULL); pthread_cond_init(&c.cv_space, NULL);
    c.o = o; c.t0 = now(); c.qlimit = (size_t)1 << 30;
    c.cpu_tbits_cap = o.tbits ? o.tbits : 21;
    if (o.threads < 1) { long k = sysconf(_SC_NPROCESSORS_ONLN); o.threads = k > 1 ? (int)k : 1; }
    c.out = fopen(outp, "wb");
    if (!c.out) { perror(outp); exit(1); }
    if (fstat(fileno(c.out), &ost) == 0) { skip_dev = ost.st_dev; skip_ino = ost.st_ino; skip_set = 1; }
    wr(&c, hdr, 8);

    /* GPU */
    if (!o.force_cpu) {
        DevInfo dv[MAXDEV]; int n = list_devices(dv, getenv("GPZ_ANY_DEVICE") == NULL);
        if (n > o.dev) {
            cl_ulong vram = dv[o.dev].mem, budget = o.vram_mb ? (cl_ulong)o.vram_mb << 20 : vram / 10 * 8;
            u32 t, want = o.slots ? o.slots : 48;
            if (o.tbits) t = o.tbits;
            else for (t = 21; t > 17; t--) {
                cl_ulong per = ((cm_arena_size(t) + 255) & ~(size_t)255) + 2ull * umax + 160;
                if (budget / per >= want) break;
            }
            {
                int e = cmgpu_open(&gpu, dv[o.dev].dev, budget, t, umax, want);
                if (e) {
                    if (o.force_gpu) { fprintf(stderr, "gpz: cannot use GPU (OpenCL error %d)\n", e); exit(1); }
                    fprintf(stderr, "gpz: GPU unavailable (error %d), CPU only\n", e);
                } else {
                    gpu_on = 1; o.tbits = t; c.o.tbits = t; c.gpu_alive = 1;
                    fprintf(stderr, "gpz: GPU %s: %lu MB of %lu MB device memory, %lu parallel streams (model 2^%u)\n",
                            dv[o.dev].name, (unsigned long)(gpu.used >> 20), (unsigned long)(vram >> 20), (unsigned long)gpu.nslots, t);
                }
            }
        } else if (o.force_gpu) die("no usable OpenCL GPU found (try: gpz list)");
    }
    if (gpu_on && o.threads > 1) o.threads--;          /* one core drives the GPU */
    th = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)o.threads);
    for (i = 0; i < o.threads; i++) pthread_create(&th[i], NULL, pack_cpu_worker, &c);
    if (gpu_on) { ga.c = &c; ga.g = &gpu; pthread_create(&gth, NULL, pack_gpu_thread, &ga); }
    fprintf(stderr, "gpz: %d CPU thread(s)%s\n", o.threads, gpu_on ? " + GPU" : "");

    walk(root, "", &o, &l);
    ub = (u8 *)malloc(umax);
    if (!ub) die("out of memory");
#define EMIT() do { if (un) { u8 *d = (u8 *)realloc(ub, un); queue_unit(&c, d ? d : ub, un, &next_id, gpu_on); ub = (u8 *)malloc(umax); if (!ub) die("out of memory"); un = 0; } } while (0)
#define APPEND(P, K) do { const u8 *pp_ = (P); size_t kk_ = (K); while (kk_) { size_t m_ = umax - un; if (m_ > kk_) m_ = kk_; memcpy(ub + un, pp_, m_); un += (u32)m_; pp_ += m_; kk_ -= m_; if (un == umax) EMIT(); } } while (0)
    for (fi = 0; fi < l.n; fi++) {
        Ent *e = &l.e[fi]; char fp[4096];
        if (e->type != 2) continue;
        snprintf(fp, sizeof fp, "%s/%s", root, e->path);
        {
            FILE *f = fopen(fp, "rb"); struct stat s1, s2; u8 *data = NULL; size_t got = 0;
            if (!f) { fprintf(stderr, "\ngpz: cannot open %s: skipped\n", e->path); e->type = 0; failed++; continue; }
            if (fstat(fileno(f), &s1) != 0) s1.st_size = (off_t)e->size;
            if ((u64)s1.st_size > ((u64)256 << 20)) {         /* huge file: stream it, no transform */
                u8 *chunk = (u8 *)malloc(umax); size_t k; u32 crc = 0; u64 tot = 0;
                if (!chunk) die("out of memory");
                e->soff = S;
                if (un) EMIT();
                while ((k = fread(chunk, 1, umax, f)) > 0) { crc = crc32u(crc, chunk, k); APPEND(chunk, k); tot += k; }
                free(chunk); fclose(f);
                e->size = tot; e->tlen = tot; e->crc = crc; e->tx = 0; S += tot; total_in += tot; nfiles++;
                continue;
            }
            data = (u8 *)malloc((size_t)s1.st_size + 1);
            if (!data) die("out of memory");
            got = fread(data, 1, (size_t)s1.st_size, f);
            if (fstat(fileno(f), &s2) == 0 && (s2.st_size != s1.st_size || MTIME_S(s2) != MTIME_S(s1) || MTIME_NS(s2) != MTIME_NS(s1))) changed++;
            fclose(f);
            e->size = got; e->crc = crc32(data, got); total_in += got;
            {
                u8 *slice = data; size_t sl = got; int nr = 0, nf = 0;
                if (!o.notx && zl_load() && got >= 30) {
                    size_t tn; u8 *t = is_mca(e->path) ? tx_build_mca(data, got, &tn, &nr, &nf) : tx_build(data, got, &tn, &nr, &nf);
                    ntxfail += (unsigned long)nf;
                    if (nr) { slice = t; sl = tn; e->tx = 1; ntx += (unsigned long)nr; } else free(t);
                }
                e->soff = S; e->tlen = sl; S += sl; nfiles++;
                if (sl >= umax / 2 && un) EMIT();
                APPEND(slice, sl);
                if (slice != data) free(slice);
            }
            free(data);
        }
        progress(&c, fi + 1, l.n, total_in);
    }
    EMIT();
    free(ub);
    pthread_mutex_lock(&c.mu); c.eof = 1; pthread_cond_broadcast(&c.cv_job); pthread_mutex_unlock(&c.mu);
    for (i = 0; i < o.threads; i++) pthread_join(th[i], NULL);
    if (gpu_on) pthread_join(gth, NULL);
    {
        double t = now() - c.t0;
        fprintf(stderr, "\r%78s\r", "");
        if (verbose && gpu_on) fprintf(stderr, "gpz: GPU compressed %lu of %lu units (%u launches, %.1fs in kernels)\n", (unsigned long)c.gpu_units, (unsigned long)c.done_units, gpu.launches, gpu.ktime);
        (void)t;
    }

    /* meta */
    {
        Buf m; u8 *mc; size_t mcn; u32 mraw; u8 codec = 1, mtb = 18; u64 table_off, meta_off; u8 foot[AR_FOOT]; size_t k;
        memset(&m, 0, sizeof m);
        buf_u32(&m, 1); buf_u64(&m, (u64)time(NULL));
        {   const char *b = base_of(root); size_t bl = strlen(b); if (bl > 65535) bl = 65535; buf_u16(&m, (u32)bl); buf_add(&m, b, bl); }
        buf_u32(&m, (u32)l.n);
        for (fi = 0; fi < l.n; fi++) {
            Ent *e = &l.e[fi]; size_t pl;
            if (e->type == 0) continue;
            pl = strlen(e->path);
            buf_u8(&m, e->type); buf_u32(&m, e->mode); buf_u32(&m, e->uid); buf_u32(&m, e->gid);
            buf_u64(&m, (u64)e->mtime); buf_u32(&m, e->mnsec); buf_u64(&m, e->size); buf_u32(&m, e->crc);
            buf_u64(&m, e->soff); buf_u64(&m, e->tlen); buf_u8(&m, e->tx);
            buf_u16(&m, (u32)pl); buf_add(&m, e->path, pl);
            if (e->type == 3) { size_t tl = strlen(e->target); buf_u16(&m, (u32)tl); buf_add(&m, e->target, tl); }
        }
        mraw = (u32)m.n;
        mc = (u8 *)malloc(m.n + 1024); mcn = cm_compress(m.p, m.n, mc, m.n, mtb);
        if (!mcn || mcn >= m.n) { codec = 0; mcn = m.n; memcpy(mc, m.p, m.n); }
        meta_off = c.outpos; wr(&c, mc, mcn);
        table_off = c.outpos;
        for (k = 0; k < c.nrec; k++) {
            u8 r[24]; memset(r, 0, 24);
            put64(r, c.rec[k].off); put32(r + 8, c.rec[k].comp); put32(r + 12, c.rec[k].raw); r[16] = c.rec[k].tbits; r[17] = c.rec[k].codec;
            wr(&c, r, 24);
        }
        memset(foot, 0, AR_FOOT); memcpy(foot, "GPZE", 4); put32(foot + 4, 1);
        put64(foot + 8, meta_off); put32(foot + 16, (u32)mcn); put32(foot + 20, mraw); foot[24] = codec; foot[25] = mtb;
        put32(foot + 28, (u32)c.nrec); put64(foot + 32, table_off); put32(foot + 40, (u32)l.n);
        put32(foot + 44, c.crc);                                  /* crc of everything before the footer */
        put64(foot + 48, S); put64(foot + 56, (u64)time(NULL)); put64(foot + 64, total_in); put32(foot + 72, umax);
        if (fwrite(foot, 1, AR_FOOT, c.out) != AR_FOOT || fclose(c.out)) die("write error");
        free(mc); free(m.p);
    }
    fprintf(stderr, "gpz: packed %lu files, %lu MB -> %s: %lu MB (%.1f%%) in %.0fs\n", nfiles, (unsigned long)(total_in >> 20), outp,
            (unsigned long)((c.outpos + AR_FOOT) >> 20), total_in ? 100.0 * (double)(c.outpos + AR_FOOT) / (double)total_in : 100.0, now() - c.t0);
    if (ntx || ntxfail) fprintf(stderr, "gpz: %lu deflate streams stored uncompressed-then-recompressed, %lu could not be reproduced exactly (kept as-is)\n", ntx, ntxfail);
    if (skipped_special) fprintf(stderr, "gpz: skipped %lu special files (sockets, devices, fifos)\n", skipped_special);
    if (failed) fprintf(stderr, "gpz: WARNING: %lu files could not be read and are NOT in the archive\n", failed);
    if (changed) fprintf(stderr, "gpz: WARNING: %lu files changed while being read - this is not a consistent snapshot.\n"
                                 "      For a server: run `save-off` and `save-all flush` first, then `save-on` afterwards.\n", changed);
    for (fi = 0; fi < l.n; fi++) { free(l.e[fi].path); free(l.e[fi].target); }
    free(l.e); free(th); free(c.rec);
}

/* ---- reading an archive ---- */
typedef struct {
    FILE *f; u8 foot[AR_FOOT]; u8 *meta; size_t metan; URec *units; u32 nunits; Ent *e; u32 ne;
    u64 created, S, total_in, fsize; char srcname[256]; u32 umax;
} Arch;

static void ar_open(Arch *a, const char *path) {
    u8 *mc, *mraw; u32 mcn, mrn, i; u64 mo, to; size_t p; u8 *tb;
    memset(a, 0, sizeof *a);
    a->f = fopen(path, "rb");
    if (!a->f) { perror(path); exit(1); }
    fseeko(a->f, 0, SEEK_END); a->fsize = (u64)ftello(a->f);
    if (a->fsize < 8 + AR_FOOT) die("not a GPZA archive");
    fseeko(a->f, (off_t)(a->fsize - AR_FOOT), SEEK_SET);
    if (fread(a->foot, 1, AR_FOOT, a->f) != AR_FOOT || memcmp(a->foot, "GPZE", 4)) die("not a GPZA archive (or truncated)");
    mo = get64(a->foot + 8); mcn = get32(a->foot + 16); mrn = get32(a->foot + 20);
    a->nunits = get32(a->foot + 28); to = get64(a->foot + 32); a->ne = get32(a->foot + 40);
    a->S = get64(a->foot + 48); a->created = get64(a->foot + 56); a->total_in = get64(a->foot + 64); a->umax = get32(a->foot + 72);
    if (mo + mcn > a->fsize || to + (u64)a->nunits * 24 > a->fsize) die("corrupt archive footer");
    mc = (u8 *)malloc((size_t)mcn + 1); mraw = (u8 *)malloc((size_t)mrn + 1); tb = (u8 *)malloc((size_t)a->nunits * 24 + 1);
    if (!mc || !mraw || !tb) die("out of memory");
    fseeko(a->f, (off_t)mo, SEEK_SET); if (fread(mc, 1, mcn, a->f) != mcn) die("truncated archive");
    if (a->foot[24] == 1) { cm_tables(); if (cm_decompress(mc, mcn, mraw, mrn, a->foot[25])) die("corrupt metadata"); }
    else { if (mcn != mrn) die("corrupt metadata"); memcpy(mraw, mc, mrn); }
    free(mc);
    fseeko(a->f, (off_t)to, SEEK_SET); if (fread(tb, 1, (size_t)a->nunits * 24, a->f) != (size_t)a->nunits * 24) die("truncated archive");
    a->units = (URec *)calloc(a->nunits + 1, sizeof(URec));
    for (i = 0; i < a->nunits; i++) {
        a->units[i].off = get64(tb + 24 * i); a->units[i].comp = get32(tb + 24 * i + 8); a->units[i].raw = get32(tb + 24 * i + 12);
        a->units[i].tbits = tb[24 * i + 16]; a->units[i].codec = tb[24 * i + 17];
    }
    free(tb);
    /* parse meta */
#define NEED(k) do { if (p + (k) > mrn) die("corrupt metadata"); } while (0)
    p = 0; NEED(14);
    p += 4; a->created = get64(mraw + p); p += 8;
    {
        u32 sl = mraw[p] | (mraw[p + 1] << 8), cp; p += 2; NEED(sl + 4);
        cp = sl > 255 ? 255 : sl; memcpy(a->srcname, mraw + p, cp); a->srcname[cp] = 0; p += sl;
    }
    a->ne = get32(mraw + p); p += 4;
    a->e = (Ent *)calloc(a->ne + 1, sizeof(Ent));
    for (i = 0; i < a->ne; i++) {
        Ent *e = &a->e[i]; u32 pl;
        NEED(1 + 4 * 3 + 8 + 4 + 8 + 4 + 8 + 8 + 1 + 2);
        e->type = mraw[p++]; e->mode = get32(mraw + p); p += 4; e->uid = get32(mraw + p); p += 4; e->gid = get32(mraw + p); p += 4;
        e->mtime = (long long)get64(mraw + p); p += 8; e->mnsec = get32(mraw + p); p += 4; e->size = get64(mraw + p); p += 8;
        e->crc = get32(mraw + p); p += 4; e->soff = get64(mraw + p); p += 8; e->tlen = get64(mraw + p); p += 8; e->tx = mraw[p++];
        pl = mraw[p] | (mraw[p + 1] << 8); p += 2; NEED(pl);
        e->path = (char *)malloc(pl + 1); memcpy(e->path, mraw + p, pl); e->path[pl] = 0; p += pl;
        if (e->type == 3) { u32 tl; NEED(2); tl = mraw[p] | (mraw[p + 1] << 8); p += 2; NEED(tl); e->target = (char *)malloc(tl + 1); memcpy(e->target, mraw + p, tl); e->target[tl] = 0; p += tl; }
    }
#undef NEED
    free(mraw);
}

static const char *fmt_time(long long t, char *buf) {
    time_t tt = (time_t)t; struct tm tmv;
    gmtime_r(&tt, &tmv); strftime(buf, 32, "%Y-%m-%d %H:%M:%S UTC", &tmv); return buf;
}

static void do_ls(const char *path, int quiet) {
    Arch a; u32 i; char tb[32];
    ar_open(&a, path);
    printf("snapshot of '%s' taken %s\n%lu entries, %lu MB original -> %lu MB archive (%.1f%%), %lu units\n",
           a.srcname, fmt_time((long long)a.created, tb), (unsigned long)a.ne, (unsigned long)(a.total_in >> 20),
           (unsigned long)(a.fsize >> 20), a.total_in ? 100.0 * (double)a.fsize / (double)a.total_in : 100.0, (unsigned long)a.nunits);
    if (quiet) return;
    for (i = 0; i < a.ne; i++) {
        Ent *e = &a.e[i];
        printf("%c %04o %10lu %s %s%s%s\n", e->type == 1 ? 'd' : e->type == 3 ? 'l' : '-', e->mode & 07777,
               (unsigned long)e->size, fmt_time(e->mtime, tb), e->path, e->type == 3 ? " -> " : "", e->type == 3 ? e->target : "");
    }
}

/* ---- unpack / verify ---- */
typedef struct DUnit { u32 id; u8 *in, *out; u32 cn, n, tbits; int ready, status, cpu_only; CmJob job; struct DUnit *next; } DUnit;

typedef struct {
    Arch *a; ArOpt o; pthread_mutex_t mu; pthread_cond_t cv_job, cv_ready, cv_space;
    DUnit *qh, *qt, **slot; u32 next_make, consumed; int eof, gpu_alive; size_t inflight_bytes, limit;
    u64 gpu_units;
} UnCtx;

static void un_decode_cpu(DUnit *u) {
    u->status = cm_decompress(u->in, u->cn, u->out, u->n, u->tbits) ? 1 : 0;
}
static void un_finish(UnCtx *c, DUnit *u) {
    pthread_mutex_lock(&c->mu);
    c->slot[u->id] = u; u->ready = 1;
    pthread_cond_broadcast(&c->cv_ready);
    pthread_mutex_unlock(&c->mu);
}
static void *un_cpu_worker(void *arg) {
    UnCtx *c = (UnCtx *)arg;
    for (;;) {
        DUnit *u;
        pthread_mutex_lock(&c->mu);
        while (!c->qh && !c->eof) pthread_cond_wait(&c->cv_job, &c->mu);
        u = c->qh;
        if (u) { c->qh = u->next; if (!c->qh) c->qt = NULL; }
        pthread_mutex_unlock(&c->mu);
        if (!u) return NULL;
        un_decode_cpu(u);
        un_finish(c, u);
    }
}
static int un_gpu_next(void *ctx, int block, CmJob **job) {
    UnCtx *c = (UnCtx *)ctx; DUnit *u, *prev;
    pthread_mutex_lock(&c->mu);
    for (;;) {
        for (u = c->qh, prev = NULL; u && u->cpu_only; prev = u, u = u->next) ;
        if (u) break;
        if (c->eof) { pthread_mutex_unlock(&c->mu); return -1; }
        if (!block) { pthread_mutex_unlock(&c->mu); return 0; }
        pthread_cond_wait(&c->cv_job, &c->mu);
    }
    if (prev) prev->next = u->next; else c->qh = u->next;
    if (c->qt == u) c->qt = prev;
    pthread_mutex_unlock(&c->mu);
    u->job.in = u->in; u->job.cn = u->cn; u->job.n = u->n; u->job.tbits = u->tbits; u->job.out = u->out; u->job.user = u;
    *job = &u->job;
    return 1;
}
static void un_gpu_done(void *ctx, CmJob *j) {
    UnCtx *c = (UnCtx *)ctx; DUnit *u = (DUnit *)j->user;
    if (j->status == 2) {                      /* GPU could not: CPU workers take it */
        u->cpu_only = 1; u->next = NULL;
        pthread_mutex_lock(&c->mu);
        if (c->qt) c->qt->next = u; else c->qh = u;
        c->qt = u; pthread_cond_broadcast(&c->cv_job);
        pthread_mutex_unlock(&c->mu);
        return;
    }
    u->status = j->status ? 1 : 0;
    pthread_mutex_lock(&c->mu); c->gpu_units++; pthread_mutex_unlock(&c->mu);
    un_finish(c, u);
}
typedef struct { UnCtx *c; CmGpu *g; } UnGpuArg;
static void *un_gpu_thread(void *arg) {
    UnGpuArg *a = (UnGpuArg *)arg;
    int e = cmgpu_run(a->g, 1, un_gpu_next, un_gpu_done, a->c);
    if (e) fprintf(stderr, "\ngpz: GPU stopped (OpenCL error %d); CPU threads finish the rest\n", e);
    pthread_mutex_lock(&a->c->mu); a->c->gpu_alive = 0;
    { DUnit *u; for (u = a->c->qh; u; u = u->next) u->cpu_only = 1; }
    pthread_cond_broadcast(&a->c->cv_job); pthread_mutex_unlock(&a->c->mu);
    return NULL;
}

static void *un_producer(void *arg) {      /* reads unit blobs in order, feeds the workers */
    UnCtx *c = (UnCtx *)arg; Arch *a = c->a; u32 i;
    for (i = 0; i < a->nunits; i++) {
        URec *r = &a->units[i]; DUnit *u = (DUnit *)calloc(1, sizeof *u);
        if (!u) die("out of memory");
        u->id = i; u->n = r->raw; u->cn = r->comp; u->tbits = r->tbits;
        u->in = (u8 *)malloc((size_t)r->comp + 1); u->out = (u8 *)malloc((size_t)r->raw + 1);
        if (!u->in || !u->out) die("out of memory");
        pthread_mutex_lock(&c->mu);
        while (c->inflight_bytes > c->limit && i > c->consumed + 4) pthread_cond_wait(&c->cv_space, &c->mu);
        c->inflight_bytes += (size_t)r->raw + r->comp;
        pthread_mutex_unlock(&c->mu);
        if (fseeko(a->f, (off_t)r->off, SEEK_SET) || fread(u->in, 1, r->comp, a->f) != r->comp) die("truncated archive");
        if (r->codec == 0) {                 /* stored: no decoding needed */
            if (r->comp != r->raw) die("corrupt unit table");
            memcpy(u->out, u->in, r->raw); u->status = 0; un_finish(c, u); continue;
        }
        pthread_mutex_lock(&c->mu);
        u->next = NULL; if (c->qt) c->qt->next = u; else c->qh = u;
        c->qt = u; pthread_cond_broadcast(&c->cv_job);
        pthread_mutex_unlock(&c->mu);
    }
    pthread_mutex_lock(&c->mu); c->eof = 1; pthread_cond_broadcast(&c->cv_job); pthread_mutex_unlock(&c->mu);
    return NULL;
}

/* ---- filesystem helpers ---- */
static int mkdirs(const char *path) {
    char tmp[4096]; size_t i, l = strlen(path);
    if (l >= sizeof tmp) return -1;
    memcpy(tmp, path, l + 1);
    for (i = 1; i < l; i++)
        if (tmp[i] == '/') { tmp[i] = 0; if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1; tmp[i] = '/'; }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}
static int safe_rel(const char *p) {       /* refuse absolute paths and .. components */
    const char *s = p;
    if (!*p || p[0] == '/') return 0;
    while (*s) {
        const char *e = strchr(s, '/'); size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == 2 && s[0] == '.' && s[1] == '.') return 0;
        if (!e) break;
        s = e + 1;
    }
    return 1;
}
static void apply_meta(const char *path, const Ent *e, int is_root) {
    struct timespec ts[2];
    (void)is_root;
    if (geteuid() == 0) { if (lchown(path, (uid_t)e->uid, (gid_t)e->gid) != 0) { /* ignore */ } }
    if (e->type != 3) chmod(path, (mode_t)(e->mode & 07777));
    ts[0].tv_sec = (time_t)e->mtime; ts[0].tv_nsec = (long)e->mnsec; ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
}

static void do_unpack(const char *arch, const char *dest, const ArOpt *o0, int verify_only) {
    Arch a; UnCtx c; ArOpt o = *o0; u32 i; pthread_t prod, *th = NULL, gth; CmGpu gpu; int gpu_on = 0, k, bad = 0;
    DUnit *cur = NULL; u32 cur_i = 0; u64 cur_off = 0, done_files = 0, done_bytes = 0; double t0 = now();
    UnGpuArg ga; char tb[32]; Ent **dirs = NULL; u32 ndirs = 0;
    crc32(NULL, 0); cm_tables(); zl_load();
    ar_open(&a, arch);
    if (verify_only) {          /* whole-archive checksum first */
        u8 *buf = (u8 *)malloc(1 << 20); u32 crc = 0; u64 left = a.fsize - AR_FOOT; size_t n;
        fseeko(a.f, 0, SEEK_SET);
        while (left && (n = fread(buf, 1, left < (1u << 20) ? (size_t)left : (1u << 20), a.f)) > 0) { crc = crc32u(crc, buf, n); left -= n; }
        free(buf);
        if (crc != get32(a.foot + 44)) { fprintf(stderr, "gpz: ARCHIVE CORRUPT: checksum mismatch\n"); exit(1); }
    } else {
        struct stat st;
        if (stat(dest, &st) == 0 && !o.force) {
            DIR *d = opendir(dest); struct dirent *de; int nonempty = 0;
            if (d) { while ((de = readdir(d)) != NULL) if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { nonempty = 1; break; } closedir(d); }
            if (nonempty) { fprintf(stderr, "gpz: %s is not empty (use --force to extract into it anyway)\n", dest); exit(1); }
        }
        if (mkdirs(dest)) { perror(dest); exit(1); }
    }
    memset(&c, 0, sizeof c); c.a = &a; c.o = o; c.limit = (size_t)1 << 30;
    pthread_mutex_init(&c.mu, NULL); pthread_cond_init(&c.cv_job, NULL); pthread_cond_init(&c.cv_ready, NULL); pthread_cond_init(&c.cv_space, NULL);
    c.slot = (DUnit **)calloc(a.nunits + 1, sizeof(DUnit *));
    if (o.threads < 1) { long kk = sysconf(_SC_NPROCESSORS_ONLN); o.threads = kk > 1 ? (int)kk : 1; }
    if (!o.force_cpu && a.nunits) {
        DevInfo dv[MAXDEV]; int n = list_devices(dv, getenv("GPZ_ANY_DEVICE") == NULL);
        if (n > o.dev) {
            cl_ulong vram = dv[o.dev].mem, budget = o.vram_mb ? (cl_ulong)o.vram_mb << 20 : vram / 10 * 8;
            u32 maxt = 0, want = o.slots ? o.slots : 48, umax = a.umax ? a.umax : UMAX_DEFAULT;
            for (i = 0; i < a.nunits; i++) if (a.units[i].codec == 1 && a.units[i].tbits > maxt) maxt = a.units[i].tbits;
            if (maxt) {
                { int e = cmgpu_open(&gpu, dv[o.dev].dev, budget, maxt, umax, want);
                  if (e) { if (o.force_gpu) { fprintf(stderr, "gpz: cannot use GPU (OpenCL error %d)\n", e); exit(1); }
                           fprintf(stderr, "gpz: GPU unavailable (error %d), CPU only\n", e); }
                  else { gpu_on = 1; c.gpu_alive = 1;
                         fprintf(stderr, "gpz: GPU %s: %lu MB device memory, %lu parallel streams\n", dv[o.dev].name, (unsigned long)(gpu.used >> 20), (unsigned long)gpu.nslots); } }
            }
        } else if (o.force_gpu) die("no usable OpenCL GPU found (try: gpz list)");
    }
    if (gpu_on && o.threads > 1) o.threads--;
    th = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)o.threads);
    for (k = 0; k < o.threads; k++) pthread_create(&th[k], NULL, un_cpu_worker, &c);
    if (gpu_on) { ga.c = &c; ga.g = &gpu; pthread_create(&gth, NULL, un_gpu_thread, &ga); }
    pthread_create(&prod, NULL, un_producer, &c);

    for (i = 0; i < a.ne; i++) {
        Ent *e = &a.e[i]; char fp[4096]; u8 *slice = NULL;
        if (!safe_rel(e->path)) { fprintf(stderr, "gpz: refusing unsafe path in archive: %s\n", e->path); bad = 1; continue; }
        snprintf(fp, sizeof fp, "%s/%s", dest, e->path);
        if (e->type == 1) {
            if (!verify_only) {
                if (mkdirs(fp)) { perror(fp); bad = 1; }
                dirs = (Ent **)realloc(dirs, (ndirs + 1) * sizeof(Ent *)); dirs[ndirs++] = e;
            }
            continue;
        }
        if (e->type == 3) {
            if (!verify_only) {
                char pd[4096]; char *sl;
                snprintf(pd, sizeof pd, "%s", fp); sl = strrchr(pd, '/'); if (sl) { *sl = 0; mkdirs(pd); }
                unlink(fp);
                if (symlink(e->target, fp) != 0) { perror(fp); bad = 1; } else apply_meta(fp, e, 0);
            }
            continue;
        }
        if (e->type != 2) continue;
        /* gather this file's slice of the stream from the decoded units, in order */
        slice = (u8 *)malloc((size_t)e->tlen + 1);
        if (!slice) die("out of memory");
        {
            u64 need = e->tlen, got = 0;
            while (got < need) {
                if (!cur) {
                    if (cur_i >= a.nunits) die("corrupt archive: stream ends early");
                    pthread_mutex_lock(&c.mu);
                    while (!c.slot[cur_i]) pthread_cond_wait(&c.cv_ready, &c.mu);
                    cur = c.slot[cur_i]; c.slot[cur_i] = NULL;
                    pthread_mutex_unlock(&c.mu);
                    if (cur->status) { fprintf(stderr, "gpz: corrupt data in unit %lu\n", (unsigned long)cur_i); exit(1); }
                    cur_off = 0;
                }
                {
                    u64 avail = cur->n - cur_off, take = need - got < avail ? need - got : avail;
                    memcpy(slice + got, cur->out + cur_off, (size_t)take); got += take; cur_off += take;
                    if (cur_off == cur->n) {
                        pthread_mutex_lock(&c.mu); c.inflight_bytes -= (size_t)cur->n + cur->cn; c.consumed = cur_i + 1;
                        pthread_cond_broadcast(&c.cv_space); pthread_mutex_unlock(&c.mu);
                        free(cur->in); free(cur->out); free(cur); cur = NULL; cur_i++;
                    }
                }
            }
        }
        {
            u8 *data = slice; size_t dn = (size_t)e->tlen;
            if (e->tx) {
                data = (u8 *)malloc((size_t)e->size + 1);
                if (!data) die("out of memory");
                if (tx_restore(slice, (size_t)e->tlen, data, (size_t)e->size)) { fprintf(stderr, "gpz: cannot restore %s\n", e->path); exit(1); }
                dn = (size_t)e->size;
            }
            if (dn != e->size || crc32(data, dn) != e->crc) { fprintf(stderr, "gpz: CHECKSUM MISMATCH in %s\n", e->path); bad = 1; }
            else if (!verify_only) {
                char pd[4096]; char *sl; FILE *f;
                snprintf(pd, sizeof pd, "%s", fp); sl = strrchr(pd, '/'); if (sl) { *sl = 0; mkdirs(pd); }
                f = fopen(fp, "wb");
                if (!f || fwrite(data, 1, dn, f) != dn || fclose(f)) { perror(fp); bad = 1; }
                else apply_meta(fp, e, 0);
            }
            done_files++; done_bytes += dn;
            if (data != slice) free(data);
        }
        free(slice);
        { static double last; double t = now() - t0; if (t - last >= 1.0) { last = t; fprintf(stderr, "\r  %lu/%lu entries, %lu MB   ", (unsigned long)i, (unsigned long)a.ne, (unsigned long)(done_bytes >> 20)); } }
    }
    pthread_join(prod, NULL);
    for (k = 0; k < o.threads; k++) pthread_join(th[k], NULL);
    if (gpu_on) pthread_join(gth, NULL);
    if (!verify_only) {   /* directory metadata last, deepest first, so file writes don't change mtimes */
        u32 d;
        for (d = ndirs; d > 0; d--) { char fp[4096]; snprintf(fp, sizeof fp, "%s/%s", dest, dirs[d - 1]->path); apply_meta(fp, dirs[d - 1], 0); }
    }
    fprintf(stderr, "\r%78s\r", "");
    if (bad) { fprintf(stderr, "gpz: %s FAILED\n", verify_only ? "verification" : "extraction"); exit(1); }
    if (verbose && gpu_on) fprintf(stderr, "gpz: GPU decoded %lu of %lu units\n", (unsigned long)c.gpu_units, (unsigned long)a.nunits);
    fprintf(stderr, "gpz: %s: %lu files, %lu MB, snapshot of '%s' from %s, in %.0fs\n",
            verify_only ? "archive verified OK" : "extracted", (unsigned long)done_files, (unsigned long)(done_bytes >> 20), a.srcname,
            fmt_time((long long)a.created, tb), now() - t0);
    free(dirs);
}

#else
typedef struct { int dummy; } ArOpt;
static void do_pack(const char *a, const char *b, const ArOpt *o) { (void)a; (void)b; (void)o; die("archive mode needs a POSIX system"); }
static void do_unpack(const char *a, const char *b, const ArOpt *o, int v) { (void)a; (void)b; (void)o; (void)v; die("archive mode needs a POSIX system"); }
static void do_ls(const char *a, int q) { (void)a; (void)q; die("archive mode needs a POSIX system"); }
#endif
