#!/usr/bin/env bash
# Prepare the cgroup v2 hierarchy used by ctxschedd and build the binaries.
#
#   sudo ./setup.sh                  check kernel, create cgroups, build
#   sudo ./setup.sh --install-tools  also apt-install stress-ng sysbench fio rt-tests
#   sudo ./setup.sh --teardown       move leftover tasks out and remove the cgroups
set -euo pipefail
cd "$(dirname "$0")"

CG=/sys/fs/cgroup/ctxsched
CLASSES="pending batch interactive latency"

if [[ $EUID -ne 0 ]]; then
    echo "setup.sh: run with sudo" >&2
    exit 1
fi

if [[ "${1:-}" == "--teardown" ]]; then
    for c in $CLASSES; do
        [[ -d $CG/$c ]] || continue
        while read -r pid; do
            echo "$pid" > /sys/fs/cgroup/cgroup.procs 2>/dev/null || true
        done < "$CG/$c/cgroup.procs"
        rmdir "$CG/$c"
    done
    [[ -d $CG ]] && rmdir "$CG"
    echo "removed $CG"
    exit 0
fi

# ---- checks ----
fs=$(stat -fc %T /sys/fs/cgroup)
if [[ $fs != cgroup2fs ]]; then
    echo "setup.sh: /sys/fs/cgroup is $fs, need cgroup v2 (cgroup2fs)" >&2
    exit 1
fi
if ! grep -qw cpu /sys/fs/cgroup/cgroup.controllers; then
    echo "setup.sh: cpu controller not available in cgroup v2" >&2
    exit 1
fi
if [[ ! -r /proc/self/schedstat ]]; then
    echo "setup.sh: /proc/<pid>/schedstat missing (kernel built without CONFIG_SCHED_INFO)" >&2
    exit 1
fi

kver=$(uname -r)
kmaj=${kver%%.*}; rest=${kver#*.}; kmin=${rest%%.*}
if (( kmaj > 6 || (kmaj == 6 && kmin >= 6) )); then
    sched="EEVDF (no fixed quantum: classes map to cpu.weight)"
else
    sched="CFS (classes map to cpu.weight)"
fi
echo "kernel $kver, scheduler: $sched, CPUs: $(nproc)"
grep -qi microsoft /proc/version && echo "running under WSL2 (Hyper-V VM): expect some timing noise"

if [[ "${1:-}" == "--install-tools" ]]; then
    apt-get update
    apt-get install -y build-essential stress-ng sysbench fio rt-tests python3
fi

# ---- cgroups ----
grep -qw cpu /sys/fs/cgroup/cgroup.subtree_control || echo "+cpu" > /sys/fs/cgroup/cgroup.subtree_control
mkdir -p "$CG"
echo "+cpu" > "$CG/cgroup.subtree_control"
for c in $CLASSES; do
    mkdir -p "$CG/$c"
done
echo "cgroups ready: $(ls -d $CG/*/ | xargs -n1 basename | tr '\n' ' ')"

# ---- build ----
make -s
echo "built ./ctxschedd and ./workload"
