# stanc3's test models against CmdStan

stanc3 keeps 1,244 models in `test/integration/good` to test that they
compile. They cover syntax and type combinations that posteriordb and the
signature generators never produce. This suite runs every one of them
through stanli and compares the results with CmdStan's answers at five
unconstrained points (`eval_point` variants 0 to 4 in
`tools/stanli_check.cpp` and `tools/ref_driver.cpp`):

- the log density and its full gradient,
- the full write_array row: constrained parameters, transformed
  parameters and generated quantities, RNG draws included (both sides
  seed Stan's generator with `create_rng(1234, 0)` at each point),
- and, where CmdStan throws, its error message: stanli has to throw at the
  same point with a message containing CmdStan's (paths, numbers and the
  `Exception:` prefix removed).

Values agree when `|a - b| / max(1, |a|, |b|) <= 1e-9`; NaN agrees with
NaN, and an infinity only with the same infinity.

## Files

| file | what it holds |
| --- | --- |
| `data.json.gz` | the data for each model, keyed by its path under `good/`, with the source sha256 |
| `references.json.gz` | CmdStan's answers per model and point, or how CmdStan failed, with toolchain provenance |
| `ledger.json` | every model that is not fully verified, with its status and a short cause |
| `overrides.json`, `overrides/` | hand-adapted sources or data, with a note saying why |

## Running it

`ctest -R test_stanc3_good` runs 16 shards. Each one fails, not skips,
when `deps/stanc3-src` is missing or not at `STANC3_SRC_SHA`. By hand:

```
python3 tools/check_stanc3_good.py --build build-rel            # all
python3 tools/check_stanc3_good.py --filter tuples/ --times     # some
```

A model fails when its result differs from the ledger in either
direction. Getting worse is a regression. Getting better means the ledger
entry is stale: delete it, or run with `--update-ledger` and review the
diff. Point-level entries name the points: `{"points": {"2": "mismatch"}}`.
An entry whose cause starts with `UNTRIAGED` fails until someone names the
cause. A mismatch with CmdStan is a bug report, not a ledger line; ledger
it only with the cause found.

## Recording

Recording needs the pinned CmdStan in `deps/cmdstan`. Nothing else in the
suite runs CmdStan.

```
python3 tools/check_stanc3_good.py --record --ref-objs DIR
```

`DIR` holds prebuilt CmdStan model objects as `<source sha256>/model.o`
with a `FLAGS` file giving the compile and link flags (the layout
`--record` itself writes into `build-stanc3-ref/objs` for any model
missing there). References use plain `stanc`, CmdStan's default. A model
CmdStan does not finish within `--ref-timeout` seconds is recorded as a
timeout and not run through stanli. `--filter` records part of the corpus
and keeps the other references.

## Adding an override

Add an entry to `overrides.json` keyed by the upstream path, with `data`
(a JSON file in `overrides/`), `source` (an adapted copy of the model) or
both, and a `note` saying what was changed and why. `copy` names a
byte-identical copy of the upstream source kept for another harness; the
suite checks that it still matches. Then re-record that model with
`--filter`.
