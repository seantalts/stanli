# The lite build: half the library, lp__ off by a constant

`-DSTANLI_LITE_LP=ON` builds a runtime that is 48% smaller and samples
from the same posterior with bit-identical gradients. It is **off by
default in every build, browser included**, so that any run's `lp__`
can be compared against CmdStan directly. `stanli_exact_lp()` (Python
`stanli.exact_lp()`, JS `fit.exactLp`) reports which build is loaded.

## What it drops

A density is not one function. stan-math decides which terms of a log
density to keep by looking at the argument *types*, so a `~` statement
that drops `-0.5 * log(2*pi)` because `sigma` is data is a different
instantiation from one that keeps it. Supporting `~` exactly costs one
instantiation per activity mask on top of the full form, twice over for
the elementwise variant: `4 * 2^N` copies of stan-math's template per
distribution.

`STANLI_LITE_LP` clears the propto half of that (`density_tier()` in
`runtime/include/stanli/optable.hpp`). `y ~ normal(mu, sigma)` then
evaluates the *full* normal density. That is still a correct log
density; it just is not the one CmdStan computes, so `lp__` lands a
per-model constant away from CmdStan's.

## What it does not drop

Every gradient. The terms propto removes are exactly the ones that are
constant in the active arguments, so they have no derivative to
contribute. Measured over the whole 119-model posteriordb corpus: every
gradient and every `write_array` value is **bitwise identical** to the
exact build, and `lp__` differs by a constant at every evaluation
point.

| | exact | lite |
|---|---|---|
| `libstanli` stripped (macOS arm64) | 15.75 MB | 8.43 MB |
| gradients vs CmdStan | bitwise | bitwise |
| `lp__` vs CmdStan | bitwise | constant offset |
| draws for a pinned seed | -- | different chain, same posterior |
| `stanli_exact_lp()` | 1 | 0 |

Speed is not part of the trade in either direction. Per-gradient
differences between the builds measure under 4% with inconsistent sign,
which is run-to-run variance: the propto choice is internal to a
kernel and never changes the graph. Do not pick the exact build for
speed; pick it because you want CmdStan's `lp__`.

## The draws are not byte-identical, and that is expected

In exact arithmetic a constant lp shift cancels in every Hamiltonian
difference NUTS looks at. In floating point it does not: the sampler
forms `H = -lp + kinetic`, and adding a shifted `lp` rounds
differently. The difference starts at one ULP and NUTS is chaotic, so
it grows: on eight schools at the same seed, `mu` differs by 2.0e-15
after 5 warmup iterations, 6.3e-13 after 20, and 1.3e-09 after 50. This
is the same class of difference as changing the seed; every draw is
from the same posterior, and no draw is byte-comparable. A run that
must reproduce another run byte for byte needs both to be the same
build.

## How the claim is checked

`tools/verify_lite.py` replays both builds over the corpus and enforces
the two things the flag promises:

```
cmake -B build     -DCMAKE_BUILD_TYPE=Release
cmake -B build-lite -DCMAKE_BUILD_TYPE=Release -DSTANLI_LITE_LP=ON
build_jobs=$(tools/build_jobs.sh)
cmake --build build --parallel "$build_jobs" --target stanli_check
cmake --build build-lite --parallel "$build_jobs" --target stanli_check
python3 tools/verify_lite.py deps/posteriordb
```

1. **Gradients bitwise.** Not "close": the same computation.
2. **The lp shift is constant.** At three points in the unconstrained
   space, `lp_exact - lp_lite` must be the same number. A shift that
   moved with the parameters would mean a dropped term was not constant
   after all, which is the only way this flag can be wrong.

The gate on the shift is relative to `lp`, not to the shift itself:
dropping a term reassociates the sum after it, so the residue lives on
`lp`'s rounding scale (half an ULP of `lp` on the worst model).
Normalizing against the shift's own magnitude would flag that rounding
as failure and teach everyone to ignore the tool.

## When to turn it on

When download or install size matters more than an `lp__` comparable
to CmdStan's: an embedded target, or a size-critical web deployment
(it roughly halves the wasm payload). The published wheels and the demo
page both ship exact, because the differential oracle against CmdStan
is the project's whole claim to correctness and it needs an `lp__` to
compare. Check `stanli_exact_lp()` if you display or compare `lp__`
across engines, or pin a seed and expect the same bytes back.
