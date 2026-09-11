// Tape islands: a compiled region must match the op-by-op graph it
// replaces (values and gradients), and everything the carver cannot prove
// safe must stay untouched.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static int failures = 0;
static void expect(const char* what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}
static void expect_exact(const std::string& what, double got, double want) {
  if (got == want) return;
  ++failures;
  std::printf("FAIL %-24s got %.17g want %.17g\n", what.c_str(), got, want);
}

static void expect_close(const std::string& what, double got, double want) {
  const double rel = std::abs(got - want) / std::max(std::abs(want), 1e-300);
  if (!(rel < 1e-12)) {
    ++failures;
    std::printf("FAIL %-24s got %.17g want %.17g rel %.2e\n", what.c_str(), got,
                want, rel);
  }
}

using namespace stanli;
using stanli::testutil::Fills;

static double fill_at(int64_t i) { return 0.3 + 0.15 * (i % 4); }
static std::vector<double> run_grad(Graph g, const Fills& fills) {
  return testutil::run_grad(std::move(g), fills, fill_at);
}

// The island kernel reuses its executor-owned compact adjoint file. Running the
// same executor twice catches a stale cell beyond the shortened zeroed range.
static std::vector<double> run_grad_twice(Graph g, const Fills& fills) {
  Executor ex(std::move(g));
  for (const auto& f : fills) {
    double* p = ex.value_ptr(f.first);
    for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
  }
  for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = fill_at(i);
  std::vector<double> first(1 + (size_t)ex.n_params());
  std::vector<double> second(1 + (size_t)ex.n_params());
  first[0] = ex.gradient(first.data() + 1);
  second[0] = ex.gradient(second.data() + 1);
  expect("repeat sizes", first.size() == second.size());
  for (size_t i = 0; i < first.size(); ++i)
    expect_close("repeat v" + std::to_string(i), second[i], first[i]);
  return second;
}

static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A necessity island whose live-out is written only inside the branch:
// `sum(a) > 0 ? ident(b + c) : c` inlines to an assignment of the --O1
// inliner's zero-length return symbol under `if (sum(a) > 0)`, and the
// program compiler sizes that symbol where it is assigned. At the zero
// point the condition is false, so those registers are the arm that did
// not run -- and the live-out harvest reads them regardless. Before the
// prologue fill (mir_prog.hpp) the backward's var replay read a register
// file no one had written and dereferenced a null vari: SIGSEGV, not a
// wrong number. Each replay now starts with an empty register file.
static void test_branch_bound_live_out() {
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/branchudf.tmir.sexp"), DataMap());
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  const int64_t n = ex.n_params();
  expect("branchudf params", n == 150);
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = 0.0;
  std::vector<double> grad((size_t)n, 0.0);
  const double lp = ex.gradient(grad.data());
  // The untaken arm contributes nothing: mix is c, so the target is
  // sum(c) = 0 and only c's 50 entries carry an adjoint.
  expect("branchudf lp", lp == 0.0);
  int64_t wrong = 0;
  for (int64_t i = 0; i < n; ++i)
    if (grad[(size_t)i] != (i < 100 ? 0.0 : 1.0)) ++wrong;
  expect("branchudf grad", wrong == 0);
}

// A mini HMM forward pass: per step, index the previous state pair, take
// their log-sum-exp, add per-state emission lps (scalar NORMAL, propto
// off), SET_INDEX the new pair into a zero-backed template vector. The
// final step's pair feeds one LSE2 whose out is the target term, which
// ends the region: the pair slots become the island's live-outs.
struct HmmGraph {
  Graph g;
  Fills fills;
  std::vector<int> terms;
  size_t body_ops = 0;
};

// W widens the state vector without changing what is computed: only two
// of its elements are ever read. What it does change is what the ops
// COPY -- each SET_INDEX rewrites the whole vector -- which is the shape
// the cost estimate is looking for (test_vector_copies_carved).
static HmmGraph build_hmm(int T, int W = 2) {
  HmmGraph h;
  Graph& g = h.g;
  const int gp0 = g.add_slot(W, true);  // initial log-state
  const int mu = g.add_slot(2, true);
  const int sigma = g.add_slot(1, true);
  const int z2 = g.add_slot(W, false);  // fill-backed template (absorbed)
  h.fills.emplace_back(z2, std::vector<double>((size_t)W, 0.0));
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    h.fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  int gp = gp0;
  const size_t start = g.ops.size();
  for (int t = 0; t < T; ++t) {
    const int y = cslot(0.35 * t - 0.8);
    const int g0 = g.add_slot(1, false);
    g.add_op(OP_INDEX, {gp}, g0, {0});
    const int g1 = g.add_slot(1, false);
    g.add_op(OP_INDEX, {gp}, g1, {1});
    const int s0 = g.add_slot(1, false);
    g.add_op(OP_LSE2, {g0, g1}, s0);
    int next = -1;
    for (int k = 0; k < 2; ++k) {
      const int mk = g.add_slot(1, false);
      g.add_op(OP_INDEX, {mu}, mk, {k});
      const int em = g.add_slot(1, false);
      const int id = g.add_op(OP_NORMAL_LPDF, {y, mk, sigma}, em);
      g.ops[id].variant = 0x06;  // y data, mu/sigma active; propto OFF
      const int nk = g.add_slot(1, false);
      g.add_op(OP_ADD, {s0, em}, nk);
      const int dst = g.add_slot(W, false);
      // The first step fills the zero template (absorbed as a constant);
      // later steps overwrite the previous step's state, which is what a
      // forward algorithm does and what lets the registers alias.
      g.add_op(OP_SET_INDEX, {k == 0 ? (t == 0 ? z2 : gp) : next, nk}, dst,
               {k});
      next = dst;
    }
    gp = next;
  }
  h.body_ops = g.ops.size() - start;
  const int f0 = g.add_slot(1, false);
  g.add_op(OP_INDEX, {gp}, f0, {0});
  const int f1 = g.add_slot(1, false);
  g.add_op(OP_INDEX, {gp}, f1, {1});
  const int lp = g.add_slot(1, false);
  g.add_op(OP_LSE2, {f0, f1}, lp);
  h.terms.push_back(lp);
  g.result_slot = lp;
  return h;
}

// compact_program (program.cpp) on programs small enough to write down.
// The gradients the removals must not move are covered by every other case
// in this file; these pin which instructions survive.
static std::string opcodes(const Program& p) {
  std::string out;
  for (const auto& I : p.code) {
    if (!out.empty()) out += ' ';
    out += program_code_spec(I.code).name;
  }
  return out;
}

static void expect_eq(const std::string& what, const std::string& got,
                      const std::string& want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %s: got [%s] want [%s]\n", what.c_str(), got.c_str(),
                want.c_str());
  }
}

static void expect_eq(const std::string& what, int got, int want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %s: got %d want %d\n", what.c_str(), got, want);
  }
}

static void test_compact_copy_chain() {
  Program p;
  p.n_regs = 4;
  p.pool = {2.0};
  p.code = {
      {Program::CONST, 1, 0}, {Program::MOV, 2, 1}, {Program::ADD, 3, 0, 2}};
  p.out_regs = {3};
  std::vector<std::pair<int, int>> seeded{{0, 1}};
  compact_program(p, seeded);
  expect_eq("copy chain code", opcodes(p), "CONST ADD");
  expect_eq("copy chain regs", p.n_regs, 3);
  expect_eq("copy chain add reads the source", p.code[1].b, 1);
  expect_eq("copy chain live-out", p.out_regs[0], 2);
}

static void test_compact_dead_fill() {
  Program p;
  p.n_regs = 5;
  p.pool = {0.0, 0.0};
  p.code = {{Program::CONSTR, 2, 0, 0, 0, 2},
            {Program::EXP_RANGE, 2, 0, 0, 0, 2},
            {Program::DOT, 4, 2, 0, 0, 2}};
  p.out_regs = {4};
  std::vector<std::pair<int, int>> seeded{{0, 2}};
  compact_program(p, seeded);
  expect_eq("dead fill code", opcodes(p), "EXP_RANGE DOT");
  expect_eq("dead fill regs", p.n_regs, 5);
}

