# Distribution coverage

What stanli lowers, what it does not, and why. Counts are against
`stanc --dump-stan-math-signatures`, which is what the Stan language
actually offers rather than what someone typed into a table here; `harnesses/fn_sweep.py` checks each one against CmdStan
with a generated single-function model.

| family | supported |
|---|---|
| densities (`_lpdf`, `_lpmf`) | 71 / 72 |
| distribution functions (`_cdf`, `_lcdf`, `_lccdf`) | 90 / 105 |
| scalar math (all-real signature) | 47 / 129 |

Every supported density's **gradients** match CmdStan bitwise, at three
evaluation points. That is the standard here, and a density whose
gradients merely agree to 1e-12 is a bug report, not a feature.

`lp__` is bitwise too for everything models actually lean on. It is
**not** for the multivariate and multinomial tail, which is built the
compact way: one instantiation instead of `2^N`, so `lp__` comes out a
per-model constant higher while every gradient stays exact. Which
densities, why, and how to check:
[docs/compact-densities.md](compact-densities.md).

Adding one is a line in an X-macro list in
`runtime/include/stanli/optable.hpp`, which generates the opcode, the
kernel, its registration, the lowering table entry and the interpreter
branch together. See `docs/hacking.md`.

## Three ways in, cheapest first

Not every density needs a kernel, and reaching for one first is how this
list stayed short longer than it had to.

**1. Rewrite, when the rewrite is exact.** `y ~ foo(...)` with every
argument data contributes EXACTLY zero: CmdStan's generated code calls
`foo_lpdf<propto=true>` on all-double arguments, `include_summand` comes
back false, and the term is dropped before any arithmetic. That is a
general rule, not an approximation, and it needs no kernel at all -- which
is the whole of `hypergeometric` and `discrete_range`, whose arguments are
all integers so every use of them lands here. `target +=` of the same call
is a compile-time constant through the data interpreter. (The one
divergence: a model whose data is outside the density's support -- CmdStan
throws from its checks, stanli contributes 0. Such a model rejects every
draw either way.)

What does NOT belong here is a rewrite that changes the arithmetic.
`multi_normal_prec(y | mu, P)` is `multi_normal(y | mu, inverse(P))`
mathematically, but it costs a K^3 inverse per evaluation and stops
matching CmdStan bitwise, which is the oracle this project runs on. The
kernel below was fifteen lines.

**2. Clone, when a signature already exists.** `multi_normal_prec` takes
the same three arguments in the same shapes as `multi_normal`;
`lkj_corr` the same two as `lkj_corr_cholesky`. Both were one template
parameter on an existing kernel. Worth checking for before writing
anything: the remaining pairs (`wishart`/`inv_wishart`,
`multi_gp`/`multi_gp_cholesky`, `multi_student_t`/its cholesky form) are
clones of each other, so the first of each pair pays for two.

**3. Write the kernel.** The var-tape-replay pattern is correct by
construction and needs no hand-written derivative: build a nested tape,
call stan-math, `grad()` it. What makes each one bespoke is only the SHAPE
PLUMBING -- how a matrix or an array-of-vectors argument reaches the
kernel -- not the math. The shared helpers (`tail_m`, `tail_v`,
`tail_scatter_*` in `runtime/kernels/matrix_fns.cpp`) reduce each to about
twenty lines.

This tier also has no restriction on what the density does with its scalar
type, which the recorder does: `ordered_probit` computes
`c_vec[i].coeff(0) - lambda_vec[i]` and `wiener` does `res *= 0.0`, and
`stan::math::var` has those operators where `rvar` deliberately does not.
Both were written off as unreachable until they were tried on a var tape.

These kernels are **compact** -- one instantiation, every argument bound
as autodiff -- so `lp__` carries a per-model constant while the gradients
stay exact. [docs/compact-densities.md](compact-densities.md) is the
contract.

## What is left

**`gaussian_dlm_obs`** -- the only unsupported density, and for a
structural reason rather than a mathematical one. It takes seven
arguments; `Op::in` holds six. Raising the limit would add bytes to every
`Op` and every `KernelCtx` in every model, for one dynamic-linear-model
density, so the lowering refuses and says why. (`add_op` used to write
past the array without a word: a seven-input op corrupted `n_in` and
surfaced as a SIGBUS inside the kernel. It throws now.)

**Vector `alpha` in the GLMs.** `bernoulli_logit_glm`, `poisson_log_glm`
and `neg_binomial_2_log_glm` take the intercept as a scalar; stan-math
also allows a per-row vector. The kernels refuse that form by name.

## Parameter transforms

All of them, and every one bitwise against CmdStan:

| transform | unconstrained size |
|---|---|
| `lower`, `upper`, `lower, upper` | n |
| `offset`, `multiplier`, `offset, multiplier` | n |
| `simplex[K]` | K-1 |
| `sum_to_zero_vector[K]` | K-1 |
| `unit_vector[K]` | K |
| `ordered[K]`, `positive_ordered[K]` | K |
| `corr_matrix[K]` | K(K-1)/2 |
| `cov_matrix[K]` | K(K+1)/2 |
| `cholesky_factor_corr[K]` | K(K-1)/2 |
| `cholesky_factor_cov[M, N]` | N(N+1)/2 + (M-N)N |

`offset`/`multiplier` is the one that matters most in practice -- it is
how a modern model writes a non-centered parameterization, and how brms
generates one -- and its offset and multiplier may themselves be
parameters, scalar or per-element.

`corr_matrix` and `cholesky_factor_cov` landing here is also what lets
`lkj_corr` and the wisharts be *declared* rather than reached through a
transformed parameter.

`harnesses/transform_sweep.py deps/cmdstan` is the oracle: one model per
transform, compared to a CmdStan build of the same model at three
deterministic points. It needs a CmdStan checkout, so CI runs
`tests/test_newtrans.cpp` instead, which checks the unconstrained size by
hand, the gradient against central finite differences, and each
transform's defining property (a unit_vector has norm 1, a correlation
matrix has unit diagonal, a Cholesky factor has a positive one).

## reject and print

Both supported, in both places they can appear. `reject` throws
`std::domain_error` -- the same exception CmdStan's generated code
throws, so the sampler counts it as a rejected proposal rather than a
failed run -- and `print` writes to stdout.

The two placements are different machinery and both are covered
(`tests/test_rejectprint.cpp`). In `transformed data` the statement runs
in the MIR interpreter at lowering time, so a taken `reject` fails the
compile, which is CmdStan failing to construct the model. In the model
block it lowers to an op and throws during the forward sweep, once per
evaluation.

What is still missing is a `reject` under a condition on a *parameter*,
because the condition is what stanli cannot lower yet, not the reject:
`lower.cpp` refuses boolean operators on parameters. That is the same
gap as parameter-dependent control flow generally, and the path to it is
island v2 -- the register program already has the branch instructions.

## Truncation and censoring

Supported. `y ~ foo(...) T[l, u]` is rewritten by stanc3 into the density
minus `log_diff_exp` of the bounds' `lcdf`s, and both pieces are in.
Gradients are bitwise against CmdStan; `lp__` comes out 1 ULP off, because
the rewrite accumulates its terms in a different order than CmdStan's
`lp_accum__` does. `tests/fixtures/trunc.stan` guards this in CI, where
there is no CmdStan for `fn_sweep` to compare against.

## Checking this yourself

```
harnesses/fn_sweep.py deps/cmdstan            # what we claim, bitwise
harnesses/fn_sweep.py deps/cmdstan --missing  # and the gap
harnesses/fn_sweep.py deps/cmdstan --from-stanc   # every scalar signature stanc knows
```
