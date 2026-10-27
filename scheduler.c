/*
 * Context-Aware CPU Scheduling for Heterogeneous Workloads
 * ----------------------------------------------------------
 * C port of the Python prototype. Same pipeline, same rules:
 *
 *   Task Arrives -> Signal Collector -> Heuristic Classifier
 *                -> Quantum Mapper -> Linux CFS (cgroups)
 *
 * Build:   gcc -O2 -Wall -o scheduler scheduler.c -lm
 * Run:     ./scheduler
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ----------------------------- config ----------------------------- */

#define N_BATCH        6
#define N_INTERACTIVE  10
#define N_LATENCY      8
#define N_TASKS        (N_BATCH + N_INTERACTIVE + N_LATENCY)
#define MAX_LOG        4096
#define CTX_SWITCH_MS  0.05

typedef enum { BATCH = 0, INTERACTIVE = 1, LATENCY_SENSITIVE = 2, N_TYPES = 3 } TaskType;
static const char *TYPE_NAME[N_TYPES] = { "batch", "interactive", "latency_sensitive" };

/* simple deterministic PRNG (xorshift) so runs are reproducible */
static unsigned long rng_state = 88172645463325252ULL;
static double rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state % 1000000) / 1000000.0;   /* [0,1) */
}
static double rnd_range(double lo, double hi) { return lo + rnd() * (hi - lo); }

/* ----------------------------- task model ----------------------------- */

typedef struct {
    int tid;
    TaskType true_type;
    double arrival_time;
    double total_work_ms;
    double remaining_ms;

    double io_wait_ratio;
    double avg_burst_ms;
    double wake_ups_per_sec;

    double first_run_time;   /* -1 = not yet run */
    double finish_time;      /* -1 = not finished */
    double run_log[MAX_LOG];
    int    run_log_len;
} Task;

static void make_workload(Task tasks[N_TASKS]) {
    int tid = 0;

    for (int i = 0; i < N_BATCH; i++, tid++) {
        Task *t = &tasks[tid];
        t->tid = tid; t->true_type = BATCH;
        t->arrival_time = rnd_range(0, 50);
        t->total_work_ms = rnd_range(800, 2000);
        t->io_wait_ratio = rnd_range(0.0, 0.1);
        t->avg_burst_ms = rnd_range(40, 80);
        t->wake_ups_per_sec = rnd_range(0.5, 2);
        t->remaining_ms = t->total_work_ms;
        t->first_run_time = -1; t->finish_time = -1; t->run_log_len = 0;
    }
    for (int i = 0; i < N_INTERACTIVE; i++, tid++) {
        Task *t = &tasks[tid];
        t->tid = tid; t->true_type = INTERACTIVE;
        t->arrival_time = rnd_range(0, 300);
        t->total_work_ms = rnd_range(20, 60);
        t->io_wait_ratio = rnd_range(0.5, 0.8);
        t->avg_burst_ms = rnd_range(3, 8);
        t->wake_ups_per_sec = rnd_range(5, 15);
        t->remaining_ms = t->total_work_ms;
        t->first_run_time = -1; t->finish_time = -1; t->run_log_len = 0;
    }
    for (int i = 0; i < N_LATENCY; i++, tid++) {
        Task *t = &tasks[tid];
        t->tid = tid; t->true_type = LATENCY_SENSITIVE;
        t->arrival_time = rnd_range(0, 300);
        t->total_work_ms = rnd_range(8, 25);
        t->io_wait_ratio = rnd_range(0.1, 0.3);
        t->avg_burst_ms = rnd_range(2, 5);
        t->wake_ups_per_sec = rnd_range(10, 25);
        t->remaining_ms = t->total_work_ms;
        t->first_run_time = -1; t->finish_time = -1; t->run_log_len = 0;
    }
}

/* ------------------------ pipeline stage 1: signal collector ------------------------ */

typedef struct {
    double io_wait_ratio;
    double avg_burst_ms;
    double wake_ups_per_sec;
    double recent_cpu_share;
} Signals;

static double jitter(double x, double noise) {
    double f = 1.0 + rnd_range(-noise, noise);
    double v = x * f;
    return v < 0 ? 0 : v;
}

static Signals collect_signals(const Task *t) {
    Signals s;
    s.io_wait_ratio     = jitter(t->io_wait_ratio, 0.05);
    s.avg_burst_ms       = jitter(t->avg_burst_ms, 0.05);
    s.wake_ups_per_sec   = jitter(t->wake_ups_per_sec, 0.05);
    s.recent_cpu_share   = jitter(t->remaining_ms / (t->total_work_ms > 1e-6 ? t->total_work_ms : 1e-6), 0.05);
    return s;
}

