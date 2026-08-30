// The generated adjoint program against the var replay it replaces.
//
// The replay is the oracle: it is the same var arithmetic CmdStan's
// generated code runs, so the bar is BITWISE, not close. Every case here
// builds a small register program, differentiates it both ways at the same
// point with the same seed adjoints, and requires the live-ins' adjoints to
// agree to the last bit.
//
// One exception, and it is a documented property rather than slack: the
// fuzzer allows two ulp, because a copied register whose destination is
// later written again groups the same sum differently under the two. The
// long comment on `check` says why, and why closing it would cost the
// contiguous register ranges the reductions depend on.
#include <stanli/adjoint.hpp>
#include <stanli/program_density.hpp>
#include <stanli/recorder.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>

#include "env_helpers.hpp"

#include <stan/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <limits>
#include <cstdio>
#include <string>
#include <vector>

using namespace stanli;

static int failures = 0;

// A private CALL target proves execution uses the payload's resolved
// functions rather than consulting the graph opcode table. Its additive
// immediates make context reuse observable without affecting the derivative.
static void test_call_forward(KernelCtx& ctx) {
  ctx.out.data[0] = ctx.in[0].data[0] * ctx.in[1].data[0] + ctx.variant +
                    (ctx.n_idata ? ctx.idata[0] : 0);
}

static void test_call_backward(KernelCtx& ctx) {
  ctx.in_adj[0].data[0] += ctx.out_adj * ctx.in[1].data[0];
  ctx.in_adj[1].data[0] += ctx.out_adj * ctx.in[0].data[0];
}

static void expect(const char* what, bool ok);

static const IslandProg* nested_backward_program = nullptr;

static void nested_leaf_forward(KernelCtx& ctx) {
  ctx.out.data[0] = ctx.in[0].data[0] + 1.0;
}

static void nested_leaf_backward(KernelCtx& ctx) {
  ctx.in_adj[0].data[0] += ctx.out_adj;
}

static void nested_outer_forward(KernelCtx& ctx) {
  ctx.out.data[0] = 2.0 * ctx.in[0].data[0];
}

// Run a second generated CALL-bearing adjoint while the outer CALL's
// backward packet is live. This catches TLS/shared-context implementations:
// the nested invocation must not replace the outer in_adj destination.
static void nested_outer_backward(KernelCtx& ctx) {
  const IslandProg& nested = *nested_backward_program;
  std::vector<double> values(static_cast<size_t>(nested.n_regs), 0.0);
  values[0] = 3.0;
  run_program(nested, values.data());
  std::vector<double> adj(static_cast<size_t>(nested.adj.n_regs), 0.0);
  adj[static_cast<size_t>(nested.adj.adj_reg[nested.out_regs[0]])] = 1.0;
  run_adjoint(nested, nested.adj, values.data(), adj.data());
  ctx.in_adj[0].data[0] += 2.0 * ctx.out_adj;
}

static void test_nested_backward_context_reentrant() {
  IslandProg nested;
  nested.n_regs = 2;
  nested.ins = {{0, 1}};
  Program::Call leaf;
  leaf.opcode = OP_COUNT_;
  leaf.forward = nested_leaf_forward;
  leaf.backward = nested_leaf_backward;
  leaf.n_in = 1;
  leaf.in[0] = 0;
  leaf.in_len[0] = 1;
  leaf.out = 1;
  leaf.out_len = 1;
  nested.calls.push_back(leaf);
  nested.code.push_back({Program::CALL, 0, 0});
  nested.out_regs = {1};
  expect("nested CALL adjoint generated", gen_adjoint(nested));

  IslandProg outer;
  outer.n_regs = 2;
  outer.ins = {{0, 1}};
  Program::Call parent;
  parent.opcode = OP_COUNT_;
  parent.forward = nested_outer_forward;
  parent.backward = nested_outer_backward;
  parent.n_in = 1;
  parent.in[0] = 0;
  parent.in_len[0] = 1;
  parent.out = 1;
  parent.out_len = 1;
  outer.calls.push_back(parent);
  outer.code.push_back({Program::CALL, 0, 0});
  outer.out_regs = {1};
  nested_backward_program = &nested;
  expect("outer nested CALL adjoint generated", gen_adjoint(outer));
  std::vector<double> values(static_cast<size_t>(outer.n_regs), 0.0);
  values[0] = 4.0;
  run_program(outer, values.data());
  std::vector<double> adj(static_cast<size_t>(outer.adj.n_regs), 0.0);
  adj[static_cast<size_t>(outer.adj.adj_reg[outer.out_regs[0]])] = 1.0;
  run_adjoint(outer, outer.adj, values.data(), adj.data());
  expect("nested backward preserves outer input adjoint",
         adj[static_cast<size_t>(outer.adj.adj_reg[outer.ins[0].reg])] == 2.0);
  nested_backward_program = nullptr;
}

