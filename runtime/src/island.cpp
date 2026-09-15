// The island carver: the LAST pass (user decision 2026-08-06 -- in-place,
// forwarding, constfold, and re-roll all get first crack; islands take only
// the scalar residue they provably cannot help).
//
// Scan for maximal runs of consecutive compilable ops and replace each
// qualifying run with one OP_ISLAND (payload: IslandProg in udata_pool)
// followed by one OP_INDEX/OP_SLICE per live-out that writes the ORIGINAL
// live-out slot id. Downstream readers, roots, and target terms never see
// a renamed slot, and adjoints flow through the extraction ops' existing
// backwards.
//
// A run ends at: an opcode outside the vocabulary, a propto density (its
// term-dropping depends on argument types; islands bind everything as T),
// an op producing a target term (terms stay graph-visible), or idata in a
// form the compiler does not model. Elementwise ops compile at any width,
// with length-1 operands broadcast. Runs shorter than kMinIslandOps stay
// as they are: below that, per-op dispatch with scratch partials is cheaper
// than a var replay. A compiled run is then kept only if it is cheaper
// than the ops it replaces -- see the cost estimate at the end of
// carve_islands, which is what decides the pass is a win rather than a
// wash. A run holding vector elementwise ops is also priced split at them,
// and carved whichever way is cheaper.
#include <stanli/island.hpp>

#include <stanli/graph.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/optable.hpp>
#include <stanli/program_density.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {
namespace {

constexpr int64_t kMinIslandOps = 32;
constexpr int kMaxLiveIns = 6;
// What one value register costs against one element of graph traffic. The
// value file is written by the forward and read by the backward. The compact
// adjoint file is charged separately below: copied registers share a cell,
// and checkpoint registers have no adjoint cell at all.
//
// It was 4 while the backward replayed the program under var, where a
// register meant an allocated vari and a virtual chain() call. That term
// dominated the estimate and is what refused thirteen of the fourteen
// regions the carver could compile; the generated adjoint (adjoint.hpp)
// is what makes it a memory cost again.
constexpr int kValueRegWeight = 2;
// And what one graph op costs against one element. An op that writes a
// scalar still pays a dispatch, a context load and a scratch-partials
// backward; measured at ~5 ns against ~1 ns for an island instruction
// (docs/benchmarks.md). Leaving this out is why regions like `garch11`
// -- 1,797 scalar ops whose elements moved barely outnumber them -- read
// as a wash to an estimate that could only see elements, and measured
// 1.28x once they were compiled.
constexpr int kOpCost = 5;

// What a graph op costs on top of the elements it moves. The executor's
// per-op profile puts a scalar op at the dispatch alone, a scalar density
// at twice that, log_sum_exp at eight times, and softmax at seventeen. A
// dot product's own dispatch measures the same as a scalar op; the elements
// it reads are charged in graph_cost below.
int64_t graph_op_cost(uint16_t opcode) {
  switch (opcode) {
    case OP_SOFTMAX:
      return 17 * kOpCost;
    case OP_LOG_SUM_EXP:
      return 8 * kOpCost;
    case OP_DOT:
      return kOpCost;
    default:
      return program_density_id_by_opcode(opcode) >= 0 ? 2 * kOpCost : kOpCost;
  }
}

// Whether an island instruction's rule runs once per element rather than
// once total. Shared by Program::Instr and AdjInstr, which both carry a
// `code` and a `len`.
bool touches_width(Program::Code code) {
  switch (code) {
    case Program::RANGE:
    case Program::DOT:
    case Program::SOFTMAX:
    case Program::LOG_RANGE:
    case Program::EXP_RANGE:
    case Program::LSE_RANGE:
    case Program::MDIVIDE_LEFT:
    case Program::MDIVIDE_RIGHT_SPD:
      return true;
    default:
      return false;
  }
}

bool scalar_ins(const Graph& g, const Op& op) {
  for (int j = 0; j < op.n_in; ++j)
    if (g.slots[op.in[j]].len != 1) return false;
  return g.slots[op.out].len == 1;
}

bool elementwise_ins(const Graph& g, const Op& op) {
  const int64_t out_len = g.slots[op.out].len;
  if (out_len < 1) return false;
  for (int j = 0; j < op.n_in; ++j) {
    const int64_t len = g.slots[op.in[j]].len;
    if (len != 1 && len != out_len) return false;
  }
  return true;
}

// The scalar unaries the island machine speaks, paired with the
// instruction each compiles to. File-local on purpose: the MIR front
// end's unary chain (mir_prog.hpp) is a different set -- it has INV and
// FABS and lacks LOG1M and TANH -- so there is nothing to share.
#define STANLI_ISLAND_UNARY_LIST(X) \
  X(OP_NEG, NEG)                    \
  X(OP_EXPV, EXP)                   \
  X(OP_LOGV, LOG)                   \
  X(OP_SQRT, SQRT)                  \
  X(OP_SQUARE, SQUARE)              \
  X(OP_INV_LOGIT, INV_LOGIT)        \
  X(OP_LOG1M, LOG1M)                \
  X(OP_TANHV, TANH)                 \
  X(OP_INV, INV)                    \
  X(OP_ABS, FABS)                   \
  X(OP_LOG1P_EXP, LOG1P_EXP)

// Unary opcode -> island instruction, or -1.
int unary_code(uint16_t oc) {
  switch (oc) {
#define X(opc, code) \
  case opc:          \
    return Program::code;
    STANLI_ISLAND_UNARY_LIST(X)
#undef X
    default:
      return -1;
  }
}

// Everything else reaches the graph's own kernel through a CALL
// instruction, so one op the machine has no rule for stops ending a run.
// Scalar-out only for now: a vector-out op's value to an island is the
// same kernel the graph already ran, and admitting them means compiling
// entire vectorized models just for the estimate to refuse them
// (docs/superpowers/plans/2026-08-09-kernel-call-instruction.md, phase 2).
// The meta ops carry udata (message text, an ODE spec) or are the island
// itself; propto stays refused for the same reason as above.
bool callable(const Graph& g, const Op& op) {
  if (has_op_trait(op.opcode, op_trait::kVariantGrouped)) return false;
  switch (op.opcode) {
    case OP_ISLAND:
    case OP_ODE:
    case OP_DAE:
    case OP_ODE_ADJOINT:
    case OP_RNG:
    case OP_PRINT:
    case OP_REJECT:
      return false;
    default:
      break;
  }
  if (op.udata != nullptr || op.out2 >= 0) return false;
  if (op.variant & 0x80u) return false;
  if (g.slots[op.out].len != 1) return false;
  return op.opcode != OP_NONE_ && find_kernel(op.opcode) != nullptr;
}

// Structural vocabulary test. Shape/idata details are re-checked during
// compilation; anything unexpected there aborts the island (compile
// returns false) and the run is left alone. Strict is the vocabulary
// before elementwise ops had range forms: scalar arithmetic, and vector
// unaries only for log and exp.
bool in_vocab(const Graph& g, const Op& op, bool strict = false) {
  if (op.out2 >= 0 || op.dyn_lengths) return false;
  switch (op.opcode) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_FMA:
    case OP_LSE2:
    case OP_LOG_MIX:
    case OP_POW:
    case OP_FMAX:
    case OP_FMIN:
    case OP_LOG_DIFF_EXP:
      return strict ? scalar_ins(g, op) : elementwise_ins(g, op);
    case OP_ADD_N:
      return scalar_ins(g, op);
    case OP_INDEX:
    case OP_SET_INDEX:
    case OP_SET_INDEX_INPLACE:
    case OP_SLICE:
    case OP_SET_SLICE:
    case OP_SET_SLICE_INPLACE:
      return op.n_idata == 1;
    case OP_DOT:
      return op.n_in == 2 && g.slots[op.in[0]].len == g.slots[op.in[1]].len;
    case OP_LOG_SUM_EXP:
    case OP_SOFTMAX:
      return op.n_in == 1;
    default:
      if (unary_code(op.opcode) >= 0) {
        const int64_t len = g.slots[op.out].len;
        if (len < 1 || len != g.slots[op.in[0]].len) return false;
        return !strict || len == 1 || op.opcode == OP_LOGV ||
               op.opcode == OP_EXPV;
      }
      // Propto term-dropping depends on argument TYPES; the island binds
      // every argument as T, which only matches the <false> instantiation.
      if (program_density_id_by_opcode(op.opcode) >= 0)
        return (op.variant & 0x80u) == 0 && scalar_ins(g, op);
      return callable(g, op);
  }
}

