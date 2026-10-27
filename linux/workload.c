/*
 * Synthetic workloads with known ground truth for ctxschedd
 * ---------------------------------------------------------
 *   batch        CPU-bound job, never blocks                 (comm: wl_batch)
 *   interactive  think/sleep 50-150 ms, then a 5-15 ms burst (comm: wl_interactive)
 *   latency      5 ms periodic timer, 0.3-1 ms burst         (comm: wl_latency)
 *   phase        behaves as --from for --after seconds, then as --to
 *                (comm: ph_<from>, renamed to ph_<to> at the switch)
 *   adversary    a batch job that naps for a moment between short bursts so it
 *                looks like a latency task (comm: adv_batch)
 *
 * The comm name is the ground truth. The daemon uses it only to score its own
 * accuracy; it never uses it to classify.
 *
 * Usage: workload <type> [--work MS] [--duration S] [--seed N] [--cgroup DIR]
 *                        [--from TYPE --to TYPE --after S]
 *                        [--burst MS --nap MS]
 *   --work      batch/adversary: CPU milliseconds to burn (default 3000)
 *   --duration  interactive/latency/phase: seconds to run (default 20);
 *               given to batch, it becomes timed background load instead
 *   --cgroup    join this cgroup v2 directory before starting
 *   --from/--to/--after   phase: the two behaviours and when to switch
 *   --burst/--nap         adversary: CPU per burst and sleep between (default 1.5 / 0.2 ms)
 *
 * Prints one CSV line when done:
 *   type,pid,samples,mean_ms,p50_ms,p95_ms,p99_ms,max_ms,completion_ms
 *   interactive: sample = response time, intended wake -> burst finished
 *   latency:     sample = wake-up latency, actual wake - timer deadline
 *   phase:       samples from whichever phase is interactive/latency
 *   batch/adversary: no samples; completion_ms = wall time to finish --work
 *   batch_timed: no samples; completion_ms = CPU time obtained in --duration
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/prctl.h>

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Burn `ms` of CPU time (not wall time, so preemption doesn't shorten the work). */
static void burn(double ms) {
    double end = cpu_ms() + ms;
    volatile unsigned long x = 0;
    while (cpu_ms() < end)
        for (int i = 0; i < 2000; i++) x += i;
}