static void expect(const char* what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

// A program under test: instructions over a register file, some registers
// designated live-ins (seeded from `in`) and some live-outs (read back).
struct Case {
  IslandProg p;
  std::vector<double> in;    // one value per live-in register, packed
  std::vector<double> seed;  // one adjoint per out_reg
};

// The oracle: run_island<var> under nested autodiff, exactly as
// island_bwd does today.
static std::vector<double> replay_adjoints(const IslandProg& p,
                                           const std::vector<double>& in,
                                           const std::vector<double>& seed,
                                           std::vector<double>* out_vals) {
  stan::math::nested_rev_autodiff nested;
  using stan::math::var;
  std::vector<var> reg((size_t)p.n_regs);
  std::vector<var> vin(in.size());
  int64_t off = 0;
  for (const auto& li : p.ins)
    for (int i = 0; i < li.len; ++i) {
      vin[(size_t)off] = in[(size_t)off];
      reg[(size_t)(li.reg + i)] = vin[(size_t)off];
      ++off;
    }
  run_program(p, reg);
  var j = 0.0;
  for (size_t m = 0; m < p.out_regs.size(); ++m) {
    out_vals->push_back(reg[(size_t)p.out_regs[m]].val());
    j += reg[(size_t)p.out_regs[m]] * seed[m];
  }
  stan::math::grad(j.vi_);
  std::vector<double> adj(in.size());
  for (size_t k = 0; k < in.size(); ++k) adj[k] = vin[k].adj();
  return adj;
}

// The forward contract for CFG native execution is the canonical Program on
// doubles. Some structured Stan Math operations (notably mdivide_left) have
// observably different primitive and reverse-mode value algorithms, so this
// is intentionally distinct from the var-replay value above.
static std::vector<double> direct_values(const IslandProg& p,
                                         const std::vector<double>& in) {
  std::vector<double> reg((size_t)p.n_regs, 0.0);
  int64_t off = 0;
  for (const auto& li : p.ins)
    for (int i = 0; i < li.len; ++i)
      reg[(size_t)(li.reg + i)] = in[(size_t)off++];
  run_program(p, reg);
  std::vector<double> out;
  out.reserve(p.out_regs.size());
  for (int r : p.out_regs) out.push_back(reg[(size_t)r]);
  return out;
}

// The subject: forward on doubles, then the generated adjoint program.
static std::vector<double> native_adjoints(const IslandProg& p,
                                           const std::vector<double>& in,
                                           const std::vector<double>& seed,
                                           std::vector<double>* out_vals,
                                           const Program* optimized = nullptr) {
  const size_t trace_words =
      p.adj.trace_bits > 0 ? (size_t)(p.adj.trace_bits + 63) / 64 : 0;
  std::vector<double> val((size_t)p.n_regs + trace_words, 0.0);
  int64_t off = 0;
  for (const auto& li : p.ins)
    for (int i = 0; i < li.len; ++i)
      val[(size_t)(li.reg + i)] = in[(size_t)off++];
  uint8_t* const executed =
      trace_words ? reinterpret_cast<uint8_t*>(val.data() + p.n_regs) : nullptr;
  if (trace_words) {
    std::memset(executed, 0, trace_words * sizeof(uint64_t));
    run_program(p, p.code, val.data(), executed, p.trace_pc.data());
  } else {
    run_program(optimized ? *optimized : static_cast<const Program&>(p),
                val.data());
  }
  for (size_t m = 0; m < p.out_regs.size(); ++m)
    out_vals->push_back(val[(size_t)p.out_regs[m]]);

  std::vector<double> adj((size_t)p.adj.n_regs, 0.0);
  const auto& map = p.adj.adj_reg;
  for (size_t m = p.out_regs.size(); m-- > 0;)
    adj[(size_t)map[(size_t)p.out_regs[m]]] += seed[m];
  run_adjoint(p, p.adj, val.data(), adj.data(), executed);

  std::vector<double> got(in.size());
  off = 0;
  for (const auto& li : p.ins)
    for (int i = 0; i < li.len; ++i)
      got[(size_t)off++] = adj[(size_t)map[(size_t)(li.reg + i)]];
  return got;
}

// The program, for when a fuzz case fails: a random program is only useful
// if you can read it back.
static void dump(const IslandProg& p) {
  std::printf("  n_regs=%d ins:", p.n_regs);
  for (const auto& li : p.ins) std::printf(" r%d(len%d)", li.reg, li.len);
  std::printf(" outs:");
  for (int r : p.out_regs) std::printf(" r%d", r);
  std::printf("\n");
  for (size_t i = 0; i < p.code.size(); ++i) {
    const auto& I = p.code[i];
    std::printf("  %3zu %-7s r%d <- r%d, r%d (len %d)\n", i,
                program_code_spec(I.code).name, I.dst, I.a, I.b, I.len);
  }
}

// How many representable doubles apart two results are. Two NaNs agree:
// stan-math poisons an adjoint with NaN deliberately (fabs, fmax) and `==`
// would call that a disagreement forever.
static int64_t ulps(double a, double b) {
  if (a == b || (std::isnan(a) && std::isnan(b))) return 0;
  int64_t ia, ib;
  std::memcpy(&ia, &a, sizeof ia);
  std::memcpy(&ib, &b, sizeof ib);
  if ((ia < 0) != (ib < 0)) return INT64_MAX;
  const int64_t d = ia - ib;
  return d < 0 ? -d : d;
}

static void test_call_binding_refusal() {
  Program::Call registered;
  registered.opcode = OP_POW;
  expect("CALL registered bind", bind_call(registered));
  const Kernel* pow = find_kernel(OP_POW);
  expect("CALL caches registered forward",
         pow && registered.forward == pow->forward);
  expect("CALL caches registered backward",
         pow && registered.backward == pow->backward);

  Program::Call unknown;
  unknown.opcode = OP_COUNT_;
  expect("CALL unknown bind refuses", !bind_call(unknown));
  expect("CALL unknown bind stays empty",
         unknown.forward == nullptr && unknown.backward == nullptr);

  Program::Call forward_only;
  forward_only.opcode = OP_RNG;
  expect("CALL forward-only bind", bind_call(forward_only));
  expect("CALL forward-only has no reverse",
         forward_only.forward != nullptr && forward_only.backward == nullptr);
  IslandProg no_reverse;
  no_reverse.n_regs = 1;
  no_reverse.out_regs = {0};
  no_reverse.calls.push_back(forward_only);
  no_reverse.code.push_back(Program::Instr{Program::CALL, 0, 0, 0, 0, 0});
  expect("CALL missing backward adjoint refuses", !gen_adjoint(no_reverse));
  expect("CALL missing backward refusal is transactional",
         no_reverse.n_regs == 1 && no_reverse.code.size() == 1 &&
             no_reverse.adj.empty());

  IslandProg p;
  p.n_regs = 3;
  p.ins = {IslandProg::LiveIn{0, 1}, IslandProg::LiveIn{1, 1}};
  p.out_regs = {2};
  p.calls.push_back(unknown);
  p.code.push_back(Program::Instr{Program::CALL, 0, 0, 0, 0, 0});
  const IslandProg before = p;
  expect("CALL unbound adjoint refuses", !gen_adjoint(p));
  expect("CALL refusal keeps register count", p.n_regs == before.n_regs);
  expect("CALL refusal keeps code",
         p.code.size() == before.code.size() &&
             std::memcmp(p.code.data(), before.code.data(),
                         p.code.size() * sizeof(Program::Instr)) == 0);
  expect("CALL refusal keeps payload",
         p.calls.size() == 1 && p.calls[0].opcode == before.calls[0].opcode &&
             p.calls[0].forward == nullptr && p.calls[0].backward == nullptr);
  expect("CALL refusal keeps adjoint empty", p.adj.empty());

  std::vector<double> reg(3, 0.0);
  bool threw = false;
  try {
    run_program(p, reg.data());
  } catch (const std::logic_error&) {
    threw = true;
  }
  expect("CALL unbound forward throws", threw);
}

static void test_call_cached_forward_reverse_aliasing() {
  // Reuse one payload twice around overwrites of both its input and output.
  // A payload-level reverse cache would make both adjoint instructions read
  // the second site's checkpoints; gen_adjoint therefore normalizes it to
  // one bound Program::Call per instruction.
  IslandProg p;
  p.n_regs = 5;
  p.ins = {IslandProg::LiveIn{0, 1}, IslandProg::LiveIn{1, 1},
           IslandProg::LiveIn{4, 1}};
  p.out_regs = {3, 2};
  Program::Call call;
  call.opcode = OP_COUNT_;  // deliberately absent from the graph table
  call.variant = 3;
  call.n_in = 2;
  call.forward = test_call_forward;
  call.backward = test_call_backward;
  call.in[0] = 0;
  call.in[1] = 1;
  call.in_len[0] = 1;
  call.in_len[1] = 1;
  call.out = 2;
  call.out_len = 1;
  call.idata = {5};
  p.calls.push_back(call);
  p.code.push_back(Program::Instr{Program::CALL, 0, 0, 0, 0, 0});
  p.code.push_back(Program::Instr{Program::MOV, 3, 2, 0, 0, 0});
  p.code.push_back(Program::Instr{Program::MOV, 0, 4, 0, 0, 0});
  p.code.push_back(Program::Instr{Program::CALL, 0, 0, 0, 0, 0});

  expect("CALL shared payload adjoint generated", gen_adjoint(p));
  expect("CALL reverse payload per instruction", p.calls.size() == 2);
  if (p.calls.size() == 2) {
    expect("CALL input checkpoints stay per instruction",
           p.calls[0].bwd_value_in[0] != p.calls[1].bwd_value_in[0]);
    expect("CALL output checkpoints stay per instruction",
           p.calls[0].bwd_value_out != p.calls[1].bwd_value_out);
    expect("CALL reverse dispatch pre-resolved",
           p.calls[0].backward == test_call_backward &&
               p.calls[1].backward == test_call_backward);
  }

  double a = 1.125, b = 1.75, c = 0.625;
  std::vector<double> val((size_t)p.n_regs, 0.0);
  val[0] = a;
  val[1] = b;
  val[4] = c;
  run_program(p, val.data());
  const double first = a * b + 8.0;
  const double second = c * b + 8.0;
  expect("CALL cached first forward bitwise", ulps(val[3], first) == 0);
  expect("CALL cached second forward bitwise", ulps(val[2], second) == 0);

  std::vector<double> adj((size_t)p.adj.n_regs, 0.0);
  const double first_seed = 0.7, second_seed = -1.1;
  adj[(size_t)p.adj.adj_reg[3]] = first_seed;
  adj[(size_t)p.adj.adj_reg[2]] = second_seed;
  run_adjoint(p, p.adj, val.data(), adj.data());

  double want_a = 0.0, want_b = 0.0, want_c = 0.0;
  KernelCtx direct;
  direct.n_in = 2;
  direct.out = Desc{nullptr, 1};
  direct.in_adj[0] = Desc{&want_c, 1};
  direct.in_adj[1] = Desc{&want_b, 1};
  direct.in[0] = Desc{&c, 1};
  direct.in[1] = Desc{&b, 1};
  direct.out_adj = second_seed;
  test_call_backward(direct);
  direct.in_adj[0] = Desc{&want_a, 1};
  direct.in[0] = Desc{&a, 1};
  direct.out_adj = first_seed;
  test_call_backward(direct);

  expect("CALL cached reverse a bitwise",
         ulps(adj[(size_t)p.adj.adj_reg[0]], want_a) == 0);
  expect("CALL cached reverse b bitwise",
         ulps(adj[(size_t)p.adj.adj_reg[1]], want_b) == 0);
  expect("CALL cached reverse replacement bitwise",
         ulps(adj[(size_t)p.adj.adj_reg[4]], want_c) == 0);
}

// One case, both ways. `tol` is how many ulp of disagreement the case
// tolerates, and is 0 -- bitwise -- everywhere except the fuzzer.
//
// Why the fuzzer is not bitwise: a var copy shares a vari, so from the copy
// onward BOTH registers accumulate into one adjoint. gen_adjoint reproduces
// that by sharing an adjoint cell, but a cell is shared for the whole
// program while a vari is shared only from the copy until the destination
// is next written. When a register is written, read, and only then copied
// over, the two group the same sum differently and the results land an ulp
// or two apart. Expressing the narrower sharing would mean one adjoint cell
// per VALUE rather than per register, which is what makes a range of
// registers stop being a contiguous range of cells -- and contiguous ranges
// are what let one instruction reduce a whole state vector. The residue is
// a reassociation and not an error: measured against the op graph these
// islands replace, the generated adjoint is CLOSER than the replay is
// (docs/benchmarks.md carries the numbers).
static bool check(const std::string& name, Case c, int64_t tol = 0,
                  bool use_softmax3 = false) {
  const IslandProg orig = c.p;  // before checkpoints are inserted
  const bool ok = gen_adjoint(c.p);
  if (!ok) {
    ++failures;
    std::printf("FAIL %s: gen_adjoint refused the program\n", name.c_str());
    return false;
  }
  std::shared_ptr<const Program> optimized;
  if (use_softmax3) {
    c.p.native_adj = true;
    optimized = specialize_softmax3(c.p, 1);
    if (!optimized) {
      ++failures;
      std::printf("FAIL %s: SOFTMAX(3) specialization refused the program\n",
                  name.c_str());
      return false;
    }
  }
  bool passed = true;
  std::vector<double> want_v, got_v;
  const IslandProg& replay = use_softmax3 ? c.p : orig;
  const std::vector<double> want =
      replay_adjoints(replay, c.in, c.seed, &want_v);
  const std::vector<double> got =
      native_adjoints(c.p, c.in, c.seed, &got_v, optimized.get());
  // Values, to the same tolerance as the adjoints. They are normally
  // identical, but DOT is deliberately not the same reduction on the two
  // scalars -- program.hpp runs an Eigen array product for double to match
  // OP_DOT's kernel and stan-math's dot_product for var -- so the oracle's
  // own forward can sit an ulp from the one the island actually ran. That
  // is a property of the replay, not of the generated adjoint: in the
  // executor the island's output always comes from the double pass.
  for (size_t m = 0; m < want_v.size(); ++m)
    if (ulps(want_v[m], got_v[m]) > tol) {
      ++failures;
      passed = false;
      std::printf("FAIL %s: value %zu replay %.17g native %.17g\n",
                  name.c_str(), m, want_v[m], got_v[m]);
    }
  for (size_t k = 0; k < want.size(); ++k) {
    // Bitwise by default: an adjoint that merely rounds to the same place is
    // a regression waiting to happen, and the replay is reachable at any
    // time through STANLI_NO_NATIVE_ADJ to prove it.
    if (ulps(want[k], got[k]) > tol) {
      ++failures;
      passed = false;
      std::printf(
          "FAIL %s: adj[%zu] replay %.17g native %.17g (rel %.2e)\n",
          name.c_str(), k, want[k], got[k],
          std::abs(got[k] - want[k]) / std::max(std::abs(want[k]), 1e-300));
      std::printf("     (%lld ulp, tolerance %lld)\n",
                  (long long)ulps(want[k], got[k]), (long long)tol);
    }
  }
  return passed;
}

static bool check_cfg(const std::string& name, Case c,
                      bool expected_profitable = true) {
  const IslandProg replay = c.p;
  if (!gen_cfg_adjoint(c.p)) {
    ++failures;
    std::printf("FAIL %s: cfg adjoint refused the program\n", name.c_str());
    return false;
  }
  expect((name + " trace map covers generated forward").c_str(),
         c.p.trace_pc.size() == c.p.code.size());
  expect((name + " trace is enabled").c_str(),
         c.p.adj.trace_bits == static_cast<int>(replay.code.size()));
  expect((name + " profitability decision").c_str(),
         cfg_native_profitable(c.p) == expected_profitable);
  std::vector<double> want_v, got_v;
  const std::vector<double> want =
      replay_adjoints(replay, c.in, c.seed, &want_v);
  const std::vector<double> got =
      native_adjoints(c.p, c.in, c.seed, &got_v);
  bool passed = true;
  for (size_t m = 0; m < want_v.size(); ++m)
    if (ulps(want_v[m], got_v[m]) != 0) {
      ++failures;
      passed = false;
      std::printf("FAIL %s: value %zu replay %.17g native %.17g\n",
                  name.c_str(), m, want_v[m], got_v[m]);
    }
  for (size_t k = 0; k < want.size(); ++k)
    if (ulps(want[k], got[k]) != 0) {
      ++failures;
      passed = false;
      std::printf("FAIL %s: adj[%zu] replay %.17g native %.17g (%lld ulp)\n",
                  name.c_str(), k, want[k], got[k],
                  (long long)ulps(want[k], got[k]));
    }
  return passed;
}

// ---- program builders -------------------------------------------------

// n scalar live-ins in registers 0..n-1; the builder appends instructions
// and names the out registers.
struct Build {
  IslandProg p;
  std::vector<double> in;
  std::vector<double> seed;

  explicit Build(std::vector<double> ins) : in(std::move(ins)) {
    for (size_t k = 0; k < in.size(); ++k) {
      p.ins.push_back(IslandProg::LiveIn{(int)k, 1});
      ++p.n_regs;
    }
  }
  // A live-in that is a whole range (a vector argument).
  Build(std::vector<double> ins, int width) : in(std::move(ins)) {
    p.ins.push_back(IslandProg::LiveIn{0, width});
    p.n_regs = width;
  }
  int alloc(int len = 1) {
    const int r = p.n_regs;
    p.n_regs += len;
    return r;
  }
  int konst(double v) {
    const int r = alloc();
    p.code.push_back(
        Program::Instr{Program::CONST, r, (int)p.pool.size(), 0, 0, 1});
    p.pool.push_back(v);
    return r;
  }
  int emit(Program::Code c, int a, int b = 0, int cc = 0, int len = 0,
           int outlen = 1) {
    const int d = alloc(outlen);
    p.code.push_back(Program::Instr{c, d, a, b, cc, len});
    return d;
  }
  // Write into an existing register (aliasing, in-place update).
  void emit_to(Program::Code c, int d, int a, int b = 0, int cc = 0,
               int len = 0) {
    p.code.push_back(Program::Instr{c, d, a, b, cc, len});
  }
  // A DENSITY laid out the way island.cpp and mir_prog.hpp lay one out:
  // three arguments or fewer ride in the instruction, a fourth goes in a
  // contiguous block.
  int emit_density(int id, std::vector<int> argv) {
    const int n = (int)argv.size();
    int a0 = argv[0], a1 = n > 1 ? argv[1] : 0, a2 = n > 2 ? argv[2] : 0;
    if (n > 3) {
      a0 = alloc(n);
      for (int k = 0; k < n; ++k)
        emit_to(Program::MOV, a0 + k, argv[(size_t)k]);
      a1 = 0;
      a2 = 0;
    }
    const int d = alloc();
    p.code.push_back(Program::Instr{Program::DENSITY, d, a0, a1, a2, id});
    return d;
  }
  Case done(std::vector<int> outs, std::vector<double> seeds) {
    p.out_regs = std::move(outs);
    seed = std::move(seeds);
    return Case{p, in, seed};
  }
};

// ---- the unary and binary arithmetic ----------------------------------

static void test_binary_ops() {
  for (Program::Code c :
       {Program::ADD, Program::SUB, Program::MUL, Program::DIV}) {
    Build b({1.7, 0.6});
    const int d = b.emit(c, 0, 1);
    check("binary", b.done({d}, {2.5}));
  }
}

static void test_fma() {
  {
    Build b({1.7, 0.6, -0.9});
    const int d = b.emit(Program::FMA, 0, 1, 2);
    check("fma", b.done({d}, {2.5}));
  }
  {
    // In place over its own addend: dst == c needs the checkpointed a, b.
    Build b({1.7, 0.6, -0.9});
    b.emit_to(Program::FMA, 2, 0, 1, 2);
    check("fma in place", b.done({2}, {2.5}));
  }
  {
    // Repeated accumulation, the recurrence shape arK's lanes carve.
    Build b({0.4, 0.3, 0.2});
    const int d1 = b.emit(Program::FMA, 0, 1, 2);
    const int d2 = b.emit(Program::FMA, 0, d1, 2);
    check("fma chain", b.done({d2}, {1.5}));
  }
}

static void test_unary_ops() {
  const Program::Code codes[] = {
      Program::NEG,    Program::EXP,       Program::LOG,      Program::SQRT,
      Program::SQUARE, Program::INV_LOGIT, Program::LOG1M,    Program::TANH,
      Program::INV,    Program::FABS,      Program::LOG1P_EXP};
  const char* names[] = {"neg",    "exp",       "log",      "sqrt",
                         "square", "inv_logit", "log1m",    "tanh",
                         "inv",    "fabs",      "log1p_exp"};
  for (size_t k = 0; k < sizeof(codes) / sizeof(codes[0]); ++k) {
    Build b({0.37});
    const int d = b.emit(codes[k], 0);
    check(std::string("unary ") + names[k], b.done({d}, {1.3}));
  }
  // fabs on the negative side takes the other branch.
  {
    Build b({-0.37});
    const int d = b.emit(Program::FABS, 0);
    check("fabs negative", b.done({d}, {1.3}));
  }
}

// A chain long enough that a register is read after being overwritten:
// the checkpoint analysis has to notice.
static void test_overwrite_needs_checkpoint() {
  Build b({1.3, 0.8});
  const int t = b.emit(Program::MUL, 0, 1);  // t = a*b
  b.emit_to(Program::MUL, 1, t, 0);          // b = t*a  (overwrites b!)
  const int u = b.emit(Program::MUL, t, 1);  // u = t*b'
  check("overwrite checkpoint", b.done({u}, {0.9}));
}

// dst aliases an operand: the value the adjoint needs is destroyed by the
// very instruction that needs it.
static void test_self_write() {
  Build b({1.3, 0.8});
  b.emit_to(Program::MUL, 0, 0, 1);  // a = a*b, needs the OLD a
  const int u = b.emit(Program::EXP, 0);
  check("self write", b.done({u}, {0.7}));
}

// A live copy: registers c[] hold a copy of v[], and BOTH are read
// afterwards, interleaved. Under the replay a copy shares varis, so a read
// of c accumulates straight into v's adjoint in tape order. The generated
// adjoint has no aliasing to share -- it accumulates into c's own cells and
// moves the total to v when it reaches the copy -- which regroups the sum
// and lands a few ulp away. gen_adjoint aliases the adjoint cells instead
// whenever the copy is dead-ended, which is what keeps this bitwise.
static void test_live_copy_both_read() {
  Build b({0.37, 0.83, 1.21, 0.43}, 4);
  const int c = b.emit(Program::MOVR, 0, 0, 0, 4, 4);
  // Three different functions, so the three contributions to the original's
  // adjoint are three different numbers and the grouping is observable. Two
  // equal contributions would make both orders agree by accident.
  const int r1 = b.emit(Program::EXP, 0);   // reads the original
  const int r2 = b.emit(Program::SQRT, c);  // reads the copy
  const int r3 = b.emit(Program::TANH, 0);  // reads the original again
  const int s1 = b.emit(Program::ADD, r1, r2);
  const int s2 = b.emit(Program::ADD, s1, r3);
  check("live copy both read", b.done({s2}, {1.0}));
}

// The carver's own emission for an unrolled state-space update: a whole
// state vector copied, one element overwritten, the result reduced, and the
// next step reading the result. `iohmm_reg` is this shape at width 1,500,
// and it is the region islands exist for.
static void test_copy_then_modify_chain() {
  Build b({0.31, 0.72, -0.45});  // three parameters feeding the updates
  const int W = 4;
  const int z = b.alloc(W);
  const std::vector<double> zeros((size_t)W, 0.0);
  b.p.code.push_back(
      Program::Instr{Program::CONSTR, z, (int)b.p.pool.size(), 0, 0, W});
  for (int k = 0; k < W; ++k) b.p.pool.push_back(0.1 * k);
  int st = z;
  int acc = -1;
  for (int t = 0; t < 5; ++t) {
    const int d = b.alloc(W);
    b.emit_to(Program::MOVR, d, st, 0, 0, W);  // copy the state
    const int m = b.emit(Program::MUL, st + (t % W), t % 3);
    const int u = b.emit(Program::TANH, m);
    b.emit_to(Program::MOV, d + (t % W), u);  // overwrite one cell
    const int lse = b.emit(Program::LSE_RANGE, d, 0, 0, W);
    acc = acc < 0 ? lse : b.emit(Program::ADD, acc, lse);
    st = d;
  }
  check("copy then modify chain", b.done({acc}, {1.0}));
}

// Copy sharing is storage sharing, not only an arithmetic shortcut. The
// value file keeps its original register ids, while adjoint equivalence
// classes are packed densely. Removing the copied range's old ids must also
// preserve the contiguity the following ranged EXP rule requires.
static void test_compact_adjoint_ranges() {
  Build b({0.31, 0.72, -0.45, 1.1}, 4);
  const int copy = b.alloc(4);
  b.emit_to(Program::MOVR, copy, 0, 0, 0, 4);
  const int out = b.emit(Program::EXP_RANGE, copy, 0, 0, 4, 4);
  Case c = b.done({out, out + 1, out + 2, out + 3}, {0.2, 0.4, 0.6, 0.8});
  Case parity = c;
  expect("compact ranges generated", gen_adjoint(c.p));
  expect("compact ranges forward ids", c.p.adj.adj_reg.size() == 12);
  expect("compact ranges cells", c.p.adj.n_regs == 8);
  const std::vector<int32_t> want_map{0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 6, 7};
  expect("compact ranges map", c.p.adj.adj_reg == want_map);
  expect("compact ranges instruction count", c.p.adj.code.size() == 1);
  if (c.p.adj.code.size() == 1) {
    const AdjInstr& A = c.p.adj.code[0];
    expect("compact ranges opcode", A.code == Program::EXP_RANGE);
    expect("compact ranges contiguous output", A.dst == 4);
    expect("compact ranges contiguous input", A.a == 0);
  }
  check("compact ranges parity", std::move(parity));
}

// The HMM shape writes one state cell repeatedly: a producer writes a private
// temporary and MOV installs it into the state. Destination forwarding removes
// both MOVs before adjoint generation; the direct ADD rules must still consume
// and clear the state adjoint at each write in exactly the replay's order.
static void test_forwarded_repeated_destination() {
  Build b({0.31, 0.72, -0.45, 1.1});
  const int state = b.alloc();
  const int first_tmp = b.emit(Program::ADD, 0, 1);
  b.emit_to(Program::MOV, state, first_tmp);
  const int first_use = b.emit(Program::MUL, state, 2);
  const int second_tmp = b.emit(Program::ADD, 2, 3);
  b.emit_to(Program::MOV, state, second_tmp);
  const int second_use = b.emit(Program::MUL, state, 0);
  const int out = b.emit(Program::ADD, first_use, second_use);
  Case c = b.done({out}, {0.83});
  compact_island(c.p);
  int moves = 0;
  for (const Program::Instr& I : c.p.code)
    if (I.code == Program::MOV || I.code == Program::MOVR) ++moves;
  expect("forwarded repeated destination removes copies", moves == 0);
  check("forwarded repeated destination", std::move(c));
}

static void test_forwarded_saveout_last_write() {
  Build b({0.31, 0.72});
  const int state = b.alloc();
  b.emit_to(Program::MOV, state, 1);  // an earlier, overwritten initializer
  const int temporary = b.emit(Program::EXP, 0);
  b.emit_to(Program::MOV, state, temporary);
  const int out = b.emit(Program::MUL, state, 1);
  Case c = b.done({out}, {0.83});
  compact_island(c.p);
  bool exp_then_copy = false;
  for (size_t i = 0; i + 1 < c.p.code.size(); ++i)
    if (c.p.code[i].code == Program::EXP &&
        (c.p.code[i + 1].code == Program::MOV ||
         c.p.code[i + 1].code == Program::MOVR))
      exp_then_copy = true;
  expect("SaveOut final write forwards", !exp_then_copy);
  check("forwarded SaveOut final write", std::move(c));
}

static void test_saveout_later_write_refuses_forwarding() {
  Build b({0.31, 0.72});
  const int state = b.alloc();
  b.emit_to(Program::MOV, state, 1);
  const int temporary = b.emit(Program::EXP, 0);
  b.emit_to(Program::MOV, state, temporary);
  const int first_use = b.emit(Program::MUL, state, 1);
  b.emit_to(Program::MOV, state, 0);  // would overwrite EXP's saved output
  const int out = b.emit(Program::ADD, first_use, state);
  Case c = b.done({out}, {0.83});
  compact_island(c.p);
  bool exp_then_copy = false;
  for (size_t i = 0; i + 1 < c.p.code.size(); ++i)
    if (c.p.code[i].code == Program::EXP &&
        (c.p.code[i + 1].code == Program::MOV ||
         c.p.code[i + 1].code == Program::MOVR))
      exp_then_copy = true;
  expect("SaveOut later write keeps copy", exp_then_copy);
  check("SaveOut later write", std::move(c));
}

static void test_forwarded_ranged_producers() {
  for (Program::Code code : {Program::LOG_RANGE, Program::EXP_RANGE}) {
    Build b({0.31, 0.72, 1.45}, 3);
    const int left = b.alloc();
    const int destination = b.alloc(3);
    const int right = b.alloc();
    const int temporary = b.emit(code, 0, 0, 0, 3, 3);
    b.emit_to(Program::MOVR, destination, temporary, 0, 0, 3);
    // Make the copy boundaries interior to a later ranged read. The ordinary
    // source-alias pass must keep that MOVR, so its disappearance specifically
    // exercises producer destination forwarding.
    b.p.pool = {-0.45, 1.1};
    b.emit_to(Program::CONST, left, 0);
    b.emit_to(Program::CONST, right, 1);
    const int out = b.emit(Program::LSE_RANGE, left, 0, 0, 5);
    Case c = b.done({out}, {0.83});
    compact_island(c.p);
    bool producer_then_copy = false;
    for (size_t i = 0; i + 1 < c.p.code.size(); ++i)
      if (c.p.code[i].code == code && c.p.code[i + 1].code == Program::MOVR)
        producer_then_copy = true;
    expect(code == Program::LOG_RANGE ? "LOG_RANGE destination forwards"
                                      : "EXP_RANGE destination forwards",
           !producer_then_copy);
    check(code == Program::LOG_RANGE ? "forwarded LOG_RANGE"
                                     : "forwarded EXP_RANGE",
          std::move(c));
  }
}

static void test_ranged_saveout_partial_later_write_refuses() {
  Build b({0.31, 0.72, 1.45, -0.45}, 4);
  const int state = b.alloc(3);
  const int temporary = b.emit(Program::EXP_RANGE, 0, 0, 0, 3, 3);
  b.emit_to(Program::MOVR, state, temporary, 0, 0, 3);
  const int before = b.emit(Program::LSE_RANGE, state, 0, 0, 3);
  b.emit_to(Program::MOV, state + 1, 3);
  const int after = b.emit(Program::LSE_RANGE, state, 0, 0, 3);
  const int out = b.emit(Program::ADD, before, after);
  Case c = b.done({out}, {0.83});
  compact_island(c.p);
  bool exp_then_copy = false;
  for (size_t i = 0; i + 1 < c.p.code.size(); ++i)
    if (c.p.code[i].code == Program::EXP_RANGE &&
        c.p.code[i + 1].code == Program::MOVR)
      exp_then_copy = true;
  expect("ranged SaveOut one-lane later write keeps copy", exp_then_copy);
  check("ranged SaveOut one-lane later write", std::move(c));
}

static void test_accumulate_into_one_register() {
  // OP_ADD_N lowers to a chain of ADD d,d,x -- pure routing, but the
  // adjoint has to keep the running register's adjoint alive across it.
  Build b({0.4, 0.9, 1.1, 0.25});
  const int d = b.emit(Program::ADD, 0, 1);
  b.emit_to(Program::ADD, d, d, 2);
  b.emit_to(Program::ADD, d, d, 3);
  check("add_n chain", b.done({d}, {1.5}));
}

// ---- ranges and reductions --------------------------------------------

static void test_ranged() {
  {
    Build b({0.3, 0.7, 1.4, 0.2}, 4);
    const int d = b.emit(Program::EXP_RANGE, 0, 0, 0, 4, 4);
    check("exp_range", b.done({d, d + 1, d + 2, d + 3}, {1.1, 0.4, 2.0, 0.7}));
  }
  {
    Build b({0.3, 0.7, 1.4, 0.2}, 4);
    const int d = b.emit(Program::LOG_RANGE, 0, 0, 0, 4, 4);
    check("log_range", b.done({d, d + 1, d + 2, d + 3}, {1.1, 0.4, 2.0, 0.7}));
  }
  {
    Build b({0.3, 0.7, 1.4, 0.2}, 4);
    const int d = b.emit(Program::MOVR, 0, 0, 0, 4, 4);
    check("movr", b.done({d, d + 1, d + 2, d + 3}, {1.1, 0.4, 2.0, 0.7}));
  }
}

// The double interpreter has a stack-backed SOFTMAX(3) result.  Compare it
// directly with the owning Stan Math expression it replaces, including every
// legal overlap between the three-lane source and destination ranges.  Full
// register-file memcmp pins NaN payloads, signed zero, and untouched cells as
// well as the ordinary finite result.
static void test_softmax3_double_exact() {
  using Vec = Eigen::Matrix<double, Eigen::Dynamic, 1>;
  const double inf = std::numeric_limits<double>::infinity();
  const auto from_bits = [](uint64_t bits) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };
  const double qnan = from_bits(UINT64_C(0x7ff8000000001234));
  const double snan = from_bits(UINT64_C(0x7ff0000000001234));
  std::vector<std::vector<double>> cases{
      {0.0, -36.7368005696771, -36.7368005696771},
      {-0.0, 0.0, -0.0},
      {2.0, 2.0, -3.0},
      {inf, inf, 0.0},
      {-inf, -inf, -inf},
      {qnan, 0.0, -1.0},
      {0.0, qnan, -1.0},
      {0.0, -1.0, qnan},
      {snan, 0.0, -1.0},
      {0.0, snan, -1.0},
      {0.0, -1.0, snan},
  };
  for (int i = 0; i < 256; ++i) {
    cases.push_back({73.0 * std::sin(0.71 * i), 91.0 * std::cos(0.43 * i + 0.2),
                     57.0 * std::sin(1.13 * i - 0.4)});
  }

  constexpr int src = 4;
  for (size_t trial = 0; trial < cases.size(); ++trial) {
    for (int delta = -2; delta <= 2; ++delta) {
      const int dst = src + delta;
      std::vector<double> before{101.0, 102.0, 103.0, 104.0, 105.0, 106.0,
                                 107.0, 108.0, 109.0, 110.0, 111.0, 112.0};
      for (int i = 0; i < 3; ++i)
        before[(size_t)(src + i)] = cases[trial][(size_t)i];

      std::vector<double> want = before;
      const Eigen::Map<const Vec> input(&want[(size_t)src], 3);
      const Vec result = stan::math::softmax(input);
      for (int i = 0; i < 3; ++i) want[(size_t)(dst + i)] = result(i);

      IslandProg p;
      p.n_regs = (int)before.size();
      p.code.push_back(Program::Instr{Program::SOFTMAX, dst, src, 0, 0, 3});
      p.native_adj = true;
      const auto optimized = specialize_softmax3(p, 1);
      expect("softmax3 exact specialized", static_cast<bool>(optimized));
      std::vector<double> got = before;
      if (optimized) run_program(*optimized, got.data());

      const bool same = std::memcmp(got.data(), want.data(),
                                    got.size() * sizeof(double)) == 0;
      if (!same) {
        ++failures;
        std::printf("FAIL softmax3 exact trial %zu overlap %+d\n", trial,
                    delta);
      }
    }
  }
}