static void test_compact_rewritten_source_kept() {
  Program p;
  p.n_regs = 4;
  p.pool = {2.0, 3.0};
  p.code = {{Program::CONST, 1, 0},
            {Program::MOV, 2, 1},
            {Program::CONST, 1, 1},
            {Program::ADD, 3, 2, 1}};
  p.out_regs = {3};
  std::vector<std::pair<int, int>> seeded{{0, 1}};
  compact_program(p, seeded);
  expect_eq("rewritten source keeps the copy", opcodes(p),
            "CONST MOV CONST ADD");
}

static void test_compact_second_writer_kept() {
  Program p;
  p.n_regs = 4;
  p.pool = {2.0, 3.0};
  p.code = {{Program::CONST, 1, 0},
            {Program::MOV, 2, 1},
            {Program::CONST, 2, 1},
            {Program::ADD, 3, 2, 1}};
  p.out_regs = {3};
  std::vector<std::pair<int, int>> seeded{{0, 1}};
  compact_program(p, seeded);
  expect_eq("second writer keeps the copy", opcodes(p), "CONST MOV CONST ADD");
}

static void test_compact_range_copy() {
  Program p;
  p.n_regs = 10;
  p.code = {{Program::MOVR, 4, 0, 0, 0, 4},
            {Program::LSE_RANGE, 8, 4, 0, 0, 4}};
  p.out_regs = {8};
  std::vector<std::pair<int, int>> seeded{{0, 4}};
  compact_program(p, seeded);
  expect_eq("range copy code", opcodes(p), "LSE_RANGE");
  expect_eq("range copy reads the source", p.code[0].a, 0);
  expect_eq("range copy regs", p.n_regs, 5);
}

// A scalar producer per lane followed by three copies constructs one ranged
// value. The ordinary source-alias pass must retain those copies because each
// destination is an interior boundary; destination forwarding can instead
// make the producers write the contiguous range directly.
static void test_compact_forwards_producers_into_range() {
  auto make = [] {
    Program p;
    p.n_regs = 13;
    p.code = {{Program::ADD, 6, 0, 3},
              {Program::MOV, 9, 6},
              {Program::ADD, 7, 1, 4},
              {Program::MOV, 10, 7},
              {Program::ADD, 8, 2, 5},
              {Program::MOV, 11, 8},
              {Program::LSE_RANGE, 12, 9, 0, 0, 3}};
    p.out_regs = {12};
    return p;
  };
  std::vector<std::pair<int, int>> seeded{{0, 6}};

  Program forwarded = make();
  compact_program(forwarded, seeded);
  expect_eq("destination forwarding code", opcodes(forwarded),
            "ADD ADD ADD LSE_RANGE");

  test_setenv("STANLI_NO_PROGRAM_DEST_FORWARD", "1", 1);
  Program ordinary = make();
  std::vector<std::pair<int, int>> ordinary_seeded{{0, 6}};
  compact_program(ordinary, ordinary_seeded);
  test_unsetenv("STANLI_NO_PROGRAM_DEST_FORWARD");
  expect_eq("destination forwarding opt-out", opcodes(ordinary),
            "ADD MOV ADD MOV ADD MOV LSE_RANGE");
}

static void test_compact_destination_forwarding_refuses_input_alias() {
  Program p;
  p.n_regs = 4;
  p.code = {
      {Program::ADD, 2, 0, 1}, {Program::MOV, 0, 2}, {Program::ADD, 3, 0, 1}};
  p.out_regs = {3};
  std::vector<std::pair<int, int>> seeded{{0, 2}};
  compact_program(p, seeded);
  expect_eq("destination forwarding input alias", opcodes(p), "ADD MOV ADD");
}

static void test_compact_destination_forwarding_refusals() {
  {
    Program p;
    p.n_regs = 11;
    p.code = {{Program::LOG_RANGE, 6, 0, 0, 0, 4},
              {Program::MOVR, 2, 6, 0, 0, 4},
              {Program::LSE_RANGE, 10, 2, 0, 0, 4}};
    p.out_regs = {10};
    std::vector<std::pair<int, int>> seeded{{0, 4}};
    compact_program(p, seeded);
    expect_eq("destination forwarding partial input overlap", opcodes(p),
              "LOG_RANGE MOVR LSE_RANGE");
  }
  {
    Program p;
    p.n_regs = 6;
    p.code = {
        {Program::ADD, 4, 0, 1}, {Program::MOV, 2, 4}, {Program::ADD, 5, 4, 2}};
    p.out_regs = {5};
    std::vector<std::pair<int, int>> seeded{{0, 3}};
    compact_program(p, seeded);
    expect_eq("destination forwarding extra temporary read", opcodes(p),
              "ADD MOV ADD");
  }
  {
    Program p;
    p.n_regs = 5;
    p.code = {
        {Program::ADD, 2, 0, 1}, {Program::MOV, 3, 2}, {Program::MUL, 4, 3, 1}};
    p.out_regs = {4};
    std::vector<std::pair<int, int>> seeded{{0, 4}};
    compact_program(p, seeded);
    expect_eq("destination forwarding seeded temporary", opcodes(p),
              "ADD MOV MUL");
  }
  {
    Program p;
    p.n_regs = 6;
    p.code = {
        {Program::ADD, 4, 0, 1}, {Program::MOV, 2, 4}, {Program::MUL, 5, 2, 1}};
    p.out_regs = {4, 5};
    std::vector<std::pair<int, int>> seeded{{0, 3}};
    compact_program(p, seeded);
    expect_eq("destination forwarding live-out temporary", opcodes(p),
              "ADD MOV MUL");
  }
  {
    Program p;
    p.n_regs = 6;
    p.code = {{Program::ADD, 4, 0, 1},
              {Program::MOV, 2, 4},
              {Program::JZ, 3, 3},
              {Program::ADD, 5, 2, 1}};
    p.out_regs = {5};
    std::vector<std::pair<int, int>> seeded{{0, 4}};
    compact_program(p, seeded);
    expect_eq("destination forwarding branch program", opcodes(p),
              "ADD MOV JZ ADD");
  }
}

// A read that spans two copies must not be split across their sources.
static void test_compact_straddling_range_kept() {
  Program p;
  p.n_regs = 12;
  p.code = {{Program::MOVR, 4, 0, 0, 0, 2},
            {Program::MOVR, 6, 2, 0, 0, 2},
            {Program::LSE_RANGE, 8, 5, 0, 0, 2}};
  p.out_regs = {8};
  std::vector<std::pair<int, int>> seeded{{0, 4}};
  compact_program(p, seeded);
  expect_eq("straddling range keeps both copies", opcodes(p),
            "MOVR MOVR LSE_RANGE");
}

static void test_compact_call_ranges() {
  Program p;
  p.n_regs = 8;
  Program::Call c;
  c.opcode = OP_ADD;
  c.n_in = 1;
  c.in[0] = 2;
  c.in_len[0] = 2;
  c.out = 4;
  c.out_len = 1;
  c.scratch = 5;
  c.scratch_len = 2;
  p.calls = {c};
  p.code = {{Program::MOVR, 2, 0, 0, 0, 2},
            {Program::CALL, 0, 0},
            {Program::MOV, 7, 4}};
  p.out_regs = {7};
  std::vector<std::pair<int, int>> seeded{{0, 2}};
  compact_program(p, seeded);
  expect_eq("call input range keeps its copy", opcodes(p), "MOVR CALL");
  expect_eq("call regs", p.n_regs, 7);
  expect_eq("call live-out", p.out_regs[0], 4);
}