void segment_noop_backward(KernelCtx&) {}

struct Compiler {
  const Graph& g;
  const std::unordered_map<int, const std::vector<double>*>& const_slots;
  // Last op (graph index) that reads each slot, over the WHOLE graph.
  const std::unordered_map<int, size_t>& last_use;
  // Slots read from outside the op graph (roots, target terms): they have
  // no op reader for last_use to see, so they can never be aliased over.
  const std::unordered_set<int>& pinned;
  IslandProg prog;
  std::unordered_map<int, int> reg_of;  // slot -> first register
  std::vector<int> live_in_slots;
  size_t op_index = 0;  // graph index of the op being compiled
  // Scratch registers CALLs allocated: working memory the graph op also
  // had, free in the estimate's eyes. They are subtracted from the two
  // value-file passes and retain only their identity cell in the compact
  // adjoint count below: effective weight 1.
  int64_t n_call_scratch = 0;
  // Scalar CONST registers holding a pool-absorbed constant, excluded from
  // the register-file charge the same way call scratch is. A CONSTR
  // register is not counted here: it can end up aliased into a SET_INDEX
  // or SET_SLICE output and stop being just a constant.
  int64_t n_const_regs = 0;
  bool ok = true;
  int max_live_ins = kMaxLiveIns;
  bool noop_backward = false;  // a kernel without a backward gets an empty one

  // A copy-then-modify op (SET_INDEX/SET_SLICE writing a slot distinct
  // from its base) can reuse the base's registers when nothing reads the
  // base after this op: the whole vector copy disappears, and so does a
  // fresh register range. Chains of these -- an unrolled loop filling one
  // vector element by element -- collapse onto one range, which is the
  // difference between 1.6M registers and a few thousand on a 1500-step
  // state-space model. Registers are island-private scratch, so this
  // never touches the arena the graph's slots live in.
  bool base_dead_here(int base) const {
    if (reg_of.find(base) == reg_of.end()) return false;  // not ours yet
    if (pinned.count(base)) return false;
    auto it = last_use.find(base);
    return it != last_use.end() && it->second <= op_index;
  }

  int alloc(int len) {
    const int r = prog.n_regs;
    prog.n_regs += len;
    return r;
  }

  // Register of a slot being READ. Unseen slots are constants (absorbed
  // into the pool) or live-ins (seeded from ctx.in).
  int read_reg(int slot) {
    auto it = reg_of.find(slot);
    if (it != reg_of.end()) return it->second;
    const int len = (int)g.slots[slot].len;
    auto cit = const_slots.find(slot);
    if (cit != const_slots.end()) {
      const int r = alloc(len);
      Program::Instr I;
      I.code = len == 1 ? Program::CONST : Program::CONSTR;
      I.dst = r;
      I.a = (int)prog.pool.size();
      I.len = len;
      prog.code.push_back(I);
      prog.pool.insert(prog.pool.end(), cit->second->begin(),
                       cit->second->end());
      reg_of.emplace(slot, r);
      // Only a scalar constant's register is excluded: a CONSTR register can
      // be aliased into a SET_INDEX/SET_SLICE output (base_dead_here below),
      // where it stops holding just the constant.
      if (len == 1) n_const_regs += 1;
      return r;
    }
    if ((int)live_in_slots.size() >= max_live_ins) {
      ok = false;
      return 0;
    }
    const int r = alloc(len);
    live_in_slots.push_back(slot);
    prog.ins.push_back(IslandProg::LiveIn{r, len});
    reg_of.emplace(slot, r);
    return r;
  }

  // Register of a slot being WRITTEN (fresh unless already mapped, which
  // is the in-place case: same slot, same registers).
  int write_reg(int slot) {
    auto it = reg_of.find(slot);
    if (it != reg_of.end()) return it->second;
    const int r = alloc((int)g.slots[slot].len);
    reg_of.emplace(slot, r);
    return r;
  }

  // The four-argument densities read their arguments as one contiguous
  // run (program.hpp), so scattered ones are copied into a fresh block --
  // skipped when they already sit in a row. Densities with three or fewer
  // arguments never come here: theirs ride in the instruction.
  int gather(const int* argv, int n) {
    bool contiguous = true;
    for (int k = 1; k < n; ++k)
      if (argv[k] != argv[0] + k) contiguous = false;
    if (contiguous) return argv[0];
    const int base = alloc(n);
    for (int k = 0; k < n; ++k) emit(Program::MOV, base + k, argv[k]);
    return base;
  }

  void emit(Program::Code c, int dst, int a, int b = 0, int cc = 0,
            int len = 0) {
    Program::Instr I;
    I.code = c;
    I.dst = dst;
    I.a = a;
    I.b = b;
    I.c = cc;
    I.len = len;
    prog.code.push_back(I);
  }

  // Everything callable() admits: the graph kernel itself, over register
  // ranges. Scratch is allocated inside the register file so the partials
  // the forward stashes survive to the backward.
  bool compile_call(const Op& op) {
    const Kernel* k = find_kernel(op.opcode);
    if (k == nullptr || op.n_in > 6) return false;
    Program::Call call;
    call.opcode = op.opcode;
    call.variant = op.variant;
    call.n_in = (int8_t)op.n_in;
    call.forward = k->forward;
    call.backward = k->backward;
    if (!call.backward && noop_backward) call.backward = segment_noop_backward;
    for (int j = 0; j < op.n_in; ++j) {
      call.in[j] = read_reg(op.in[j]);
      call.in_len[j] = (int)g.slots[op.in[j]].len;
    }
    call.out = write_reg(op.out);
    call.out_len = (int)g.slots[op.out].len;
    // An output aliasing an input would need the in-place value dance the
    // scalar rules do; no callable op is written that way, so refuse
    // rather than reason about it.
    for (int j = 0; j < op.n_in; ++j)
      if (call.out < call.in[j] + call.in_len[j] &&
          call.in[j] < call.out + call.out_len)
        return false;
    call.scratch_len =
        k->scratch_size ? (int)k->scratch_size(op, g.slots.data()) : 0;
    call.scratch = call.scratch_len ? alloc(call.scratch_len) : 0;
    call.idata.assign(op.idata, op.idata + op.n_idata);
    n_call_scratch += call.scratch_len;
    prog.calls.push_back(std::move(call));
    emit(Program::CALL, 0, (int)prog.calls.size() - 1);
    return ok;
  }

