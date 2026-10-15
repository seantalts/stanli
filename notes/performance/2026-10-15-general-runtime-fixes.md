# General runtime fixes extracted from the ISA investigation

The candidate starts at `cc99a937c201f732d9fd6cb35b32ebd5db5fc935` and
retains the existing minimum ISA and floating-point flags. Three independent
changes reuse existing runtime infrastructure:

- The program compactor shares bit-identical scalar constants using its
  existing alias/remap machinery and scratch vectors. Eligible constants
  are private, single-writer, initialized before their first read, outside
  contiguous ranges, and not direct outputs or seeded inputs. Control flow,
  effects, calls, and ranged computations retain the existing path. Literal
  equality includes signed zero and NaN payload bits. No arithmetic is moved
  or reassociated.
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

Focused tests compare constant-compacted execution with the original program
through native and Stan Math autodiff, including cancellation, signed zero,
non-finite values, range boundaries, output/input ownership, repeated writes,
and reads before initialization. The ADD_N test covers all supported arities,
eight buffer alignments, output/input aliasing, and fallback selection. The
existing registry test checks every overload and canonical pointer identity.

Validation results and the full corpus comparison will be recorded here
before merge. Earlier v3 candidate measurements do not establish performance
for this baseline-ISA extraction.
