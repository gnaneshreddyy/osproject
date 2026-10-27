#!/usr/bin/env python3
"""Summarize a run_experiment.sh results directory (stdlib only).

usage: python3 analyze.py results/<timestamp>[/<scenario>]

For each scenario directory it prints and writes summary.csv:
  - per-mode metrics (baseline / adaptive / noguard), mean and 95% CI over reps
  - classifier accuracy, overall and per workload source, plus confusion matrix
  - reaction time of the classifier to phase changes
"""
import csv
import glob
import json
import math
import os
import re
import statistics
import sys
from collections import defaultdict

CLASSES = ["batch", "interactive", "latency"]
MODE_ORDER = ["baseline", "adaptive", "noguard"]
WARMUP_S = 1.0          # ignore the first second of each task's samples
FILE_RE = re.compile(r"(baseline|adaptive|noguard)_(\d+)_(.+)$")

# two-sided 95% t quantiles, df = n - 1
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
       8: 2.306, 9: 2.262, 10: 2.228, 15: 2.131, 20: 2.086, 30: 2.042}


def t95(df):
    if df > 30:
        return 1.96
    return T95[max(k for k in T95 if k <= df)]


def mean_ci(xs):
    xs = [x for x in xs if not math.isnan(x)]
    if not xs:
        return float("nan"), float("nan")
    m = statistics.fmean(xs)
    if len(xs) < 2:
        return m, float("nan")
    return m, t95(len(xs) - 1) * statistics.stdev(xs) / math.sqrt(len(xs))


def jain(xs):
    if len(xs) < 2:
        return float("nan")
    return sum(xs) ** 2 / (len(xs) * sum(x * x for x in xs))


def avg(xs):
    return statistics.fmean(xs) if xs else float("nan")


# ------------------------ per-file metric extractors ------------------------

def metrics_workload(path):
    rows = defaultdict(list)
    with open(path) as f:
        for r in csv.reader(f):
            if len(r) != 9:
                continue
            rows[r[0]].append({"n": int(r[2]), "mean": float(r[3]), "p95": float(r[5]),
                               "p99": float(r[6]), "max": float(r[7]), "completion": float(r[8])})
    m = {}
    if rows["latency"]:
        lat = rows["latency"]
        m["Latency wake-up p95 (ms)"] = avg([r["p95"] for r in lat])
        m["Latency wake-up p99 (ms)"] = avg([r["p99"] for r in lat])
        m["Latency wake-up max (ms)"] = max(r["max"] for r in lat)
    if rows["interactive"]:
        m["Interactive response p95 (ms)"] = avg([r["p95"] for r in rows["interactive"]])
        m["Interactive response mean (ms)"] = avg([r["mean"] for r in rows["interactive"]])
    if rows["batch"]:
        m["Batch mean completion (ms)"] = avg([r["completion"] for r in rows["batch"]])
        if len(rows["batch"]) > 1:
            m["Batch fairness (Jain, 1=fair)"] = jain([1.0 / r["completion"] for r in rows["batch"]])
    if rows["batch_timed"]:
        m["Background batch CPU got (ms) [higher=better]"] = avg([r["completion"] for r in rows["batch_timed"]])
    if rows["adversary"]:
        m["Adversary completion (ms)"] = avg([r["completion"] for r in rows["adversary"]])
    for kind in sorted(k for k in rows if k.startswith("phase_")):
        src, dst = kind[len("phase_"):].split("2")
        what = "wake-up" if "latency" in (src, dst) else "response"
        m[f"Phase {src}->{dst}: {what} p95 (ms)"] = avg([r["p95"] for r in rows[kind]])
    return m


def metrics_stressng(path):
    with open(path) as f:
        vals = [float(v) for v in re.findall(r"bogo-ops-per-second-real-time:\s*([\d.]+)", f.read())]
    return {"stress-ng bogo ops/s [higher=better]": sum(vals)} if vals else {}


def metrics_sysbench(path):
    with open(path) as f:
        m = re.search(r"total time:\s*([\d.]+)s", f.read())
    return {"sysbench completion (s)": float(m.group(1))} if m else {}


def metrics_fio(path):
    try:
        with open(path) as f:
            w = json.load(f)["jobs"][0]["write"]
    except (OSError, ValueError, KeyError, IndexError):
        return {}
    pct = w["clat_ns"].get("percentile", {})
    return {"fio IOPS [higher=better]": w["iops"],
            "fio write latency p99 (ms)": pct.get("99.000000", float("nan")) / 1e6}


def metrics_cyclictest(path):
    hist, overflow, mx = [], 0, float("nan")
    with open(path) as f:
        for line in f:
            if re.match(r"^\d+\s+\d+", line):
                us, n = line.split()[:2]
                hist.append((int(us), int(n)))
            elif line.startswith("# Histogram Overflows:"):
                overflow = int(line.split(":")[1].split()[0])
            elif line.startswith("# Max Latencies:"):
                mx = int(line.split(":")[1].split()[0]) / 1000
    total = sum(n for _, n in hist) + overflow
    if not total:
        return {}

    def pct(p):
        acc = 0
        for us, n in hist:
            acc += n
            if acc >= p * total:
                return us / 1000
        return hist[-1][0] / 1000   # in the overflow bucket

    return {"cyclictest p95 (ms)": pct(0.95), "cyclictest p99 (ms)": pct(0.99), "cyclictest max (ms)": mx}


