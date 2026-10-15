// Allocation-free fixed-size helpers for shared register-program islands.
//
// Keep these out of program.cpp and last in the runtime source inventory: the
// helper is selected by a private Program::CALL, and appending its object keeps
// unrelated interpreter and runtime code at the same layout as builds without
// the specialization.
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include "../kernels/add_n_fold.hpp"

#include <cassert>

namespace stanli {

void island_softmax3_fwd(KernelCtx& ctx);
extern void (*const canonical_add_n_forward)(KernelCtx&);

namespace {
// Keep fixed helpers in the existing late TU; the hot kernel inventory
// retains its original generic fold and all backwards are unchanged.
template <int Arity>
void add_n_fixed_fwd(KernelCtx& ctx) {
  assert(ctx.n_in == Arity);
  add_n_fold<Arity>(ctx);
}
void (*pick_add_n_arity(unsigned arity))(KernelCtx&) {
  switch (arity) {
    case 0:
      return &add_n_fixed_fwd<0>;
    case 1:
      return &add_n_fixed_fwd<1>;
    case 2:
      return &add_n_fixed_fwd<2>;
    case 3:
      return &add_n_fixed_fwd<3>;
    case 4:
      return &add_n_fixed_fwd<4>;
    case 5:
      return &add_n_fixed_fwd<5>;
    case 6:
      return &add_n_fixed_fwd<6>;
  }
  return canonical_add_n_forward;
}

void softmax3_into(const double* input, double* output) {
  using Vec = Eigen::Matrix<double, Eigen::Dynamic, 1>;
  const Eigen::Map<const Vec> x(input, 3);
  EIGEN_ALIGN_MAX double storage[3];
  Eigen::Map<Vec> result(storage, 3);
  const auto theta = (x.array() - x.maxCoeff()).exp();
  result = (theta / theta.sum()).matrix();
  for (int32_t i = 0; i < 3; ++i) output[i] = result(i);
}

// Cloning a program which has only a few matching instructions, or which is
// huge for unrelated reasons, would turn a local execution optimization into
// an unbounded setup-time and memory cost.  IOHMM is about 2 KiB of copied
// program data per matching site, so these limits leave useful headroom while
// ruling out both shapes.
constexpr size_t kMaxCloneBytes = 2 * 1024 * 1024;
constexpr size_t kMaxBytesPerSoftmax3 = 4096;

bool add_clone_bytes(size_t count, size_t width, size_t budget, size_t& total) {
  if (total > budget || count > (budget - total) / width) return false;
  total += count * width;
  return true;
}

bool within_clone_budget(const Program& p, size_t softmax3_count) {
  // min(kMaxCloneBytes, softmax3_count * kMaxBytesPerSoftmax3), without
  // overflowing the multiplication for adversarially large programs.
  size_t budget = kMaxCloneBytes;
  if (softmax3_count <= kMaxCloneBytes / kMaxBytesPerSoftmax3)
    budget = softmax3_count * kMaxBytesPerSoftmax3;

  size_t bytes = 0;
  if (!add_clone_bytes(p.code.size(), sizeof(Program::Instr), budget, bytes) ||
      !add_clone_bytes(p.calls.size(), sizeof(Program::Call), budget, bytes) ||
      !add_clone_bytes(p.pool.size(), sizeof(double), budget, bytes) ||
      !add_clone_bytes(p.out_regs.size(), sizeof(int), budget, bytes) ||
      !add_clone_bytes(softmax3_count, sizeof(Program::Call), budget, bytes))
    return false;
  for (const auto& call : p.calls)
    if (!add_clone_bytes(call.idata.size(), sizeof(int), budget, bytes))
      return false;
  return true;
}

}  // namespace

void program_softmax3_fwd(KernelCtx& ctx) {
  assert(ctx.variant == kProgramSoftmax3Variant && ctx.n_in == 1 &&
         ctx.in[0].len == 3 && ctx.out.len == 3);
  softmax3_into(ctx.in[0].data, ctx.out.data);
}

bool ensure_program_softmax3_kernel() {
  // Function-local initialization is the one-time, thread-safe publication
  // point. OP_NONE_ has no graph meaning and no ordinary registered kernel;
  // if that ever changes, leave the table alone and decline specialization.
  static const bool registered = [] {
    if (find_kernel(kProgramSoftmax3Opcode) != nullptr) return false;
    register_kernel(kProgramSoftmax3Opcode,
                    Kernel{program_softmax3_fwd, nullptr, nullptr});
    return true;
  }();
  return registered;
}

using ForwardFn = void (*)(KernelCtx&);

ForwardFn resolve_forward_fn(const Op& op) {
  constexpr int capacity = sizeof(KernelCtx{}.in) / sizeof(Desc);
  if (op.opcode == OP_ADD_N && op.n_in <= capacity && op.variant == 0 &&
      !op.dyn_lengths && kernel(op.opcode).forward == canonical_add_n_forward) {
    return pick_add_n_arity(op.n_in);
  }
  if (op.opcode == OP_ISLAND && op.variant == kIslandSoftmax3Variant)
    return island_softmax3_fwd;
  if (op.opcode == OP_ISLAND && op.variant == kIslandCallVariant)
    return island_calls_fwd;
  return kernel(op.opcode).forward;
}

std::shared_ptr<const Program> specialize_softmax3(const IslandProg& p,
                                                   size_t min_count) {
  if (!p.native_adj) return nullptr;
  size_t count = 0;
  for (const auto& I : p.code)
    if (I.code == Program::SOFTMAX && I.len == 3) ++count;
  if (count == 0 || count < min_count) return nullptr;
  if (!within_clone_budget(p, count)) return nullptr;
  if (!ensure_program_softmax3_kernel()) return nullptr;
  const Kernel* softmax3 = find_kernel(kProgramSoftmax3Opcode);
  if (softmax3 == nullptr || softmax3->forward == nullptr) return nullptr;

  // Copy the whole Program base so a future field cannot be silently omitted
  // from the optimized clone. The one-time reserve may move existing CALLs,
  // but keeps every appended payload stable thereafter.
  auto optimized = std::make_shared<Program>(static_cast<const Program&>(p));
  optimized->calls.reserve(optimized->calls.size() + count);
  for (auto& I : optimized->code) {
    if (I.code != Program::SOFTMAX || I.len != 3) continue;
    Program::Call call;
    call.opcode = kProgramSoftmax3Opcode;
    call.variant = kProgramSoftmax3Variant;
    call.n_in = 1;
    call.forward = softmax3->forward;
    call.backward = softmax3->backward;
    call.in[0] = I.a;
    call.in_len[0] = 3;
    call.out = I.dst;
    call.out_len = 3;
    optimized->calls.push_back(std::move(call));
    I.code = Program::CALL;
    I.a = static_cast<int32_t>(optimized->calls.size() - 1);
    I.dst = 0;
    I.len = 0;
  }
  return optimized;
}

void island_softmax3_fwd(KernelCtx& ctx) {
  const auto* base = static_cast<const IslandProg*>(ctx.udata);
  const auto& p = *static_cast<const Softmax3IslandProg*>(base);
  assert(p.native_adj && p.optimized_double);
  const Program& optimized = *p.optimized_double;
  assert(optimized.n_regs == p.n_regs);
  // Mirrors island_fwd's native-adjoint path; keep these seed and harvest
  // loops in lockstep with runtime/kernels/island.cpp.
  for (size_t k = 0; k < p.ins.size(); ++k) {
    const auto& li = p.ins[k];
    const int input = li.input >= 0 ? li.input : static_cast<int>(k);
    for (int i = 0; i < li.len; ++i)
      ctx.scratch[li.reg + i] = ctx.in[input].data[li.offset + i];
  }
  run_program_impl<true>(optimized, ctx.scratch, ctx.eval_state);
  for (size_t m = 0; m < p.out_regs.size(); ++m)
    ctx.out.data[m] = ctx.scratch[p.out_regs[m]];
}

}  // namespace stanli