static void sleep_until(double t_ms) {
    struct timespec ts = { (time_t)(t_ms / 1e3), (long)((t_ms - (time_t)(t_ms / 1e3) * 1e3) * 1e6) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

static unsigned int seed;
static double rnd_range(double lo, double hi) { return lo + (hi - lo) * (rand_r(&seed) / (double)RAND_MAX); }

/* ------------------------ samples + report ------------------------ */

typedef struct { double *v; int n, cap; } Samples;

static void add_sample(Samples *s, double x) {
    if (s->n == s->cap) {
        s->cap = s->cap ? 2 * s->cap : 1024;
        s->v = realloc(s->v, s->cap * sizeof(double));
        if (!s->v) { perror("realloc"); exit(1); }
    }
    s->v[s->n++] = x;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct(const double *v, int n, double p) {
    if (n == 0) return 0;
    int i = (int)(p * n);
    return v[i < n ? i : n - 1];
}

static void report(const char *type, Samples *s, double completion_ms) {
    double sum = 0;
    int n = s->n;
    for (int i = 0; i < n; i++) sum += s->v[i];
    if (n) qsort(s->v, n, sizeof(double), cmp_double);
    printf("%s,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f\n", type, getpid(), n,
           n ? sum / n : 0, pct(s->v, n, 0.50), pct(s->v, n, 0.95), pct(s->v, n, 0.99),
           n ? s->v[n - 1] : 0, completion_ms);
    fflush(stdout);
}

/* ------------------------ behaviours ------------------------ */

/* CPU-bound until `work_ms` of CPU is used or the wall clock reaches `until`. */
static void do_batch(double work_ms, double until) {
    double end = cpu_ms() + work_ms;
    while (cpu_ms() < end && mono_ms() < until)
        burn(1.0);
}

static void do_interactive(double until, Samples *s) {
    double wake = mono_ms();
    for (;;) {
        wake += rnd_range(50, 150);            /* user think time / waiting on I/O */
        if (wake >= until) break;
        sleep_until(wake);
        burn(rnd_range(5, 15));                /* handle the event */
        double done = mono_ms();
        add_sample(s, done - wake);
        if (done > wake) wake = done;
    }
}

static void do_latency(double until, Samples *s) {
    const double period = 5.0;
    double deadline = mono_ms() + period;
    while (deadline < until) {
        sleep_until(deadline);
        add_sample(s, mono_ms() - deadline);  /* scheduling delay */
        burn(rnd_range(0.3, 1.0));
        deadline += period;
        double now = mono_ms();
        while (deadline < now) deadline += period;   /* missed periods: skip, don't backlog */
    }
}

/* Gaming the classifier: many wake-ups and short bursts, but almost never idle. */
static void do_adversary(double work_ms, double burst_ms, double nap_ms) {
    double end = cpu_ms() + work_ms;
    struct timespec nap = { 0, (long)(nap_ms * 1e6) };
    while (cpu_ms() < end) {
        burn(burst_ms);
        nanosleep(&nap, NULL);
    }
}

static int run_behaviour(const char *type, double work_ms, double until, Samples *s) {
    if (strcmp(type, "batch") == 0)            do_batch(work_ms, until);
    else if (strcmp(type, "interactive") == 0) do_interactive(until, s);
    else if (strcmp(type, "latency") == 0)     do_latency(until, s);
    else { fprintf(stderr, "unknown behaviour: %s\n", type); return -1; }
    return 0;
}

static void set_comm(const char *prefix, const char *type) {
    char name[16];
    snprintf(name, sizeof(name), "%s_%s", prefix, type);
    prctl(PR_SET_NAME, name);
}

static int join_cgroup(const char *dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/cgroup.procs", dir);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    fprintf(f, "%d\n", getpid());
    if (fclose(f) != 0) { perror(path); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <batch|interactive|latency|phase|adversary> [options]\n", argv[0]);
        return 2;
    }
    const char *type = argv[1];
    double work_ms = 3000, duration_s = 20, after_s = 10, burst_ms = 1.5, nap_ms = 0.2;
    const char *cgroup = NULL, *from = "batch", *to = "latency";
    int duration_given = 0;
    seed = (unsigned int)getpid();

    static const struct option opts[] = {
        { "work",     required_argument, 0, 'w' },
        { "duration", required_argument, 0, 'd' },
        { "seed",     required_argument, 0, 's' },
        { "cgroup",   required_argument, 0, 'c' },
        { "from",     required_argument, 0, 'f' },
        { "to",       required_argument, 0, 't' },
        { "after",    required_argument, 0, 'a' },
        { "burst",    required_argument, 0, 'b' },
        { "nap",      required_argument, 0, 'n' },
        { 0, 0, 0, 0 }
    };
    int ch;
    optind = 2;
    while ((ch = getopt_long(argc, argv, "w:d:s:c:f:t:a:b:n:", opts, NULL)) != -1) {
        switch (ch) {
        case 'w': work_ms = atof(optarg); break;
        case 'd': duration_s = atof(optarg); duration_given = 1; break;
        case 's': seed = (unsigned int)strtoul(optarg, NULL, 10); break;
        case 'c': cgroup = optarg; break;
        case 'f': from = optarg; break;
        case 't': to = optarg; break;
        case 'a': after_s = atof(optarg); break;
        case 'b': burst_ms = atof(optarg); break;
        case 'n': nap_ms = atof(optarg); break;
        default:  return 2;
        }
    }
    if (cgroup && join_cgroup(cgroup) != 0) return 1;

    Samples s = { 0 };
    double start = mono_ms();
    double end = start + duration_s * 1e3;

    if (strcmp(type, "batch") == 0 && duration_given) {
        /* background load: run for --duration, report CPU time obtained instead */
        set_comm("wl", type);
        double cpu0 = cpu_ms();
        do_batch(1e300, end);
        report("batch_timed", &s, cpu_ms() - cpu0);

    } else if (strcmp(type, "batch") == 0 || strcmp(type, "interactive") == 0 || strcmp(type, "latency") == 0) {
        set_comm("wl", type);
        /* a plain batch job runs until its work is done, however long that takes */
        if (run_behaviour(type, work_ms, strcmp(type, "batch") == 0 ? 1e300 : end, &s) != 0) return 2;
        report(type, &s, mono_ms() - start);

    } else if (strcmp(type, "phase") == 0) {
        set_comm("ph", from);
        if (run_behaviour(from, 1e300, start + after_s * 1e3, &s) != 0) return 2;
        set_comm("ph", to);
        if (run_behaviour(to, 1e300, end, &s) != 0) return 2;
        char label[64];
        snprintf(label, sizeof(label), "phase_%s2%s", from, to);
        report(label, &s, mono_ms() - start);

    } else if (strcmp(type, "adversary") == 0) {
        prctl(PR_SET_NAME, "adv_batch");
        do_adversary(work_ms, burst_ms, nap_ms);
        report("adversary", &s, mono_ms() - start);

    } else {
        fprintf(stderr, "unknown workload type: %s\n", type);
        return 2;
    }
    free(s.v);
    return 0;
}
