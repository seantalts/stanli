# Stan source lit tests

Every marked `.stan` file below `tests/` becomes its own CTest automatically.
New standalone cases live below this directory; existing fixture sources can
opt in where they already are. No CMake edit or C++ test target is needed.

Each file has two required directives and one optional data directive:

```stan
// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 3}
// STANLI-LIT-TIMEOUT: 60
```

- `STANLI-LIT` is `PASS` for supported behavior and `XFAIL` for a known gap.
- `STANLI-LIT-EXPECT` is `OK`, `CRASH`, or a substring of the expected
  `COMPILE_FAIL`/`EVAL_FAIL` result.
- `STANLI-LIT-DATA` is JSON. It defaults to `{}` when omitted.
- `STANLI-LIT-TIMEOUT` is an optional positive timeout in seconds. It
  defaults to `30` when omitted.

A matching `XFAIL` passes CTest. Any changed result fails so the case is
reviewed and, when fixed, promoted to `PASS` with its new expectation. This is
also how a silently accepted invalid model is recorded: `XFAIL` plus `OK`.
XFAIL cases also carry the CTest label `broken`, so the known-gap inventory is
directly runnable with `ctest --test-dir build -L broken`.

The runner copies each source into a temporary directory before invoking
`stanli_check`, because its pinned stanc writes a sibling `.hpp` even when MIR
is sent to stdout. The ordinary developer setup provisions that compiler. A
missing compiler is a test failure, not a skip, and no generated MIR is checked
into this directory. `stanli_check` uses `deps/stanc3/stanc` by default; set the
`STANC` environment variable to test another compiler explicitly.

After configuring the ordinary native build, run the complete source-lit
suite with the local build target:

```sh
cmake --build build --target lit -j24
```

Run just the annotated known gaps with:

```sh
cmake --build build --target lit-broken -j24
```

The targets default to 24 parallel CTest jobs. A smaller machine can choose a
different value at configure time with `-DSTANLI_LIT_JOBS=8`.

The exhaustive ordinary-builtin and density fixtures are generated from the
same runtime registry exposed by `build/dump_function_specs`. Their shared
generator support partitions large inventories and renders each shard in all
four execution contexts. Regenerate them after changing registered functions:

```sh
tools/generate_builtin_signature_models.py \
  --registry build/dump_function_specs
tools/generate_density_signature_model.py \
  --registry build/dump_function_specs
```

The corresponding JSON manifests under `tests/function_coverage/` record each
tested, excluded, and missing stanc signature, including functions for which
only some overloads are excluded. CTest checks that fixtures and manifests
remain current. These generated models are source-only lit inputs: ordinary
builds do not also materialize their legacy MIR under `tests/fixtures/`.

For a single case, invoke the runner directly; `--discover` prints the current
result without enforcing the checked-in expectation:

```sh
tests/lit/run.py tests/lit/issue_257/to_array_1d.stan \
  build/stanli_check

tests/lit/run.py --discover tests/lit/issue_257/to_array_1d.stan \
  build/stanli_check
```