static void test_softmax3_activation() {
  // Both reused kernels are built-ins: lookup must work before any program
  // has been specialized, through the same thread-safe registry as all other
  // executor kernels. Keeping this first catches a return to lazy writes.
  expect("softmax3 program kernel registered",
         find_kernel(OP_SOFTMAX) != nullptr);
  expect("softmax3 island kernel registered",
         find_kernel(OP_ISLAND) != nullptr);
  expect("softmax3 private slot initially empty",
         find_kernel(kProgramSoftmax3Opcode) == nullptr);

  const auto make_program = [](int n) {
    IslandProg p;
    p.native_adj = true;
    p.n_regs = 16;
    for (int i = 0; i < n; ++i)
      p.code.push_back(Program::Instr{Program::SOFTMAX, 4, 0, 0, 0, 3});
    return p;
  };
  const auto seed_existing_payload = [](IslandProg& p) {
    Program::Call call;
    call.opcode = OP_EXP;
    call.n_in = 1;
    call.in[0] = 7;
    call.in_len[0] = 1;
    call.out = 8;
    call.out_len = 1;
    call.idata = {11, 13, 17};
    expect("softmax3 seed CALL binds kernel", bind_call(call));
    p.calls.push_back(std::move(call));
    p.code.insert(p.code.begin(), Program::Instr{Program::CALL, 8, 0, 0, 0, 0});
    p.out_regs = {8};
  };
  const auto same_instr = [](const Program::Instr& a, const Program::Instr& b) {
    return a.code == b.code && a.dst == b.dst && a.a == b.a && a.b == b.b &&
           a.c == b.c && a.len == b.len;
  };
  const auto pad_clone_bytes = [](IslandProg& p, size_t target) {
    size_t bytes = p.code.size() * sizeof(Program::Instr) +
                   p.calls.size() * sizeof(Program::Call) +
                   p.pool.size() * sizeof(double) +
                   p.out_regs.size() * sizeof(int);
    for (const auto& call : p.calls) bytes += call.idata.size() * sizeof(int);
    size_t sites = 0;
    for (const auto& I : p.code)
      if (I.code == Program::SOFTMAX && I.len == 3) ++sites;
    bytes += sites * sizeof(Program::Call);
    expect("softmax3 boundary target fits", bytes <= target);
    expect("softmax3 boundary padding aligned",
           (target - bytes) % sizeof(double) == 0);
    if (bytes <= target && (target - bytes) % sizeof(double) == 0)
      p.pool.resize(p.pool.size() + (target - bytes) / sizeof(double));
  };

  IslandProg replay = make_program(32);
  replay.native_adj = false;
  expect("softmax3 native gate", !specialize_softmax3(replay));
  expect("softmax3 native gate leaves opcode",
         replay.code[0].code == Program::SOFTMAX);

  IslandProg below = make_program(31);
  expect("softmax3 count gate", !specialize_softmax3(below));

  // Equality is admitted and one allocation unit beyond is refused at both
  // limits.  32 sites exercise the per-site bound; 513 make the 2 MiB
  // absolute cap tighter than the per-site allowance.
  constexpr size_t per_site_limit = 32 * 4096;
  IslandProg per_site_exact = make_program(32);
  seed_existing_payload(per_site_exact);
  pad_clone_bytes(per_site_exact, per_site_limit);
  const auto per_site_plan = specialize_softmax3(per_site_exact);
  expect("softmax3 per-site equality", static_cast<bool>(per_site_plan));
  IslandProg per_site_over = make_program(32);
  seed_existing_payload(per_site_over);
  pad_clone_bytes(per_site_over, per_site_limit);
  const std::vector<Program::Instr> per_site_canonical = per_site_over.code;
  per_site_over.calls.front().idata.push_back(19);
  expect("softmax3 per-site byte gate", !specialize_softmax3(per_site_over));
  expect(
      "softmax3 per-site gate leaves canonical code",
      per_site_over.code.size() == per_site_canonical.size() &&
          same_instr(per_site_over.code.front(), per_site_canonical.front()));

  constexpr size_t absolute_limit = 2 * 1024 * 1024;
  IslandProg absolute_exact = make_program(513);
  pad_clone_bytes(absolute_exact, absolute_limit);
  const auto absolute_plan = specialize_softmax3(absolute_exact);
  expect("softmax3 absolute equality", static_cast<bool>(absolute_plan));
  IslandProg absolute_over = make_program(513);
  pad_clone_bytes(absolute_over, absolute_limit);
  const std::vector<Program::Instr> absolute_canonical = absolute_over.code;
  Program::Call absolute_extra;
  absolute_extra.idata.push_back(1);
  absolute_over.calls.push_back(std::move(absolute_extra));
  expect("softmax3 absolute byte gate", !specialize_softmax3(absolute_over));
  expect(
      "softmax3 absolute gate leaves canonical code",
      absolute_over.code.size() == absolute_canonical.size() &&
          same_instr(absolute_over.code.front(), absolute_canonical.front()));

  IslandProg empty = make_program(0);
  expect("softmax3 empty min zero", !specialize_softmax3(empty, 0));

  IslandProg eligible = make_program(32);
  Program::Call existing;
  existing.opcode = OP_EXP;
  existing.n_in = 1;
  existing.in[0] = 7;
  existing.in_len[0] = 1;
  existing.out = 8;
  existing.out_len = 1;
  expect("softmax3 existing CALL binds kernel", bind_call(existing));
  eligible.calls.push_back(existing);
  eligible.code.insert(eligible.code.begin(),
                       Program::Instr{Program::CALL, 8, 0, 0, 0, 0});
  eligible.code.push_back(Program::Instr{Program::SOFTMAX, 8, 0, 0, 0, 4});
  const std::vector<Program::Instr> canonical = eligible.code;
  const auto optimized_plan = specialize_softmax3(eligible);
  expect("softmax3 threshold activates", static_cast<bool>(optimized_plan));
  expect("softmax3 private kernel registered",
         find_kernel(kProgramSoftmax3Opcode) != nullptr);
  expect("softmax3 canonical code size",
         eligible.code.size() == canonical.size());
  bool canonical_same = eligible.code.size() == canonical.size();
  for (size_t i = 0; i < eligible.code.size() && canonical_same; ++i)
    canonical_same = same_instr(eligible.code[i], canonical[i]);
  expect("softmax3 canonical bytecode", canonical_same);
  expect("softmax3 canonical calls", eligible.calls.size() == 1);

  const Program& optimized = *optimized_plan;
  expect("softmax3 clone keeps existing call",
         optimized.code[0].code == Program::CALL && optimized.code[0].a == 0 &&
             optimized.calls[0].opcode == OP_EXP);
  size_t rewritten = 0;
  for (size_t i = 1; i + 1 < optimized.code.size(); ++i) {
    const auto& I = optimized.code[i];
    if (I.code != Program::CALL) continue;
    ++rewritten;
    const auto& call = optimized.calls[(size_t)I.a];
    expect("softmax3 call opcode", call.opcode == kProgramSoftmax3Opcode);
    expect("softmax3 call variant", call.variant == kProgramSoftmax3Variant);
    expect("softmax3 call input",
           call.n_in == 1 && call.in[0] == 0 && call.in_len[0] == 3);
    expect("softmax3 call output", call.out == 4 && call.out_len == 3);
  }
  expect("softmax3 rewrites threshold", rewritten == 32);
  expect("softmax3 leaves other lengths",
         optimized.code.back().code == Program::SOFTMAX &&
             optimized.code.back().len == 4);
  expect("softmax3 appends calls", optimized.calls.size() == 33);
}

