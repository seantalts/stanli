# Parked, allocator-independent loop alignment

This branch isolates the existing `STANLI_ALIGN_LOOPS` experiment from
private allocation. OFF remains the default. It is not part of the allocator
integration or a merge/default-promotion request.

ON adds `-falign-loops=32` to optimized Release/RelWithDebInfo consumers of
the Stan Math interface on native single-architecture AppleClang arm64.
That includes static/CLI consumers, so its rollout gate is independent of
the shared allocator. AUTO falls back on unsupported hosts/configurations;
ON refuses them. Sanitizers, LTO, other compilers/architectures and WASM are
not enabled.

Configuration tests cover platform selection, unavailable flags, build
modes, refusal and cached ON-to-OFF rollback. No Stan Math/Eigen source is
changed.

The [immutable research checkpoint](https://github.com/seantalts/stanli/tree/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439)
retains the code-layout investigation, standalone CLI experiment and full
evidence. The [rollout report](https://github.com/seantalts/stanli/blob/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439/docs/allocator-rollout.md)
records inconclusive CLI controls and a negative ODE signal; the
[allocator/alignment ablation](https://github.com/seantalts/stanli/blob/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439/docs/allocator-regression-investigation.md)
also shows that alignment did not resolve the Apple large-vector allocator
loss. Promotion needs its own corrected broad consumer/corpus and
end-to-end checks. No additional performance experiment is underway.