static size_t hmm_island_instrs(const char* what) {
  HmmGraph h = build_hmm(8);
  if (carve_islands(h.g, h.fills, h.terms, {}) != 1) {
    ++failures;
    std::printf("FAIL %s: no island carved\n", what);
    return 0;
  }
  for (const Op& op : h.g.ops)
    if (op.opcode == OP_ISLAND)
      return static_cast<const IslandProg*>(op.udata)->code.size();
  return 0;
}

static void test_compact_env_disable() {
  test_setenv("STANLI_NO_ISLAND_COMPACT", "1", 1);
  const size_t off = hmm_island_instrs("compaction off");
  test_unsetenv("STANLI_NO_ISLAND_COMPACT");
  const size_t on = hmm_island_instrs("compaction on");
  expect("compaction shrinks the hmm island", on < off);
}

static void test_hmm_parity() {
  HmmGraph ref = build_hmm(8);  // 8*11 = 88 body ops, above threshold
  const std::vector<double> want = run_grad(std::move(ref.g), ref.fills);

  HmmGraph isl = build_hmm(8);
  const size_t before = isl.g.ops.size();
  const int carved = carve_islands(isl.g, isl.fills, isl.terms, {});
  expect("hmm carved==1", carved == 1);
  // The run swallows the two trailing INDEX ops too (in vocab, non-term);
  // their outs feed the term LSE2 outside, so they are the live-outs:
  // island + 2 extractions + the final LSE2.
  expect("hmm ops==4", isl.g.ops.size() == 4);
  expect("hmm shrank", isl.g.ops.size() < before);
  expect("hmm island first", isl.g.ops[0].opcode == OP_ISLAND);
  expect("hmm 3 live-ins", isl.g.ops[0].n_in == 3);
  const std::vector<double> got = run_grad(std::move(isl.g), isl.fills);
  expect("hmm sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("hmm v" + std::to_string(i), got[i], want[i]);
}

// STANLI_NO_ISLAND switches the carver off entirely. harnesses/ab_corpus.py
// builds its whole A side out of this and the three sibling switches, so a
// rename here would leave that oracle comparing the optimized graph against
// itself, green, with no ctest to catch it.
static void test_env_disable() {
  HmmGraph h = build_hmm(8);
  const size_t before = h.g.ops.size();

  test_setenv("STANLI_NO_ISLAND", "1", 1);
  expect("disabled: nothing carved",
         carve_islands(h.g, h.fills, h.terms, {}) == 0);
  expect("disabled: graph untouched", h.g.ops.size() == before);
  test_unsetenv("STANLI_NO_ISLAND");

  // The same graph with the switch off: the run carves.
  expect("enabled: one island", carve_islands(h.g, h.fills, h.terms, {}) == 1);
}

static void test_short_run_untouched() {
  HmmGraph h = build_hmm(2);  // 22 body ops, under threshold
  const size_t before = h.g.ops.size();
  const int carved = carve_islands(h.g, h.fills, h.terms, {});
  expect("short none carved", carved == 0);
  expect("short ops unchanged", h.g.ops.size() == before);
}

static void test_propto_density_refused() {
  HmmGraph h = build_hmm(8);
  for (auto& op : h.g.ops)
    if (op.opcode == OP_NORMAL_LPDF) op.variant = 0x86;  // propto ON
  const size_t before = h.g.ops.size();
  const int carved = carve_islands(h.g, h.fills, h.terms, {});
  expect("propto none carved", carved == 0);
  expect("propto ops unchanged", h.g.ops.size() == before);
}

static void test_unsupported_op_splits() {
  // A vector-out op mid-region splits the run into halves below the
  // threshold. It used to be POW, but POW -- and every scalar-out op with
  // a kernel -- now compiles as a CALL; what still refuses is an output
  // wider than one register (phase 2 of the kernel-call plan).
  HmmGraph h = build_hmm(5);  // 55 body ops
  Graph& g = h.g;
  const size_t mid = g.ops.size() / 2;
  Op pw;
  pw.opcode = OP_REP_VEC;
  pw.n_in = 1;
  pw.in[0] = g.ops[mid].in[0];
  pw.out = g.add_slot(3, false);
  g.ops.insert(g.ops.begin() + (long)mid, pw);
  h.terms.back() = h.g.result_slot;  // unchanged, re-anchor after insert
  const size_t before = g.ops.size();
  const int carved = carve_islands(g, h.fills, h.terms, {});
  expect("split none carved", carved == 0);
  expect("split ops unchanged", g.ops.size() == before);
}

// A region that carries far more state than it computes: each step drops
// one scalar into its own wide template, so the register file grows by a
// whole vector per three instructions, and the file is written by the
// forward and read back by the backward every call. This is the one shape
// the estimate still has to refuse once the backward stops building vars:
// `bones_model` is it (36 ops behind 4,024 registers) and islands cost it
// 19x replayed, 4x with a generated adjoint.
static Graph build_wide_state(Fills& fills, std::vector<int>& terms) {
  Graph g;
  const int W = 64;
  int prev = g.add_slot(1, true);
  for (int t = 0; t < 12; ++t) {
    const int sq = g.add_slot(1, false);
    g.add_op(OP_MUL, {prev, prev}, sq);
    const int tmpl = g.add_slot(W, false);
    fills.emplace_back(tmpl, std::vector<double>((size_t)W, 0.0));
    const int wide = g.add_slot(W, false);
    g.add_op(OP_SET_INDEX, {tmpl, sq}, wide, {0});
    const int back = g.add_slot(1, false);
    g.add_op(OP_INDEX, {wide}, back, {0});
    prev = back;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_ADD, {prev, prev}, lp);
  g.result_slot = lp;
  terms = {lp};
  return g;
}

static void test_wide_state_refused() {
  Fills fills;
  std::vector<int> terms;
  Graph g = build_wide_state(fills, terms);
  const size_t before = g.ops.size();  // 36 in vocab, above kMinIslandOps
  const int carved = carve_islands(g, fills, terms, {});
  expect("wide none carved", carved == 0);
  expect("wide ops unchanged", g.ops.size() == before);

  // Pin the override directly instead of relying on the cross-path fixture
  // corpus to retain a model on exactly the losing side of the cost model.
  Fills forced_fills;
  std::vector<int> forced_terms;
  Graph forced = build_wide_state(forced_fills, forced_terms);
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
  expect("wide forced carve",
         carve_islands(forced, forced_fills, forced_terms, {}) == 1);
  test_unsetenv("STANLI_ISLAND_ALWAYS");
}

// The cost estimate, on the two shapes it has to tell apart. A wide
// state vector copied per step is `iohmm_reg`: the ops move far more
// than the register file does, and the island is the cheaper form.
static void test_vector_copies_carved() {
  HmmGraph ref = build_hmm(8, 128);
  const std::vector<double> want = run_grad(std::move(ref.g), ref.fills);

  HmmGraph ordinary = build_hmm(8, 128);
  test_setenv("STANLI_NO_PROGRAM_DEST_FORWARD", "1", 1);
  const int ordinary_carved =
      carve_islands(ordinary.g, ordinary.fills, ordinary.terms, {});
  test_unsetenv("STANLI_NO_PROGRAM_DEST_FORWARD");

  HmmGraph isl = build_hmm(8, 128);
  const int carved = carve_islands(isl.g, isl.fills, isl.terms, {});
  expect("copies carved==1", carved == 1);
  expect("copies opt-out also carved==1", ordinary_carved == 1);
  expect("copies ops==4", isl.g.ops.size() == 4);
  auto island_instructions = [](const Graph& g) {
    size_t n = 0;
    for (const Op& op : g.ops)
      if (op.opcode == OP_ISLAND)
        n += static_cast<const IslandProg*>(op.udata)->code.size();
    return n;
  };
  expect("copies forwarding shrinks an already profitable island",
         island_instructions(isl.g) < island_instructions(ordinary.g));
  const std::vector<double> got = run_grad_twice(std::move(isl.g), isl.fills);
  expect("copies sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("copies v" + std::to_string(i), got[i], want[i]);
}

// The SOFTMAX(3) specialization is selected only after a graph run has been
// compiled, admitted by the island cost model, and given a native adjoint.
// Exercise that whole route rather than calling specialize_softmax3 directly:
// 32 softmaxes clear its activation threshold, their selected lanes feed one
// scalar recurrence, and the target op remains graph-visible after carving.
static HmmGraph build_softmax3_island() {
  HmmGraph h;
  Graph& g = h.g;
  const int logits = g.add_slot(3, true);
  int acc = -1;
  const size_t start = g.ops.size();
  for (int k = 0; k < 32; ++k) {
    const int probs = g.add_slot(3, false);
    g.add_op(OP_SOFTMAX, {logits}, probs);
    const int lane = g.add_slot(1, false);
    g.add_op(OP_INDEX, {probs}, lane, {k % 3});
    if (acc < 0) {
      acc = lane;
    } else {
      const int next = g.add_slot(1, false);
      g.add_op(OP_ADD, {acc, lane}, next);
      acc = next;
    }
  }
  h.body_ops = g.ops.size() - start;
  const int lp = g.add_slot(1, false);
  g.add_op(OP_SQUARE, {acc}, lp);
  h.terms.push_back(lp);
  g.result_slot = lp;
  return h;
}

static void test_softmax3_island_executor() {
  HmmGraph ref = build_softmax3_island();
  const std::vector<double> want = run_grad(std::move(ref.g), ref.fills);

  HmmGraph disabled = build_softmax3_island();
  test_setenv("STANLI_NO_ISLAND_SOFTMAX3", "1", 1);
  const int disabled_carved =
      carve_islands(disabled.g, disabled.fills, disabled.terms, {});
  test_unsetenv("STANLI_NO_ISLAND_SOFTMAX3");
  expect_eq("softmax3 opt-out still carves", disabled_carved, 1);
  int ordinary_islands = 0;
  for (const Op& op : disabled.g.ops) {
    if (op.opcode != OP_ISLAND) continue;
    ++ordinary_islands;
    expect("softmax3 opt-out ordinary variant", op.variant == 0);
    const auto* program = static_cast<const IslandProg*>(op.udata);
    expect("softmax3 opt-out canonical payload", program != nullptr);
  }
  expect_eq("softmax3 opt-out ordinary island count", ordinary_islands, 1);
  const std::vector<double> disabled_got =
      run_grad_twice(std::move(disabled.g), disabled.fills);
  expect("softmax3 opt-out result size", disabled_got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < disabled_got.size(); ++i)
    expect_close("softmax3 opt-out v" + std::to_string(i), disabled_got[i],
                 want[i]);

  HmmGraph optimized = build_softmax3_island();
  expect("softmax3 e2e body above island threshold", optimized.body_ops >= 32);
  expect("softmax3 e2e carved",
         carve_islands(optimized.g, optimized.fills, optimized.terms, {}) == 1);

  const Softmax3IslandProg* program = nullptr;
  int specialized_islands = 0;
  for (const Op& op : optimized.g.ops) {
    if (op.opcode != OP_ISLAND || op.variant != kIslandSoftmax3Variant)
      continue;
    ++specialized_islands;
    const auto* base = static_cast<const IslandProg*>(op.udata);
    program = static_cast<const Softmax3IslandProg*>(base);
  }
  expect_eq("softmax3 e2e specialized island count", specialized_islands, 1);
  expect("softmax3 e2e payload", program != nullptr);
  if (program) {
    expect("softmax3 e2e native adjoint", program->native_adj);
    expect("softmax3 e2e optimized clone",
           static_cast<bool>(program->optimized_double));
    int canonical_softmaxes = 0;
    for (const Program::Instr& I : program->code)
      if (I.code == Program::SOFTMAX && I.len == 3) ++canonical_softmaxes;
    expect_eq("softmax3 e2e canonical softmaxes", canonical_softmaxes, 32);

    if (program->optimized_double) {
      int optimized_calls = 0;
      for (const Program::Call& call : program->optimized_double->calls)
        if (call.opcode == kProgramSoftmax3Opcode &&
            call.variant == kProgramSoftmax3Variant)
          ++optimized_calls;
      expect_eq("softmax3 e2e optimized calls", optimized_calls, 32);
    }
  }

  const std::vector<double> got =
      run_grad_twice(std::move(optimized.g), optimized.fills);
  expect("softmax3 e2e result size", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("softmax3 e2e v" + std::to_string(i), got[i], want[i]);
}

// Once specialization registers its private OP_NONE_ table slot, malformed
// graph IR must still fail exactly as loudly as it did when that slot was
// null. Check both routes: the carver must not turn it into a private CALL,
// and the executor must reject it before the helper can see an arbitrary
// scalar context as a three-lane softmax.
static Graph build_rogue_none_graph() {
  Graph g;
  int value = g.add_slot(1, true);
  for (int i = 0; i < 32; ++i) {
    const int next = g.add_slot(1, false);
    g.add_op(OP_NONE_, {value}, next);
    value = next;
  }
  g.result_slot = value;
  return g;
}

static void test_softmax3_private_slot_stays_invalid_graph_ir() {
  expect("softmax3 rogue guard kernel is registered",
         find_kernel(kProgramSoftmax3Opcode) != nullptr);

  Graph carver_graph = build_rogue_none_graph();
  const std::vector<int> terms{carver_graph.result_slot};
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
  expect("softmax3 rogue guard not callable",
         carve_islands(carver_graph, Fills{}, terms, {}) == 0);
  test_unsetenv("STANLI_ISLAND_ALWAYS");

  bool threw = false;
  try {
    Executor invalid(build_rogue_none_graph());
  } catch (const std::runtime_error&) {
    threw = true;
  }
  expect("softmax3 rogue guard executor throws", threw);
}

// Copy an Executor whose only opaque payload is a specialized island, then
// destroy the source before evaluation.  The idata-free graph isolates the
// shared derived-owner contract from unrelated raw graph-immediate pointers.
static void test_softmax3_payload_copy_lifetime() {
  Graph g;
  const int logits = g.add_slot(3, true);
  const int probs = g.add_slot(3, false);
  const int lp = g.add_slot(1, false);

  auto specialized = std::make_shared<Softmax3IslandProg>();
  specialized->n_regs = 6;
  specialized->ins.push_back(IslandProg::LiveIn{0, 3});
  specialized->code.push_back(Program::Instr{Program::SOFTMAX, 3, 0, 0, 0, 3});
  specialized->out_regs = {3, 4, 5};
  expect("softmax3 copy generated adjoint", gen_adjoint(*specialized));
  specialized->native_adj = true;
  specialized->optimized_double = specialize_softmax3(*specialized, 1);
  expect("softmax3 copy optimized plan",
         static_cast<bool>(specialized->optimized_double));

  std::weak_ptr<Softmax3IslandProg> lifetime = specialized;
  std::shared_ptr<IslandProg> owner = std::move(specialized);
  expect("softmax3 copy transfers local owner", !specialized);
  Op island;
  island.opcode = OP_ISLAND;
  island.variant = kIslandSoftmax3Variant;
  island.n_in = 1;
  island.in[0] = logits;
  island.out = probs;
  island.udata = owner.get();
  g.udata_pool.push_back(std::move(owner));
  g.ops.push_back(island);
  g.add_op(OP_SUM_VEC, {probs}, lp);
  g.result_slot = lp;

  std::unique_ptr<Executor> copy;
  {
    Executor source(std::move(g));
    source.params_data()[0] = 0.3;
    source.params_data()[1] = 0.7;
    source.params_data()[2] = 1.4;
    copy = std::make_unique<Executor>(source);
  }
  expect("softmax3 copy keeps payload alive", !lifetime.expired());
  double first_grad[3] = {};
  double second_grad[3] = {};
  const double first = copy->gradient(first_grad);
  const double second = copy->gradient(second_grad);
  expect("softmax3 copy value stable", first == second);
  for (int i = 0; i < 3; ++i)
    expect("softmax3 copy gradient stable", first_grad[i] == second_grad[i]);
  copy.reset();
  expect("softmax3 copy releases payload", lifetime.expired());
}

// A measured-cost boundary made from the same two facts as iohmm_reg:
// INDEX copies share their source's adjoint cell, while ADD_N expands to a
// short instruction chain. Charging one adjoint cell per forward register
// refuses this region; charging the compact file carves it. Constants keep
// the boundary honest by adding real value/adjoint work without a graph op.
static HmmGraph build_compact_cost_boundary() {
  HmmGraph h;
  Graph& g = h.g;
  constexpr int W = 4;
  const int x = g.add_slot(W, true);
  int last = -1;
  for (int t = 0; t < 8; ++t) {
    int e[W];
    for (int k = 0; k < W; ++k) {
      e[k] = g.add_slot(1, false);
      g.add_op(OP_INDEX, {x}, e[k], {k});
    }
    const int c = g.add_slot(1, false);
    h.fills.emplace_back(c, std::vector<double>{0.1 * (t + 1)});
    last = g.add_slot(1, false);
    g.add_op(OP_ADD_N, {e[0], e[1], e[2], e[3], c}, last);
  }
  h.body_ops = g.ops.size();  // 40 scalar ops, all one maximal run
  const int lp = g.add_slot(1, false);
  g.add_op(OP_ADD, {last, last}, lp);  // target term ends the run
  h.terms.push_back(lp);
  g.result_slot = lp;
  return h;
}

// The adjoint file is what the boundary is about, so this runs with
// compaction off: with it on the copies never reach gen_adjoint, and the
// region clears the estimate by a margin instead of sitting on it.
static void test_compact_adjoint_cost_boundary() {
  test_setenv("STANLI_NO_ISLAND_COMPACT", "1", 1);
  HmmGraph forced = build_compact_cost_boundary();
  const int64_t graph_cost = 6 * (int64_t)forced.body_ops;
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
  expect("compact boundary forced carve",
         carve_islands(forced.g, forced.fills, forced.terms, {}) == 1);
  test_unsetenv("STANLI_ISLAND_ALWAYS");
  const IslandProg* p = nullptr;
  for (const Op& op : forced.g.ops)
    if (op.opcode == OP_ISLAND) p = static_cast<const IslandProg*>(op.udata);
  expect("compact boundary has island", p != nullptr);
  if (p) {
    const int64_t streams =
        (int64_t)p->code.size() + (int64_t)p->adj.code.size();
    const int64_t old_sparse_cost = 3 * (int64_t)p->n_regs + streams;
    const int64_t compact_cost =
        2 * (int64_t)p->n_regs + p->adj.n_regs + streams;
    expect("compact boundary new wins", compact_cost < graph_cost);
    expect("compact boundary old loses", graph_cost < old_sparse_cost);
    expect("compact boundary actually smaller",
           p->adj.n_regs < (int)p->adj.adj_reg.size());
  }

  HmmGraph normal = build_compact_cost_boundary();
  expect("compact boundary default carve",
         carve_islands(normal.g, normal.fills, normal.terms, {}) == 1);
  test_unsetenv("STANLI_NO_ISLAND_COMPACT");

  HmmGraph compacted = build_compact_cost_boundary();
  expect("compact boundary carves compacted too",
         carve_islands(compacted.g, compacted.fills, compacted.terms, {}) == 1);
}

// The same recurrence on a two-element state: nothing is copied, so the
// island buys no data movement at all -- only the per-op tax. The estimate
// refused this shape while the backward replayed under var, because a var
// replay of the same arithmetic cost more than the ops did; with a
// generated adjoint the corpus regions it stands for measure 1.5-1.7x
// (`hmm_example`, `hmm_gaussian`, both `hmm_drive`s). So it carves now --
// and the gradient still has to be the one the ops produced.
static void test_scalar_chain_carved() {
  HmmGraph ref = build_hmm(8);
  const std::vector<double> want = run_grad(std::move(ref.g), ref.fills);

  HmmGraph isl = build_hmm(8);
  const int carved = carve_islands(isl.g, isl.fills, isl.terms, {});
  expect("scalar chain carved==1", carved == 1);
  const std::vector<double> got = run_grad(std::move(isl.g), isl.fills);
  expect("scalar chain sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("scalar chain v" + std::to_string(i), got[i], want[i]);
}

static Graph build_native_extras(Fills& fills, std::vector<int>& terms) {
  Graph g;
  const int p0 = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  int acc = p0;
  for (int t = 0; t < 6; ++t) {
    const int il = g.add_slot(1, false);
    g.add_op(OP_INV_LOGIT, {acc}, il);
    const int pw = g.add_slot(1, false);
    g.add_op(OP_POW, {il, cslot(1.5)}, pw);
    const int iv = g.add_slot(1, false);
    g.add_op(OP_INV, {pw}, iv);
    const int fx = g.add_slot(1, false);
    g.add_op(OP_FMAX, {iv, cslot(0.2)}, fx);
    const int fn = g.add_slot(1, false);
    g.add_op(OP_FMIN, {fx, cslot(5.0)}, fn);
    const int ab = g.add_slot(1, false);
    g.add_op(OP_ABS, {fn}, ab);
    const int ld = g.add_slot(1, false);
    g.add_op(OP_LOG_DIFF_EXP, {ab, cslot(-1.0)}, ld);
    const int l1 = g.add_slot(1, false);
    g.add_op(OP_LOG1P_EXP, {ld}, l1);
    acc = l1;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_ADD, {acc, p0}, lp);
  g.result_slot = lp;
  terms = {lp};
  return g;
}

static void test_native_extras_carved() {
  Fills ref_fills;
  std::vector<int> ref_terms;
  Graph ref = build_native_extras(ref_fills, ref_terms);
  const std::vector<double> want = run_grad(std::move(ref), ref_fills);

  Fills fills;
  std::vector<int> terms;
  Graph g = build_native_extras(fills, terms);
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
  const int carved = carve_islands(g, fills, terms, {});
  test_unsetenv("STANLI_ISLAND_ALWAYS");
  expect("native extras carved==1", carved == 1);

  bool has_pow = false, has_fmax = false, has_fmin = false, has_inv = false,
       has_fabs = false, has_log_diff_exp = false, has_log1p_exp = false,
       has_call = false;
  for (const Op& op : g.ops) {
    if (op.opcode != OP_ISLAND) continue;
    const auto& p = *static_cast<const IslandProg*>(op.udata);
    for (const auto& I : p.code) {
      switch (I.code) {
        case Program::POW:
          has_pow = true;
          break;
        case Program::FMAX:
          has_fmax = true;
          break;
        case Program::FMIN:
          has_fmin = true;
          break;
        case Program::INV:
          has_inv = true;
          break;
        case Program::FABS:
          has_fabs = true;
          break;
        case Program::LOG_DIFF_EXP:
          has_log_diff_exp = true;
          break;
        case Program::LOG1P_EXP:
          has_log1p_exp = true;
          break;
        case Program::CALL:
          has_call = true;
          break;
        default:
          break;
      }
    }
  }
  expect("native extras has POW", has_pow);
  expect("native extras has FMAX", has_fmax);
  expect("native extras has FMIN", has_fmin);
  expect("native extras has INV", has_inv);
  expect("native extras has FABS", has_fabs);
  expect("native extras has LOG_DIFF_EXP", has_log_diff_exp);
  expect("native extras has LOG1P_EXP", has_log1p_exp);
  expect("native extras no CALL", !has_call);

  const std::vector<double> got = run_grad(std::move(g), fills);
  expect("native extras sizes", got.size() == want.size());
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("native extras v" + std::to_string(i), got[i], want[i]);
}

// pow at a base of exactly zero, over the exponents stan-math dispatches on
// plus one that stays a parameter. Against the values, not just the ops:
// both engines used to agree on the same wrong zero. `law` is what the
// lowering would have read off the exponent's static type.
static Graph build_pow_zero(Fills& fills, std::vector<int>& terms,
                            uint8_t law) {
  Graph g;
  const int base = g.add_slot(1, true);
  const int vexp = g.add_slot(1, true);
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  auto pw = [&](int e, uint8_t v) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_POW, {base, e}, s);
    g.ops.back().variant = v;
    return s;
  };
  int acc = -1;
  auto add = [&](int x) {
    if (acc < 0) {
      acc = x;
      return;
    }
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD, {acc, x}, s);
    acc = s;
  };
  for (int t = 0; t < 8; ++t) {
    add(pw(cslot(1.0), law));
    add(pw(cslot(2.0), law));
    add(pw(cslot(0.5), law));
    add(pw(vexp, kPowZeroBaseGuarded));
  }
  g.result_slot = acc;
  terms = {acc};
  return g;
}

static void check_pow_zero_law(const std::string& tag, uint8_t law,
                               const std::vector<double>& want) {
  auto at_zero = [](int64_t) { return 0.0; };

  Fills ref_fills;
  std::vector<int> ref_terms;
  Graph ref = build_pow_zero(ref_fills, ref_terms, law);
  const std::vector<double> ops =
      testutil::run_grad(std::move(ref), ref_fills, at_zero);
  expect((tag + " ops sizes").c_str(), ops.size() == want.size());
  for (size_t i = 0; i < want.size() && i < ops.size(); ++i)
    expect_close(tag + " ops v" + std::to_string(i), ops[i], want[i]);

  // Both island engines: the generated adjoint reads the law off the
  // instruction, the var replay off the overload it calls.
  for (const bool replay : {false, true}) {
    const std::string what = tag + (replay ? " replay" : " island");
    Fills fills;
    std::vector<int> terms;
    Graph g = build_pow_zero(fills, terms, law);
    test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
    if (replay) test_setenv("STANLI_NO_NATIVE_ADJ", "1", 1);
    const int carved = carve_islands(g, fills, terms, {});
    test_unsetenv("STANLI_ISLAND_ALWAYS");
    test_unsetenv("STANLI_NO_NATIVE_ADJ");
    expect((what + " carved==1").c_str(), carved == 1);
    const std::vector<double> got =
        testutil::run_grad(std::move(g), fills, at_zero);
    expect((what + " sizes").c_str(), got.size() == want.size());
    for (size_t i = 0; i < want.size() && i < got.size(); ++i)
      expect_close(what + " v" + std::to_string(i), got[i], want[i]);
  }
}

static void test_pow_zero_base_carved() {
  check_pow_zero_law("pow zero scalar law", kPowZeroBaseScalar,
                     {8.0, 8.0, 0.0});
  check_pow_zero_law("pow zero guarded", kPowZeroBaseGuarded, {8.0, 0.0, 0.0});
}

// A recurrence threaded through ops the register machine has no
// instruction for -- ATAN2, a unary from the generated list, a cdf, an
// integer-outcome lpmf. Each compiles as a CALL to the graph's own
// kernel, so one op the machine cannot say stops ending the run, and
// the derivative is the kernel's own backward. The reference is the
// same graph uncarved: a CALL runs the identical kernel, so the graph
// is the arbiter, not the var replay (which cannot execute a CALL and
// never meets one).
static void test_kernel_call_ops_carved(bool compact) {
  if (!compact) test_setenv("STANLI_NO_ISLAND_COMPACT", "1", 1);
  Graph g;
  Fills fills;
  const int p0 = g.add_slot(1, true);
  const int p1 = g.add_slot(1, true);
  const int seed_vec = g.add_slot(2, true);
  const int seed = g.add_slot(1, false);
  g.add_op(OP_INDEX, {seed_vec}, seed, {0});
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  // INDEX compiles to an aliasable MOV before the first CALL. Compacting
  // that shared cell shifts the CALL's later ranges, so the assertions
  // below exercise mapped addressing rather than identity by accident.
  int acc = seed;
  for (int t = 0; t < 12; ++t) {
    // inv_logit keeps the recurrence in (0, 1), and tanh below goes negative.
    const int il = g.add_slot(1, false);
    g.add_op(OP_INV_LOGIT, {acc}, il);
    const int pw = g.add_slot(1, false);
    g.add_op(OP_ATAN2, {il, p1}, pw);  // out of vocabulary until CALL
    const int sq = g.add_slot(1, false);
    g.add_op(OP_SQUARE, {pw}, sq);
    const int lg = g.add_slot(1, false);
    g.add_op(OP_LGAMMA, {sq}, lg);  // generated-unary list
    const int sm = g.add_slot(1, false);
    g.add_op(OP_ADD, {lg, cslot(0.15 * t + 0.4)}, sm);
    const int cd = g.add_slot(1, false);
    g.add_op(OP_NORMAL_LCDF, {cslot(0.3), sm, cslot(2.0)}, cd);  // cdf
    const int pm = g.add_slot(1, false);
    {
      Op lp;  // poisson_lpmf(2 | exp-ish rate): int outcome rides in idata
      lp.opcode = OP_POISSON_LPMF;
      lp.n_in = 1;
      lp.in[0] = sq;
      lp.out = pm;
      lp.variant = 0x01;  // rate active; propto OFF
      g.idata_pool.push_back({2});
      lp.idata = g.idata_pool.back().data();
      lp.n_idata = 1;
      g.ops.push_back(lp);
    }
    const int nx = g.add_slot(1, false);
    g.add_op(OP_ADD, {cd, pm}, nx);
    const int th = g.add_slot(1, false);
    g.add_op(OP_TANHV, {nx}, th);
    acc = th;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_ADD, {acc, p0}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  const int carved = carve_islands(g, fills, terms, {});
  expect("callops carved==1", carved == 1);
  bool shifted_call_range = false;
  for (const Op& op : g.ops) {
    if (op.opcode != OP_ISLAND) continue;
    expect("call island dispatch variant", op.variant == kIslandCallVariant);
    const auto& p = *static_cast<const IslandProg*>(op.udata);
    for (const Program::Call& call : p.calls) {
      expect("call forward dispatch prebound", call.forward != nullptr);
      expect("call backward dispatch prebound", call.backward != nullptr);
      auto check_range = [&](int reg, int len) {
        if (len == 0) return;
        const int base = p.adj.adj_reg[(size_t)reg];
        expect("call compact base", base >= 0 && base + len <= p.adj.n_regs);
        for (int k = 1; k < len; ++k)
          expect("call compact contiguous",
                 p.adj.adj_reg[(size_t)(reg + k)] == base + k);
        if (base != reg) shifted_call_range = true;
      };
      for (int k = 0; k < call.n_in; ++k)
        check_range(call.in[k], call.in_len[k]);
      check_range(call.out, call.out_len);
    }
    const size_t call_instrs =
        std::count_if(p.code.begin(), p.code.end(),
                      [](const auto& I) { return I.code == Program::CALL; });
    expect("call reverse packet count", p.calls.size() == call_instrs);
    for (const Program::Call& call : p.calls)
      expect("call reverse packet prebound", call.backward != nullptr);
  }
  // With compaction on, the copies gen_adjoint used to share a cell for are
  // gone before it runs, so the mapping is an identity it never built.
  if (!compact) expect("call range actually compacted", shifted_call_range);
  const std::vector<double> got = run_grad_twice(std::move(g), fills);
  expect("callops sizes", got.size() == want.size());
  expect("callops forward/reverse bitwise",
         got.size() == want.size() &&
             std::memcmp(got.data(), want.data(),
                         got.size() * sizeof(double)) == 0);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("callops v" + std::to_string(i), got[i], want[i]);
  if (!compact) test_unsetenv("STANLI_NO_ISLAND_COMPACT");
}

static void test_too_many_live_ins() {
  Graph g;
  Fills fills;
  std::vector<int> ps;
  for (int k = 0; k < 7; ++k) ps.push_back(g.add_slot(1, true));
  int acc = ps[0];
  for (int k = 1; k < 7; ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD, {acc, ps[k]}, s);
    acc = s;
  }
  for (int k = 0; k < 40; ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_TANHV, {acc}, s);
    acc = s;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_SQUARE, {acc}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};
  const size_t before = g.ops.size();
  const int carved = carve_islands(g, fills, terms, {});
  expect("livein7 none carved", carved == 0);
  expect("livein7 ops unchanged", g.ops.size() == before);
}

