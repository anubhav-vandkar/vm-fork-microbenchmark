# Metadata comparison checklist

The first experiment has no added guest memory buffer. Its question is:
**what does creating 50 sibling VMs add, and what remains after deleting them,
repeated over 50 rounds?** Compare the same fields for Lightning and Firecracker.

## Memory and userspace VM metadata — always attempted

| Measurement | Source | What to compare |
|---|---|---|
| Proportional resident memory (PSS) | `/proc/PID/smaps_rollup` | Group footprint at parent-only, 51-VM and post-cleanup phases |
| Resident memory (RSS) | Same | Per-process residency; group sum double-counts shared pages |
| Private dirty / clean pages | Same | Private runtime and naturally modified guest pages |
| Anonymous / file / shmem memory | cgroup `memory.stat` | Distinguish runtime growth from snapshot and output cache |
| Total memory and group peak | `memory.current`, `memory.peak` | Overall charged footprint; includes controller/cache/kernel |
| Swap use | `SwapPss`, `memory.swap.current` | Flag runs affected by swapping |
| VM ID, PID, parent/source, round, child position | Controller | Verify a persistent parent and 50 sibling children each round |
| vCPU/RAM/page configuration | Run summary and options | Ensure matched settings; capacity is not resident memory |
| Host threads and open fd count | `/proc/PID/status`, `/proc/PID/fd` | Detect unexpected runtime growth and remaining resources |
| Clone response / guest readiness | Monotonic host timer; Lightning reply | Keep process-ready and guest-ready boundaries distinct |
| Batch destruction elapsed time | Controller | Time to shut down/reap children and release snapshot backing |
| FC snapshot state bytes | Snapshot file size | Serialized CPU/device state size; includes userspace state, not live KVM allocation bytes |
| FC snapshot RAM logical/allocated bytes | `stat` | Storage/backing cost; actual resident cache can differ |
| Snapshot preparation time and count | Controller | One fresh synchronized full snapshot per child; duration is a component of guest readiness |
| Snapshot policy and unique backing paths | Run summary and per-child metadata | Audit that no snapshot was reused |
| Combined creation time, mean | Sum raw child readiness durations, divide by completed children | Every child timer already includes its preparation; excludes settling, sampling, parent boot and cleanup |

The raw cgroup CPU/IO counters are also saved as context; they include controller
work in a shared group. They do not identify individual kernel objects.

The Firecracker child timer starts before pausing the parent and ends after the
restored guest answers the readiness check. It includes full snapshot creation,
synchronization, parent resume, child process launch and restore. There are 2,500
preparations for 2,500 children. Preparation remains separately observable, but
must not be added again to the child timer. Lightning's timer covers live fork
and readiness. Compute means from raw durations rather than component medians.
At most 50 distinct RAM snapshot files remain live in a batch; file cache and
sharing differ from the historical shared-snapshot workload.

Old saved runs without a policy field are treated as historical per-round reuse:
their combined mean adds the measured round preparations to child durations.
Reports retain this label and never extrapolate fresh-snapshot measurements.

## Minimal KVM metadata — no broad kernel profiling

| Measurement | Source | Caveat |
|---|---|---|
| KVM VM descriptor count and targets | `/proc/PID/fd`, matching `kvm-vm` | Descriptor/reference count; not a byte measurement |
| KVM vCPU descriptor aliases | Same, matching `kvm-vcpu` | Multiple descriptors may reference the same vCPU; do not treat aliases as separate objects |
| KVM MMU/secondary page-table memory | cgroup `memory.stat: sec_pagetables` | Bytes when supported; also includes IOMMU tables, so isolate this group and avoid passthrough devices |
| Per-VM 4 KiB / 2 MiB / 1 GiB mapping counters | `/sys/kernel/debug/kvm/PID-*/pages_*`, when available/readable | Mapping counts/coverage; not page-table allocation bytes |
| KVM MMU cache misses and shadow-page zaps | Per-VM debugfs `mmu_cache_miss`, `mmu_shadow_zapped` | Kernel/version dependent; cumulative counters |
| KVM page-fault and exit counters | Per-VM debugfs `pf_taken`, `pf_fixed`, `exits` | Availability and semantics depend on host kernel; compare deltas/VM lifetimes |

Debugfs is optional. The program does not mount it, change permissions, install
probes or require privilege solely to access it. Missing counters are unavailable,
not zero. Ordinary unprivileged runs still collect process fd metadata and
cgroup counters when readable. KVM debugfs lookup matches the exact PID prefix
to avoid mixing VM counters from unrelated processes.

`sec_pagetables` is the minimal useful kernel-side memory measure. It **does not
cover every KVM allocation**. In particular, this controller cannot report exact
allocated bytes for every `struct kvm`, vCPU structure, memslot, interrupt chip
or device object from standard procfs counters. That would require additional
VMM instrumentation or kernel allocation tracing, beyond this initial test.
The KVM statistics-fd API provides counters, not a universal total-KVM-bytes value.

## Useful comparisons

For each round and backend:

1. `batch_ready PSS − round_before PSS`: added resident group footprint of the batch.
2. `batch_destroyed PSS − round_before PSS`: parent footprint shift after cleanup.
3. Compare group `sec_pagetables` before, during and after the batch, separately
   from user memory. Never add it again to `memory.current`.
4. Check that parent fd/thread counts recover and the next round starts with
   one live VM. Check KVM descriptors on children for unexpected inherited references.
5. Report snapshot preparation and backing costs alongside clone timings, so
   the Firecracker baseline's per-child snapshot cost remains visible.

Use repeated runs and matched host/VM settings before making a percentage or
bytes-per-VM claim. Historical repository measurements and simulated controller
tests are not results for this 2,500-fork experiment.

Sources: [cgroup memory accounting](https://docs.kernel.org/admin-guide/cgroup-v2.html),
[KVM statistics API](https://docs.kernel.org/virt/kvm/api.html#kvm-get-stats-fd), and
the inspected Lightning and Linux KVM source code described in README.md.
