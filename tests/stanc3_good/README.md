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
| `references.json.xz` | CmdStan's answers per model and point, or how CmdStan failed, with toolchain provenance |
| `ledger.json` | every model that is not fully verified, with its status and a short cause |
| `overrides.json`, `overrides/` | hand-adapted sources or data, with a note saying why |

## Data

`data.json.gz` holds frozen data for every model in stanc3's
`test/integration/good` at `STANC3_SRC_SHA` (deps/pins.env), keyed by path
relative to that directory:

```
{"schema": 1, "stanc3_sha": "...", "models": {"<path>": {
  "source_sha256": "...", "data": {...} | null,
  "origin": "generated" | "hand" | "none_needed",
  "attempts": N, "accepted": true | false, "note": "..."}}}
```

`tools/gen_stanc3_good_data.py` writes it. Each model's data comes from
`stanc --debug-generate-data`, retried up to `--max-attempts` times until
CmdStan's reference driver (`tools/ref_driver.cpp`) loads it without a
`DATA_FAIL`. Among loadable candidates it keeps the one finite at the most
points. Candidates are stanc's draws, then the same draws with small repairs
(ints in containers made 0/1, reals moved into (0, 1), sizes cut to 1,
square matrices made the identity, vectors made a simplex or ascending).
`accepted: false` means no candidate loaded; `note` then holds the most
common `DATA_FAIL` message, or why the model never built, and `data` is the
last rejected candidate; the suite runs it only to check that stanli
rejects it too. An accepted entry that is not finite at every point says so
in `note`. A model with no data has `origin: none_needed` and `data: {}`.
The generator also reads hand data from an optional `hand_data.json`
(`{"<path>": {"note": "...", "data": {...}}}`); hand data for this suite
belongs in `overrides.json`, which wins over `data.json.gz`.

A rerun keeps accepted entries whose source is unchanged, since stanc has
no seed; `--regenerate` redoes them. `--improve` retries the accepted
entries that are not finite at every point and keeps a new candidate only
if it is finite at more points; `attempts` accumulates across such runs. `--check` (ctest
`test_stanc3_good_data`) verifies the file covers exactly the pinned
corpus with matching source hashes, and needs no CmdStan.

### The generator's reference binaries

`build-stanc3-ref/` (gitignored) keeps one linked reference binary per
model, keyed by the sha256 of the model source:

```
build-stanc3-ref/<sha256>/ref          ref_driver linked against the model
build-stanc3-ref/<sha256>/stamp.json   what it was built from
build-stanc3-ref/driver-<sha16>.o      ref_driver.cpp object (--objs only)
```

Models are translated with plain `stanc` (no `--O1`). Reuse a binary only
when `stamp.json` matches what you would build: it records the driver's
sha256 plus either the prebuilt object directory (`--objs DIR`, holding
`<sha256>/{model.o,status}` and a `FLAGS` file) with the size and mtime of
the `model.o` it linked, or the stanc sha256, CmdStan HEAD, stanc flags and
optimisation level.

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
cause.

| status | meaning |
| --- | --- |
| `unsupported` | stanli refuses to compile the model; the cause is its message |
| `rejected_both` | every point throws on both sides with matching messages |
| `data_rejected` | CmdStan rejects every data candidate and stanli rejects the last one with a matching message |
| `data_message_mismatch` | both reject the data with different messages |
| `data_unavailable` | stanc could not generate data; not run |
| `one_side_threw` | one side rejects the data or the compile, the other does not |
| `points` | per point: `mismatch`, `one_side_threw`, `message_mismatch`, or `wa_unsupported` (stanli's write_array refuses a feature) |
| `cmdstan_timeout`, `cmdstan_crashed` | CmdStan does not finish or dies; stanli is not run |
| `timeout`, `crashed` | stanli does not finish or dies |

A mismatch with CmdStan is a bug report, not a ledger line; ledger it only
with the cause found.

## Recording

Recording needs the pinned CmdStan in `deps/cmdstan`. Nothing else in the
suite runs CmdStan.

```
python3 tools/check_stanc3_good.py --record --ref-objs DIR --jobs 28
```

Only models whose source or data changed are recorded, unless
`tools/ref_driver.cpp` changed or `--all` is given.

`DIR` holds prebuilt CmdStan model objects as `<source sha256>/model.o`,
with `status` and an optional `opt` file per model, and a `FLAGS` file
giving the compile and link flags. The largest models do not fit many
parallel compiles in memory, so `--record` builds a missing model object
only for an adapted override source, or for every model with
`--build-objects` (and a small `--jobs`); it writes them in the same layout
into `build-stanc3-ref/objs`. Without a `FLAGS` file the flags are
`tools/cmdstan_ref.py`'s. References use plain `stanc`, CmdStan's
default; each model's C++ optimization level is kept in its reference. A model
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