static void test_six_live_ins_ok() {
  Graph g;
  Fills fills;
  std::vector<int> ps;
  for (int k = 0; k < 6; ++k) ps.push_back(g.add_slot(1, true));
  int acc = ps[0];
  for (int k = 1; k < 6; ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD, {acc, ps[k]}, s);
    acc = s;
  }
  for (int k = 0; k < 40; ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_TANHV, {acc}, s);
    acc = s;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_SQUARE, {acc}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  const int carved = carve_islands(g, fills, terms, {});
  expect("livein6 carved==1", carved == 1);
  const std::vector<double> got = run_grad(std::move(g), fills);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("livein6 v" + std::to_string(i), got[i], want[i]);
}

// Necessity-region lowering can need more logical live-ins than Op::in can
// name.  It concatenates a leading group into one graph descriptor, while the
// program retains the original register ranges and offsets.  Exercise both
// backward implementations through that exact mapping: the CONCAT2 backward
// must then distribute the packed adjoints to the original parameters.
static Graph build_packed_live_ins(bool native) {
  Graph g;
  std::vector<int> params;
  for (int k = 0; k < 7; ++k) params.push_back(g.add_slot(1, true));
  const int packed = g.add_slot(2, false);
  g.add_op(OP_CONCAT2, {params[0], params[1]}, packed);

  auto p = std::make_shared<IslandProg>();
  p->n_regs = 13;
  p->ins.push_back(IslandProg::LiveIn{0, 1, 0, 0});
  p->ins.push_back(IslandProg::LiveIn{1, 1, 0, 1});
  for (int k = 2; k < 7; ++k)
    p->ins.push_back(IslandProg::LiveIn{k, 1, k - 1, 0});
  int acc = 0;
  for (int k = 1; k < 7; ++k) {
    const int dst = 6 + k;
    p->code.push_back(Program::Instr{Program::ADD, dst, acc, k});
    acc = dst;
  }
  p->out_regs = {acc};
  expect("packed live-ins adjoint generated", gen_adjoint(*p));
  p->native_adj = native;

  Op island;
  island.opcode = OP_ISLAND;
  island.n_in = 6;
  island.in[0] = packed;
  for (int k = 1; k < 6; ++k) island.in[k] = params[k + 1];
  island.out = g.add_slot(1, false);
  island.udata = p.get();
  g.udata_pool.push_back(p);
  g.ops.push_back(island);
  g.result_slot = island.out;
  return g;
}

