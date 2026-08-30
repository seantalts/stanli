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
// A run ends at: an opcode outside the vocabulary, a vector binary (already
// vectorized -- islands are for scalar residue), a propto density (its
// term-dropping depends on argument types; islands bind everything as T),
// an op producing a target term (terms stay graph-visible), or idata in a
// form the compiler does not model. Runs shorter than kMinIslandOps stay
// as they are: below that, per-op dispatch with scratch partials is cheaper
// than a var replay. A compiled run is then kept only if it is cheaper
// than the ops it replaces -- see the cost estimate at the end of
// carve_islands, which is what decides the pass is a win rather than a
// wash.
#include <stanli/island.hpp>

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/program_density.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
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

bool scalar_ins(const Graph& g, const Op& op) {
  for (int j = 0; j < op.n_in; ++j)
    if (g.slots[op.in[j]].len != 1) return false;
  return g.slots[op.out].len == 1;
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
  X(OP_TANHV, TANH)

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
  switch (op.opcode) {
    case OP_ISLAND:
    case OP_ODE:
    case OP_RNG:
    case OP_PROD_VEC:
    case OP_EXTREMA_VEC:
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
// returns false) and the run is left alone.
bool in_vocab(const Graph& g, const Op& op) {
  if (op.out2 >= 0) return false;
  switch (op.opcode) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_FMA:
    case OP_ADD_N:
    case OP_LSE2:
    case OP_LOG_MIX:
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
      if (unary_code(op.opcode) >= 0)
        return g.slots[op.out].len == g.slots[op.in[0]].len;
      // Propto term-dropping depends on argument TYPES; the island binds
      // every argument as T, which only matches the <false> instantiation.
      if (program_density_id_by_opcode(op.opcode) >= 0)
        return (op.variant & 0x80u) == 0 && scalar_ins(g, op);
      return callable(g, op);
  }
}

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
  bool ok = true;
  size_t max_live_ins = (size_t)kMaxLiveIns;
  // The legacy carver deliberately refuses already-vectorized work.  An
  // explicitly requested graph fragment has different policy: preserve a
  // shaped direct opcode by calling its graph kernel when the register
  // machine's scalar instruction cannot represent it.
  bool explicit_kernel_fallback = false;

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
      return r;
    }
    if (live_in_slots.size() >= max_live_ins) {
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
    call.udata = op.udata;
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
    const int64_t scratch =
        k->scratch_size ? k->scratch_size(op, g.slots.data()) : 0;
    if (scratch < 0 || scratch > std::numeric_limits<int>::max()) return false;
    call.scratch_len = (int)scratch;
    call.scratch = call.scratch_len ? alloc(call.scratch_len) : 0;
    call.idata.assign(op.idata, op.idata + op.n_idata);
    n_call_scratch += call.scratch_len;
    prog.calls.push_back(std::move(call));
    emit(Program::CALL, 0, (int)prog.calls.size() - 1);
    return ok;
  }

  bool compile(const Op& op) {
    const int64_t out_len = g.slots[op.out].len;
    switch (op.opcode) {
      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV: {
        if (explicit_kernel_fallback && !scalar_ins(g, op))
          return compile_call(op);
        static_assert(Program::SUB == Program::ADD + 1 &&
                          Program::MUL == Program::ADD + 2 &&
                          Program::DIV == Program::ADD + 3 &&
                          OP_SUB == OP_ADD + 1 && OP_MUL == OP_ADD + 2 &&
                          OP_DIV == OP_ADD + 3,
                      "binary code order");
        const int a = read_reg(op.in[0]), b = read_reg(op.in[1]);
        const auto c = (Program::Code)(Program::ADD + (op.opcode - OP_ADD));
        emit(c, write_reg(op.out), a, b);
        return ok;
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
        {
          const Program::Code c = (Program::Code)unary_code(op.opcode);
          if (explicit_kernel_fallback && out_len != 1 && c != Program::LOG &&
              c != Program::EXP)
            return compile_call(op);
          const int a = read_reg(op.in[0]);
          const int d = write_reg(op.out);
          if (out_len == 1) {
            emit(c, d, a);
          } else if (c == Program::LOG) {
            emit(Program::LOG_RANGE, d, a, 0, 0, (int)out_len);
          } else if (c == Program::EXP) {
            emit(Program::EXP_RANGE, d, a, 0, 0, (int)out_len);
          } else {
            return false;  // vector unary outside the range vocabulary
          }
          return ok;
        }
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
      case OP_LSE2: {
        if (explicit_kernel_fallback && out_len != 1) return compile_call(op);
        const int a = read_reg(op.in[0]), b = read_reg(op.in[1]);
        emit(Program::LSE2, write_reg(op.out), a, b);
        return ok;
      }
      case OP_LOG_MIX: {
        if (explicit_kernel_fallback && out_len != 1) return compile_call(op);
        const int a = read_reg(op.in[0]), b = read_reg(op.in[1]);
        const int c = read_reg(op.in[2]);
        emit(Program::LOG_MIX, write_reg(op.out), a, b, c);
        return ok;
      }
      case OP_FMA: {
        if (explicit_kernel_fallback && !scalar_ins(g, op))
          return compile_call(op);
        const int a = read_reg(op.in[0]), b = read_reg(op.in[1]);
        const int c = read_reg(op.in[2]);
        emit(Program::FMA, write_reg(op.out), a, b, c);
        return ok;
      }
      default: {
        const int dc = program_density_id_by_opcode(op.opcode);
        if (dc < 0) return compile_call(op);
        if (op.n_in != program_density_arity(dc)) return false;
        if (explicit_kernel_fallback && !scalar_ins(g, op))
          return compile_call(op);
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

bool compiler_direct_opcode(uint16_t opcode) {
  switch (opcode) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_FMA:
    case OP_ADD_N:
    case OP_INDEX:
    case OP_SET_INDEX:
    case OP_SET_INDEX_INPLACE:
    case OP_SLICE:
    case OP_SET_SLICE:
    case OP_SET_SLICE_INPLACE:
    case OP_DOT:
    case OP_LOG_SUM_EXP:
    case OP_SOFTMAX:
    case OP_LSE2:
    case OP_LOG_MIX:
      return true;
    default:
      return unary_code(opcode) >= 0 ||
             program_density_id_by_opcode(opcode) >= 0;
  }
}

bool direct_shape_allowed(const Graph& g, const Op& op) {
  if (!in_vocab(g, op)) return false;
  switch (op.opcode) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_LSE2:
    case OP_DOT:
      return op.n_in == 2;
    case OP_FMA:
    case OP_LOG_MIX:
      return op.n_in == 3;
    case OP_ADD_N:
      return op.n_in >= 1;
    case OP_INDEX:
    case OP_SLICE:
      return op.n_in == 1 && op.n_idata == 1 && op.idata != nullptr;
    case OP_SET_INDEX:
    case OP_SET_INDEX_INPLACE:
    case OP_SET_SLICE:
    case OP_SET_SLICE_INPLACE:
      return op.n_in == 2 && op.n_idata == 1 && op.idata != nullptr;
    case OP_LOG_SUM_EXP:
    case OP_SOFTMAX:
      return op.n_in == 1;
    default:
      if (unary_code(op.opcode) >= 0) return op.n_in == 1;
      const int density = program_density_id_by_opcode(op.opcode);
      return density >= 0 && op.n_in == program_density_arity(density);
  }
}

bool explicit_kernel_fallback_allowed(const Graph& g, const Op& op) {
  if (op.udata != nullptr || op.n_idata != 0 ||
      find_kernel(op.opcode) == nullptr)
    return false;
  const int64_t out_len = g.slots[(size_t)op.out].len;
  const auto broadcastable = [&]() {
    for (int j = 0; j < op.n_in; ++j) {
      const int64_t len = g.slots[(size_t)op.in[j]].len;
      if (len != 1 && len != out_len) return false;
    }
    return true;
  };
  const int density = program_density_id_by_opcode(op.opcode);
  if (density >= 0)
    return op.n_in == program_density_arity(density) && out_len == 1;
  switch (op.opcode) {
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DIV:
    case OP_LSE2:
      return op.n_in == 2 && out_len != 1 && broadcastable();
    case OP_FMA:
    case OP_LOG_MIX:
      return op.n_in == 3 && out_len != 1 && broadcastable();
    default:
      return unary_code(op.opcode) >= 0 && op.n_in == 1 && out_len != 1 &&
             g.slots[(size_t)op.in[0]].len == out_len;
  }
}

bool explicit_op_allowed(const Graph& g, const Op& op) {
  if (op.opcode == OP_NONE_ || op.opcode == OP_SCAN || op.opcode == OP_RNG ||
      is_effectful_op(op.opcode) || op.out2 >= 0 || op.n_in < 0 ||
      op.n_in > 6 || op.out < 0 || (size_t)op.out >= g.slots.size())
    return false;
  if (compiler_direct_opcode(op.opcode))
    return op.udata == nullptr && (direct_shape_allowed(g, op) ||
                                   explicit_kernel_fallback_allowed(g, op));
  return find_kernel(op.opcode) != nullptr;
}

bool retain_udata_owner(const Graph& g, const void* payload, uint16_t opcode,
                        std::unordered_set<const void*>* retained,
                        std::unordered_set<const void*>* expanded_islands,
                        std::vector<std::shared_ptr<void>>* owners) {
  if (payload == nullptr) return true;
  if (retained->insert(payload).second) {
    auto owner = std::find_if(g.udata_pool.begin(), g.udata_pool.end(),
                              [payload](const std::shared_ptr<void>& p) {
                                return p.get() == payload;
                              });
    if (owner == g.udata_pool.end()) return false;
    owners->push_back(*owner);
  }
  if (opcode != OP_ISLAND) return true;
  if (!expanded_islands->insert(payload).second) return true;
  const auto& nested = *static_cast<const IslandProg*>(payload);
  for (const Program::Call& call : nested.calls)
    if (!retain_udata_owner(g, call.udata, call.opcode, retained,
                            expanded_islands, owners))
      return false;
  return true;
}

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

bool compile_graph_fragment(
    const Graph& g, size_t op_begin, size_t op_end,
    const std::vector<int>& live_out_slots,
    const std::vector<std::pair<int, std::vector<double>>>& fills,
    const std::vector<uint8_t>& slot_active, GraphFragmentProgram* out,
    std::string* diagnostic) {
  auto refuse = [&](const std::string& why) {
    if (diagnostic != nullptr) *diagnostic = why;
    return false;
  };
  if (out == nullptr) return refuse("null graph-fragment result");
  if (op_begin >= op_end || op_end > g.ops.size())
    return refuse("invalid graph-fragment op range");
  if (slot_active.size() != g.slots.size())
    return refuse("graph-fragment activity does not cover every slot");
  if (live_out_slots.empty())
    return refuse("graph fragment has no explicit live-outs");

  std::unordered_map<int, size_t> last_use;
  std::unordered_set<int> written;
  for (size_t u = 0; u < g.ops.size(); ++u) {
    const Op& op = g.ops[u];
    if (op.n_in < 0 || op.n_in > 6)
      return refuse("graph fragment contains an invalid input count");
    for (int j = 0; j < op.n_in; ++j) {
      const int input = op.in[j];
      if (input < 0 || (size_t)input >= g.slots.size())
        return refuse("graph fragment contains an invalid input slot");
      last_use[input] = u;
    }
    if (op.out >= 0) {
      if ((size_t)op.out >= g.slots.size())
        return refuse("graph fragment contains an invalid output slot");
      written.insert(op.out);
    }
    if (op.out2 >= 0) {
      if ((size_t)op.out2 >= g.slots.size())
        return refuse("graph fragment contains an invalid second output");
      written.insert(op.out2);
    }
  }

  std::unordered_map<int, const std::vector<double>*> const_slots;
  for (const auto& fill : fills) {
    if (fill.first < 0 || (size_t)fill.first >= g.slots.size() ||
        (int64_t)fill.second.size() != g.slots[(size_t)fill.first].len)
      return refuse("graph-fragment fill does not match its slot");
    if (!written.count(fill.first)) const_slots[fill.first] = &fill.second;
  }

  std::unordered_set<int> produced;
  std::unordered_set<const void*> retained_payloads;
  std::unordered_set<const void*> expanded_islands;
  GraphFragmentProgram candidate;
  for (size_t u = op_begin; u < op_end; ++u) {
    const Op& op = g.ops[u];
    if (!explicit_op_allowed(g, op))
      return refuse("graph fragment contains an unsupported op");
    if (op.n_idata < 0 || (op.n_idata > 0 && op.idata == nullptr))
      return refuse("graph fragment contains malformed integer payload");
    if ((op.opcode == OP_ISLAND || op.opcode == OP_ODE) && op.udata == nullptr)
      return refuse("graph fragment contains a missing required payload");
    if (!retain_udata_owner(g, op.udata, op.opcode, &retained_payloads,
                            &expanded_islands, &candidate.udata_owners))
      return refuse("graph fragment payload has no graph owner");
    if (g.slots[(size_t)op.out].len < 0 ||
        g.slots[(size_t)op.out].len > std::numeric_limits<int>::max())
      return refuse("graph-fragment output is too large for a program");
    for (int j = 0; j < op.n_in; ++j) {
      const int64_t len = g.slots[(size_t)op.in[j]].len;
      if (len < 0 || len > std::numeric_limits<int>::max())
        return refuse("graph-fragment input is too large for a program");
    }
    produced.insert(op.out);
  }

  std::unordered_set<int> pinned;
  for (int slot : live_out_slots) {
    if (slot < 0 || (size_t)slot >= g.slots.size() || !produced.count(slot))
      return refuse("graph-fragment live-out is not produced by the range");
    if (!pinned.insert(slot).second)
      return refuse("graph-fragment live-outs contain a duplicate slot");
  }

  Compiler cc{g, const_slots, last_use, pinned, {}, {}, {}, 0, 0, true};
  cc.max_live_ins = std::numeric_limits<size_t>::max();
  cc.explicit_kernel_fallback = true;
  for (size_t u = op_begin; u < op_end; ++u) {
    cc.op_index = u;
    if (!cc.compile(g.ops[u]) || !cc.ok)
      return refuse("graph fragment could not compile an op");
  }
  for (int slot : live_out_slots) {
    auto reg = cc.reg_of.find(slot);
    if (reg == cc.reg_of.end())
      return refuse("graph-fragment live-out has no program register");
    const int64_t len = g.slots[(size_t)slot].len;
    for (int64_t k = 0; k < len; ++k)
      cc.prog.out_regs.push_back(reg->second + (int)k);
  }
  for (size_t k = 0; k < cc.prog.ins.size(); ++k)
    cc.prog.ins[k].active = slot_active[(size_t)cc.live_in_slots[k]] != 0;

  compact_island(cc.prog);
  const bool generated = gen_adjoint(cc.prog);
  cc.prog.native_adj = generated && !std::getenv("STANLI_NO_NATIVE_ADJ");
  if (!cc.prog.calls.empty() && !generated)
    return refuse("graph-fragment CALL has no generated adjoint");

  candidate.program = std::move(cc.prog);
  candidate.live_in_slots = std::move(cc.live_in_slots);
  *out = std::move(candidate);
  if (diagnostic != nullptr) diagnostic->clear();
  return true;
}

int carve_islands(Graph& g,
                  const std::vector<std::pair<int, std::vector<double>>>& fills,
                  const std::vector<int>& target_terms,
                  const std::vector<int>& extra_roots) {
  if (std::getenv("STANLI_NO_ISLAND")) return 0;

  std::unordered_set<int> term_set(target_terms.begin(), target_terms.end());
  std::unordered_set<int> root_set(extra_roots.begin(), extra_roots.end());
  std::unordered_set<int> pinned(term_set);
  pinned.insert(root_set.begin(), root_set.end());

  // Last op index reading each slot, and whether any op writes it. A fill
  // slot no op writes is a load-time constant the program can absorb.
  std::unordered_map<int, size_t> last_use;
  std::unordered_set<int> written;
  for (size_t u = 0; u < g.ops.size(); ++u) {
    for (int j = 0; j < g.ops[u].n_in; ++j) last_use[g.ops[u].in[j]] = u;
    if (g.ops[u].out >= 0) written.insert(g.ops[u].out);
    if (g.ops[u].out2 >= 0) written.insert(g.ops[u].out2);
  }
  std::unordered_map<int, const std::vector<double>*> const_slots;
  for (const auto& f : fills)
    if (!written.count(f.first)) const_slots[f.first] = &f.second;

  // Which slots carry a parameter. The adjoint generator turns this into
  // per-argument masks on the densities it differentiates; slots added
  // below are active until something says otherwise.
  std::vector<char> slot_active(g.slots.size(), 0);
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

  std::vector<Op> result;
  result.reserve(g.ops.size());
  int carved = 0;
  size_t i = 0;
  while (i < g.ops.size()) {
    // Grow the run of compilable ops.
    size_t j = i;
    while (j < g.ops.size() && in_vocab(g, g.ops[j]) &&
           term_set.count(g.ops[j].out) == 0)
      ++j;
    if ((int64_t)(j - i) < kMinIslandOps) {
      const size_t stop = j > i ? j : i + 1;
      while (i < stop) result.push_back(g.ops[i++]);
      continue;
    }

    // Compile. A failure mid-run keeps the graph untouched for the run.
    Compiler cc{g, const_slots, last_use, pinned, {}, {}, {}, 0, 0, true};
    bool compiled = true;
    for (size_t u = i; u < j && compiled; ++u) {
      cc.op_index = u;
      compiled = cc.compile(g.ops[u]) && cc.ok;
    }
    // Live-outs: written in the region and visible after it, in first-write
    // order for a deterministic packing. Settled before compaction, because
    // the program's live-outs are registers and compaction renumbers them.
    std::vector<int> live_outs;
    std::unordered_set<int> in_set(cc.live_in_slots.begin(),
                                   cc.live_in_slots.end());
    if (compiled) {
      std::unordered_set<int> seen;
      for (size_t u = i; u < j; ++u) {
        const int o = g.ops[u].out;
        if (seen.count(o)) continue;
        seen.insert(o);
        auto lit = last_use.find(o);
        const bool read_after = lit != last_use.end() && lit->second >= j;
        if (read_after || root_set.count(o) || term_set.count(o))
          live_outs.push_back(o);
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
      for (int o : live_outs)
        if (in_set.count(o) && pinned.count(o)) aliased_pinned = true;
      if (live_outs.empty() || aliased_pinned) compiled = false;
    }
    if (compiled)
      for (int o : live_outs)
        for (int e = 0; e < (int)g.slots[o].len; ++e)
          cc.prog.out_regs.push_back(cc.reg_of.at(o) + e);

    // Price the pre-destination-forwarding program. This deliberately keeps
    // activation identical to the established cost model: copy coalescing may
    // make an island that was already selected faster, but it cannot use its
    // own savings to pull a new island across the line. A raw copy is retained
    // only when the optimization is enabled, and is compacted after the cost
    // decision below.
    std::unique_ptr<IslandProg> destination_source;
    bool priced_gen = false;
    if (compiled) {
      for (size_t k = 0; k < cc.prog.ins.size(); ++k)
        cc.prog.ins[k].active = slot_active[(size_t)cc.live_in_slots[k]] != 0;
      if (!std::getenv("STANLI_NO_PROGRAM_DEST_FORWARD") &&
          !std::getenv("STANLI_NO_ISLAND_COMPACT") &&
          may_forward_adjacent_copy_destination(cc.prog))
        destination_source = std::make_unique<IslandProg>(cc.prog);
      compact_island_gated(cc.prog, false);
      priced_gen = gen_adjoint(cc.prog);
      cc.prog.native_adj = priced_gen && !std::getenv("STANLI_NO_NATIVE_ADJ");
      // The var replay cannot execute a CALL (kernels are double
      // machinery), so a CALL-bearing island exists only with its
      // generated adjoint; otherwise the run stays as ops, which is the
      // same work the CALLs would have done anyway.
      if (!cc.prog.calls.empty() && !cc.prog.native_adj) compiled = false;
      // A refusal is not an error -- the replay still gives the right
      // gradient -- but it is worth being able to see, because it is the
      // difference between a region that is fast and one that merely
      // works, and nothing else about the model would show it.
      if (!priced_gen && std::getenv("STANLI_DEBUG_ISLAND"))
        std::fprintf(stderr,
                     "island: no adjoint generated for a %zu-op region; "
                     "it will replay under var\n",
                     j - i);
    }
    // Is the island cheaper than the ops it replaces? The graph's side is
    // what its ops move (an in-place element update moves one element, not
    // a vector) plus what they pay per dispatch; the island's is its
    // register file plus its two instruction streams, forward and adjoint.
    //
    // `bones_model` is what the estimate is still for: 36 ops behind a
    // 4,024-register file, which is 3,979 live-outs packed into one op and
    // measured 0.25x. Everything else the carver reaches now wins, because
    // the register file stopped being built as vars. (`STANLI_ISLAND_ALWAYS=1`
    // bypasses this, for tests that exercise the compiler on small graphs
    // and for asking why a region was left alone.)
    if (compiled && !std::getenv("STANLI_ISLAND_ALWAYS")) {
      int64_t graph_elems = 0;
      for (size_t u = i; u < j; ++u) {
        const Op& op = g.ops[u];
        if (op.opcode == OP_SET_INDEX_INPLACE)
          graph_elems += 1;
        else if (op.opcode == OP_SET_SLICE_INPLACE)
          graph_elems += g.slots[op.in[1]].len;
        else
          graph_elems += g.slots[op.out].len;
      }
      const int64_t graph_cost = graph_elems + kOpCost * (int64_t)(j - i);
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
      const int64_t n_calls = (int64_t)cc.prog.calls.size();
      // A rare program the generator refuses keeps the replay. Preserve its
      // old one-cell-per-value charge rather than treating an absent compact
      // adjoint program as a zero-sized file.
      const int64_t adj_regs = cc.prog.adj.empty()
                                   ? (int64_t)cc.prog.n_regs
                                   : (int64_t)cc.prog.adj.n_regs;
      const int64_t island_cost =
          kValueRegWeight * ((int64_t)cc.prog.n_regs - cc.n_call_scratch) +
          adj_regs + (int64_t)cc.prog.code.size() +
          (int64_t)cc.prog.adj.code.size() + (kOpCost - 1) * 2 * n_calls;
      if (std::getenv("STANLI_DEBUG_ISLAND"))
        std::fprintf(stderr, "island? ops=%zu graph=%lld island=%lld\n", j - i,
                     (long long)graph_cost, (long long)island_cost);
      if (graph_cost < island_cost) compiled = false;
    }
    if (compiled && destination_source && priced_gen) {
      IslandProg optimized = std::move(*destination_source);
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
          cc.prog = std::move(optimized);
        } else if (std::getenv("STANLI_DEBUG_ISLAND")) {
          std::fprintf(stderr,
                       "island: destination forwarding kept the priced "
                       "program because its optimized adjoint was refused\n");
        }
      }
    }
    if (compiled) {
      int64_t packed = 0;
      for (int o : live_outs) packed += g.slots[o].len;
      std::shared_ptr<const Program> optimized;
      if (!std::getenv("STANLI_NO_ISLAND_SOFTMAX3"))
        optimized = specialize_softmax3(cc.prog);
      const bool specialized = static_cast<bool>(optimized);
      Op is;
      is.opcode = OP_ISLAND;
      is.variant = specialized             ? kIslandSoftmax3Variant
                   : cc.prog.calls.empty() ? 0
                                           : kIslandCallVariant;
      is.n_in = (int)cc.live_in_slots.size();
      for (int k = 0; k < is.n_in; ++k) is.in[k] = cc.live_in_slots[k];
      is.out = g.add_slot(packed, false);
      slot_active.resize(g.slots.size(), 1);
      if (specialized) {
        auto specialized_prog = std::make_shared<Softmax3IslandProg>();
        static_cast<IslandProg&>(*specialized_prog) = std::move(cc.prog);
        specialized_prog->optimized_double = std::move(optimized);
        // Erase the converted base pointer, not the derived pointer: readers
        // shared with OP_ISLAND recover IslandProg directly from udata.
        std::shared_ptr<IslandProg> prog = std::move(specialized_prog);
        is.udata = prog.get();
        g.udata_pool.push_back(std::move(prog));
      } else {
        auto prog = std::make_shared<IslandProg>(std::move(cc.prog));
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
      for (int o : live_outs) {
        const int64_t len = g.slots[o].len;
        int dst = o;
        if (in_set.count(o)) {
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
        std::fprintf(stderr,
                     "island: ops=%zu instr=%zu regs=%d ins=%zu outs=%zu "
                     "adj=%zu adj_regs=%d\n",
                     j - i, p.code.size(), p.n_regs, p.ins.size(),
                     p.out_regs.size(), p.adj.code.size(), p.adj.n_regs);
        // Which instructions the region is made of, so a disagreement
        // with the replay can be attributed to an opcode rather than
        // guessed at.
        std::vector<int> hist(64, 0);
        for (const auto& I : p.code)
          if ((int)I.code < 64) ++hist[(size_t)I.code];
        std::fprintf(stderr, "island opcodes:");
        for (int c = 0; c < 64; ++c)
          if (hist[(size_t)c])
            std::fprintf(stderr, " %d:%d", c, hist[(size_t)c]);
        std::fprintf(stderr, "\n");
      }
      ++carved;
      i = j;
      continue;
    }
    // Compile failed or nothing escapes: leave the run as ops.
    while (i < j) result.push_back(g.ops[i++]);
  }
  g.ops = std::move(result);
  return carved;
}

}  // namespace stanli
