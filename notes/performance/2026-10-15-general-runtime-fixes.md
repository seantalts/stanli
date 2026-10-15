# General runtime fixes extracted from the ISA investigation

The candidate starts at `cc99a937c201f732d9fd6cb35b32ebd5db5fc935` and
retains the existing minimum ISA and floating-point flags. Two changes reuse
existing runtime infrastructure:

- The private function registry uses the Boost flat map already used in
  lowering, with one inline overload pointer per name. Canonical specification
  storage, overload insertion order, family views, and the public API remain
  unchanged. Reserving 512 specification entries avoids the current growth
  from 384 to 768. Earlier allocation attribution measured 52 index allocations
  versus 864 and approximately 4.8 KiB more retained requested storage overall;
  those are allocator requests, not RSS measurements.
- The existing late forward-callback resolver selects an arity-specialized
  version of the original `ADD_N` loop. Every version accumulates from zero
  in input order and writes only after reading all inputs. Dynamic lengths,
  unrecognized variants, and custom kernel registrations keep their original
  callback. This extraction handles upstream's scalar inputs only; the
  research branch's separate indexed-input extension is excluded.

The ADD_N regression test covers all supported arities, eight buffer
alignments, output/input aliasing, and fallback selection. The existing
registry test checks every overload and canonical pointer identity.

## Rejected inclusion: constant sharing

The initial three-fix candidate passed all 406 CTest cases and a strict
353-model, three-point comparison with unchanged upstream, with zero output
changes. Recorded CmdStan replay passed 350/353; three GP failures reproduced
on unchanged upstream with identical errors on this host.

The full performance run was stopped with its partial evidence retained after
five warm-gradient crossings. A six-round independent-process comparison of
baseline, each individual fix, the combination, and an identical-binary
control confirmed slowdowns from constant sharing: Rate_2_model +4.994%,
aalto_grp_aov +2.574%, aalto_grp_prior_mean +4.501%, and arK +6.443%. The
combination also slowed GLM_Poisson_model +6.396%. These models contain no
register islands in their printed graphs; the constant-sharing transform does
not execute in their warm paths. The actual placement/layout cause remains
unresolved. Mb_model improved 8.91% with constant sharing alone, but that gain
does not excuse the other regressions. Constant sharing is therefore excluded
from this PR and retained in the ISA research artifacts for investigation.

The registry-plus-ADD_N combination passed the same six-case confirmation:
GLM_Poisson_model -0.095%, Rate_2_model +0.187%, aalto_grp_aov -1.031%,
aalto_grp_prior_mean -2.043%, arK +0.048%, and Mb_model +0.313%. Identical-binary
controls ranged from -0.298% to +1.229%; this is focused evidence, not a
full-corpus pass. The complete corpus comparison remains required before merge.
