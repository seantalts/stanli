// Batched OP_LSE2 / OP_LOG_MIX: any argument len 1 (broadcast) or len N,
// out len N, out[n] = f(args at n). Two obligations:
//
//  1. Value and gradient match the stan-math var reference (the C++ stanc3
//     would generate for the source loop), exactly.
//  2. The fused op is bit-identical to the unrolled graph it will replace
//     under re-rolling: INDEX -> scalar op -> SET_INDEX chain -> SUM_VEC.
//     This includes the broadcast-adjoint accumulation order.
#include "graph_helpers.hpp"

#include <stan/math.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace stanli;

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-28s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

using stan::math::var;

static const std::vector<double> TH{0.35, 0.62, 0.18, 0.91};
static const std::vector<double> A{-1.3, 0.4, 2.1, -0.7};
static const std::vector<double> B{0.9, -2.2, 1.5, 0.1};
static const double S = 0.55, U = -0.8, W = 1.2;

// ---- 1. Against the var reference -----------------------------------------
// vals[k] is len 1 or len N; ref applies the scalar stan-math function per
// element (broadcasting len-1 args), accumulating lp ascending like SUM_VEC.
template <typename F>
static void check_ref(const std::string& tag, uint16_t opcode, int64_t out_len,
                      const std::vector<std::vector<double>>& vals, F&& f) {
  auto r = stanli::testutil::run_op_sum(opcode, out_len, vals,
                                        std::vector<bool>(vals.size(), true));
  std::vector<std::vector<var>> vs;
  for (const auto& v : vals) vs.emplace_back(v.begin(), v.end());
  var lp = 0.0;
  for (int64_t n = 0; n < out_len; ++n) {
    std::vector<var> a;
    for (auto& v : vs) a.push_back(v[v.size() == 1 ? 0 : n]);
    lp += f(a);
  }
  lp.grad();
  expect_eq(tag + " lp", r.value, lp.val());
  size_t gi = 0;
  for (auto& v : vs)
    for (auto& x : v)
      expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], x.adj()), ++gi;
  stan::math::recover_memory();
}

// ---- 2. Against the unrolled graph ----------------------------------------
// Reference: the shape lowering emits for
//   for (n in 1:N) outvec[n] = f(args...[n]);  target += sum(outvec);
// i.e. per element INDEX each vector arg, run the scalar op, SET_INDEX into
// a threaded vector; then SUM_VEC. Fused: one op with a len-N out, SUM_VEC.
struct GraphRun {
  double value;
  std::vector<double> out;  // the len-N intermediate
  std::vector<double> grad;
};

static GraphRun run_fused(uint16_t opcode, int64_t n,
                          const std::vector<std::vector<double>>& vals) {
  stanli::Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (const auto& v : vals) {
    slots.push_back(g.add_slot((int64_t)v.size(), true));
    n_par += (int64_t)v.size();
  }
  const int out = g.add_slot(n, false);
  const int lp = g.add_slot(1, false);
  stanli::Op op;
  op.opcode = opcode;
  op.out = out;
  for (int s : slots) op.in[op.n_in++] = s;
  g.ops.push_back(op);
  g.add_op(OP_SUM_VEC, {out}, lp);
  g.result_slot = lp;

  stanli::Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i)
    for (size_t j = 0; j < vals[i].size(); ++j)
      ex.value_ptr(slots[i])[j] = vals[i][j];
  GraphRun r;
  r.grad.assign(n_par, 0.0);
  r.value = ex.gradient(r.grad.data());
  r.out.assign(ex.value_ptr(out), ex.value_ptr(out) + n);
  return r;
}

static GraphRun run_unrolled(uint16_t opcode, int64_t n,
                             const std::vector<std::vector<double>>& vals) {
  stanli::Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (const auto& v : vals) {
    slots.push_back(g.add_slot((int64_t)v.size(), true));
    n_par += (int64_t)v.size();
  }
  int vec = g.add_slot(n, false);  // zeros; SET_INDEX chain threads through
  for (int64_t i = 0; i < n; ++i) {
    std::vector<int> args;
    for (int s : slots) {
      if (g.slots[s].len == 1) {
        args.push_back(s);
      } else {
        const int e = g.add_slot(1, false);
        g.add_op(OP_INDEX, {s}, e, {(int)i});
        args.push_back(e);
      }
    }
    const int fi = g.add_slot(1, false);
    stanli::Op op;
    op.opcode = opcode;
    op.out = fi;
    for (int s : args) op.in[op.n_in++] = s;
    g.ops.push_back(op);
    const int next = g.add_slot(n, false);
    g.add_op(OP_SET_INDEX, {vec, fi}, next, {(int)i});
    vec = next;
  }
  const int lp = g.add_slot(1, false);
  g.add_op(OP_SUM_VEC, {vec}, lp);
  g.result_slot = lp;

  stanli::Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i)
    for (size_t j = 0; j < vals[i].size(); ++j)
      ex.value_ptr(slots[i])[j] = vals[i][j];
  GraphRun r;
  r.grad.assign(n_par, 0.0);
  r.value = ex.gradient(r.grad.data());
  r.out.assign(ex.value_ptr(vec), ex.value_ptr(vec) + n);
  return r;
}

static void check_bitwise(const std::string& tag, uint16_t opcode, int64_t n,
                          const std::vector<std::vector<double>>& vals) {
  const GraphRun f = run_fused(opcode, n, vals);
  const GraphRun u = run_unrolled(opcode, n, vals);
  expect_eq(tag + " lp", f.value, u.value);
  for (int64_t i = 0; i < n; ++i)
    expect_eq(tag + " out" + std::to_string(i), f.out[i], u.out[i]);
  for (size_t i = 0; i < f.grad.size(); ++i)
    expect_eq(tag + " g" + std::to_string(i), f.grad[i], u.grad[i]);
}

int main() {
  const int64_t N = 4;

  // -- var reference, every shape combination --
  auto lse2 = [](const std::vector<var>& a) {
    return stan::math::log_sum_exp(a[0], a[1]);
  };
  check_ref("lse2 vv", OP_LSE2, N, {A, B}, lse2);
  check_ref("lse2 vs", OP_LSE2, N, {A, {S}}, lse2);
  check_ref("lse2 sv", OP_LSE2, N, {{S}, B}, lse2);
  check_ref("lse2 ss", OP_LSE2, 1, {{S}, {U}}, lse2);

  auto lmix = [](const std::vector<var>& a) {
    return stan::math::log_mix(a[0], a[1], a[2]);
  };
  check_ref("log_mix vvv", OP_LOG_MIX, N, {TH, A, B}, lmix);
  check_ref("log_mix svv", OP_LOG_MIX, N, {{S}, A, B}, lmix);
  check_ref("log_mix vss", OP_LOG_MIX, N, {TH, {U}, {W}}, lmix);
  check_ref("log_mix sss", OP_LOG_MIX, 1, {{S}, {U}, {W}}, lmix);

  // -- bit-identical to the unrolled loop it replaces --
  check_bitwise("unroll lse2 vv", OP_LSE2, N, {A, B});
  check_bitwise("unroll lse2 vs", OP_LSE2, N, {A, {S}});
  check_bitwise("unroll lmix svv", OP_LOG_MIX, N, {{S}, A, B});
  check_bitwise("unroll lmix vvv", OP_LOG_MIX, N, {TH, A, B});

  if (failures) {
    std::printf("%d failures\n", failures);
    return 1;
  }
  std::printf("test_mixture: all passed\n");
  return 0;
}
