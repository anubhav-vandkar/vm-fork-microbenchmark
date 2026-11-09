#!/usr/bin/env bash
# Figure 4(a) retained siblings and Figure 4(b) retained chains.
set -euo pipefail
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$project"
if [[ "${1:-}" == --report-only ]]; then
    [[ $# -eq 2 ]] || { echo 'Usage: run-depth-suite.sh --report-only RUN_DIRECTORY' >&2; exit 2; }
    exec ./build/vm-fork-bench --figure4-report "$2"
fi
[[ $# -eq 0 ]] || { echo 'Use environment settings documented in README.md.' >&2; exit 2; }
experiment=${VM_BENCH_EXPERIMENT:-depth}
case "$experiment" in
    depth) figure='Figure 4(b)'; default_repetitions=10 ;;
    footprint) figure='Figure 4(a)'; default_repetitions=1 ;;
    *) echo 'VM_BENCH_EXPERIMENT must be depth or footprint.' >&2; exit 2 ;;
esac
depth=${VM_BENCH_DEPTH:-64}
children=${VM_BENCH_CHILDREN:-16}
duration_s=${VM_BENCH_DURATION_S:-60}
repetitions=${VM_BENCH_REPETITIONS:-$default_repetitions}
memory_mib=${VM_BENCH_MEMORY_MIB:-512}
populate_mib=${VM_BENCH_POPULATE_MIB:-128}
vcpus=${VM_BENCH_VCPUS:-1}
settle_ms=${VM_BENCH_SETTLE_MS:-200}
snapshot_type=${VM_BENCH_SNAPSHOT_TYPE:-diff}
read -r -a backends <<< "${VM_BENCH_BACKENDS:-lightning}"
for value in "$depth" "$children" "$duration_s" "$repetitions" "$memory_mib" "$populate_mib" "$vcpus" "$settle_ms"; do
    [[ "$value" =~ ^(0|[1-9][0-9]{0,5})$ ]] || { echo 'Numeric settings must be nonnegative decimal integers.' >&2; exit 2; }
done
((depth >= 1 && depth <= 64 && children >= 1 && children <= 16 && duration_s >= 1 && duration_s <= 60 &&
    repetitions >= 1 && repetitions <= 1000 && memory_mib >= 128 &&
    populate_mib > 0 && populate_mib <= memory_mib - 64 && vcpus >= 1 && vcpus <= 32 && settle_ms <= 10000)) || {
    echo 'Invalid Figure 4 workload setting.' >&2; exit 2;
}
[[ "$snapshot_type" == diff || "$snapshot_type" == full ]] || { echo 'Snapshot type must be full or diff.' >&2; exit 2; }
((${#backends[@]} >= 1 && ${#backends[@]} <= 3)) || { echo 'Choose lightning, lightning-cxl, and/or firecracker.' >&2; exit 2; }
declare -A selected=()
for backend in "${backends[@]}"; do
    [[ "$backend" == lightning || "$backend" == lightning-cxl || "$backend" == firecracker ]] && [[ ! -v selected[$backend] ]] || {
        echo 'Backends must be unique: lightning, lightning-cxl, and/or firecracker.' >&2; exit 2;
    }
    selected[$backend]=1
done
lightning_build=lightning
lightning_source="$project/build/deps/lightning"
cxl_pool=${VM_BENCH_CXL_POOL:-}
cxl_offset=${VM_BENCH_CXL_OFFSET:-}
cxl_standin=${VM_BENCH_CXL_STANDIN:-0}
cxl_args=()
preflight_backend=${backends[0]}
if [[ -v selected[lightning-cxl] ]]; then
    lightning_build=lightning-cxl
    lightning_source="$HOME/Projects/lightning-cxl-lightning"
    preflight_backend=lightning-cxl
    if [[ "$cxl_standin" == 1 && -z "$cxl_pool" && -z "$cxl_offset" ]]; then
        cxl_args=(--cxl-standin)
    elif [[ "$cxl_standin" == 0 && -n "$cxl_pool" && -n "$cxl_offset" ]]; then
        cxl_args=(--cxl-pool "$cxl_pool" --cxl-offset "$cxl_offset")
    else
        echo 'Set VM_BENCH_CXL_POOL and VM_BENCH_CXL_OFFSET for a reserved devdax range, or VM_BENCH_CXL_STANDIN=1.' >&2
        exit 2
    fi
elif [[ -n "$cxl_pool" || -n "$cxl_offset" || "$cxl_standin" != 0 ]]; then
    echo 'CXL settings require lightning-cxl in VM_BENCH_BACKENDS.' >&2
    exit 2
fi
work_dir=${VM_BENCH_WORK_DIR:-"$project/build/w"}
results_dir="$project/results/$experiment"
mkdir -p build/tmp "$work_dir" "$results_dir"
work_dir=$(cd -- "$work_dir" && pwd -P)
export TMPDIR="$project/build/tmp" CCACHE_DISABLE=1
exec 9>build/depth-suite.lock
flock -n 9 || { echo 'Another Figure 4 suite is running.' >&2; exit 1; }
lightning=${VM_BENCH_LIGHTNING:-"$project/build/vmm/$lightning_build/release/lightning"}
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
run_dir=$(mktemp -d "$results_dir/run-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
ln -sfn "${run_dir##*/}" "$results_dir/latest"
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
printf '%s: repetitions=%s, RAM=%s MiB, population=%s MiB, vCPUs=%s, THP requested.\n' \
    "$figure" "$repetitions" "$memory_mib" "$populate_mib" "$vcpus"
if [[ "$experiment" == footprint ]]; then
    printf '%d siblings, %d seconds of sampling. Results: %s\n' "$children" "$duration_s" "$run_dir"
else
    printf 'Depth %d with ancestors retained. Results: %s\n' "$depth" "$run_dir"
fi
backend_json=$(printf '%s\n' "${backends[@]}" | jq -R . | jq -s .)
arguments=(--experiment "$experiment" --depth "$depth" --children "$children" --duration-s "$duration_s"
    --populate-mib "$populate_mib" --memory-mib "$memory_mib" --vcpus "$vcpus"
    --huge-pages Transparent --settle-ms "$settle_ms" --snapshot-type "$snapshot_type" --work-dir "$work_dir"
    --lightning "$lightning" --firecracker "$firecracker" --kernel "$kernel" --initrd "$initrd"
    --lightning-source "${VM_BENCH_LIGHTNING_SOURCE:-$lightning_source}" --firecracker-source "$project/build/deps/firecracker")
./build/vm-fork-bench --preflight --backend "$preflight_backend" "${arguments[@]}" "${cxl_args[@]}" > "$run_dir/preflight.json"
jq -n --arg experiment "$experiment" --argjson backends "$backend_json" --argjson depth "$depth" \
    --argjson children "$children" --argjson duration "$duration_s" --argjson repetitions "$repetitions" \
    --argjson memory "$memory_mib" --argjson populate "$populate_mib" --argjson vcpus "$vcpus" \
    --argjson settle "$settle_ms" --arg snapshot_type "$snapshot_type" --arg work "$work_dir" \
    --slurpfile preflight "$run_dir/preflight.json" \
    '{schema_version:1,experiment:$experiment,backends:$backends,repetitions:$repetitions,
      memory_mib:$memory,populate_mib:$populate,vcpus:$vcpus,huge_pages:"Transparent",settle_ms:$settle,
      snapshot_type:$snapshot_type,work_directory:$work,
      readiness:"first guest reply containing inherited counter and first populated byte"}
      + (if $experiment == "footprint" then
          {children:$children,duration_s:$duration,sample_interval_s:1,
           write_policy:"one byte in each sibling after all siblings are ready, followed by idle sampling",
           memory_policy:"one sample per second per retained child after the private-write checks"}
         else
          {depth:$depth,ancestors_retained:true,
           write_policy:"one byte in first populated page before fork and in child; counter isolation",
           memory_policy:"ready and post-scan/isolation samples; no disk or network workload"}
         end)
      + (if $preflight[0].cxl then {cxl:$preflight[0].cxl} else {} end)' > "$run_dir/settings.json"
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
        if [[ "$backend" == lightning-cxl ]]; then command+=("${cxl_args[@]}"); fi
        if [[ "$cgroup_mode" == isolated_user_scope ]]; then
            command=(systemd-run --user --scope --quiet --unit="vmdepth-${run_dir##*/}-$backend-$name"
                --property=MemoryAccounting=yes "${command[@]}")
        fi
        "${command[@]}"
    done
done
./build/vm-fork-bench --figure4-report "$run_dir"
status=ok
printf '\n%s completed. Dashboard: %s/dashboard.html\n' "$figure" "$run_dir"
