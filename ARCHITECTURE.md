# Code structure and responsibility boundaries

Each implementation file owns one cohesive concern. Functions express one named
operation; workflows compose those operations rather than embedding transport,
process, measurement and formatting code in the same function.

## Project layout

```text
include/vmbench/   public interfaces and small ownership/data types
src/              production implementations
tests/            fake backend and independent regression tests
build/            objects, dependency files and the two executables
```

| Concern | Implementation | Responsibility |
|---|---|---|
| Program entry | `src/main.cpp` | Delegate to the application |
| Application workflow | `src/application.cpp` | Dispatch commands and connect run/cleanup/report phases |
| Command-line configuration | `src/options.cpp` | Parse, validate and resolve options |
| Runtime prerequisites | `src/preflight.cpp` | Inspect host and validate a requested real VM run |
| Benchmark experiment | `src/benchmark.cpp` | Sequence rounds, children, state checks and observer events |
| Backend selection | `src/backend_factory.cpp` | Construct the requested backend |
| Shared backend lifecycle | `src/managed_backend.cpp` | Guest readiness, round marker and tracked cleanup |
| Lightning mechanism | `src/lightning_backend.cpp` | Live-fork VM lifecycle |
| Firecracker mechanism | `src/firecracker_backend.cpp` | Snapshot/restore VM lifecycle and snapshot backing |
| Matched guest settings | `src/guest_config.cpp` | Build the common guest configuration |
| Native process ownership | `src/process_manager.cpp` | Launch, pidfd tracking, wait/reap, stop and release resources |
| Unix transport | `src/unix_socket.cpp` | Bounded socket connection, sending and response reads |
| Lightning protocol | `src/lightning_client.cpp` | Newline-delimited JSON request/response validation |
| Firecracker protocol | `src/firecracker_client.cpp` | HTTP request formatting and API response validation |
| Guest protocol | `src/guest_client.cpp` | Vsock handshake, guest command and readiness retry |
| Per-process readings | `src/proc_reader.cpp` | Proc parsing, process descriptors and memory aggregation |
| Cgroup readings | `src/cgroup_reader.cpp` | Memory group discovery and selected accounting fields |
| KVM readings | `src/kvm_reader.cpp` | Read optional per-VM debugfs counters with exact PID matching |
| Sample composition | `src/sample_collector.cpp` | Combine independent readers into one sample |
| Event recording | `src/recorder.cpp` | Attach event context and route samples to output/statistics |
| CSV serialization | `src/csv_writer.cpp` | Preserve the compact CSV schema |
| Summary statistics | `src/statistics.cpp` | Track PSS summaries and readiness percentiles |
| Support concerns | `src/files.cpp`, `src/json.cpp`, `src/timing.cpp`, `src/signals.cpp`, `src/temporary_directory.cpp` | Each file handles only its named support concern |

## Dependency rules

- The benchmark depends on the `Backend` interface and an observer callback. It
  does not select backends, send socket requests, inspect procfs or write files.
- Backend implementations translate the interface into their own VM mechanism.
  The benchmark has no `if lightning / else firecracker` execution branch.
- Protocol clients do not know the experiment's round count or sample schema.
- Process ownership is centralized. Backends do not duplicate pidfd and reaping
  bookkeeping. `FileDescriptor` releases its descriptor through RAII.
- Measurement readers do not create/destroy VMs or write reports.
- Reporting consumes samples. Statistics can be tested without procfs or KVM.
- The fake backend and test runner are linked into `vm-fork-bench-tests`, not the
  production `vm-fork-bench`. The existing `--self-test` option executes that
  separate sibling test binary for command compatibility.
- Boost.JSON definitions are compiled once in `src/json.cpp`.

## Build and validation

The Makefile compiles each source to its own object and emits compiler dependency
files, so header changes rebuild affected objects. `make -B check` forces both
executables to compile and runs the regression suite. Compiler warnings are errors.

Tests cover the 2,500-fork lifecycle/barrier and corrupted inheritance, proc and
cgroup readers, KVM PID matching, CSV schema, statistics, CLI parsing, HTTP
formatting, JSON round trips, optional socket framing and native process cleanup.
These establish controller behavior; they do not establish real VM performance.
