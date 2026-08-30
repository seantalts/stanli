// Tape islands: a compiled region must match the op-by-op graph it
// replaces (values and gradients), and everything the carver cannot prove
// safe must stay untouched.
#include "env_helpers.hpp"
#include "graph_helpers.hpp"
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
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
static void expect_close(const std::string& what, double got, double want) {
  const double rel = std::abs(got - want) / std::max(std::abs(want), 1e-300);
  if (!(rel < 1e-12)) {
    ++failures;
    std::printf("FAIL %-24s got %.17g want %.17g rel %.2e\n", what.c_str(), got,
                want, rel);
  }
}
static void expect_bitwise(const std::string& what, double got, double want) {
  if (std::memcmp(&got, &want, sizeof(double)) != 0) {
    ++failures;
    std::printf("FAIL %-24s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

using namespace stanli;
using stanli::testutil::Fills;

static double fill_at(int64_t i) { return 0.3 + 0.15 * (i % 4); }
static std::vector<double> run_grad(Graph g, const Fills& fills) {
  return testutil::run_grad(std::move(g), fills, fill_at);
}

// The island kernel reuses its thread-local compact adjoint file. Running the
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
// wrong number. Runs first so the replay's thread_local register file is
// still empty, which is the state that makes that a null rather than a
// vari from an already-recovered nested tape.
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

// A recurrence threaded through ops the register machine has no
// instruction for -- POW, a unary from the generated list, a cdf, an
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
    // inv_logit keeps the recurrence in (0, 1): pow of a negative base at
    // a non-integer exponent is NaN, and tanh below goes negative.
    const int il = g.add_slot(1, false);
    g.add_op(OP_INV_LOGIT, {acc}, il);
    const int pw = g.add_slot(1, false);
    g.add_op(OP_POW, {il, p1}, pw);  // out of vocabulary until CALL
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

static Graph build_selector_island(double threshold) {
  Graph g;
  const int condition = g.add_slot(1, true);
  const int left = g.add_slot(1, true);
  const int right = g.add_slot(1, true);

  auto selector = std::make_shared<IslandProg>();
  selector->n_regs = 5;
  selector->ins = {{0, 1}, {1, 1}, {2, 1}};
  selector->pool = {threshold};
  selector->code = {
      {Program::CONST, 3, 0}, {Program::GT, 4, 0, 3}, {Program::JZ, 5, 4},
      {Program::MOV, 1, 2},   {Program::JMP, 5},
  };
  selector->out_regs = {1};
  expect("selector island recognized", supports_selector_adjoint(*selector));
  selector->selector_adj = true;

  Op island;
  island.opcode = OP_ISLAND;
  island.n_in = 3;
  island.in[0] = condition;
  island.in[1] = left;
  island.in[2] = right;
  island.out = g.add_slot(1, false);
  island.udata = selector.get();
  g.udata_pool.push_back(selector);
  g.ops.push_back(island);
  g.result_slot = island.out;
  return g;
}

static void test_selector_island_backward() {
  // Parameters are 0.30, 0.45, 0.60. The comparison itself has zero
  // derivative; the chosen value receives the live-out seed exactly once.
  const std::vector<double> skipped = run_grad(build_selector_island(0.4), {});
  expect("selector skipped result width", skipped.size() == 4);
  if (skipped.size() == 4) {
    expect_close("selector skipped value", skipped[0], 0.45);
    expect_close("selector skipped condition adj", skipped[1], 0.0);
    expect_close("selector skipped left adj", skipped[2], 1.0);
    expect_close("selector skipped right adj", skipped[3], 0.0);
  }

  const std::vector<double> taken = run_grad(build_selector_island(0.2), {});
  expect("selector taken result width", taken.size() == 4);
  if (taken.size() == 4) {
    expect_close("selector taken value", taken[0], 0.60);
    expect_close("selector taken condition adj", taken[1], 0.0);
    expect_close("selector taken left adj", taken[2], 0.0);
    expect_close("selector taken right adj", taken[3], 1.0);
  }
}

static Graph build_cfg_island(double threshold) {
  Graph g;
  const int condition = g.add_slot(1, true);
  const int x = g.add_slot(1, true);
  const int y = g.add_slot(1, true);

  auto program = std::make_shared<IslandProg>();
  program->n_regs = 7;
  program->ins = {{0, 1}, {1, 1}, {2, 1}};
  program->pool = {threshold};
  program->code = {
      {Program::CONST, 3, 0},  {Program::GT, 4, 0, 3}, {Program::JZ, 6, 4},
      {Program::MUL, 5, 1, 2}, {Program::MOV, 6, 5},   {Program::JMP, 8},
      {Program::SQUARE, 5, 1}, {Program::MOV, 6, 5},   {Program::ADD, 6, 6, 2},
  };
  program->out_regs = {6};
  expect("cfg island adjoint generated", gen_cfg_adjoint(*program));
  program->native_adj = true;

  Op island;
  island.opcode = OP_ISLAND;
  island.n_in = 3;
  island.in[0] = condition;
  island.in[1] = x;
  island.in[2] = y;
  island.out = g.add_slot(1, false);
  island.udata = program.get();
  g.udata_pool.push_back(program);
  g.ops.push_back(island);
  g.result_slot = island.out;
  return g;
}

static void test_cfg_island_backward() {
  // run_grad seeds the three parameters as 0.30, 0.45, 0.60.  These two
  // thresholds execute opposite paths through the same packed-trace kernel.
  const std::vector<double> taken = run_grad(build_cfg_island(0.2), {});
  expect("cfg island taken result width", taken.size() == 4);
  if (taken.size() == 4) {
    expect_close("cfg island taken value", taken[0], 0.45 * 0.60 + 0.60);
    expect_close("cfg island taken condition adj", taken[1], 0.0);
    expect_close("cfg island taken x adj", taken[2], 0.60);
    expect_close("cfg island taken y adj", taken[3], 1.45);
  }

  const std::vector<double> skipped = run_grad(build_cfg_island(0.4), {});
  expect("cfg island skipped result width", skipped.size() == 4);
  if (skipped.size() == 4) {
    expect_close("cfg island skipped value", skipped[0], 0.45 * 0.45 + 0.60);
    expect_close("cfg island skipped condition adj", skipped[1], 0.0);
    expect_close("cfg island skipped x adj", skipped[2], 0.90);
    expect_close("cfg island skipped y adj", skipped[3], 1.0);
  }
}

static Graph build_cfg_matrix_island(bool native, double threshold) {
  Graph g;
  const int condition = g.add_slot(1, true);
  const int matrix = g.add_slot(4, true);
  auto program = std::make_shared<IslandProg>();
  program->n_regs = 11;
  program->ins = {{0, 1}, {1, 4}};
  program->pool = {threshold};
  program->code = {{Program::CONST, 5, 0},
                   {Program::GT, 6, 0, 5},
                   {Program::MOVR, 7, 1, 0, 0, 4},
                   {Program::JZ, 5, 6},
                   {Program::MATRIX_EXP, 7, 1, 2, 2, 4}};
  program->out_regs = {7, 8, 9, 10};
  expect("cfg matrix island adjoint generated", gen_cfg_adjoint(*program));
  expect("cfg matrix island retains canonical replay",
         program->var_replay && program->var_replay->calls.empty() &&
             program->var_replay->code.back().code == Program::MATRIX_EXP);
  program->native_adj = native;

  Op island;
  island.opcode = OP_ISLAND;
  island.n_in = 2;
  island.in[0] = condition;
  island.in[1] = matrix;
  island.out = g.add_slot(4, false);
  island.udata = program.get();
  g.udata_pool.push_back(program);
  g.ops.push_back(island);
  const int result = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {island.out}, result);
  g.result_slot = result;
  return g;
}

static void test_cfg_matrix_replay_oracle() {
  for (double threshold : {0.2, 0.4}) {
    const std::vector<double> native =
        run_grad(build_cfg_matrix_island(true, threshold), {});
    const std::vector<double> replay =
        run_grad(build_cfg_matrix_island(false, threshold), {});
    expect("cfg structured native/replay result width",
           native.size() == replay.size());
    for (size_t k = 0; k < native.size() && k < replay.size(); ++k)
      expect("cfg structured native/replay bitwise", native[k] == replay[k]);
  }
}

struct SparseIslandGraph {
  Graph graph;
  std::shared_ptr<IslandProg> program;
};

static SparseIslandGraph build_sparse_alias_island(int chain) {
  SparseIslandGraph built;
  Graph& g = built.graph;
  const int input = g.add_slot(1, true);
  auto program = std::make_shared<IslandProg>();
  program->ins = {{0, 1}};
  int previous = 0;
  for (int i = 0; i < chain; ++i) {
    const int destination = i + 1;
    program->code.push_back({Program::NEG, destination, previous});
    previous = destination;
  }
  const int copy = chain + 1;
  program->code.push_back({Program::MOV, copy, previous});
  program->n_regs = copy + 1;
  // All three outputs share one compact cell: the MOV aliases its source and
  // the repeated output names it twice. Descending seeding must still sum all
  // three before the reverse sweep, while the clear plan names the cell once.
  program->out_regs = {previous, copy, copy};
  expect("sparse alias adjoint generated", gen_adjoint(*program));
  program->native_adj = true;

  Op island;
  island.opcode = OP_ISLAND;
  island.n_in = 1;
  island.in[0] = input;
  island.out = g.add_slot(3, false);
  island.udata = program.get();
  g.ops.push_back(island);
  g.udata_pool.push_back(program);
  const int result = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {island.out}, result);
  g.result_slot = result;
  built.program = std::move(program);
  return built;
}

static SparseIslandGraph build_sparse_cfg_island(double threshold, int chain) {
  SparseIslandGraph built;
  Graph& g = built.graph;
  const int condition = g.add_slot(1, true);
  const int x = g.add_slot(1, true);
  const int y = g.add_slot(1, true);
  auto program = std::make_shared<IslandProg>();
  program->ins = {{0, 1}, {1, 1}, {2, 1}};
  program->pool = {threshold};
  program->code = {{Program::CONST, 3, 0}, {Program::GT, 4, 0, 3},
                   {Program::JZ, 5, 4},    {Program::MUL, 5, 1, 2},
                   {Program::JMP, 6},      {Program::SQUARE, 5, 1},
                   {Program::ADD, 6, 5, 2}};
  int previous = 6;
  for (int i = 0; i < chain; ++i) {
    const int destination = 7 + i;
    program->code.push_back({Program::NEG, destination, previous});
    previous = destination;
  }
  program->n_regs = previous + 1;
  program->out_regs = {previous};
  expect("sparse cfg adjoint generated", gen_cfg_adjoint(*program));
  program->native_adj = true;

  Op island;
  island.opcode = OP_ISLAND;
  island.n_in = 3;
  island.in[0] = condition;
  island.in[1] = x;
  island.in[2] = y;
  island.out = g.add_slot(1, false);
  island.udata = program.get();
  g.ops.push_back(island);
  g.udata_pool.push_back(program);
  g.result_slot = island.out;
  built.program = std::move(program);
  return built;
}

static void expect_bitwise_vector(const char* what,
                                  const std::vector<double>& got,
                                  const std::vector<double>& want) {
  expect(what, got.size() == want.size());
  for (size_t i = 0; i < got.size() && i < want.size(); ++i)
    expect_bitwise(std::string(what) + " v" + std::to_string(i), got[i],
                   want[i]);
}

static std::vector<double> gradient_at(Executor& executor, double parameter) {
  executor.params_data()[0] = parameter;
  std::vector<double> result(2);
  result[0] = executor.gradient(result.data() + 1);
  return result;
}

static void test_sparse_native_reset() {
  test_setenv("STANLI_ISLAND_SPARSE_ADJ_RESET", "1", 1);

  SparseIslandGraph alias = build_sparse_alias_island(320);
  expect("sparse alias plan sorted",
         std::is_sorted(alias.program->sparse_adj_clear_cells.begin(),
                        alias.program->sparse_adj_clear_cells.end()));
  expect("sparse alias plan deduplicated",
         alias.program->sparse_adj_clear_cells.size() == 2);
  expect("sparse alias density admitted",
         alias.program->sparse_adj_clear_eligible);

  test_setenv("STANLI_NO_ISLAND_SPARSE_ADJ_RESET", "1", 1);
  const std::vector<double> full_alias =
      run_grad(build_sparse_alias_island(320).graph, {});
  test_unsetenv("STANLI_NO_ISLAND_SPARSE_ADJ_RESET");
  const std::vector<double> sparse_alias =
      run_grad_twice(std::move(alias.graph), {});
  expect_bitwise_vector("sparse alias/full", sparse_alias, full_alias);
  expect_close("sparse duplicate output value", sparse_alias[0], 0.9);
  expect_close("sparse duplicate output adjoint", sparse_alias[1], 3.0);

  // The same large forward-only CFG executes opposite traced paths. It also
  // writes register 5 on both arms, exercising repeated destinations.
  for (double threshold : {0.2, 0.4}) {
    SparseIslandGraph sparse = build_sparse_cfg_island(threshold, 320);
    expect("sparse cfg density admitted",
           sparse.program->sparse_adj_clear_eligible);
    expect("sparse cfg clear plan width",
           sparse.program->sparse_adj_clear_cells.size() == 4);
    test_setenv("STANLI_NO_ISLAND_SPARSE_ADJ_RESET", "1", 1);
    const std::vector<double> full =
        run_grad(build_sparse_cfg_island(threshold, 320).graph, {});
    test_unsetenv("STANLI_NO_ISLAND_SPARSE_ADJ_RESET");
    const std::vector<double> got = run_grad(std::move(sparse.graph), {});
    expect_bitwise_vector("sparse cfg/full", got, full);
  }

  // Alternate differently sized programs on one OS thread. A reset bounded
  // by the current smaller program would leave the larger TLS tail stale.
  const std::vector<double> large_first =
      run_grad(build_sparse_alias_island(640).graph, {});
  const std::vector<double> small =
      run_grad(build_sparse_cfg_island(0.2, 320).graph, {});
  const std::vector<double> large_second =
      run_grad(build_sparse_alias_island(640).graph, {});
  expect_bitwise_vector("sparse alternating large", large_second, large_first);
  expect_close("sparse alternating small value", small[0], 0.45 * 0.60 + 0.60);

  // Executor copies share immutable island payloads but own their arenas;
  // their native adjoint workspaces must remain independent TLS state.
  SparseIslandGraph threaded = build_sparse_alias_island(320);
  Executor base(std::move(threaded.graph));
  Executor left(base);
  Executor right(base);
  std::vector<double> left_result, right_result;
  bool left_ok = true, right_ok = true;
  std::thread left_thread([&] {
    try {
      left_result = gradient_at(left, 0.25);
    } catch (...) {
      left_ok = false;
    }
  });
  std::thread right_thread([&] {
    try {
      right_result = gradient_at(right, 0.5);
    } catch (...) {
      right_ok = false;
    }
  });
  left_thread.join();
  right_thread.join();
  expect("sparse copied executor left", left_ok && left_result.size() == 2);
  expect("sparse copied executor right", right_ok && right_result.size() == 2);
  if (left_result.size() == 2) {
    expect_close("sparse thread left value", left_result[0], 0.75);
    expect_close("sparse thread left adjoint", left_result[1], 3.0);
  }
  if (right_result.size() == 2) {
    expect_close("sparse thread right value", right_result[0], 1.5);
    expect_close("sparse thread right adjoint", right_result[1], 3.0);
  }

  test_unsetenv("STANLI_ISLAND_SPARSE_ADJ_RESET");
}

static thread_local bool sparse_test_throw_backward = false;

static void sparse_test_call_fwd(KernelCtx& ctx) {
  ctx.out.data[0] = ctx.in[0].data[0] + ctx.in[1].data[0];
}

static void sparse_test_call_bwd(KernelCtx& ctx) {
  if (sparse_test_throw_backward)
    throw std::runtime_error("injected sparse island backward failure");
  if (ctx.in_adj[0].data) ctx.in_adj[0].data[0] += ctx.out_adj_vec.data[0];
  if (ctx.in_adj[1].data) ctx.in_adj[1].data[0] += ctx.out_adj_vec.data[0];
}

static SparseIslandGraph build_sparse_throw_island(int chain) {
  SparseIslandGraph built;
  Graph& g = built.graph;
  const int left = g.add_slot(1, true);
  const int right = g.add_slot(1, true);
  auto program = std::make_shared<IslandProg>();
  program->ins = {{0, 1}, {1, 1}};
  Program::Call call;
  call.opcode = OP_NONE_;
  call.n_in = 2;
  call.in[0] = call.bwd_value_in[0] = 0;
  call.in[1] = call.bwd_value_in[1] = 1;
  call.in_len[0] = call.in_len[1] = 1;
  call.out = call.bwd_value_out = 2;
  call.out_len = 1;
  if (!bind_call(call)) {
    ++failures;
    std::printf("FAIL sparse CALL binds kernel\n");
  }
  program->calls.push_back(call);
  program->code.push_back({Program::CALL, 0, 0});
  int previous = 2;
  for (int i = 0; i < chain; ++i) {
    const int destination = 3 + i;
    program->code.push_back({Program::NEG, destination, previous});
    previous = destination;
  }
  program->n_regs = previous + 1;
  program->out_regs = {previous};
  expect("sparse throw adjoint generated", gen_adjoint(*program));
  program->native_adj = true;

  Op island;
  island.opcode = OP_ISLAND;
  island.n_in = 2;
  island.in[0] = left;
  island.in[1] = right;
  island.out = g.add_slot(1, false);
  island.udata = program.get();
  g.ops.push_back(island);
  g.udata_pool.push_back(program);
  g.result_slot = island.out;
  built.program = std::move(program);
  return built;
}

static void test_sparse_native_reset_exception_recovery() {
  (void)find_kernel(OP_ISLAND);  // Ensure the built-in table is initialized.
  const Kernel original = kernel(OP_NONE_);
  struct RestoreKernel {
    Kernel original;
    ~RestoreKernel() { register_kernel(OP_NONE_, original); }
  } restore{original};
  register_kernel(OP_NONE_,
                  Kernel{sparse_test_call_fwd, sparse_test_call_bwd, nullptr});
  test_setenv("STANLI_ISLAND_SPARSE_ADJ_RESET", "1", 1);

  SparseIslandGraph failing = build_sparse_throw_island(640);
  expect("sparse throw density admitted",
         failing.program->sparse_adj_clear_eligible);
  sparse_test_throw_backward = true;
  bool threw = false;
  try {
    (void)run_grad(std::move(failing.graph), {});
  } catch (const std::runtime_error&) {
    threw = true;
  }
  sparse_test_throw_backward = false;
  expect("sparse backward exception observed", threw);

  // Recovery must clear the whole prior allocation, even though this next
  // program is smaller; growing back afterward checks the old tail as well.
  const std::vector<double> recovered =
      run_grad(build_sparse_throw_island(320).graph, {});
  expect_close("sparse exception recovered value", recovered[0], 0.75);
  expect_close("sparse exception recovered left", recovered[1], 1.0);
  expect_close("sparse exception recovered right", recovered[2], 1.0);
  const std::vector<double> grown =
      run_grad(build_sparse_throw_island(640).graph, {});
  expect_bitwise_vector("sparse exception regrow", grown, recovered);

  test_unsetenv("STANLI_ISLAND_SPARSE_ADJ_RESET");
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

static void test_explicit_graph_fragment() {
  Graph g;
  const int x = g.add_slot(1, true);
  const int c = g.add_slot(1, false);
  const int sum = g.add_slot(1, false);
  g.add_op(OP_ADD, {x, c}, sum);
  const int square = g.add_slot(1, false);
  g.add_op(OP_SQUARE, {sum}, square);
  Fills fills{{c, {2.0}}};
  std::vector<uint8_t> active(g.slots.size(), 0);
  active[(size_t)x] = 1;
  active[(size_t)sum] = 1;
  active[(size_t)square] = 1;

  GraphFragmentProgram fragment;
  std::string why;
  expect("explicit fragment compiles",
         compile_graph_fragment(g, 0, 2, {square, sum}, fills, active,
                                &fragment, &why));
  expect("explicit fragment bypasses minimum",
         fragment.program.code.size() > 0);
  expect("explicit fragment absorbs fill",
         fragment.live_in_slots == std::vector<int>{x} &&
             fragment.program.pool.size() == 1);
  expect("explicit fragment applies activity",
         fragment.program.ins.size() == 1 && fragment.program.ins[0].active);
  expect("explicit fragment orders live-outs",
         fragment.program.out_regs.size() == 2);
  const double xv = 3.0;
  const double* inputs[] = {&xv};
  double outputs[2] = {0.0, 0.0};
  run_island<double>(fragment.program, inputs, outputs);
  expect_close("explicit fragment square", outputs[0], 25.0);
  expect_close("explicit fragment sum", outputs[1], 5.0);
}

static void test_explicit_fragment_many_live_ins() {
  Graph g;
  std::vector<int> inputs;
  for (int k = 0; k < 7; ++k) inputs.push_back(g.add_slot(1, true));
  int sum = inputs[0];
  for (int k = 1; k < 7; ++k) {
    const int next = g.add_slot(1, false);
    g.add_op(OP_ADD, {sum, inputs[(size_t)k]}, next);
    sum = next;
  }
  std::vector<uint8_t> active(g.slots.size(), 1);
  GraphFragmentProgram fragment;
  expect(
      "explicit fragment permits seven live-ins",
      compile_graph_fragment(g, 0, g.ops.size(), {sum}, {}, active, &fragment));
  expect_eq("explicit fragment seven live-in count",
            (int)fragment.live_in_slots.size(), 7);
}

static void test_explicit_fragment_container_calls() {
  Graph g;
  const int seed = g.add_slot(1, true);
  const int vector = g.add_slot(4, false);
  g.add_op(OP_REP_VEC, {seed}, vector);
  const int matrix = g.add_slot(6, false);
  g.add_op(OP_REP_MAT, {seed}, matrix, {2, 3, 0});
  std::vector<uint8_t> active(g.slots.size(), 0);
  active[(size_t)seed] = 1;

  GraphFragmentProgram fragment;
  expect(
      "container CALL fragment compiles",
      compile_graph_fragment(g, 0, 2, {matrix, vector}, {}, active, &fragment));
  expect("container CALL widths retained",
         fragment.program.calls.size() == 2 &&
             fragment.program.calls[0].out_len == 4 &&
             fragment.program.calls[1].out_len == 6 &&
             fragment.program.out_regs.size() == 10);
  expect("container CALL adjoint generated", !fragment.program.adj.empty());
  const double seed_value = 1.25;
  const double* inputs[] = {&seed_value};
  double outputs[10] = {};
  run_island<double>(fragment.program, inputs, outputs);
  int wrong = 0;
  for (double value : outputs)
    if (value != seed_value) ++wrong;
  expect_eq("container CALL values", wrong, 0);
}

static void test_explicit_fragment_shaped_direct_call() {
  Graph g;
  const int vector = g.add_slot(3, true);
  const int scalar = g.add_slot(1, true);
  const int sum = g.add_slot(3, false);
  g.add_op(OP_ADD, {vector, scalar}, sum);
  std::vector<uint8_t> active(g.slots.size(), 0);
  active[(size_t)vector] = 1;
  active[(size_t)scalar] = 1;

  GraphFragmentProgram fragment;
  expect("shaped direct op compiles as explicit CALL",
         compile_graph_fragment(g, 0, 1, {sum}, {}, active, &fragment));
  expect("shaped direct op preserves kernel widths",
         fragment.program.calls.size() == 1 &&
             fragment.program.calls[0].opcode == OP_ADD &&
             fragment.program.calls[0].in_len[0] == 3 &&
             fragment.program.calls[0].in_len[1] == 1 &&
             fragment.program.calls[0].out_len == 3);
  expect("shaped direct CALL adjoint generated", !fragment.program.adj.empty());
  const double values[] = {1.0, 2.0, 4.0};
  const double increment = 0.5;
  const double* inputs[] = {values, &increment};
  double outputs[3] = {};
  run_island<double>(fragment.program, inputs, outputs);
  expect_close("shaped direct CALL value 0", outputs[0], 1.5);
  expect_close("shaped direct CALL value 1", outputs[1], 2.5);
  expect_close("shaped direct CALL value 2", outputs[2], 4.5);
}

static void test_explicit_fragment_shaped_density_call() {
  Graph g;
  const int observations = g.add_slot(3, true);
  const int mean = g.add_slot(1, true);
  const int sigma = g.add_slot(1, false);
  const int density = g.add_slot(1, false);
  g.add_op(OP_NORMAL_LPDF, {observations, mean, sigma}, density);
  Fills fills{{sigma, {2.0}}};
  std::vector<uint8_t> active(g.slots.size(), 0);
  active[(size_t)observations] = 1;
  active[(size_t)mean] = 1;

  GraphFragmentProgram fragment;
  expect("shaped density compiles as explicit CALL",
         compile_graph_fragment(g, 0, 1, {density}, fills, active, &fragment));
  expect("shaped density preserves kernel widths",
         fragment.program.calls.size() == 1 &&
             fragment.program.calls[0].opcode == OP_NORMAL_LPDF &&
             fragment.program.calls[0].in_len[0] == 3 &&
             fragment.program.calls[0].in_len[1] == 1 &&
             fragment.program.calls[0].in_len[2] == 1 &&
             fragment.program.calls[0].out_len == 1);
  expect("shaped density CALL adjoint generated",
         !fragment.program.adj.empty());
  const double values[] = {-0.5, 0.25, 1.5};
  const double mean_value = 0.4;
  const double* inputs[] = {values, &mean_value};
  double output = 0.0;
  run_island<double>(fragment.program, inputs, &output);
  const std::vector<double> observations_value(values, values + 3);
  expect_close("shaped density CALL value", output,
               stan::math::normal_lpdf(observations_value, mean_value, 2.0));
}

static void test_explicit_fragment_nested_payload_owner() {
  auto inner = std::make_shared<IslandProg>();
  inner->n_regs = 2;
  inner->ins.push_back(IslandProg::LiveIn{0, 1, -1, 0, true});
  inner->code.push_back({Program::SQUARE, 1, 0});
  inner->out_regs.push_back(1);
  expect("nested payload adjoint", gen_adjoint(*inner));
  inner->native_adj = true;
  std::weak_ptr<void> lifetime = inner;

  Graph g;
  const int input = g.add_slot(1, true);
  const int output = g.add_slot(1, false);
  Op nested;
  nested.opcode = OP_ISLAND;
  nested.n_in = 1;
  nested.in[0] = input;
  nested.out = output;
  nested.udata = inner.get();
  g.ops.push_back(nested);
  g.udata_pool.push_back(inner);
  std::vector<uint8_t> active(g.slots.size(), 0);
  active[(size_t)input] = 1;

  GraphFragmentProgram fragment;
  expect("nested island fragment compiles",
         compile_graph_fragment(g, 0, 1, {output}, {}, active, &fragment));
  expect("nested island CALL retains payload",
         fragment.program.calls.size() == 1 &&
             fragment.program.calls[0].udata == inner.get() &&
             fragment.udata_owners.size() == 1);
  inner.reset();
  g.udata_pool.clear();
  expect("nested island owner survives source graph", !lifetime.expired());
  const double input_value = 4.0;
  const double* inputs[] = {&input_value};
  double result = 0.0;
  run_island<double>(fragment.program, inputs, &result);
  expect_close("nested island fragment value", result, 16.0);

  // Execute the generated fragment as a native outer island. Its CALL enters
  // the inner native island backward, exercising nested workspace depth and
  // ensuring the outer adjoint allocation remains intact while the inner
  // sweep runs.
  auto outer_payload =
      std::make_shared<IslandProg>(std::move(fragment.program));
  Graph outer_graph;
  const int outer_input = outer_graph.add_slot(1, true);
  const int outer_output = outer_graph.add_slot(1, false);
  Op outer_island;
  outer_island.opcode = OP_ISLAND;
  outer_island.n_in = 1;
  outer_island.in[0] = outer_input;
  outer_island.out = outer_output;
  outer_island.udata = outer_payload.get();
  outer_graph.ops.push_back(outer_island);
  for (auto& owner : fragment.udata_owners)
    outer_graph.udata_pool.push_back(std::move(owner));
  fragment.udata_owners.clear();
  outer_graph.udata_pool.push_back(outer_payload);
  outer_graph.result_slot = outer_output;
  Executor outer_executor(std::move(outer_graph));
  double outer_gradient = 0.0;
  *outer_executor.param_ptr(outer_input) = 4.0;
  expect_close("nested native island backward value",
               outer_executor.gradient(&outer_gradient), 16.0);
  expect_close("nested native island backward gradient", outer_gradient, 8.0);
}

static void test_explicit_fragment_refusals() {
  auto activity = [](const Graph& g) {
    return std::vector<uint8_t>(g.slots.size(), 0);
  };
  GraphFragmentProgram sentinel;
  sentinel.program.n_regs = 99;

  Graph effect;
  const int eout = effect.add_slot(1, false);
  Op rng;
  rng.opcode = OP_RNG;
  rng.out = eout;
  effect.ops.push_back(rng);
  expect("explicit fragment refuses RNG",
         !compile_graph_fragment(effect, 0, 1, {eout}, {}, activity(effect),
                                 &sentinel));
  expect_eq("refusal leaves result untouched", sentinel.program.n_regs, 99);

  Graph second;
  const int a = second.add_slot(1, true);
  const int b = second.add_slot(1, true);
  const int out = second.add_slot(1, false);
  const int out2 = second.add_slot(1, false);
  const int oi = second.add_op(OP_ADD, {a, b}, out);
  second.ops[(size_t)oi].out2 = out2;
  expect("explicit fragment refuses out2",
         !compile_graph_fragment(second, 0, 1, {out}, {}, activity(second),
                                 &sentinel));

  Graph scan;
  const int sout = scan.add_slot(1, false);
  Op scan_op;
  scan_op.opcode = OP_SCAN;
  scan_op.out = sout;
  scan.ops.push_back(scan_op);
  expect("explicit fragment refuses nested scan",
         !compile_graph_fragment(scan, 0, 1, {sout}, {}, activity(scan),
                                 &sentinel));
}

int main() {
  // What the compiler does with a region, on graphs small enough to
  // reason about. The cost estimate would refuse most of them -- it is
  // policy, tested separately below, and these are about correctness.
  test_branch_bound_live_out();
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
  test_explicit_graph_fragment();
  test_explicit_fragment_many_live_ins();
  test_explicit_fragment_container_calls();
  test_explicit_fragment_shaped_direct_call();
  test_explicit_fragment_shaped_density_call();
  test_explicit_fragment_nested_payload_owner();
  test_explicit_fragment_refusals();
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
  test_selector_island_backward();
  test_cfg_island_backward();
  test_cfg_matrix_replay_oracle();
  test_sparse_native_reset();
  test_sparse_native_reset_exception_recovery();
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
  test_inplace_slice_cost_refuses_wide_state();
  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_island: all passed\n");
  return 0;
}
