#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace stanli;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? std::numeric_limits<int64_t>::min() - i : i;
}

void expect_ulp(const std::string& what, double got, double want) {
  const int64_t d = std::llabs(ulp_key(got) - ulp_key(want));
  if (d > 2) {
    ++failures;
    std::printf("FAIL %-24s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
  }
}

struct Batch {
  std::vector<double> x;     // nb * inner_raw
  std::vector<double> seed;  // nb * inner_con
  double out2_adj;
};

struct RunResult {
  std::vector<double> in_adj;
  std::vector<double> out;
  double lp;
};

RunResult run_kernel(uint16_t opcode, const Batch& b, int64_t nb,
                     int64_t inner_raw, int64_t inner_con) {
  const Kernel* k = find_kernel(opcode);
  if (!k) throw std::runtime_error("missing kernel");
  RunResult r;
  r.in_adj.assign((size_t)(nb * inner_raw), 0.0);
  r.out.assign((size_t)(nb * inner_con), 0.0);
  std::vector<double> scratch((size_t)(nb * inner_raw), 0.0);
  int idata[3] = {(int)nb, (int)inner_raw, (int)inner_con};
  KernelCtx ctx;
  ctx.n_in = 1;
  ctx.in[0] = Desc{const_cast<double*>(b.x.data()), nb * inner_raw};
  ctx.out = Desc{r.out.data(), nb * inner_con};
  ctx.out2 = Desc{&r.lp, 1};
  ctx.scratch = scratch.data();
  ctx.idata = idata;
  ctx.n_idata = 3;
  ctx.in_adj[0] = Desc{r.in_adj.data(), nb * inner_raw};
  ctx.out_adj_vec = Desc{const_cast<double*>(b.seed.data()), nb * inner_con};
  ctx.out2_adj = b.out2_adj;
  k->forward(ctx);
  k->backward(ctx);
  return r;
}

using stan::math::var;

std::vector<double> reference_simplex(const Batch& b, int64_t nb,
                                      int64_t inner_raw, int64_t inner_con) {
  std::vector<double> adj((size_t)(nb * inner_raw));
  for (int64_t bi = 0; bi < nb; ++bi) {
    Eigen::Matrix<var, -1, 1> y(inner_raw);
    for (int64_t i = 0; i < inner_raw; ++i)
      y(i) = b.x[(size_t)(bi * inner_raw + i)];
    var lp = 0.0;
    auto theta = stan::math::simplex_constrain(y, lp);
    Eigen::Map<const Eigen::VectorXd> seed(b.seed.data() + bi * inner_con,
                                           inner_con);
    var obj = stan::math::dot_product(seed, theta) + b.out2_adj * lp;
    stan::math::grad(obj.vi_);
    for (int64_t i = 0; i < inner_raw; ++i)
      adj[(size_t)(bi * inner_raw + i)] = y(i).adj();
    stan::math::recover_memory();
  }
  return adj;
}

std::vector<double> reference_ordered(const Batch& b, int64_t nb,
                                      int64_t inner_raw, int64_t inner_con) {
  std::vector<double> adj((size_t)(nb * inner_raw));
  for (int64_t bi = 0; bi < nb; ++bi) {
    Eigen::Matrix<var, -1, 1> x(inner_raw);
    for (int64_t i = 0; i < inner_raw; ++i)
      x(i) = b.x[(size_t)(bi * inner_raw + i)];
    var lp = 0.0;
    auto y = stan::math::ordered_constrain(x, lp);
    Eigen::Map<const Eigen::VectorXd> seed(b.seed.data() + bi * inner_con,
                                           inner_con);
    var obj = stan::math::dot_product(seed, y) + b.out2_adj * lp;
    stan::math::grad(obj.vi_);
    for (int64_t i = 0; i < inner_raw; ++i)
      adj[(size_t)(bi * inner_raw + i)] = x(i).adj();
    stan::math::recover_memory();
  }
  return adj;
}

std::vector<double> reference_positive_ordered(const Batch& b, int64_t nb,
                                               int64_t inner_raw,
                                               int64_t inner_con) {
  std::vector<double> adj((size_t)(nb * inner_raw));
  for (int64_t bi = 0; bi < nb; ++bi) {
    Eigen::Matrix<var, -1, 1> x(inner_raw);
    for (int64_t i = 0; i < inner_raw; ++i)
      x(i) = b.x[(size_t)(bi * inner_raw + i)];
    var lp = 0.0;
    auto y = stan::math::positive_ordered_constrain(x, lp);
    Eigen::Map<const Eigen::VectorXd> seed(b.seed.data() + bi * inner_con,
                                           inner_con);
    var obj = stan::math::dot_product(seed, y) + b.out2_adj * lp;
    stan::math::grad(obj.vi_);
    for (int64_t i = 0; i < inner_raw; ++i)
      adj[(size_t)(bi * inner_raw + i)] = x(i).adj();
    stan::math::recover_memory();
  }
  return adj;
}

}  // namespace

