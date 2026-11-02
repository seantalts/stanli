# stanc3 integration models

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
last rejected candidate, not usable data. An accepted entry that is not
finite at every point says so in `note`. A model with no data has `origin: none_needed` and
`data: {}`. Optional hand data lives in `hand_data.json` as
`{"<path>": {"note": "...", "data": {...}}}` and always wins over
generation.

A rerun keeps accepted entries whose source is unchanged, since stanc has
no seed; `--regenerate` redoes them. `--improve` retries the accepted
entries that are not finite at every point and keeps a new candidate only
if it is finite at more points; `attempts` accumulates across such runs. `--check` (ctest
`test_stanc3_good_data`) verifies the file covers exactly the pinned
corpus with matching source hashes, and needs no CmdStan.

### Reference binary cache

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
