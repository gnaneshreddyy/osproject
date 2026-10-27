# Context-Aware CPU Scheduling for Heterogeneous Workloads

A general-purpose scheduler treats every task the same way. This project
observes how each running process actually behaves, sorts it into one of three
classes, and gives each class different treatment from the CPU scheduler:

| Class | Typical behaviour | What it needs |
|---|---|---|
| **batch** | runs flat out, rarely sleeps (compilers, encoders, number crunching) | throughput; can wait |
| **interactive** | sleeps waiting for a user or I/O, then runs a short burst (editors, shells, request handlers, disk-heavy jobs) | quick response when it wakes |
| **latency-sensitive** | wakes on a short timer and does a tiny amount of work (audio, control loops, game ticks) | to run *immediately* after waking |

The project has two halves that share the same pipeline:

```
Task arrives -> Signal Collector -> Heuristic Classifier -> Mapper -> Kernel scheduler
```

1. **`scheduler.c`: the simulator.** A self-contained discrete-time model in
   which the mapper sets a time quantum per class. It is used for controlled
   what-if experiments (Review 2).
2. **`linux/`: the real system.** A daemon (`ctxschedd`) that runs on a real
   Linux kernel, including WSL2 on Windows. It reads live per-PID scheduler
   counters from `/proc`, classifies each process, and moves it between cgroup
   v2 groups whose `cpu.weight` encodes the class. It comes with workloads with
   known ground truth, real benchmarks, an experiment runner and a
   statistical analysis script.

---

