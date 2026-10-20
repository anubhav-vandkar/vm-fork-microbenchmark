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

Production code lives in `src/`, interfaces in `include/vmbench/`, and tests in
`tests/`. `src/benchmark.cpp` owns the experiment; separate Lightning and
Firecracker backend files own their VM mechanisms. Transport, process ownership,
measurement readers and reporting are separate components. The monolithic
`microbench.cpp` has been removed.

## What the two backends do

**Lightning:** fork the live parent through `{"op":"fork"}`. Each child inherits
private guest RAM mappings and rebuilds its own KVM VM/vCPU objects. The code in
the supplied repository explicitly drops inherited KVM owners in children.

**Firecracker:** create one full snapshot from the parent at the start of each
round, resume the parent, then restore 50 independent children from that same
immutable snapshot with separate vsock endpoints. Release the snapshot after
all 50 children have exited. Snapshot preparation time/state-file size/RAM-file
size are recorded separately from child restoration times.

This shared-snapshot baseline allows siblings to share snapshot-backed pages;
it avoids creating 50 unrelated RAM snapshot files for a single identical source.
Snapshot synchronization is explicitly enabled. Full snapshot creation can
populate guest RAM and file cache; resulting footprint differences are part of
the reported mechanism, not necessarily differences in pure metadata size.
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
VMM binaries, and a compatible guest kernel/initramfs. On the machine where
this artifact was prepared, `/dev/kvm` and those built VMM/kernel artifacts were
unavailable. The C++ controller builds and its non-VM checks pass; no VM resource
results are claimed.

## Build the VM binaries and minimal guest on the KVM machine

The expected Lightning checkout is `~/Projects/lightning-main`.
Its `rust-toolchain.toml` pins Rust 1.97.0; use rustup-managed Cargo. Its output
directory is `build/cargo_target/release`.

```sh
cd "$HOME/Projects/lightning-main"
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
  --repo "$HOME/Projects/lightning-main" \
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
    --repo "$HOME/Projects/lightning-main" \
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
