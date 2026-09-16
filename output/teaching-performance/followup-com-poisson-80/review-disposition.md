# Review disposition

Two read-only Fable 5.1 CLI reviews used the model recorded in the accompanying
JSON results. The reviewer inspected changing work, so its timing and admission
remarks describe intermediate snapshots.

- Replaced the prototype's thread-local destructor with existing executor-owned
  KernelState storage. No TLS destructor or intentionally leaked buffer ships.
- Added the definite-initialization proof and fresh-buffer fallback. The final
  compiled COM-Poisson program admits 20/20 replay regions; admission output is
  archived. The proof is cached at the end of necessity lowering and reused by
  executor copies; hand-built programs are checked at binding.
- Added tests for refused-but-valid execution, a lowered loop with reuse on/off,
  a copy after the source workspace has been populated, nested evaluation,
  exceptions, changing sizes, incoming jump labels, dynamic read windows, and
  CALL scratch. Existing worker tests exercise independent executor ownership.
- Ran the separately instrumented NaN-sentinel checker through all 316 corpus
  reference checks. Restored production source and binaries before final timing.
- Used Stan arena allocation for scalar callback vectors, preserving Stan Math
  operations and accumulation order. No closed-form derivative rewrite.
- Documented opt-out switches in TESTING.md. Full tests and numerical references
  pass. The final benchmark is distinct from the earlier TLS and uncached-proof
  probes retained in the experiment archive.