static void test_packed_live_ins() {
  double want_lp = 0.0;
  for (int k = 0; k < 7; ++k) want_lp += fill_at(k);
  for (bool native : {false, true}) {
    const std::vector<double> got = run_grad(build_packed_live_ins(native), {});
    expect("packed live-ins result width", got.size() == 8);
    if (got.size() != 8) continue;
    expect_close(native ? "packed native value" : "packed replay value", got[0],
                 want_lp);
    for (int k = 0; k < 7; ++k)
      expect_close(native ? "packed native gradient" : "packed replay gradient",
                   got[(size_t)k + 1], 1.0);
  }
}

// A slot PRODUCED BEFORE the region, then read and updated in place inside
// it, and read again after: it is a live-in and a live-out at once. If the
// island's extraction wrote that same slot, its adjoint buffer would hold
// two different quantities at once -- the extraction's backward leaves
// d(lp)/d(slot-after-region) there, and the island's backward then adds
// d(lp)/d(slot-before-region) on top. The producer's backward, which runs
// later in the reverse sweep, would read the sum and double-count. The
// extraction gets a fresh slot instead, so this checks the gradient with
// respect to the producer's own parameter.
static void test_live_in_and_out_slot() {
  Graph g;
  Fills fills;
  const int seedp = g.add_slot(1, true);  // feeds the producer
  const int a = g.add_slot(1, true);
  const int vec = g.add_slot(10, false);
  g.add_op(OP_REP_VEC, {seedp}, vec);  // the producer, before the region
  auto cslot = [&](double v) {
    const int s = g.add_slot(1, false);
    fills.emplace_back(s, std::vector<double>{v});
    return s;
  };
  // Each lane reads vec[k] (the producer's value), and writes vec[k]
  // destructively, exactly as an unrolled `x[k] = f(x[k])` loop lowers.
  for (int k = 0; k < 10; ++k) {
    const int e = g.add_slot(1, false);
    g.add_op(OP_INDEX, {vec}, e, {k});
    const int m = g.add_slot(1, false);
    g.add_op(OP_MUL, {e, a}, m);
    const int s1 = g.add_slot(1, false);
    g.add_op(OP_ADD, {m, cslot(0.2 * k)}, s1);
    const int t = g.add_slot(1, false);
    g.add_op(OP_TANHV, {s1}, t);
    Op si;  // destructive, as make_inplace_updates emits
    si.opcode = OP_SET_INDEX_INPLACE;
    si.n_in = 2;
    si.in[0] = vec;
    si.in[1] = t;
    si.out = vec;
    g.idata_pool.push_back({k});
    si.idata = g.idata_pool.back().data();
    si.n_idata = 1;
    g.ops.push_back(si);
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP, {vec}, lp);
  g.result_slot = lp;
  std::vector<int> terms{lp};

  Graph ref = g;
  const std::vector<double> want = run_grad(std::move(ref), fills);
  const int carved = carve_islands(g, fills, terms, {});
  expect("liveinout carved==1", carved == 1);
  expect("liveinout vec is live-in",
         g.ops[1].opcode == OP_ISLAND && g.ops[1].n_in >= 1);
  const std::vector<double> got = run_grad(std::move(g), fills);
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("liveinout v" + std::to_string(i), got[i], want[i]);
}

