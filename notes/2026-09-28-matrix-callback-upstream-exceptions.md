# Pinned Stan Math expression arguments: two oracle limitations

During the matrix callback geometry audit, two issues arose in the independent
CmdStan 2.40.0 oracle (Stan Math 5252d51d47c1). Neither was fixed by changing
Stanli or weakening a tolerance.

1. Passing `block(A', 1, 1, 2, 3)` directly as an active ODE argument fails C++
   compilation in `stan/math/rev/core/save_varis.hpp:120`. Its linear `coeff(i)`
   accessor is instantiated on an Eigen block-of-transpose expression without
   LinearAccessBit. The full compiler trace is retained in the local experiment
   artifact `.cache/callback-geometry/edge-oracle-compile.log`.
2. Passing `A'` directly as the active argument in the fallback solve of the
   geometry-edge fixture produces the correct states but an incorrect gradient
   in the independent oracle. At rate=0.1 both implementations return
   `answer=fallback_answer=[0.45300000000000007,-0.184]` and
   `lp=-0.24406500000000006`. The oracle reports gradient
   `-0.32510000000000006`; Stanli reports `-0.22460000000000005`.

The RHS does not depend on state. Its exact solved states as functions of rate
q are `s1=0.423+0.3*q` and `s2=-0.224+0.4*q`. The model's proportional target is
`-q*q/2-s1*s1-s2*s2`; its derivative is `-q-0.6*s1-0.8*s2`, or `-0.2246` at
q=0.1. Thus the discrepancy is not explained by adaptive integration or a
small floating-point difference. It is consistent with sensitivity argument
ordering for a transposed Eigen view; the precise upstream root cause remains
unconfirmed.

The committed `matrix_callback_geometry_edges` fixture passes active
`A' * diag_matrix(rep_vector(1,3))` expressions to both solves. This preserves
the mathematical argument, materializes its values in the upstream C++ path,
and still tests expression-valued active matrices. The data argument remains a
direct `B'`; generated quantities also retain a direct active-source transpose
in a value-only call. The complete final reference is newly recorded from this
exact source, at all three points, and passes at 0 ULP (42 values).

To reproduce the gradient discrepancy from that fixture, replace the active
product argument of `dynamic_rhs` with `A'`. To reproduce the C++ failure,
replace the active product argument of `rhs` with `block(A',1,1,2,3)`. The
original source and oracle output are retained locally as
`.cache/callback-geometry/upstream-transpose-repro.stan` and
`upstream-transpose-reference.json`.

This is a scoped exception to exact upstream numerical agreement, not proof
that arbitrary matrix expressions share either problem. No issue was posted
externally and no upstream dependency was changed.
