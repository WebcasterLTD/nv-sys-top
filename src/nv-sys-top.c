/*
 * nv-sys-top - an htop-style system monitor with NVIDIA GPUs in the same view.
 *
 * Top:    one meter per CPU core, memory, swap, load, uptime.
 * Middle: every NVIDIA GPU - load, VRAM, temperature, power, fan, clock.
 * Bottom: one process list with CPU, RAM, GPU load and VRAM side by side.
 *
 * CPU, memory and processes come straight from /proc. GPU data comes from
 * NVML (libnvidia-ml.so.1, shipped with the NVIDIA driver), loaded at run
 * time: no CUDA toolkit is needed to build, and without an NVIDIA driver the
 * program still runs and shows the system part only.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION  "1.0.0"
#define MAX_GPUS 32
#define MAX_CPUS 1024
#define CMD_LEN  512

/* ------------------------------------------------------------------ */
/* NVML, loaded at run time                                            */
/* ------------------------------------------------------------------ */

typedef int nvret;
typedef void *nvdev;
typedef struct { unsigned gpu, memory; } nv_util;
typedef struct { unsigned long long total, free, used; } nv_mem;
typedef struct { unsigned pid; unsigned long long mem; unsigned gi, ci; } nv_proc2; /* _v2 / _v3 */
typedef struct { unsigned pid; unsigned long long mem; } nv_proc1;                    /* original */
typedef struct { unsigned pid; unsigned long long ts; unsigned sm, mem, enc, dec; } nv_sample;

#define NV_SUCCESS            0
#define NV_TEMPERATURE_GPU    0
#define NV_CLOCK_SM           1
#define NV_VALUE_NA           0xFFFFFFFFFFFFFFFFULL

static struct {
    void *lib;
    int ok;
    size_t proc_size;
    nvret (*init)(void);
    nvret (*shutdown)(void);
    nvret (*count)(unsigned *);
    nvret (*handle)(unsigned, nvdev *);
    nvret (*name)(nvdev, char *, unsigned);
    nvret (*util)(nvdev, nv_util *);
    nvret (*mem)(nvdev, nv_mem *);
    nvret (*temp)(nvdev, int, unsigned *);
    nvret (*power)(nvdev, unsigned *);
    nvret (*plimit)(nvdev, unsigned *);
    nvret (*fan)(nvdev, unsigned *);
    nvret (*clock)(nvdev, int, unsigned *);
    nvret (*compute)(nvdev, unsigned *, void *);
    nvret (*graphics)(nvdev, unsigned *, void *);
    nvret (*samples)(nvdev, nv_sample *, unsigned *, unsigned long long);
} nv;

typedef struct {
    nvdev dev;
    char name[96];
    unsigned util, temp, fan, clk, pw, pwl;   /* pw, pwl in mW */
    unsigned long long mem, memtot;           /* bytes */
    unsigned long long last_ts;               /* newest process sample seen */
} gpu_t;

static gpu_t gpus[MAX_GPUS];
static unsigned ngpu;

#define LOAD(field, symname) (*(void **)&nv.field = dlsym(nv.lib, symname))