  // The scalar instruction, or one ranged instruction with its length-1
  // operands broadcast.
  bool compile_elementwise(const Op& op, Program::Code c, int law) {
    const int64_t out_len = g.slots[op.out].len;
    int r[3] = {0, 0, 0};
    uint8_t bcast = 0;
    for (int j = 0; j < op.n_in; ++j) {
      r[j] = read_reg(op.in[j]);
      if (g.slots[op.in[j]].len == 1) bcast |= (uint8_t)(1u << j);
    }
    const int d = write_reg(op.out);
    if (out_len == 1) {
      prog.code.push_back(Program::Instr(c, d, r[0], r[1], r[2], law));
      return ok;
    }
    if (c == Program::LOG || c == Program::EXP) {
      emit(c == Program::LOG ? Program::LOG_RANGE : Program::EXP_RANGE, d, r[0],
           0, 0, (int)out_len);
      return ok;
    }
    Program::Instr I(Program::RANGE, d, r[0], r[1], r[2], (int32_t)out_len);
    I.sub = static_cast<uint8_t>(c);
    I.bcast = bcast;
    I.law = static_cast<uint8_t>(law);
    prog.code.push_back(I);
    return ok;
  }

  bool compile(const Op& op) {
    const int64_t out_len = g.slots[op.out].len;
    switch (op.opcode) {
      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV: {
        static_assert(Program::SUB == Program::ADD + 1 &&
                          Program::MUL == Program::ADD + 2 &&
                          Program::DIV == Program::ADD + 3 &&
                          OP_SUB == OP_ADD + 1 && OP_MUL == OP_ADD + 2 &&
                          OP_DIV == OP_ADD + 3,
                      "binary code order");
        const auto c = (Program::Code)(Program::ADD + (op.opcode - OP_ADD));
        return compile_elementwise(op, c,
                                   c == Program::DIV ? kDivSafeGrouping : 0);
      }
      case OP_ADD_N: {
        const int a0 = read_reg(op.in[0]);
        const int d = write_reg(op.out);
        if (op.n_in == 1) {
          emit(Program::MOV, d, a0);
          return ok;
        }
        emit(Program::ADD, d, a0, read_reg(op.in[1]));
        for (int j = 2; j < op.n_in; ++j)
          emit(Program::ADD, d, d, read_reg(op.in[j]));
        return ok;
      }
#define X(opc, code) case opc:
        STANLI_ISLAND_UNARY_LIST(X)
#undef X
        return compile_elementwise(op, (Program::Code)unary_code(op.opcode), 0);
      case OP_INDEX: {
        const int idx = op.idata[0];
        if (idx < 0 || idx >= g.slots[op.in[0]].len) return false;
        const int a = read_reg(op.in[0]);
        emit(Program::MOV, write_reg(op.out), a + idx);
        return ok;
      }
      case OP_SLICE: {
        const int start = op.idata[0];
        if (start < 0 || start + out_len > g.slots[op.in[0]].len) return false;
        const int a = read_reg(op.in[0]);
        emit(Program::MOVR, write_reg(op.out), a + start, 0, 0, (int)out_len);
        return ok;
      }
      case OP_SET_INDEX:
      case OP_SET_INDEX_INPLACE: {
        const int idx = op.idata[0];
        if (op.n_in != 2 || idx < 0 || idx >= out_len) return false;
        const int base = read_reg(op.in[0]);
        const int val = read_reg(op.in[1]);
        // In-place, or a base nothing reads again: same registers, no copy.
        if (base_dead_here(op.in[0])) reg_of[op.out] = base;
        const int d = write_reg(op.out);
        if (d != base) emit(Program::MOVR, d, base, 0, 0, (int)out_len);
        emit(Program::MOV, d + idx, val);
        return ok;
      }
      case OP_SET_SLICE:
      case OP_SET_SLICE_INPLACE: {
        const int start = op.idata[0];
        const int64_t vlen = g.slots[op.in[1]].len;
        if (op.n_in != 2 || start < 0 || start + vlen > out_len) return false;
        if (op.opcode == OP_SET_SLICE_INPLACE &&
            (op.out != op.in[0] || op.in[0] == op.in[1]))
          return false;
        const int base = read_reg(op.in[0]);
        const int val = read_reg(op.in[1]);
        if (base_dead_here(op.in[0])) reg_of[op.out] = base;
        const int d = write_reg(op.out);
        if (d != base) emit(Program::MOVR, d, base, 0, 0, (int)out_len);
        emit(Program::MOVR, d + start, val, 0, 0, (int)vlen);
        return ok;
      }
      case OP_DOT: {
        const int a = read_reg(op.in[0]), b = read_reg(op.in[1]);
        emit(Program::DOT, write_reg(op.out), a, b, 0,
             (int)g.slots[op.in[0]].len);
        return ok;
      }
      case OP_LOG_SUM_EXP: {
        const int a = read_reg(op.in[0]);
        emit(Program::LSE_RANGE, write_reg(op.out), a, 0, 0,
             (int)g.slots[op.in[0]].len);
        return ok;
      }
      case OP_SOFTMAX: {
        if (out_len != g.slots[op.in[0]].len) return false;
        const int a = read_reg(op.in[0]);
        emit(Program::SOFTMAX, write_reg(op.out), a, 0, 0, (int)out_len);
        return ok;
      }
      case OP_LSE2:
        return compile_elementwise(op, Program::LSE2, 0);
      case OP_LOG_DIFF_EXP:
        return compile_elementwise(op, Program::LOG_DIFF_EXP, 0);
      case OP_POW:
        return compile_elementwise(op, Program::POW, op.variant);
      // The variant is the operands' activity, set at lowering; ties and
      // NaN adjoints depend on the instantiation (adjoint.cpp).
      case OP_FMAX:
        return compile_elementwise(op, Program::FMAX, op.variant);
      case OP_FMIN:
        return compile_elementwise(op, Program::FMIN, op.variant);
      case OP_LOG_MIX:
        return compile_elementwise(op, Program::LOG_MIX, 0);
      case OP_FMA:
        return compile_elementwise(op, Program::FMA, 0);
      default: {
        const int dc = program_density_id_by_opcode(op.opcode);
        if (dc < 0) return compile_call(op);
        if (op.n_in != program_density_arity(dc)) return false;
        int argv[kMaxDensityArgs];
        for (int k = 0; k < op.n_in; ++k) argv[k] = read_reg(op.in[k]);
        if (op.n_in > 3) {
          emit(Program::DENSITY, write_reg(op.out), gather(argv, op.n_in), 0, 0,
               dc);
        } else {
          emit(Program::DENSITY, write_reg(op.out), argv[0],
               op.n_in > 1 ? argv[1] : 0, op.n_in > 2 ? argv[2] : 0, dc);
        }
        return ok;
      }
    }
  }
};

// Retaining the raw program lets an accepted island receive destination
// forwarding after the legacy program has been priced, but copying every
// candidate would add avoidable preparation work. Every legal rewrite has
// this adjacent producer/copy syntax. The full pass remains the authority for
// opcode, range, liveness, overlap, and branch safety, so false positives here
// are harmless while this deliberately coarse probe cannot suppress a legal
// rewrite.
bool may_forward_adjacent_copy_destination(const Program& p) {
  for (size_t i = 0; i + 1 < p.code.size(); ++i) {
    const Program::Instr& producer = p.code[i];
    const Program::Instr& copy = p.code[i + 1];
    if ((copy.code == Program::MOV || copy.code == Program::MOVR) &&
        copy.a == producer.dst)
      return true;
  }
  return false;
}

enum class CarveDecision { kIsland, kSplit, kLeave };

