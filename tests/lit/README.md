# Stan model lit tests

Every marked `.stan` file below `tests/` and its adjacent checked-in
`.tmir.sexp` fixture become one CTest automatically. New standalone cases live
below this directory; existing fixture sources can opt in where they already
are. No CMake edit or C++ test target is needed.

Each file has two required directives and one optional data directive:

```stan
// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 3}
```

- `STANLI-LIT` is `PASS` for supported behavior and `XFAIL` for a known gap.
- `STANLI-LIT-EXPECT` is `OK`, `CRASH`, or a substring of the expected
  `COMPILE_FAIL`/`EVAL_FAIL` result.
- `STANLI-LIT-DATA` is JSON. It defaults to `{}` when omitted.

A matching `XFAIL` passes CTest. Any changed result fails so the case is
reviewed and, when fixed, promoted to `PASS` with its new expectation. This is
also how a silently accepted invalid model is recorded: `XFAIL` plus `OK`.
XFAIL cases also carry the CTest label `broken`, so the known-gap inventory is
directly runnable with `ctest --test-dir build -L broken`.

The ordinary suite runs each fixture through `stanli_check --mir`. It therefore
does not need the optional stanc executable and never converts a missing
compiler into a skipped test. Compiler-bearing CI regenerates the MIR with the
repository's pinned stanc and rejects stale fixtures. To refresh them locally,
point `STANC` at that compiler:

```sh
STANC=deps/stanc3/stanc tools/gen_fixtures.sh
```

After configuring the ordinary native build, run the complete model-lit
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

For a single case, invoke the runner directly; `--discover` prints the current
result without enforcing the checked-in expectation:

```sh
tests/lit/run.py tests/lit/issue_257/to_array_1d.stan \
  build/stanli_check

tests/lit/run.py --discover tests/lit/issue_257/to_array_1d.stan \
  build/stanli_check
```