static int call_activity_forwards = 0;
static int call_activity_backwards = 0;

static void call_activity_fwd(KernelCtx& ctx) {
  ++call_activity_forwards;
  const double x = ctx.in[0].data[0];
  ctx.out.data[0] = x * x;
  for (int k = 0; k < 3; ++k) ctx.scratch[k] = x + k;
}

static void call_activity_bwd(KernelCtx& ctx) {
  ++call_activity_backwards;
  ctx.in_adj[0].data[0] += (2.0 * ctx.in[0].data[0]) * ctx.out_adj_vec.data[0];
}

static void test_call_activity_elision() {
  // The private opcode was exercised by the softmax tests above. Reuse its
  // otherwise-test-only registry entry for a kernel whose backward invocation
  // is observable here.
  register_kernel(kProgramSoftmax3Opcode,
                  Kernel{call_activity_fwd, call_activity_bwd, nullptr});

  const auto run = [](bool call_input_active) {
    IslandProg p;
    p.n_regs = 8;
    p.ins.push_back(IslandProg::LiveIn{0, 1, 0, 0, call_input_active});
    p.ins.push_back(IslandProg::LiveIn{1, 1, 1, 0, true});
    Program::Call call;
    call.opcode = kProgramSoftmax3Opcode;
    call.n_in = 1;
    call.in[0] = 0;
    call.in_len[0] = 1;
    call.out = 2;
    call.out_len = 1;
    call.scratch = 3;
    call.scratch_len = 3;
    expect("CALL activity binds kernel", bind_call(call));
    p.calls.push_back(call);
    p.code.push_back(Program::Instr{Program::CALL, 2, 0, 0, 0, 0});
    // Overwrite the CALL input after it runs. An active CALL backward needs
    // the generated value checkpoint; an inactive one does not run at all.
    p.code.push_back(Program::Instr{Program::ADD, 0, 0, 1, 0, 0});
    p.code.push_back(Program::Instr{Program::MUL, 7, 2, 0, 0, 0});
    p.out_regs = {7};

    expect("CALL activity adjoint generated", gen_adjoint(p));
    int adjoint_calls = 0;
    for (const AdjInstr& instr : p.adj.code)
      if (instr.code == Program::CALL) ++adjoint_calls;
    expect(call_input_active ? "active CALL retains backward"
                             : "inactive CALL elides backward",
           adjoint_calls == (call_input_active ? 1 : 0));
    expect("CALL scratch shares one dead adjoint cell",
           p.adj.adj_reg[3] == p.adj.adj_reg[0] &&
               p.adj.adj_reg[4] == p.adj.adj_reg[0] &&
               p.adj.adj_reg[5] == p.adj.adj_reg[0]);
    expect(call_input_active ? "active CALL checkpoints overwritten input"
                             : "inactive CALL skips value checkpoint",
           call_input_active ? p.calls[0].bwd_value_in[0] >= 8
                             : p.calls[0].bwd_value_in[0] == 0);

    std::vector<double> values((size_t)p.n_regs, 0.0);
    values[0] = 2.0;
    values[1] = 3.0;
    call_activity_forwards = 0;
    call_activity_backwards = 0;
    run_program(p, values.data());
    std::vector<double> adj((size_t)p.adj.n_regs, 0.0);
    adj[(size_t)p.adj.adj_reg[7]] = 1.0;
    run_adjoint(p, p.adj, values.data(), adj.data());

    expect("CALL activity forward value", values[7] == 20.0);
    expect("CALL activity forward invoked", call_activity_forwards == 1);
    expect(call_input_active ? "active CALL backward invoked"
                             : "inactive CALL backward not invoked",
           call_activity_backwards == (call_input_active ? 1 : 0));
    expect("CALL activity parameter gradient",
           adj[(size_t)p.adj.adj_reg[1]] == 4.0);
    if (call_input_active)
      expect("active CALL input gradient uses checkpoint",
             adj[(size_t)p.adj.adj_reg[0]] == 24.0);
    expect("CALL output adjoint consumed",
           adj[(size_t)p.adj.adj_reg[2]] == 0.0);
  };

  run(false);
  run(true);
}