// One run compiled, with the estimate's verdict on it. Holds what emit()
// needs by value -- never the Compiler that built it, which carries
// references into the Carver that compiled this candidate (its graph, its
// const_slots, its last_use).
struct Candidate {
  size_t begin;
  size_t end;
  IslandProg prog;
  std::vector<int> live_in_slots;
  std::vector<int> live_outs;
  std::unordered_set<int> in_set;
  std::unique_ptr<IslandProg> destination_source;
  bool priced_gen = false;
  bool compiled = false;
  bool accepted = false;
  int64_t graph_cost = 0;
  int64_t island_cost = 0;
  // What the island pays at its edges: live-ins copied in and their
  // adjoints harvested, live-outs copied out, seeded, and extracted by one
  // graph op each. Charged only where carvings differ, when a run is priced
  // whole against split.
  int64_t boundary = 0;
};

struct Carver {
  Graph& g;
  std::unordered_set<int> term_set;
  std::unordered_set<int> root_set;
  std::unordered_set<int> pinned;
  std::unordered_map<int, size_t> last_use;
  std::unordered_map<int, const std::vector<double>*> const_slots;
  std::vector<char> slot_active;
  std::vector<Op> result;
  int carved = 0;
  // Strict sub-runs split_cost compiled to price a split, discarded after
  // join_cost_floor sums their accepted island costs: emission never
  // replays a strict-sourced split (liveness_split_cost subsumes it, see
  // run()), so nothing needs these by position afterward.
  std::vector<Candidate> strict_queue;
  mutable std::vector<int> boundary_produced;
  mutable std::vector<int> boundary_livein;
  mutable std::vector<int> boundary_liveout;
  mutable int boundary_stamp = 0;
  // liveness_split_cost's own sub-pieces, and the interior cut points
  // between them (liveness_cuts's own output), so run() can re-derive the
  // same piece boundaries for emission without repeating the search.
  std::vector<Candidate> liveness_queue;
  size_t liveness_queue_pos = 0;
  std::vector<size_t> liveness_queue_cuts;

  Carver(Graph& graph,
         const std::vector<std::pair<int, std::vector<double>>>& fills,
         const std::vector<int>& target_terms,
         const std::vector<int>& extra_roots)
      : g(graph),
        term_set(target_terms.begin(), target_terms.end()),
        root_set(extra_roots.begin(), extra_roots.end()),
        pinned(term_set) {
    pinned.insert(root_set.begin(), root_set.end());

    // Last op index reading each slot, and whether any op writes it. A fill
    // slot no op writes is a load-time constant the program can absorb.
    std::unordered_set<int> written;
    for (size_t u = 0; u < g.ops.size(); ++u) {
      for (int j = 0; j < g.ops[u].n_in; ++j) last_use[g.ops[u].in[j]] = u;
      if (g.ops[u].out >= 0) written.insert(g.ops[u].out);
      if (g.ops[u].out2 >= 0) written.insert(g.ops[u].out2);
    }
    for (const auto& f : fills)
      if (!written.count(f.first)) const_slots[f.first] = &f.second;

    // Which slots carry a parameter. The adjoint generator turns this into
    // per-argument masks on the densities it differentiates; slots added
    // below are active until something says otherwise.
    slot_active.assign(g.slots.size(), 0);
    for (size_t s = 0; s < g.slots.size(); ++s)
      slot_active[s] = g.slots[s].is_param ? 1 : 0;
    for (const Op& op : g.ops) {
      bool any = false;
      for (int j = 0; j < op.n_in && !any; ++j)
        if (op.in[j] >= 0 && slot_active[(size_t)op.in[j]]) any = true;
      if (!any) continue;
      if (op.out >= 0) slot_active[(size_t)op.out] = 1;
      if (op.out2 >= 0) slot_active[(size_t)op.out2] = 1;
    }
  }

  // The run of compilable ops from i, stopping before `end`.
  size_t grow(size_t i, size_t end, bool strict) const {
    size_t j = i;
    while (j < end && in_vocab(g, g.ops[j], strict) &&
           term_set.count(g.ops[j].out) == 0)
      ++j;
    return j;
  }

  // The graph's side of the estimate: what its ops move (an in-place
  // element update moves one element, not a vector) plus what each op
  // costs to run.
  int64_t graph_cost(size_t i, size_t j) const {
    int64_t cost = 0;
    for (size_t u = i; u < j; ++u) {
      const Op& op = g.ops[u];
      if (op.opcode == OP_SET_INDEX_INPLACE)
        cost += 1;
      else if (op.opcode == OP_SET_SLICE_INPLACE)
        cost += g.slots[op.in[1]].len;
      else if (op.opcode == OP_DOT)
        cost += g.slots[op.in[0]].len;
      else
        cost += g.slots[op.out].len;
      cost += graph_op_cost(op.opcode);
    }
    return cost;
  }

