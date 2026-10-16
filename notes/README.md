# Research evidence

Agents should start at [the short research index](../docs/internal/README.md),
then read only the relevant topic and evidence. The
[full catalog](../docs/internal/catalog.md) describes individual records.

- [Execution checklist](execution/2026-09-29-execution-coverage-checklist.md):
  current ordered gap map and validation/design checkpoints.
- [Remaining MIR uses](execution/2026-09-29-mir-interpreter-remaining-uses.md):
  current source audit and concrete coverage opportunities. The
  [execution roadmap](execution/2026-09-28-execution-engines-and-roadmap.md)
  covers shipped improvements, local tapes and removed experiments;
  raw measurements are in `execution/data/`.
- `performance/`: earlier ctsem, numerical-oracle, sampler and regression
  investigations with their evidence. These are dated records, not current
  task instructions or a blanket validation of today's build. Earlier
  user-facing benchmark pages live here too:
  [benchmark history](performance/benchmark-history.md), the
  [September 11 posteriordb tables](performance/2026-09-11-posteriordb-benchmark.md),
  [brms performance](performance/2026-09-16-brms-performance.md) and
  [build performance](performance/build-performance.md).
- `validation/`: dated validation records, such as the
  [stanfit compatibility audit](validation/2026-09-16-stanfit-compatibility.md).

Keep new notes in topic directories. Preserve proof obligations, decisions,
failed experiments, numerical limits, input/build identities and raw evidence.
Summarize settled work and link details rather than growing another chronology.
Production code and executable fixtures belong outside this directory.
