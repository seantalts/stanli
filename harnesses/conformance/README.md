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

Fetch stanli's pinned dependencies first. The helper then prepares the exact
CmdStan/Stan/Stan Math combination used by BridgeStan, and builds the
conformance stanc from source at `STANC3_SRC_SHA` (`tools/dev_setup.sh`) --
the same revision the wheels embed -- rather than trusting any downloaded
binary. The build needs opam and takes ~30 minutes once per pin; it is a
no-op when `deps/stanc3/stanc-pinned` already matches the pin. The workflow
likewise pins the public BridgeStan Python client used to drive stanli's
facade.

`signature_watch.sh` is the other half: it diffs stanc3's checked-in
signature dump between the pin and upstream master, so new language surface
is reported nightly without executing anything unreviewed. Adopting what it
reports is a pin advance: bump `STANC3_SRC_SHA` and rerun this suite.

```sh
./deps/fetch.sh
./harnesses/conformance/fetch_cmdstan.sh
python3 -m pip install -r harnesses/conformance/requirements.txt

python3 harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned \
  --cmdstan deps/cmdstan \
  --build build-rel \
  --jobs 4
```

By default `--cmdstan` selects the generated differential mode (the historical
CLI spelling is `--mode scalar`) and also executes matching construct cases.
Use `--mode inventory` for the fast classification-only pass. Development
selectors are stable:

```sh
python3 harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan --build build-rel \
  --case 'abs(real)=>real'

python3 harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --mode inventory --shard 2/8
```

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
python3 harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan --build build-rel \
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
python3 tests/test_conformance.py
```

The tests cover recursive signature parsing, policy mutation guards,
deterministic generation and sharding, catalog validation, construct phase and
exception gates, nonfinite comparison, status transitions, snapshot refusal,
repro generation, distributed aggregation, and a two-case JSON-lines oracle
mutation.