  // Compile and price [i, j) without touching the graph. label tags the
  // debug line so a liveness-piece compile does not read as the whole
  // span's own join-vs-leave estimate to a caller counting those lines.
  Candidate evaluate(size_t i, size_t j, const char* label = "island") {
    Candidate c{i, j};
    Compiler cc{g, const_slots, last_use, pinned, {}, {}, {}, 0, 0, 0, true};
    bool compiled = true;
    for (size_t u = i; u < j && compiled; ++u) {
      cc.op_index = u;
      compiled = cc.compile(g.ops[u]) && cc.ok;
    }
    // Live-outs: written in the region and visible after it, in first-write
    // order for a deterministic packing. Settled before compaction, because
    // the program's live-outs are registers and compaction renumbers them.
    c.in_set.insert(cc.live_in_slots.begin(), cc.live_in_slots.end());
    if (compiled) {
      std::unordered_set<int> seen;
      for (size_t u = i; u < j; ++u) {
        const int o = g.ops[u].out;
        if (seen.count(o)) continue;
        seen.insert(o);
        auto lit = last_use.find(o);
        const bool read_after = lit != last_use.end() && lit->second >= j;
        if (read_after || root_set.count(o) || term_set.count(o))
          c.live_outs.push_back(o);
      }
      // A slot that is BOTH a live-in and a live-out (an in-place chain
      // whose template the region reads first and overwrites last) cannot
      // keep its id on the output side: the island reads the arena buffer
      // in the forward, and an extraction writing the same buffer would
      // feed the NEXT gradient's forward its own previous output. The
      // extraction gets a fresh slot and later references are renamed --
      // unless the slot is read from outside the graph, where the id is
      // the contract, and then the run stays as ops.
      bool aliased_pinned = false;
      for (int o : c.live_outs)
        if (c.in_set.count(o) && pinned.count(o)) aliased_pinned = true;
      if (c.live_outs.empty() || aliased_pinned) compiled = false;
    }
    if (compiled)
      for (int o : c.live_outs)
        for (int e = 0; e < (int)g.slots[o].len; ++e)
          cc.prog.out_regs.push_back(cc.reg_of.at(o) + e);

    // Price the pre-destination-forwarding program. This deliberately keeps
    // activation identical to the established cost model: copy coalescing may
    // make an island that was already selected faster, but it cannot use its
    // own savings to pull a new island across the line. A raw copy is retained
    // only when the optimization is enabled, and is compacted after the cost
    // decision below.
    if (compiled) {
      for (size_t k = 0; k < cc.prog.ins.size(); ++k)
        cc.prog.ins[k].active = slot_active[(size_t)cc.live_in_slots[k]] != 0;
      if (!std::getenv("STANLI_NO_PROGRAM_DEST_FORWARD") &&
          !std::getenv("STANLI_NO_ISLAND_COMPACT") &&
          may_forward_adjacent_copy_destination(cc.prog))
        c.destination_source = std::make_unique<IslandProg>(cc.prog);
      compact_island_gated(cc.prog, false);
      c.priced_gen = gen_adjoint(cc.prog);
      cc.prog.native_adj = c.priced_gen && !std::getenv("STANLI_NO_NATIVE_ADJ");
      // A failed generated adjoint would make this optimization replay each
      // kernel through callback vars. Leave the run as graph ops instead;
      // that is the same work without the gather/scatter adapter.
      if (!cc.prog.calls.empty() && !cc.prog.native_adj) compiled = false;
      // A refusal is not an error -- the replay still gives the right
      // gradient -- but it is worth being able to see, because it is the
      // difference between a region that is fast and one that merely
      // works, and nothing else about the model would show it.
      if (!c.priced_gen && std::getenv("STANLI_DEBUG_ISLAND"))
        emit_diagnostic("island: no adjoint generated for a " +
                        std::to_string(j - i) +
                        "-op region; it will replay under var");
    }
    c.compiled = compiled;
    c.graph_cost = graph_cost(i, j);
    c.prog = std::move(cc.prog);
    c.live_in_slots = std::move(cc.live_in_slots);
    if (!compiled) return c;
    // Is the island cheaper than the ops it replaces? The graph's side is
    // graph_cost above; the island's is its register file plus its two
    // instruction streams, forward and adjoint.
    //
    // `bones_model` is what the estimate is still for: 36 ops behind a
    // 4,024-register file, which is 3,979 live-outs packed into one op and
    // measured 0.25x. Everything else the carver reaches now wins, because
    // the register file stopped being built as vars. (`STANLI_ISLAND_ALWAYS=1`
    // bypasses this, for tests that exercise the compiler on small graphs
    // and for asking why a region was left alone.)
    //
    // A CALL should read as cost-NEUTRAL: it runs the graph's own
    // kernel with the graph's own per-call overhead (context assembly,
    // indirect call, twice per gradient), so absorbing one buys
    // continuity, never speed. Two corrections make that true in the
    // arithmetic: its scratch is subtracted from the two value-file passes
    // (working memory the graph op also had) but retains its identity cell
    // in the compact adjoint count, for effective weight 1; and each CALL
    // pays the same kOpCost the graph side is charged -- without which a
    // region of nothing but CALLs reads as a win and measures a loss
    // (dugongs_model, 0.63x, the first sweep after the vocabulary widened).
    const int64_t n_calls = (int64_t)c.prog.calls.size();
    // A rare program the generator refuses keeps the replay. Preserve its
    // old one-cell-per-value charge rather than treating an absent compact
    // adjoint program as a zero-sized file.
    const int64_t adj_regs = c.prog.adj.empty() ? (int64_t)c.prog.n_regs
                                                : (int64_t)c.prog.adj.n_regs;
    // An instruction whose rule runs once per element (RANGE and the other
    // ranged opcodes) counts its width; everything else counts 1.
    int64_t instrs = 0;
    for (const auto& I : c.prog.code)
      instrs += touches_width(I.code) ? I.len : 1;
    for (const auto& I : c.prog.adj.code)
      instrs += touches_width(I.code) ? I.len : 1;
    c.island_cost = kValueRegWeight * ((int64_t)c.prog.n_regs -
                                       cc.n_call_scratch - cc.n_const_regs) +
                    adj_regs + instrs + (kOpCost - 1) * 2 * n_calls;
    for (const auto& li : c.prog.ins) c.boundary += 2 * li.len;
    for (int o : c.live_outs) c.boundary += kOpCost + 3 * g.slots[o].len;
    if (std::getenv("STANLI_ISLAND_ALWAYS")) {
      c.accepted = true;
      return c;
    }
    if (std::getenv("STANLI_DEBUG_ISLAND"))
      emit_diagnostic(std::string(label) + "? ops=" + std::to_string(j - i) +
                      " graph=" + std::to_string(c.graph_cost) +
                      " island=" + std::to_string(c.island_cost) +
                      " boundary=" + std::to_string(c.boundary));
    c.accepted = c.graph_cost >= c.island_cost;
    return c;
  }

  int64_t joined_boundary(size_t i, size_t j) const {
    const size_t n = g.slots.size();
    if (boundary_produced.size() < n) boundary_produced.assign(n, 0);
    if (boundary_livein.size() < n) boundary_livein.assign(n, 0);
    if (boundary_liveout.size() < n) boundary_liveout.assign(n, 0);
    const int stamp = ++boundary_stamp;
    int64_t boundary = 0;
    for (size_t u = i; u < j; ++u) {
      const Op& op = g.ops[u];
      for (int k = 0; k < op.n_in; ++k) {
        const int s = op.in[k];
        if (boundary_produced[(size_t)s] == stamp || const_slots.count(s))
          continue;
        if (boundary_livein[(size_t)s] != stamp) {
          boundary_livein[(size_t)s] = stamp;
          boundary += 2 * g.slots[s].len;
        }
      }
      if (op.out >= 0) boundary_produced[(size_t)op.out] = stamp;
    }
    for (size_t u = i; u < j; ++u) {
      const int o = g.ops[u].out;
      if (o < 0 || boundary_liveout[(size_t)o] == stamp) continue;
      boundary_liveout[(size_t)o] = stamp;
      const auto lit = last_use.find(o);
      const bool read_after = lit != last_use.end() && lit->second >= j;
      if (read_after || root_set.count(o) || term_set.count(o))
        boundary += kOpCost + 3 * g.slots[o].len;
    }
    return boundary;
  }

  void vector_op_costs(size_t i, size_t j, int64_t* vector_graph,
                       int64_t* vector_range) const {
    *vector_graph = 0;
    *vector_range = 0;
    for (size_t u = i; u < j; ++u) {
      const Op& op = g.ops[u];
      if (in_vocab(g, op, true)) continue;
      *vector_graph += graph_cost(u, u + 1);
      *vector_range += (kValueRegWeight + 1 + 2) * g.slots[op.out].len;
    }
  }

  // The strict-vocabulary pieces of [i, j) at least kMinIslandOps long: the
  // candidate boundary set join_cost_floor, split_cost_floor, and
  // split_has_multiple_pieces all bound cost(split, i, j, B) against.
  std::vector<std::pair<size_t, size_t>> strict_pieces(size_t i,
                                                       size_t j) const {
    std::vector<std::pair<size_t, size_t>> pieces;
    size_t a = i;
    while (a < j) {
      const size_t b = grow(a, j, true);
      if ((int64_t)(b - a) >= kMinIslandOps) pieces.emplace_back(a, b);
      a = b > a ? b : a + 1;
    }
    return pieces;
  }

  int64_t pieces_boundary(
      const std::vector<std::pair<size_t, size_t>>& pieces) const {
    int64_t sum = 0;
    for (const auto& p : pieces) sum += joined_boundary(p.first, p.second);
    return sum;
  }

  // A lower bound on cost(join, i, j): the strict pieces' already-priced
  // cost, plus the vector ops swapped from graph to island form, plus the
  // whole span's own boundary.
  int64_t join_cost_floor(size_t i, size_t j) const {
    int64_t scalar = 0;
    for (const Candidate& sc : strict_queue)
      if (sc.accepted) scalar += sc.island_cost;
    int64_t vector_graph, vector_range;
    vector_op_costs(i, j, &vector_graph, &vector_range);
    return scalar - vector_graph + vector_range + joined_boundary(i, j);
  }

