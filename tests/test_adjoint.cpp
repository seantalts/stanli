// The generated adjoint program against the var replay it replaces.
//
// The replay is the oracle: it is the same var arithmetic CmdStan's
// generated code runs, so the bar is BITWISE, not close. Every case here
// builds a small register program, differentiates it both ways at the same
// point with the same seed adjoints, and requires the live-ins' adjoints to
// agree to the last bit.
#include <stanli/adjoint.hpp>
#include <stanli/island.hpp>
#include <stanli/program.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace stanli;

static int failures = 0;

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
  std::vector<double> in;   // one value per live-in register, packed
  std::vector<double> seed; // one adjoint per out_reg
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
                                           std::vector<double>* out_vals) {
  std::vector<double> val((size_t)p.n_regs, 0.0);
  int64_t off = 0;
  for (const auto& li : p.ins)
    for (int i = 0; i < li.len; ++i) val[(size_t)(li.reg + i)] = in[(size_t)off++];
  run_program(p, val.data());
  for (size_t m = 0; m < p.out_regs.size(); ++m)
    out_vals->push_back(val[(size_t)p.out_regs[m]]);

  std::vector<double> adj((size_t)p.n_regs, 0.0);
  for (size_t m = 0; m < p.out_regs.size(); ++m)
    adj[(size_t)p.out_regs[m]] += seed[m];
  run_adjoint(p.adj, val.data(), adj.data());

  std::vector<double> got(in.size());
  off = 0;
  for (const auto& li : p.ins)
    for (int i = 0; i < li.len; ++i) got[(size_t)off++] = adj[(size_t)(li.reg + i)];
  return got;
}

// One case, both ways, bitwise.
static void check(const std::string& name, Case c) {
  const IslandProg orig = c.p;  // before checkpoints are inserted
  const bool ok = gen_adjoint(c.p);
  if (!ok) {
    ++failures;
    std::printf("FAIL %s: gen_adjoint refused the program\n", name.c_str());
    return;
  }
  std::vector<double> want_v, got_v;
  const std::vector<double> want = replay_adjoints(orig, c.in, c.seed, &want_v);
  const std::vector<double> got = native_adjoints(c.p, c.in, c.seed, &got_v);
  for (size_t m = 0; m < want_v.size(); ++m)
    if (!(want_v[m] == got_v[m])) {
      ++failures;
      std::printf("FAIL %s: value %zu replay %.17g native %.17g\n",
                  name.c_str(), m, want_v[m], got_v[m]);
    }
  for (size_t k = 0; k < want.size(); ++k) {
    // Bitwise: an adjoint that merely rounds to the same place is a
    // regression waiting to happen, and the replay is reachable at any
    // time through STANLI_NO_NATIVE_ADJ to prove it.
    if (!(want[k] == got[k])) {
      ++failures;
      std::printf("FAIL %s: adj[%zu] replay %.17g native %.17g (rel %.2e)\n",
                  name.c_str(), k, want[k], got[k],
                  std::abs(got[k] - want[k]) /
                      std::max(std::abs(want[k]), 1e-300));
    }
  }
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
    p.code.push_back(Program::Instr{Program::CONST, r, (int)p.pool.size(), 0, 0, 1});
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

static void test_unary_ops() {
  const Program::Code codes[] = {
      Program::NEG,  Program::EXP,       Program::LOG,   Program::SQRT,
      Program::SQUARE, Program::INV_LOGIT, Program::LOG1M, Program::TANH,
      Program::INV,  Program::FABS};
  const char* names[] = {"neg",       "exp",   "log",   "sqrt", "square",
                         "inv_logit", "log1m", "tanh",  "inv",  "fabs"};
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
  const int t = b.emit(Program::MUL, 0, 1);       // t = a*b
  b.emit_to(Program::MUL, 1, t, 0);               // b = t*a  (overwrites b!)
  const int u = b.emit(Program::MUL, t, 1);       // u = t*b'
  check("overwrite checkpoint", b.done({u}, {0.9}));
}

// dst aliases an operand: the value the adjoint needs is destroyed by the
// very instruction that needs it.
static void test_self_write() {
  Build b({1.3, 0.8});
  b.emit_to(Program::MUL, 0, 0, 1);   // a = a*b, needs the OLD a
  const int u = b.emit(Program::EXP, 0);
  check("self write", b.done({u}, {0.7}));
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

static void test_reductions() {
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

// ---- densities ---------------------------------------------------------

static void test_densities() {
  // One point inside every listed density's support: 0 < y < 1 for beta,
  // y above the lower bound and below the upper for uniform, and positive
  // shape/scale arguments for the rest.
#define STANLI_TEST_DENSITY(code, fn, arity)                              \
  {                                                                       \
    Build b({0.63, 0.4, 1.7});                                            \
    const int d = b.emit(Program::code, 0, arity > 1 ? 1 : 0,             \
                         arity > 2 ? 2 : 0);                              \
    check(std::string("density ") + #fn, b.done({d}, {1.25}));            \
  }
  STANLI_PROGRAM_DENSITY_LIST(STANLI_TEST_DENSITY)
#undef STANLI_TEST_DENSITY
}

// ---- a composite region ------------------------------------------------

// The shape the carver actually sees: an unrolled recurrence, each step
// reading the previous state, adding a density term, and writing the state
// back in place.
static void test_recurrence() {
  Build b({0.35, -0.2, 0.9});  // state, mu, sigma
  int st = 0;
  for (int t = 0; t < 6; ++t) {
    const int y = b.konst(0.3 * t - 0.5);
    const int e = b.emit(Program::NORMAL, y, 1, 2);
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

int main() {
  test_binary_ops();
  test_unary_ops();
  test_overwrite_needs_checkpoint();
  test_self_write();
  test_accumulate_into_one_register();
  test_ranged();
  test_reductions();
  test_densities();
  test_recurrence();
  test_two_gradients();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_adjoint: all passed\n");
  return 0;
}