static void nv_open(void)
{
    nv.lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (!nv.lib)
        nv.lib = dlopen("libnvidia-ml.so", RTLD_NOW);
    if (!nv.lib)
        return;

    if (!LOAD(init, "nvmlInit_v2")) LOAD(init, "nvmlInit");
    LOAD(shutdown, "nvmlShutdown");
    if (!LOAD(count, "nvmlDeviceGetCount_v2")) LOAD(count, "nvmlDeviceGetCount");
    if (!LOAD(handle, "nvmlDeviceGetHandleByIndex_v2")) LOAD(handle, "nvmlDeviceGetHandleByIndex");
    LOAD(name, "nvmlDeviceGetName");
    LOAD(util, "nvmlDeviceGetUtilizationRates");
    LOAD(mem, "nvmlDeviceGetMemoryInfo");
    LOAD(temp, "nvmlDeviceGetTemperature");
    LOAD(power, "nvmlDeviceGetPowerUsage");
    LOAD(plimit, "nvmlDeviceGetEnforcedPowerLimit");
    LOAD(fan, "nvmlDeviceGetFanSpeed");
    LOAD(clock, "nvmlDeviceGetClockInfo");
    LOAD(samples, "nvmlDeviceGetProcessUtilization");

    /* Process lists: take the newest version both calls share. */
    nv.proc_size = sizeof(nv_proc2);
    if (LOAD(compute, "nvmlDeviceGetComputeRunningProcesses_v3"))
        LOAD(graphics, "nvmlDeviceGetGraphicsRunningProcesses_v3");
    else if (LOAD(compute, "nvmlDeviceGetComputeRunningProcesses_v2"))
        LOAD(graphics, "nvmlDeviceGetGraphicsRunningProcesses_v2");
    else {
        LOAD(compute, "nvmlDeviceGetComputeRunningProcesses");
        LOAD(graphics, "nvmlDeviceGetGraphicsRunningProcesses");
        nv.proc_size = sizeof(nv_proc1);
    }

    if (!nv.init || !nv.count || !nv.handle || nv.init() != NV_SUCCESS)
        return;

    unsigned n = 0;
    if (nv.count(&n) != NV_SUCCESS)
        return;
    for (unsigned i = 0; i < n && ngpu < MAX_GPUS; i++) {
        gpu_t *g = &gpus[ngpu];
        if (nv.handle(i, &g->dev) != NV_SUCCESS)
            continue;
        char raw[96] = "GPU";
        if (nv.name)
            nv.name(g->dev, raw, sizeof raw);
        const char *s = raw;
        if (!strncmp(s, "NVIDIA ", 7)) s += 7;
        if (!strncmp(s, "GeForce ", 8)) s += 8;
        snprintf(g->name, sizeof g->name, "%s", s);
        ngpu++;
    }
    nv.ok = 1;
}

static void nv_close(void)
{
    if (nv.ok && nv.shutdown)
        nv.shutdown();
    if (nv.lib)
        dlclose(nv.lib);
}

/* ------------------------------------------------------------------ */
/* Processes                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int pid;
    uid_t uid;
    char st;
    unsigned long long ticks;     /* utime + stime */
    unsigned long long rss;       /* bytes */
    double cpu, mem;              /* percent */
    unsigned gmask;               /* bit i = on GPU i */
    unsigned gutil;               /* SM load, percent */
    unsigned long long vram;      /* bytes */
    char user[12];
    char comm[64];
    char cmd[CMD_LEN];
} proc_t;

typedef struct { int pid; unsigned long long ticks; } prev_t;

static proc_t *procs;
static int nprocs, cap_procs;
static prev_t *prev;
static int nprev, cap_prev;

static long hz, pagesize;

/* System counters */
static int ncpu;
static double cpu_pct[MAX_CPUS], cpu_all;
static unsigned long long cpu_tot0[MAX_CPUS + 1], cpu_busy0[MAX_CPUS + 1];
static unsigned long long mem_total, mem_avail, swap_total, swap_free;   /* kB */
static double load1, load5, load15, uptime_s;

static size_t read_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    size_t got = 0;
    ssize_t r;
    while (got < n - 1 && (r = read(fd, buf + got, n - 1 - got)) > 0)
        got += (size_t)r;
    close(fd);
    buf[got] = 0;
    return got;
}

static const char *user_name(uid_t uid)
{
    static struct { uid_t uid; char name[12]; } cache[128];
    static int n;
    for (int i = 0; i < n; i++)
        if (cache[i].uid == uid)
            return cache[i].name;
    struct passwd *pw = getpwuid(uid);
    int slot = n < 128 ? n++ : 127;
    cache[slot].uid = uid;
    if (pw)
        snprintf(cache[slot].name, sizeof cache[slot].name, "%s", pw->pw_name);
    else
        snprintf(cache[slot].name, sizeof cache[slot].name, "%u", (unsigned)uid);
    return cache[slot].name;
}

