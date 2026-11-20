# Audit 2026-11-20: questions that need a decision

These items came out of the 2026-11-20 audit. Each one needs a person to say
which reading is intended before anyone changes code or text.

## What a density element costs in the reroll and partition cost model

Where: `kLaneDensityElem` in `runtime/src/pass_util.hpp`, used by
`runtime/src/reroll_plan.hpp` (region pricing) and `runtime/src/partition.cpp`
(bucket pricing).

The comment above the constant says the currencies are about 5 ns per graph
op eliminated, about 1 ns per element that a slice or gather moves, and "~6 op
dispatches to evaluate one density element". The constant is 6.

The arithmetic does not follow that comment. Both passes add `rows *
kLaneDensityElem` to a running total that they otherwise fill with element
counts, and they price one op dispatch at `kLaneOpCost = 5` in the same units.
A density element therefore costs 6 units, which is about 1.2 op dispatches
(6 ns at 5 ns per op). That is a fifth of the six that the comment states.

Two readings are possible:

- The comment is right. A density element costs about 6 op dispatches, so
  `kLaneDensityElem` should be about 30 element units. The passes currently
  under-charge vector densities by about five times, so they fuse more often
  than the stated model allows.
- The constant is right. A density element costs about 6 element units, and
  the comment (and the sentence in `runtime/src/OPTIMIZATIONS.md` that repeats
  it) should say "six element units, about 1.2 op dispatches".

Changing the constant moves which regions reroll and partition fuse,
so a change needs the compiler-pipeline benchmark that AGENTS.md describes.
This audit did not change the constant.
