# Elementwise-lp densities and batched mixture kernels

**Goal:** vectorize the one remaining loser shape in the corpus — per-
observation densities whose results feed `log_mix`/`log_sum_exp` instead of
the target — without writing any new math: stan-math computes every value
and every partial, exactly as it does today.

## The shape, concretely

`low_dim_gauss_mix` (N=500), lane period 7 after lowering:

```
INDEX(mu, 0)      hoistable (same element every lane)
INDEX(sigma, 0)   hoistable
NORMAL_LPDF v=06  (y_n, mu1, sig1) -> lp1     out feeds an op, not the target
INDEX(mu, 1)      hoistable
INDEX(sigma, 1)   hoistable
NORMAL_LPDF v=06  (y_n, mu2, sig2) -> lp2     out feeds an op, not the target
LOG_MIX           (theta, lp1, lp2) -> term   the lane's target term
```

Re-roll refuses the region at the density positions: its density rule
requires every lane's density output to BE a target term, because the fused
vector kernel returns one summed lp. Here the outputs are inputs to
`LOG_MIX`, so the whole lane stays scalar, at ~17-20 ns of per-op overhead
per op per gradient, both sweeps.

Affected corpus models and their current per-gradient ratios:
`low_dim_gauss_mix` 0.78x, `normal_mixture_k` 0.51x, `ldaK2`/`ldaK5` ~0.5x,
`Mt/Mth/Mtbh_model` 0.42-0.48x, `Survey_model`, and pieces of the occupancy
models. The hand-vectorized spike (`harnesses/low_dim_gauss_mix_vec.stan`)
put the available win at 2x+ for gauss_mix alone.

## Design: three pieces, one re-roll rule

### A. Batched `LOG_MIX` / `LSE2` kernels (runtime/kernels/mixture.cpp)

Today both are scalar-only. Give them the same shape dispatch the
elementwise binaries have: any argument may be len 1 (broadcast) or len N,
out is len N, and

```
out[n] = log_mix(theta[n or 0], a[n or 0], b[n or 0])
```

The existing forward already replays each element's math on stan-math
(`legacy_fwd_partials_scalars`) and stashes partials in scratch; the batched
form loops that per element inside ONE kernel — scratch becomes
`3 * out.len`, the backward scales `scratch[3n + k]` by `out_adj_vec[n]`
and accumulates into each argument's adjoint (summed when the argument is a
broadcast scalar). Per-element math is bit-identical to the scalar op run N
times, so the unit test can demand bitwise equality against a loop of the
scalar kernel.

`backward_ignores_input_values` already lists both opcodes; the batched
forms keep partials in scratch, so the whitelist entry stays true and
tests/test_pass_safety.cpp's poison test covers them unchanged.

### B. An elementwise-lp variant of the density kernels
   (runtime/kernels/densities.cpp)

Variant byte: bits 0-5 are per-argument activity, bit 7 is propto. Bit 6
becomes **elementwise**: `out` is len N and `out[n]` is element n's lp
instead of the sum.

Mechanically this is a sibling of `density_fwd_v` / `density_bwd`:

- `density_fwd_elt<NArgs>`: loop n = 0..N-1, bind each argument's element
  (or its single value when len 1) as scalar `rvar`s, call the SAME
  stan-math prim lpdf the scalar lanes call today, write the value to
  `out[n]` and the per-argument partials to `scratch[k*N + n]`. All math
  and all partials come from stan-math through the recorder, exactly as
  the scalar path does now; what is removed is N-1 executor dispatches and
  context reads per statement.
- `density_bwd_elt<NArgs>`: for active argument k,
  `adj[n] += out_adj_vec[n] * scratch[k*N + n]` when the argument is len
  N, and the sum of that product when it is a broadcast scalar.
- `scratch_size` dispatches on the variant bit: `NArgs * out.len` for
  elementwise (a broadcast scalar still needs per-element partials, since
  each element is scaled by its own adjoint).

propto semantics are unchanged and match CmdStan by construction: CmdStan's
generated code calls the scalar lpdf per element inside the loop too, so
the per-element constants dropped are identical.

### C. The re-roll rule (runtime/src/reroll.cpp)

Three changes, all in the existing classification/rewrite machinery:

1. A density position whose lane outputs are NOT terms is no longer an
   automatic refusal: if every lane's output is consumed only inside its
   own lane (the existing `br_internal` test already computes this), the
   position classifies as an **elementwise density** and the rewrite emits
   the fused op with variant bit 6 set and a len-N output.
2. `OP_LOG_MIX` and `OP_LSE2` join `is_widenable` (their batched kernels
   from piece A make widening the same opcode).
3. A widenable position whose lane outputs are all target terms — which is
   what the widened `LOG_MIX` becomes — emits the widened op plus one
   `OP_SUM_VEC`, and swaps the N lane terms for the summed term using the
   exact term-replacement bookkeeping the density fusion already has.

Refusals that must stay refusals (and get tests): a density output consumed
by another LANE's op (cross-lane mixture recursions), an elementwise
density output that escapes the region, and the scalar-chain rule from the
losscurve bug interacts here too — an elementwise density whose every input
is scalar must produce a broadcast, not a len-N op over scalar inputs.

### D. Follow-on, not in this slice

A row-wise `log_sum_exp` (out[n] = lse of row n of K vectors) for the lda
family's K>2 mixtures, and hmm_marginal-style batched steps. The pieces
above deliberately cover only the binary-mixture idiom, which is what the
non-lda models in the list use.

## Order of work

1. Piece A + unit tests (bitwise vs scalar loop). Small, self-contained.
2. Piece B + unit tests (elementwise values/partials vs the summed kernel
   at tolerance; propto on/off; broadcast args; every mask).
3. Piece C + reroll unit tests (the gauss_mix shape end to end with
   gradient parity, the refusal cases, the scalar-input broadcast case).
4. Corpus A/B (`harnesses/ab_corpus.py`), direct verify_sample on
   low_dim_gauss_mix, normal_mixture_k, Mt/Mth/Mtbh, Survey_model, ldaK2;
   bench_grad before/after on the same set; docs.

Each step gates on the one before it; the corpus A/B is the acceptance
test for the whole slice, given its record (three real miscompiles caught).

## Expected impact

| model | now | expected |
| --- | ---: | ---: |
| `low_dim_gauss_mix` | 0.78x | ~1.5-2x (spike-measured 2x+ ceiling) |
| `normal_mixture_k` | 0.51x | ~1-1.5x |
| `Mt/Mth/Mtbh_model` | 0.42-0.48x | ~1x |
| `Survey_model` | — | lane fusion where the ADD/LSE2 chain widens |
| `ldaK2`/`ldaK5` | ~0.5x | partial (K>2 rows need the follow-on) |

## Risks

- The elementwise recorder loop keeps stan-math reuse but pays recorder
  overhead (~9 ns/element) that a native elementwise normal would not;
  if the measured win lands well short of the spike's 2x, the documented
  fallback is porting `normal_lpdf` natively as the agreed exception —
  measured first, not assumed.
- Scratch layout changes shape under bit 6 (`NArgs * N` vs sum of lens);
  the bind-time scratch sizing must dispatch on the variant or the arenas
  under- or over-allocate. Covered by the unit tests running both
  variants of the same op back to back.
- Term-swap for a non-density position is new bookkeeping; the fuzz test
  in test_pass_safety and the corpus A/B are the guard.
