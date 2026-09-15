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

## Checking a lowering pass

A case can additionally assert the shape of one pass dump. `STANLI-LIT-DUMP`
names the stage; the runner asks `stanli_check` for it on stdout and matches
the `CHECK` directives against that stage's dump alone. The ordinary result
expectation still applies.

```stan
// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DUMP: log_prob:reroll
// STANLI-LIT-CHECK: s{{[0-9]+}}[1] = INDEX s{{[0-9]+}}[5,P] idata=[4]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[15] = FMA s{{[0-9]+}}[1] s{{[0-9]+}}[15] s{{[0-9]+}}[15]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[1] = NORMAL_LPDF.v=0x86 s{{[0-9]+}}[15] s{{[0-9]+}}[15] s{{[0-9]+}}[1]
```

- `CHECK` searches forward from the line after the previous match.
- `CHECK-NEXT` must match the immediately following line.
- `CHECK-NOT` must match nothing between the checks surrounding it, or
  nothing in the rest of the dump when no check follows it.
- A pattern is matched literally except inside `{{...}}`, whose contents are
  a regular expression. Slot numbers are model-dependent, so write them as
  `{{[0-9]+}}` rather than pinning them.
- Matching is a search within a line, not a whole-line match.

Write the checks from real output rather than from expectation: `--discover`
prints the sliced dump for the named stage.

`stanli_check` compiles each case through the embedded stanc3 pipeline, the
same compiler `stanli_run` and the Python and R packages use, so a case sees
every source pass that ships. The runner writes the source and its data into a
temporary directory so a run never dirties the checkout. A build without the
embedded compiler runs the same pipeline through the `stanli-compile`
executable beside `stanli_check` or on `PATH`; a build with neither fails
these tests rather than skipping them, and no generated MIR is checked into
this directory. To test another compiler explicitly, run `stanli_check` by
hand with `--stanli-compile PATH` or `--stanc PATH`.

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