  // A bound on cost(split, i, j, B_strict): the joined candidate's own
  // priced cost, with the vector ops swapped from island to graph form and
  // the joined boundary traded for each strict piece's own.
  int64_t split_cost_floor(const Candidate& joined, size_t i, size_t j) const {
    int64_t vector_graph, vector_range;
    vector_op_costs(i, j, &vector_graph, &vector_range);
    return joined.island_cost - vector_range + vector_graph +
           pieces_boundary(strict_pieces(i, j));
  }

  // The same B_strict as strict_pieces, without paying for a piece past the
  // second: called only for the yes/no of |B_strict| >= 2.
  bool split_has_multiple_pieces(size_t i, size_t j) const {
    int pieces = 0;
    size_t a = i;
    while (a < j && pieces < 2) {
      const size_t b = grow(a, j, true);
      if ((int64_t)(b - a) >= kMinIslandOps) ++pieces;
      a = b > a ? b : a + 1;
    }
    return pieces >= 2;
  }

  // Prices each strict sub-run of [i, j) at least kMinIslandOps long and
  // keeps the compiled candidates in strict_queue, so join_cost_floor can
  // sum their accepted island costs without pricing the whole span's
  // split itself: liveness_split_cost subsumes that (see run()).
  void populate_strict_queue(size_t i, size_t j) {
    strict_queue.clear();
    size_t a = i;
    while (a < j) {
      const size_t b = grow(a, j, true);
      if ((int64_t)(b - a) < kMinIslandOps) {
        a = b > a ? b : a + 1;
        continue;
      }
      strict_queue.push_back(evaluate(a, b));
      a = b;
    }
  }

  // A proxy for what a cut at each position in [i, j) would cost: the
  // summed length of slots produced before that position and still read at
  // or after it, the same quantity joined_boundary charges per crossing
  // value. Index k of the result is position i + k. A slot written more
  // than once in [i, j) (an in-place chain reusing one slot id) is charged
  // once, at its first write, the same convention joined_boundary uses;
  // otherwise every rewrite would double- or triple-book the same value.
  std::vector<int64_t> live_pressure(size_t i, size_t j) const {
    std::vector<int64_t> diff(j - i + 2, 0);
    std::unordered_set<int> charged;
    for (size_t u = i; u < j; ++u) {
      const int o = g.ops[u].out;
      if (o < 0 || !charged.insert(o).second) continue;
      const auto it = last_use.find(o);
      const size_t lu = it != last_use.end() ? it->second : u;
      if (lu < u + 1) continue;
      const int64_t len = g.slots[(size_t)o].len;
      diff[u + 1 - i] += len;
      diff[std::min(lu + 1, j) - i] -= len;
    }
    std::vector<int64_t> pressure(j - i + 1, 0);
    int64_t running = 0;
    for (size_t p = i; p <= j; ++p) {
      running += diff[p - i];
      pressure[p - i] = running;
    }
    return pressure;
  }

  // The single interior point of [i, j) with the least crossing traffic
  // (live_pressure's proxy), or i when the span cannot hold two pieces of
  // at least kMinIslandOps.
  size_t best_cut_point(size_t i, size_t j) const {
    if (j - i < (size_t)(2 * kMinIslandOps)) return i;
    const std::vector<int64_t> pressure = live_pressure(i, j);
    size_t best = i + (size_t)kMinIslandOps;
    int64_t best_val = pressure[best - i];
    for (size_t p = best + 1; p <= j - (size_t)kMinIslandOps; ++p) {
      if (pressure[p - i] < best_val) {
        best_val = pressure[p - i];
        best = p;
      }
    }
    return best;
  }

  // Priced form of a liveness carving. Two totals are kept apart: decide
  // is the boundary-free accepted?island:graph convention c.accepted
  // itself uses, comparable between a whole span and a fragment of it
  // without a boundary-only piece looking artificially worse than
  // interpreting it; report is split_cost's own convention (boundary
  // added for an accepted piece), the figure this carving competes with
  // strict's on. pieces is the full partition in order (gaps included,
  // unpriced), and queue holds the compiled Candidate for each piece at
  // least kMinIslandOps long, aligned with pieces by position.
  struct LivenessPricing {
    int64_t decide = 0;
    int64_t report = 0;
    bool any = false;
    std::vector<std::pair<size_t, size_t>> pieces;
    std::vector<Candidate> queue;
  };

  // [i, j) split at cut and priced on each side; the caller keeps this
  // only if it beats every other candidate, including whole.
  LivenessPricing price_cut(size_t i, size_t cut, size_t j) {
    LivenessPricing left = price_liveness(i, cut);
    LivenessPricing right = price_liveness(cut, j);
    LivenessPricing out;
    out.decide = left.decide + right.decide;
    out.report = left.report + right.report;
    out.any = left.any || right.any;
    out.pieces = std::move(left.pieces);
    out.pieces.insert(out.pieces.end(), right.pieces.begin(),
                      right.pieces.end());
    out.queue = std::move(left.queue);
    for (auto& rc : right.queue) out.queue.push_back(std::move(rc));
    return out;
  }

  // [i, j) priced whole, against splitting it at the least-crossing-
  // traffic interior point (best_cut_point), and against splitting it
  // where the strict vocabulary itself would have cut (grow(i, j, true)):
  // never fragments where the whole span is cheaper on the boundary-free
  // decide figure, so a liveness carving is at worst the whole span, and
  // it can still recover a strict cut the pressure proxy missed.
  LivenessPricing price_liveness(size_t i, size_t j) {
    LivenessPricing whole;
    whole.pieces.emplace_back(i, j);
    if ((int64_t)(j - i) < kMinIslandOps) {
      whole.decide = whole.report = graph_cost(i, j);
      return whole;
    }
    Candidate c = evaluate(i, j, "island-liveness");
    whole.decide = c.accepted ? c.island_cost : c.graph_cost;
    whole.report = c.accepted ? c.island_cost + c.boundary : c.graph_cost;
    whole.any = c.accepted;
    whole.queue.push_back(std::move(c));

    std::optional<LivenessPricing> pressure_result;
    const size_t pressure_cut = best_cut_point(i, j);
    if (pressure_cut != i) pressure_result = price_cut(i, pressure_cut, j);

    // Unlike the pressure cut, a strict cut may leave a piece under
    // kMinIslandOps on either side; price_liveness already prices such a
    // piece as a plain graph-cost gap, the same as strict_pieces would. An
    // op the strict vocabulary refuses at i itself (grow returns i) is
    // skipped by one, the same advance strict_pieces makes, so recursion
    // gets a chance to find the strict-compatible run just past it.
    std::optional<LivenessPricing> strict_result;
    size_t strict_cut = grow(i, j, true);
    if (strict_cut == i) strict_cut = i + 1;
    if (strict_cut != j && strict_cut != pressure_cut)
      strict_result = price_cut(i, strict_cut, j);

    LivenessPricing* best = &whole;
    if (pressure_result && pressure_result->decide < best->decide)
      best = &*pressure_result;
    if (strict_result && strict_result->decide < best->decide)
      best = &*strict_result;
    return std::move(*best);
  }

  // [i, j) carved at liveness-chosen boundaries instead of the strict
  // vocabulary: every piece compiles at the run's own (non-strict)
  // vocabulary, since a liveness cut never lands on an op the run's
  // vocabulary already refused. Mirrors split_cost, queuing into
  // liveness_queue and remembering the cuts in liveness_queue_cuts so
  // run() can re-derive the same pieces without searching again. Returns
  // the report figure, comparable with split_cost's own return.
  int64_t liveness_split_cost(size_t i, size_t j, bool* any) {
    LivenessPricing p = price_liveness(i, j);
    liveness_queue = std::move(p.queue);
    liveness_queue_pos = 0;
    liveness_queue_cuts.clear();
    for (size_t k = 1; k < p.pieces.size(); ++k)
      liveness_queue_cuts.push_back(p.pieces[k].first);
    *any = p.any;
    return p.report;
  }

