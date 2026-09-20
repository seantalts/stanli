// Exact forward oracle: preserve the pinned Stan overload's scalar types,
// Eigen vector/matrix shape, validation order, and nonfinite behavior.
#include <stanli/optable.hpp>
#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using Mat = Eigen::MatrixXd;
using Var = stan::math::var;
enum class Kind { Plain, Spd, Tri };
static int failures = 0, cases = 0;

template <bool Left, bool Vector, typename T>
using Dividend = std::conditional_t<
    Vector,
    std::conditional_t<Left, Eigen::Matrix<T, -1, 1>, Eigen::Matrix<T, 1, -1>>,
    Eigen::Matrix<T, -1, -1>>;

template <bool Left, typename A, typename B>
Eigen::Matrix<Var, -1, -1> reference(Kind kind, const A& a, const B& b) {
  if constexpr (Left) {
    if (kind == Kind::Plain) return stan::math::mdivide_left(a, b);
    if (kind == Kind::Spd) return stan::math::mdivide_left_spd(a, b);
    return stan::math::mdivide_left_tri_low(a, b);
  } else {
    if (kind == Kind::Plain) return stan::math::mdivide_right(b, a);
    if (kind == Kind::Spd) return stan::math::mdivide_right_spd(b, a);
    return stan::math::mdivide_right_tri_low(b, a);
  }
}

struct Outcome {
  Mat value;
  std::string error;
};
template <class F>
Outcome evaluate(F&& f) {
  try {
    return {f(), {}};
  } catch (const std::domain_error& e) {
    return {{}, std::string("domain: ") + e.what()};
  } catch (const std::invalid_argument& e) {
    return {{}, std::string("invalid: ") + e.what()};
  }
}

template <bool Left, bool Vector, int Activity>
void compare(Kind kind, Mat a, Mat b, int fixture) {
  using namespace stanli;
  ++cases;
  const int n = a.rows(), k = Left ? b.cols() : b.rows();
  const int ai = Left ? 0 : 1, bi = Left ? 1 : 0;
  const uint16_t op =
      kind == Kind::Plain ? (Left ? OP_MDIVIDE_LEFT : OP_MDIVIDE_RIGHT)
      : kind == Kind::Spd
          ? (Left ? OP_MDIVIDE_LEFT_SPD : OP_MDIVIDE_RIGHT_SPD)
          : (Left ? OP_MDIVIDE_LEFT_TRI_LOW : OP_MDIVIDE_RIGHT_TRI_LOW);
  auto got = evaluate([&]() -> Mat {
    Mat result(b.rows(), b.cols());
    int dims[] = {n, k};
    KernelCtx ctx;
    ctx.n_in = 2;
    ctx.idata = dims;
    ctx.n_idata = 2;
    ctx.variant = 1u | (Vector ? 2u : 0u) | (Activity << 2);
    ctx.in[ai] = {a.data(), a.size()};
    ctx.in[bi] = {b.data(), b.size()};
    ctx.out = {result.data(), result.size()};
    const Kernel& kernel = *find_kernel(op);
    Op shape;
    shape.variant = ctx.variant;
    shape.idata = dims;
    shape.n_idata = 2;
    std::vector<double> scratch(
        kernel.scratch_size ? kernel.scratch_size(shape, nullptr) : 0);
    ctx.scratch = scratch.empty() ? nullptr : scratch.data();
    kernel.forward(ctx);
    return result;
  });
  auto want = evaluate([&]() -> Mat {
    stan::math::nested_rev_autodiff scope;
    using AT = std::conditional_t<Activity == 2, double, Var>;
    using BT = std::conditional_t<Activity == 1, double, Var>;
    const Eigen::Matrix<AT, -1, -1> av = a.cast<AT>();
    const Dividend<Left, Vector, BT> bv = b.cast<BT>();
    const auto result = reference<Left>(kind, av, bv);
    return stan::math::value_of(result);
  });
  bool ok = got.error == want.error;
  if (ok && got.error.empty()) {
    ok = got.value.rows() == want.value.rows() &&
         got.value.cols() == want.value.cols();
    for (int i = 0; ok && i < got.value.size(); ++i) {
      const double x = got.value.data()[i], y = want.value.data()[i];
      // NaN payloads are not a numerical contract. Finite values, infinities
      // and signed zero are compared by representation, with no ULP budget.
      ok = (std::isnan(x) && std::isnan(y)) ||
           std::memcmp(&x, &y, sizeof(double)) == 0;
      if (!ok && failures < 12)
        std::printf("  value[%d] %.17g != %.17g\n", i, x, y);
    }
  }
  if (!ok && ++failures <= 12)
    std::printf(
        "FAIL %s kind=%d vector=%d activity=%d n=%d k=%d fixture=%d\n"
        "  got %s\n  want %s\n",
        Left ? "left" : "right", int(kind), Vector, Activity, n, k, fixture,
        got.error.c_str(), want.error.c_str());
}