## Contents
1. [Repository layout](#1-repository-layout)
2. [Deep dive: the simulator (`scheduler.c`)](#2-deep-dive-the-simulator-schedulerc)
3. [Deep dive: the real daemon (`linux/daemon.c`)](#3-deep-dive-the-real-daemon-linuxdaemonc)
4. [Deep dive: the supporting files](#4-deep-dive-the-supporting-files)
5. [How process IDs are captured and managed](#5-how-process-ids-are-captured-and-managed)
6. [Running everything](#6-running-everything)
7. [Reading and comparing the results](#7-reading-and-comparing-the-results)
8. [Doing your own experiments by hand](#8-doing-your-own-experiments-by-hand)
9. [Troubleshooting](#9-troubleshooting)
10. [Limitations and design decisions](#10-limitations-and-design-decisions)

---

## 1. Repository layout

```
project/
├── README.md                     this file
├── scheduler.c                   simulator: whole pipeline in one C file
├── Context-Aware_CPU_Scheduling_Review2.pptx   Review 2 slides
├── results_chart.png             chart used in the slides (from scheduler.c output)
└── linux/                        real-system implementation
    ├── daemon.c                  ctxschedd: signals -> classifier -> cgroup weights
    ├── workload.c                test programs with known ground truth
    ├── Makefile                  builds ctxschedd and workload
    ├── setup.sh                  checks the kernel, creates cgroups, installs tools, builds
    ├── run_experiment.sh         runs scenarios in baseline / adaptive / noguard modes
    ├── analyze.py                statistics, classifier accuracy, summary.csv
    ├── README.md                 short pointer back to this file
    └── results/<timestamp>/<scenario>/   output of each experiment run
```

---

## 2. Deep dive: the simulator (`scheduler.c`)

A single dependency-free C file. Build and run it with:

```bash
gcc -O2 -Wall -o scheduler scheduler.c -lm && ./scheduler
```

### 2.1 Task model
`make_workload()` creates 24 tasks: 6 batch, 10 interactive and 8
latency-sensitive. Each `Task` has a true type, an arrival time, a total
amount of work, and three behaviour parameters drawn from ranges that fit its
type:

| Type | Work (ms) | `io_wait_ratio` | `avg_burst_ms` | `wake_ups_per_sec` |
|---|---|---|---|---|
| batch | 800–2000 | 0.0–0.1 | 40–80 | 0.5–2 |
| interactive | 20–60 | 0.5–0.8 | 3–8 | 5–15 |
| latency | 8–25 | 0.1–0.3 | 2–5 | 10–25 |

A fixed-seed xorshift PRNG makes every run reproducible.

### 2.2 Pipeline stage 1: `collect_signals()`
Returns the task's three behaviour parameters with ±5% random jitter, plus
`recent_cpu_share` (remaining work ÷ total work).
**Limitation:** these "signals" come from the task's own true parameters, so
the classifier is partly reading the answer key. The real daemon fixes this by
*measuring* behaviour instead (§3).

### 2.3 Stage 2: `classify()`
Two ordered rules with a conservative default:
```
if wake_ups >= 8/s and burst <= 6 ms        -> LATENCY_SENSITIVE
else if io_wait >= 0.4 and wake_ups >= 3/s  -> INTERACTIVE
else                                        -> BATCH
```

### 2.4 Stage 3: `quantum_for_type()`
Maps the class to a time quantum: batch 20 ms, interactive 6 ms, latency 2 ms.
Short quanta mean a task that wakes up gets the CPU again sooner.

### 2.5 Schedulers and the dispatch loop
- `baseline_quantum()` gives everyone 10 ms. It stands in for a stock scheduler.
- `adaptive_quantum()` runs the full pipeline (collect → classify → quantum)
  **on every reschedule**.
- `run_simulation()` is a FIFO round-robin. Tasks join the ready queue on
  arrival. The head task runs for `min(quantum, remaining)`, a 0.05 ms context
  switch is charged, and an unfinished task goes back to the tail of the queue.
  Every dispatch time is logged per task.

### 2.6 Metrics
- **Interactive p95/mean response:** gaps between successive dispatches of
  interactive and latency tasks.
- **Batch mean completion:** finish time − arrival time.
- **Fairness:** the standard deviation of per-class CPU share.
- **Makespan:** the time the last task finishes.
- **Classifier accuracy:** 200 noisy classifications per task, with a confusion
  matrix.

---

## 3. Deep dive: the real daemon (`linux/daemon.c`)

`ctxschedd` is the simulator's pipeline running against a real kernel. It is
one C file, organised in the same stages.

### 3.1 The cgroup hierarchy it uses
```
/sys/fs/cgroup/ctxsched/
├── pending/       cpu.weight 100   new tasks start here (the "default" class)
├── batch/         cpu.weight  50
├── interactive/   cpu.weight 200
└── latency/       cpu.weight 400
```
`cpu.weight` is cgroup v2's relative CPU share (range 1–10000, default 100).
The kernel scheduler, EEVDF on 6.6 and later or CFS before that, divides CPU
time between sibling groups in proportion to these weights. A heavier group
also gets earlier virtual deadlines, so its tasks are picked sooner after they
wake up. At startup the daemon writes these four weights (`apply_weights()`).
The daemon only ever touches processes inside `ctxsched/`. The rest of the
system is left alone.

### 3.2 Main loop (runs every 100 ms by default)
```
scan_cgroups()                     which PIDs are in ctxsched/*?  (§5)
for each PID:
    read_comm()                    process name (used only for scoring)
    read_counters()                raw cumulative counters from /proc
    signals_from_deltas()          turn counter changes into 5 signals
    ewma_update()                  smooth them
    classify()                     class + confidence
    hysteresis                     only switch after 3 agreeing samples
    move_to_class()                write PID into the new cgroup (unless --observe)
    log one CSV row
sleep until the next tick (absolute timer, so the cadence doesn't drift)
```

### 3.3 Stage 1: Signal collector
`read_counters()` reads cumulative counters and **sums them over every thread**
in `/proc/<pid>/task/*`:

| Counter | Source | Meaning |
|---|---|---|
| `cpu_ns` | `/proc/<pid>/task/<tid>/schedstat`, field 1 | total time spent running on a CPU |
| `wait_ns` | `schedstat`, field 2 | total time runnable but waiting in the run queue |
| `vol` | `voluntary_ctxt_switches` in `/proc/<pid>/task/<tid>/status` | times the task gave up the CPU by itself (sleep, I/O, lock): one per block/wake cycle |
| `io_bytes` | `read_bytes` + `write_bytes` in `/proc/<pid>/io` | bytes that actually went to or from storage |

`signals_from_deltas()` takes the difference between this sample and the
previous one (Δt ≈ 100 ms) and derives:

| Signal | Formula | Intuition |
|---|---|---|
| `cpu_share` | Δcpu / Δt | how much of the time it was running |
| `blocked` | 1 − cpu_share − Δwait / Δt | how much of the time it was asleep |
| `wake_s` | Δvol / Δt | wake-ups per second |
| `burst_ms` | Δcpu / Δvol (or Δcpu if it never slept) | CPU used per wake-up |
| `io_mbps` | Δio_bytes / Δt / 10⁶ | storage throughput |

If a thread exits, the summed counters can shrink. `delta()` clamps negative
differences to 0. `ewma_update()` then smooths each signal with an
exponentially weighted moving average (α = 0.4), so a single noisy interval
can't flip the classification.

One more derived value is central to the classifier:

```
sleep_frac = blocked / (blocked + cpu_share)
```

This is "of the time the task was *not* stuck waiting for a CPU, how much did
it spend asleep?" It matters because on a busy CPU even a latency task spends
most of its wall time in the run queue, so its `blocked` share drops.
`sleep_frac` ignores run-queue time and gives the same answer at any load. An
early version of the guard used `blocked`, and under contention it sent real
latency tasks to the batch class 67% of the time.

### 3.4 Stage 2: Heuristic classifier (`classify()`)
The rules are checked in order:

| # | Rule | Conditions | Why |
|---|---|---|---|
| 1 | **I/O-bound → interactive** | `sleep_frac ≥ 0.4` and `io_mbps ≥ 1` | A disk-heavy task (e.g. fio) wakes often with tiny bursts, so without this rule it would look latency-sensitive. |
| 2 | **Latency** | `wake_s ≥ 50`, `burst_ms ≤ 2`, and *guard*: `sleep_frac ≥ 0.5` | Frequent wake-ups with tiny bursts. The guard demands that the task really sleeps more than it computes. |
| 3 | **Interactive** | `sleep_frac ≥ 0.4` and `wake_s ≥ 2` | Mostly asleep, wakes regularly. |
| 4 | **Batch** | everything else | The conservative default: the lowest weight is the safe mistake. |

**Confidence.** Each rule has a *margin*: how far the signals sit past its
thresholds. Rate-type signals use a log₂ scale, so a margin of 1.0 means "a
factor of 2 past the threshold". Fractions use units of 0.2. A rule fires when
all of its margins are positive. The confidence of the decision is the
smallest margin involved, including how clearly the earlier rules *failed*,
capped at 1. A task that sits right on a threshold therefore gets low
confidence.

**Fallback.** If confidence is below 0.25, the task is placed in `default`
(the `pending` group, weight 100) instead of being trusted to a class.

**Hysteresis.** The daemon keeps a `candidate` class and a `streak` counter.
A task changes class only after the same candidate wins **3 consecutive
samples** (≈ 300 ms). This stops tasks near a threshold from bouncing between
groups. It also sets the reaction time to a behaviour change, which the
`phase` scenario measures at roughly 0.5–0.7 s including EWMA settling.

**The guard against gaming.** A batch job could try to grab the latency
weight by sleeping for a moment between bursts (1.5 ms run, 0.2 ms nap gives
about 600 wake-ups/s with 1.5 ms bursts). Without the guard, rule 2 accepts
it. With the guard, its `sleep_frac` is only about 0.12, so it is classed as
batch. `--no-guard` turns the guard off for the ablation experiment.

### 3.5 Stage 3: Weight mapper / cgroup writer
`move_to_class()` writes the PID into
`/sys/fs/cgroup/ctxsched/<class>/cgroup.procs`. The kernel moves the whole
process, all its threads, atomically. From then on the kernel enforces the
group's weight itself. The daemon does **not** sit in the scheduling path and
adds no overhead to a context switch.

**Why weights instead of a quantum:** kernels 6.6 and later (including WSL2's
6.6.87) use EEVDF, where user space has no per-task quantum to set; custom
slices only arrived in 6.12. The simulator's 2/6/20 ms quanta are therefore
mapped to weights of 400/200/50. In effect a heavier group gets an earlier
virtual deadline, so its tasks are picked sooner after waking, which is
exactly what a short quantum was meant to achieve.

### 3.6 Command-line options
| Option | Effect |
|---|---|
| *(none)* | adaptive mode: classify and move tasks |
| `--observe` | classify and log, but never move anything. This is the **baseline**: all tasks stay at weight 100, while you can still see what the classifier *would* have done. |
| `--no-guard` | disable the latency guard (ablation) |
| `--interval MS` | sampling period (default 100, minimum 10) |
| `--duration S` | stop after S seconds (default: run until Ctrl-C or SIGTERM) |
| `--log FILE` | per-sample CSV log |

Class changes are printed to stderr as they happen:
```
t=  0.70s pid 399    wl_latency      -> latency     (conf 1.00)
```

### 3.7 The log (`--log FILE`)
There is one row per PID per tick:

`t_s, pid, comm, truth, cpu_share, blocked, sleep_frac, wake_s, burst_ms, io_mbps, raw_class, conf, decision, moved`

- `raw_class` is the classifier's answer for this sample.
- `decision` is the class after the fallback and hysteresis, which is what was
  actually applied.
- `truth` comes from the process name (§3.8). It is `unknown` for processes
  that aren't ours.
- `moved` is 1 on the sample where a cgroup write happened.

### 3.8 Ground truth (scoring only)
`truth_from_comm()` maps a process name to its intended class, so accuracy
can be measured. **The classifier never sees this value.**
- Our workloads are named `<tag>_<class>`: `wl_batch`, `wl_interactive`,
  `wl_latency`, `ph_*` (phase tasks rename themselves when they switch), and
  `adv_batch`.
- Benchmark tools: `stress-ng*` and `sysbench` → batch, `fio` → interactive
  (I/O-bound), `cyclictest` → latency.

The name is re-read every tick, so a phase change is detected the moment the
task renames itself.

---

## 4. Deep dive: the supporting files

### 4.1 `linux/workload.c`: test programs with known behaviour
One binary, several personalities. All of them spend *CPU time* (measured
with `CLOCK_THREAD_CPUTIME_ID`), not wall time, so being preempted doesn't
shrink the work. Each one sets its own process name with `prctl(PR_SET_NAME)`.

| Type | Behaviour | What it measures |
|---|---|---|
| `batch --work MS` | burns MS of CPU, never sleeps | completion time (wall) |
| `batch --duration S` | background load for S seconds | CPU time it managed to get (`batch_timed`) |
| `interactive` | sleeps 50–150 ms ("think time"), then a 5–15 ms burst | **response time** = from intended wake-up until the burst finishes |
| `latency` | absolute 5 ms timer, 0.3–1 ms burst per tick | **wake-up latency** = actual wake-up − timer deadline. Missed periods are skipped, not queued. |
| `phase --from A --to B --after S` | behaves as A, then as B, and renames itself `ph_A` → `ph_B` | samples from whichever phase is interactive or latency |
| `adversary --work MS [--burst 1.5 --nap 0.2]` | batch work broken into short bursts with tiny naps | completion time |

`--cgroup DIR` makes the program write its own PID into `DIR/cgroup.procs`
before it starts. `--seed N` makes the random think times and bursts
reproducible.

On exit it prints one CSV line:
`type,pid,samples,mean_ms,p50_ms,p95_ms,p99_ms,max_ms,completion_ms`

### 4.2 `linux/setup.sh`
1. Refuses to run without root.
2. Checks that `/sys/fs/cgroup` is cgroup v2, that the `cpu` controller
   exists, and that `/proc/<pid>/schedstat` is present.
3. Prints the kernel version and scheduler (EEVDF or CFS), and whether it is
   running under WSL2.
4. `--install-tools`: `apt-get install` build-essential, stress-ng, sysbench,
   fio, rt-tests (cyclictest) and python3.
5. Enables `+cpu` in the root and `ctxsched` `cgroup.subtree_control`, then
   creates the four class groups.
6. Builds `ctxschedd` and `workload` with `make`.

`--teardown` moves any leftover processes back to the root cgroup and removes
the groups.

### 4.3 `linux/run_experiment.sh`
```
sudo ./run_experiment.sh [scenario] [reps] [duration_s]
```
- **Contention is forced.** The `in_cg` helper starts every task with
  `taskset -c $CORE` (core 0) after putting it in `ctxsched/pending`. The
  daemon runs on another core (`DAEMON_CORE=1`). On an idle 20-CPU machine
  every task would get its own core and weights would change nothing.
- **Modes alternate within each repetition:** baseline (daemon `--observe`)
  → adaptive (→ noguard for the adversarial scenario). Slow drift on the host
  therefore hits every mode equally.
- For each mode and repetition it starts the daemon with `--log`, launches the
  scenario's tasks, waits for all of them to finish, stops the daemon with
  SIGINT, and saves every output file.
- At the end it runs `analyze.py` on the whole results folder.

| Scenario | Tasks (all on core 0) | Question it answers |
|---|---|---|
| `mixed` | 4 batch (work-based), 3 interactive, 3 latency | Do latency and interactive tasks improve, and what does batch pay? |
| `phase` | 2 batch→latency, 2 interactive→batch (switch at half time), 3 background batch, 1 latency | Does the classifier follow behaviour changes, and how fast? |
| `adversarial` | 2 adversaries, 2 background batch, 2 interactive, 3 latency; modes baseline / adaptive / **noguard** | Can a batch job game the classifier, and does the guard stop it? |
| `tools` | `stress-ng --cpu 2`, `sysbench cpu`, `fio` (4 KiB random sync direct writes), `cyclictest` (1 ms timer, histogram), 1 interactive | Does it hold up on real, unmodified programs? |
| `all` | all four, one after another | |

Tunables are set as environment variables: `CORE`, `DAEMON_CORE`, `N_BATCH`,
`N_INT`, `N_LAT`, `BATCH_WORK`, `SYSBENCH_EVENTS`.

### 4.4 `linux/analyze.py` (Python standard library only)
For every scenario folder it:
1. **Extracts metrics** from each file named `<mode>_<rep>_<kind>`:
   - `wl.csv`: latency wake-up p95/p99/max, interactive response p95/mean,
     batch completion, batch Jain fairness, background batch CPU, adversary
     completion, and phase-task p95
   - `stressng.yaml`: bogo ops/s
   - `sysbench.txt`: total time
   - `fio.json`: IOPS and write-latency p99
   - `cyclictest.txt`: p95/p99/max, computed from the 1 µs histogram
2. **Averages over repetitions** and prints the mean ± 95% confidence interval
   (Student's t), plus the % change of each mode against the baseline. It
   writes this table to `summary.csv`.
3. **Scores the classifier** from the daemon logs. It skips each task's first
   second (warm-up) and reports raw accuracy, accuracy after hysteresis, the
   fallback rate, accuracy per source (synthetic, phase-change, adversary,
   each tool), and a confusion matrix.
4. **Measures phase-change reaction time**: for each phase task, the time from
   its rename until the daemon's decision matches the new class.

---

## 5. How process IDs are captured and managed

This is the whole life of one PID under the daemon:

1. **A process enters the managed set** when its PID is written to
   `/sys/fs/cgroup/ctxsched/pending/cgroup.procs`. The experiment does this
   through `in_cg`, which runs `echo $$ > .../pending/cgroup.procs` and then
   `exec`s the real program, so the PID that joined is the program's own PID.
   You can do the same by hand for **any** process (§8.1). A process's
   children (stress-ng workers, fio jobs) are born in their parent's cgroup,
   so they are managed automatically.
2. **Discovery.** Every tick, `scan_cgroups()` reads `cgroup.procs` from all
   four class groups. A PID it hasn't seen before gets an entry in the
   process table (`lookup()`), and its current group is recorded as its
   current class.
3. **First sample.** The first time a PID is seen, the daemon only records its
   counters. Signals need two samples to form a delta.
4. **Every following tick.** It reads the PID's counters, computes signals,
   updates the EWMA, classifies, applies hysteresis, and moves the PID if its
   class changed (§3.2).
5. **Exit or removal.** When a PID no longer appears in any `ctxsched` group
   (it exited, or someone moved it out), `scan_cgroups()` frees its entry.

You can watch this happen live:
```bash
watch -n0.5 'for c in pending batch interactive latency; do echo "$c: $(cat /sys/fs/cgroup/ctxsched/$c/cgroup.procs | tr "\n" " ")"; done'
```

Why only processes in `ctxsched`, and not the whole system? Moving systemd
services or your desktop session between cgroups would interfere with how the
system manages them. The design is opt-in on purpose. To manage something,
place it in `pending`.

---

## 6. Running everything

### 6.1 One-time setup on Windows
1. Install WSL2 with Ubuntu. Run this in an **admin** PowerShell, then reboot:
   ```bash
   wsl --install -d Ubuntu
   ```
   (Already done on this machine: Ubuntu, kernel 6.6.87, cgroup v2, systemd.)
2. Open Ubuntu (type `wsl` in PowerShell) and go to the project. Windows
   drives appear under `/mnt/c`:
   ```bash
   cd /mnt/c/Users/gnane/vit/OS/project/linux
   ```
3. Install the tools, create the cgroups and build:
   ```bash
   sudo ./setup.sh --install-tools
   ```

On a native Linux machine, steps 2–3 are the same.

> The cgroups live in memory. **After every WSL restart** (`wsl --shutdown`,
> or a Windows reboot), run `sudo ./setup.sh` again. It doesn't reinstall
> anything; it only re-creates the groups.

### 6.2 The simulator
```bash
cd /mnt/c/Users/gnane/vit/OS/project && gcc -O2 -Wall -o scheduler scheduler.c -lm && ./scheduler
```
It prints the metrics table and the classifier confusion matrix.

### 6.3 The full real-kernel experiment
```bash
sudo ./run_experiment.sh all 10 20
```
That is 4 scenarios × 10 repetitions × 20 s, about 45 minutes. Keep the
machine otherwise quiet while it runs. Progress is printed as
`== scenario: rep i/10, mode ==`, and the analysis prints at the end.

A quick check (2 repetitions, 8 s each, about 3 minutes):
```bash
sudo BATCH_WORK=1500 SYSBENCH_EVENTS=8000 ./run_experiment.sh all 2 8
```

One scenario only:
```bash
sudo ./run_experiment.sh adversarial 10 20
```

Re-analyse an earlier run without re-running it:
```bash
python3 analyze.py results/<timestamp>
```

### 6.4 Clean up
```bash
sudo ./setup.sh --teardown
```

---

## 7. Reading and comparing the results

### 7.1 What a results folder contains
```
results/<timestamp>/
├── config.txt                         kernel, reps, duration, task counts
└── <scenario>/
    ├── <mode>_<rep>_wl.csv            one line per workload process
    ├── <mode>_<rep>_daemon.csv        per-tick classifier log
    ├── <mode>_<rep>_daemon.err        class changes printed by the daemon
    ├── <mode>_<rep>_{fio.json,cyclictest.txt,sysbench.txt,stressng.yaml}   tools scenario
    └── summary.csv                    the final table
```

### 7.2 How to read the metrics table
```
Metric                          baseline            adaptive         vs base (adaptive)
Latency wake-up p95 (ms)        12.07 +/- 4.89      2.29 +/- 1.23    -81.1%
```
- **mean ± CI** is the average over repetitions, ± a 95% confidence interval.
  When two modes' intervals don't overlap, the difference is clearly real.
  When they overlap heavily, treat the change as not shown, whatever the %.
- **Lower is better** unless the metric says `[higher=better]` (IOPS, bogo
  ops/s, background batch CPU).
- **Trade-offs are expected.** Latency and interactive gains come from CPU
  time given up by batch work, so batch completion going *up* a little is the
  intended cost, not a bug. Report both sides.
- **The modes:**
  - `baseline` is the stock kernel scheduler with every task at weight 100.
  - `adaptive` is this project.
  - `noguard` is this project without the anti-gaming guard. It appears only
    in the adversarial scenario.

### 7.3 Which numbers answer which question
| Question | Look at |
|---|---|
| Does context-awareness help latency tasks? | `mixed`: latency wake-up p95/p99 |
| Does it help interactive tasks? | `mixed` / `tools`: interactive response p95 |
| What does it cost? | `mixed`: batch mean completion; `phase` / `adversarial`: background batch CPU; `tools`: stress-ng / sysbench |
| Is it fair inside a class? | batch Jain index (1.0 = perfectly fair) |
| Can it be gamed? | `adversarial`: adversary accuracy (`by source`), latency p95 and adversary completion in noguard vs adaptive |
| Does it adapt to behaviour changes? | `phase`: reaction time, phase-task p95 |
| Does it work on real programs? | `tools`: classifier `by source`, fio, cyclictest |

### 7.4 How to read the classifier section
```
Classifier (adaptive, 1605 samples after 1s warm-up): raw 100.0%, after hysteresis 100.0%, low-confidence fallback 0.0%
  by source: synthetic 100%
  true \ decided   batch  interactive  latency  default
```
- **raw** is the accuracy of the per-sample answer. **after hysteresis** is
  the accuracy of what was actually applied.
- **fallback** is the share of samples sent to default because confidence was
  low.
- **Confusion matrix:** rows are the truth, columns are the decision. Anything
  off the diagonal is a mistake. In `noguard`, the batch row shows a large
  share in `latency`: that is the adversary winning.
- In **`baseline`** the classifier still runs (`--observe`), but nothing is
  moved. Comparing its accuracy with `adaptive` shows whether moving tasks
  changes their own signals (a feedback effect).

### 7.5 Comparing simulator and real system
The simulator gives clean, controlled numbers: one seed, a modelled quantum,
and signals that are partly given. The real system gives honest but noisier
numbers: measured signals, real kernel, a WSL2 VM. In a report, present the
simulator as the design study and the `linux/` results as the validation.
Point out where they agree (latency and interactive gain, a batch cost) and
where they differ, and why (quantum vs weight, the noise sources).

---

## 8. Doing your own experiments by hand

All of these run inside Ubuntu/WSL in `.../project/linux`, after
`sudo ./setup.sh`.

### 8.1 Manage any program, by PID
Terminal 1, start the daemon and watch its decisions:
```bash
sudo ./ctxschedd --log /tmp/mylog.csv
```
Terminal 2, start something pinned to core 0, then hand its PID to the
daemon:
```bash
taskset -c 0 stress-ng --cpu 2 --timeout 60s &
```
```bash
for p in $(pgrep stress-ng); do echo $p | sudo tee /sys/fs/cgroup/ctxsched/pending/cgroup.procs; done
```
An already-running process works the same way, for example a Python script
you started earlier:
```bash
echo <PID> | sudo tee /sys/fs/cgroup/ctxsched/pending/cgroup.procs
```
Within about half a second, terminal 1 prints `pid ... -> batch (conf ...)`.

### 8.2 See the raw signals for one PID
```bash
cat /proc/<PID>/schedstat; grep ctxt /proc/<PID>/status; sudo grep bytes /proc/<PID>/io
```
`schedstat` shows CPU ns, wait ns and timeslice count. The other two show the
context switches and storage bytes the daemon turns into signals.

To follow the daemon's view of that PID over time:
```bash
grep ",<PID>," /tmp/mylog.csv | cut -d, -f1,3,5-14 | column -ts,
```

### 8.3 A manual baseline-vs-adaptive comparison
Run the same load twice: once with `sudo ./ctxschedd --observe`, once with
`sudo ./ctxschedd`. Measure something latency-sensitive each time, for
example:
```bash
sudo bash -c 'echo $$ > /sys/fs/cgroup/ctxsched/pending/cgroup.procs; exec taskset -c 0 cyclictest -m -t1 -i 1000 -D 20 -q'
```
Compare the `Avg` and `Max` lines it prints.

### 8.4 Change the policy
Everything tunable is a `#define` at the top of `linux/daemon.c`:

- **Class weights:** `CLASS_WEIGHT[]`
- **Rule thresholds:**
  - `LAT_MIN_WAKE`, `LAT_MAX_BURST`
  - `LAT_MIN_SLEEP` (the guard)
  - `INT_MIN_SLEEP`, `INT_MIN_WAKE`
  - `IO_MIN_MBPS`
- **Fallback and smoothing:** `CONF_MIN`, `HYST_N`, `EWMA_ALPHA`

After editing, rebuild (`make`) and re-run a scenario. This is how to do
sensitivity sweeps and ablations, such as `HYST_N 1` for no hysteresis or
`CONF_MIN 0` for no fallback.

### 8.5 Change the load
Use the environment variables of `run_experiment.sh`, for example a heavier
batch load:
```bash
sudo N_BATCH=8 BATCH_WORK=4000 ./run_experiment.sh mixed 10 20
```
You can also add a new `scenario_<name>()` function to the script; it is
picked up by name.

---

## 9. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `ctxsched/pending not found, run: sudo ./setup.sh` | WSL was restarted; the cgroups are gone. Run `sudo ./setup.sh`. |
| `Permission denied` writing `cgroup.procs` | Run with `sudo`. |
| Baseline and adaptive numbers are identical | No contention. Make sure the workloads share one core (`taskset -c 0`) and that there are more runnable tasks than cores. |
| `stress-ng not installed` | `sudo ./setup.sh --install-tools` |
| `$'\r': command not found` | A script got Windows line endings. `sed -i 's/\r$//' *.sh` |
| Very wide confidence intervals | WSL2/VM noise or too few reps: use ≥ 10 reps and close other heavy programs. |
| `Device or resource busy` on teardown | A process is still in the group; teardown moves them out first. If it persists, check with `cat /sys/fs/cgroup/ctxsched/*/cgroup.procs`. |

---

## 10. Limitations and design decisions

- **Weights, not quanta.** On EEVDF kernels user space can't set a time
  quantum. Weights give a share-and-deadline bias, not a hard guarantee, which
  is why cyclictest (already served in microseconds) gains little.
- **Opt-in management.** Only processes placed in `ctxsched/` are managed; the
  rest of the system is deliberately left alone.
- **One process, one class.** Classification is per process, with threads
  summed. A process mixing a latency thread and a batch thread gets one class.
  Per-thread classification would need threaded cgroups.
- **Ground truth is a label we chose.** For example, fio counts as
  "interactive" because it is I/O-bound. Accuracy is measured against these
  labels.
- **Residual gaming.** An adversary can pass the guard only by really sleeping
  at least half of the time, which gives up the CPU it was trying to grab.
- **WSL2 is a VM.** Absolute latencies include hypervisor noise. Comparisons
  between modes stay valid because the modes alternate on the same machine.
- **Simulator signals.** `scheduler.c` still derives its signals from each
  task's true parameters. Its numbers are a design study; the `linux/` results
  are the evidence.