  // grow's counterpart for the liveness cut list liveness_split_cost just
  // priced: the next stored cut past a, or end if none remains.
  size_t grow_liveness(size_t a, size_t end) const {
    auto it = std::upper_bound(liveness_queue_cuts.begin(),
                               liveness_queue_cuts.end(), a);
    return it != liveness_queue_cuts.end() ? std::min(*it, end) : end;
  }

  // The span liveness_split_cost already compiled and priced for [i, j),
  // strict_candidate's counterpart for the liveness queue.
  Candidate liveness_candidate(size_t i, size_t j) {
    if (liveness_queue_pos < liveness_queue.size()) {
      Candidate& next = liveness_queue[liveness_queue_pos];
      if (next.begin == i && next.end == j) {
        Candidate c = std::move(next);
        ++liveness_queue_pos;
        return c;
      }
    }
    return evaluate(i, j, "island-liveness");
  }

  // A live-out that is also a live-in of the same candidate gets a fresh
  // slot on emission, and every later op referencing the old slot is
  // rewritten. That invalidates any queued candidate compiled against the
  // old ids.
  static bool renames_a_slot(const Candidate& c) {
    for (int o : c.live_outs)
      if (c.in_set.count(o)) return true;
    return false;
  }

  // A run holding ops the strict vocabulary refuses is priced whole and
  // split at them, boundaries included, and the split is carved when it is
  // cheaper or when only its pieces are accepted.
  bool split_wins(const Candidate& c, int64_t split, bool any) {
    if (!c.accepted) return any;
    const int64_t joined = c.island_cost + c.boundary;
    if (std::getenv("STANLI_DEBUG_ISLAND"))
      emit_diagnostic("island? ops=" + std::to_string(c.end - c.begin) +
                      " joined=" + std::to_string(joined) +
                      " split=" + std::to_string(split));
    return any && split < joined;
  }

  void emit(Candidate& c) {
    const size_t j = c.end;
    if (c.destination_source && c.priced_gen) {
      IslandProg optimized = std::move(*c.destination_source);
      if (compact_island_gated(optimized, true)) {
        const bool optimized_gen = gen_adjoint(optimized);
        optimized.native_adj =
            optimized_gen && !std::getenv("STANLI_NO_NATIVE_ADJ");
        // Do not trade the existing generated backward for replay if
        // forwarding exposes an unforeseen generator limitation. CALL cannot
        // replay at all.
        const bool usable =
            optimized_gen && (optimized.calls.empty() || optimized.native_adj);
        if (usable) {
          c.prog = std::move(optimized);
        } else if (std::getenv("STANLI_DEBUG_ISLAND")) {
          emit_diagnostic(
              "island: destination forwarding kept the priced program "
              "because its optimized adjoint was refused");
        }
      }
    }
    int64_t packed = 0;
    for (int o : c.live_outs) packed += g.slots[o].len;
    std::shared_ptr<const Program> optimized;
    if (!std::getenv("STANLI_NO_ISLAND_SOFTMAX3"))
      optimized = specialize_softmax3(c.prog);
    const bool specialized = static_cast<bool>(optimized);
    Op is;
    is.opcode = OP_ISLAND;
    is.variant = specialized            ? kIslandSoftmax3Variant
                 : c.prog.calls.empty() ? 0
                                        : kIslandCallVariant;
    is.n_in = (int)c.live_in_slots.size();
    for (int k = 0; k < is.n_in; ++k) is.in[k] = c.live_in_slots[k];
    is.out = g.add_slot(packed, false);
    slot_active.resize(g.slots.size(), 1);
    if (specialized) {
      auto specialized_prog = std::make_shared<Softmax3IslandProg>();
      static_cast<IslandProg&>(*specialized_prog) = std::move(c.prog);
      specialized_prog->optimized_double = std::move(optimized);
      // Erase the converted base pointer, not the derived pointer: readers
      // shared with OP_ISLAND recover IslandProg directly from udata.
      std::shared_ptr<IslandProg> prog = std::move(specialized_prog);
      is.udata = prog.get();
      g.udata_pool.push_back(std::move(prog));
    } else {
      auto prog = std::make_shared<IslandProg>(std::move(c.prog));
      is.udata = prog.get();
      g.udata_pool.push_back(std::move(prog));
    }
    result.push_back(is);
    // Extractions write the ORIGINAL slot ids, so downstream readers,
    // roots, and target terms are untouched -- except for the
    // live-in/live-out slots above, which get a fresh slot and a
    // rename of every later reference (read or write, as reroll's
    // write fusion does, so the two stay consistent).
    int64_t off = 0;
    for (int o : c.live_outs) {
      const int64_t len = g.slots[o].len;
      int dst = o;
      if (c.in_set.count(o)) {
        dst = g.add_slot(len, false);
        slot_active.resize(g.slots.size(), 1);
        slot_active[(size_t)dst] = slot_active[(size_t)o];
        for (size_t u = j; u < g.ops.size(); ++u) {
          for (int q = 0; q < g.ops[u].n_in; ++q)
            if (g.ops[u].in[q] == o) g.ops[u].in[q] = dst;
          if (g.ops[u].out == o) g.ops[u].out = dst;
          if (g.ops[u].out2 == o) g.ops[u].out2 = dst;
        }
      }
      Op ex;
      ex.opcode = len == 1 ? OP_INDEX : OP_SLICE;
      ex.n_in = 1;
      ex.in[0] = is.out;
      ex.out = dst;
      g.idata_pool.push_back({(int)off});
      ex.idata = g.idata_pool.back().data();
      ex.n_idata = 1;
      result.push_back(ex);
      off += len;
    }
    if (std::getenv("STANLI_DEBUG_ISLAND")) {
      const IslandProg& p = *static_cast<const IslandProg*>(is.udata);
      emit_diagnostic("island: ops=" + std::to_string(j - c.begin) +
                      " instr=" + std::to_string(p.code.size()) +
                      " regs=" + std::to_string(p.n_regs) +
                      " ins=" + std::to_string(p.ins.size()) +
                      " outs=" + std::to_string(p.out_regs.size()) +
                      " adj=" + std::to_string(p.adj.code.size()) +
                      " adj_regs=" + std::to_string(p.adj.n_regs));
      // Which instructions the region is made of, so a disagreement
      // with the replay can be attributed to an opcode rather than
      // guessed at.
      std::vector<int> hist(64, 0);
      for (const auto& I : p.code)
        if ((int)I.code < 64) ++hist[(size_t)I.code];
      std::string opcodes = "island opcodes:";
      for (int cd = 0; cd < 64; ++cd)
        if (hist[(size_t)cd])
          opcodes +=
              " " + std::to_string(cd) + ":" + std::to_string(hist[(size_t)cd]);
      emit_diagnostic(opcodes);
    }
    ++carved;
  }

