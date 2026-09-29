# Handoff: lessons from the stanc3 vectorize_loops work that apply to stanli

Context: the stanc3 compiler now has a `vectorize_loops` optimization pass
(stan-dev/stanc3 #1666 merged, #1678 in review, a multi-statement branch
staged). Building it surfaced facts that apply to stanli's runtime
(~/claud/stanrt). Big caveat before investing anywhere: reroll.cpp is
slated for deletion once stanc3 pre-vectorizes the MIR stanli consumes
(see the "REROLL DELETION PLAN" note in the project memory). So work that
improves reroll.cpp itself is throwaway. Work that improves lowering or
verifies correctness survives. Items below are ordered with that in mind.

## 1. Verify: hoisting must not change side-effect counts (correctness check, cheap)

stanc3's reviewer caught a real bug in the pass there: an invariant
argument with side effects (an `_lp` call) was evaluated once after
vectorization instead of N times. stanli's reroll has an analogous
mechanism: an all-invariant op in a lane template is hoisted and emitted
once (`ap.hoist` in runtime/src/reroll.cpp). Verify that no opcode
reachable in a hoisted position can have side effects (reject, print,
target mutation outside the declared output). If every opcode in the
match vocabulary is a pure kernel this is a one-paragraph argument; write
it down next to the hoist arm or as a test. If any impure opcode can
appear in a template, that is a live bug of the same shape stanc3 had.

## 2. Verify: generated-quantities name reuse (correctness check, cheap)

Stan scoping allows a model-block local to reuse the name of a generated
quantities variable at a different size, because GQ names are not in
scope in the model block. This broke stanc3 (a trusted-size map keyed by
name picked up the GQ declaration). Check stanli's lowering and the
interpreted write_array path: anywhere a name-to-slot or name-to-size map
spans blocks, a model local named like a GQ variable (or vice versa) must
not collide. Repro shape:

    model { vector[3] x = rep_vector(1.0, 3); for (i in 1:2) x[i] ~ std_normal(); }
    generated quantities { vector[2] x; }

Run it through stanli_check and confirm lp and the GQ output are right.

## 3. Port to lowering, not reroll: fuse pure-output TP lanes (small win, survives deletion)

