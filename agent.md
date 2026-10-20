# Project instructions

## Completion checks

Before marking any C++ code change complete:

1. Run `make -B check` from this project directory. This must force compilation
   and run the controller self-tests, rather than merely reuse an old binary.
2. Resolve compilation errors, enabled compiler warnings and test failures before
   reporting the code change as complete.
3. Report the build/test outcome and any skipped checks. A skipped check is not
   a passing check.
4. Distinguish controller compilation and simulated lifecycle tests from actual
   Lightning/Firecracker VM runs. Do not claim VM validation or measured resource
   results without executing the test on a KVM-enabled host.

The intended workload is one persistent parent, 50 rounds of 50 sequentially
created children, with all 50 children retained for measurement and destroyed
before the next round. There is no added guest memory buffer.

Keep kernel-side collection limited to KVM-related accounting and counters unless
the user explicitly requests broader profiling.

## Separation of concerns

- Prefer a small number of cohesive files. The current layout has six production
  `.cpp` files, one shared header and one test file. Do not create a separate
  source/header pair for each helper.
- Keep each function focused on one named operation. Compose workflows from
  these operations, grouping related helpers with their component.
- Keep experiment orchestration independent of backend selection, socket
  transport, process ownership, measurement collection and output formatting.
- Implement Lightning and Firecracker behind the common `Backend` interface.
- Keep the fake backend and all tests in `tests/`, outside the production binary.
- Use RAII for owned descriptors and resources. Keep process cleanup bookkeeping
  in `ProcessManager` rather than duplicating it in the backends.
- Update `ARCHITECTURE.md` when responsibility boundaries change. Avoid new
  catch-all utility files or combining unrelated responsibilities for convenience.