static Graph build_slice_island(int n_updates, int width,
                                std::vector<int>& terms) {
  Graph g;
  const int seed = g.add_slot(1, true);
  const int rhs = g.add_slot(2, true);
  const int vec = g.add_slot(width, false);
  g.add_op(OP_REP_VEC, {seed}, vec);  // vector-out barrier before the region
  for (int k = 0; k < n_updates; ++k)
    // Repeated overlapping windows exercise last-write-wins in the generated
    // adjoint: each later MOVR must consume the cells before an earlier one.
    g.add_op(OP_SET_SLICE_INPLACE, {vec, rhs}, vec, {k % 5});
  const int lp = g.add_slot(1, false);
  g.add_op(OP_LOG_SUM_EXP, {vec}, lp);
  g.result_slot = lp;
  terms.push_back(lp);
  return g;
}

static void test_inplace_slices_carved() {
  std::vector<int> ref_terms;
  Graph ref = build_slice_island(36, 40, ref_terms);
  const std::vector<double> want = run_grad(std::move(ref), {});

  std::vector<int> terms;
  Graph g = build_slice_island(36, 40, terms);
  Fills fills;
  expect("slice inplace island carved",
         carve_islands(g, fills, terms, {}) == 1);
  const std::vector<double> got = run_grad(std::move(g), {});
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    expect_close("slice island v" + std::to_string(i), got[i], want[i]);
}

