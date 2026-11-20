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

### Decision (2026-11-20): the constant is right, the comment was wrong

The comment and `OPTIMIZATIONS.md` now say "6 element units, a little more
than one op dispatch". No decision changed. The reasons:

- The constant arrived in 648a490c6 ("perf: partition slices 2 and 3"). In
  that commit every use of it adds to `lane_elems` or to `added`, which hold
  element units (a slice adds `2 * width`, a gather `2 * L * width`). Only
  `ops_out` is multiplied by `kOpCost`. The arithmetic has been in element
  units from the start; later edits (85d7570b8, 39f73f3f0) moved the constant
  into `pass_util.hpp` without changing how it is used.
- The same commit message gives the measurements that the constant prices:
  bernoulli_logit costs 10.6 ns per element in its summed form and 38 ns per
  element through the recorder. At about 1 ns per element unit, 6 units is
  the same order as the summed form's cost per element, which is what
  `lane_elems` charges for re-evaluating a duplicate lane. Thirty units would
  price a vector density element at 30 ns, three times the measured 10.6 ns.
- The island carver prices a whole scalar density op at 2 dispatches
  (`graph_op_cost` in `island.cpp`), not 6 per element.

One open point remains, and it is a calibration question, not an
arithmetic bug. The constant also prices the extra cost of the recorder
form (`added += L * rows * kLaneDensityElem` when
`lane_elt_costs_per_element` holds). The commit measured that extra cost at
about 27 ns per element (38 minus 10.6), and the code charges 6. The commit
reports that this charge was enough to make the measured case decline. Raising
it would change which buckets reroll and partition fuse, so it needs the
compiler-pipeline benchmark that AGENTS.md describes, with models outside the
original census.
