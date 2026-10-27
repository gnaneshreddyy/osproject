#!/usr/bin/env bash
# Baseline vs context-aware on the real kernel, across several scenarios.
#
#   sudo ./run_experiment.sh [scenario] [reps] [duration_s]
#
# Scenarios (default: mixed; "all" runs every one):
#   mixed        synthetic batch + interactive + latency tasks
#   phase        tasks that switch behaviour halfway (batch->latency,
#                interactive->batch) over steady background load
#   adversarial  batch jobs that nap briefly between bursts to look
#                latency-sensitive; also runs the daemon with --no-guard
#   tools        real benchmarks: stress-ng, sysbench (batch), fio (I/O),
#                cyclictest (latency), plus one synthetic interactive task
#
# Every task is pinned to one core so they actually compete for CPU (on an
# idle multi-core box weights change nothing). Modes alternate within each
# repetition so slow drift on the host affects all of them equally:
#   baseline  daemon in --observe mode: everything stays in ctxsched/pending
#   adaptive  daemon moves tasks to batch/interactive/latency cgroups
#   noguard   adaptive without the latency-rule guard (adversarial only)
#
# Tunables (env): CORE=0 DAEMON_CORE=1 N_BATCH=4 N_INT=3 N_LAT=3
#                 BATCH_WORK=3000 SYSBENCH_EVENTS=20000
set -euo pipefail
cd "$(dirname "$0")"

SCENARIO=${1:-mixed}
REPS=${2:-5}
DUR=${3:-20}
CORE=${CORE:-0}
DAEMON_CORE=${DAEMON_CORE:-1}
N_BATCH=${N_BATCH:-4}
N_INT=${N_INT:-3}
N_LAT=${N_LAT:-3}
BATCH_WORK=${BATCH_WORK:-3000}
SYSBENCH_EVENTS=${SYSBENCH_EVENTS:-20000}
CG=/sys/fs/cgroup/ctxsched

case $SCENARIO in
    all)                          SCENARIOS="mixed phase adversarial tools" ;;
    mixed|phase|adversarial|tools) SCENARIOS=$SCENARIO ;;
    *) echo "unknown scenario: $SCENARIO (mixed|phase|adversarial|tools|all)" >&2; exit 2 ;;
esac

if [[ $EUID -ne 0 ]]; then
    echo "run_experiment.sh: run with sudo" >&2
    exit 1
fi
if [[ $SCENARIOS == *tools* ]]; then
    for t in stress-ng sysbench fio cyclictest; do
        command -v "$t" >/dev/null || { echo "$t not installed: sudo ./setup.sh --install-tools" >&2; exit 1; }
    done
fi
[[ -d $CG/pending ]] || ./setup.sh
make -s

TOP=results/$(date +%Y%m%d_%H%M%S)
mkdir -p "$TOP"
cat > "$TOP/config.txt" <<EOF
kernel=$(uname -r)
scenarios=$SCENARIOS reps=$REPS duration_s=$DUR core=$CORE daemon_core=$DAEMON_CORE
n_batch=$N_BATCH n_interactive=$N_INT n_latency=$N_LAT batch_work_ms=$BATCH_WORK sysbench_events=$SYSBENCH_EVENTS
EOF

trap 'kill $(jobs -p) 2>/dev/null || true' EXIT

# Run a command inside ctxsched/pending, pinned to the contended core.
in_cg() {
    bash -c 'echo $$ > "$0/pending/cgroup.procs" && exec taskset -c "$1" "${@:2}"' "$CG" "$CORE" "$@"
}

pids=()
bg() { "$@" & pids+=($!); }

# ---- scenarios: each launches its tasks with bg; $1 = output prefix, $2 = rep ----

scenario_mixed() {
    local out=$1 rep=$2 i
    for i in $(seq 1 "$N_BATCH"); do bg in_cg ./workload batch --work "$BATCH_WORK" --seed $((rep*100+i)) >> "${out}_wl.csv"; done
    for i in $(seq 1 "$N_INT");   do bg in_cg ./workload interactive --duration "$DUR" --seed $((rep*100+10+i)) >> "${out}_wl.csv"; done
    for i in $(seq 1 "$N_LAT");   do bg in_cg ./workload latency --duration "$DUR" --seed $((rep*100+20+i)) >> "${out}_wl.csv"; done
}

scenario_phase() {
    local out=$1 rep=$2 half i
    half=$(awk "BEGIN{print $DUR/2}")
    for i in 1 2 3; do bg in_cg ./workload batch --duration "$DUR" >> "${out}_wl.csv"; done
    for i in 1 2; do
        bg in_cg ./workload phase --from batch --to latency --after "$half" --duration "$DUR" --seed $((rep*100+i)) >> "${out}_wl.csv"
        bg in_cg ./workload phase --from interactive --to batch --after "$half" --duration "$DUR" --seed $((rep*100+10+i)) >> "${out}_wl.csv"
    done
    bg in_cg ./workload latency --duration "$DUR" --seed $((rep*100+20)) >> "${out}_wl.csv"
}

scenario_adversarial() {
    local out=$1 rep=$2 i
    for i in 1 2; do bg in_cg ./workload adversary --work "$BATCH_WORK" >> "${out}_wl.csv"; done
    for i in 1 2; do bg in_cg ./workload batch --duration "$DUR" >> "${out}_wl.csv"; done
    for i in 1 2; do bg in_cg ./workload interactive --duration "$DUR" --seed $((rep*100+10+i)) >> "${out}_wl.csv"; done
    for i in 1 2 3; do bg in_cg ./workload latency --duration "$DUR" --seed $((rep*100+20+i)) >> "${out}_wl.csv"; done
}

scenario_tools() {
    local out=$1 rep=$2
    bg in_cg stress-ng --cpu 2 --timeout "${DUR}s" --metrics-brief --yaml "${out}_stressng.yaml" >/dev/null 2>&1
    bg in_cg sysbench cpu --threads=1 --events="$SYSBENCH_EVENTS" --time=0 run > "${out}_sysbench.txt"
    bg in_cg fio --name=io --thread --rw=randwrite --bs=4k --size=64M --direct=1 --fsync=1 \
        --runtime="$DUR" --time_based --filename=/tmp/ctxsched_fio.dat \
        --output-format=json --output="${out}_fio.json" >/dev/null
    bg in_cg cyclictest -m -t1 -i 1000 -D "$DUR" -q -h 20000 > "${out}_cyclictest.txt"
    bg in_cg ./workload interactive --duration "$DUR" --seed $((rep*100+10)) >> "${out}_wl.csv"
}

# ---- main loop ----

for scen in $SCENARIOS; do
    OUT=$TOP/$scen
    mkdir -p "$OUT"
    modes="baseline adaptive"
    [[ $scen == adversarial ]] && modes="baseline adaptive noguard"

    for rep in $(seq 1 "$REPS"); do
        for mode in $modes; do
            echo "== $scen: rep $rep/$REPS, $mode =="
            case $mode in
                baseline) flags=(--observe) ;;
                adaptive) flags=() ;;
                noguard)  flags=(--no-guard) ;;
            esac
            prefix="$OUT/${mode}_${rep}"

            taskset -c "$DAEMON_CORE" ./ctxschedd "${flags[@]}" --log "${prefix}_daemon.csv" 2>"${prefix}_daemon.err" &
            dpid=$!
            sleep 0.3

            pids=()
            "scenario_$scen" "$prefix" "$rep"
            wait "${pids[@]}" || true

            kill -INT "$dpid"
            wait "$dpid" || true
        done
    done
done
rm -f /tmp/ctxsched_fio.dat

echo
python3 analyze.py "$TOP"