// In-place slice cost is the RHS window, not the wide aliased output. If it
// were charged as a full-vector copy, this register-heavy region would look
// profitable and the carver would build a 4K-register island for tiny writes.
static void test_inplace_slice_cost_refuses_wide_state() {
  std::vector<int> terms;
  Graph g = build_slice_island(36, 4096, terms);
  Fills fills;
  const size_t before = g.ops.size();
  expect("wide inplace slices not carved",
         carve_islands(g, fills, terms, {}) == 0);
  expect("wide inplace slice graph unchanged", g.ops.size() == before);
}

// The hmm region's densities are `normal_lpdf(y | mu, sigma)` with y a fill
// slot and mu/sigma parameters, so the generated adjoint should ask
// stan-math for two partials out of three.
static std::vector<uint8_t> hmm_density_masks(Graph& g, const Fills& fills,
                                              const std::vector<int>& terms) {
  std::vector<uint8_t> masks;
  if (carve_islands(g, fills, terms, {}) != 1) return masks;
  for (const Op& op : g.ops) {
    if (op.opcode != OP_ISLAND) continue;
    for (const AdjInstr& I : static_cast<const IslandProg*>(op.udata)->adj.code)
      if (I.code == Program::DENSITY) masks.push_back(I.mask);
  }
  return masks;
}

