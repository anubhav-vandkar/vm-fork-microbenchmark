# Benchmark design principles

1. **Control the workload.** Both backends use the same minimal guest, kernel,
   vCPU count, configured RAM, page policy and readiness check. There is no added
   memory workload in this initial experiment.

2. **Make the lifecycle explicit.** One parent stays alive. Each round creates
   50 siblings sequentially, retains all 50 for a group measurement, then destroys
   and reaps them. Fifty rounds give 2,500 clone operations per backend, with at
   most 51 VMs alive. The next round cannot start until cleanup finishes.

3. **Compare documented mechanisms.** Lightning performs live process forks and
   reconstructs independent KVM objects. Firecracker defaults to KVM dirty-page
   diffs, natively updating a private cumulative base before each child and
   restoring from an immutable reflink or sparse copy. Full mode remains selectable.
   Preparation is inside the child's creation timer and also reported as a
   component, including native merging, copying and synchronization. Retain child
   backing until exit, delete the cumulative base at final cleanup, and expose
   allocated bytes and copy strategy without treating logical size as residency.

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
   For one combined creation number, sum child readiness durations and divide by
   completed children: every Firecracker duration already includes preparation.
   Do not add preparation twice or add component medians. Historical per-round
   runs retain their original timing boundaries and policy labels. Do not call
   the creation mean total suite wall time.

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