  int run() {
    result.reserve(g.ops.size());
    // Liveness subsumes strict splitting (its own search always includes
    // the strict cut as one candidate, so it is never worse; a corpus
    // sweep over 254 models found it never even ties by losing, matching
    // strict's carving in every cell that changed nothing). This switch
    // disables splitting altogether, rather than falling back to a
    // strict-only split that no longer exists as a carving outcome.
    const bool liveness_off =
        std::getenv("STANLI_NO_ISLAND_LIVENESS") != nullptr;
    size_t i = 0;
    size_t liveness_until = 0;
    while (i < g.ops.size()) {
      const bool via_liveness = i < liveness_until;
      const size_t j = via_liveness ? grow_liveness(i, liveness_until)
                                    : grow(i, g.ops.size(), false);
      if ((int64_t)(j - i) < kMinIslandOps) {
        const size_t stop = j > i ? j : i + 1;
        while (i < stop) result.push_back(g.ops[i++]);
        continue;
      }
      if (!liveness_off && !via_liveness && grow(i, j, true) != j) {
        const bool always = std::getenv("STANLI_ISLAND_ALWAYS") != nullptr;
        const bool guard_off =
            std::getenv("STANLI_NO_ISLAND_JOIN_GUARD") != nullptr;
        const bool multi =
            !always && !guard_off && split_has_multiple_pieces(i, j);

        std::optional<Candidate> c;
        int64_t split = 0;
        bool any = false;
        CarveDecision natural = CarveDecision::kLeave;

        if (multi) {
          c.emplace(evaluate(i, j));
          if (c->accepted) {
            const int64_t floor = split_cost_floor(*c, i, j);
            const int64_t joined = c->island_cost + c->boundary;
            if (floor >= joined) {
              if (std::getenv("STANLI_DEBUG_ISLAND"))
                emit_diagnostic("island? ops=" + std::to_string(j - i) +
                                " split_floor=" + std::to_string(floor) +
                                " joined=" + std::to_string(joined) +
                                " split_skip=1");
              natural = CarveDecision::kIsland;
            }
          }
        }
        if (natural != CarveDecision::kIsland) {
          // join_cost_floor's accepted-cost sum comes from the strict
          // pieces; join_cost_floor is the only reader, only in the
          // non-multi branch.
          if (!multi) populate_strict_queue(i, j);
          split = liveness_split_cost(i, j, &any);
          if (!multi) {
            if (!always && !guard_off) {
              const int64_t floor = join_cost_floor(i, j);
              if (floor > split) {
                if (std::getenv("STANLI_DEBUG_ISLAND"))
                  emit_diagnostic("island? ops=" + std::to_string(j - i) +
                                  " join_floor=" + std::to_string(floor) +
                                  " split=" + std::to_string(split) +
                                  " skip=1");
                natural = CarveDecision::kSplit;
              }
            }
            if (natural != CarveDecision::kSplit) c.emplace(evaluate(i, j));
          }
          if (natural != CarveDecision::kIsland &&
              natural != CarveDecision::kSplit) {
            if (always ? !c->compiled : split_wins(*c, split, any))
              natural = CarveDecision::kSplit;
            else if (!c->accepted)
              natural = CarveDecision::kLeave;
            else
              natural = CarveDecision::kIsland;
          }
        }

        switch (natural) {
          case CarveDecision::kSplit:
            liveness_until = j;
            break;
          case CarveDecision::kLeave:
            while (i < j) result.push_back(g.ops[i++]);
            break;
          case CarveDecision::kIsland:
            emit(*c);
            i = j;
            break;
        }
        continue;
      }
      Candidate c = via_liveness ? liveness_candidate(i, j) : evaluate(i, j);
      if (c.accepted) {
        const bool renamed = via_liveness && renames_a_slot(c);
        emit(c);
        if (renamed) {
          liveness_queue.clear();
          liveness_queue_pos = 0;
        }
        i = j;
      } else {
        while (i < j) result.push_back(g.ops[i++]);
      }
    }
    g.ops = std::move(result);
    return carved;
  }
};

}  // namespace

void compact_island(IslandProg& p) { (void)compact_island_gated(p, true); }

bool compact_island_gated(IslandProg& p, bool enable_destination_forwarding) {
  if (std::getenv("STANLI_NO_ISLAND_COMPACT")) return false;
  std::vector<std::pair<int, int>> seeded;
  seeded.reserve(p.ins.size());
  for (const auto& li : p.ins) seeded.emplace_back(li.reg, li.len);
  const bool destination_forwarded =
      compact_program_gated(p, seeded, enable_destination_forwarding);
  for (size_t k = 0; k < p.ins.size(); ++k) p.ins[k].reg = seeded[k].first;
  return destination_forwarded;
}

int carve_islands(Graph& g,
                  const std::vector<std::pair<int, std::vector<double>>>& fills,
                  const std::vector<int>& target_terms,
                  const std::vector<int>& extra_roots) {
  if (std::getenv("STANLI_NO_ISLAND")) return 0;
  Carver carver(g, fills, target_terms, extra_roots);
  return carver.run();
}

bool segment_supports(const Graph& g, const Op& op) {
  return !is_effectful_op(op.opcode) && !op.dyn_lengths && in_vocab(g, op);
}

bool compile_segment(
    const Graph& g, const std::vector<SegmentItem>& items,
    const std::unordered_map<int, const std::vector<double>*>& constants,
    const std::vector<int>& live_outs, const std::vector<char>& slot_active,
    Segment* out) {
  static const std::unordered_map<int, size_t> no_last_use;
  static const std::unordered_set<int> no_pinned;
  Compiler cc{g,   constants, no_last_use, no_pinned,
              {},  {},        {},          0,
              0,   0,         true,        std::numeric_limits<int>::max(),
              true};
  std::vector<int> written;
  std::unordered_set<int> written_set;
  const auto write = [&](int slot) {
    if (written_set.insert(slot).second) written.push_back(slot);
  };
  for (const auto& item : items) {
    if (item.op < 0) {
      cc.reg_of[item.alias_dst] = cc.read_reg(item.alias_src);
      if (!cc.ok) return false;
      write(item.alias_dst);
      continue;
    }
    const Op& op = g.ops[(size_t)item.op];
    if (!segment_supports(g, op)) return false;
    // Every kernel output is a fresh value, whatever the slot held before.
    cc.reg_of.erase(op.out);
    if (!cc.compile(op) || !cc.ok) return false;
    write(op.out);
  }
  std::unordered_set<int> out_set(live_outs.begin(), live_outs.end());
  for (int slot : cc.live_in_slots)
    if (written_set.count(slot)) out_set.insert(slot);
  std::vector<int> outs;
  for (int slot : written)
    if (out_set.count(slot)) outs.push_back(slot);
  for (int slot : outs)
    for (int e = 0; e < (int)g.slots[slot].len; ++e)
      cc.prog.out_regs.push_back(cc.reg_of.at(slot) + e);
  for (size_t k = 0; k < cc.prog.ins.size(); ++k)
    cc.prog.ins[k].active = slot_active[(size_t)cc.live_in_slots[k]] != 0;
  compact_island_gated(cc.prog, false);
  if (!gen_adjoint(cc.prog)) return false;
  cc.prog.native_adj = true;

  Segment segment;
  const auto& adj_reg = cc.prog.adj.adj_reg;
  size_t off = 0;
  for (int slot : outs) {
    const int len = (int)g.slots[slot].len;
    const int reg = len ? cc.prog.out_regs[off] : 0;
    for (int e = 0; e < len; ++e)
      if (cc.prog.out_regs[off + e] != reg + e ||
          adj_reg[(size_t)(reg + e)] != adj_reg[(size_t)reg] + e)
        return false;
    segment.outs.push_back(SegmentBinding{slot, reg, len});
    off += (size_t)len;
  }
  for (size_t k = 0; k < cc.prog.ins.size(); ++k)
    segment.ins.push_back(SegmentBinding{
        cc.live_in_slots[k], cc.prog.ins[k].reg, cc.prog.ins[k].len});
  segment.program = std::move(cc.prog);
  *out = std::move(segment);
  return true;
}

}  // namespace stanli
