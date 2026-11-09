# VM fork microbenchmark — C++

Project location: `~/Projects/vm-fork-microbenchmark`.
See [design principles](PRINCIPLES.md) and [agent build/test rules](agent.md).
The implementation is separated into focused source files and interfaces;
see [architecture and dependency boundaries](ARCHITECTURE.md).

One persistent parent, **50 rounds of 50 children**, created sequentially. Keep
all 50 children alive for the round's group measurement, then destroy all of
them before starting the next round. This is **2,500 clone operations per backend**,
2,501 VM instances including the parent, and at most 51 VMs alive at once.
There is no recursive chain/tree or added memory workload.

```text
boot parent once
repeat 50 rounds:
    record parent baseline
    create child 1, wait for guest response, record metadata
    ...
    create child 50, wait for guest response, record metadata
    record parent + all 50 children
    destroy all 50 children, wait for exit/reaping
    record parent alone again
destroy parent
```

The controller and report generation are C++20. It directly uses Lightning's
newline-delimited JSON control socket, Firecracker's HTTP API over a Unix
socket, the minimal guest's vsock service, procfs and cgroup files. It does not
execute or import a Python controller. Boost.JSON is compiled into the binary.

The source is organized into six production files: `main.cpp` (CLI and startup),
`benchmark.cpp` (rounds), `vm.cpp` (VM lifecycle), `transport.cpp` (socket protocols),
`metrics.cpp` (measurements), and `report.cpp` (output). They share `benchmark.hpp`.
All tests live in `tests/test.cpp`. Related helpers stay with their component;
separation of concerns does not require a separate file for every helper.

## What the two backends do

**Lightning:** fork the live parent through `{"op":"fork"}`. Each child inherits
private guest RAM mappings and rebuilds its own KVM VM/vCPU objects. The code in
the supplied repository explicitly drops inherited KVM owners in children.

**Firecracker:** by default, enable KVM dirty-page tracking on the persistent
parent and request a synchronized `Diff` snapshot before every child. The first
diff of this freshly booted VM establishes a sparse, restorable base: untouched
pages are zero. Later diffs are merged **by Firecracker directly into the same
base RAM file**, using the pinned version's native in-place diff support.
The cumulative base is never mapped by a child. Before each restore, freeze its
contents into a separate child RAM file using a filesystem reflink when supported
or a sparse copy otherwise (including on tmpfs). Each child uses the matching
state file from that capture and its immutable RAM backing until it exits.
Dirty tracking is disabled on restored children, which never produce snapshots.

Fifty rounds of 50 children still create **2,500 snapshots**. Capture/native merge,
child backing copy and synchronization are all inside the per-child preparation
and guest-ready timers. Initial base creation is charged to the first child,
not hidden in parent boot or added again when reporting the combined mean.
Child files are deleted after exit; the cumulative base is deleted at final cleanup.

Use `--snapshot-type full` for the previous full-snapshot-per-child baseline;
the default is `--snapshot-type diff`. Full mode does not enable dirty tracking.
The full mode writes all configured guest RAM on every capture. Diff mode writes
tracked dirty pages and copies the allocated base extents when reflink is unavailable.
Sparse files retain the full logical guest size, so allocated bytes, sharing and
resident memory must be measured separately. If most memory is written, diff
storage can approach full mode; no fixed savings are assumed.
Upstream Firecracker has no equivalent live-fork endpoint.

Both paths use the repo's minimal PID 1/vsock guest, with one vCPU and 256 MiB
configured guest capacity by default. **Capacity is not physical memory use.**
No `ALLOC` command is sent. A tiny round counter checks inheritance and source
liveness; ordinary guest/kernel activity still causes private writes.
Both VMMs run with `--no-seccomp`, as required by this Lightning implementation.

## Build locally

Requires Linux, a C++20 compiler, Make and Boost.JSON headers (Boost 1.75+).
The build does not download dependencies.

```sh
# Fedora, if dependencies are missing:
sudo dnf install gcc-c++ make boost-devel

# Ubuntu/Debian alternative:
sudo apt install g++ make libboost-json-dev

make
make check
./build/vm-fork-bench --preflight
```

