# Documentation

Start with the [project overview](../README.md) or your interface guide:
[Python](../python/README.md), [R](../r/README.md), or
[JavaScript/browser](../js/README.md).

| What you want to understand | Read |
| --- | --- |
| Performance and its measurement limits | [Benchmarks](benchmarks.md), [benchmark protocol](benchmark-protocol.md) |
| Model and function support | [Corpus status](corpus-status.md), [probability-function coverage](coverage.md) |
| Numerical correctness, tests and exceptions | [Testing and numerical contracts](../TESTING.md), [compact densities](compact-densities.md), [optional lite build](lite-lp.md) |
| Moving an R workflow to Stanli | [From cmdstanr](from-cmdstanr.md), [native stanfit compatibility](stanfit-compatibility.md) |
| Generated models and teaching | [brms/Rethinking workflows](teaching-support.md), [teaching guide](teaching.md) |
| Faster results that may differ from CmdStan in the last bits | [Fast mode](fast-mode.md) |
| Parallel execution within a chain | [Native reductions](native-reduce-sum.md) |
| How execution works | [Architecture explained](how-it-works.md), [three-model walkthrough](lowering-walkthrough.md) |
| Contributing or building | [Contributor guide](hacking.md), [build/CI measurements](build-performance.md), [runtime source map](../runtime/src/README.md) |
| Older measurements | [Benchmark history](benchmark-history.md), [September 11 tables](benchmark-2026-09-11.md), [brms performance](brms-performance.md) |
| Releases and licensing | [Changelog](../CHANGELOG.md), [third-party licenses](../THIRD_PARTY_LICENSES.md) |

These guides are intended for people using or contributing to Stanli. Internal
research has a separate [short index](internal/README.md) and
[full document catalog](internal/catalog.md). Historical plans, review transcripts,
and machine-readable evidence live below `internal/` or in topical `notes/`
directories; they are not prerequisites for using the packages.
