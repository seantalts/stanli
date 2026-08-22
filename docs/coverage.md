# Distribution coverage

What stanli lowers, what it does not, and why. Counts are against
`stanc --dump-stan-math-signatures`, so they track what the Stan
language actually offers; `harnesses/fn_sweep.py` checks each function
against CmdStan with a generated single-function model.

| family | supported |
|---|---|
| densities (`_lpdf`, `_lpmf`) | 71 / 72 |
| distribution functions (`_cdf`, `_lcdf`, `_lccdf`) | 105 / 105 |
| scalar math (all-real signature) | 94 / 100 |

All three rows come from the same place and mean the same thing: a
function name counts as supported when the nightly conformance sweep
reports no `unexpected_unsupported` row for it, over the whole signature
space rather than one probe per name. The scalar-math family is the
distinct names in `stanc --dump-stan-math-signatures` whose arguments and
result are all `real` and whose name carries no distribution suffix.

This used to be three numbers from two different tools with the
definition living in whoever last measured it -- the scalar-math row read
`47 / 129` against no invocation that reproduced either half. It is worth
regenerating them from a green nightly rather than trusting them:

```sh
python3 - <<'EOF'
import json, re, collections
r = json.load(open("conformance-aggregate/conformance.json"))
seen = collections.defaultdict(set)
for row in r["results"]:
    m = re.match(r"signature:([A-Za-z_0-9]+)\(", row["case_id"])
    if m:
        seen[m.group(1)].add(row["status"])
def tally(pred):
    names = [n for n in seen if pred(n)]
    return sum("unexpected_unsupported" not in seen[n] for n in names), len(names)
print(tally(lambda n: n.endswith(("_lpdf", "_lpmf"))))
EOF
```

What is left in scalar math is six names: `hypergeometric_1F0`,
`hypergeometric_2F1`, `inc_beta`, `inv_inc_beta`, and the two
`wiener_*_unnorm` forms.

Every supported density's **gradients** match CmdStan bitwise, at three
evaluation points. That is the standard here: a density whose gradients
merely agree to 1e-12 is a bug report, not a feature.

`lp__` is bitwise too for everything models commonly use. It is not for
the multivariate and multinomial tail, which is built the compact way:
one instantiation instead of `2^N`, so `lp__` comes out a per-model
constant higher while every gradient stays exact. See
[docs/compact-densities.md](compact-densities.md).

Adding a function is one line in an X-macro list in
`runtime/include/stanli/optable.hpp`; see `docs/hacking.md`.

## Three ways in, cheapest first

Not every density needs a kernel.

**1. Rewrite, when the rewrite is exact.** A rewrite must preserve checks as
well as the returned value. In particular, CmdStan checks an all-data
`foo_lpdf<propto=true>` call before dropping its constant term. Stanli
therefore refuses the propto forms of `hypergeometric` and
`discrete_range` until it can execute their support checks; their normalized,
all-data function calls can still constant-fold exactly. A rewrite that
changes arithmetic does not belong here: `multi_normal_prec(y | mu, P)` equals
`multi_normal(y | mu, inverse(P))` mathematically, but the inverse
costs K^3 per evaluation and stops matching CmdStan bitwise. The kernel
was fifteen lines.

**2. Clone, when a signature already exists.** `multi_normal_prec`
takes the same argument shapes as `multi_normal`; `lkj_corr` the same
as `lkj_corr_cholesky`. Both were one template parameter on an existing
kernel. The remaining pairs (`wishart`/`inv_wishart`,
`multi_gp`/`multi_gp_cholesky`, `multi_student_t`/its cholesky form)
are clones of each other, so the first of each pair pays for two.

**3. Write the kernel.** The var-tape-replay pattern is correct by
construction and needs no hand-written derivative: build a nested tape,
call stan-math, `grad()` it. Only the shape plumbing is bespoke, and
the shared helpers (`tail_m`, `tail_v`, `tail_scatter_*` in
`runtime/kernels/matrix_fns.cpp`) reduce each kernel to about twenty
lines. Unlike the recorder, this tier has no restriction on what the
density does with its scalar type: `ordered_probit` and `wiener` use
operators `rvar` deliberately lacks, and both work on a var tape. So do
the five distribution functions that build their result with arithmetic
on the scalar -- `von_mises_cdf`, `von_mises_lcdf`, `von_mises_lccdf`
(`res *= 0.0`) and `neg_binomial_2_lcdf`, `neg_binomial_2_lccdf`
(`phi / (phi + mu)`). Whether a distribution function needs this tier is
mechanical rather than a judgement: `grep -c operands_and_partials` on
its prim header is 2 for everything the recorder takes and 0 for these
five. The price is a nested tape per gradient call, so they are slower
than a listed cdf by roughly the cost of building and sweeping that
tape. These
kernels are **compact** (one instantiation, every argument bound as
autodiff), so `lp__` carries a per-model constant while gradients stay
exact; [docs/compact-densities.md](compact-densities.md) is the
contract.