static void test_density_mask_data_argument() {
  HmmGraph h = build_hmm(8);
  const std::vector<uint8_t> masks = hmm_density_masks(h.g, h.fills, h.terms);
  expect_eq("mask: densities differentiated", (int)masks.size(), 16);
  int wrong = 0;
  for (uint8_t m : masks)
    if (m != 0x6) ++wrong;
  expect_eq("mask: data outcome dropped", wrong, 0);
}

// STANLI_NO_DENSITY_MASK restores the all-active binding. ab_corpus.py's A
// side is built out of switches like this one.
static void test_density_mask_env_disable() {
  test_setenv("STANLI_NO_DENSITY_MASK", "1", 1);
  HmmGraph h = build_hmm(8);
  const std::vector<uint8_t> masks = hmm_density_masks(h.g, h.fills, h.terms);
  test_unsetenv("STANLI_NO_DENSITY_MASK");
  expect_eq("mask off: densities differentiated", (int)masks.size(), 16);
  int masked = 0;
  for (uint8_t m : masks)
    if (m != 0xf) ++masked;
  expect_eq("mask off: nothing dropped", masked, 0);
}

// The masks remove partials the executor discards, so lp and every gradient
// must come out to the bit -- not close, identical.
static void test_density_mask_gradient_identical() {
  HmmGraph on = build_hmm(8);
  expect("mask: carved", carve_islands(on.g, on.fills, on.terms, {}) == 1);
  const std::vector<double> got = run_grad(std::move(on.g), on.fills);
  test_setenv("STANLI_NO_DENSITY_MASK", "1", 1);
  HmmGraph off = build_hmm(8);
  expect("mask off: carved",
         carve_islands(off.g, off.fills, off.terms, {}) == 1);
  const std::vector<double> want = run_grad(std::move(off.g), off.fills);
  test_unsetenv("STANLI_NO_DENSITY_MASK");
  expect("mask: sizes", got.size() == want.size());
  int wrong = 0;
  for (size_t i = 0; i < want.size() && i < got.size(); ++i)
    if (got[i] != want[i]) ++wrong;
  expect_eq("mask: bitwise identical", wrong, 0);
}

// A parameter-dependent while loop holding poisson_log_lpmf and log2()
// against the same terms written outside one, where the flat path unrolls a
// data-bounded for. The trip count is 2 below zero and 3 above it.
static void test_while_lpmf_region_matches_flat() {
  const auto observations = [] {
    DataMap data;
    data.set_int("N", 3);
    data.set_int_array("y", {2, 0, 5});
    return data;
  };
  CompiledModel region = compile_model(
      slurp("tests/fixtures/while_lpmf_region.tmir.sexp"), observations());
  Executor region_ex(std::move(region.graph));
  region.bind(region_ex);

  for (double eta : {-0.3, -1.25, 0.4, 1.5}) {
    DataMap data = observations();
    data.set_int("reps", eta > 0 ? 3 : 2);
    CompiledModel flat =
        compile_model(slurp("tests/fixtures/while_lpmf_flat.tmir.sexp"), data);
    Executor flat_ex(std::move(flat.graph));
    flat.bind(flat_ex);

    const std::string tag = "while lpmf eta=" + std::to_string(eta);
    double region_grad = 0, flat_grad = 0;
    region_ex.params_data()[0] = eta;
    flat_ex.params_data()[0] = eta;
    const double region_lp = region_ex.gradient(&region_grad);
    const double flat_lp = flat_ex.gradient(&flat_grad);
    expect_exact(tag + " lp", region_lp, flat_lp);
    expect_exact(tag + " grad", region_grad, flat_grad);
  }
}

// Exercise both island buffer paths on short-lived workers, sharing graph
// payloads while each executor owns its scratch and each replay owns its vars.
static void test_worker_lifetimes() {
  for (bool native : {false, true}) {
    const Graph graph = build_packed_live_ins(native);
    std::vector<int> ok(4, 1);
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
      workers.emplace_back([&, t] {
        stan::math::ChainableStack tape;
        Executor ex(graph);
        for (int repeat = 0; repeat < 6; ++repeat) {
          for (int64_t i = 0; i < ex.n_params(); ++i)
            ex.params_data()[i] = t + repeat + 1;
          std::vector<double> grad((size_t)ex.n_params());
          const double value = ex.gradient(grad.data());
          ok[t] &= value == 7.0 * (t + repeat + 1);
          for (double derivative : grad) ok[t] &= derivative == 1.0;
        }
      });
    }
    for (auto& worker : workers) worker.join();
    for (int result : ok) expect("island worker values and gradients", result);
  }
}

int main() {
  test_worker_lifetimes();
  // What the compiler does with a region, on graphs small enough to
  // reason about. The cost estimate would refuse most of them -- it is
  // policy, tested separately below, and these are about correctness.
  test_branch_bound_live_out();
  test_while_lpmf_region_matches_flat();
  test_compact_copy_chain();
  test_compact_dead_fill();
  test_compact_rewritten_source_kept();
  test_compact_second_writer_kept();
  test_compact_range_copy();
  test_compact_forwards_producers_into_range();
  test_compact_destination_forwarding_refuses_input_alias();
  test_compact_destination_forwarding_refusals();
  test_compact_straddling_range_kept();
  test_compact_call_ranges();
  test_compact_env_disable();
  test_setenv("STANLI_ISLAND_ALWAYS", "1", 1);
  test_hmm_parity();
  test_env_disable();
  test_live_in_and_out_slot();
  test_inplace_slices_carved();
  test_short_run_untouched();
  test_propto_density_refused();
  test_unsupported_op_splits();
  test_too_many_live_ins();
  test_six_live_ins_ok();
  test_packed_live_ins();
  test_kernel_call_ops_carved(true);
  test_kernel_call_ops_carved(false);
  test_density_mask_data_argument();
  test_density_mask_env_disable();
  test_density_mask_gradient_identical();

  test_unsetenv("STANLI_ISLAND_ALWAYS");
  test_wide_state_refused();
  test_vector_copies_carved();
  test_softmax3_island_executor();
  test_softmax3_private_slot_stays_invalid_graph_ir();
  test_softmax3_payload_copy_lifetime();
  test_compact_adjoint_cost_boundary();
  test_scalar_chain_carved();
  test_native_extras_carved();
  test_pow_zero_base_carved();
  test_inplace_slice_cost_refuses_wide_state();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_island: all passed\n");
  return 0;
}
