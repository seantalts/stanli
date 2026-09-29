# Fable review: retained Cholesky-correlation reverse

Read-only claude --model fable --effort medium, Read-only tool list, eight reads.

## Dispositions

- Values-only state: Executor explicitly prohibits reversing after a values-only forward, and resets reverse readiness before running any kernel. Nevertheless, the kernel now clears eligibility in values-only mode as a defensive local invariant. The next full gradient repopulates retained state; mode-reuse tests pass.
- Scratch geometry: changed the sizing hook to the same batch/raw geometry used by indexing, avoiding reliance on the physical input slot length. This does not authorize malformed shorter inputs; their input reads were already invalid before this change.
- Seed stride: now uses the shared inner_con value passed by structured_bwd rather than independently multiplying K*K.
- Scratch ownership: executor.cpp allocates scratch_ per Executor and binds each context to its own offsets. The existing ordered transforms use the same retained-state contract. No static mutable retained buffers were introduced.
- sqrt zero behavior is excluded from the fast domain and delegated to the old tape. Nonfinite seed/input and saturation tests now pass. Updated plan wording accordingly.
- Primitive arithmetic and Jacobian ordering were independently confirmed by Fable. No tolerance changes were made.

## Review text

Review complete within the 8-read budget. The reverse arithmetic matches the pinned scalar tape operation by operation; the concrete issues are in scratch lifetime and sizing, not in the math.

**Verified against upstream (reads of the pinned files)**

- **Accumulation order.** Per z(k) the tape order is dot-product seed, then the Cholesky consumer (multiply for j>0, square for column 0), then the corr_constrain square/log1m chain, then tanh. The patch reproduces that sequence, and the per-row order add, square, multiply, sqrt, subtract, then the 0.5*log1m edge matches vari creation order in cholesky_corr_constrain.hpp lines 68 to 74. The fresh-zero `0.0 +` starts mirror each vari's zero-initialised adjoint, including the final `adjoint[k] += 0.0 + q`, which equals the fallback's `+= y(i).adj()`.
- **Primitive forms.** square uses `adj * 2.0 * val`, log1m uses `adj / (val - 1.0)`, sqrt uses `adj / (2.0 * vi.val())` with the output value, tanh uses `adj / (cosh*cosh)`. All four are matched literally. Operand swaps in the multiply and 0.5 edges are bitwise safe by IEEE commutativity.
- **Jacobian.** Every log1m term receives exactly out2_adj through pure add nodes, and `0.0 + out2_adj` equals the fallback's `0 + out2_adj * 1.0`, including the -0.0 case.
- **sqrt zero guard.** Not reproduced but subsumed: eligibility requires root > 0 and diagonal > 0, so histories where the guard would fire fall back to the tape. Correct, but the plan's "preserve the guard" wording should say "excluded by eligibility".
- **Batch offsets and k walk.** Per-batch layout 3*raw+K matches between forward and backward, and the reverse k walk decrements exactly raw times per batch in mirror order.

**Concrete issues**

1. **Stale eligibility flag.** Forward writes the flag only when not values-only. A values-only forward followed by a backward would read a stale 1.0 and stale retained arrays from an earlier input, giving a silently wrong gradient. The fix is one line: write 0.0 to the flag in the values-only branch so backward takes the tape. The test suite never exercises this sequence.
2. **Scratch size derives from the slot length, not the op geometry.** The formula uses the input slot's length for the 3*raw term but idata for the K term. Forward and backward index by idata[0] and idata[1]. If a slot is ever a view shorter than nb*inner_raw, the retained writes run out of bounds. Compute the size from idata[0]*idata[1] instead, or assert equality.
3. **Seed stride uses K*K while forward uses idata[2]*idata[3].** These agree today, but the backward should use inner_con for consistency with the copy in forward.
4. **Shared-scratch reentrancy is unverified.** Retained state now lives in per-op scratch across forward and backward. Ordered already relies on this, so the contract exists, but I could not confirm per-chain scratch under threading from the reads allowed.

**Scope limitations**

I did not read corr_constrain.hpp, dot_product.hpp, sum.hpp, the var operator headers, the KernelCtx scratch allocation, or values_only(). The dot-product seed contribution being exact relies on multiplication by 1.0, which holds for any operand order. The eligibility rule is conservative and correct for the checked denominators, but it rejects on values only. Nonfinite seeds are handled identically by both paths only if every intermediate is op-for-op identical, which the analysis supports but the still-running edge tests must confirm. Performance claims, the 21% attribution, and the plan's 10% gate are unmeasured here.