static Case terminal_jz_case(double condition) {
  Build b({condition, 1.7, 0.6});
  const int prefix = b.emit(Program::MUL, 1, 2);
  const size_t jz = b.p.code.size();
  b.p.code.push_back(Program::Instr{Program::JZ, 0, 0});
  // Both overwritten live-ins make the test exercise ordinary value
  // checkpoints and the branch-condition checkpoint together. The outputs
  // retain their old live-in values when the body is skipped.
  b.emit_to(Program::ADD, 1, prefix, 2);
  b.emit_to(Program::SQUARE, 0, 1);
  b.p.code[jz].dst = static_cast<int32_t>(b.p.code.size());
  return b.done({0, 1, prefix}, {1.1, -0.4, 0.7});
}

static void test_terminal_jz_adjoint() {
  check("terminal JZ taken", terminal_jz_case(1.0));
  check("terminal JZ skipped", terminal_jz_case(0.0));

  {
    Case c = terminal_jz_case(1.0);
    const int old_regs = c.p.n_regs;
    expect("terminal JZ generates adjoint", gen_adjoint(c.p));
    const auto guard = std::find_if(
        c.p.adj.code.begin(), c.p.adj.code.end(),
        [](const AdjInstr& instr) { return instr.code == Program::JZ; });
    expect("terminal JZ has one reverse guard",
           guard != c.p.adj.code.end() &&
               std::count_if(c.p.adj.code.begin(), c.p.adj.code.end(),
                             [](const AdjInstr& instr) {
                               return instr.code == Program::JZ;
                             }) == 1);
    expect("terminal JZ checkpoints overwritten condition",
           guard != c.p.adj.code.end() && guard->va >= old_regs);
    const auto forward_guard = std::find_if(
        c.p.code.begin(), c.p.code.end(),
        [](const Program::Instr& instr) { return instr.code == Program::JZ; });
    expect("terminal JZ retargets around checkpoints",
           forward_guard != c.p.code.end() &&
               forward_guard->dst == static_cast<int>(c.p.code.size()));
  }
  const auto refused = [](std::vector<Program::Instr> code) {
    IslandProg p;
    p.n_regs = 4;
    p.ins.push_back(IslandProg::LiveIn{0, 4});
    p.code = std::move(code);
    p.out_regs = {3};
    return !gen_adjoint(p);
  };
  expect("nonterminal JZ refused", refused({{Program::JZ, 2, 0},
                                            {Program::SQUARE, 3, 1},
                                            {Program::ADD, 3, 1, 2}}));
  expect("backedge JZ refused",
         refused({{Program::ADD, 3, 1, 2}, {Program::JZ, 0, 0}}));
  expect("multiple JZs refused", refused({{Program::JZ, 3, 0},
                                          {Program::JZ, 3, 1},
                                          {Program::SQUARE, 3, 2}}));
  expect("JMP control refused",
         refused({{Program::JMP, 2}, {Program::SQUARE, 3, 1}}));
}

static Case nested_cfg_case(double outer, double inner) {
  Build b({outer, inner, 0.37, 0.83});
  const int temporary = b.alloc();
  const int selected = b.alloc();
  b.p.code = {
      {Program::JZ, 8, 0},
      {Program::JZ, 5, 1},
      {Program::MUL, temporary, 2, 3},
      {Program::MOV, selected, temporary},
      {Program::JMP, 10},
      {Program::SQUARE, temporary, 2},
      {Program::MOV, selected, temporary},
      {Program::JMP, 10},
      {Program::EXP, temporary, 3},
      {Program::MOV, selected, temporary},
      // Repeated destination after the join forces the selected value to be
      // checkpointed; both untaken arm writes must remain absent in reverse.
      {Program::ADD, selected, selected, 2},
  };
  return b.done({selected}, {1.3});
}

// One branch containing the dominant scalar pair families. The output is
// initialized before the branch so both taken and skipped paths are valid;
// the taken arm overwrites that same register, exercising checkpoint and
// repeated-destination semantics as well as pair dispatch.
static Case scalar_pair_cfg_case(double condition) {
  Build b({condition, 0.73, 1.17});
  const int selected = b.alloc();
  b.emit_to(Program::MOV, selected, 1);
  const size_t skip = b.p.code.size();
  b.p.code.push_back({Program::JZ, 0, 0});
  int chain = 1;
  for (int k = 0; k < 10; ++k) chain = b.emit(Program::NEG, chain);
  chain = b.emit(Program::ADD, chain, 2);
  chain = b.emit(Program::MUL, chain, 2);
  chain = b.emit(Program::MUL, chain, 2);
  chain = b.emit(Program::ADD, chain, 2);
  chain = b.emit(Program::MUL, chain, 2);
  chain = b.emit(Program::MUL, chain, 2);
  chain = b.emit(Program::ADD, chain, 2);
  chain = b.emit(Program::ADD, chain, 2);
  chain = b.emit(Program::SUB, chain, 2);
  chain = b.emit(Program::SUB, chain, 2);
  b.emit_to(Program::MOV, selected, chain);
  b.p.code[skip].dst = static_cast<int>(b.p.code.size());
  return b.done({selected}, {1.3});
}

// The same families with every destination overwriting its first operand.
// This is the consume-clear-add corner: dst and a share an adjoint cell, so
// each pair handler must retain the exact read/clear/update order.
static Case inplace_pair_cfg_case(double condition) {
  Build b({condition, 0.73, 1.17});
  const size_t skip = b.p.code.size();
  b.p.code.push_back({Program::JZ, 0, 0});
  b.emit_to(Program::NEG, 1, 1);
  b.emit_to(Program::NEG, 1, 1);
  b.emit_to(Program::ADD, 1, 1, 2);
  b.emit_to(Program::MUL, 1, 1, 2);
  b.emit_to(Program::MUL, 1, 1, 2);
  b.emit_to(Program::ADD, 1, 1, 2);
  b.emit_to(Program::MUL, 1, 1, 2);
  b.emit_to(Program::MUL, 1, 1, 2);
  b.emit_to(Program::ADD, 1, 1, 2);
  b.emit_to(Program::ADD, 1, 1, 2);
  b.emit_to(Program::SUB, 1, 1, 2);
  b.emit_to(Program::SUB, 1, 1, 2);
  b.p.code[skip].dst = static_cast<int>(b.p.code.size());
  return b.done({1}, {1.3});
}

static size_t adj_pair_count(const IslandProg& p) {
  return static_cast<size_t>(std::count_if(
      p.adj.code.begin(), p.adj.code.end(),
      [](const AdjInstr& instruction) {
        return instruction.pair != AdjPair::None;
      }));
}

static bool trace_plan_partitions(const IslandProg& p) {
  int32_t begin = 0;
  for (const AdjTraceBlock& block : p.adj.trace_blocks) {
    if (block.end <= begin ||
        block.end > static_cast<int32_t>(p.adj.code.size()) ||
        block.trace_pc < 0 || block.trace_pc >= p.adj.trace_bits)
      return false;
    begin = block.end;
  }
  return !p.adj.trace_blocks.empty() &&
         begin == static_cast<int32_t>(p.adj.code.size());
}

static void test_cfg_trace_blocks() {
  test_unsetenv("STANLI_CFG_ADJ_TRACE_BLOCKS");
  test_unsetenv("STANLI_NO_CFG_ADJ_TRACE_BLOCKS");
  test_unsetenv("STANLI_CFG_ADJ_SUPERINSTRUCTIONS");
  test_unsetenv("STANLI_NO_CFG_ADJ_SUPERINSTRUCTIONS");
  {
    Case c = nested_cfg_case(1.0, 0.0);
    expect("cfg trace blocks default generation succeeds",
           gen_cfg_adjoint(c.p));
    expect("cfg trace blocks default off", c.p.adj.trace_blocks.empty());
    expect("cfg scalar pairs default off",
           !c.p.adj.has_pairs && adj_pair_count(c.p) == 0);
  }

  test_setenv("STANLI_CFG_ADJ_TRACE_BLOCKS", "1", 1);
  for (const auto& path : {std::pair<double, double>{1.0, 1.0},
                           {1.0, 0.0}, {0.0, 1.0}}) {
    Case c = nested_cfg_case(path.first, path.second);
    const IslandProg replay = c.p;
    expect("cfg trace block generation succeeds", gen_cfg_adjoint(c.p));
    expect("cfg trace block plan partitions reverse",
           trace_plan_partitions(c.p));
    expect("cfg trace-only plan does not tag scalar pairs",
           !c.p.adj.has_pairs && adj_pair_count(c.p) == 0);
    std::vector<double> want_value, got_value;
    const std::vector<double> want =
        replay_adjoints(replay, c.in, c.seed, &want_value);
    const std::vector<double> got =
        native_adjoints(c.p, c.in, c.seed, &got_value);
    expect("cfg trace block value bitwise",
           want_value.size() == got_value.size() &&
               std::equal(want_value.begin(), want_value.end(),
                          got_value.begin()));
    expect("cfg trace block adjoint bitwise",
           want.size() == got.size() &&
               std::equal(want.begin(), want.end(), got.begin()));
  }

  {
    Case c = nested_cfg_case(1.0, 0.0);
    test_setenv("STANLI_NO_CFG_ADJ_TRACE_BLOCKS", "1", 1);
    expect("cfg trace escape recompilation succeeds", gen_cfg_adjoint(c.p));
    expect("cfg trace escape authoritative", c.p.adj.trace_blocks.empty());
    test_unsetenv("STANLI_NO_CFG_ADJ_TRACE_BLOCKS");
  }

  // The explicit seam lets malformed-map behavior remain a focused unit
  // property rather than relying on a generator bug to manufacture it.
  {
    Case c = nested_cfg_case(1.0, 0.0);
    test_unsetenv("STANLI_CFG_ADJ_TRACE_BLOCKS");
    expect("cfg trace malformed base generation succeeds",
           gen_cfg_adjoint(c.p));
    IslandProg bad_forward = c.p;
    auto mapped = std::find_if(bad_forward.trace_pc.begin(),
                               bad_forward.trace_pc.end(),
                               [](int32_t pc) { return pc >= 0; });
    expect("cfg trace malformed has mapped forward",
           mapped != bad_forward.trace_pc.end());
    if (mapped != bad_forward.trace_pc.end()) *mapped = -1;
    expect("cfg trace malformed forward fails closed",
           !prepare_cfg_trace_blocks(bad_forward) &&
               bad_forward.adj.trace_blocks.empty());

    IslandProg bad_reverse = c.p;
    bad_reverse.adj.code.front().fwd_pc = -1;
    expect("cfg trace malformed reverse fails closed",
           !prepare_cfg_trace_blocks(bad_reverse) &&
               bad_reverse.adj.trace_blocks.empty());

    IslandProg backedge = c.p;
    auto branch = std::find_if(
        backedge.code.begin(), backedge.code.end(),
        [](const Program::Instr& instruction) {
          return instruction.code == Program::JZ ||
                 instruction.code == Program::JMP;
        });
    expect("cfg trace malformed has final branch",
           branch != backedge.code.end());
    if (branch != backedge.code.end()) branch->dst = 0;
    expect("cfg trace final backedge fails closed",
           !prepare_cfg_trace_blocks(backedge) &&
               backedge.adj.trace_blocks.empty());
  }
  test_unsetenv("STANLI_CFG_ADJ_TRACE_BLOCKS");
  test_unsetenv("STANLI_NO_CFG_ADJ_TRACE_BLOCKS");

  test_setenv("STANLI_CFG_ADJ_SUPERINSTRUCTIONS", "1", 1);
  for (double condition : {1.0, 0.0}) {
    Case c = scalar_pair_cfg_case(condition);
    const IslandProg replay = c.p;
    expect("cfg pair generation succeeds", gen_cfg_adjoint(c.p));
    expect("cfg pair force implicitly builds trace blocks",
           trace_plan_partitions(c.p));
    expect("cfg pair force tags scalar pairs",
           c.p.adj.has_pairs && adj_pair_count(c.p) >= 5);
    int32_t begin = 0;
    for (const AdjTraceBlock& block : c.p.adj.trace_blocks) {
      for (int32_t pc = begin; pc < block.end; ++pc) {
        const AdjInstr& instruction = c.p.adj.code[static_cast<size_t>(pc)];
        if (instruction.pair == AdjPair::None) continue;
        expect("cfg pair remains inside one trace block", pc + 1 < block.end);
        if (pc + 1 < block.end)
          expect("cfg pair second entry is never a pair head",
                 c.p.adj.code[static_cast<size_t>(pc + 1)].pair ==
                     AdjPair::None);
      }
      begin = block.end;
    }
    std::vector<double> want_value, got_value;
    const std::vector<double> want =
        replay_adjoints(replay, c.in, c.seed, &want_value);
    const std::vector<double> got =
        native_adjoints(c.p, c.in, c.seed, &got_value);
    expect(condition == 0.0 ? "cfg pair skipped value bitwise"
                            : "cfg pair taken value bitwise",
           want_value == got_value);
    expect(condition == 0.0 ? "cfg pair skipped adjoint bitwise"
                            : "cfg pair taken adjoint bitwise",
           want == got);
  }

  for (double condition : {1.0, 0.0}) {
    Case c = inplace_pair_cfg_case(condition);
    const IslandProg replay = c.p;
    expect("cfg in-place pair generation succeeds", gen_cfg_adjoint(c.p));
    expect("cfg in-place pair tags overlapping scalar rules",
           c.p.adj.has_pairs && adj_pair_count(c.p) >= 5);
    std::vector<double> want_value, got_value;
    const std::vector<double> want =
        replay_adjoints(replay, c.in, c.seed, &want_value);
    const std::vector<double> got =
        native_adjoints(c.p, c.in, c.seed, &got_value);
    expect(condition == 0.0 ? "cfg in-place skipped value bitwise"
                            : "cfg in-place taken value bitwise",
           want_value == got_value);
    expect(condition == 0.0 ? "cfg in-place skipped adjoint bitwise"
                            : "cfg in-place taken adjoint bitwise",
           want == got);
  }

  {
    Case c = scalar_pair_cfg_case(1.0);
    test_setenv("STANLI_NO_CFG_ADJ_SUPERINSTRUCTIONS", "1", 1);
    expect("cfg pair escape recompilation succeeds", gen_cfg_adjoint(c.p));
    expect("cfg pair escape is authoritative",
           !c.p.adj.has_pairs && adj_pair_count(c.p) == 0);
    test_unsetenv("STANLI_NO_CFG_ADJ_SUPERINSTRUCTIONS");
  }
  {
    Case c = scalar_pair_cfg_case(1.0);
    test_setenv("STANLI_NO_CFG_ADJ_TRACE_BLOCKS", "1", 1);
    expect("cfg pair trace escape recompilation succeeds",
           gen_cfg_adjoint(c.p));
    expect("cfg pair trace escape fails closed",
           c.p.adj.trace_blocks.empty() && !c.p.adj.has_pairs &&
               adj_pair_count(c.p) == 0);
    test_unsetenv("STANLI_NO_CFG_ADJ_TRACE_BLOCKS");
  }

  // The pair seam independently revalidates the trace partition. A caller
  // cannot merge neighboring blocks and thereby pair two differently
  // controlled instructions under one representative trace bit.
  {
    test_unsetenv("STANLI_CFG_ADJ_SUPERINSTRUCTIONS");
    test_setenv("STANLI_CFG_ADJ_TRACE_BLOCKS", "1", 1);
    Case c = scalar_pair_cfg_case(1.0);
    expect("cfg pair malformed base generation succeeds",
           gen_cfg_adjoint(c.p));
    expect("cfg pair malformed base has multiple blocks",
           c.p.adj.trace_blocks.size() > 1);
    if (c.p.adj.trace_blocks.size() > 1)
      c.p.adj.trace_blocks.front().end = c.p.adj.trace_blocks[1].end;
    expect("cfg pair malformed partition fails transactionally",
           !prepare_cfg_adjoint_superinstructions(c.p) &&
               !c.p.adj.has_pairs && adj_pair_count(c.p) == 0);
  }
  test_unsetenv("STANLI_CFG_ADJ_TRACE_BLOCKS");
  test_unsetenv("STANLI_NO_CFG_ADJ_TRACE_BLOCKS");
  test_unsetenv("STANLI_CFG_ADJ_SUPERINSTRUCTIONS");
  test_unsetenv("STANLI_NO_CFG_ADJ_SUPERINSTRUCTIONS");
}

