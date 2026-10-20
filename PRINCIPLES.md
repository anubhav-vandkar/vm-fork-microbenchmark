# Benchmark design principles

1. **Control the workload.** Both backends use the same minimal guest, kernel,
   vCPU count, configured RAM, page policy and readiness check. There is no added
   memory workload in this initial experiment.

2. **Make the lifecycle explicit.** One parent stays alive. Each round creates
   50 siblings sequentially, retains all 50 for a group measurement, then destroys
   and reaps them. Fifty rounds give 2,500 clone operations per backend, with at
   most 51 VMs alive. The next round cannot start until cleanup finishes.

3. **Compare documented mechanisms.** Lightning performs live process forks and
   reconstructs independent KVM objects. Firecracker restores 50 children from
   one shared full snapshot per round. Snapshot preparation is measured separately
   from each child's restoration, and its memory/storage cost remains visible.

4. **Measure shared memory correctly.** Use summed PSS as the primary resident
   process-group footprint. RSS can double-count shared pages. Record private
   pages as well. Configured guest capacity is not actual resident memory.

5. **Measure creation and recovery.** Sample parent-only memory, the full batch,
   and parent-only memory after cleanup. Compare those phases within each round,
   then look for drift across rounds. Cache retention or deferred freeing is not
   automatically a VM leak.

6. **Keep timing boundaries explicit.** Use a monotonic host clock. Distinguish
   fork/API return, guest readiness, snapshot preparation and destruction time.
   Collection and intentional settling are outside the per-child creation timer,
   although instrumentation can still perturb subsequent operations.

7. **Keep kernel inspection focused.** Collect KVM VM/vCPU descriptor information,
   secondary page-table bytes and optional readable per-VM MMU/fault/exit counters.
   Do not equate descriptor aliases with independent objects or claim these give
   the exact size of every internal KVM allocation.

8. **Make scope and evidence visible.** Process PSS excludes the controller;
   cgroup totals include charged controller/log/cache memory. Record settings and
   raw samples. Keep simulated checks, skipped checks and real VM measurements
   clearly separate. Recompile and test code before reporting completion.

9. **Separate responsibilities.** The experiment owns round order, backends own
   VM mechanisms, transports own protocol exchanges, readers own measurement
   collection and writers own serialization. Use interfaces to keep those
   concerns independently testable, and RAII to centralize resource ownership.
   Group related helpers in cohesive modules; separation does not require one
   file per helper. See `ARCHITECTURE.md` for the source layout and boundaries.