The built controller is `build/vm-fork-bench`. The separate test executable is
`build/vm-fork-bench-tests`; `--self-test` delegates to it. The tests validate a full
2,500-operation simulated lifecycle, the 50-child barrier, peak count, retained
parent, cleanup ordering, proc parsing, a real local process-memory read, JSON
parsing and native process cleanup. It also attempts Unix-socket JSON framing;
an explicit `SKIP` is printed if the execution sandbox denies socket sends.
It **does not create KVM VMs**.

Actual VM runs require x86_64 Linux with read/write access to `/dev/kvm`, built
VMM binaries, and a compatible guest kernel/initramfs. Controller tests include
a local simulated Firecracker API; they do not execute KVM VMs. Historical runs
using one shared snapshot per round do not validate or measure this new policy.

## Repeat the full suite with project-local dependencies

On a Linux x86_64 host, `scripts/prepare-local.sh LIGHTNING_SOURCE [FIRECRACKER_SOURCE]`
prepares Rust 1.97.0, Lightning, an independent Firecracker baseline,
a Linux 6.1.128 guest kernel and the minimal initramfs. Downloads, compiler
caches, sources and outputs remain under this project. It needs the native build
tools listed below, plus Clang/LLVM tools, jq and flock. It does not use sudo or
modify shell profiles. The guest kernel uses Clang and disables the system
trusted keyring to avoid the old kernel's OpenSSL ENGINE build dependency.
The final kernel config and dependency versions are saved with the run.

```sh
./scripts/prepare-local.sh --check-sources "$HOME/lightning" "$HOME/firecracker"
./scripts/prepare-local.sh "$HOME/lightning" "$HOME/firecracker"
./scripts/run-full-suite.sh
```

Both source directories are configurable. Alternatively set `LIGHTNING_SOURCE`
and `FIRECRACKER_SOURCE` when invoking `prepare-local.sh`; positional paths take
precedence. Lightning defaults to `~/lightning`. Omitting the Firecracker path
downloads upstream commit `fd9fc7a9f164b73002d22065d17c150d8c262d91`.
The sources are copied into `build/deps` and built into `build/vmm` without
modifying your checkouts. The runner uses these prepared copies and binaries;
rerun preparation after changing a source path or checkout. Source paths,
commits, copied source hashes and binary hashes are recorded for reproducibility.
Use a compatible independent Firecracker checkout with the APIs described below.

For direct controller runs, `--lightning-source DIR` and `--firecracker-source DIR`
select separate source trees (defaults `~/lightning` and `~/firecracker`).
`--repo` remains an alias for `--lightning-source`. Their default binaries are
`DIR/build/cargo_target/release/lightning` and
`DIR/build/cargo_target/release/firecracker`; use `--lightning BIN` and
`--firecracker BIN` for other build locations. The controller does not build sources.

The runner performs 50 rounds of 50 children for each backend, sequentially,
with 256 MiB configured RAM, one vCPU and a 200 ms settling delay. Change
`memory_mib` in `scripts/run-full-suite.sh` to adjust the capacity for both
backends, preflight and saved settings. The controller's standalone default
is also 256 MiB. At 256 MiB, the 50 child backing files represent 12.5 GiB of logical
RAM files per batch, plus a 256 MiB logical cumulative base in diff mode. Actual
allocation depends on written pages and reflink/sparse-copy behavior. These
logical sizes are not an estimate of physical disk usage or resident RAM.
The runner verifies
2,500 completions per backend and clean teardown. It uses a separate user
systemd scope per backend when available, and records shared-cgroup fallback
explicitly. Results live under `results/full-suite/run-*`, with `latest` pointing
to the latest attempt. Temporary sockets and snapshots stay under `build/w`.
Paths should remain short enough for Unix sockets; use a similarly short project
path on the server. `make clean` removes the prepared dependencies too.

Select the previous full mode or a different temporary filesystem without editing
the runner:

```sh
VM_BENCH_SNAPSHOT_TYPE=full ./scripts/run-full-suite.sh
mkdir -m 700 -p "/dev/shm/vmbench-$UID"
VM_BENCH_WORK_DIR="/dev/shm/vmbench-$UID" ./scripts/run-full-suite.sh
```