static int read_proc(int pid, proc_t *p)
{
    char path[64], buf[2048];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (!read_file(path, buf, sizeof buf))
        return 0;
    char *l = strchr(buf, '('), *r = strrchr(buf, ')');
    if (!l || !r || r < l)
        return 0;
    size_t cl = (size_t)(r - l - 1);
    if (cl >= sizeof p->comm)
        cl = sizeof p->comm - 1;
    memcpy(p->comm, l + 1, cl);
    p->comm[cl] = 0;

    unsigned long long ut, stt;
    long long rss;
    /* field 3 state, 4..13 skipped, 14 utime, 15 stime, 16..23 skipped, 24 rss */
    if (sscanf(r + 2, "%c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu %llu "
                      "%*s %*s %*s %*s %*s %*s %*s %*s %lld",
               &p->st, &ut, &stt, &rss) != 4)
        return 0;
    p->pid = pid;
    p->ticks = ut + stt;
    p->rss = rss > 0 ? (unsigned long long)rss * (unsigned long long)pagesize : 0;

    struct stat sb;
    snprintf(path, sizeof path, "/proc/%d", pid);
    p->uid = stat(path, &sb) == 0 ? sb.st_uid : 0;
    snprintf(p->user, sizeof p->user, "%s", user_name(p->uid));

    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    size_t n = read_file(path, p->cmd, sizeof p->cmd);
    for (size_t i = 0; i < n; i++)
        if (p->cmd[i] == 0 || p->cmd[i] == '\n' || p->cmd[i] == '\t')
            p->cmd[i] = ' ';
    while (n > 0 && p->cmd[n - 1] == ' ')
        p->cmd[--n] = 0;
    if (n == 0)
        snprintf(p->cmd, sizeof p->cmd, "[%s]", p->comm);

    p->cpu = p->mem = 0;
    p->gmask = p->gutil = 0;
    p->vram = 0;
    return 1;
}

static int by_pid(const void *a, const void *b)
{
    int x = ((const proc_t *)a)->pid, y = ((const proc_t *)b)->pid;
    return (x > y) - (x < y);
}

static proc_t *find_proc(int pid)
{
    proc_t key = { .pid = pid };
    return bsearch(&key, procs, (size_t)nprocs, sizeof *procs, by_pid);
}

static void scan_procs(double dt)
{
    DIR *d = opendir("/proc");
    if (!d)
        return;
    nprocs = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        if (nprocs == cap_procs) {
            cap_procs = cap_procs ? cap_procs * 2 : 1024;
            procs = realloc(procs, (size_t)cap_procs * sizeof *procs);
            if (!procs) { endwin(); fprintf(stderr, "out of memory\n"); exit(1); }
        }
        if (read_proc(atoi(e->d_name), &procs[nprocs]))
            nprocs++;
    }
    closedir(d);
    qsort(procs, (size_t)nprocs, sizeof *procs, by_pid);

    /* CPU% from the tick delta since the last scan; both lists are pid-sorted */
    int j = 0;
    for (int i = 0; i < nprocs; i++) {
        proc_t *p = &procs[i];
        while (j < nprev && prev[j].pid < p->pid)
            j++;
        if (dt > 0 && j < nprev && prev[j].pid == p->pid && p->ticks >= prev[j].ticks)
            p->cpu = (double)(p->ticks - prev[j].ticks) * 100.0 / ((double)hz * dt);
        if (mem_total)
            p->mem = (double)p->rss * 100.0 / ((double)mem_total * 1024.0);
    }

    if (nprocs > cap_prev) {
        cap_prev = cap_procs;
        prev = realloc(prev, (size_t)cap_prev * sizeof *prev);
        if (!prev) { endwin(); fprintf(stderr, "out of memory\n"); exit(1); }
    }
    for (int i = 0; i < nprocs; i++) {
        prev[i].pid = procs[i].pid;
        prev[i].ticks = procs[i].ticks;
    }
    nprev = nprocs;
}

/* ------------------------------------------------------------------ */
/* System and GPU sampling                                             */
/* ------------------------------------------------------------------ */

