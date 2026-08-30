// Bounded-memory execution contract for a sequential loop.
//
// Lowering is deliberately not part of this header.  A ScanSpec is an
// immutable, already-proved plan: the kernel only binds one row, runs its
// acyclic Program, and checkpoints the canonical carry layout.
#ifndef STANLI_SCAN_HPP
#define STANLI_SCAN_HPP

#include <stanli/island.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace stanli {

// Keep the scan's memory/time policy in one place. When every carry boundary
// fits in this additional scratch budget, retaining them avoids the complete
// block-replay sweep during reverse mode. Larger scans keep square-root
// blocking and its bounded-memory behavior.
inline constexpr int64_t kScanFullBoundaryBudgetBytes = 64LL * 1024 * 1024;
inline constexpr int64_t kScanPreparedRetentionBudgetBytes =
    64LL * 1024 * 1024;

int64_t choose_scan_checkpoint_block(
    int64_t count, int64_t carry_cells,
    int64_t full_boundary_budget_bytes = kScanFullBoundaryBudgetBytes);

struct ScanSpec {
  // Copy ctx.in[op_input][input_offset + iteration * iteration_stride, ...)
  // into the step register file.  A zero stride is an invariant binding.
  // `active` means the range may carry a parameter adjoint; inactive data
  // bindings are never visited while harvesting the step adjoint.
  struct InputBinding {
    int32_t op_input = -1;
    int64_t input_offset = 0;
    int64_t iteration_stride = 0;
    int32_t step_reg = 0;
    int64_t len = 0;
    bool active = false;
  };

  // Carry has a canonical packed order shared by every template.  Row zero
  // reads it from the op input.  Each later row reads the preceding step's
  // exit registers.  The final value is written into the packed scan output;
  // reverse mode routes the row-zero carry adjoint back to the same op input.
  // A negative entry_reg means this template does not read the preceding
  // carry in its step program. If exit_reg is non-negative, the template
  // resets the carry from that register (for example, at a new subject). If
  // exit_reg is negative, the carry is unchanged by this template; entry_reg
  // may still be non-negative when the step reads that unchanged value.
  struct CarryBinding {
    int32_t op_input = -1;
    int64_t input_offset = 0;
    int32_t entry_reg = 0;
    int32_t exit_reg = 0;
    int64_t len = 0;
    int64_t output_offset = 0;
    // Template-local output activity. False means a changed exit is a
    // data-only reset and must not be seeded. Identity transitions and carry
    // inputs are routed from entry/exit structure independently: another
    // template may make the same canonical carry parameter-active.
    bool active = true;
  };

  // A row writes one proven-disjoint range in the packed output.  The stride
  // must be non-negative; first-release lowering declines non-affine sinks.
  struct SinkBinding {
    int32_t step_reg = 0;
    int64_t output_offset = 0;
    int64_t iteration_stride = 0;
    int64_t len = 0;
  };

  struct Template {
    // gen_adjoint(step) must have succeeded before the template is published.
    // The generated AdjProgram is stored in IslandProg::adj and reused for
    // every row.
    IslandProg step;
    // Own every payload referenced non-owningly by step.calls[].udata.
    std::vector<std::shared_ptr<void>> udata_pool;
    std::vector<InputBinding> inputs;
    std::vector<CarryBinding> carry;
    std::vector<SinkBinding> sinks;
    int32_t target_reg = -1;

    // A CALL whose inputs are all loop-invariant and whose generated adjoint
    // is absent may run once per template.  Its output has a single writer,
    // so the scan caches that range after the template's first actual use and
    // restores it before running `repeated_code`, which is `step.code` with
    // only those CALL instructions removed.  Scratch is deliberately not
    // cached: an adjoint-free CALL has no backward consumer, and preparation
    // refuses any scratch range read or externally named by the forward.
    struct InvariantCall {
      int32_t call_index = -1;
      int32_t step_reg = 0;
      int32_t len = 0;
      int64_t cache_offset = 0;
    };
    std::vector<InvariantCall> invariant_calls;
    std::vector<Program::Instr> repeated_code;
    int64_t invariant_cache_cells = 0;

    // Force-only replay experiment: an outer step CALL enters a native
    // parameter-conditional island, whose prepared PartialPivLU CALL owns the
    // exact output and factor scratch needed by its backward. One packed
    // record is {valid, output, scratch}. `record_offset` is relative to this
    // template's per-occurrence record; the scan-level prefix below packs only
    // scheduled occurrences of templates which actually contain a solve.
    struct PreparedSolveRetention {
      int32_t outer_call_index = -1;
      int32_t inner_call_index = -1;
      int32_t inner_trace_bit = -1;
      int64_t record_offset = 0;
      int32_t output_len = 0;
      int32_t scratch_len = 0;
    };
    std::vector<PreparedSolveRetention> prepared_solve_retention;
    int64_t prepared_retention_record_cells = 0;
  };

  int64_t first = 0;  // source-language lower bound; diagnostic in Phase 1
  int64_t count = 0;
  std::vector<Template> templates;
  // Empty is the compact one-template schedule.  Otherwise there is exactly
  // one template index per iteration.  A later lowering phase may replace
  // this spelling with RLE without changing the kernel contract.
  std::vector<uint32_t> template_for_iteration;
  int64_t carry_cells = 0;
  // Includes final carry ranges, all sink ranges, and the target scalar.  The
  // target is always the last cell and is zero when target_reg is absent.
  int64_t output_cells = 1;
  // Resolved once while building the immutable plan.  One means retain every
  // row boundary; values above one use square/block checkpoint replay.
  int64_t checkpoint_block = 1;

  // Immutable prefix offsets captured while lowering. Environment selection
  // must happen before Executor scratch is bound: consulting getenv again in
  // scan_fwd could enable more storage than scan_scratch allocated. Empty is
  // the production/default-off representation. Otherwise size is count+1,
  // the final entry equals prepared_retention_cells, and each interval has
  // the selected template's exact record width.
  std::vector<int64_t> prepared_retention_iteration_offsets;
  int64_t prepared_retention_cells = 0;
};

// Conservatively prepare the once-per-template CALL plan described above.
// The step must already have its generated adjoint and all scan entry
// bindings. Returns the number of CALL instructions removed from repeated
// execution. An unsupported/malformed program simply receives no plan; the
// scan's ordinary validator remains the authority on whether it is runnable.
size_t prepare_scan_invariants(ScanSpec::Template* tm);

// Capture the force-only prepared-prim-LU retention plan. The authoritative
// escape is STANLI_NO_SCAN_PREPARED_RETENTION=1; the experiment is selected by
// STANLI_SCAN_PREPARED_RETENTION=1 and is otherwise production-off. The
// update is transactional and fails closed on any unsupported nesting,
// malformed range, schedule, overflow, or budget excess.
size_t prepare_scan_prepared_retention(ScanSpec* spec);

}  // namespace stanli

#endif
