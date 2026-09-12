#ifndef STANLI_STRUCTURED_LOOP_HPP
#define STANLI_STRUCTURED_LOOP_HPP

#include <stanli/graph.hpp>
#include <stanli/island.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stanli {

// Fixed logical extents, runtime selections. Matrices keep column-major
// storage inside outer-major arrays. Selector values occupy one packed input.
struct DynamicIndexSpec {
  struct Axis {
    enum Kind { Single, All, Multi, Range } kind = Single;
    int64_t extent = 0, stride = 0, count = 0, input_offset = 0;
    // Runtime logical base extent. -1 means the full fixed capacity.
    int64_t extent_input_offset = -1;
    // -1 keeps count fixed. Multi reads an explicit logical count; Range
    // reads its inclusive runtime endpoint and derives max(0, hi-lo+1).
    int64_t count_input_offset = -1;
    // KernelCtx input descriptor containing each runtime field. Existing
    // hand-built and packed descriptors use input 1 for every field. Native
    // structured lowering may instead bind common-rank selector values
    // directly and keep each offset local to that descriptor.
    int selector_input = 1;
    int extent_input = 1;
    int count_input = 1;
  };
  std::vector<Axis> axes;
  bool matrix_leaf = false;
  int64_t selected_size = 0;
  // Zero preserves the original two-input read / three-input update ABI.
  // A positive value is the exact direct-input arity. Direct updates keep the
  // RHS last and record its descriptor index explicitly; reads and packed
  // updates leave rhs_input at -1.
  int input_count = 0;
  int rhs_input = -1;
};

// Immutable control tree over ordinary graph kernels. A node describes code,
// never a particular iteration. Versions and executed control live in the
// bound executor's kernel state.
struct StructuredLoop {
  struct Node {
    enum Kind {
      Sequence,
      KernelCall,
      Alias,
      If,
      For,
      While,
      Break,
      Continue,
      Target,
      Segment
    } kind = Sequence;
    enum Storage { Retained, Transient, InPlace } storage = Retained;
    bool active = false;
    // The active call still records handles and owns an adjoint, but its
    // historical output primal may live in one per-site workspace cell.
    bool reuse_primal_output = false;
    // Snapshot of the variant whose registered backward contract was used
    // during prepare. Runtime mutation fails closed to ordinary retention.
    uint8_t primal_contract_variant = 0;
    BackwardPrimalReadFn primal_contract = nullptr;
    bool memo = false;
    // A memo node whose values are read by nothing once it exits, so the
    // recording evaluation gives its storage back.
    bool memo_silent = false;
    // If/For/While whose data-only decisions are recorded once and replayed.
    bool trace = false;
    int invariant_loop = -1;
    int memo_index = -1;
    uint32_t site = ~uint32_t{0};
    int64_t workspace = -1;
    int64_t kernel_scratch = 0;
    int loop_index = -1;
    int segment = -1;
    // Live-out slots of a memo node. The first `memo_fresh` may be held by an
    // alias, record or target and get a new version per visit; the rest are
    // only read in place and share one version whose pointer moves.
    std::vector<int> memo_outs;
    size_t memo_fresh = 0;
    // Silent nodes only: slots a traced reader still reads while recording.
    std::vector<int> memo_keep;
    std::vector<Node> children;
    int op = -1;
    int dst = -1, src = -1;
    int condition = -1, iterator = -1, lower = -1, upper = -1;
    void (*forward)(KernelCtx&) = nullptr;
    void (*backward)(KernelCtx&) = nullptr;
  };
  struct Import {
    int slot = -1;
    int input = -1;
    int64_t offset = 0;
    bool active = false;
    bool data_only = false;
  };

  Graph body;  // owns all inner idata and udata
  std::vector<std::pair<int, std::vector<double>>> fills;
  std::vector<Import> imports;
  std::vector<int> outputs;
  bool has_target = false;  // one scalar output after `outputs`
  Node root;
  // Straight-line runs of the body compiled to register programs; a Segment
  // node's `segment` indexes this.
  std::vector<Segment> segments;
  int64_t initial_size = 0;
  int64_t workspace_size = 0;
  size_t node_count = 0, site_count = 0, loop_count = 0, memo_count = 0,
         trace_count = 0;

  // Validate, number sites and loops, and decide every KernelCall's storage
  // class. Throws on malformed trees; builders publish only after success.
  void prepare();
};

void structured_loop_forward(KernelCtx& ctx);
void structured_loop_backward(KernelCtx& ctx);
void register_structured_loop_kernel();

}  // namespace stanli
#endif
