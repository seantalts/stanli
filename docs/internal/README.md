# Research index for agents

Read this page before proposing research. Choose one topic below, then open
only the decision or result that bears on the task. The
[full catalog](catalog.md) lists every repository-owned Markdown document;
use it for lookup, not as a required reading list. Human readers start at
[the documentation index](../README.md).

| Research question | Read first | What it tells you |
| --- | --- | --- |
| Interpreter fallbacks, callbacks, RNGs, register coverage, local tapes | [Execution coverage](research/execution.md) | Enabled improvements, remaining boundaries, removed experiments and deletion criteria. |
| Retained loops, frames, recording, ctsem, memory | [Loops and memory](research/loops.md) | Which frame/data-provenance ideas worked, which failed, and why validation order matters. |
| Compiler lowering, shapes, lanes, vectorization, preparation | [Compiler and layout](research/compiler.md) | Design trail, shared representations, transactional refusal and preparation regressions. |
| Numerical disagreement, sampler work, corpus oracles, performance claims | [Numerics and measurement](research/numerics.md) | Authoritative gates, source/platform provenance, unresolved exceptions and benchmark boundaries. |
| Native reduce_sum, shared inputs, worker ownership | [Parallel reductions](research/parallelism.md) | Shipped interface versus historical prototypes, deterministic partitions and memory costs. |
| Binaries, browser packaging, interfaces, installation | [Packaging and interfaces](research/packaging.md) | Current contracts and earlier designs or rejected packaging experiments. |

## Rules for interpreting the records

- Current source/tests and [TESTING.md](../../TESTING.md) define support and
  numerical gates. A dated plan's word “current” refers to its recorded revision.
- A plan proposes work; a review checks assumptions; a result records evidence.
  None is fresh authorization. Check the last disposition before repeating work.
- Keep source/build identity, numerical evidence and performance phase together.
  Do not combine medians from different experiments or infer a universal
  no-regression guarantee from finite canaries.
- Native instruction generation and stencil JIT are tabled. The general-value
  interpreter replacement is deferred. Prefer measured improvements in the
  existing engines. Fable reviews are reserved for rare overarching plans.
- Open raw data only to answer a particular numerical or provenance question.
  [Artifact inventory](artifacts/README.md) identifies its owners; recorded paths
  inside evidence describe the original run and have not been rewritten.

## Where to write

Keep these topic guides short and update their status links rather than appending
session diaries. Write new proof/result notes under `notes/<topic>/`, with the
decision, baseline, measured outcome, limits, next question and evidence links.
Historical Superpowers plans/specs/reviews are in [archive](archive/README.md).
Large completed plans may become digests with immutable Git links to the full
version. Preserve negative results and unresolved failures when compacting.
Update [the catalog](catalog.md) when adding or moving a document.
