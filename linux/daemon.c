/*
 * ctxschedd — Context-Aware CPU Scheduling on a real Linux kernel
 * ----------------------------------------------------------------
 * The real-system version of scheduler.c's pipeline:
 *
 *   /proc/<pid> counters -> Signal Collector (deltas + EWMA)
 *     -> Heuristic Classifier (+ confidence, fallback, hysteresis)
 *     -> Weight Mapper -> cgroup v2 cpu.weight (kernel EEVDF/CFS does the rest)
 *
 * Only processes inside /sys/fs/cgroup/ctxsched/<class>/ are managed, so the
 * daemon never touches the rest of the system. New tasks are started in
 * ctxsched/pending (default weight) and the daemon moves them between
 * ctxsched/{batch,interactive,latency} as their behaviour is observed.
 *
 * On kernel 6.6+ (EEVDF) there is no settable time quantum, so the class is
 * mapped to a CPU weight instead of the simulator's 2/6/20 ms quanta.
 *
 * Build:  make
 * Run:    sudo ./ctxschedd [--observe] [--no-guard] [--interval MS] [--duration S] [--log FILE]
 *   --observe   classify and log, but never move tasks (baseline runs)
 *   --no-guard  drop the "must actually sleep" check on the latency rule
 *               (ablation: lets a busy task game its way into the latency class)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

/* ----------------------------- config ----------------------------- */

#define CG_ROOT     "/sys/fs/cgroup/ctxsched"
#define MAX_PROCS   1024

typedef enum { C_DEFAULT = 0, C_BATCH, C_INTERACTIVE, C_LATENCY, N_CLASSES } Class;
static const char *CLASS_NAME[N_CLASSES]  = { "default", "batch", "interactive", "latency" };
static const char *CLASS_DIR[N_CLASSES]   = { "pending", "batch", "interactive", "latency" };
/* Weight mapper: replaces the simulator's quantum mapper (20/6/2 ms, 10 ms default). */
static const int   CLASS_WEIGHT[N_CLASSES] = { 100, 50, 200, 400 };

/*
 * Classifier thresholds. Each one is anchored to an external reference (see
 * the "Core classifier" section of README.md for the full argument):
 *
 * SLEEP_MIN      FreeBSD ULE calls a thread interactive when its score
 *                50 * run/sleep is below 30, i.e. run/sleep < 0.6, i.e.
 *                sleep/(sleep+run) > 0.625. ULE measures voluntary sleep and
 *                excludes run-queue wait, exactly like sleep_frac below.
 * LAT_MIN_WAKE   period <= 25 ms: covers PipeWire's default audio quantum
 *                (1024 frames / 48 kHz = 21.3 ms) and 50/60 Hz frame loops,
 *                and is well inside the 0.1 s "instantaneous" limit.
 * lat_max_burst  the kernel's own EEVDF base slice (0.75 ms * (1 + log2 CPUs),
 *                3 ms here): work per wake-up fits in one slice, so the task
 *                never needs to be preempted to finish its burst.
 * INT_MIN_WAKE   at least one event per second, the 1 s flow-of-thought limit.
 * I/O-bound      the textbook definition: more time waiting for I/O than
 *                computing. Block-I/O wait comes from kernel delay accounting;
 *                without it, fall back to storage throughput.
 * CONF_MIN, HYST_N, EWMA_ALPHA are fitted to measured signal noise.
 */
#define SLEEP_MIN        0.625 /* ULE interactivity threshold, as a sleep fraction */
#define LAT_MIN_WAKE    40.0   /* wake-ups per second (period <= 25 ms) */
#define INT_MIN_WAKE     1.0   /* wake-ups per second            */
#define IO_MIN_WAIT      0.01  /* ignore block-I/O wait below 1% of the time */
#define IO_MIN_MBPS      1.0   /* fallback I/O rule when delay accounting is off */
#define CONF_MIN         0.25  /* below this -> fall back to default weight */
#define HYST_N           3     /* consecutive agreeing samples before switching */
#define EWMA_ALPHA       0.4

static double lat_max_burst = 3.0;   /* ms; replaced by the kernel's base slice at startup */
static int    have_delayacct;        /* per-task block-I/O delay available */
static double clk_tck = 100.0;       /* units of /proc/<pid>/stat field 42 */

/* ------------------------ pipeline stage 1: signal collector ------------------------ */

