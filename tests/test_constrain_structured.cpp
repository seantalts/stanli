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

void check_cholesky_reverse(int size, int batches, double magnitude,
                            double jacobian_seed, double value_seed) {
  const int raw = size * (size - 1) / 2, width = size * size;
  std::vector<double> input(batches * raw), seed(batches * width),
      output(batches * width), adjoint(batches * raw, 0.19);
  for (int i = 0; i < batches * raw; ++i)
    input[i] = magnitude * (0.1 + std::sin(0.73 * i));
  for (int i = 0; i < batches * width; ++i)
    seed[i] = value_seed * std::cos(0.31 * i);
  const int dims[] = {batches, raw, size, size};
  Op op;
  op.in[0] = 0;
  op.idata = dims;
  const Slot slot{0, batches * raw, true};
  const auto* kernel = find_kernel(OP_CONSTRAIN_CHOL_CORR);
  std::vector<double> scratch(kernel->scratch_size(op, &slot), 999.0);
  double jacobian = 0;
  KernelCtx ctx;
  ctx.n_in = 1;
  ctx.in[0] = {input.data(), batches * raw};
  ctx.in_adj[0] = {adjoint.data(), batches * raw};
  ctx.out = {output.data(), batches * width};
  ctx.out2 = {&jacobian, 1};
  ctx.out_adj_vec = {seed.data(), batches * width};
  ctx.out2_adj = jacobian_seed;
  ctx.idata = dims;
  ctx.n_idata = 4;
  ctx.scratch = scratch.data();
  try {
    kernel->forward(ctx);
  } catch (const std::exception& error) {
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<var, -1, 1> y(raw);
    for (int i = 0; i < raw; ++i) y[i] = input[i];
    var lp = 0.0;
    try {
      stan::math::cholesky_corr_constrain(y, size, lp);
      check(false, "Cholesky correlation unexpected forward rejection");
    } catch (const std::exception& reference) {
      check(std::string(error.what()) == reference.what(),
            "Cholesky correlation rejection matches upstream");
    }
    return;
  }
  const bool fast = scratch[0] == 1.0;
  if (magnitude == 0.0 || magnitude == 0.3)
    check(fast, "Cholesky correlation ordinary history uses retained reverse");
  if (batches && raw && std::isinf(magnitude))
    check(!fast, "Cholesky correlation saturation retains taped fallback");
  const auto arena_bytes =
      stan::math::ChainableStack::instance_->memalloc_.bytes_allocated();
  kernel->backward(ctx);
  if (fast)
    check(stan::math::ChainableStack::instance_->memalloc_.bytes_allocated() ==
              arena_bytes,
          "Cholesky correlation retained reverse does not grow Stan arena");
  const auto exact = [&](double got, double want, const std::string& what) {
    if (std::memcmp(&got, &want, sizeof(double)) &&
        !(std::isnan(got) && std::isnan(want))) {
      ++failures;
      if (failures < 25)
        std::printf(
            "FAIL chol size=%d batches=%d magnitude=%g jac=%g seed=%g %s %.17g "
            "!= %.17g\n",
            size, batches, magnitude, jacobian_seed, value_seed, what.c_str(),
            got, want);
    }
  };
  for (int b = 0; b < batches; ++b) {
    stan::math::nested_rev_autodiff nested;
    Eigen::Matrix<var, -1, 1> y(raw);
    for (int i = 0; i < raw; ++i) y[i] = input[b * raw + i];
    var lp = 0.0;
    auto x = stan::math::cholesky_corr_constrain(y, size, lp);
    Eigen::Matrix<var, -1, 1> flat(width);
    for (int i = 0; i < width; ++i) {
      flat[i] = x.data()[i];
      exact(output[b * width + i], flat[i].val(), "value");
    }
    Eigen::Map<const Eigen::VectorXd> weights(seed.data() + b * width, width);
    var objective = stan::math::dot_product(weights, flat) + jacobian_seed * lp;
    stan::math::grad(objective.vi_);
    for (int i = 0; i < raw; ++i)
      exact(adjoint[b * raw + i], 0.19 + y[i].adj(),
            "adjoint " + std::to_string(i));
  }
}

}  // namespace

int main() {
  check_cholesky_reverse(30, 2, 0.3, 0.37, -0.73);
  for (double exceptional : {std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
    check_cholesky_reverse(4, 2, 0.3, exceptional, -0.73);
    check_cholesky_reverse(4, 2, 0.3, 0.37, exceptional);
    check_cholesky_reverse(4, 2, exceptional, 0.37, -0.73);
  }
  for (int size : {0, 1, 2, 4, 8, 30})
    for (int batches : {0, 1, 3})
      for (double magnitude :
           {0.0, 0.3, 2.0, 18.0, std::numeric_limits<double>::infinity()})
        for (double jac : {0.0, -0.0, 1.0, -0.37, 1e-308, 1e308})
          for (double seed : {0.0, -0.73, 1e-308, 1e308})
            check_cholesky_reverse(size, batches, magnitude, jac, seed);
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
    std::vector<double> expected_gradient(nb * raw), reused_gradient(nb * raw);
    ex.gradient(expected_gradient.data());
    for (bool value_only : {false, true, false}) {
      if (value_only) {
        ex.forward_value_only();
      } else {
        ex.gradient(reused_gradient.data());
        check(reused_gradient == expected_gradient,
              "Cholesky correlation gradient after value-only reuse");
      }
      const auto& values = value_only ? prim_values : rev_values;
      for (int i = 0; i < nb * width; ++i)
        check(ex.value_ptr(output)[i] == values[i],
              "Cholesky correlation forward value");
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
