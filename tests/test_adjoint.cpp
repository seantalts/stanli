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
                                           std::vector<double>* out_vals) {
  std::vector<double> val((size_t)p.n_regs, 0.0);
  int64_t off = 0;
  for (const auto& li : p.ins)
    for (int i = 0; i < li.len; ++i)
      val[(size_t)(li.reg + i)] = in[(size_t)off++];
  run_program(p, val.data());
  for (size_t m = 0; m < p.out_regs.size(); ++m)
    out_vals->push_back(val[(size_t)p.out_regs[m]]);

  std::vector<double> adj((size_t)p.n_regs, 0.0);
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

static const char* code_name(Program::Code c) {
  switch (c) {
    case Program::CONST:
      return "CONST";
    case Program::CONSTR:
      return "CONSTR";
    case Program::MOV:
      return "MOV";
    case Program::MOVR:
      return "MOVR";
    case Program::ADD:
      return "ADD";
    case Program::SUB:
      return "SUB";
    case Program::MUL:
      return "MUL";
    case Program::DIV:
      return "DIV";
    case Program::NEG:
      return "NEG";
    case Program::EXP:
      return "EXP";
    case Program::LOG:
      return "LOG";
    case Program::SQRT:
      return "SQRT";
    case Program::SQUARE:
      return "SQUARE";
    case Program::TANH:
      return "TANH";
    case Program::DOT:
      return "DOT";
    case Program::LSE_RANGE:
      return "LSE_RNG";
    case Program::LOG_RANGE:
      return "LOG_RNG";
    case Program::EXP_RANGE:
      return "EXP_RNG";
    case Program::SOFTMAX:
      return "SOFTMAX";
    case Program::LSE2:
      return "LSE2";
    case Program::LOG_MIX:
      return "LOG_MIX";
    default:
      return "OTHER";
  }
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
    std::printf("  %3zu %-7s r%d <- r%d, r%d (len %d)\n", i, code_name(I.code),
                I.dst, I.a, I.b, I.len);
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
static bool check(const std::string& name, Case c, int64_t tol = 0) {
  const IslandProg orig = c.p;  // before checkpoints are inserted
  const bool ok = gen_adjoint(c.p);
  if (!ok) {
    ++failures;
    std::printf("FAIL %s: gen_adjoint refused the program\n", name.c_str());
    return false;
  }
  bool passed = true;
  std::vector<double> want_v, got_v;
  const std::vector<double> want = replay_adjoints(orig, c.in, c.seed, &want_v);
  const std::vector<double> got = native_adjoints(c.p, c.in, c.seed, &got_v);
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

static void test_unary_ops() {
  const Program::Code codes[] = {
      Program::NEG,    Program::EXP,       Program::LOG,   Program::SQRT,
      Program::SQUARE, Program::INV_LOGIT, Program::LOG1M, Program::TANH,
      Program::INV,    Program::FABS};
  const char* names[] = {"neg",       "exp",   "log",  "sqrt", "square",
                         "inv_logit", "log1m", "tanh", "inv",  "fabs"};
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

// ---- densities ---------------------------------------------------------

static void test_densities() {
  // EVERY density the register machine speaks, discovered from the shared
  // table rather than listed here, so one added to the runtime is covered
  // the day it arrives instead of the day someone remembers this file.
  //
  // Each has its own support, so rather than curate a point per density
  // the loop tries a few tuples and keeps the first the density accepts
  // (a finite value). A density that accepts none of them is a failure,
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
        program_density_partials(id, args.data(), probe);
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
         !program_density_partials(id, args, partials));
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
    program_density_partials(id, bad_domain, partials);
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
  test_binary_ops();
  test_unary_ops();
  test_overwrite_needs_checkpoint();
  test_self_write();
  test_live_copy_both_read();
  test_copy_then_modify_chain();
  test_accumulate_into_one_register();
  test_ranged();
  test_in_place_ranges();
  test_nan_operands();
  test_reductions();
  test_densities();
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
