# Full-range slice regression

Release validation at `f1face395bfe9511a81fa2bb8c53decd3bb9b4a0`
found `radon_pooled` vectorization on/off ratios of 1.0792 and 1.0826
on Linux x86_64/GCC 14.2.1. All three numerical points agreed exactly.
The on graph retained one full-range active `SLICE` over 12,573 values;
the off graph had no slice. The previous main artifact (`d73a3583`,
[run 35730246633](https://github.com/seantalts/stanli/actions/runs/35730246633))
had no slice in either graph. This followed the compiler's stability changes,
which disabled partial evaluation; those numerical protections remain enabled.

Extend the existing full-extent copy elision to identity slices. Eligibility
requires offset zero, equal lengths, one output writer, no extra metadata,
no external roots, and a source read only by this slice and never subsequently
written. Existing opaque-consumer guards remain. The source adjoint therefore
starts at zero: forwarding consumers preserves their accumulation sequence.
An unconditional lowering alias was rejected because shared source adjoints
could regroup additions. Seven focused cases cover acceptance, partial ranges,
shared sources, roots, source/output rewrites, and disabling the pass.

Release Clang on macOS arm64, current source plus this patch, eight alternating
process runs per model (off/on/on/off twice), with a fresh confirmation of
the baseline failure:

| Model | Before on/off | After on/off |
| --- | ---: | ---: |
| radon_pooled | 1.1007 (confirmation 1.1049) | 0.9986 |
| radon_county | — | 0.8437 |
| normal_mixture | — | 0.9939 |

Radon pooled changed from 56.73/51.54 µs to 51.30/51.37 µs (on/off).
Both final graphs have eight operations. `test_inplace` passed; the focused
A/B had nine passing semantic points and no timing, graph, or infrastructure
failures. Broader integration and Linux validation remain the release gate.

Raw evidence: `/tmp/stanli-0171-vectorize` (Linux failure),
`/tmp/stanli-vectorize-prior-d73` (prior Linux),
`/tmp/stanli-0171-radon-before`, `/tmp/stanli-0171-radon-after` (local A/B),
and `/tmp/stanli-0171-identity-slice.patch` (measured implementation).
