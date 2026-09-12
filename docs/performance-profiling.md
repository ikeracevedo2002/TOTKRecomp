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
enabled. The current refinement implementation reuses frozen maps for modules
that are not the target module and uses the existing persistent function reuse
path for the rebuilt module; map finalization and ownership normalization are
still reconstructed.
