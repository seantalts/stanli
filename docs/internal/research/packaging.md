# Packaging, interfaces and installation

For current contracts, use the [Python](../../../python/README.md),
[R](../../../r/README.md), [JavaScript](../../../js/README.md),
[stanfit compatibility](../../stanfit-compatibility.md), and
[build measurements](../../build-performance.md) guides. Installation simplicity,
first useful results, compressed size and installed size are separate concerns.

| Prior work | What to recover from it |
| --- | --- |
| [Portable runtime design](../archive/specs/2026-08-04-stan-portable-runtime-design.md) | Original architecture and distribution assumptions; not a current feature inventory. |
| [BridgeStan facade](../archive/specs/2026-08-10-bridgestan-facade-design.md) | ABI and surrounding ecosystem compatibility rationale. |
| [Embedded MIR/data](../archive/specs/2026-08-11-embedded-mir-data.md) | Loading and artifact ownership design. |
| [OCaml MIR rollout](../archive/plans/2026-08-26-ocaml-mir-backend-rollout.md) | Compiler producer pins and portable/native discovery. |
| [Python function overhead](../archive/plans/2026-08-30-python-function-overhead.md) | Paired call-boundary measurements and the four-way A/B recipe. |
| [Browser density-pack experiment](../archive/density-pack.md) | A working prototype that was removed; measured payload and the remaining deployment blocker. |

The density-pack experiment is historical, while the optional
[lite build](../../lite-lp.md) has a current public numerical contract; do not
confuse them. Native instruction generation and stencil JIT are tabled, and
the recent [code-generation probes](../../../notes/execution/2026-09-28-codegen-decision.md)
were removed with their [cleanup](../../../notes/execution/2026-09-29-execution-code-cleanup.md).
A browser import microbenchmark does not establish native-host performance.

Exercise installed artifacts and real clients, including compiler/runtime
discovery and error messages. A successful source-tree build is insufficient.
Raw records preserve exact compiler and library identities; historical download
sizes must not be advertised as sizes of the current release.
