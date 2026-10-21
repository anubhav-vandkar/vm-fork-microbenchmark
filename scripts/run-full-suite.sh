#!/usr/bin/env bash
set -euo pipefail

verify_backend() {
    local backend=$1 directory=$2
    jq -e '.status == "ok" and (.cleanup_errors | length == 0) and
        .result.forks_completed == 2500 and .result.rounds_completed == 50 and
        .result.maximum_live_vms == 51' "$directory/$backend/summary.json" >/dev/null || return 1
    awk -F, '
        NR > 1 {
            if ($1 == "child_ready") { children++; if ($5 != $3 + 1 || $4 != ($2 - 1) * 50 + $3) bad++ }
            if ($1 == "batch_ready") { batches++; if ($5 != 51) bad++ }
            if ($1 == "batch_destroyed") { destroyed++; if ($5 != 1) bad++ }
            last_phase=$1; last_live=$5
        }
        END { exit !(children == 2500 && batches == 50 && destroyed == 50 && bad == 0 && NR == 2654 && last_phase == "all_stopped" && last_live == 0) }
    ' "$directory/$backend/samples.csv"
}

main() {
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$project"
if [[ "${1:-}" == --report-only ]]; then
    [[ "$#" -eq 2 ]] || { echo 'Usage: run-full-suite.sh --report-only RUN_DIRECTORY' >&2; return 2; }
    exec "$project/build/vm-fork-bench" --report-only "$2"
fi
if [[ "${1:-}" == --verify-only ]]; then
    [[ "$#" -eq 2 ]] || { echo 'Usage: run-full-suite.sh --verify-only RUN_DIRECTORY' >&2; return 2; }
    for backend in lightning firecracker; do verify_backend "$backend" "$2"; done
    echo 'Verified both saved VM runs: 2,500 child creations each, 50 batches, clean final teardown.'
    return 0
fi
mkdir -p build/tmp build/w results/full-suite
export TMPDIR="$project/build/tmp" CCACHE_DISABLE=1
exec 9>build/full-suite.lock
flock -n 9 || { echo 'Another full-suite run is active.' >&2; exit 1; }
run_dir=$(mktemp -d "$project/results/full-suite/run-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
ln -sfn "$(basename "$run_dir")" results/full-suite/latest
exec > >(tee -a "$run_dir/runner.log") 2>&1
status=running
finish() {
    exit_code=$?
    if [[ "$status" == running ]]; then status=failed; fi
    printf '{"status":"%s","exit_code":%d,"rounds":50,"children_per_round":50,"planned_forks_per_backend":2500}\n' \
        "$status" "$exit_code" > "$run_dir/runner-status.json"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'Results: %s\n' "$run_dir"
printf 'Workload: 50 rounds x 50 children per backend; maximum 51 live VMs.\n'
printf 'Firecracker: fresh synchronized full snapshot before every child; preparation included in creation timing.\n'
lightning="$project/build/vmm/lightning/release/lightning"
firecracker="$project/build/vmm/firecracker/release/firecracker"
kernel="$project/build/guest/vmlinux"
initrd="$project/build/guest/initramfs.cpio.gz"
for executable in "$lightning" "$firecracker"; do
    [[ -x "$executable" ]] || { status=missing_dependencies; echo "Missing executable: $executable"; exit 2; }
done
for image in "$kernel" "$initrd"; do
    [[ -s "$image" ]] || { status=missing_dependencies; echo "Missing guest image: $image"; exit 2; }
done
make check
./build/vm-fork-bench --preflight --lightning "$lightning" --firecracker "$firecracker" \
    --lightning-source "$project/build/deps/lightning" --firecracker-source "$project/build/deps/firecracker" \
    --kernel "$kernel" --initrd "$initrd" --rounds 50 --forks-per-round 50 > "$run_dir/preflight.json"
if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
    status=blocked_kvm
    echo 'KVM unavailable in this session. Run this script from a normal host terminal with /dev/kvm access.'
    exit 2
fi
sha256sum build/vm-fork-bench "$lightning" "$firecracker" "$kernel" "$initrd" > "$run_dir/binaries.sha256"
cp build/guest/kernel.config "$run_dir/kernel.config"
cp results/full-suite/dependencies.json "$run_dir/dependencies.json"
cp results/full-suite/sources.sha256 "$run_dir/sources.sha256"
cgroup_mode=shared
if command -v systemd-run >/dev/null && systemctl --user show-environment >/dev/null 2>&1; then
    cgroup_mode=isolated_user_scope
fi
printf '{"cgroup_mode":"%s","work_directory":"build/w","memory_mib":256,"vcpus":1,"settle_ms":200,"huge_pages":"None","firecracker_snapshot_policy":"per-fork"}\n' \
    "$cgroup_mode" > "$run_dir/settings.json"
if [[ "$cgroup_mode" == shared ]]; then
    echo 'Using a shared cgroup: process PSS remains useful; group totals may include unrelated activity.'
fi
failures=0
for backend in lightning firecracker; do
    command=("$project/build/vm-fork-bench" --backend "$backend"
        --lightning-source "$project/build/deps/lightning" --firecracker-source "$project/build/deps/firecracker"
        --lightning "$lightning" --firecracker "$firecracker"
        --kernel "$kernel" --initrd "$initrd" --memory-mib 256 --vcpus 1 --huge-pages None
        --rounds 50 --forks-per-round 50 --settle-ms 200 --work-dir "$project/build/w"
        --output "$run_dir/$backend")
    printf '\nStarting %s\n' "$backend"
    runner=("${command[@]}")
    if [[ "$cgroup_mode" == isolated_user_scope ]]; then
        runner=(systemd-run --user --scope --quiet --unit="vmbench-${run_dir##*/}-$backend"
            --property=MemoryAccounting=yes "${command[@]}")
    fi
    if ! "${runner[@]}"; then
        echo "$backend failed; preserving its output and continuing with the other backend."
        failures=$((failures + 1))
        continue
    fi
    if ! verify_backend "$backend" "$run_dir"; then
        echo "$backend did not satisfy the completion checks."
        failures=$((failures + 1))
        continue
    fi
    jq '{backend,status,result,metrics}' "$run_dir/$backend/summary.json"
done
if [[ "$failures" -gt 0 ]]; then
    status=failed
    echo "$failures backend runs failed; inspect their summary and logs."
    exit 1
fi

"$project/build/vm-fork-bench" --report-only "$run_dir"
status=ok
printf '\nBoth complete: 2,500 child creations per backend. Results: %s\n' "$run_dir"
}

main "$@"; exit "$?"
