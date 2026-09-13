# Final evaluator verification

All 16 processes in `verification.json` pass full gradient/graph parity and
declared warmup-duration checks. Their timings are verification-only.

Afterward, clang-format 22.1.8 split one adjacent string literal in the new
warmup telemetry. Rebuilding the benchmark produces a **byte-identical DSO**
to the one used by every private/default/explicit/short verification process:

`4f77da1e3b2c05a79532d4c482d0d742af5c09734f9cd3dc68fa560595499120`

Native source SHA-256 before formatting:
`3a29967ed1c8a795a43ca90d01b811c0a9e887adb1750ed38f581416dac0e8f0`

Native source SHA-256 after formatting:
`b273a23f6e6ca45c4c27597833c5b987197d54ae7235727105ebbd389e7dd306`

The whole tracked-source `tools/format.sh --check` passes. Focused CTest
allocator ownership, export and configuration checks pass (three cases),
as do actionlint and Python syntax checks. Shipping runtime sources/defaults
are unchanged by the regression investigation.
