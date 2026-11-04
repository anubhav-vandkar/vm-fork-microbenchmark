# Code structure

Keep the controller small. Group related operations into a few cohesive files;
use focused functions and interfaces to separate responsibilities within them.
A helper does not need its own source/header pair.

| File | Responsibility |
|---|---|
| `main.cpp` | Parse options, check prerequisites and connect the application workflow |
| `benchmark.cpp` | Sequence rounds, child creation, inheritance checks and cleanup |
| `vm.cpp` | Implement both backends and own VM processes and temporary resources |
| `transport.cpp` | Exchange JSON/HTTP/guest messages over bounded Unix sockets |
| `metrics.cpp` | Read and combine process, cgroup and optional KVM measurements |
| `report.cpp` | Record samples, validate saved lifecycles and generate comparison reports |
| `templates/results-dashboard.html` | Present the saved-result dashboard with shared report values |
| `benchmark.hpp` | Share interfaces, data types and small resource ownership types |
| `tests/test.cpp` | Run controller regression tests using a fake backend and local fixtures |

## Boundaries

- The experiment depends on `Backend` and an observer callback. It does not
  select backends, send socket requests, read procfs or format reports.
- Both backend implementations live in `vm.cpp`; their concrete classes stay
  private to that file. `ProcessManager` centralizes pidfd tracking and reaping.
  Firecracker's clone operation owns fresh snapshot preparation and restore;
  a child-ID map retains its backing paths until the child process has stopped.
  Diff mode uses native Firecracker merging into a private cumulative base,
  followed by immutable child reflinks or sparse copies. File extents and fsync
  belong to this VM mechanism; no external merging executable is invoked.
  Failure cleanup removes partial files after process termination, and removes
  the cumulative base before final accounting. The temporary directory is a fallback.
- Protocol clients do not know the round count or measurement schema.
- Measurement readers do not create or destroy VMs. Reporting consumes samples.
- `--report-only` dispatches before host/KVM checks or backend construction. It
  streams saved measurements, checks complete lifecycle ordering and summary
  agreement, and regenerates the comparison, report and dashboard. A separate
  reporting status preserves the historical launcher status and raw inputs.
  Policy-aware analysis counts fresh preparations per child without adding them
  twice, audits diff initial-base/copy metadata, rejects mixed types/reused child
  paths, and preserves legacy full/per-round timing boundaries.
- Descriptors and temporary resources use RAII.
- Boost.JSON definitions are compiled once in `transport.cpp`.
- Tests link into a separate executable. `--self-test` delegates to it.
  The CLI source is also compiled with `VMBENCH_TESTING` to test its options
  without linking a second program entry point.

## Build and validation

The Makefile builds one object per source and tracks header dependencies.
`make -B check` forces both executables to compile and runs the regression suite;
compiler warnings are errors.

Tests cover the 2,500-fork lifecycle and corrupted inheritance, proc and cgroup
readers, KVM PID matching, CSV schema, statistics, CLI parsing, HTTP formatting,
JSON round trips, Unix-socket framing and native process cleanup. They validate
the controller, not real VM performance. A simulated HTTP API exercises the real
Firecracker backend's pause/capture/resume sequence, unique backing retention,
normal teardown, sparse zero/hole semantics, history across rounds, child backing
immutability, dirty tracking on boot and failed-capture/restore cleanup. Saved
fixtures test full/diff/legacy timing and reject invalid diff metadata.

## Retained-chain experiment (Figure 4b)

`--experiment depth` selects `DepthBenchmark` in `benchmark.cpp`; the original
batch orchestration remains separate. The chain controller uses the shared
backend interface's guest commands to populate/check memory and always clones
from the immediate predecessor. It emits first-ready and post-verification
samples, retaining ancestors for final lineage checks and backend-owned cleanup.
`vm.cpp` enables dirty tracking on restored Firecracker depth sources, while
batch leaf children keep their existing tracking policy. No VMM source changes
are needed. Each repetition is a fresh controller/backend process, so snapshot
bases and process lifetimes never carry over between independent chains.

`scripts/run-depth-suite.sh` owns repetitions and selected backends, records
settings/provenance, and optionally uses a separate cgroup scope per chain.
`--depth-report` is a distinct saved-data path in `report.cpp`: it checks ancestry,
PID stability, paired measurement phases, counters, snapshot tracking/timing and
final teardown before aggregating by backend/depth. It writes a portable HTML
table and numeric CSV. It does not reuse the sibling-batch validator or relabel
historical measurements. Ten repetitions provide ten samples per depth, not
640 samples for each depth's p99. Anonymous huge-page observations are explicit;
requesting THP is not taken as evidence of actual guest-VMA coverage.