EXTRACTORS = {"wl.csv": metrics_workload, "stressng.yaml": metrics_stressng,
              "sysbench.txt": metrics_sysbench, "fio.json": metrics_fio,
              "cyclictest.txt": metrics_cyclictest}


# ------------------------ classifier ------------------------

def source_of(comm):
    if comm.startswith(("wl_", "ph_", "adv_")):
        return {"wl": "synthetic", "ph": "phase-change", "adv": "adversary"}[comm.split("_")[0]]
    for tool in ("stress-ng", "sysbench", "fio", "cyclictest"):
        if comm.startswith(tool):
            return tool
    return comm


def classifier_report(out, mode):
    conf = defaultdict(lambda: defaultdict(int))
    by_src = defaultdict(lambda: [0, 0])
    raw_ok = dec_ok = fallback = total = 0
    reactions = []
    for path in glob.glob(os.path.join(out, f"{mode}_*_daemon.csv")):
        with open(path) as f:
            rows = [r for r in csv.DictReader(f) if r["truth"] in CLASSES]
        first_seen, first_truth, switched, reacted = {}, {}, {}, set()
        for r in rows:
            pid, t = r["pid"], float(r["t_s"])
            first_seen.setdefault(pid, t)
            first_truth.setdefault(pid, r["truth"])
            # phase change: time from the switch until the decision follows it
            if r["truth"] != first_truth[pid] and pid not in switched:
                switched[pid] = t
            if pid in switched and pid not in reacted and r["decision"] == r["truth"]:
                reactions.append(t - switched[pid])
                reacted.add(pid)
            if t - first_seen[pid] < WARMUP_S:
                continue
            total += 1
            ok = r["decision"] == r["truth"]
            raw_ok += r["raw_class"] == r["truth"]
            dec_ok += ok
            fallback += r["decision"] == "default"
            conf[r["truth"]][r["decision"]] += 1
            src = by_src[source_of(r["comm"])]
            src[0] += ok
            src[1] += 1
    if not total:
        return
    print(f"\n  Classifier ({mode}, {total} samples after {WARMUP_S:.0f}s warm-up): "
          f"raw {raw_ok / total:.1%}, after hysteresis {dec_ok / total:.1%}, "
          f"low-confidence fallback {fallback / total:.1%}")
    print("    by source: " + ", ".join(f"{s} {ok / n:.0%}" for s, (ok, n) in sorted(by_src.items())))
    if reactions:
        print(f"    phase-change reaction time: mean {avg(reactions):.2f}s, max {max(reactions):.2f}s "
              f"({len(reactions)} switches)")
    cols = CLASSES + ["default"]
    print("    " + "true \\ decided".ljust(16) + "".join(f"{c:>13}" for c in cols))
    for t in CLASSES:
        n = sum(conf[t].values())
        if n:
            print(f"    {t:16}" + "".join(f"{100 * conf[t][c] / n:12.1f}%" for c in cols))


# ------------------------ scenario report ------------------------

def analyze(out):
    data = defaultdict(lambda: defaultdict(lambda: defaultdict(float)))   # mode -> rep -> metric
    for path in sorted(glob.glob(os.path.join(out, "*"))):
        m = FILE_RE.match(os.path.basename(path))
        if not m or m.group(3) not in EXTRACTORS:
            continue
        mode, rep, kind = m.groups()
        data[mode][rep].update(EXTRACTORS[kind](path))
    if not data:
        return False

    modes = [m for m in MODE_ORDER if m in data]
    metrics = []
    for mode in modes:
        for rep in data[mode].values():
            metrics += [k for k in rep if k not in metrics]

    reps = len(data["baseline"])
    print(f"\n### {os.path.basename(os.path.normpath(out))}: {reps} repetition(s), mean +/- 95% CI")
    print(f"  {'Metric':46}" + "".join(f"{m:>24}" for m in modes) +
          "".join(f"{'vs base (' + m + ')':>20}" for m in modes[1:]))
    rows = []
    for metric in metrics:
        stats = [mean_ci([r[metric] for r in data[m].values() if metric in r]) for m in modes]
        base = stats[0][0]
        changes = [f"{(s[0] - base) / base * 100:+.1f}%" if base else "-" for s in stats[1:]]
        print(f"  {metric:46}" + "".join(f"{s[0]:13.3f} +/-{s[1]:7.3f}" for s in stats) +
              "".join(f"{c:>20}" for c in changes))
        rows.append([metric] + [v for s in stats for v in s] + changes)

    with open(os.path.join(out, "summary.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["metric"] + [f"{m}_{k}" for m in modes for k in ("mean", "ci95")] +
                   [f"change_{m}" for m in modes[1:]])
        w.writerows(rows)

    for mode in modes:
        classifier_report(out, mode)
    return True


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    top = sys.argv[1]
    dirs = [top] + sorted(d for d in glob.glob(os.path.join(top, "*")) if os.path.isdir(d))
    found = [d for d in dirs if analyze(d)]
    if not found:
        sys.exit(f"no results found in {top}")
    print("\nsummary.csv written to: " + ", ".join(found))


if __name__ == "__main__":
    main()
