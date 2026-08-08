# The lite build: half the library, lp__ off by a constant

`-DSTANLI_LITE_LP=ON` builds a runtime that is 48% smaller and samples
from the same posterior with bit-identical gradients. It is on by default
for emscripten and off everywhere else.

## What it drops

A density is not one function. stan-math decides which terms of a log
density to keep by looking at the argument *types* — `include_summand<propto,
T_y, T_loc, T_scale>` is a compile-time query — so a `~` statement that
drops `-0.5 * log(2π)` because `sigma` is data is a *different
instantiation* from one that keeps it. Supporting `~` exactly therefore
costs one instantiation per activity mask, on top of the full form, twice
over for the elementwise variant: `4 * 2^N` copies of stan-math's template
per distribution, about 630 KB of object for a three-argument one.

`STANLI_LITE_LP` clears the propto half of that (`density_tier()` in
`runtime/include/stanli/optable.hpp`). `y ~ normal(mu, sigma)` then
evaluates the *full* normal density. That is still a correct log density —
it just is not the one CmdStan computes, so `lp__` lands a per-model
constant above CmdStan's.

## What it does not drop

Every gradient. The terms propto removes are exactly the ones that are
constant in the active arguments, so they have no derivative to contribute.

Measured over the whole 119-model posteriordb corpus: every gradient and
every `write_array` value is **bitwise identical** to the exact build, and
`lp__` differs by a constant at every evaluation point.

| | exact | lite |
|---|---|---|
| `libstanli` stripped (macOS arm64) | 14.93 MB | 7.79 MB |
| gradients vs CmdStan | bitwise | bitwise |
| `lp__` vs CmdStan | bitwise | constant offset |
| draws for a pinned seed | — | different chain, same posterior |
| `stanli_exact_lp()` | 1 | 0 |

### The draws are not byte-identical, and that is expected

It is tempting to reason: the constant cancels in every Hamiltonian
*difference* NUTS looks at — the Metropolis ratio, the multinomial
weights, the divergence test — so the trajectory must be identical. In
exact arithmetic that is true. In floating point it is not: the sampler
forms `H = -lp + kinetic`, and adding a shifted `lp` to the kinetic energy
**rounds differently**. The difference starts at one ULP and NUTS is
chaotic, so it grows.

Measured on eight schools, same seed, exact against lite:

| warmup iterations | relative difference in `mu` |
|---|---|
| 5 | 2.0e-15 |
| 20 | 6.3e-13 |
| 50 | 1.3e-09 |

Both chains start from the identical initial point and take the identical
number of gradient evaluations; they separate purely by amplification.
This is the same class of difference as changing the seed — every draw is
from the same posterior, and no draw is byte-comparable. If you need a run
that reproduces another run byte for byte, both must be the same build.
0.2.1 carries the same caveat for a different reason (the RNG change).

## How the claim is checked

`tools/verify_lite.py` is the oracle. It replays both builds over the
corpus and enforces the two things the flag actually promises:

```
cmake -B build     -DCMAKE_BUILD_TYPE=Release
cmake -B build-lite -DCMAKE_BUILD_TYPE=Release -DSTANLI_LITE_LP=ON
cmake --build build --target stanli_check -j8
cmake --build build-lite --target stanli_check -j8
python3 tools/verify_lite.py deps/posteriordb
```

1. **Gradients bitwise.** Not "close" — the same computation.
2. **The lp shift is constant.** Evaluated at three points in the
   unconstrained space, `lp_exact - lp_lite` must come out the same every
   time. This is the load-bearing check: a shift that *moved* with the
   parameters would mean a dropped term was not constant after all, which
   is the only way this flag can be wrong, and it would show up as an O(1)
   relative change.

The gate on the shift is relative to `lp`, not to the shift. Dropping a
term reassociates the sum after it, so the residue lives on `lp`'s rounding
scale: `rats_model`'s `lp` is -4.7e6 (one ULP is 9.3e-10) and its shift
moves by 4.7e-10 — half an ULP of the number it was subtracted from.
Normalizing against the shift's own magnitude instead would flag that as a
failure and teach everyone to ignore the tool.

## When to use which

**The Python wheel is exact and should stay that way.** The differential
oracle against CmdStan is the project's whole claim to correctness, and it
needs an `lp__` to compare.

**The browser build is lite.** The runtime has to be downloaded before
anything happens, nobody is diffing a browser demo's `lp__` against
CmdStan, and the posterior is the same one.

Callers can ask: `stanli_exact_lp()` in C, `stanli.exact_lp()` in Python,
`fit.exactLp` in JS. Check it if you display or compare `lp__` across
engines, or if you pin a seed and expect the same bytes back. Anything
that just wants draws from the posterior can ignore it.