typedef struct {
    unsigned long long cpu_ns;    /* time on CPU            (schedstat field 1) */
    unsigned long long wait_ns;   /* time runnable, waiting (schedstat field 2) */
    unsigned long long vol;       /* voluntary context switches = times it blocked */
    unsigned long long io_bytes;  /* read_bytes + write_bytes from /proc/<pid>/io */
    unsigned long long blkio;     /* block-I/O delay, clock ticks (stat field 42) */
} Counters;

typedef struct {
    double cpu_share;     /* fraction of wall time on CPU          */
    double blocked;       /* fraction of wall time sleeping / I/O  */
    double wake_s;        /* wake-ups per second                   */
    double burst_ms;      /* CPU time per wake-up                  */
    double io_mbps;       /* storage I/O, MB per second            */
    double io_wait;       /* fraction of wall time waiting for block I/O */
} Signals;

typedef struct {
    int      used, seen;
    int      pid;
    char     comm[32];
    int      truth;               /* ground truth from workload name, -1 = unknown */
    Counters last;
    double   last_t;
    int      have_last, have_ewma;
    Signals  ewma;
    Class    current, candidate;
    int      streak;
} Proc;

static Proc procs[MAX_PROCS];

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Sum the counters over every thread of the process. */
static int read_counters(int pid, Counters *c) {
    char path[300];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) return -1;

    memset(c, 0, sizeof(*c));
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        snprintf(path, sizeof(path), "/proc/%d/task/%s/schedstat", pid, e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            unsigned long long run, wait, slices;
            if (fscanf(f, "%llu %llu %llu", &run, &wait, &slices) == 3) {
                c->cpu_ns += run; c->wait_ns += wait; n++;
            }
            fclose(f);
        }

        snprintf(path, sizeof(path), "/proc/%d/task/%s/status", pid, e->d_name);
        f = fopen(path, "r");
        if (f) {
            char line[256]; unsigned long long v;
            while (fgets(line, sizeof(line), f))
                if (sscanf(line, "voluntary_ctxt_switches: %llu", &v) == 1) c->vol += v;
            fclose(f);
        }

        /* field 42 (delayacct_blkio_ticks) is per thread, so sum it here */
        snprintf(path, sizeof(path), "/proc/%d/task/%s/stat", pid, e->d_name);
        f = fopen(path, "r");
        if (f) {
            char buf[1024];
            size_t len = fread(buf, 1, sizeof(buf) - 1, f);
            buf[len] = 0;
            char *p = strrchr(buf, ')');       /* comm may contain spaces */
            for (int field = 2; p && field < 42; field++) p = strchr(p + 1, ' ');
            if (p) c->blkio += strtoull(p + 1, NULL, 10);
            fclose(f);
        }
    }
    closedir(d);

    /* storage I/O is already summed over threads */
    snprintf(path, sizeof(path), "/proc/%d/io", pid);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256]; unsigned long long v;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "read_bytes: %llu", &v) == 1 || sscanf(line, "write_bytes: %llu", &v) == 1)
                c->io_bytes += v;
        fclose(f);
    }
    return n > 0 ? 0 : -1;
}

static unsigned long long delta(unsigned long long now, unsigned long long before) {
    return now >= before ? now - before : 0;   /* a thread exited: counters shrank */
}

static Signals signals_from_deltas(const Counters *a, const Counters *b, double dt) {
    double cpu_ms  = delta(b->cpu_ns, a->cpu_ns) / 1e6;
    double wait_ms = delta(b->wait_ns, a->wait_ns) / 1e6;
    double vol     = (double)delta(b->vol, a->vol);
    double dt_ms   = dt * 1000.0;

    Signals s;
    s.cpu_share = cpu_ms / dt_ms;
    s.blocked   = 1.0 - s.cpu_share - wait_ms / dt_ms;
    if (s.blocked < 0) s.blocked = 0;
    if (s.blocked > 1) s.blocked = 1;
    s.wake_s    = vol / dt;
    s.burst_ms  = vol > 0 ? cpu_ms / vol : cpu_ms;   /* never blocked: one long burst */
    s.io_mbps   = delta(b->io_bytes, a->io_bytes) / 1e6 / dt;
    s.io_wait   = delta(b->blkio, a->blkio) / clk_tck / dt;
    if (s.io_wait > 1) s.io_wait = 1;
    return s;
}