template <bool Left, bool Vector>
void activities(Kind kind, const Mat& a, const Mat& b, int fixture) {
  compare<Left, Vector, 0>(kind, a, b, fixture);  // legacy vv encoding
  compare<Left, Vector, 1>(kind, a, b, fixture);
  compare<Left, Vector, 2>(kind, a, b, fixture);
  compare<Left, Vector, 3>(kind, a, b, fixture);
}

static void both_sides(Kind kind, const Mat& a, const Mat& b, int fixture) {
  activities<true, false>(kind, a, b, fixture);
  activities<false, false>(kind, a, b.transpose(), fixture);
  if (b.cols() == 1) {
    activities<true, true>(kind, a, b, fixture);
    activities<false, true>(kind, a, b.transpose(), fixture);
  }
}

int main() {
  std::mt19937 rng(5183);
  std::uniform_real_distribution<double> draw(-0.4, 0.4);
  for (auto kind : {Kind::Plain, Kind::Spd, Kind::Tri})
    for (int n : {0, 1, 2, 5, 10, 20, 55})
      for (int k : {0, 1, 3, n})
        for (int repetition = 0; repetition < 2; ++repetition) {
          Mat a(n, n), b(n, k);
          for (int i = 0; i < a.size(); ++i) a.data()[i] = draw(rng);
          for (int i = 0; i < b.size(); ++i) b.data()[i] = draw(rng);
          if (kind == Kind::Spd) a = (a * a.transpose()).eval();
          a.diagonal().array() += n + 1.0;
          both_sides(kind, a, b, repetition);
        }

  for (auto kind : {Kind::Plain, Kind::Spd, Kind::Tri})
    for (int n : {1, 5, 10})
      for (int fixture = 0; fixture < 9; ++fixture) {
        Mat a(n, n), b(n, 1);
        for (int j = 0; j < n; ++j)
          for (int i = 0; i < n; ++i) a(i, j) = 1.0 / (i + j + 1.0);
        for (int i = 0; i < n; ++i) b(i, 0) = (i % 2 ? -1 : 1) * (i + 1);
        if (fixture == 1) a(0, 0) = -1;               // non-positive definite
        if (fixture == 2 && n > 1) a(0, 1) += 1e-10;  // accepted asymmetry
        if (fixture == 3 && n > 1) a(0, 1) += 0.5;    // rejected by SPD only
        if (fixture == 4) a(0, 0) = std::numeric_limits<double>::quiet_NaN();
        if (fixture == 5 && n > 1)
          a(0, 1) = std::numeric_limits<double>::quiet_NaN();  // ignored by tri
        if (fixture == 6) b(0, 0) = std::numeric_limits<double>::infinity();
        if (fixture == 7) {
          a.setIdentity();
          b.setConstant(-0.0);
        }
        if (fixture == 8) a.setZero();  // singular / division by zero
        both_sides(kind, a, b, 10 + fixture);
      }
  std::printf("test_solve_forwards: %d cases, %d failures\n", cases, failures);
  return failures ? 1 : 0;
}