/* ------------------------ pipeline stage 2: heuristic classifier ------------------------ */

static TaskType classify(const Signals *s) {
    if (s->wake_ups_per_sec >= 8.0 && s->avg_burst_ms <= 6.0)
        return LATENCY_SENSITIVE;
    if (s->io_wait_ratio >= 0.4 && s->wake_ups_per_sec >= 3.0)
        return INTERACTIVE;
    return BATCH;   /* conservative default */
}

/* ------------------------ pipeline stage 3: quantum mapper ------------------------ */

static double quantum_for_type(TaskType t) {
    static const double Q[N_TYPES] = { 20.0, 6.0, 2.0 }; /* batch, interactive, latency */
    return Q[t];
}

/* ------------------------ schedulers ------------------------ */

/* Baseline: fixed quantum for everyone (proxy for vanilla CFS default). */
static double baseline_quantum(Task *t) {
    (void)t;
    return 10.0;
}

/* Adaptive: runs the full pipeline on every reschedule. */
static double adaptive_quantum(Task *t) {
    Signals s = collect_signals(t);
    TaskType cls = classify(&s);
    return quantum_for_type(cls);
}

/* ------------------------ simulation engine ------------------------ */

typedef double (*QuantumFn)(Task *);

static void run_simulation(Task tasks[N_TASKS], QuantumFn qfn) {
    /* order pool by arrival time (simple insertion sort, N is tiny) */
    Task *pending[N_TASKS];
    for (int i = 0; i < N_TASKS; i++) pending[i] = &tasks[i];
    for (int i = 1; i < N_TASKS; i++) {
        Task *key = pending[i]; int j = i - 1;
        while (j >= 0 && pending[j]->arrival_time > key->arrival_time) {
            pending[j + 1] = pending[j]; j--;
        }
        pending[j + 1] = key;
    }
    int pending_head = 0;

    Task *ready[N_TASKS]; int ready_len = 0;
    double now = 0.0;
    int finished = 0;

    while (finished < N_TASKS) {
        while (pending_head < N_TASKS && pending[pending_head]->arrival_time <= now)
            ready[ready_len++] = pending[pending_head++];

        if (ready_len == 0) {
            if (pending_head < N_TASKS) { now = pending[pending_head]->arrival_time; continue; }
            break;
        }

        Task *task = ready[0];
        memmove(ready, ready + 1, (--ready_len) * sizeof(Task *));

        if (task->first_run_time < 0) {
            task->first_run_time = now;
            if (task->run_log_len < MAX_LOG) task->run_log[task->run_log_len++] = now - task->arrival_time;
        }

        double quantum = qfn(task);
        double run_for = quantum < task->remaining_ms ? quantum : task->remaining_ms;
        now += run_for + CTX_SWITCH_MS;
        task->remaining_ms -= run_for;

        while (pending_head < N_TASKS && pending[pending_head]->arrival_time <= now)
            ready[ready_len++] = pending[pending_head++];

        if (task->remaining_ms <= 1e-9) {
            task->finish_time = now;
            finished++;
        } else {
            if (task->run_log_len < MAX_LOG) task->run_log[task->run_log_len++] = now;
            ready[ready_len++] = task;
        }
    }
}

/* ------------------------ metrics ------------------------ */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double p95(double *values, int n) {
    if (n == 0) return NAN;
    qsort(values, n, sizeof(double), cmp_double);
    int idx = (int)(0.95 * n);
    if (idx >= n) idx = n - 1;
    return values[idx];
}

static double mean(const double *values, int n) {
    if (n == 0) return NAN;
    double s = 0; for (int i = 0; i < n; i++) s += values[i];
    return s / n;
}

typedef struct {
    double interactive_p95, interactive_mean, batch_mean_completion, fairness_stdev, makespan;
} Summary;

