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

// The subject: forward on doubles, then the generated adjoint program.
static std::vector<double> native_adjoints(const IslandProg& p,
                                           const std::vector<double>& in,
                                           const std::vector<double>& seed,
                                           std::vector<double>* out_vals,
                                           const Program* optimized = nullptr) {
  std::vector<double> val((size_t)p.n_regs, 0.0);
  int64_t off = 0;
  for (const auto& li : p.ins)
    for (int i = 0; i < li.len; ++i)
      val[(size_t)(li.reg + i)] = in[(size_t)off++];
  run_program(optimized ? *optimized : static_cast<const Program&>(p),
              val.data());
  for (size_t m = 0; m < p.out_regs.size(); ++m)
    out_vals->push_back(val[(size_t)p.out_regs[m]]);

  std::vector<double> adj((size_t)p.adj.n_regs, 0.0);
  const auto& map = p.adj.adj_reg;
  for (size_t m = p.out_regs.size(); m-- > 0;)
    adj[(size_t)map[(size_t)p.out_regs[m]]] += seed[m];
  run_adjoint(p, p.adj, val.data(), adj.data());

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

static void test_call_primal_read_contract() {
  const BackwardPrimalReads add_reads =
      backward_primal_reads(find_kernel(OP_ADD), 0);
  expect("ADD backward reads no primals",
         !add_reads.input(0) && !add_reads.input(1) && !add_reads.output());
  const BackwardPrimalReads unknown = backward_primal_reads(nullptr, 255);
  expect("unknown backward retains every primal",
         unknown.input(0) && unknown.input(5) && unknown.output(0) &&
             unknown.output(1));

  // Both an input and the output are overwritten after the call.  ADD's
  // registered backward only routes adjoints, so gen_adjoint need not insert
  // value checkpoints for either range.
  IslandProg p;
  p.n_regs = 5;
  p.ins = {IslandProg::LiveIn{0, 1}, IslandProg::LiveIn{1, 1},
           IslandProg::LiveIn{4, 1}};
  p.out_regs = {3};
  Program::Call add;
  add.opcode = OP_ADD;
  add.n_in = 2;
  add.in[0] = 0;
  add.in[1] = 1;
  add.in_len[0] = add.in_len[1] = 1;
  add.out = 2;
  add.out_len = 1;
  expect("value-free CALL binds", bind_call(add));
  p.calls.push_back(add);
  p.code = {{Program::CALL, 0, 0},
            {Program::MOV, 3, 2},
            {Program::MOV, 0, 4},
            {Program::MOV, 2, 4}};
  expect("value-free CALL adjoint generated", gen_adjoint(p));
  if (!p.calls.empty()) {
    expect("value-free CALL keeps original input binding",
           p.calls[0].bwd_value_in[0] == 0);
    expect("value-free CALL keeps original output binding",
           p.calls[0].bwd_value_out == 2);
  }

  // A private backward carrying the same opcode is not covered by the
  // registered kernel's metadata and must retain the conservative saves.
  IslandProg private_call;
  private_call.n_regs = 5;
  private_call.ins = p.ins;
  private_call.out_regs = {3};
  add.backward = test_call_backward;
  private_call.calls.push_back(add);
  private_call.code = {{Program::CALL, 0, 0},
                       {Program::MOV, 3, 2},
                       {Program::MOV, 0, 4},
                       {Program::MOV, 2, 4}};
  expect("private CALL adjoint generated", gen_adjoint(private_call));
  if (!private_call.calls.empty()) {
    expect("private CALL checkpoints overwritten input",
           private_call.calls[0].bwd_value_in[0] != 0);
    expect("private CALL checkpoints overwritten output",
           private_call.calls[0].bwd_value_out != 2);
  }

  // Registry identity alone is insufficient: a replacement implementation
  // has no contract unless it explicitly registers one alongside itself.
  const Kernel saved_add = *find_kernel(OP_ADD);
  register_kernel(OP_ADD,
                  Kernel{saved_add.forward, test_call_backward, nullptr});
  IslandProg replaced;
  replaced.n_regs = 5;
  replaced.ins = p.ins;
  replaced.out_regs = {3};
  Program::Call rebound = add;
  expect("replacement CALL binds", bind_call(rebound));
  replaced.calls.push_back(rebound);
  replaced.code = {{Program::CALL, 0, 0},
                   {Program::MOV, 3, 2},
                   {Program::MOV, 0, 4},
                   {Program::MOV, 2, 4}};
  expect("replacement CALL adjoint generated", gen_adjoint(replaced));
  if (!replaced.calls.empty()) {
    expect("replacement CALL defaults to input checkpoint",
           replaced.calls[0].bwd_value_in[0] != 0);
    expect("replacement CALL defaults to output checkpoint",
           replaced.calls[0].bwd_value_out != 2);
  }
  register_kernel(OP_ADD, saved_add);
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
  int emit_wide(Program::Code c, int a, int b, int cc, int law, int width,
                uint8_t bcast) {
    const int d = alloc(width);
    emit_wide_to(c, d, a, b, cc, law, width, bcast);
    return d;
  }
  void emit_wide_to(Program::Code c, int d, int a, int b, int cc, int law,
                    int width, uint8_t bcast) {
    Program::Instr I(Program::RANGE, d, a, b, cc, width);
    I.sub = static_cast<uint8_t>(c);
    I.bcast = bcast;
    I.law = static_cast<uint8_t>(law);
    p.code.push_back(I);
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

// Ranged elementwise instructions: every code, every broadcast pattern, in
// place, and with the operands and the output overwritten afterwards.
static void test_elementwise_width() {
  struct Spec {
    Program::Code code;
    int n_in;
  };
  const Spec specs[] = {{Program::ADD, 2},          {Program::SUB, 2},
                        {Program::MUL, 2},          {Program::DIV, 2},
                        {Program::POW, 2},          {Program::FMAX, 2},
                        {Program::FMIN, 2},         {Program::LSE2, 2},
                        {Program::LOG_DIFF_EXP, 2}, {Program::FMA, 3},
                        {Program::LOG_MIX, 3},      {Program::NEG, 1},
                        {Program::EXP, 1},          {Program::LOG, 1},
                        {Program::SQRT, 1},         {Program::SQUARE, 1},
                        {Program::INV, 1},          {Program::FABS, 1},
                        {Program::INV_LOGIT, 1},    {Program::LOG1M, 1},
                        {Program::LOG1P_EXP, 1},    {Program::TANH, 1}};
  // Three length-4 operand ranges at 0, 4 and 8. Every a exceeds every b,
  // so log_diff_exp stays finite whichever operand broadcasts.
  const std::vector<double> in = {0.72, 0.85, 0.9, 0.93, 0.31, 0.4,
                                  0.55, 0.65, 0.6, 0.35, 0.8,  0.45};
  const std::vector<double> seed = {1.1, 0.4, 2.0, 0.7};
  for (const Spec& s : specs) {
    const std::string name = program_code_spec(s.code).name;
    for (int bcast = 0; bcast < (1 << s.n_in); ++bcast) {
      Build b(in, 12);
      const int d = b.emit_wide(s.code, 0, 4, 8, 0, 4, (uint8_t)bcast);
      check(name + " width bcast=" + std::to_string(bcast),
            b.done({d, d + 1, d + 2, d + 3}, seed));
    }
    {
      Build b(in, 12);
      b.emit_wide_to(s.code, 0, 0, 4, 8, 0, 4, 0);
      check(name + " width in place", b.done({0, 1, 2, 3}, seed));
    }
    if (s.n_in >= 2) {
      Build b(in, 12);
      b.emit_wide_to(s.code, 4, 0, 4, 8, 0, 4, 0);
      check(name + " width in place b", b.done({4, 5, 6, 7}, seed));
    }
    if (s.n_in >= 3) {
      Build b(in, 12);
      b.emit_wide_to(s.code, 8, 0, 4, 8, 0, 4, 0);
      check(name + " width in place c", b.done({8, 9, 10, 11}, seed));
    }
    for (int bcast = 0; bcast < (1 << s.n_in); ++bcast) {
      Build b(in, 12);
      const int d = b.emit_wide(s.code, 0, 4, 8, 0, 4, (uint8_t)bcast);
      const int copy = b.emit(Program::MOVR, d, 0, 0, 4, 4);
      b.emit_to(Program::MOVR, 0, 8, 0, 0, 4);
      b.emit_to(Program::MOVR, 4, 8, 0, 0, 4);
      b.emit_to(Program::MOVR, d, 8, 0, 0, 4);
      check(name + " width overwritten bcast=" + std::to_string(bcast),
            b.done({copy, copy + 1, copy + 2, copy + 3}, seed));
    }
  }
}

// compact_program renumbers `dst` only when the instruction writes something,
// so a zero-length range copy keeps a destination from the old numbering.
static void test_zero_length_output() {
  IslandProg p;
  p.n_regs = 3;
  p.ins = {IslandProg::LiveIn{0, 1}, IslandProg::LiveIn{1, 1}};
  p.out_regs = {2};
  p.code.push_back(Program::Instr{Program::MOVR, 5000, 0, 0, 0, 0});
  p.code.push_back(Program::Instr{Program::MUL, 2, 0, 1, 0, 0});

  expect("zero-length output adjoint generated", gen_adjoint(p));
  bool in_range = true;
  for (const AdjInstr& A : p.adj.code)
    if (A.dst < 0 || A.dst >= p.adj.n_regs) in_range = false;
  expect("zero-length output keeps adjoint registers in range", in_range);

  std::vector<double> out_vals;
  const std::vector<double> got =
      native_adjoints(p, {1.25, 3.5}, {1.0}, &out_vals);
  expect("zero-length output gradient",
         got.size() == 2 && got[0] == 3.5 && got[1] == 1.25);
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

// ---- fmax/fmin instantiations ------------------------------------------
// stan-math's three overloads disagree at ties and around NaN: fmax(var,
// var) hands a tie to b, fmax(var, double) hands it to the var, and
// fmax(double, var) hands it to b -- and when the constant side of a mixed
// call wins, the result is a fresh constant and no adjoint flows at all.
// The instruction's len byte carries operand activity (bit 0: a, bit 1: b;
// 0 is the legacy all-var form), set at lowering from the arguments'
// data-only classification, so the forward, the var replay, and the
// generated backward all run the instantiation CmdStan's generated code
// selects. The single-active signature gate found the old unconditional
// ties-to-b rule as a halved fmax(vector, vector) gradient.
static void check_extremum(const std::string& tag, Program::Code code, int law,
                           double x, double y, double want_da, double want_db) {
  const double s = 2.0;
  {
    Build b({x, y});
    const int d = b.emit(code, 0, 1, 0, law);
    if (!check(tag, b.done({d}, {s}))) return;
  }
  Build b({x, y});
  const int d = b.emit(code, 0, 1, 0, law);
  Case c = b.done({d}, {s});
  if (!gen_adjoint(c.p)) {
    ++failures;
    std::printf("FAIL %s: gen_adjoint refused the program\n", tag.c_str());
    return;
  }
  std::vector<double> values;
  const std::vector<double> got = native_adjoints(c.p, c.in, c.seed, &values);
  const double want[2] = {want_da, want_db};
  for (int k = 0; k < 2; ++k) {
    const bool same = got[(size_t)k] == want[k] ||
                      (std::isnan(got[(size_t)k]) && std::isnan(want[k]));
    if (!same) {
      ++failures;
      std::printf("FAIL %s: adj[%d] got %.17g want %.17g\n", tag.c_str(), k,
                  got[(size_t)k], want[k]);
    }
  }
}

static void test_extremum_instantiations() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double s = 2.0;
  // Ties: b unless only a is active.
  check_extremum("fmax tie var,var", Program::FMAX, 0x3, 0.95, 0.95, 0, s);
  check_extremum("fmax tie legacy", Program::FMAX, 0, 0.95, 0.95, 0, s);
  check_extremum("fmax tie var,data", Program::FMAX, 0x1, 0.95, 0.95, s, 0);
  check_extremum("fmax tie data,var", Program::FMAX, 0x2, 0.95, 0.95, 0, s);
  check_extremum("fmin tie var,var", Program::FMIN, 0x3, 0.95, 0.95, 0, s);
  check_extremum("fmin tie var,data", Program::FMIN, 0x1, 0.95, 0.95, s, 0);
  check_extremum("fmin tie data,var", Program::FMIN, 0x2, 0.95, 0.95, 0, s);
  // A winning constant side carries no adjoint in the mixed overloads.
  check_extremum("fmax const a wins", Program::FMAX, 0x2, 1.0, 0.5, 0, 0);
  check_extremum("fmax const b wins", Program::FMAX, 0x1, 0.5, 1.0, 0, 0);
  check_extremum("fmin const b wins", Program::FMIN, 0x1, 1.0, 0.5, 0, 0);
  check_extremum("fmin const a wins", Program::FMIN, 0x2, 0.5, 1.0, 0, 0);
  check_extremum("fmax var a wins", Program::FMAX, 0x1, 1.0, 0.5, s, 0);
  check_extremum("fmin var b wins", Program::FMIN, 0x2, 1.0, 0.5, 0, s);
  // NaN: only active operands are poisoned, and a NaN data side returns
  // the var while a NaN var side returns a fresh constant.
  check_extremum("fmax nan both var,var", Program::FMAX, 0x3, nan, nan, nan,
                 nan);
  check_extremum("fmax nan both var,data", Program::FMAX, 0x1, nan, nan, nan,
                 0);
  check_extremum("fmax nan both data,var", Program::FMAX, 0x2, nan, nan, 0,
                 nan);
  check_extremum("fmax nan a var,data", Program::FMAX, 0x1, nan, 0.5, 0, 0);
  check_extremum("fmax nan a data,var", Program::FMAX, 0x2, nan, 0.5, 0, s);
  check_extremum("fmax nan b var,data", Program::FMAX, 0x1, 0.5, nan, s, 0);
  check_extremum("fmax nan b data,var", Program::FMAX, 0x2, 0.5, nan, 0, 0);
  check_extremum("fmin nan a var,data", Program::FMIN, 0x1, nan, 0.5, 0, 0);
  check_extremum("fmin nan b data,var", Program::FMIN, 0x2, 0.5, nan, 0, 0);
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
  test_call_primal_read_contract();
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
  test_elementwise_width();
  test_zero_length_output();
  test_softmax3_activation();
  test_softmax3_double_exact();
  test_in_place_ranges();
  test_nan_operands();
  test_extremum_instantiations();
  test_reductions();
  test_densities();
  test_density_masked_partials();
  test_density_early_return_partials();
  test_recurrence();
  test_two_gradients();
  test_fuzz();
  test_fuzz_ranges();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_adjoint: all passed\n");
  return 0;
}