static void scan_cpu(void)
{
    static char buf[1 << 16];
    if (!read_file("/proc/stat", buf, sizeof buf))
        return;
    char *line = buf;
    int maxcore = -1;
    while (line && !strncmp(line, "cpu", 3)) {
        int idx = 0;                       /* 0 = all cores, i+1 = core i */
        char *s = line + 3;
        if (isdigit((unsigned char)*s))
            idx = atoi(s) + 1;
        while (*s && *s != ' ') s++;
        unsigned long long v[8] = { 0 };
        sscanf(s, "%llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        unsigned long long tot = 0;
        for (int k = 0; k < 8; k++) tot += v[k];
        unsigned long long busy = tot - v[3] - v[4];   /* minus idle, iowait */
        if (idx <= MAX_CPUS) {
            unsigned long long dtot = tot - cpu_tot0[idx], dbusy = busy - cpu_busy0[idx];
            double pct = (cpu_tot0[idx] && dtot) ? (double)dbusy * 100.0 / (double)dtot : 0;
            if (idx == 0) cpu_all = pct;
            else cpu_pct[idx - 1] = pct;
            cpu_tot0[idx] = tot;
            cpu_busy0[idx] = busy;
            if (idx > 0 && idx - 1 > maxcore) maxcore = idx - 1;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    ncpu = maxcore + 1;
}

static void scan_mem(void)
{
    char buf[8192];
    if (!read_file("/proc/meminfo", buf, sizeof buf))
        return;
    for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        unsigned long long v;
        if (sscanf(l, "MemTotal: %llu", &v) == 1) mem_total = v;
        else if (sscanf(l, "MemAvailable: %llu", &v) == 1) mem_avail = v;
        else if (sscanf(l, "SwapTotal: %llu", &v) == 1) swap_total = v;
        else if (sscanf(l, "SwapFree: %llu", &v) == 1) swap_free = v;
    }
    if (read_file("/proc/loadavg", buf, sizeof buf))
        sscanf(buf, "%lf %lf %lf", &load1, &load5, &load15);
    if (read_file("/proc/uptime", buf, sizeof buf))
        sscanf(buf, "%lf", &uptime_s);
}

/* Merge one GPU process list into the process table. */
static void gpu_procs(unsigned gi, nvret (*fn)(nvdev, unsigned *, void *))
{
    static unsigned char *buf;
    static unsigned cap;
    if (!fn)
        return;
    unsigned n = cap;
    nvret r;
    while ((r = fn(gpus[gi].dev, &n, buf)) != NV_SUCCESS) {
        if (n <= cap)          /* any error other than "buffer too small" */
            return;
        cap = n + 16;
        buf = realloc(buf, (size_t)cap * nv.proc_size);
        if (!buf) { cap = 0; return; }
        n = cap;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned pid;
        unsigned long long mem;
        memcpy(&pid, buf + i * nv.proc_size, sizeof pid);
        memcpy(&mem, buf + i * nv.proc_size + offsetof(nv_proc1, mem), sizeof mem);
        proc_t *p = find_proc((int)pid);
        if (!p)
            continue;
        if (!(p->gmask & (1u << gi)) && mem != NV_VALUE_NA)
            p->vram += mem;     /* a pid in both lists is counted once per GPU */
        p->gmask |= 1u << gi;
    }
}

static void scan_gpus(void)
{
    if (!nv.ok)
        return;
    for (unsigned i = 0; i < ngpu; i++) {
        gpu_t *g = &gpus[i];
        nv_util u = { 0, 0 };
        nv_mem m = { 0, 0, 0 };
        if (nv.util && nv.util(g->dev, &u) == NV_SUCCESS) g->util = u.gpu;
        if (nv.mem && nv.mem(g->dev, &m) == NV_SUCCESS) { g->mem = m.used; g->memtot = m.total; }
        if (nv.temp) nv.temp(g->dev, NV_TEMPERATURE_GPU, &g->temp);
        if (nv.power) nv.power(g->dev, &g->pw);
        if (nv.plimit) nv.plimit(g->dev, &g->pwl);
        if (nv.fan) nv.fan(g->dev, &g->fan);
        if (nv.clock) nv.clock(g->dev, NV_CLOCK_SM, &g->clk);

        gpu_procs(i, nv.compute);
        gpu_procs(i, nv.graphics);

        /* Per-process SM load: samples newer than the last ones we read. */
        if (nv.samples) {
            static nv_sample *sb;
            static unsigned scap;
            unsigned n = 0;
            nv.samples(g->dev, NULL, &n, g->last_ts);
            if (n > scap) {
                scap = n + 32;
                sb = realloc(sb, (size_t)scap * sizeof *sb);
                if (!sb) { scap = 0; continue; }
            }
            n = scap;
            if (n && nv.samples(g->dev, sb, &n, g->last_ts) == NV_SUCCESS) {
                for (unsigned k = 0; k < n; k++) {
                    if (sb[k].ts > g->last_ts) g->last_ts = sb[k].ts;
                    proc_t *p = find_proc((int)sb[k].pid);
                    if (p && sb[k].sm > p->gutil) p->gutil = sb[k].sm;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

enum { C_OK = 1, C_MID, C_HIGH, C_LABEL, C_GPU, C_BAR, C_SORT, C_SEL };
enum { S_CPU, S_MEM, S_VRAM, S_GPU };

static int W, H;
static int sort_mode = S_CPU, gpu_only, sel, top, sel_pid = -1, follow;
static double rate = 1.0;
static char filt[64], msg[160];
static proc_t **view;
static int nview, cap_view;
static volatile sig_atomic_t quit;

static void put(int y, int x, attr_t a, const char *fmt, ...)
{
    if (y < 0 || y >= H || x < 0 || x >= W)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int room = W - x - (y == H - 1 ? 1 : 0);
    if (room <= 0)
        return;
    attron(a);
    mvaddnstr(y, x, buf, room);
    attroff(a);
}

static void human(char *out, size_t n, unsigned long long b)
{
    const char *u = "BKMGTP";
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 5) { v /= 1024; i++; }
    if (i <= 1) snprintf(out, n, "%.0f%c", v, u[i]);
    else snprintf(out, n, "%.1f%c", v, u[i]);
}

static int level(double f)
{
    return f < 0.5 ? C_OK : f < 0.8 ? C_MID : C_HIGH;
}

/* htop-style meter:  label[|||||||        text] */
static void meter(int y, int x, int w, const char *label, double frac, const char *text, int pair)
{
    int ll = (int)strlen(label);
    int inner = w - ll - 2;
    if (inner < 4)
        return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fill = (int)(frac * inner + 0.5);
    char bars[512];
    if (inner > (int)sizeof bars - 1) inner = (int)sizeof bars - 1;
    if (fill > inner) fill = inner;
    memset(bars, '|', (size_t)fill);
    bars[fill] = 0;
    int tl = (int)strlen(text);
    if (tl > inner) tl = inner;

    put(y, x, COLOR_PAIR(C_LABEL) | A_BOLD, "%s", label);
    put(y, x + ll, A_BOLD, "[");
    put(y, x + ll + 1, COLOR_PAIR(pair), "%s", bars);
    put(y, x + ll + 1 + inner - tl, fill > inner - tl ? COLOR_PAIR(pair) : A_DIM, "%.*s", tl, text);
    put(y, x + ll + 1 + inner, A_BOLD, "]");
}

static int cmp_view(const void *a, const void *b)
{
    const proc_t *x = *(proc_t *const *)a, *y = *(proc_t *const *)b;
    double kx, ky;
    switch (sort_mode) {
    case S_MEM:  kx = (double)x->rss;  ky = (double)y->rss;  break;
    case S_VRAM: kx = (double)x->vram; ky = (double)y->vram; break;
    case S_GPU:  kx = x->gutil;        ky = y->gutil;        break;
    default:     kx = x->cpu;          ky = y->cpu;          break;
    }
    if (kx != ky) return kx < ky ? 1 : -1;
    if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static void build_view(void)
{
    if (nprocs > cap_view) {
        cap_view = cap_procs;
        view = realloc(view, (size_t)cap_view * sizeof *view);
        if (!view) { endwin(); fprintf(stderr, "out of memory\n"); exit(1); }
    }
    nview = 0;
    for (int i = 0; i < nprocs; i++) {
        proc_t *p = &procs[i];
        if (gpu_only && !p->gmask)
            continue;
        if (filt[0] && !strcasestr(p->cmd, filt))
            continue;
        view[nview++] = p;
    }
    qsort(view, (size_t)nview, sizeof *view, cmp_view);

    /* keep the cursor on the same process when the order changes */
    if (follow && sel_pid >= 0)
        for (int i = 0; i < nview; i++)
            if (view[i]->pid == sel_pid) { sel = i; break; }
    if (sel >= nview) sel = nview - 1;
    if (sel < 0) sel = 0;
    sel_pid = nview ? view[sel]->pid : -1;
}

static void draw(void)
{
    char a[32], b[32], t[96];
    getmaxyx(stdscr, H, W);
    erase();
    int y = 0;

    /* CPU cores in columns, numbered down each column like htop */
    int ncol = W >= 160 ? 4 : W >= 80 ? 2 : 1;
    int colw = W / ncol;
    int rows = ncpu ? (ncpu + ncol - 1) / ncol : 0;
    for (int i = 0; i < ncpu; i++) {
        char lab[8];
        snprintf(lab, sizeof lab, "%3d", i);
        snprintf(t, sizeof t, "%.0f%%", cpu_pct[i]);
        meter(y + i % rows, (i / rows) * colw, colw - 1, lab, cpu_pct[i] / 100, t, level(cpu_pct[i] / 100));
    }
    y += rows;

    int half = W / 2;
    unsigned long long used = mem_total - mem_avail;
    human(a, sizeof a, used * 1024);
    human(b, sizeof b, mem_total * 1024);
    snprintf(t, sizeof t, "%s/%s", a, b);
    meter(y, 0, half - 1, "Mem", mem_total ? (double)used / (double)mem_total : 0, t, C_OK);
    long up = (long)uptime_s;
    put(y, half, A_BOLD, "CPU %3.0f%%  Load %.2f %.2f %.2f  Up %ldd %02ld:%02ld  Tasks %d",
        cpu_all, load1, load5, load15, up / 86400, up % 86400 / 3600, up % 3600 / 60, nprocs);
    y++;
    unsigned long long sused = swap_total - swap_free;
    human(a, sizeof a, sused * 1024);
    human(b, sizeof b, swap_total * 1024);
    snprintf(t, sizeof t, "%s/%s", a, b);
    meter(y, 0, half - 1, "Swp", swap_total ? (double)sused / (double)swap_total : 0, t, C_HIGH);
    y++;

    /* GPUs */
    if (!nv.ok) {
        put(y++, 0, COLOR_PAIR(C_HIGH), "No NVIDIA GPU: libnvidia-ml.so.1 not found or NVML did not start");
    }
    for (unsigned i = 0; i < ngpu; i++) {
        gpu_t *g = &gpus[i];
        put(y++, 0, COLOR_PAIR(C_GPU) | A_BOLD, "GPU%u %s  %u\xC2\xB0" "C  %u/%uW  fan %u%%  %uMHz",
            i, g->name, g->temp, g->pw / 1000, g->pwl / 1000, g->fan, g->clk);
        snprintf(t, sizeof t, "%u%%", g->util);
        meter(y, 0, half - 1, "Util", g->util / 100.0, t, level(g->util / 100.0));
        double mf = g->memtot ? (double)g->mem / (double)g->memtot : 0;
        snprintf(t, sizeof t, "%.1f/%.1fG", g->mem / 1073741824.0, g->memtot / 1073741824.0);
        meter(y, half, W - half - 1, "VRAM", mf, t, level(mf));
        y++;
    }

    /* process table */
    static const char *hdr = "    PID USER      S  CPU%  MEM%     RES  GPU  GPU%    VRAM  COMMAND";
    static const char *keys[] = { [S_CPU] = "CPU%", [S_MEM] = "RES", [S_VRAM] = "VRAM", [S_GPU] = "GPU%" };
    attron(COLOR_PAIR(C_BAR));
    mvhline(y, 0, ' ', W);
    attroff(COLOR_PAIR(C_BAR));
    put(y, 0, COLOR_PAIR(C_BAR), "%s", hdr);
    const char *k = strstr(hdr, keys[sort_mode]);
    if (sort_mode == S_GPU) k = strstr(hdr, "GPU%");
    if (k) put(y, (int)(k - hdr), COLOR_PAIR(C_SORT), "%s", keys[sort_mode]);
    y++;

    build_view();
    int avail = H - y - 1;
    if (avail < 1) avail = 1;
    if (sel < top) top = sel;
    if (sel >= top + avail) top = sel - avail + 1;
    if (top > nview - avail) top = nview - avail;
    if (top < 0) top = 0;

    for (int r = 0; r < avail && top + r < nview; r++) {
        proc_t *p = view[top + r];
        char res[16], vram[16] = "", gl[16] = "", gu[8] = "";
        human(res, sizeof res, p->rss);
        if (p->gmask) {
            int o = 0;
            for (unsigned i = 0; i < ngpu && o < 12; i++)
                if (p->gmask & (1u << i))
                    o += snprintf(gl + o, sizeof gl - (size_t)o, o ? ",%u" : "%u", i);
            if (p->vram) human(vram, sizeof vram, p->vram);
            snprintf(gu, sizeof gu, "%u", p->gutil);
        }
        attr_t at = 0;
        if (top + r == sel) {
            at = COLOR_PAIR(C_SEL);
            attron(at);
            mvhline(y + r, 0, ' ', W);
            attroff(at);
        } else if (p->gmask) {
            at = COLOR_PAIR(C_GPU);
        }
        put(y + r, 0, at, "%7d %-9.9s %c %5.1f %5.1f %7s %4s %5s %7s  %s",
            p->pid, p->user, p->st, p->cpu, p->mem, res, gl, gu, vram, p->cmd);
    }

    /* footer */
    attron(COLOR_PAIR(C_BAR));
    mvhline(H - 1, 0, ' ', W - 1);
    attroff(COLOR_PAIR(C_BAR));
    if (msg[0])
        put(H - 1, 0, COLOR_PAIR(C_BAR), " %s", msg);
    else
        put(H - 1, 0, COLOR_PAIR(C_BAR),
            " c/m/g/u sort  v gpu-only[%s]  / filter[%s]  k kill  K kill-9  +/- %.1fs  q quit",
            gpu_only ? "on" : "off", filt, rate);
    refresh();
}

static void prompt(const char *q, char *out, int n)
{
    attron(COLOR_PAIR(C_BAR));
    mvhline(H - 1, 0, ' ', W - 1);
    mvaddnstr(H - 1, 0, q, W - 1);
    attroff(COLOR_PAIR(C_BAR));
    move(H - 1, (int)strlen(q));
    echo();
    curs_set(1);
    timeout(-1);
    out[0] = 0;
    getnstr(out, n - 1);
    noecho();
    curs_set(0);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_signal(int s)
{
    (void)s;
    quit = 1;
}

static void sample(double dt)
{
    scan_cpu();
    scan_mem();
    scan_procs(dt);
    scan_gpus();
}

static int batch, batch_rows = 20;

/* One snapshot as plain text: sample, wait one interval, sample, print. */
static int run_batch(void)
{
    char a[32], b[32], res[16], vram[16];
    sample(0);
    double t0 = now_s();
    struct timespec ts = { (time_t)rate, (long)((rate - (long)rate) * 1e9) };
    nanosleep(&ts, NULL);
    sample(now_s() - t0);

    printf("CPU %.0f%%  Load %.2f %.2f %.2f  Tasks %d\n", cpu_all, load1, load5, load15, nprocs);
    human(a, sizeof a, (mem_total - mem_avail) * 1024);
    human(b, sizeof b, mem_total * 1024);
    printf("Mem %s/%s", a, b);
    human(a, sizeof a, (swap_total - swap_free) * 1024);
    human(b, sizeof b, swap_total * 1024);
    printf("  Swp %s/%s\n", a, b);
    for (unsigned i = 0; i < ngpu; i++) {
        gpu_t *g = &gpus[i];
        printf("GPU%u %s  util %u%%  vram %.1f/%.1fG  %u\xC2\xB0" "C  %u/%uW  fan %u%%  %uMHz\n",
               i, g->name, g->util, g->mem / 1073741824.0, g->memtot / 1073741824.0,
               g->temp, g->pw / 1000, g->pwl / 1000, g->fan, g->clk);
    }
    if (!nv.ok)
        printf("No NVIDIA GPU: libnvidia-ml.so.1 not found or NVML did not start\n");

    build_view();
    printf("%7s %-9s %s %5s %5s %7s %4s %5s %7s  %s\n",
           "PID", "USER", "S", "CPU%", "MEM%", "RES", "GPU", "GPU%", "VRAM", "COMMAND");
    for (int r = 0; r < nview && (batch_rows <= 0 || r < batch_rows); r++) {
        proc_t *p = view[r];
        char gl[16] = "", gu[8] = "";
        human(res, sizeof res, p->rss);
        vram[0] = 0;
        if (p->gmask) {
            int o = 0;
            for (unsigned i = 0; i < ngpu && o < 12; i++)
                if (p->gmask & (1u << i))
                    o += snprintf(gl + o, sizeof gl - (size_t)o, o ? ",%u" : "%u", i);
            if (p->vram) human(vram, sizeof vram, p->vram);
            snprintf(gu, sizeof gu, "%u", p->gutil);
        }
        printf("%7d %-9.9s %c %5.1f %5.1f %7s %4s %5s %7s  %.*s\n",
               p->pid, p->user, p->st, p->cpu, p->mem, res, gl, gu, vram, 200, p->cmd);
    }
    nv_close();
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
        "nv-sys-top " VERSION " - htop-style system monitor with NVIDIA GPUs\n\n"
        "usage: nv-sys-top [-d SECONDS] [-g] [-s cpu|mem|vram|gpu] [-b [-n ROWS]]\n\n"
        "  -d SECONDS   refresh interval (default 1.0, 0.5 .. 10)\n"
        "  -g           GPU processes only\n"
        "  -s KEY       sort by cpu, mem, vram or gpu\n"
        "  -b           batch: print one snapshot as text and exit\n"
        "  -n ROWS      process rows in batch mode (default 20, 0 = all)\n"
        "  -h           this help\n"
        "  -V           version\n\n"
        "keys: c/m/g/u sort by CPU / RAM / VRAM / GPU load, v GPU processes only,\n"
        "      / filter, arrows PgUp PgDn Home End move, k kill, K kill -9,\n"
        "      + / - refresh interval, q quit\n");
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "bn:d:gs:hV")) != -1) {
        switch (opt) {
        case 'b': batch = 1; break;
        case 'n': batch_rows = atoi(optarg); break;
        case 'd': rate = atof(optarg); break;
        case 'g': gpu_only = 1; break;
        case 's':
            if (!strcmp(optarg, "mem")) sort_mode = S_MEM;
            else if (!strcmp(optarg, "vram")) sort_mode = S_VRAM;
            else if (!strcmp(optarg, "gpu")) sort_mode = S_GPU;
            else if (!strcmp(optarg, "cpu")) sort_mode = S_CPU;
            else { usage(stderr); return 2; }
            break;
        case 'h': usage(stdout); return 0;
        case 'V': puts("nv-sys-top " VERSION); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (rate < 0.5) rate = 0.5;
    if (rate > 10) rate = 10;

    setlocale(LC_ALL, "");
    hz = sysconf(_SC_CLK_TCK);
    pagesize = sysconf(_SC_PAGESIZE);
    nv_open();

    if (batch)
        return run_batch();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_OK, COLOR_GREEN, -1);
        init_pair(C_MID, COLOR_YELLOW, -1);
        init_pair(C_HIGH, COLOR_RED, -1);
        init_pair(C_LABEL, COLOR_CYAN, -1);
        init_pair(C_GPU, COLOR_MAGENTA, -1);
        init_pair(C_BAR, COLOR_BLACK, COLOR_GREEN);
        init_pair(C_SORT, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_SEL, COLOR_BLACK, COLOR_CYAN);
    }

    /* short first interval so the first screen already has CPU numbers */
    sample(0);
    double last = now_s();
    struct timespec first = { 0, 200000000L };
    nanosleep(&first, NULL);
    sample(now_s() - last);
    last = now_s();
    int dirty = 1;
    while (!quit) {
        double t = now_s();
        if (t - last >= rate) {
            sample(t - last);
            last = t;
            dirty = 1;
        }
        if (dirty) {
            draw();
            dirty = 0;
        }
        timeout(100);
        int ch = getch();
        if (ch == ERR)
            continue;
        dirty = 1;
        msg[0] = 0;
        int page = H / 2 > 1 ? H / 2 : 1;
        switch (ch) {
        case 'q': case 'Q': case 27: quit = 1; break;
        case 'c': sort_mode = S_CPU; break;
        case 'm': sort_mode = S_MEM; break;
        case 'g': sort_mode = S_VRAM; break;
        case 'u': sort_mode = S_GPU; break;
        case 'v': gpu_only = !gpu_only; break;
        case '+': case '=': rate = rate + 0.5 > 10 ? 10 : rate + 0.5; break;
        case '-': rate = rate - 0.5 < 0.5 ? 0.5 : rate - 0.5; break;
        case KEY_UP:    sel--; sel_pid = -1; follow = 1; break;
        case KEY_DOWN:  sel++; sel_pid = -1; follow = 1; break;
        case KEY_PPAGE: sel -= page; sel_pid = -1; follow = 1; break;
        case KEY_NPAGE: sel += page; sel_pid = -1; follow = 1; break;
        case KEY_HOME:  sel = 0; sel_pid = -1; follow = 0; break;
        case KEY_END:   sel = nview - 1; sel_pid = -1; follow = 1; break;
        case '/': {
            char in[64];
            prompt(" filter: ", in, sizeof in);
            snprintf(filt, sizeof filt, "%s", in);
            sel = 0;
            sel_pid = -1;
            follow = 0;
            break;
        }
        case 'k': case 'K': {
            if (!nview) break;
            proc_t *p = view[sel];
            int pid = p->pid;
            int sig = ch == 'K' ? SIGKILL : SIGTERM;
            char q[160], in[8];
            snprintf(q, sizeof q, " %s %d %s? [y/N] ", sig == SIGKILL ? "kill -9" : "kill", pid, p->comm);
            prompt(q, in, sizeof in);
            if (in[0] == 'y' || in[0] == 'Y') {
                if (kill(pid, sig) == 0)
                    snprintf(msg, sizeof msg, "sent %s to %d", sig == SIGKILL ? "SIGKILL" : "SIGTERM", pid);
                else
                    snprintf(msg, sizeof msg, "kill %d failed: %s", pid, strerror(errno));
            }
            last = 0;   /* refresh right away */
            break;
        }
        default: break;
        }
    }

    endwin();
    nv_close();
    return 0;
}
