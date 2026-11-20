# Where lowering spends its time on a large explicit loop

Date: 2026-11-20. Base: `fix/audit-nov20` at f94c05155, branch
`fix/audit3-lowering`. Evidence for a later decision about lowering an
explicit loop over rows without unrolling it. Nothing here is a timing
claim: the counts are deterministic, and the profile gives proportions only.

## The shape

`tools/gen_size_sweep.py`'s `hierarchical` model
(posteriordb `radon_variable_intercept_centered`): a varying-intercept model
whose likelihood is a loop over N rows,

```stan
for (n in 1:N) {
  mu[n] = alpha[county_idx[n]] + floor_measure[n] * beta;
  target += normal_lpdf(log_radon[n] | mu[n], sigma_y);
}
```

Counts from `STANLI_PROFILE_PREP=1` (`lower` and `lower_work` rows):

| N | statements lowered | raw ops | slots | fills | ops after reroll |
| --- | --- | --- | --- | --- | --- |
| 10^4 | 30,014 | 50,007 | 60,020 | 10,006 | 10 |
| 10^5 | 300,014 | 500,007 | 600,020 | 100,006 | 10 |

Lowering unrolls the loop: 3 statements, 5 ops, 6 slots and 1 fill per row.
Reroll then folds the 500,007 ops back to 10. The work is linear in N, so
this is not a cliff, but every large hand-written loop pays it.

## Where the time goes inside `lower`

One `sample` profile of `bench_grad --prep` at N = 10^6 (the 10^5 data
repeated ten times), 767 samples inside `Lowering::run`. Inclusive shares:

| part | share | what it is |
| --- | --- | --- |
| folding data subexpressions | 40% | `fold_const` on `floor_measure[n]` and `log_radon[n]` for every row: the MIR interpreter evaluates each one (17%), then `constant` makes a constant slot, a fill and an observation for it (21%) |
| constant lookup | 11% (part of the above) | `const_slot` searched a `std::map` that holds one entry per distinct data value |
| observation map | 8% (part of the above) | `observe` and `observation` on a `std::map` keyed by slot |
| name lookups | about 22% | string comparisons in `std::map<std::string, ...>` lookups of `scope`, `int_env`, `td.env()` and `decls`, spread over `lower_expr_impl` (variables), `missing_input`, `needs_runtime_value`, `runtime_scalar_form`, `eval_int` and the interpreter's `find` |
| emitting ops | about 11% | `emit_value` and `finish_emit` |
| allocation | about 10% | `malloc` and `free`, mostly for one-element `DataMap::Entry` vectors and op argument lists |

The `lower` stage is about two thirds of preparation at N = 10^5 in the
profile trace; reroll is most of the rest.

## What changed on this branch

`const_cache` and `observations` are now hash maps. Neither map's order was
read, and entries stay where they are on a rehash, as the borrowed pointers
into `observations` require. In a second profile `const_slot` fell from 88
samples (11%) to 16 (2%). The prepared programs of the corpus do not change.

## What is left, for a design decision

- Each row still turns its data reads into constants one at a time, and
  reroll then has to find the pattern again. A loop over rows whose body
  reads data only through the loop variable could be lowered once with the
  data as a column, which is what reroll produces anyway. That is a new
  mechanism, so this branch does not try it.
- Name lookups compare strings. Interning names once when the MIR is read
  would make each lookup an integer comparison, but it touches every map in
  the lowering.
- The interpreter evaluates `floor_measure[n]` by building a one-element
  entry per row. A direct read of a data element at a known index would skip
  it.