static void ewma_update(Proc *p, const Signals *s) {
    if (!p->have_ewma) { p->ewma = *s; p->have_ewma = 1; return; }
    p->ewma.cpu_share = EWMA_ALPHA * s->cpu_share + (1 - EWMA_ALPHA) * p->ewma.cpu_share;
    p->ewma.blocked   = EWMA_ALPHA * s->blocked   + (1 - EWMA_ALPHA) * p->ewma.blocked;
    p->ewma.wake_s    = EWMA_ALPHA * s->wake_s    + (1 - EWMA_ALPHA) * p->ewma.wake_s;
    p->ewma.burst_ms  = EWMA_ALPHA * s->burst_ms  + (1 - EWMA_ALPHA) * p->ewma.burst_ms;
    p->ewma.io_mbps   = EWMA_ALPHA * s->io_mbps   + (1 - EWMA_ALPHA) * p->ewma.io_mbps;
    p->ewma.io_wait   = EWMA_ALPHA * s->io_wait   + (1 - EWMA_ALPHA) * p->ewma.io_wait;
}

/* ------------------------ pipeline stage 2: heuristic classifier ------------------------ */

static double lg2(double x) { return log2(x > 1e-9 ? x : 1e-9); }

static int guard = 1;   /* --no-guard clears it */

/*
 * Of the time the task was not waiting in the run queue, how much did it
 * spend asleep? Unlike `blocked` (a share of wall time) this does not shrink
 * when the CPU is contended: a starved latency task waits a lot, but still
 * sleeps far more than it computes. Runnable-but-never-run counts as CPU-bound.
 */
static double sleep_frac(const Signals *s) {
    double busy = s->blocked + s->cpu_share;
    return busy > 1e-6 ? s->blocked / busy : 0.0;
}

/*
 * Same rule order as scheduler.c's classify(), with an I/O-bound rule in front
 * (a task streaming to disk wakes often with tiny bursts and would otherwise
 * look latency-sensitive). I/O-bound tasks share the interactive class.
 *
 * Each rule's margin says how far the signals sit past its thresholds (1.0 =
 * a factor of 2, or 0.2 of sleep fraction); positive means the rule fires.
 * Confidence is the smallest margin that decided the outcome, capped at 1.
 */
static Class classify(const Signals *s, double *conf) {
    double burst = s->burst_ms > 1e-6 ? s->burst_ms : 1e-6;
    double sleep = sleep_frac(s);
    double m_io  = have_delayacct
        ? fmin(lg2(s->io_wait / (s->cpu_share > 1e-6 ? s->cpu_share : 1e-6)), lg2(s->io_wait / IO_MIN_WAIT))
        : fmin((sleep - SLEEP_MIN) / 0.2, lg2(s->io_mbps / IO_MIN_MBPS));
    double m_lat = fmin(lg2(s->wake_s / LAT_MIN_WAKE), lg2(lat_max_burst / burst));
    if (guard) m_lat = fmin(m_lat, (sleep - SLEEP_MIN) / 0.2);
    double m_int = fmin((sleep - SLEEP_MIN) / 0.2, lg2(s->wake_s / INT_MIN_WAKE));

    Class c; double m;
    if (m_io > 0)       { c = C_INTERACTIVE; m = m_io; }
    else if (m_lat > 0) { c = C_LATENCY;     m = fmin(m_lat, -m_io); }
    else if (m_int > 0) { c = C_INTERACTIVE; m = fmin(fmin(m_int, -m_lat), -m_io); }
    else                { c = C_BATCH;       m = fmin(fmin(-m_lat, -m_int), -m_io); }   /* conservative default */
    *conf = m < 1.0 ? m : 1.0;
    return c;
}

/* ------------------------ pipeline stage 3: weight mapper / cgroup writer ------------------------ */