// A production-policy probe with one direct structured instruction and an
// exact number of mapped reverse instructions on its cheapest structured
// path.  Unique scalar destinations avoid checkpoints, keeping the count
// transparent.  `cold_skip` adds a path that jumps over the whole body.
static IslandProg structured_profitability_case(size_t structured_work,
                                                 bool cold_skip) {
  IslandProg p;
  p.n_regs = 4;
  p.ins = {{0, 4}};  // condition, scalar chain input, 1x1 diagonal and matrix
  p.code.reserve(structured_work + 2);
  size_t opening_jz = std::numeric_limits<size_t>::max();
  if (cold_skip) {
    opening_jz = p.code.size();
    p.code.push_back({Program::JZ, 0, 0});
  }

  const int structured_out = p.n_regs++;
  p.code.push_back(
      {Program::DIAG_PRE_MULTIPLY, structured_out, 2, 3, 1, 1});
  int chain = 1;
  for (size_t k = 1; k < structured_work; ++k) {
    const int next = p.n_regs++;
    p.code.push_back({Program::NEG, next, chain});
    chain = next;
  }

  if (cold_skip) {
    p.code[opening_jz].dst = static_cast<int>(p.code.size());
  } else {
    // Keep this a CFG while making every path execute exactly
    // `structured_work` common reverse instructions.
    const size_t trailing_jz = p.code.size();
    p.code.push_back({Program::JZ, 0, 0});
    const int optional = p.n_regs++;
    p.code.push_back({Program::NEG, optional, chain});
    p.code[trailing_jz].dst = static_cast<int>(p.code.size());
  }
  p.out_regs = {chain};
  return p;
}

static void test_cfg_adjoint() {
  check_cfg("cfg nested first arm", nested_cfg_case(1.0, 1.0));
  check_cfg("cfg nested second arm", nested_cfg_case(1.0, 0.0));
  check_cfg("cfg nested outer skip", nested_cfg_case(0.0, 1.0));

  {
    Case scalar = nested_cfg_case(1.0, 0.0);
    expect("scalar cfg generated for profitability",
           gen_cfg_adjoint(scalar.p));
    expect("scalar cfg remains production-profitable",
           cfg_native_profitable(scalar.p));

    IslandProg bad_reverse = scalar.p;
    bad_reverse.adj.code.front().fwd_pc = -1;
    expect("cfg malformed reverse map fails closed",
           !cfg_native_profitable(bad_reverse));

    IslandProg bad_forward = scalar.p;
    auto mapped = std::find_if(
        bad_forward.trace_pc.begin(), bad_forward.trace_pc.end(),
        [](int32_t pc) { return pc >= 0; });
    expect("cfg test has a mapped forward pc",
           mapped != bad_forward.trace_pc.end());
    if (mapped != bad_forward.trace_pc.end()) *mapped = -1;
    expect("cfg malformed forward map fails closed",
           !cfg_native_profitable(bad_forward));
  }

  {
    IslandProg ctsem_shaped = structured_profitability_case(19754, false);
    expect("ctsem-shaped cfg generated", gen_cfg_adjoint(ctsem_shaped));
    expect("ctsem-shaped minimum structured work is selected",
           cfg_native_profitable(ctsem_shaped));
  }

  {
    IslandProg cold_huge = structured_profitability_case(32769, true);
    expect("cold huge structured cfg generated", gen_cfg_adjoint(cold_huge));
    expect("cold huge structured cfg exceeds trace-scan cap",
           !cfg_native_profitable(cold_huge));
  }

  const auto check_call_path = [](double condition) {
    IslandProg cfg;
    cfg.n_regs = 6;
    cfg.ins = {{0, 1, 0, 0, true}, {1, 1, 1, 0, true}};
    Program::Call call;
    call.opcode = kProgramSoftmax3Opcode;
    call.n_in = 1;
    call.in[0] = 1;
    call.in_len[0] = 1;
    call.out = 2;
    call.out_len = 1;
    call.scratch = 3;
    call.scratch_len = 3;
    expect("CFG CALL binds kernel", bind_call(call));
    cfg.calls = {call};
    cfg.code = {{Program::MOV, 2, 1},
                {Program::JZ, 3, 0},
                {Program::CALL, 2, 0}};
    cfg.out_regs = {2};

    // The registered test CALL computes square.  Replacing it with the
    // corresponding Program opcode gives a genuine var-replay oracle while
    // preserving the same control flow.  x=2 makes both reverse
    // multiplication groupings bit-identical even for a non-unit seed.
    IslandProg oracle = cfg;
    oracle.n_regs = 3;
    oracle.calls.clear();
    oracle.code[2] = {Program::SQUARE, 2, 1};
    const std::vector<double> input{condition, 2.0};
    const std::vector<double> seed{1.3};
    std::vector<double> want_value, got_value;
    const std::vector<double> want =
        replay_adjoints(oracle, input, seed, &want_value);
    expect(condition == 0.0 ? "cfg skipped CALL generated"
                            : "cfg taken CALL generated",
           gen_cfg_adjoint(cfg));
    expect("cfg CALL trace plan partitions", trace_plan_partitions(cfg));
    int32_t block_begin = 0;
    for (const AdjTraceBlock& block : cfg.adj.trace_blocks) {
      for (int32_t pc = block_begin; pc < block.end; ++pc)
        if (cfg.adj.code[static_cast<size_t>(pc)].code == Program::CALL)
          expect("cfg CALL trace block is singleton",
                 block.end == block_begin + 1);
      block_begin = block.end;
    }
    expect(condition == 0.0 ? "cfg skipped canonical CALL fails closed"
                            : "cfg taken canonical CALL fails closed",
           !cfg_native_profitable(cfg));
    call_activity_forwards = 0;
    call_activity_backwards = 0;
    const std::vector<double> got =
        native_adjoints(cfg, input, seed, &got_value);
    expect(condition == 0.0 ? "cfg skipped CALL value parity"
                            : "cfg taken CALL value parity",
           ulps(want_value[0], got_value[0]) == 0);
    expect(condition == 0.0 ? "cfg skipped CALL adjoint parity"
                            : "cfg taken CALL adjoint parity",
           ulps(want[0], got[0]) == 0 && ulps(want[1], got[1]) == 0);
    expect(condition == 0.0 ? "cfg skipped CALL not forwarded"
                            : "cfg taken CALL forwarded",
           call_activity_forwards == (condition == 0.0 ? 0 : 1));
    expect(condition == 0.0 ? "cfg skipped CALL not reversed"
                            : "cfg taken CALL reversed",
           call_activity_backwards == (condition == 0.0 ? 0 : 1));
  };
  test_setenv("STANLI_CFG_ADJ_TRACE_BLOCKS", "1", 1);
  check_call_path(1.0);
  check_call_path(0.0);
  test_unsetenv("STANLI_CFG_ADJ_TRACE_BLOCKS");

  {
    Case c = nested_cfg_case(1.0, 0.0);
    const IslandProg before = c.p;
    expect("legacy generator refuses general cfg", !gen_adjoint(c.p));
    expect("legacy cfg refusal leaves code",
           c.p.code.size() == before.code.size() &&
               c.p.code[1].dst == before.code[1].dst);
  }
  {
    IslandProg backedge;
    backedge.n_regs = 2;
    backedge.ins = {{0, 2}};
    backedge.code = {{Program::ADD, 1, 0, 1}, {Program::JMP, 0}};
    backedge.out_regs = {1};
    const IslandProg before = backedge;
    expect("cfg backedge refused", !gen_cfg_adjoint(backedge));
    expect("cfg refusal transactional",
           backedge.code.size() == before.code.size() &&
               backedge.code.back().dst == before.code.back().dst &&
               backedge.n_regs == before.n_regs);
  }
}

