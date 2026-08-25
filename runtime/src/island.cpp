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

#include <cstdio>
#include <cstdlib>
#include <memory>
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
    case OP_PRINT:
    case OP_REJECT:
      return false;
    default:
      break;
  }
  if (op.udata != nullptr || op.out2 >= 0) return false;
  if (op.variant & 0x80u) return false;
  if (g.slots[op.out].len != 1) return false;
  return find_kernel(op.opcode) != nullptr;
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
    if ((int)live_in_slots.size() >= kMaxLiveIns) {
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
    for (int j = 0; j < op.n_in; ++j) {
      call.in[j] = read_reg(op.in[j]);
      call.in_len[j] = (int)g.slots[op.in[j]].len;
      call.bwd_in[j] = call.in[j];
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
    call.bwd_out = call.out;
    call.scratch_len =
        k->scratch_size ? (int)k->scratch_size(op, g.slots.data()) : 0;
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
        const int a = read_reg(op.in[0]), b = read_reg(op.in[1]);
        emit(Program::LSE2, write_reg(op.out), a, b);
        return ok;
      }
      case OP_LOG_MIX: {
        const int a = read_reg(op.in[0]), b = read_reg(op.in[1]);
        const int c = read_reg(op.in[2]);
        emit(Program::LOG_MIX, write_reg(op.out), a, b, c);
        return ok;
      }
      case OP_FMA: {
        const int a = read_reg(op.in[0]), b = read_reg(op.in[1]);
        const int c = read_reg(op.in[2]);
        emit(Program::FMA, write_reg(op.out), a, b, c);
        return ok;
      }
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

}  // namespace

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
    // Generate the backward before estimating, because generating it is
    // what the estimate is about: it appends the checkpoint saves the
    // adjoint needs, so both the register count and the instruction count
    // below are the ones the island will actually run. STANLI_NO_NATIVE_ADJ
    // does not skip this -- it only stops the kernel USING the result, so
    // the two backwards can be compared over the same islands.
    if (compiled) {
      const bool gen = gen_adjoint(cc.prog);
      cc.prog.native_adj = gen && !std::getenv("STANLI_NO_NATIVE_ADJ");
      // The var replay cannot execute a CALL (kernels are double
      // machinery), so a CALL-bearing island exists only with its
      // generated adjoint; otherwise the run stays as ops, which is the
      // same work the CALLs would have done anyway.
      if (!cc.prog.calls.empty() && !cc.prog.native_adj) compiled = false;
      // A refusal is not an error -- the replay still gives the right
      // gradient -- but it is worth being able to see, because it is the
      // difference between a region that is fast and one that merely
      // works, and nothing else about the model would show it.
      if (!gen && std::getenv("STANLI_DEBUG_ISLAND"))
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
    if (compiled) {
      // Live-outs: written in the region and visible after it, in
      // first-write order for a deterministic packing.
      std::vector<int> live_outs;
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
      std::unordered_set<int> in_set(cc.live_in_slots.begin(),
                                     cc.live_in_slots.end());
      bool aliased_pinned = false;
      for (int o : live_outs)
        if (in_set.count(o) && pinned.count(o)) aliased_pinned = true;
      if (!live_outs.empty() && !aliased_pinned) {
        int64_t packed = 0;
        for (int o : live_outs) {
          for (int e = 0; e < (int)g.slots[o].len; ++e)
            cc.prog.out_regs.push_back(cc.reg_of.at(o) + e);
          packed += g.slots[o].len;
        }
        auto prog = std::make_shared<IslandProg>(std::move(cc.prog));
        Op is;
        is.opcode = OP_ISLAND;
        is.n_in = (int)cc.live_in_slots.size();
        for (int k = 0; k < is.n_in; ++k) is.in[k] = cc.live_in_slots[k];
        is.out = g.add_slot(packed, false);
        is.udata = prog.get();
        g.udata_pool.push_back(std::move(prog));
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
    }
    // Compile failed or nothing escapes: leave the run as ops.
    while (i < j) result.push_back(g.ops[i++]);
  }
  g.ops = std::move(result);
  return carved;
}

}  // namespace stanli
