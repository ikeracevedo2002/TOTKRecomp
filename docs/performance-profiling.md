# Performance profiling

Set `SWITCHRECOMP_PROFILE=1` to emit lightweight timing and lift-cache counters
on stderr. The normal execution JSON and guest accounting are unchanged.

The execution-session lift cache is scoped to a session generation. Entries are
validated by the finalized function-map object and a deterministic CFG
fingerprint (module/address lookup remains the outer key). A changed CFG
therefore invalidates only its entry; immutable records are reused for later
execution slices. `ExecutionSessionResult::performance` exposes hit, miss,
invalidations, and lift counters to library callers and focused tests.

Function-map construction also reports its elapsed time when profiling is
enabled. Refinement publishes immutable process maps by sharing unchanged
module records and rebuilding only touched modules. The rebuilt module passes
the previous frozen map to `FunctionMapBuilder`, which retains decoded/finalized
records until a newly introduced callable boundary invalidates their ownership
or boundary dependency. The publication coordinator still reconstructs the
process entry index; that is the remaining map-level reconstruction cost.

The refinement driver first assesses the currently pending candidates against
one map generation. Structurally independent candidates are grouped by target
module and candidate-owned ranges, each touched module is built once, and the
coordinator publishes one deterministic batch. A failed batch is retried as
stable singleton candidates so batching cannot change semantic acceptance.
Candidate accounting and resource budgets charge each assessment and the
aggregate analysis work exactly once, while the map-rebuild counter charges one
publication per successful batch.

Independent assessment can use a bounded worker pool:

```text
build/run-entry --analysis-workers 1 ...
build/run-entry --analysis-workers 4 ...
```

`1` is the serial reference behavior. The default is the lesser of four and
the host-reported hardware concurrency. Workers receive immutable memory,
process-image, and process-map inputs and write fixed-index result slots; the
coordinator merges those slots in input order. Worker count is emitted only in
the diagnostic profile stream, not in the stable JSON report.

The optional stderr stream additionally marks refinement wall time, rounds and
transactions, target classification, IR verification, interpreter boundary
execution, report generation, and total run wall time. It reports rebuilds,
functions analyzed/lifted, candidate assessments, assessment/publication time,
batch width, avoided rebuilds, and incremental reuse. The stable report exposes
the `batching` and `incremental_reuse` objects; worker completion timing and
host CPU details remain diagnostic only.

The batch profile fields are:

```text
batch_count, batch_candidates, singleton_batches, average_batch_width,
max_batch_width, rebuilds_avoided,
finalized_functions_reused, cfgs_reused, functions_rebuilt,
modules_touched, incremental_updates, full_rebuilds
```

Reuse invalidation is conservative: a module is rebuilt when a promoted target
belongs to it; a function record is invalidated when a newly introduced strong
entry intersects its precise ownership or recorded boundary dependency; all
other module records remain shared. No persistent disk cache is used by this
milestone.