static void test_cfg_structured_calls() {
  for (double condition : {1.0, 0.0}) {
    {
      Build b({condition, 0.7, -0.4, 1.2, -0.2, 0.3, 0.9});
      const int out = b.alloc(4);
      b.p.code = {{Program::MOVR, out, 3, 0, 0, 4},
                  {Program::JZ, 3, 0},
                  {Program::DIAG_PRE_MULTIPLY, out, 1, 3, 2, 2}};
      check_cfg(condition ? "cfg diag_pre taken" : "cfg diag_pre skipped",
                b.done({out, out + 1, out + 2, out + 3},
                       {0.2, -0.4, 0.7, 1.1}),
                false);
    }
    {
      Build b({condition, 0.1, -0.2, 0.3, 0.4});
      const int out = b.alloc(4);
      b.p.code = {{Program::MOVR, out, 1, 0, 0, 4},
                  {Program::JZ, 3, 0},
                  {Program::MATRIX_EXP, out, 1, 2, 2, 4}};
      Case c = b.done({out, out + 1, out + 2, out + 3},
                      {0.2, -0.4, 0.7, 1.1});
      const IslandProg replay = c.p;
      const std::vector<double> want_double = direct_values(replay, c.in);
      expect(condition ? "cfg matrix_exp generated" : "cfg matrix_exp skip generated",
             gen_cfg_adjoint(c.p));
      expect("cfg matrix_exp keeps direct forward and kernel backward",
             std::count_if(c.p.code.begin(), c.p.code.end(),
                           [](const Program::Instr& instruction) {
                             return instruction.code == Program::MATRIX_EXP;
                           }) == 1 &&
                 std::none_of(c.p.code.begin(), c.p.code.end(),
                              [](const Program::Instr& instruction) {
                                return instruction.code == Program::CALL;
                              }) &&
                 c.p.calls.size() == 1 &&
                 c.p.calls[0].opcode == OP_MATRIX_EXP);
      expect("cfg matrix_exp is generated but fails closed",
             !cfg_native_profitable(c.p));
      std::vector<double> want_v, got_v;
      const std::vector<double> want =
          replay_adjoints(replay, c.in, c.seed, &want_v);
      const std::vector<double> got =
          native_adjoints(c.p, c.in, c.seed, &got_v);
      for (size_t k = 0; k < want.size(); ++k)
        expect("cfg matrix_exp adjoint parity", ulps(want[k], got[k]) == 0);
      for (size_t k = 0; k < want_double.size(); ++k)
        expect("cfg matrix_exp direct value parity",
               ulps(want_double[k], got_v[k]) == 0);
    }
    {
      Build b({condition, 2.0, 0.3, 0.3, 1.4, 0.6, -0.2});
      const int out = b.alloc(2);
      b.p.code = {{Program::MOVR, out, 5, 0, 0, 2},
                  {Program::JZ, 3, 0},
                  {Program::MDIVIDE_LEFT, out, 1, 5, -2, 2}};
      Case c = b.done({out, out + 1}, {0.7, -1.1});
      const IslandProg replay = c.p;
      const std::vector<double> want_double = direct_values(replay, c.in);
      expect("cfg mdivide_left generated", gen_cfg_adjoint(c.p));
      expect("cfg mdivide_left attaches active vector backward",
                 c.p.calls.size() == 1 &&
                 c.p.calls[0].opcode == OP_MDIVIDE_LEFT &&
                 c.p.calls[0].variant == 15u);
      std::vector<double> want_v, got_v;
      const std::vector<double> want =
          replay_adjoints(replay, c.in, c.seed, &want_v);
      const std::vector<double> got =
          native_adjoints(c.p, c.in, c.seed, &got_v);
      for (size_t k = 0; k < want.size(); ++k)
        expect("cfg mdivide_left adjoint parity", ulps(want[k], got[k]) == 0);
      for (size_t k = 0; k < want_double.size(); ++k)
        expect("cfg mdivide_left direct value parity",
               ulps(want_double[k], got_v[k]) == 0);
    }
    {
      Build b({condition, 1.5, 0.2, 0.2, 0.9, 0.7, -0.1, 0.4, 0.8});
      const int out = b.alloc(4);
      b.p.code = {{Program::MOVR, out, 5, 0, 0, 4},
                  {Program::JZ, 3, 0},
                  {Program::QUAD_FORM_SYM, out, 1, 5, 2, 4}};
      Case c = b.done({out, out + 1, out + 2, out + 3},
                      {0.2, -0.4, 0.7, 1.1});
      const IslandProg replay = c.p;
      const std::vector<double> want_double = direct_values(replay, c.in);
      expect("cfg quad_form_sym generated", gen_cfg_adjoint(c.p));
      expect("cfg quad_form_sym attaches active matrix backward",
             c.p.calls.size() == 1 &&
                 c.p.calls[0].opcode == OP_QUAD_FORM_SYM &&
                 c.p.calls[0].variant == 2u);
      std::vector<double> want_v, got_v;
      const std::vector<double> want =
          replay_adjoints(replay, c.in, c.seed, &want_v);
      const std::vector<double> got =
          native_adjoints(c.p, c.in, c.seed, &got_v);
      for (size_t k = 0; k < want.size(); ++k)
        expect("cfg quad_form_sym adjoint parity", ulps(want[k], got[k]) == 0);
      for (size_t k = 0; k < want_double.size(); ++k)
        expect("cfg quad_form_sym direct value parity",
               ulps(want_double[k], got_v[k]) == 0);
    }
  }
}

static void test_selector_adjoint_recognition() {
  IslandProg selector;
  selector.n_regs = 5;
  selector.ins = {{0, 1}, {1, 1}, {2, 1}};
  selector.pool = {0.4};
  selector.code = {
      {Program::CONST, 3, 0},
      {Program::GT, 4, 0, 3},
      {Program::JZ, 5, 4},
      {Program::MOV, 1, 2},
      {Program::JMP, 5},
  };
  selector.out_regs = {1};
  expect("pure selector recognized", supports_selector_adjoint(selector));

  IslandProg arithmetic = selector;
  arithmetic.code[3] = {Program::ADD, 1, 1, 2};
  expect("arithmetic selector refused",
         !supports_selector_adjoint(arithmetic));

  IslandProg backedge = selector;
  backedge.code.back().dst = 1;
  expect("selector backedge refused", !supports_selector_adjoint(backedge));

  IslandProg bad_pool = selector;
  bad_pool.pool.clear();
  expect("selector bad constant refused",
         !supports_selector_adjoint(bad_pool));
}

static void test_reductions() {
  {
    Build b({0.3, 0.7, 1.4}, 3);
    const int d = b.emit(Program::SOFTMAX, 0, 0, 0, 3, 3);
    check("softmax3", b.done({d, d + 1, d + 2}, {1.1, -0.4, 2.0}), 0, true);
  }
  {
    Build b({0.3, 0.7, 1.4, 0.2}, 4);
    const int d = b.emit(Program::LSE_RANGE, 0, 0, 0, 4);
    check("lse_range", b.done({d}, {1.7}));
  }
  {
    Build b({0.3, 0.7, 1.4, 0.2}, 4);
    const int d = b.emit(Program::SOFTMAX, 0, 0, 0, 4, 4);
    check("softmax", b.done({d, d + 1, d + 2, d + 3}, {1.1, -0.4, 2.0, 0.7}));
  }
  {
    // DOT over two halves of one live-in range.
    Build b({0.3, 0.7, 1.4, 0.2}, 4);
    const int d = b.emit(Program::DOT, 0, 2, 0, 2);
    check("dot", b.done({d}, {0.85}));
  }
  {
    Build b({0.3, 1.1});
    const int d = b.emit(Program::LSE2, 0, 1);
    check("lse2", b.done({d}, {1.9}));
  }
  {
    Build b({1.1, 0.3});
    const int d = b.emit(Program::LOG_DIFF_EXP, 0, 1);
    check("log_diff_exp", b.done({d}, {1.9}));
  }
  // Long enough that Eigen vectorizes the reductions. A short vector hides
  // any disagreement between the double pass's redux over the register file
  // and whatever the var pass reduces.
  {
    std::vector<double> v;
    for (int k = 0; k < 17; ++k) v.push_back(0.21 * k - 1.3 + 0.03 * (k % 5));
    {
      Build b(v, 17);
      const int d = b.emit(Program::LSE_RANGE, 0, 0, 0, 17);
      check("lse_range long", b.done({d}, {1.7}));
    }
    {
      Build b(v, 17);
      const int d = b.emit(Program::SOFTMAX, 0, 0, 0, 17, 17);
      std::vector<int> outs;
      std::vector<double> seeds;
      for (int k = 0; k < 17; ++k) {
        outs.push_back(d + k);
        seeds.push_back(0.7 - 0.11 * k);
      }
      check("softmax long", b.done(outs, seeds));
    }
    {
      std::vector<double> v16(v.begin(), v.begin() + 16);
      Build b(v16, 16);
      const int d = b.emit(Program::DOT, 0, 8, 0, 8);
      check("dot long", b.done({d}, {0.85}));
    }
  }
  {
    Build b({0.4, 0.3, 1.1});
    const int d = b.emit(Program::LOG_MIX, 0, 1, 2);
    check("log_mix", b.done({d}, {1.9}));
  }
  {
    // log_mix takes the other branch when lambda1 < lambda2.
    Build b({0.4, 1.1, 0.3});
    const int d = b.emit(Program::LOG_MIX, 0, 1, 2);
    check("log_mix swapped", b.done({d}, {1.9}));
  }
}

// An instruction whose output range IS its input range. gen_adjoint admits
// this deliberately -- `x = exp(x)` is ordinary and the elementwise rules
// read, clear and accumulate one cell at a time, which is safe when the
// ranges coincide. The reductions have to hold to that too: softmax reads
// every output adjoint to form its contraction, so clearing them in a
// second pass would wipe what the first pass just accumulated.
static void test_in_place_ranges() {
  {
    Build b({0.3, 0.7, 1.4}, 3);
    b.emit_to(Program::SOFTMAX, 0, 0, 0, 0, 3);
    check("in-place softmax3", b.done({0, 1, 2}, {1.1, -0.4, 2.0}), 0, true);
  }
  const std::vector<double> v{0.3, 0.7, 1.4, 0.2};
  const Program::Code codes[] = {Program::SOFTMAX, Program::EXP_RANGE,
                                 Program::LOG_RANGE, Program::MOVR};
  const char* names[] = {"softmax", "exp_range", "log_range", "movr"};
  for (size_t k = 0; k < 4; ++k) {
    Build b(v, 4);
    b.emit_to(codes[k], 0, 0, 0, 0, 4);  // dst == a: in place
    check(std::string("in-place ") + names[k],
          b.done({0, 1, 2, 3}, {1.1, -0.4, 2.0, 0.7}));
  }
}

// stan-math's fmax/fmin return one of their operands rather than building a
// node, and both go to NaN's own branch: `a > b` is false for any NaN, so
// fmax(x, NaN) returns x and its derivative belongs to x. A local declared
// and not assigned is NaN (mir_prog.hpp), so this is reachable.
static void test_nan_operands() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  {
    Build b({0.7, nan});
    const int d = b.emit(Program::FMAX, 0, 1);
    check("fmax nan second", b.done({d}, {2.5}));
  }
  {
    Build b({0.7, nan});
    const int d = b.emit(Program::FMIN, 0, 1);
    check("fmin nan second", b.done({d}, {2.5}));
  }
  {
    Build b({nan, 0.7});
    const int d = b.emit(Program::FMAX, 0, 1);
    check("fmax nan first", b.done({d}, {2.5}));
  }
  // fabs poisons its operand's adjoint at NaN rather than leaving it alone,
  // which is what makes a sampler reject the draw instead of accepting a
  // finite gradient computed from nothing.
  {
    Build b({nan});
    const int d = b.emit(Program::FABS, 0);
    check("fabs nan", b.done({d}, {1.3}));
  }
}

// ---- scalar probability functions -------------------------------------

static void test_densities() {
  // EVERY scalar density/CDF the register machine speaks, discovered from
  // the shared table rather than listed here, so one added to the runtime is
  // covered the day it arrives instead of the day someone remembers this
  // file.
  //
  // Each has its own support, so rather than curate a point per function the
  // loop tries a few tuples and keeps the first one it accepts (a finite
  // value). A function that accepts none of them is a failure,
  // not a skip -- silently testing nothing is the thing to avoid.
  static const double kPoints[][kMaxDensityArgs] = {
      {0.63, 0.4, 1.7, 0.5}, {0.63, 1.4, 2.2, 0.25}, {2.5, 3.0, 1.0, 0.75},
      {0.35, 2.0, 0.8, 0.5}, {1.25, 0.7, 1.3, 0.9},  {0.5, 4.0, 0.25, 0.5},
      {0.63, 1.4, 2.2, 1.1}, {2.5, 3.0, 1.0, 2.0},
  };
  const int n_points = (int)(sizeof(kPoints) / sizeof(kPoints[0]));
  for (int id = 0; id < program_density_count(); ++id) {
    const int arity = program_density_arity(id);
    const std::string name = program_density_name(id);
    bool tested = false;
    for (int pt = 0; pt < n_points && !tested; ++pt) {
      std::vector<double> args(kPoints[pt], kPoints[pt] + arity);
      // stan-math signals an argument outside a density's domain by
      // throwing, not by returning a nonfinite value, so both count as
      // "try the next point".
      bool usable = true;
      try {
        double probe[kMaxDensityArgs] = {0, 0, 0, 0};
        const double v = program_density<double>(id, args.data());
        program_density_partials(id, 0xf, args.data(), probe);
        usable = std::isfinite(v);
        for (int k = 0; k < arity; ++k)
          if (!std::isfinite(probe[k])) usable = false;
      } catch (const std::exception&) {
        usable = false;
      }
      if (!usable) continue;
      Build b(args);
      std::vector<int> argv;
      for (int k = 0; k < arity; ++k) argv.push_back(k);
      const int d = b.emit_density(id, argv);
      check("density " + name, b.done({d}, {1.25}));
      tested = true;
    }
    if (!tested) {
      ++failures;
      std::printf("FAIL density %s: no probe point inside its support\n",
                  name.c_str());
    }
  }
}

