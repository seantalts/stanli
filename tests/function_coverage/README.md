# Integrated function coverage

These ten small mother-style models cover 430 distinct names: supported Stan
functions, higher-order solver variants, and five operators. Five models are
generated from the smallest verified overload of each of 243 names in the
conformance baseline. The other five cover containers, data-only functions,
multivariate densities/GLMs, RNGs, and higher-order calls.

This is **function-name coverage, not complete overload or input coverage**.
`manifest.json` records each case and the supported boundaries. In particular,
GP coordinates are data, GLMs use supported design/outcome shapes, and the
integrated `map_rect` probe covers its empty-job case; `map_rect_region.stan`
covers nonempty serial calls through both graph and runtime-control lowering.
`ode_region.stan` similarly covers every legacy and modern forward ODE solver
through runtime control. `dae_region.stan` covers `dae` and `dae_tol` through
the same retained-callback and graph/Program kernel-call infrastructure.
`ode_adjoint_region.stan` covers the CVODES adjoint solver across ordinary and
runtime-control expressions, including active initial/output times. The
quadrature family has focused graph/runtime-control coverage in
`quadrature_region.stan`; `ode_active_times.stan` and
`ode_time_activity.stan` cover the modern forward-solvers' independently
active initial and output times. `higher_order_transformed_data.stan` runs the
same retained ODE, adjoint-ODE, DAE, algebra, and quadrature kernels from the
preparation and interpreted-write-array backends, with structural map/reduce
calls alongside them;
`gaussian_dlm_obs_lpdf` is explicitly unsupported, not silently skipped.
Parameter declaration transforms remain in their dedicated tests; a transform
being supported in a declaration does not imply its named function is callable.

Every case contributes to the log density and/or a retained output column.
Nonuniform container projections expose element permutations. Data-only
results multiply a parameter as well as appearing in output; RNGs are compared
with the same seed and chain. Every RNG is also drawn in transformed data with
data arguments, where CmdStan uses the model-construction stream (seed 1,
chain 0); those draws enter the log density and are copied into the output. CmdStan supplies all reference numbers. At three
fixed unconstrained points the gate compares the log density, every gradient,
the full column schema, and every output value. It rejects nonfinite probes,
missing output, compilation failures and shape differences. The numerical
gate is the corpus replay's `abs(a-b)/max(1,abs(a),abs(b)) <= 1e-9`.

CTest runs freshness, harness mutation tests, and numerical replay on each PR:

```sh
python3 tools/gen_function_models.py --check
python3 tests/test_function_models.py
python3 tools/check_function_models.py --build build-rel
```

The generator checks runtime dispatch literals and X-macro registrations
against its recipes and the pinned signature inventory. Adding a named runtime
function without a probe fails freshness; there is no automatic "unsupported"
escape. This source audit is intentionally conservative and is not proof that
every overload or execution path is implemented. The exhaustive nightly sweep
and focused boundary tests retain those separate responsibilities.

To change coverage, edit `recipes.py` or `higher_order.stan`, regenerate, and
record all ten models with the pinned CmdStan checkout:

```sh
python3 tools/gen_function_models.py
python3 tools/check_function_models.py --build build-rel --record --jobs 2
```

The reference artifact records compiler/dependency identities and model source
hashes with CRLF normalized to LF, so Windows checkouts use the same references.
Compiler binary hashes remain byte exact. Recording is independent of stanli's
answers: all CmdStan rows are saved before replay, and a mismatch still fails.
Never change references to match a stanli regression. The generated sources
and reference artifact are committed so normal CI needs no CmdStan build or
reference download.

## Container write/read grid

`tools/gen_container_flow_models.py` writes `container_flow/*.stan` and
`container_flow_manifest.json`. It crosses container shapes (int and real
arrays of rank 1-3, vector, row_vector, matrix, arrays of vectors, row_vectors
and matrices) with write forms (initializer, whole assignment, UDF return,
builtin, element loop, row, `[i, :]`, `[:, j]`, range, multi-index, `+=`) and
read forms (int- and real-typed element reads, loop bounds, int indexing and
gathers, `size`, `dims`, int arithmetic, row and column copies) in every block
and inside a UDF. There is one model per block and container, plus one per
container for builtin writes, because a refused case fails its whole model.
Issue #523 was an int read of a container written by rows, so int containers
are read through int-typed paths. Each read lands in its own output column,
or in the model block multiplies its own element of `p`; the manifest names
every gradient element and lists the skipped combinations and why.

Further families read `NAME.json` data beside each model (the replay uses a
model's own data file when one exists): writes and reads through run-time
indices (loop variables, data ints, ints read from other containers, run-time
ranges and multi-indices) per block and container; multi-index reads with
data index arrays; zero-size containers whose extent comes from data; locals
whose size changes on each loop pass, including per-group likelihood UDFs
called with a different-length index array; and int arrays returned from
UDFs, sliced, row-selected, or chosen by a `categorical_rng` draw.

These models are not in `tests/fixtures`, so the cross-path matrix does not
run them: their model blocks sum many `read * p[k]` terms, and rerolling and
constant folding regroup that sum by 1-2 ULP.

Regenerate and re-record with:

```sh
python3 tools/gen_container_flow_models.py
python3 tools/check_function_models.py --build build-rel --record --jobs 10 \
  --fixtures tests/function_coverage/container_flow \
  --manifest tests/function_coverage/container_flow_manifest.json \
  --reference tests/function_coverage/container_flow_references.json.gz
```

`--single DIR` also writes each case as its own model, to find which case a
refusal comes from.
