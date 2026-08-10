# Language models, from stanc3's own test suite

The posteriordb corpus is 119 real posteriors, and real posteriors are
written in a small dialect of Stan: bounded reals, vectors, arrays of
vectors, the dozen densities people actually fit. Whole regions of the
language never appear in it. Nothing in posteriordb declares
`offset`/`multiplier`, a `cholesky_factor_cov`, a user-defined `_lupdf`,
or an integer modulus.

These models fill that in. They are lifted unchanged from stanc3's
`test/integration/good` (commit `ac69570`), where they exist to be
compiled and never to be run, and they go through the same oracle as the
corpus: CmdStan's recorded log density and full gradient in
`docs/corpus-refs.json.gz`, replayed by `tools/verify_refs.py` in CI on
every push. `tools/verify_refs.py` finds a model here by name before it
looks in posteriordb, so nothing about the CI step changed.

| model | upstream path | what it reaches |
| --- | --- | --- |
| `cholesky_cov_param_block` | `parser-generator/` | `cholesky_factor_cov[5,4]` and `[3]` parameters, square and rectangular |
| `declare-define-multi` | (top level) | multi-declarations with initializers in every block; `array[3,2] vector[5]` and `array[3,2] matrix[5,4]` data |
| `lupdf-inlining` | `compiler-optimizations/` | user `_lpdf`/`_lpmf` functions whose bodies call `_lupdf`/`_lupmf` |
| `multidim_var_param_ar45_mat23` | `parser-generator/` | `array[4,5] matrix<lower,upper>[2,3]`: 120 bounded parameters four levels deep |
| `operators` | `code-gen/expressions/` | integer `%`, unary `+`/`-`, comparison to a real, `rv / A` and `A \ v` |
| `reductions_allowed` | `compiler-optimizations/mem_patterns/` | parameter-dependent conditionals selecting whole matrices; matrix-valued UDFs |
| `tern_op_contains_var` | (top level) | a ternary choosing between two parameters inside a loop, under `binomial` |
| `validate_set_double_offset_multiplier_good` | (top level) | `offset`/`multiplier` on reals, vectors, row vectors and arrays of matrices |
| `vector-size-stmts` | (top level) | containers sized from data inside functions, in every block |

Everything upstream is BSD-3-Clause, the same license as this repository;
see `THIRD_PARTY_LICENSES.md`.

## Where the data comes from

stanc3's test models carry no data, which is why
`harnesses/fn_sweep.py` generates its models instead of borrowing
them. stanc generates data too:

```
./deps/stanc3/stanc --debug-generate-data model.stan > model.json
```

The values are random per invocation, so the file is committed, not
regenerated. It respects declared constraints (a `simplex` sums to 1, a
`corr_matrix` is a correlation matrix), which is most of the problem. It
does not know a distribution's support: `tern_op_contains_var.json` came
out with a `binomial` outcome above its own `y_max`, which CmdStan
rejects, and its `y` was edited by hand.

## Adding one

```
cp deps/stanc3-src/test/integration/good/PATH/foo.stan tests/stanc3/
./deps/stanc3/stanc --debug-generate-data tests/stanc3/foo.stan \
    > tests/stanc3/foo.json
python3 tools/verify_sample.py deps/cmdstan deps/posteriordb foo
```

That prints `VERIFIED` and writes the reference; commit
`docs/corpus-refs.json.gz` and `docs/verification.json` with the model,
then run `tools/gen_docs.py` so the README count follows. Pick models
that reach something the corpus does not: `tools/corpus.py` and the
`FAIL` reasons from a sweep of the upstream directory are the fastest way
to see what is left.

Two candidates did not make it and are worth knowing about:

- `declarations.stan`, which declares every type in every block, is
  unrunnable by CmdStan at any data. Its transformed data block declares
  constrained variables and never assigns them, so they are NaN when
  CmdStan validates them and it throws. stanli accepts it, which is its
  own small divergence: constraints on transformed data go unchecked.
- `code-gen/sum_to_zero.stan` is blocked on `sum_to_zero_matrix`, below.

## What these found

The point of a new oracle is the things it catches. Three, on the first
run:

1. **`sum_to_zero_matrix` had the wrong number of free parameters.** Its
   read dims, `[N, M]`, look exactly like `array[N] sum_to_zero_vector[M]`,
   and it was lowered as that: `N*(M-1)` unconstrained where Stan has
   `(N-1)*(M-1)`, because the matrix transform centers both axes. The
   result was a finite gradient of the wrong model. It is now refused at
   compile time (`tests/test_newtrans.cpp` pins the message), and
   `code-gen/sum_to_zero.stan` joins the table above when the transform
   lands.
2. **A user `_lpdf` called normalized stays unnormalized.** `f_lpdf`
   whose body calls `normal_lupdf` must drop the normalizing constant
   only when the caller wrote `f_lupdf`; stanli drops it either way. In
   the model block that is invisible in the gradient, but `lp__` is wrong
   by the constant, and a `transformed parameters` or `generated
   quantities` value computed that way is wrong by it too. Repro:
   `target += f_lpdf(mu | 1.0)` gives -0.405 where CmdStan gives
   -1.3239385332046729.
3. **`write_array` transposes an array of matrices.** For
   `array[2] matrix[2,3] m`, the columns are named
   `m.1.1.1, m.2.1.1, m.1.2.1, ...` on both sides but stanli fills each
   matrix row-major where CmdStan fills it column-major. `log_prob` and
   the gradients are unaffected -- the read order is right -- so only the
   reported draw is wrong, which no posteriordb model's write_array
   reference could have shown.

2 and 3 are open.
