# Generated Stan conformance harness

`harnesses/stan_conformance.py` is the single entry point for an exhaustive,
deterministic Stan Math signature inventory and differential conformance run.
It never infers product support from stanli implementation files. Anything the
generator cannot exercise is a red `generator_gap`, not an unsupported claim.

The implemented numeric phase covers every applicable ordinary scalar
`int`/`real` overload and every scalar probability overload with a known
support profile, plus recursively nested array, vector, row-vector, and matrix
overloads that have an accepted scalar analogue. All 19,009 such signatures in
the pinned inventory receive deterministic probes. Recursive shapes share one
size-two axis across their vectorization axes once depth exceeds two, with that
axis distributed by stable case ID; ordinary vectors remain length two and
matrices remain 2x2. Every real leaf originates in an independent parameter.
The harness packs cases into content-addressed Stan shards and compares log
density plus the complete unconstrained gradient at three points through the
official BridgeStan client on both the pinned reference and stanli sides.
If stanli rejects a generated source before selecting a case, the runner
bisects that side into content-addressed retry shards while retaining the one
compiled reference shard. Thus one unsupported call cannot classify its
supported neighbors as unsupported.
The named construct phase evaluates 31 file-backed cases across 10 categories.
It additionally compares construction/evaluation/write-array phase,
exception category, unconstrained parameter names and count, and constrained
output names/order/values. One deliberately uninitialized upstream model is
classified as an explicit compilation census. Structural operations without
a valid scalar analogue remain visible as red generator queue entries until
their dedicated domain and reduction cases are added.

## Setup and run

One command, on Linux or macOS:

```sh
tools/dev_setup.sh --conformance
```

That prepares the whole oracle: the exact CmdStan/Stan/Stan Math combination
BridgeStan compiles against, the conformance stanc built from source at
`STANC3_SRC_SHA` -- the same revision the wheels embed, rather than a
downloaded binary -- the version-pinned BridgeStan Python client in
`.venv-conformance/`, and the stanli library staged where the harness drives
it from. Every step is a no-op once its output exists, so the stanc build's
half hour is paid once per machine per pin, not once per session, and
`--embed` and `--corpus` pay most of it already. It ends by printing the
invocation below with your paths filled in.

Only the nightly *workflow* is pinned to Linux. Nothing in the harness is:
`fetch_cmdstan.sh` selects its TBB target per `uname` and has handled Darwin
all along, and a full local run on macOS reproduces the same statuses. The
half-hour figure above reads like a wall and is not one, which is how this
suite ended up treated as CI-only and its findings argued from the last
nightly's recorded rows instead of being checked.

Run the driver under the venv interpreter, not the host's `python3`. The
harness reads its policy and catalog as TOML; `tomllib` arrived in Python
3.11, and `requirements.txt` carries the backport for older hosts -- but
into the venv, so on an older system `python3` the driver refuses to start
at all. Running it from the venv also makes `--stanli-python` default to the
same interpreter, which is where the pinned BridgeStan client lives.

```sh
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned \
  --cmdstan deps/cmdstan \
  --build build-rel \
  --stanli-pythonpath python \
  --jobs 4
```

`--stanli-pythonpath python` points the worker at this checkout's package
instead of an installed wheel. The library it loads is the staged copy in
`python/stanli/_bin`, not the one in `--build`; `--build` only labels the
report and the reproduction commands. So restage after a rebuild, or the run
measures the library from before it:

```sh
cmake --build build-rel --target stanli_shared
cp build-rel/libstanli.* python/stanli/_bin/
```

`signature_watch.sh` is the other half of the pin story: it diffs stanc3's
checked-in signature dump between the pin and upstream master, so new
language surface is reported nightly without executing anything unreviewed.
Adopting what it reports is a pin advance: bump `STANC3_SRC_SHA` and rerun
this suite.

### Selecting what to run