static Summary summarize(Task tasks[N_TASKS]) {
    double gaps[MAX_LOG]; int n_gaps = 0;
    double batch_completion[N_BATCH]; int n_batch = 0;
    double class_shares[N_TYPES]; int n_class = 0;
    double makespan = 0;

    for (int i = 0; i < N_TASKS; i++) {
        Task *t = &tasks[i];
        if (t->finish_time > makespan) makespan = t->finish_time;

        if (t->true_type == INTERACTIVE || t->true_type == LATENCY_SENSITIVE) {
            for (int k = 1; k < t->run_log_len; k++)
                if (n_gaps < MAX_LOG) gaps[n_gaps++] = t->run_log[k] - t->run_log[k - 1];
        }
        if (t->true_type == BATCH)
            batch_completion[n_batch++] = t->finish_time - t->arrival_time;
    }

    for (TaskType ty = 0; ty < N_TYPES; ty++) {
        double sum_share = 0; int cnt = 0;
        for (int i = 0; i < N_TASKS; i++) {
            Task *t = &tasks[i];
            if (t->true_type == ty) {
                double denom = t->finish_time - t->arrival_time;
                if (denom < 1e-6) denom = 1e-6;
                sum_share += t->total_work_ms / denom;
                cnt++;
            }
        }
        if (cnt > 0) class_shares[n_class++] = sum_share / cnt;
    }

    double fairness = 0;
    if (n_class > 1) {
        double m = mean(class_shares, n_class), var = 0;
        for (int i = 0; i < n_class; i++) var += (class_shares[i] - m) * (class_shares[i] - m);
        fairness = sqrt(var / n_class);
    }

    Summary s;
    double gaps_copy[MAX_LOG]; memcpy(gaps_copy, gaps, n_gaps * sizeof(double));
    s.interactive_p95 = p95(gaps_copy, n_gaps);
    s.interactive_mean = mean(gaps, n_gaps);
    s.batch_mean_completion = mean(batch_completion, n_batch);
    s.fairness_stdev = fairness;
    s.makespan = makespan;
    return s;
}

/* ------------------------ classifier accuracy ------------------------ */

static double classifier_accuracy(Task tasks[N_TASKS], int trials, int confusion[N_TYPES][N_TYPES]) {
    int correct = 0, total = 0;
    memset(confusion, 0, sizeof(int) * N_TYPES * N_TYPES);
    for (int trial = 0; trial < trials; trial++) {
        for (int i = 0; i < N_TASKS; i++) {
            Signals s = collect_signals(&tasks[i]);
            TaskType pred = classify(&s);
            confusion[tasks[i].true_type][pred]++;
            total++;
            if (pred == tasks[i].true_type) correct++;
        }
    }
    return (double)correct / total;
}

/* ------------------------ main ------------------------ */

static Task base_tasks[N_TASKS];
static Task adapt_tasks[N_TASKS];

int main(void) {
    Task seed_tasks[N_TASKS];
    make_workload(seed_tasks);
    memcpy(base_tasks, seed_tasks, sizeof(seed_tasks));
    memcpy(adapt_tasks, seed_tasks, sizeof(seed_tasks));

    run_simulation(base_tasks, baseline_quantum);
    run_simulation(adapt_tasks, adaptive_quantum);

    Summary base = summarize(base_tasks);
    Summary adapt = summarize(adapt_tasks);

    printf("%-38s %16s %16s %10s\n", "Metric", "Baseline", "Context-Aware", "Change");
    for (int i = 0; i < 84; i++) putchar('-');
    putchar('\n');

    #define ROW(label, bval, aval) do { \
        double b = (bval), a = (aval); \
        char chg[16]; \
        if (b != 0) snprintf(chg, sizeof(chg), "%+.1f%%", (a - b) / b * 100.0); \
        else strcpy(chg, "-"); \
        printf("%-38s %16.2f %16.2f %10s\n", label, b, a, chg); \
    } while (0)

    ROW("Interactive p95 response (ms)", base.interactive_p95, adapt.interactive_p95);
    ROW("Interactive mean response (ms)", base.interactive_mean, adapt.interactive_mean);
    ROW("Batch mean completion (ms)", base.batch_mean_completion, adapt.batch_mean_completion);
    ROW("Fairness: CPU-share stdev", base.fairness_stdev, adapt.fairness_stdev);
    ROW("Makespan (ms)", base.makespan, adapt.makespan);

    printf("\n");
    int confusion[N_TYPES][N_TYPES];
    double acc = classifier_accuracy(seed_tasks, 200, confusion);
    printf("Heuristic classifier accuracy over noisy signals: %.1f%%\n\n", acc * 100.0);

    printf("%-20s", "true \\ pred");
    for (int j = 0; j < N_TYPES; j++) printf("%18s", TYPE_NAME[j]);
    printf("\n");
    for (int i = 0; i < N_TYPES; i++) {
        int row_total = 0;
        for (int j = 0; j < N_TYPES; j++) row_total += confusion[i][j];
        printf("%-20s", TYPE_NAME[i]);
        for (int j = 0; j < N_TYPES; j++)
            printf("%17.1f%%", row_total ? 100.0 * confusion[i][j] / row_total : 0.0);
        printf("\n");
    }

    return 0;
}