int main() {
  const int64_t nb = 2, K = 4;

  // Gradient evaluation must use the scalar-var transform's values. The
  // double overload has packet tanh, so value-only evaluation keeps its own
  // reference. Include empty, scalar and packet-sized batches, and alternate
  // evaluation modes to catch retained mode state.
  for (int size : {0, 1, 2, 4, 8}) {
    const int raw = size * (size - 1) / 2, width = size * size;
    Graph g;
    const int input = g.add_slot(nb * raw, true);
    const int output = g.add_slot(nb * width, false);
    const int jac = g.add_slot(1, false);
    const int sum = g.add_slot(1, false);
    const int lp = g.add_slot(1, false);
    Op op;
    op.opcode = OP_CONSTRAIN_CHOL_CORR;
    op.in[0] = input;
    op.n_in = 1;
    op.out = output;
    op.out2 = jac;
    const int dims[] = {(int)nb, raw, size, size};
    op.idata = dims;
    op.n_idata = 4;
    g.ops.push_back(op);
    g.add_op(OP_SUM_VEC, {output}, sum);
    g.add_op(OP_ADD_N, {sum, jac}, lp);
    g.result_slot = lp;
    Executor ex(std::move(g));
    for (int i = 0; i < nb * raw; ++i)
      ex.param_ptr(input)[i] = 0.07 * ((i % 9) - 4);
    stan::math::nested_rev_autodiff nested;
    std::vector<double> rev_values, prim_values;
    var rev_lp = 0.0;
    double prim_lp = 0.0;
    for (int b = 0; b < nb; ++b) {
      Eigen::VectorXd y(raw);
      for (int i = 0; i < raw; ++i) y(i) = ex.param_ptr(input)[b * raw + i];
      Eigen::Matrix<var, -1, 1> vy = y;
      const auto v = stan::math::cholesky_corr_constrain(vy, size, rev_lp);
      const auto d = stan::math::cholesky_corr_constrain(y, size, prim_lp);
      for (int i = 0; i < width; ++i) {
        rev_values.push_back(v.data()[i].val());
        prim_values.push_back(d.data()[i]);
      }
    }
    for (bool value_only : {false, true, false}) {
      if (value_only)
        ex.forward_value_only();
      else
        ex.forward();
      const auto& values = value_only ? prim_values : rev_values;
      for (int i = 0; i < nb * width; ++i)
        check(ex.value_ptr(output)[i] == values[i], "Cholesky correlation forward value");
      check(ex.value_ptr(jac)[0] == (value_only ? prim_lp : rev_lp.val()),
            "Cholesky correlation forward Jacobian");
    }
  }

  {
    Batch b;
    b.x = {0.3, -0.8, 0.5, -0.2, 0.6, -0.4};
    b.seed = {0.1, -0.2, 0.3, 0.4, -0.5, 0.25, 0.05, -0.15};
    b.out2_adj = 0.37;
    const RunResult got = run_kernel(OP_CONSTRAIN_SIMPLEX, b, nb, K - 1, K);
    const std::vector<double> want = reference_simplex(b, nb, K - 1, K);
    for (int64_t i = 0; i < nb * (K - 1); ++i)
      expect_ulp("simplex in_adj[" + std::to_string(i) + "]",
                 got.in_adj[(size_t)i], want[(size_t)i]);
  }

  {
    Batch b;
    b.x = {0.2, -0.1, 0.4, -0.3, -0.5, 0.3, 0.1, -0.2};
    b.seed = {0.15, -0.25, 0.35, -0.05, 0.4, -0.1, 0.2, 0.3};
    b.out2_adj = -0.42;
    const RunResult got = run_kernel(OP_CONSTRAIN_ORDERED, b, nb, K, K);
    const std::vector<double> want = reference_ordered(b, nb, K, K);
    for (int64_t i = 0; i < nb * K; ++i)
      expect_ulp("ordered in_adj[" + std::to_string(i) + "]",
                 got.in_adj[(size_t)i], want[(size_t)i]);

    const Kernel* k = find_kernel(OP_CONSTRAIN_ORDERED);
    check(k != nullptr && k->scratch_size != nullptr,
          "ordered kernel declares a scratch hook");
  }

  {
    Batch b;
    b.x = {0.2, -0.1, 0.4, -0.3, -0.5, 0.3, 0.1, -0.2};
    b.seed = {0.15, -0.25, 0.35, -0.05, 0.4, -0.1, 0.2, 0.3};
    b.out2_adj = 0.19;
    const RunResult got = run_kernel(OP_CONSTRAIN_POS_ORDERED, b, nb, K, K);
    const std::vector<double> want = reference_positive_ordered(b, nb, K, K);
    for (int64_t i = 0; i < nb * K; ++i)
      expect_ulp("pos_ordered in_adj[" + std::to_string(i) + "]",
                 got.in_adj[(size_t)i], want[(size_t)i]);

    const Kernel* k = find_kernel(OP_CONSTRAIN_POS_ORDERED);
    check(k != nullptr && k->scratch_size != nullptr,
          "positive_ordered kernel declares a scratch hook");
  }

  if (failures == 0) std::printf("test_constrain_structured OK\n");
  return failures == 0 ? 0 : 1;
}