## What is left

**`hypergeometric` and `discrete_range` propto forms.** Their values are
constant zero, but Stan Math performs support checks first. Until Stanli has
an exact checked implementation, lowering refuses rather than silently
accepting invalid data.

**`discrete_range`'s three.** Integers all the way down, so there is no
real edge for a kernel to differentiate and no layout for one yet.

**`gaussian_dlm_obs`.** It is unsupported for a structural reason: it takes
seven arguments and `Op::in` holds six. Raising the limit would grow every
`Op` and `KernelCtx` in every model for one density, so the lowering refuses
and says why.

**Vector `alpha` in the GLMs.** `bernoulli_logit_glm`,
`poisson_log_glm` and `neg_binomial_2_log_glm` take the intercept as a
scalar; stan-math also allows a per-row vector, and the kernels refuse
that form by name.

**A scalar outcome in those same three GLMs.** `y ~ poisson_log_glm(x, ...)`
wants one outcome per row of `x`; stan-math also accepts a single `int` and
broadcasts it, and the lowering refuses that form by name. The three share
one integer-group layout and one length check, so they are refused together,
and `poisson_log_glm` is why the check is a refusal rather than a
replication: its non-`propto` form subtracts `lgamma(y + 1)` once for a
scalar and once per row for an array -- on four rows of `y = 3`, -4.98
against -10.36, with identical gradients -- so replicating would buy the
right gradient and an lp a constant off CmdStan's. `bernoulli_logit_glm` and
`neg_binomial_2_log_glm` were measured and do not have that problem; they are
refused for uniformity, and either could be replicated if a model wanted it.
`binomial_logit_glm` and `categorical_logit_glm` do replicate, and are
checked to: for those two the scalar and array calls agree bitwise in both
`propto` forms.

## Parameter transforms

All of them, every one bitwise against CmdStan:

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

`offset`/`multiplier` matters most in practice: it is how a modern
model writes a non-centered parameterization, and its offset and
multiplier may themselves be parameters, scalar or per-element.
`corr_matrix` and `cholesky_factor_cov` are what let `lkj_corr` and the
wisharts be *declared* rather than reached through a transformed
parameter.

`harnesses/transform_sweep.py deps/cmdstan` is the oracle: one model
per transform against a CmdStan build at three deterministic points. It
needs a CmdStan checkout, so CI runs `tests/test_newtrans.cpp` instead,
which checks the unconstrained size, the gradient against central
finite differences, and each transform's defining property.

## reject and print

Both supported, in both places they can appear
(`tests/test_rejectprint.cpp`). `reject` throws `std::domain_error`,
the same exception CmdStan's generated code throws, so the sampler
counts a rejected proposal rather than a failed run; `print` writes to
stdout. In `transformed data` the statement runs at lowering time, so a
taken `reject` fails the compile, which is CmdStan failing to construct
the model. In the model block it lowers to an op and throws during the
forward sweep.

Still missing: `reject` under a condition on a *parameter*, because
`lower.cpp` refuses boolean operators on parameters. That is the same
gap as parameter-dependent control flow generally; the register program
already has the branch instructions.

## Truncation and censoring

Supported. `y ~ foo(...) T[l, u]` is rewritten by stanc3 into the
density minus `log_diff_exp` of the bounds' `lcdf`s, and both pieces
are in. Gradients are bitwise against CmdStan; `lp__` is 1 ULP off
because the rewrite accumulates in a different order than CmdStan's
`lp_accum__`. `tests/fixtures/trunc.stan` guards this in CI.

## Checking this yourself

```
harnesses/fn_sweep.py deps/cmdstan            # what we claim, bitwise
harnesses/fn_sweep.py deps/cmdstan --missing  # and the gap
harnesses/fn_sweep.py deps/cmdstan --from-stanc   # every scalar signature stanc knows
```