// Dropping a data argument's partial must not move the partials that are
// kept. Bitwise, every density, every mask: an argument bound as a double
// takes a different stan-math instantiation, and a reassociated intermediate
// there would show up as a last-bit gradient difference in a model nobody
// would think to attribute to activity.
static void test_density_masked_partials() {
  static const double kPoints[][kMaxDensityArgs] = {
      {0.63, 0.4, 1.7, 0.5}, {0.63, 1.4, 2.2, 0.25}, {2.5, 3.0, 1.0, 0.75},
      {0.35, 2.0, 0.8, 0.5}, {1.25, 0.7, 1.3, 0.9},  {0.5, 4.0, 0.25, 0.5},
  };
  const int n_points = (int)(sizeof(kPoints) / sizeof(kPoints[0]));
  for (int id = 0; id < program_density_count(); ++id) {
    const int arity = program_density_arity(id);
    const std::string name = program_density_name(id);
    const unsigned all = (1u << arity) - 1u;
    bool tested = false;
    for (int pt = 0; pt < n_points && !tested; ++pt) {
      const double* args = kPoints[pt];
      double full[kMaxDensityArgs] = {0, 0, 0, 0};
      try {
        if (!std::isfinite(program_density<double>(id, args))) continue;
        if (!program_density_partials(id, all, args, full)) continue;
      } catch (const std::exception&) {
        continue;
      }
      bool finite = true;
      for (int k = 0; k < arity; ++k)
        if (!std::isfinite(full[k])) finite = false;
      if (!finite) continue;
      tested = true;
      for (unsigned mask = 1; mask <= all; ++mask) {
        double part[kMaxDensityArgs] = {-1, -1, -1, -1};
        const bool built = program_density_partials(id, mask, args, part);
        expect((name + " mask connected").c_str(), built);
        for (int k = 0; k < arity; ++k) {
          const bool want = (mask >> k) & 1u;
          const std::string what = name + " mask " + std::to_string(mask) +
                                   " arg " + std::to_string(k);
          if (want && part[k] != full[k]) {
            ++failures;
            std::printf("FAIL %s: got %.17g want %.17g\n", what.c_str(),
                        part[k], full[k]);
          }
          if (!want && part[k] != -1.0) {
            ++failures;
            std::printf("FAIL %s: masked-off partial written\n", what.c_str());
          }
        }
      }
    }
    if (!tested) {
      ++failures;
      std::printf("FAIL density %s: no masked probe point\n", name.c_str());
    }
  }
}

// Stan Math can return a constant support value before constructing its
// partials propagator. The register-program API promises to fill every
// partial, so it must write zeros rather than leave the caller's old values.
static void test_density_early_return_partials() {
  const int id = program_density_id_by_name("inv_gamma_lpdf");
  expect("inv_gamma density id", id >= 0);
  const double args[3] = {-1.0, 2.0, 3.0};
  expect("inv_gamma early value", program_density<double>(id, args) ==
                                      -std::numeric_limits<double>::infinity());
  double partials[3] = {4.0, 5.0, 6.0};
  expect("inv_gamma early disconnected",
         !program_density_partials(id, 0xf, args, partials));
  for (double partial : partials)
    expect("inv_gamma early partial", partial == 0.0);

  // The var replay has no edge from an early-return literal to its inputs.
  // An infinite upstream adjoint must therefore be skipped, not multiplied
  // by a stored zero partial.
  Build b({-1.0, 2.0, 3.0});
  const int density = b.emit_density(id, {0, 1, 2});
  const int squared = b.emit(Program::SQUARE, density);
  check("inv_gamma disconnected", b.done({squared}, {1.0}));

  // A built zero partial stays connected. Native reverse must retain the
  // same indeterminate inf * 0 adjoint as the var replay.
  Build connected({1.0, 2.0, 3.0});
  const int connected_density = connected.emit_density(id, {0, 1, 2});
  const int infinity = connected.konst(std::numeric_limits<double>::infinity());
  const int scaled = connected.emit(Program::MUL, connected_density, infinity);
  check("inv_gamma connected zero partial", connected.done({scaled}, {1.0}));

  // Restoring the thread-local sink on exceptions is part of making the
  // shared recorder compositional: one rejected evaluation must not leave a
  // dangling pointer for the next density call.
  const double bad_domain[3] = {1.0, -2.0, 3.0};
  bool threw = false;
  try {
    program_density_partials(id, 0xf, bad_domain, partials);
  } catch (const std::domain_error&) {
    threw = true;
  }
  expect("inv_gamma invalid alpha throws", threw);
  expect("density exception restores sink", active_sink() == nullptr);
}

// ---- a composite region ------------------------------------------------

// The shape the carver actually sees: an unrolled recurrence, each step
// reading the previous state, adding a density term, and writing the state
// back in place.
static void test_recurrence() {
  const int kNormal = program_density_id_by_name("normal_lpdf");
  Build b({0.35, -0.2, 0.9});  // state, mu, sigma
  int st = 0;
  for (int t = 0; t < 6; ++t) {
    const int y = b.konst(0.3 * t - 0.5);
    const int e = b.emit_density(kNormal, {y, 1, 2});
    const int s = b.emit(Program::ADD, st, e);
    const int u = b.emit(Program::TANH, s);
    b.emit_to(Program::MOV, 0, u);  // state overwritten in place
    st = 0;
  }
  check("recurrence", b.done({st}, {1.0}));
}

// Two gradients at the same point must agree: the island work hit exactly
// this failure (live-in and live-out registers double-counting, off by
// exactly 1.0), and it only shows up on the second call.
static void test_two_gradients() {
  Build bb({0.35, -0.2, 0.9});
  const int m = bb.emit(Program::MUL, 0, 1);
  const int e = bb.emit(Program::EXP, m);
  bb.emit_to(Program::ADD, 0, e, 2);
  Case c = bb.done({0}, {1.0});
  expect("two-grad gen", gen_adjoint(c.p));
  std::vector<double> v1, v2;
  const std::vector<double> a1 = native_adjoints(c.p, c.in, c.seed, &v1);
  const std::vector<double> a2 = native_adjoints(c.p, c.in, c.seed, &v2);
  for (size_t k = 0; k < a1.size(); ++k)
    expect("two gradients agree", a1[k] == a2[k]);
}

// ---- fuzz ---------------------------------------------------------------

// Random programs over the opcodes the carver emits most, checked against
// the replay. The hand-written cases above each isolate one rule; this is
// what covers their INTERACTIONS -- a register written twice, then read as
// part of a range, then aliased onto by a copy. `Mb_model` disagreed at
// 1e-14 on nothing more exotic than CONST/MOV/MOVR/ADD/MUL, which is a
// combination no single-opcode case was ever going to produce.
struct Rng {
  uint64_t s;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  int pick(int n) { return (int)(next() % (uint64_t)n); }
  double real() { return 0.25 + (double)(next() % 1000) / 400.0; }
};

static void test_fuzz() {
  for (int trial = 0; trial < 400; ++trial) {
    Rng rng{(uint64_t)(trial * 2654435761u + 12345)};
    const int n_in = 1 + rng.pick(3);
    std::vector<double> ins;
    for (int k = 0; k < n_in; ++k) ins.push_back(rng.real());
    Build b(ins);
    // Registers known to hold a value: reading anything else would be
    // reading an uninitialized register, which no front end emits.
    std::vector<int> live;
    for (int k = 0; k < n_in; ++k) live.push_back(k);
    const int n_instr = 4 + rng.pick(20);
    for (int t = 0; t < n_instr; ++t) {
      const int form = rng.pick(6);
      const int a = live[(size_t)rng.pick((int)live.size())];
      const int c = live[(size_t)rng.pick((int)live.size())];
      // Half the writes go to a fresh register, half overwrite a live one,
      // which is what exercises the checkpoints and the copy aliasing.
      const bool fresh = rng.pick(2) == 0;
      int d;
      if (fresh) {
        d = b.alloc();
      } else {
        d = live[(size_t)rng.pick((int)live.size())];
      }
      switch (form) {
        case 0: {
          const double v = rng.real();
          b.p.code.push_back(
              Program::Instr{Program::CONST, d, (int)b.p.pool.size(), 0, 0, 1});
          b.p.pool.push_back(v);
          break;
        }
        case 1:
          b.emit_to(Program::MOV, d, a);
          break;
        case 2:
          b.emit_to(Program::ADD, d, a, c);
          break;
        case 3:
          b.emit_to(Program::SUB, d, a, c);
          break;
        case 4:
          b.emit_to(Program::MUL, d, a, c);
          break;
        default:
          b.emit_to(Program::TANH, d, a);
          break;
      }
      if (std::find(live.begin(), live.end(), d) == live.end())
        live.push_back(d);
    }
    const int n_out = 1 + rng.pick(3);
    std::vector<int> outs;
    std::vector<double> seeds;
    for (int k = 0; k < n_out; ++k) {
      outs.push_back(live[(size_t)rng.pick((int)live.size())]);
      seeds.push_back(0.3 + 0.4 * k);
    }
    Case c = b.done(outs, seeds);
    const IslandProg before = c.p;
    // Two ulp: enough for the copy-sharing reassociation above, far too
    // little to hide a wrong rule (those miss by a factor, not a bit).
    if (!check("fuzz " + std::to_string(trial), std::move(c), 2)) dump(before);
  }
}

// The same idea over RANGES, which is where the bugs actually were: an
// in-place softmax cleared the adjoints it had just accumulated, and no
// scalar program can express that. Ranges, reductions, in-place writes and
// single-element pokes into a live range, against the replay.
static void test_fuzz_ranges() {
  for (int trial = 0; trial < 300; ++trial) {
    Rng rng{(uint64_t)(trial * 40503u + 7919)};
    const int W = 2 + rng.pick(4);
    std::vector<double> ins;
    for (int k = 0; k < W; ++k) ins.push_back(rng.real());
    Build b(ins, W);
    std::vector<int> ranges{0};  // starts of W-wide live ranges
    std::vector<int> scalars;    // registers holding one value
    const int n_instr = 3 + rng.pick(8);
    for (int t = 0; t < n_instr; ++t) {
      const int src = ranges[(size_t)rng.pick((int)ranges.size())];
      // Half the range writes go somewhere fresh, half in place.
      const bool fresh = rng.pick(2) == 0;
      const int dst = fresh ? b.alloc(W) : src;
      switch (rng.pick(6)) {
        case 0:
          b.emit_to(Program::MOVR, dst, src, 0, 0, W);
          break;
        case 1:
          b.emit_to(Program::EXP_RANGE, dst, src, 0, 0, W);
          break;
        case 2:
          b.emit_to(Program::SOFTMAX, dst, src, 0, 0, W);
          break;
        case 3: {
          // A reduction: its output is a scalar, not a range.
          const int r = b.alloc();
          b.emit_to(rng.pick(2) ? Program::LSE_RANGE : Program::DOT, r, src,
                    ranges[(size_t)rng.pick((int)ranges.size())], 0, W);
          scalars.push_back(r);
          continue;
        }
        case 4: {
          // Poke one element of a live range, the SET_INDEX shape.
          if (scalars.empty()) continue;
          const int e = scalars[(size_t)rng.pick((int)scalars.size())];
          b.emit_to(Program::MOV, src + rng.pick(W), e);
          continue;
        }
        default: {
          // Read one element out and transform it.
          const int r = b.alloc();
          b.emit_to(Program::TANH, r, src + rng.pick(W));
          scalars.push_back(r);
          continue;
        }
      }
      if (std::find(ranges.begin(), ranges.end(), dst) == ranges.end())
        ranges.push_back(dst);
    }
    std::vector<int> outs;
    std::vector<double> seeds;
    const int base = ranges[(size_t)rng.pick((int)ranges.size())];
    for (int k = 0; k < W; ++k) {
      outs.push_back(base + k);
      seeds.push_back(0.9 - 0.23 * k);
    }
    for (int r : scalars)
      if (rng.pick(3) == 0) {
        outs.push_back(r);
        seeds.push_back(0.31);
      }
    Case c = b.done(outs, seeds);
    const IslandProg before = c.p;
    if (!check("fuzz range " + std::to_string(trial), std::move(c), 16))
      dump(before);
  }
}

int main() {
  test_call_binding_refusal();
  test_call_cached_forward_reverse_aliasing();
  test_nested_backward_context_reentrant();
  test_binary_ops();
  test_fma();
  test_unary_ops();
  test_overwrite_needs_checkpoint();
  test_self_write();
  test_live_copy_both_read();
  test_copy_then_modify_chain();
  test_compact_adjoint_ranges();
  test_forwarded_repeated_destination();
  test_forwarded_saveout_last_write();
  test_saveout_later_write_refuses_forwarding();
  test_forwarded_ranged_producers();
  test_ranged_saveout_partial_later_write_refuses();
  test_accumulate_into_one_register();
  test_ranged();
  test_softmax3_activation();
  test_softmax3_double_exact();
  test_in_place_ranges();
  test_nan_operands();
  test_reductions();
  test_densities();
  test_density_masked_partials();
  test_density_early_return_partials();
  test_recurrence();
  test_two_gradients();
  test_fuzz();
  test_fuzz_ranges();
  test_call_activity_elision();
  test_terminal_jz_adjoint();
  test_cfg_trace_blocks();
  test_cfg_adjoint();
  test_cfg_structured_calls();
  test_selector_adjoint_recognition();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_adjoint: all passed\n");
  return 0;
}
