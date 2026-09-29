# Machine-readable evidence

These files moved from the top level of `docs/` without changing their bytes.
They are inputs to validation or historical measurements, not introductory
reading. Do not edit recorded values/provenance to match a new implementation.
Embedded paths describe the original run and may refer to earlier locations.

| Files | Contents and maintained reader |
| --- | --- |
| `corpus-refs.json.gz` | Independent CmdStan model references; `tools/verify_refs.py` reads them and `tools/verify_sample.py` records reviewed updates. |
| `corpus-refs-linux-x86_64.json.gz`, `corpus-refs-linux-x86_64-gcc.json.gz`, `corpus-refs-darwin-x86_64.json.gz` | Platform/compiler supplements selected by the reference reader; preserve their own provenance. |
| `verification.json` | Recording-time status/deviations used by `tools/corpus.py`, `tools/gen_docs.py` and `tools/gen_web_models.py`; not a fresh replay result. |
| `conformance-baseline.json.gz` | Signature conformance baseline, read by the conformance harness and function-model generator. |
| `census-baseline.json.gz` | Model-language support baseline for `harnesses/model_census.py`. |
| `corpus-bench.tsv` | Historical benchmark source validated by `tools/corpus_table.py` and documentation generation. |
| `corpus-bench-o1vec.tsv`, `corpus-bench-o1vec.manifest.json` | Historical optimized-reference comparison and provenance. |
| `educational-bench-o1.tsv`, `educational-bench-o1.manifest.json`, `educational-bench-o1.metadata.json` | Imported-model optimized-reference measurements and metadata. |
| `native-reduce-sum-probe.json`, `native-reduce-sum-imports.json`, `native-reduce-sum-integration.json` | Successive prototype/import/integration experiments; [parallelism guide](../research/parallelism.md) explains their distinct scopes. |
| `python-function-overhead.json` | Function-call overhead A/B evidence linked from the [interface guide](../research/packaging.md). |

Current published corpus runs are under [output/corpus-performance](../../../output/corpus-performance/README.md)
and [the vectorized reference run](../../../output/corpus-performance-vectorized/README.md).
Dated execution evidence is in `notes/execution/data/`; older ctsem/numerical
records remain in `notes/performance/`. Their adjacent Markdown reports explain
what each sample can establish. The [catalog](../catalog.md) locates those reports.