static int write_file(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = fputs(val, f) >= 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int move_to_class(int pid, Class c) {
    char path[256], val[32];
    snprintf(path, sizeof(path), CG_ROOT "/%s/cgroup.procs", CLASS_DIR[c]);
    snprintf(val, sizeof(val), "%d\n", pid);
    if (write_file(path, val) != 0 && errno != ESRCH) {
        fprintf(stderr, "ctxschedd: move pid %d -> %s: %s\n", pid, CLASS_DIR[c], strerror(errno));
        return -1;
    }
    return 0;
}

static int apply_weights(void) {
    for (int c = 0; c < N_CLASSES; c++) {
        char path[256], val[32];
        snprintf(path, sizeof(path), CG_ROOT "/%s/cpu.weight", CLASS_DIR[c]);
        snprintf(val, sizeof(val), "%d\n", CLASS_WEIGHT[c]);
        if (write_file(path, val) != 0) {
            fprintf(stderr, "ctxschedd: cannot write %s: %s (did you run setup.sh as root?)\n",
                    path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/*
 * Read the kernel's EEVDF base slice (debugfs, root only) or derive it the way
 * the kernel does: 0.75 ms * (1 + ilog2(min(ncpus, 8))). Turn on per-task
 * delay accounting (kernel.task_delayacct, Linux 5.14+) so block-I/O wait can
 * be measured; tasks started before this have no delay data.
 */
static void init_kernel_params(void) {
    long t = sysconf(_SC_CLK_TCK);
    if (t > 0) clk_tck = (double)t;

    FILE *f = fopen("/sys/kernel/debug/sched/base_slice_ns", "r");
    unsigned long long ns = 0;
    if (f) {
        if (fscanf(f, "%llu", &ns) != 1) ns = 0;
        fclose(f);
    }
    if (ns) {
        lat_max_burst = ns / 1e6;
    } else {
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus > 8) cpus = 8;
        int lg = 0;
        while (cpus > 1) { cpus >>= 1; lg++; }
        lat_max_burst = 0.75 * (1 + lg);
    }

    write_file("/proc/sys/kernel/task_delayacct", "1\n");
    f = fopen("/proc/sys/kernel/task_delayacct", "r");
    int on = 0;
    if (f) {
        if (fscanf(f, "%d", &on) != 1) on = 0;
        fclose(f);
    }
    have_delayacct = on == 1;
}

/* ------------------------ process table ------------------------ */

/*
 * Ground truth for scoring only. Our workloads are named <tag>_<class>
 * (wl_, ph_, adv_); a phase task renames itself when it switches. Benchmark
 * tools are labelled by what they are built to be.
 */
static int truth_from_comm(const char *comm) {
    static const struct { const char *prefix; int cls; } tools[] = {
        { "stress-ng",  C_BATCH },
        { "sysbench",   C_BATCH },
        { "fio",        C_INTERACTIVE },   /* I/O-bound */
        { "cyclictest", C_LATENCY },
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
        if (strncmp(comm, tools[i].prefix, strlen(tools[i].prefix)) == 0) return tools[i].cls;

    if (strncmp(comm, "wl_", 3) && strncmp(comm, "ph_", 3) && strncmp(comm, "adv_", 4)) return -1;
    const char *cls = strchr(comm, '_') + 1;
    for (int c = C_BATCH; c < N_CLASSES; c++)
        if (strcmp(cls, CLASS_NAME[c]) == 0) return c;
    return -1;
}

static void read_comm(Proc *p) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", p->pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(p->comm, sizeof(p->comm), f)) p->comm[strcspn(p->comm, "\n")] = 0;
    fclose(f);
    p->truth = truth_from_comm(p->comm);
}

static Proc *lookup(int pid, Class where) {
    Proc *free_slot = NULL;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
        if (!procs[i].used && !free_slot) free_slot = &procs[i];
    }
    if (!free_slot) return NULL;

    Proc *p = free_slot;
    memset(p, 0, sizeof(*p));
    p->used = 1; p->pid = pid;
    p->current = p->candidate = where;
    return p;
}

static void scan_cgroups(void) {
    for (int i = 0; i < MAX_PROCS; i++) procs[i].seen = 0;

    for (int c = 0; c < N_CLASSES; c++) {
        char path[256];
        snprintf(path, sizeof(path), CG_ROOT "/%s/cgroup.procs", CLASS_DIR[c]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int pid;
        while (fscanf(f, "%d", &pid) == 1) {
            Proc *p = lookup(pid, (Class)c);
            if (p) p->seen = 1;
        }
        fclose(f);
    }

    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].used && !procs[i].seen) procs[i].used = 0;   /* exited or moved out */
}

/* ------------------------ main loop ------------------------ */

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--observe] [--no-guard] [--interval MS] [--duration S] [--log FILE]\n"
        "  --observe     classify and log only, never move tasks (baseline)\n"
        "  --no-guard    latency rule ignores how much the task sleeps (ablation)\n"
        "  --interval    sampling period in ms (default 100)\n"
        "  --duration    stop after S seconds (default: run until Ctrl-C)\n"
        "  --log         per-sample CSV log (default: none)\n", argv0);
}

