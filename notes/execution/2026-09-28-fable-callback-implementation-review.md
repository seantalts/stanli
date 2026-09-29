# Fable review: matrix callback implementation

I could not review the higher-order lowering file: the Read tool rejected it twice as too large, even with an offset and limit, and that used my last permitted call. Everything below about graph, program, and quadrature packing in `runtime/src/lower_higher_order.cpp` is therefore inferred from its callers and the helper contract, not from the code.

## What I verified from the code

- **Legacy path is untouched.** The fallback helper returns the old positional call whenever no depth-0 matrix formal exists. Scalar, vector, and one-dimensional array callbacks cannot reach the typed overload.
- **Typed binding matches positional binding for non-matrix formals.** Both paths give scalars no dims, give vectors and one-dimensional arrays a single dim, and mirror ints into the real vector. The typed path is strictly stricter: it throws on missing or surplus arguments where the positional path would silently bind reals into an int formal.
- **Geometry is checked before admission and before interpretation.** Rank, non-negative extents, the checked extent product against the flattened length, and the int-range bound are all enforced by one helper, called from both compilers and from the fallback. Malformed geometry becomes a compiler refusal at preparation and an exception at interpretation. The C++ test covers all six malformed cases plus the array-of-matrix rejection.
- **State formals cannot be matrices.** The prefix logic refuses a matrix in the state slots, and every kernel signature puts scalars or vectors there, so this branch is unreachable by well-typed Stan.
- **Effects run once.** The value-only packer evaluates each argument once, and the C++ test asserts the message sink fires exactly once per interpreted call.
- **Zero extents work when the RhsArg is built by hand.** The 0x3, 3x0, and 0x0 cases pass through both compilers and the fallback in the C++ test.

## Concrete risks

1. **Zero-row data matrices from JSON may lose their second extent (guess, grounded in a comment).** The value-only packer only records geometry when the evaluated entry has exactly two dims. The interpreter comment at `runtime/include/stanli/mir_interp.hpp:112` says an empty JSON array cannot encode trailing extents. If a data `matrix[0,3]` arrives with dims of length one or zero, rows and cols stay at -1, compilation refuses, and the fallback then throws at solve time. That is fail-closed, but it turns a legal model into a hard error with a misleading message. The C++ zero-extent test bypasses this because it constructs the RhsArg directly. Disposition: add a fixture that passes an empty data matrix through JSON in the generated-quantities path and, if the same guard exists in graph lowering, there too.

2. **Silent guard instead of a preparation-time error.** The same `dims.size() == 2` guard in `runtime/src/higher_order_eval.cpp:89` swallows any upstream dims loss and defers the failure to the kernel. Disposition: throw a CompileError naming the argument when a matrix-typed expression has the wrong rank, so the refusal appears at preparation with a clear cause.

3. **Two different matrix predicates in the value-only packer.** Storage-order suppression keys on the type string being `UMatrix`, while geometry capture keys on the unsized view. If those ever disagree, a two-dim entry would be reordered as an outer-major array while still being tagged as a matrix. I have no evidence they disagree today. Disposition: use the unsized view for both.

4. **Matrix argument expressions that are not plain variables (unverified).** Products, transposes, blocks, and `to_matrix` results must carry rows and cols on whatever the graph packer reads. A transpose represented as a strided view would flatten in the wrong order while still reporting the right extents, and no geometry check can catch that. The shipped fixtures appear to pass named matrices. Disposition: add a graph-path fixture whose matrix argument is a transpose and one that is a block or product, and compare against CmdStan.

5. **Source-order interleaving of active and data args (unverified).** The fallback consumes reals in formal order, so every adapter must rebuild reals interleaved by source position, not as all-theta then all-x_r. The old positional path had the same requirement, so the adapters presumably already do this, but the new C++ test only exercises active-before-data. Disposition: one fixture with a data matrix before an active matrix, and an int array between them.

6. **Declared dims now set for callback formals.** The typed overload calls `set_declared_dims` for each formal while the positional overload never did. That changes what FnCheck sees for vector and array formals inside matrix callbacks only. Probably more correct, but it is a behavioral difference the "positional call exactly" claim does not cover for the matrix case. Disposition: note it in the plan; no change needed unless a check-statement fixture diverges.

## Minor observations

- The geometry helper runs twice per matrix argument in each compiler, once in the sizing loop and once in the stamp. Harmless.
- The typed path copies each real vector twice on the fallback route. Only the interpreted fallback pays this.
- The Python test asserts the generated-quantities refusal by substring match, which will break on any wording change. Acceptable for now.

Overall the bounded slice looks sound where I could see it. The two items worth closing before merge are the JSON empty-matrix behavior and a transpose-argument oracle fixture, since neither is covered by the current tests and both would fail silently or with a misleading error rather than a wrong number.

## Follow-up packing review

No confirmed issues in the three packing sites as excerpted. Everything I could check against the stated invariants holds. Details and what I could not verify:

**Checked and consistent**

- **Column-major values.** The Program site converts data entries through `graph_order` with the nested-matrix flag. The quadrature and ODE sites take `require_constant_reals` output as-is. That is only correct if that accessor already yields graph order for matrix data. The geometry fixture pins the ODE site with a data transpose against CmdStan, so depth-0 matrix data at that site is empirically confirmed.
- **Complete extents.** All three sites set rows and cols only for depth-0 matrix args. Active args take extents from the lowered value's shape info, data args from the memoized entry dims when there are two, else from the logical shape of the lowered value. A matrix-typed active arg that lowers to a non-matrix view leaves extents absent, which the downstream validator rejects rather than miscomputing. That is a safe fallback, not a silent error.
- **Source ordering and effects.** The ODE site walks arguments in index order in one loop and pushes each binding in that order, so data, int, and active args interleave exactly as written. The other two sites delegate to the shared packing helper with the same per-index lambdas. Active-arg lowering happens inside the lambda, so graph or program emission order matches source order as long as the helper iterates in order.
- **Empty matrices.** Entry dims of 0x3 and 3x0 pass straight through as extents with an empty value vector. Nothing in the excerpt special-cases zero extents, so no place to go wrong there. The fixture covers both.

**Minor observations, not defects**

- **Dead graph node on shape fallback.** In the quadrature and ODE sites, the fallback path calls `value()` on a data-only argument purely to read its shape. Memoization prevents double evaluation, and data expressions have no effects, but the lowered node stays in the graph unused. Cost only.
- **No overflow guard at the ODE site.** The quadrature site rejects arguments longer than int range before the cast. The ODE site casts `vals.size()` to int without that check. Theoretical only at realistic sizes.

**Scope limitations**

- The packing helper, `graph_order`, `require_constant_reals`, `logical_shape`, and the theta copy loop at the Program site are not in the excerpt. In-order iteration and the column-major claim for the two non-Program sites rest on the invariants you stated plus the ODE fixture, not on code I read.
- The fixture exercises only the ODE site with depth-0 matrices. Nested array-of-matrix data at the quadrature and ODE sites has no explicit reorder call and no fixture coverage I could see. The Program site handles that case through the nested flag. Whether the shared accessor already reorders nested entries is the one remaining question I would resolve by reading it.
