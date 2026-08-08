# Distribution coverage

What stanli lowers, what it does not, and why. Counts are against
`stanc --dump-stan-math-signatures`, which is what the Stan language
actually offers rather than what someone typed into a table here; `harnesses/fn_sweep.py` checks each one against CmdStan
with a generated single-function model.

| family | supported |
|---|---|
| densities (`_lpdf`, `_lpmf`) | 47 / 72 |
| distribution functions (`_cdf`, `_lcdf`, `_lccdf`) | 90 / 105 |
| scalar math (all-real signature) | 47 / 129 |

Everything supported matches CmdStan **bitwise** -- 0 ULP on every
argument
slot, at three evaluation points. That is the standard here; a density that
merely agrees to 1e-12 is a bug report, not a feature.

Adding one is a line in an X-macro list in
`runtime/include/stanli/optable.hpp`, which generates the opcode, the
kernel, its registration, the lowering table entry and the interpreter
branch together. See `docs/hacking.md`.

## The gaps, and what each one actually needs

### Ordinal regression -- `ordered_logistic` is in, `ordered_probit` is not

`ordered_logistic` works and matches CmdStan bitwise. Getting there needed
three things, and the first was a wrong diagnosis: the recorder's Eigen
edge already *has* `partials_vec_`, so the vector-of-vectors partials were
never the problem. What actually blocked it was that the failing edge was
the **scalar** one -- `bind_args_m` compiles both shape branches, and a
cutpoint set bound as a scalar picks stan-math overloads the scalar edge
cannot serve. `VecMask` (densities.cpp) marks an argument as a vector
whatever its length, which is correct anyway: a one-element cutpoint set
is a one-element vector.

The other two: the outcome goes over as a `std::vector<int>`, because
`ordered_logistic` asks `scalar_seq_view` for a mutable `data()` that an
`Eigen::Map<const VectorXi>` cannot give (and a `std::vector` is what
CmdStan's generated code passes, so it is the instantiation the references
came from); and `rvar` gained a `Scalar` member -- see below.

`ordered_probit` is still out, for the recorder's structural reason:
`c_vec[i].coeff(0) - lambda_vec[i]` does arithmetic on the scalar type.

### Multivariate -- wishart, `multi_student_t`, `multi_normal_prec`,
`multi_gp`, `lkj_corr`, `gaussian_dlm_obs`

Matrix arguments, each with its own shape plumbing. `multi_normal`,
`multi_normal_cholesky`, `lkj_corr_cholesky` and `dirichlet` are already
in as hand-written kernels; these would follow the same pattern, one at a
time.

### GLM fast paths -- `poisson_log_glm`, `neg_binomial_2_log_glm`,
`binomial_logit_glm`, `categorical_logit_glm`, `ordered_logistic_glm`

`bernoulli_logit_glm` is in and shows the shape: a data matrix in a
row-major slot with its dims in `idata`. These matter for coverage rather
than speed -- brms and rstanarm emit the GLM form directly, so a model
using one does not merely run slower, it does not run.

### Multinomial family -- `multinomial`, `multinomial_logit`,
`dirichlet_multinomial`, `beta_binomial`, `hypergeometric`,
`discrete_range`

Integer outcomes that are not one int per lane: an array of counts, or two
int groups. `binomial` already carries two groups
(`with_int_group` in `densities.cpp`); the rest need their own layouts.
`hypergeometric` and `discrete_range` have no real arguments at all, so
they contribute a constant and no gradient.

### The 18 remaining distribution functions

`binomial` and `beta_binomial` carry a second int group (the trial count)
and `discrete_range` is integers all the way down, so those nine want a
layout rather than a list line -- `with_int_group` in `densities.cpp`
shows
the shape.

`neg_binomial_2`'s three reparameterize to `neg_binomial` by computing
`phi / mu` **on the scalar type**, which is the recorder's hard limit
again (see below). The other six are `von_mises` and
`skew_double_exponential`, also below.

### Two that will not work as they stand

- **`von_mises_cdf`** does `res *= 0.0` on the scalar type at a degenerate
  endpoint. The recorder computes in doubles and carries no tape, so that
  assignment would change the value and leave the partials describing the
  old one. `rvar` deliberately has no arithmetic operators, which is why
  this fails to compile rather than silently producing a wrong gradient.
`skew_double_exponential`'s three cdfs used to be listed here for a
related reason and are now in. The cause was the same missing trait: we
register `is_fvar<rvar>`, and stan-math's fvar `value_type` specialization
is written `typename std::decay_t<T>::Scalar` -- which applies to
`const rvar&` as much as to `rvar`, and asked for a member `rvar` did not
have. A real `fvar` defines `Scalar`; we claimed the trait without
honouring that part of its contract. One member declaration, and three
cdfs plus `ordered_logistic` compiled.

### `wiener_lpdf`

The only remaining all-real scalar density, and it fails the same way
`von_mises_cdf` does: it computes with the scalar type directly
(`square(y - tau)`, comparisons against doubles) rather than deriving
partials in doubles. Same fix would be needed -- a tape, or a
hand-written
kernel.

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