int main(int argc, char **argv) {
    int observe = 0, interval_ms = 100;
    double duration = 0;
    const char *log_path = NULL;

    static const struct option opts[] = {
        { "observe",  no_argument,       0, 'o' },
        { "no-guard", no_argument,       0, 'g' },
        { "interval", required_argument, 0, 'i' },
        { "duration", required_argument, 0, 'd' },
        { "log",      required_argument, 0, 'l' },
        { "help",     no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "ogi:d:l:h", opts, NULL)) != -1) {
        switch (ch) {
        case 'o': observe = 1; break;
        case 'g': guard = 0; break;
        case 'i': interval_ms = atoi(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'l': log_path = optarg; break;
        default:  usage(argv[0]); return ch == 'h' ? 0 : 2;
        }
    }
    if (interval_ms < 10) interval_ms = 10;

    struct stat st;
    if (stat(CG_ROOT "/pending", &st) != 0) {
        fprintf(stderr, "ctxschedd: %s/pending not found, run: sudo ./setup.sh\n", CG_ROOT);
        return 1;
    }
    if (apply_weights() != 0) return 1;
    init_kernel_params();

    FILE *log = NULL;
    if (log_path) {
        log = fopen(log_path, "w");
        if (!log) { perror(log_path); return 1; }
        fprintf(log, "t_s,pid,comm,truth,cpu_share,blocked,sleep_frac,wake_s,burst_ms,io_mbps,io_wait,raw_class,conf,decision,moved\n");
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "ctxschedd: %s mode%s, interval %d ms, weights default=%d batch=%d interactive=%d latency=%d\n",
            observe ? "observe" : "adaptive", guard ? "" : " (no guard)", interval_ms,
            CLASS_WEIGHT[C_DEFAULT], CLASS_WEIGHT[C_BATCH],
            CLASS_WEIGHT[C_INTERACTIVE], CLASS_WEIGHT[C_LATENCY]);
    fprintf(stderr, "ctxschedd: latency burst limit %.2f ms (kernel base slice), I/O rule uses %s\n",
            lat_max_burst, have_delayacct ? "block-I/O delay accounting" : "storage throughput (delay accounting off)");

    double t0 = now_s();
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (running) {
        double t = now_s();
        if (duration > 0 && t - t0 >= duration) break;

        scan_cgroups();

        for (int i = 0; i < MAX_PROCS; i++) {
            Proc *p = &procs[i];
            if (!p->used) continue;

            read_comm(p);   /* every tick: a phase-change task renames itself */
            Counters cur;
            if (read_counters(p->pid, &cur) != 0) continue;
            if (!p->have_last) { p->last = cur; p->last_t = t; p->have_last = 1; continue; }

            Signals s = signals_from_deltas(&p->last, &cur, t - p->last_t);
            p->last = cur; p->last_t = t;
            ewma_update(p, &s);

            double conf;
            Class raw = classify(&p->ewma, &conf);
            Class want = conf < CONF_MIN ? C_DEFAULT : raw;

            /* hysteresis: only switch after HYST_N consecutive agreeing samples */
            if (want == p->candidate) p->streak++;
            else { p->candidate = want; p->streak = 1; }

            int moved = 0;
            if (p->candidate != p->current && p->streak >= HYST_N) {
                p->current = p->candidate;
                if (!observe && move_to_class(p->pid, p->current) == 0) moved = 1;
                fprintf(stderr, "t=%6.2fs pid %-6d %-15s -> %-11s (conf %.2f)%s\n",
                        t - t0, p->pid, p->comm, CLASS_NAME[p->current], conf,
                        observe ? " [observe]" : "");
            }

            if (log)
                fprintf(log, "%.3f,%d,%s,%s,%.4f,%.4f,%.4f,%.2f,%.3f,%.3f,%.4f,%s,%.3f,%s,%d\n",
                        t - t0, p->pid, p->comm,
                        p->truth >= 0 ? CLASS_NAME[p->truth] : "unknown",
                        p->ewma.cpu_share, p->ewma.blocked, sleep_frac(&p->ewma),
                        p->ewma.wake_s, p->ewma.burst_ms, p->ewma.io_mbps, p->ewma.io_wait,
                        CLASS_NAME[raw], conf, CLASS_NAME[p->current], moved);
        }
        if (log) fflush(log);

        next.tv_nsec += (long)interval_ms * 1000000L;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR && running)
            ;
    }

    if (log) fclose(log);
    fprintf(stderr, "ctxschedd: stopped\n");
    return 0;
}
