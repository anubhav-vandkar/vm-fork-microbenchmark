#!/usr/bin/env bash
# Figure 4(b): independent retained chains; no source builds or VMM configuration changes.
set -euo pipefail
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$project"
if [[ "${1:-}" == --report-only ]]; then
    [[ $# -eq 2 ]] || { echo 'Usage: run-depth-suite.sh --report-only RUN_DIRECTORY' >&2; exit 2; }
    exec ./build/vm-fork-bench --depth-report "$2"
fi
[[ $# -eq 0 ]] || { echo 'Use environment settings documented in README.md.' >&2; exit 2; }
depth=${VM_BENCH_DEPTH:-64}
repetitions=${VM_BENCH_REPETITIONS:-10}
memory_mib=${VM_BENCH_MEMORY_MIB:-512}
populate_mib=${VM_BENCH_POPULATE_MIB:-128}
vcpus=${VM_BENCH_VCPUS:-1}
settle_ms=${VM_BENCH_SETTLE_MS:-200}
snapshot_type=${VM_BENCH_SNAPSHOT_TYPE:-diff}
read -r -a backends <<< "${VM_BENCH_BACKENDS:-lightning}"
for value in "$depth" "$repetitions" "$memory_mib" "$populate_mib" "$vcpus" "$settle_ms"; do
    [[ "$value" =~ ^(0|[1-9][0-9]{0,5})$ ]] || { echo 'Numeric settings must be nonnegative decimal integers.' >&2; exit 2; }
done
((depth >= 1 && depth <= 64 && repetitions >= 1 && repetitions <= 1000 && memory_mib >= 128 &&
    populate_mib > 0 && populate_mib <= memory_mib - 64 && vcpus >= 1 && vcpus <= 32 && settle_ms <= 10000)) || {
    echo 'Invalid depth, repetition, memory, population, CPU or settling setting.' >&2; exit 2;
}
[[ "$snapshot_type" == diff || "$snapshot_type" == full ]] || { echo 'Snapshot type must be full or diff.' >&2; exit 2; }
((${#backends[@]} >= 1 && ${#backends[@]} <= 2)) || { echo 'Choose lightning, firecracker, or both.' >&2; exit 2; }
declare -A selected=()
for backend in "${backends[@]}"; do
    [[ "$backend" == lightning || "$backend" == firecracker ]] && [[ ! -v selected[$backend] ]] || {
        echo 'Backends must be unique: lightning and/or firecracker.' >&2; exit 2;
    }
    selected[$backend]=1
done
work_dir=${VM_BENCH_WORK_DIR:-"$project/build/w"}
mkdir -p build/tmp "$work_dir" results/depth
work_dir=$(cd -- "$work_dir" && pwd -P)
export TMPDIR="$project/build/tmp" CCACHE_DISABLE=1
exec 9>build/depth-suite.lock
flock -n 9 || { echo 'Another depth suite is running.' >&2; exit 1; }
lightning=${VM_BENCH_LIGHTNING:-"$project/build/vmm/lightning/release/lightning"}
firecracker=${VM_BENCH_FIRECRACKER:-"$project/build/vmm/firecracker/release/firecracker"}
kernel=${VM_BENCH_KERNEL:-"$project/build/guest/vmlinux"}
initrd=${VM_BENCH_INITRD:-"$project/build/guest/initramfs.cpio.gz"}
for backend in "${backends[@]}"; do
    binary=$lightning
    if [[ "$backend" == firecracker ]]; then binary=$firecracker; fi
    [[ -x "$binary" ]] || { echo "Missing $backend executable: $binary. Run preparation first." >&2; exit 2; }
done
[[ -s "$kernel" && -s "$initrd" ]] || { echo 'Guest kernel/initramfs missing; run preparation first.' >&2; exit 2; }
make check
run_dir=$(mktemp -d "$project/results/depth/run-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
ln -sfn "${run_dir##*/}" results/depth/latest
exec > >(tee -a "$run_dir/runner.log") 2>&1
status=running
finish() {
    exit_code=$?
    if [[ "$status" == running ]]; then status=failed; fi
    jq -n --arg status "$status" --argjson exit_code "$exit_code" \
        '{status:$status,exit_code:$exit_code}' > "$run_dir/runner-status.json"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'Figure 4(b): depth=%s, repetitions=%s, RAM=%s MiB, population=%s MiB, vCPUs=%s; THP requested.\n' \
    "$depth" "$repetitions" "$memory_mib" "$populate_mib" "$vcpus"
printf 'Ancestors retained: maximum %d VMs per chain. Results: %s\n' "$((depth + 1))" "$run_dir"
backend_json=$(printf '%s\n' "${backends[@]}" | jq -R . | jq -s .)
jq -n --argjson backends "$backend_json" --argjson depth "$depth" --argjson repetitions "$repetitions" \
    --argjson memory "$memory_mib" --argjson populate "$populate_mib" --argjson vcpus "$vcpus" \
    --argjson settle "$settle_ms" --arg snapshot_type "$snapshot_type" --arg work "$work_dir" \
    '{schema_version:1,experiment:"depth",backends:$backends,depth:$depth,repetitions:$repetitions,
      memory_mib:$memory,populate_mib:$populate,vcpus:$vcpus,huge_pages:"Transparent",settle_ms:$settle,
      snapshot_type:$snapshot_type,work_directory:$work,ancestors_retained:true,
      write_policy:"one byte in first populated page before fork and in child; counter isolation",
      readiness:"first guest reply containing inherited counter and first populated byte",
      memory_policy:"ready and post-scan/isolation samples; no disk or network workload"}' > "$run_dir/settings.json"
arguments=(--experiment depth --depth "$depth" --populate-mib "$populate_mib" --memory-mib "$memory_mib" --vcpus "$vcpus"
    --huge-pages Transparent --settle-ms "$settle_ms" --snapshot-type "$snapshot_type" --work-dir "$work_dir"
    --lightning "$lightning" --firecracker "$firecracker" --kernel "$kernel" --initrd "$initrd"
    --lightning-source "$project/build/deps/lightning" --firecracker-source "$project/build/deps/firecracker")
./build/vm-fork-bench --preflight "${arguments[@]}" > "$run_dir/preflight.json"
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    status=blocked_kvm
    echo 'KVM unavailable here. Run this script in a normal KVM-enabled host terminal.' >&2
    exit 2
fi
sha256sum ./build/vm-fork-bench "$kernel" "$initrd" > "$run_dir/binaries.sha256"
for backend in "${backends[@]}"; do
    binary=$lightning
    if [[ "$backend" == firecracker ]]; then binary=$firecracker; fi
    sha256sum "$binary" >> "$run_dir/binaries.sha256"
done
cgroup_mode=shared
if command -v systemd-run >/dev/null && systemctl --user show-environment >/dev/null 2>&1; then cgroup_mode=isolated_user_scope; fi
printf '%s\n' "$cgroup_mode" > "$run_dir/cgroup-mode.txt"
for backend in "${backends[@]}"; do
    mkdir -p "$run_dir/$backend"
    for ((repetition = 1; repetition <= repetitions; repetition++)); do
        printf -v name 'rep-%03d' "$repetition"
        output="$run_dir/$backend/$name"
        printf '\nStarting %s %s/%s\n' "$backend" "$repetition" "$repetitions"
        command=("$project/build/vm-fork-bench" "${arguments[@]}" --backend "$backend" --output "$output")
        if [[ "$cgroup_mode" == isolated_user_scope ]]; then
            command=(systemd-run --user --scope --quiet --unit="vmdepth-${run_dir##*/}-$backend-$name"
                --property=MemoryAccounting=yes "${command[@]}")
        fi
        "${command[@]}"
    done
done
./build/vm-fork-bench --depth-report "$run_dir"
status=ok
printf '\nFigure 4(b) completed. Dashboard: %s/dashboard.html\n' "$run_dir"