`VM_BENCH_SNAPSHOT_TYPE=diff` is the runner default. The chosen type and absolute
work directory are recorded in preflight and settings. Existing tmpfs can use swap;
use a `noswap` tmpfs or a suitable enforced swap limit when snapshot RAM must not
reach disk. Keep paths short enough for Unix sockets. Runtime directories are
removed automatically, while the user-created parent work directory remains empty.

Diff mode uses only the existing Firecracker binary and controller; no external
snapshot editor is needed. Native in-place merging is required by this implementation
and is present in the pinned upstream baseline. A different Firecracker checkout
must support this behavior (see the pinned
[native merge implementation](https://github.com/firecracker-microvm/firecracker/blob/fd9fc7a9f164b73002d22065d17c150d8c262d91/src/vmm/src/vstate/vm.rs#L594)).
Diff support is marked developer preview upstream:
validate actual KVM runs on the target host before relying on performance results.
Historical full and per-round measurements remain labeled with their original policy.

Regenerate just the final report and metadata dashboard from an existing run:

```sh
./scripts/run-full-suite.sh --report-only results/full-suite/latest
```

This starts no VMs and requires no KVM access. It validates both raw lifecycle
streams and their summary statistics, then writes `comparison.json`, `report.md`,
`dashboard.html` and `reporting-status.json` in that run directory. Historical
launcher status and raw measurements stay intact. New full runs invoke this
reporting step automatically after both backend runs pass validation.
New reports require one unique snapshot preparation per Firecracker child.
Old runs without a policy field remain labeled as historical per-round reuse;
their durations are never multiplied to estimate fresh-snapshot results.

## Build the VM binaries and minimal guest on the KVM machine

The default Lightning checkout is `~/lightning`; source paths are configurable.
Its `rust-toolchain.toml` pins Rust 1.97.0; use rustup-managed Cargo. Its output
directory is `build/cargo_target/release`.

```sh
cd "$HOME/lightning"
cargo build --locked --release -p lightning
tools/lightning/build-guest.sh /tmp/lt-meta-guest
```

The guest build needs GCC, static libc development support, cpio and gzip.
On Fedora these include `gcc glibc-static cpio gzip`; on Ubuntu/Debian use
`build-essential cpio gzip`. Rust builds may also require CMake and standard
native build tools for dependencies.

Use the same compatible x86_64 `vmlinux` for both backends, with initramfs,
serial console and virtio-vsock support. The repository offers this recipe
when you have a dedicated Linux 6.1 source tree:

```sh
tools/lightning/build-device-kernel.sh /absolute/path/to/linux-6.1-source /tmp/lt-vmlinux
```

That helper replaces `.config` in the supplied kernel source tree. Kernel builds
also need Make, GCC, flex, bison, bc, ELF and OpenSSL development dependencies.

For an unmodified generic Firecracker baseline, use a separate checkout at the
upstream base declared in Lightning's README:

```sh
cd "$HOME/Projects"
git clone https://github.com/firecracker-microvm/firecracker.git firecracker-baseline
cd firecracker-baseline
git checkout fd9fc7a9f164b73002d22065d17c150d8c262d91
cargo build --locked --release -p firecracker
```

A Firecracker binary built inside the Lightning checkout shares its modified
VMM library, so use `--firecracker` to point to the independent baseline binary.
The selected baseline must support `vsock_override` and `sync_snapshot_files`
in its snapshot APIs. Record binary/kernel/initrd checksums alongside the run:

```sh
sha256sum /absolute/path/to/lightning /absolute/path/to/firecracker \
  /tmp/lt-vmlinux /tmp/lt-meta-guest/initramfs.cpio.gz > binaries.sha256
```

## Smoke test and full run

From this benchmark folder on the KVM machine, first try two rounds of two
children with each backend. Use a different, new output directory for each run.

```sh
./build/vm-fork-bench --backend lightning \
  --lightning-source "$HOME/lightning" \
  --kernel /tmp/lt-vmlinux --initrd /tmp/lt-meta-guest/initramfs.cpio.gz \
  --rounds 2 --forks-per-round 2 --output results/lightning-smoke

./build/vm-fork-bench --backend firecracker \
  --firecracker "$HOME/Projects/firecracker-baseline/build/cargo_target/release/firecracker" \
  --kernel /tmp/lt-vmlinux --initrd /tmp/lt-meta-guest/initramfs.cpio.gz \
  --rounds 2 --forks-per-round 2 --output results/firecracker-smoke
```

For the full measurement, run each backend separately in a fresh cgroup v2.
This systemd example runs Lightning first and Firecracker second, each with
its own memory-accounting group. Change the unit/output prefix on repeat runs.

```sh
LT_BENCH="$(pwd)/build/vm-fork-bench"
LT_OUT="$(pwd)/results/run01"
for backend in lightning firecracker; do
  sudo systemd-run --wait --pipe --collect \
    --unit="lt-metadata-run01-${backend}" \
    --uid="$(id -u)" --gid="$(id -g)" \
    --property=MemoryAccounting=yes \
    "$LT_BENCH" --backend "$backend" \
    --lightning-source "$HOME/lightning" \
    --firecracker "$HOME/Projects/firecracker-baseline/build/cargo_target/release/firecracker" \
    --kernel /tmp/lt-vmlinux --initrd /tmp/lt-meta-guest/initramfs.cpio.gz \
    --rounds 50 --forks-per-round 50 --memory-mib 256 --vcpus 1 \
    --output "$LT_OUT/$backend"
done
```

The service user needs KVM group access. A direct invocation also works, but
cgroup totals from a shared desktop/session group include unrelated processes.
If automatic cgroup discovery does not match your environment, supply
`--cgroup-dir /sys/fs/cgroup/your-run-group`; this only chooses what to read,
it does not move processes or create the group.

`--settle-ms 200` is applied between child readiness and sampling, and after
batch deletion. It is excluded from the per-child clone timer. Sampling and
debugfs reads can perturb subsequent operations; treat this as an instrumented
footprint microbenchmark. Fixed delays do not prove that every deferred kernel
free has completed. Increase the delay in a separate matched run if needed.
With the default delay alone, the full test takes over eight minutes per backend,
plus cloning, measurement and destruction time.

Use the same CPU/NUMA placement, guest kernel, VM capacity, vCPU count and page
policy for both backends. Default page policy is `None`; if testing transparent
huge pages, pass `--huge-pages Transparent` to both in a separate matched run.
`--work-dir /tmp` contains temporary sockets and FC snapshots. Record whether
that directory is on disk or tmpfs, since this changes snapshot cache behavior.
The default config uses no disks or network interfaces.

## Outputs

- `samples.jsonl`: raw measurements, per-process metadata, preparation/clone/
  cleanup timings and optional KVM counters.
- `samples.csv`: compact series keyed by round, child position and total fork
  count. Use `batch_ready` rows for the complete 51-VM group and
  `batch_destroyed` rows for the parent-only post-cleanup footprint.
- `summary.json`: status, counts, root/maximum batch/final parent PSS, readiness
  median/p95, run settings, host/kernel information, mounts and cleanup errors.
- Top-level process logs. Lightning's child console logs are temporary and
  are removed by its graceful cleanup.

The controller terminates and reaps tracked VMMs on success, failure or handled
interrupts, uses pidfds for process identity, and does not retain descriptors
for all 2,500 exited children. In a failed/ambiguous Lightning fork handshake,
the VMM performs its own best-effort cleanup; inspect the error and temporary
work location rather than assuming an exactly-once fork response.

## Interpretation and minimal kernel scope

See [METADATA.md](METADATA.md) for the exact comparison fields. The primary
process-memory number is summed **PSS**, which apportions shared resident pages.
Summed RSS counts shared pages repeatedly. Kernel measurements are deliberately
limited to secondary page-table accounting and optional KVM debugfs counters;
there is no slab scan, BPF, perf recording or general kernel profiling.

Process PSS excludes the controller. Cgroup totals include the controller and
charged output/log cache as well as VMMs and snapshots; use the pre-boot baseline
and the separate `anon`/`file` fields, and do not label all retained cache as a
VM leak. A small readiness-timing vector stays in the controller for final
percentiles. Stable post-cleanup values across rounds indicate bounded footprint;
growth needs investigation, not an automatic leak diagnosis.

PSS and cgroup totals are alternative views with different scopes; do not add
them together. `sec_pagetables` is already part of charged total memory. KVM
debugfs page counts describe mappings, not the byte size of all KVM metadata.
Guest/kernel writes, VMM heap, snapshot file cache and KVM objects all contribute
to observed memory; this test does not isolate pure `struct kvm` bytes.

## Implementation sources

Reviewed Lightning files: `README.md`, `src/lightning/src/live.rs`,
`src/lightning/src/main.rs`, `tools/lightning/bench.py`,
`tools/lightning/guest.c`, `benchmarks/lightning/INDEPENDENT_LIFETIMES.md`.
The local checkout is a source archive without a `.git` directory. Older
benchmark reports may describe superseded lifecycle restrictions.

- [Linux cgroup v2 documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html)
- [KVM API and statistics](https://docs.kernel.org/virt/kvm/api.html)
- [Boost.JSON header-only build](https://www.boost.org/doc/libs/1_89_0/libs/json/doc/html/index.html#json.overview.header_only)

## Figure 4(a): child private memory over time

The Figure 4 runner also supports a retained cohort of **16 siblings from one
parent**. Each child verifies the inherited buffer and writes one byte after
all children are ready. The controller verifies the parent's state and each
child's distinct marker, lets the cohort settle, then samples every child once
per second for 60 seconds. The parent and all 16 children stay alive through
the final isolation check. This controlled write followed by idle sampling is
the initial workload while the agent workload is TBD.

Use the same prepared VMMs, guest images, CXL device settings and tracing access
as for the depth experiment below. For the three-backend comparison, run from
the project directory with `pool_device` and `pool_offset` set to your reserved
devdax range:

```sh
VM_BENCH_EXPERIMENT=footprint \
VM_BENCH_BACKENDS="lightning lightning-cxl firecracker" \
VM_BENCH_CXL_POOL="$pool_device" VM_BENCH_CXL_OFFSET="$pool_offset" \
  ./scripts/run-depth-suite.sh
```

Defaults are one cohort per backend, 512 MiB RAM, 128 MiB populated and one
vCPU with Transparent huge pages requested. Set `VM_BENCH_CHILDREN=2` and
`VM_BENCH_DURATION_S=3` for a short smoke test. For Firecracker, each sibling
uses a synchronized snapshot of the root with its own backing. Restored
siblings can use ordinary memory without dirty tracking because they are
leaves in this experiment.

Read `results/footprint/latest/footprint.csv` for each child's private memory,
PSS, anonymous huge pages and CXL mapping counters when applicable. The CSV
includes actual elapsed nanoseconds and scheduled seconds. The dashboard
summarizes per-child values at each second. The reporter checks sample
coverage, stable cohort membership and teardown. Regenerate with
`./scripts/run-depth-suite.sh --report-only results/footprint/latest`.

## Figure 4(b): latency and private memory versus fork depth

Both Figure 4 modes support Lightning with anonymous DRAM, Lightning with its
CXL pool, and Firecracker snapshot/restore. The CXL path uses the same-host
fork mechanism in the updated Lightning source. Figure 3 has a separate
fan-out and readiness protocol.

This is a separate experiment from the 50-sibling batch suite. Each independent
chain forks its newest VM, retains every ancestor, and ends at depth 64 (65 live
VMs including the original parent). The runner tears down the entire chain
before starting the next repetition. Default pilot: **Lightning only, 512 MiB
RAM, 128 MiB populated, 1 vCPU, Transparent huge pages, 10 repetitions**.
These are explicit pilot settings, not the 8-vCPU setup of Figure 3.

For a fresh Ubuntu server checkout with Lightning sources at `~/lightning`,
install prerequisites and prepare both VMMs and the guest once:

```sh
sudo apt-get update
sudo apt-get install -y build-essential git curl ca-certificates pkg-config cmake \
  clang llvm lld flex bison bc cpio xz-utils libssl-dev libelf-dev zlib1g-dev \
  libseccomp-dev libboost-json-dev jq util-linux
./scripts/prepare-local.sh "$HOME/lightning"
```

Preparation downloads the pinned independent Firecracker source automatically,
builds both VMMs and the guest, and forces controller compilation and self-tests.
Allow it to finish successfully; compiler logs are under `results/full-suite/`.
Then run a small smoke test before attempting depth 64:

```sh
VM_BENCH_BACKENDS="lightning firecracker" \
VM_BENCH_DEPTH=4 VM_BENCH_REPETITIONS=1 \
VM_BENCH_MEMORY_MIB=512 VM_BENCH_POPULATE_MIB=128 \
VM_BENCH_VCPUS=1 VM_BENCH_SNAPSHOT_TYPE=diff \
  ./scripts/run-depth-suite.sh
cat results/depth/latest/runner-status.json
jq '.chains[] | {backend, status, final_live_vms}' results/depth/latest/comparison.json
```

This creates four forks per backend, runs the backends sequentially, and retains
at most five VMs at once. Success requires runner/chain status `ok` and zero final
live VMs. The dashboard is `results/depth/latest/dashboard.html`.

With dependencies already prepared, run from this project directory:

```sh
mkdir -p build/tmp
TMPDIR="$PWD/build/tmp" CCACHE_DISABLE=1 make -B check
./scripts/run-depth-suite.sh
```

The script must run from a normal Linux host terminal with KVM access. It uses
prepared VMMs under `build/vmm`, the existing guest kernel/initramfs under
`build/guest`, and project-local temporary files under `build/w`. The DRAM and
Firecracker paths run without a CXL device. Preparation builds the VMMs separately.
The guest must implement `ALLOC`, `SET`, `WRITE`, `GET`, and `SCAN` as in the
prepared minimal guest; an initial checksum check rejects incompatible guests.
Build prerequisites remain Make, a C++20 compiler and Boost.JSON. Building the
VMM dependencies through `prepare-local.sh` also requires `libseccomp-dev` on
Ubuntu, in addition to the other native build tools; this is a build-time
requirement even when execution uses `--no-seccomp`.

Configuration is explicit and independent from `run-full-suite.sh`:

```sh
# Match a larger VM / CPU configuration after the default pilot works.
VM_BENCH_MEMORY_MIB=4096 VM_BENCH_POPULATE_MIB=1024 VM_BENCH_VCPUS=8 \
  ./scripts/run-depth-suite.sh

# Optional matching Firecracker baseline, run after Lightning (not concurrently).
VM_BENCH_BACKENDS="lightning firecracker" ./scripts/run-depth-suite.sh
```

### Lightning with a CXL pool

With the normal preparation artifacts and Rust dependencies already cached,
build the CXL revision from the local source export. Run these commands in the
benchmark project:

```sh
source_dir="$HOME/Projects/lightning-cxl-lightning"
CARGO_HOME="$PWD/build/deps/cargo" RUSTUP_HOME="$PWD/build/deps/rustup" \
RUSTUP_TOOLCHAIN=1.97.0 CARGO_TARGET_DIR="$PWD/build/vmm/lightning-cxl" \
TMPDIR="$PWD/build/tmp" CCACHE_DISABLE=1 \
  "$PWD/build/deps/cargo/bin/cargo" build --offline --locked --release -j 4 \
  --manifest-path "$source_dir/Cargo.toml" --config "$source_dir/.cargo/config.toml" -p lightning
```

Run with KVM access and permission to use the `kvm:kvm_page_fault` tracepoint
through tracefs and perf (the upstream workflow runs as root). For hardware,
set `pool_device` to a verified CXL devdax device and `pool_offset` to the
start of an exclusively reserved range, in bytes. The device must have 2 MiB
alignment. Offset and guest size must be multiples of 2 MiB.
**Lightning zeroes the configured range at every root boot.**
Reserve at least the configured guest RAM size. Lightning's range lock
coordinates Lightning instances on the same host, so keep other users of the
range out of it.

```sh
VM_BENCH_BACKENDS=lightning-cxl \
VM_BENCH_CXL_POOL="$pool_device" VM_BENCH_CXL_OFFSET="$pool_offset" \
VM_BENCH_DEPTH=64 VM_BENCH_REPETITIONS=10 \
  ./scripts/run-depth-suite.sh
```

For a protocol smoke test with a private temporary file, use
`VM_BENCH_BACKENDS=lightning-cxl VM_BENCH_CXL_STANDIN=1 VM_BENCH_DEPTH=4 VM_BENCH_REPETITIONS=1 ./scripts/run-depth-suite.sh`.
The report labels it a file stand-in. It still needs KVM and tracing access.
Selecting both `lightning` and `lightning-cxl` uses the same CXL-capable
binary for both cases. Override its path with `VM_BENCH_LIGHTNING` and its
source record with `VM_BENCH_LIGHTNING_SOURCE` if needed.

The controller verifies writable pool backing before the first fork,
full-size first freeze, zero newly frozen bytes at later depths, and a
nonempty frozen mapping in each descendant. It records Lightning's freeze
time and per-VM frozen and DRAM overlay bytes alongside the existing
inheritance and isolation checks. These counters describe mappings. An
overlay inherited by another fork can still be physically shared. The
status requests run outside fork and guest-ready timing.

Other settings: `VM_BENCH_DEPTH` (1–64), `VM_BENCH_REPETITIONS` (1–1000),
`VM_BENCH_SETTLE_MS` (default 200), `VM_BENCH_SNAPSHOT_TYPE` (`diff` or `full`),
`VM_BENCH_WORK_DIR`, `VM_BENCH_LIGHTNING`, `VM_BENCH_FIRECRACKER`,
`VM_BENCH_KERNEL`, and `VM_BENCH_INITRD`. Memory/population sizes are in MiB.
THP is requested, never enabled globally by this script. A 1-generation/1-repeat
smoke test is `VM_BENCH_DEPTH=1 VM_BENCH_REPETITIONS=1 ./scripts/run-depth-suite.sh`.

The original parent populates one buffer by writing one marker per 4 KiB page.
Before each fork the source changes a counter and one byte in the first
populated page. The child must immediately serve that inherited counter and byte.
The timer starts before the clone and ends at this first guest reply. A subsequent
scan checks the complete buffer's marker checksum; the child then changes its
own counter and first byte, and the source must retain its old values. These
checks, settling, metadata reads, boot, initial population and final cleanup are
outside creation timing. Every retained ancestor's expected marker is checked
again at the end. This is a controlled memory workload, not an agent, network or
disk-readiness experiment.

Two memory phases are recorded at every depth: **ready** immediately after the
first guest reply, and **verified** after the scan, isolation check and settling.
The primary per-child private footprint is `Private_Dirty + Private_Clean`
(userspace USS). PSS is reported separately and contains a proportional share of
shared pages. Both include VMM userspace; neither is a total-KVM-bytes metric.
All original process memory fields and minimal KVM measurements remain in raw
samples. Population and source writes affect both methods equally; guest/kernel
activity and huge-page copying/splitting can also affect private memory.

`AnonHugePages` is recorded for each process. This is evidence of process-level
anonymous huge-page backing, not exact guest-VMA coverage. A zero/missing value
must not be described as verified THP backing. Firecracker's file-backed restored
memory may have different backing from Lightning's inherited anonymous memory;
check the observations before making a matched-THP or general performance claim.

In Firecracker diff mode, **every restored descendant has dirty tracking enabled**
because it will become the next source. Each generation snapshots its immediate
parent and updates the private cumulative base, then receives an immutable
reflink/sparse copy. This is a *materialized snapshot ancestry chain*, not a
claim that restores traverse 64 delta files. Initial base, native merging, copy
and synchronization are charged inside creation time. Full mode remains
selectable. All 64 child backing files remain until teardown: disk usage is
not capped and must be budgeted before enabling Firecracker. For 512 MiB full
snapshots the retained child files alone total 32 GiB; sparse/reflink allocations
in diff mode depend on the actual populated data and filesystem.

Outputs are under `results/depth/run-*`; `results/depth/latest` points to the
latest attempt. Each `BACKEND/rep-NNN/` contains raw JSONL/CSV, process logs and
a summary. The suite also records settings, binary hashes, preflight, launcher
status, and creates `dashboard.html`, `report.md`, `comparison.json`, and the
plot-ready `depth.csv`. One sample per repetition/depth contributes to the
aggregate. With ten repetitions the nearest-rank p99 equals the maximum; it is
not a reliable tail estimate. Failed/incomplete chains are rejected by reporting.

Regenerate reports without running VMs:

```sh
./scripts/run-depth-suite.sh --report-only results/depth/latest
```

The dashboard is a portable table of measurements; the CSV preserves numeric
precision for subsequent plotting of actual KVM results. Simulated tests and
preflight output are not empirical latency or memory results.
