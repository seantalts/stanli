# Runtime indexing in compiled callbacks

Register programs now handle fixed-shape matrix and nested-array selections
whose indices are computed during execution. They reuse the existing dynamic
gather and update kernels, including duplicate indices and last-write-wins
assignments. The simpler scalar array cases keep their existing instructions.
Integer-array writes in runtime loops now stay in registers too.

The proof requires known container dimensions and selection lengths. Runtime
indices can change the elements selected, but cannot change the output shape.
Runtime-length ranges still fall back. Loop writes invalidate preparation-time
integer facts before compiling the body, so a later iteration cannot reuse an
obsolete value. Assignment evaluates the right side before replacing the base,
preserving aliased gathers and updates. No execution engine or opcode was added.

Validation passed all 304 CTest tests and all 329 recorded corpus models
(1,020,194 values and 124 same-platform ULP gates). The new ODE fixture compares
39 independent CmdStan values at 0 ULP. Weighted callback values and gradients
match both interpreter paths bitwise for repeated gathers, repeated writes,
and aliased assignments. Invalid indices reject in both paths. The existing
corpus cancellation exceptions are unchanged.

Six alternating baseline/candidate pairs used a Release native build on Darwin
arm64. The baseline is bf3ab5d1. Checksums agree exactly between versions.

| Model / version | Preparation µs | First gradient µs | Native warm gradient µs | Inference ms |
| --- | ---: | ---: | ---: | ---: |
| Runtime indexing, before | 422.8 | 424.7 | 310.03 | 405.23 |
| Runtime indexing, after | 492.7 | 67.1 | 21.94 | 23.33 |
| Branch-return canary, before | 318.3 | 39.8 | 1.91 | 3.14 |
| Branch-return canary, after | 314.8 | 37.8 | 1.92 | 3.27 |

The target gains about 14× in warm gradients and 17× in short inference,
at a preparation cost of about 70 µs. The canary is roughly unchanged; these
short timings do not establish an improvement for it. Target process peak RSS
was 29.53 MB before and 29.07 MB after. This is not retained model memory.
The runtime library grew by 848 bytes uncompressed.

Raw samples, medians, dispersion, source/output timings, and binary hashes are
in `2026-09-28-runtime-index-performance.json`. Reproduction scripts and logs
are retained in `/tmp/stanli-runtime-index`.