Measured on surgical_model: stanli's lp graph keeps 12 scalar INV_LOGIT +
12 scalar INDEX ops because the transformed parameter `p` is a pure
output (read from the arena by write_array, no lp consumer). reroll
correctly refuses these lanes via the extra_roots protection. But the
loop could lower directly to one vector INV_LOGIT plus one vector store
into the arena view, the same way lowering already emits GATHER for
`a[idx[i]]` reads. Doing this in lowering (not reroll) survives reroll's
deletion. Expected effect is small per model (surgical drops ~24 ops) but
the pattern shows up in several models' residual scalar counts
(accel_gp's census had similar leftovers). Judge whether it is worth the
lowering complexity; simplicity beats perf for stanli per standing
preference.

## 4. Optional one-liner: kMinLanes

reroll refuses runs shorter than kMinLanes=4 (runtime/src/reroll.cpp).
lotka_volterra's y_init loops (2 lanes) and pilots' small group priors
stay scalar because of it. stanc3 vectorizes those and measures ~1.0x,
so the win is near zero. Only lower the threshold if some profile shows
2-3 lane regions mattering; otherwise leave it.

## 5. Port to reroll: position-subset extraction (truncated tildes)

stanc3's multi-statement branch extracts vectorizable target-increment
statements out of loops whose other statements cannot vectorize. This is
how a truncated tilde gets its lpdf term vectorized while the bounds
check keeps the loop. reroll has no equivalent: a region whose period
contains one unclassifiable position bails whole (reroll.cpp, the
`classified` check after the position loop).

The port: when classification fails, check whether the classifiable
positions include term densities, i.e. positions where every lane's
output is a target term. Those positions can be extracted alone. The
soundness argument has two parts. Target accumulation commutes, so
summing the extracted lanes early does not change the result. And a term
output has no op consumers by definition (reroll already rejects a term
that is also an op input), so no remaining position can observe the
extraction. Mechanically: rewrite just the term-density positions' lanes
to one vector density and swap the lane terms for its output, exactly
what the existing term_density arm does, and emit every other op of the
region unchanged in place. Positions that are not term densities need no
classification at all in this mode.

One check first: confirm what a truncated tilde actually lowers to in
stanli's op graph. If the truncation conditional lowers through
interpreter constructs rather than a periodic op template, the region
may not look periodic at all, and the extraction wants to happen during
lowering instead. dump_ops on a small truncated model answers this in
minutes.

This is reroll investment and the deletion plan argues against those,
but the stanc3 series has no merge date, so the models it would claim
(truncated-tilde loops, mixed bodies where only the densities classify)
are otherwise stuck. Judge by the dump_ops result and by how small the
diff turns out.

## Facts worth knowing, no action

- stanli's lowering already beats reroll on the hierarchical idiom:
  election88_full lowers to 65 ops with 5 vector GATHERs and no unrolled
  region at all. The reroll deletion criterion is therefore the 13
  stanli-only models (arK, dogs x3-ish, gauss_mix x2, normal_mixture_k,
  ldaK5, rats, M0, accel_gp, GLMM1, one_comp, Survey), not the full
  29-model touch list. Full lists in the project memory and in
  scratchpad/reroll_ab_full.txt of the current session.
- The elementwise-map audit of all 24k Stan Math signatures found no
  function with both a scalar signature and a non-elementwise container
  overload reachable by widening. stanli's is_widenable list was
  verified against kernels directly, so nothing to change, but the audit
  is a useful reference if the list ever grows: the dangerous class is
  container-returning overloads of scalar-signature functions, and the
  near-misses are squared_distance and the cdf family (blocked in stanc3
  by return type, not by name).

## Outcomes (2026-08-25, verified against main at v0.8.5)

1. Live bug, fixed in #181. ops_match omitted OP_PRINT and OP_REJECT
   while constfold and island refuse both. The all-invariant hoist arm
   collapsed 8 per-lane prints to 1, and 5 straight-line prints with
   distinct literals deduped to the first (ops_match ignores udata; the
   effect ops write a dead scalar slot the term and escape guards never
   see). The _lp-UDF shape from stanc3 reproduces because UDF inlining
   deposits the print op into the periodic lane body. One-line blocklist
   addition plus a bail test.

2. No collision. Every declaration unconditionally overwrites the
   name-keyed maps (lower.cpp Decl arms, mir_interp Decl), and
   int_env_data snapshots before any section's locals fold in. Fourteen
   paired repros against renamed controls were bitwise identical. The
   probe found an unrelated bug instead: the graph write_array path
   zero-filled never-assigned elements of a bare container where CmdStan
   and the interpreter NaN-fill. Fixed in #182 with the shadowing
   contract pinned by test.

3. Refuted on current main. surgical keeps 1 residual scalar lane, not
   12: lowering emits lane 0 as SET_INDEX and later lanes as
   SET_INDEX_INPLACE, so lane 0 never joins the template. About 4 ops
   per element-store loop across the corpus. Skipped.

4. Left alone, as recommended.

5. Probed, then skipped. Truncated tildes lower to strictly periodic
   templates; reroll bails the whole region because NORMAL_LCCDF and
   NORMAL_LCDF have no op_traits entry. Opting the cdfs into the density
   vocabulary does not work: the correction feeds NEG rather than
   target, which routes to the elt_density arm, and the elementwise cdf
   variant deliberately throws. The workable shape is a summed-vector
   cdf disposition (the summed kernels exist; hand-vectorized gradients
   matched the scalar lanes to the last ulp). Zero of 240 corpus models
   use T[], so the pass investment is unjustified while the stanc3
   series covers the same ground.


## 6. Design rule with a fresh proof: synthesize only shapes reachable from source

Sequel to the stanc3 whole-variable assignment story. The pass wanted to
turn its sliced assignment `v[1:N] = rhs` into a plain `v = rhs` because
the slice costs an extra indexed pass at runtime (radon_county measured
0.92x against its own scalar loop). Emitting an assignment with no index
broke: the frontend never produces that statement where the pass put it,
and it walked codegen paths that do not compile for some type mixes.
Reverted. The working fix emits `v[ : ] = rhs` instead. That is a shape a
user can write by hand, so every downstream stage handles it, including
the SoA-into-AoS mix that broke the no-index form, and it reduces to the
same plain assign in the generated code. radon_county went 0.92x to
1.08x and election88_full 1.56x to 1.62x from this one change.

The rule for stanli: when a pass synthesizes graph structure, prefer the
op and variant combinations that lowering itself produces, because those
are the ones every kernel and the interpreter see constantly. A
pass-only combination (a variant bit set on an opcode lowering never
emits it on, a stride pattern only the rewriter creates) is only as
tested as its own unit tests. Where reroll or inplace need a shape
lowering cannot produce, that is exactly where to concentrate tests, or
better, find the nearest lowering-reachable equivalent the way `[ : ]`
substituted for the missing no-index form.