The full differential sweep is a nightly-sized job. Day to day you want a
slice, and three stable selectors give you one:

| selector | meaning |
| --- | --- |
| `--filter SUBSTR` | case-insensitive substring of the signature; the one you usually want |
| `--case CASE` | one exact canonical or case ID |
| `--shard N/M` | stable one-based partition, the same split the nightly uses |

```sh
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan --build build-rel \
  --stanli-pythonpath python --filter log1p

.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan --build build-rel \
  --stanli-pythonpath python --case 'abs(real)=>real'

.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --mode inventory --shard 2/8
```

`--case` is not repeatable, and `--case` and `--filter` do not combine: one
case per invocation, `--filter` for a group. Passing `--case` twice used to
run only the second one silently, which reads as the harness disagreeing
about the case you named first; it is now rejected.

By default `--cmdstan` selects the generated differential mode (the historical
CLI spelling is `--mode scalar`) and also executes matching construct cases.
Use `--mode inventory` for the fast classification-only pass.

Any selected run prints `RED` and `gate issues: partial_run`. That is the
gate reporting its scope, not a finding: the suite is green only when it is
complete, and a slice is by construction not complete. Read `status_counts`
in `conformance-out/conformance.json` -- or the `verified=`/`mismatch=`
summary on the first line of output -- for what the slice actually found.

The `reproduce:` line printed under each blocking row spells the driver
`python3`, because a report is read on machines other than the one that
wrote it. Substitute your own interpreter, which on a pre-3.11 host means
`.venv-conformance/bin/python`.

The default output directory is `conformance-out/`. It contains the complete
`conformance.json`, generated `unsupported.md`, raw `signatures.txt`, cached
reference shards, and a compact `repro/` index. A probed red case also gets its
selector data, active parameter slices, paired outcomes, and reproduction
command. Generated Stan sources are content-addressed once under
`repro/sources/`, so many failures in one shard do not duplicate the model.

## Gates and snapshots

Every inventory row has exactly one of the seven design statuses. A run is
green only when it is complete, contains no blocking status, has no stale
policy exception, and (when configured) exactly matches its classification
snapshot. Generated reports and snapshots are build artifacts, not checked-in
source. To freeze a run, pass an explicit ignored or externally retained path:

```sh
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan --build build-rel \
  --stanli-pythonpath python \
  --baseline conformance-out/conformance-baseline.json --update-snapshot
```

`--update-snapshot` requires `--baseline` and refuses partial or failed runs.
It cannot promote an observed refusal into `expected_unsupported`; that first
requires a reviewed rule in `policy.toml`. Without `--baseline`, live inventory
completeness, blocking statuses, pinned tool identities, and the reviewed
policy remain the gate, while every generated file stays in the ignored output
tree.

Distributed runs use `--write-manifest M`, `--shard N/M`, and `--aggregate`.
The aggregate gate rejects missing and duplicate case IDs before optional
snapshot comparison. `--report-only` regenerates Markdown and repro indexes
without reevaluation.

`.github/workflows/stan-conformance-nightly.yml` runs the pinned Linux x86-64
job nightly and on manual dispatch. It builds the public runtime once, executes
eight stable differential partitions with content-addressed reference caches,
then strictly aggregates and uploads reports even while the semantic gate is
red. The aggregate Actions summary links directly to the complete artifact and
shows the first 40 missing signatures plus the first 20 other blocking findings,
including their reasons and copy-paste reproduction commands.

## Fast harness tests

```sh
.venv-conformance/bin/python tests/test_conformance.py
```

Any Python 3.11 or newer works; the venv is simply the one the setup step
leaves you with, and on an older host it is the only one that imports.

The tests cover recursive signature parsing, policy mutation guards,
deterministic generation and sharding, catalog validation, construct phase and
exception gates, nonfinite comparison, status transitions, snapshot refusal,
repro generation, distributed aggregation, and a two-case JSON-lines oracle
mutation.
