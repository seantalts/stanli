# Read after BridgeStan's Makefile with the same assignments as model builds.
# Reuse its platform/configuration-specific link prerequisites instead of
# hard-coding library paths. One make process owns shared artifacts before
# the harness starts independent parallel model builds.
.PHONY: stanli-conformance-runtime
stanli-conformance-runtime: $(BRIDGE_O) $(SUNDIALS_TARGETS) $(MPI_TARGETS) $(TBB_TARGETS)
