// Elementwise expression ops vs the stan-math var operations that stanc3's
// C++ backend would emit for the same MIR node. lp = sum(op(args)).
#include "graph_helpers.hpp"

#include <stan/math.hpp>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-24s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

using stan::math::var;
using VecV = Eigen::Matrix<var, -1, 1>;

static const std::vector<double> A{0.5, -1.2, 2.0, 0.3};
static const std::vector<double> B{1.5, 0.7, -0.4, 2.2};
static const double S = 0.8, T = -1.7;

static VecV mkv(const std::vector<double>& v) {
  VecV x(v.size());
  for (size_t i = 0; i < v.size(); ++i) x(i) = v[i];
  return x;
}

template <typename F>
static void check_case(const std::string& tag, uint16_t opcode, int64_t out_len,
                       const std::vector<std::vector<double>>& vals,
                       F&& ref_fn) {
  auto r = stanli::testutil::run_op_sum(opcode, out_len, vals,
                                        std::vector<bool>(vals.size(), true));
  // Reference: promote all inputs to var, apply ref_fn, sum, grad.
  std::vector<VecV> vs;
  for (const auto& v : vals) vs.push_back(mkv(v));
  var lp = ref_fn(vs);
  lp.grad();
  expect_eq(tag + " lp", r.value, lp.val());
  size_t gi = 0;
  for (auto& v : vs)
    for (int i = 0; i < v.size(); ++i)
      expect_eq(tag + " g" + std::to_string(gi), r.grad[gi], v(i).adj()), ++gi;
  stan::math::recover_memory();
}

int main() {
  using namespace stanli;
  const int N = 4;

  // Binary: all shape combos. Scalars are length-1 slots; the var reference
  // uses the scalar overloads (v(0)) exactly as generated C++ would.

  // ADD
  check_case("add vv", OP_ADD, N, {A, B}, [](auto& v) {
    return stan::math::sum(stan::math::add(v[0], v[1]));
  });
  check_case("add vs", OP_ADD, N, {A, {S}}, [](auto& v) {
    return stan::math::sum(stan::math::add(v[0], v[1](0)));
  });
  check_case("add sv", OP_ADD, N, {{S}, B}, [](auto& v) {
    return stan::math::sum(stan::math::add(v[0](0), v[1]));
  });
  check_case("add ss", OP_ADD, 1, {{S}, {T}}, [](auto& v) {
    return v[0](0) + v[1](0);
  });
  // SUB
  check_case("sub vv", OP_SUB, N, {A, B}, [](auto& v) {
    return stan::math::sum(stan::math::subtract(v[0], v[1]));
  });
  check_case("sub vs", OP_SUB, N, {A, {S}}, [](auto& v) {
    return stan::math::sum(stan::math::subtract(v[0], v[1](0)));
  });
  check_case("sub sv", OP_SUB, N, {{S}, B}, [](auto& v) {
    return stan::math::sum(stan::math::subtract(v[0](0), v[1]));
  });
  check_case("sub ss", OP_SUB, 1, {{S}, {T}}, [](auto& v) {
    return v[0](0) - v[1](0);
  });
  // MUL (vv = elt_multiply, matching EltTimes__)
  check_case("mul vv", OP_MUL, N, {A, B}, [](auto& v) {
    return stan::math::sum(stan::math::elt_multiply(v[0], v[1]));
  });
  check_case("mul vs", OP_MUL, N, {A, {S}}, [](auto& v) {
    return stan::math::sum(stan::math::multiply(v[0], v[1](0)));
  });
  check_case("mul sv", OP_MUL, N, {{S}, B}, [](auto& v) {
    return stan::math::sum(stan::math::multiply(v[0](0), v[1]));
  });
  check_case("mul ss", OP_MUL, 1, {{S}, {T}}, [](auto& v) {
    return v[0](0) * v[1](0);
  });
  // DIV (vv = elt_divide, vs = divide)
  check_case("div vv", OP_DIV, N, {A, B}, [](auto& v) {
    return stan::math::sum(stan::math::elt_divide(v[0], v[1]));
  });
  check_case("div vs", OP_DIV, N, {A, {T}}, [](auto& v) {
    return stan::math::sum(stan::math::divide(v[0], v[1](0)));
  });
  check_case("div ss", OP_DIV, 1, {{S}, {T}}, [](auto& v) {
    return v[0](0) / v[1](0);
  });
  // POW (ss)
  check_case("pow ss", OP_POW, 1, {{S}, {T}}, [](auto& v) {
    return stan::math::pow(v[0](0), v[1](0));
  });

  // Unaries, vector + scalar shapes.
  check_case("neg v", OP_NEG, N, {A}, [](auto& v) {
    return stan::math::sum(stan::math::minus(v[0]));
  });
  check_case("exp v", OP_EXPV, N, {A}, [](auto& v) {
    return stan::math::sum(stan::math::exp(v[0]));
  });
  check_case("exp s", OP_EXPV, 1, {{S}}, [](auto& v) {
    return stan::math::exp(v[0](0));
  });
  check_case("log v", OP_LOGV, N, {{1.5, 0.7, 0.4, 2.2}}, [](auto& v) {
    return stan::math::sum(stan::math::log(v[0]));
  });
  check_case("inv_logit v", OP_INV_LOGIT, N, {A}, [](auto& v) {
    return stan::math::sum(stan::math::inv_logit(v[0]));
  });
  check_case("sqrt v", OP_SQRT, N, {{0.5, 1.2, 2.0, 0.3}}, [](auto& v) {
    return stan::math::sum(stan::math::sqrt(v[0]));
  });
  check_case("square v", OP_SQUARE, N, {A}, [](auto& v) {
    return stan::math::sum(stan::math::square(v[0]));
  });
  check_case("log1m v", OP_LOG1M, N, {{0.2, -0.5, 0.7, 0.05}}, [](auto& v) {
    return stan::math::sum(stan::math::log1m(v[0]));
  });
  // DOT
  check_case("dot", OP_DOT, 1, {A, B}, [](auto& v) {
    return stan::math::dot_product(v[0], v[1]);
  });

  if (failures == 0) std::printf("test_eltwise OK\n");
  return failures == 0 ? 0 : 1;
}
