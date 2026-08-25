// The gp_regr multi_normal_cholesky fast path retains the pinned Stan Math
// single-vector pullback in scratch. Pin its exact value and gradient, reuse
// one Executor across value-only and gradient calls, and make every guard
// mutation fall back to the generic nested-var implementation.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using MatD = Eigen::MatrixXd;
using VecD = Eigen::VectorXd;
using VarM = Eigen::Matrix<stan::math::var, -1, -1>;
using VarV = Eigen::Matrix<stan::math::var, -1, 1>;

int failures = 0;

uint64_t bits(double x) {
  uint64_t out;
  std::memcpy(&out, &x, sizeof(out));
  return out;
}

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

void expect_bits(const std::string& what, double got, double want) {
  if (bits(got) != bits(want)) {
    ++failures;
    std::printf("FAIL %-42s got %.17g [%016llx] want %.17g [%016llx]\n",
                what.c_str(), got, (unsigned long long)bits(got), want,
                (unsigned long long)bits(want));
  }
}

struct Inputs {
  int n = 0;
  int m = 1;
  std::vector<double> y;
  std::vector<double> mu;
  std::vector<double> L;
};

Inputs inputs(int n, int m, double shift) {
  Inputs x;
  x.n = n;
  x.m = m;
  x.y.resize((size_t)n * m);
  x.mu.resize((size_t)n);
  x.L.assign((size_t)n * n, 0.0);
  for (int k = 0; k < m; ++k)
    for (int i = 0; i < n; ++i)
      x.y[(size_t)k * n + i] =
          std::sin(0.17 * (i + 1) * (k + 2) + shift) + 0.03 * i;
  for (int i = 0; i < n; ++i)
    x.mu[(size_t)i] = std::cos(0.11 * (i + 2) + shift) - 0.02 * i;
  for (int j = 0; j < n; ++j)
    for (int i = j; i < n; ++i)
      x.L[(size_t)j * n + i] =
          i == j ? 1.15 + 0.04 * i + 0.01 * shift
                 : 0.025 * std::sin(0.19 * (i + 1) * (j + 2) + shift);
  return x;
}

struct Result {
  double density = 0.0;
  double total = 0.0;
  std::vector<double> dy;
  std::vector<double> dmu;
  std::vector<double> dL;
};

template <bool Propto>
Result reference_l_only(const Inputs& x, double seed) {
  stan::math::nested_rev_autodiff nested;
  Eigen::Map<const VecD> y(x.y.data(), x.n);
  Eigen::Map<const VecD> mu(x.mu.data(), x.n);
  VarM L(x.n, x.n);
  for (int i = 0; i < x.n * x.n; ++i) L.data()[i] = x.L[(size_t)i];
  stan::math::var density =
      stan::math::multi_normal_cholesky_lpdf<Propto>(y, mu, L);
  stan::math::var total = density * seed;
  stan::math::grad(total.vi_);
  Result r;
  r.density = density.val();
  r.total = total.val();
  r.dy.assign(x.y.size(), 0.0);
  r.dmu.assign(x.mu.size(), 0.0);
  r.dL.resize(x.L.size());
  for (int i = 0; i < x.n * x.n; ++i) r.dL[(size_t)i] = L.data()[i].adj();
  return r;
}

template <bool Propto>
Result reference_all_active(const Inputs& x, double seed) {
  stan::math::nested_rev_autodiff nested;
  VarV y(x.n), mu(x.n);
  VarM L(x.n, x.n);
  for (int i = 0; i < x.n; ++i) {
    y(i) = x.y[(size_t)i];
    mu(i) = x.mu[(size_t)i];
  }
  for (int i = 0; i < x.n * x.n; ++i) L.data()[i] = x.L[(size_t)i];
  stan::math::var density =
      stan::math::multi_normal_cholesky_lpdf<Propto>(y, mu, L);
  stan::math::var total = density * seed;
  stan::math::grad(total.vi_);
  Result r;
  r.density = density.val();
  r.total = total.val();
  r.dy.resize(x.y.size());
  r.dmu.resize(x.mu.size());
  r.dL.resize(x.L.size());
  for (int i = 0; i < x.n; ++i) {
    r.dy[(size_t)i] = y(i).adj();
    r.dmu[(size_t)i] = mu(i).adj();
  }
  for (int i = 0; i < x.n * x.n; ++i) r.dL[(size_t)i] = L.data()[i].adj();
  return r;
}

template <bool Propto>
Result reference_vectorized_l(const Inputs& x, double seed) {
  stan::math::nested_rev_autodiff nested;
  std::vector<VecD> y((size_t)x.m, VecD(x.n));
  for (int k = 0; k < x.m; ++k)
    for (int i = 0; i < x.n; ++i) y[(size_t)k](i) = x.y[(size_t)k * x.n + i];
  Eigen::Map<const VecD> mu(x.mu.data(), x.n);
  VarM L(x.n, x.n);
  for (int i = 0; i < x.n * x.n; ++i) L.data()[i] = x.L[(size_t)i];
  stan::math::var density =
      stan::math::multi_normal_cholesky_lpdf<Propto>(y, mu, L);
  stan::math::var total = density * seed;
  stan::math::grad(total.vi_);
  Result r;
  r.density = density.val();
  r.total = total.val();
  r.dy.assign(x.y.size(), 0.0);
  r.dmu.assign(x.mu.size(), 0.0);
  r.dL.resize(x.L.size());
  for (int i = 0; i < x.n * x.n; ++i) r.dL[(size_t)i] = L.data()[i].adj();
  return r;
}

std::vector<double> sentinel(size_t n, double base) {
  std::vector<double> out(n);
  for (size_t i = 0; i < n; ++i)
    out[i] = base + 0.03125 * static_cast<double>(i % 7);
  return out;
}

Result run_direct(const Inputs& x, uint8_t variant, double seed,
                  const std::vector<double>& dy0,
                  const std::vector<double>& dmu0,
                  const std::vector<double>& dL0, bool connect_L = true) {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_MULTI_NORMAL_CHOL_LPDF);
  if (!kernel) throw std::runtime_error("MNC kernel missing");
  int dims[2] = {x.n, x.m};
  double density = std::numeric_limits<double>::quiet_NaN();
  // The generic fallback stashes one partial per input element, which is
  // more than the native pullback's n*n.
  std::vector<double> scratch(
      std::max<size_t>(1, x.y.size() + x.mu.size() + x.L.size()),
      std::numeric_limits<double>::quiet_NaN());
  Result r;
  r.dy = dy0;
  r.dmu = dmu0;
  r.dL = dL0;
  KernelCtx ctx;
  ctx.n_in = 3;
  ctx.in[0] = Desc{const_cast<double*>(x.y.data()), (int64_t)x.y.size()};
  ctx.in[1] = Desc{const_cast<double*>(x.mu.data()), (int64_t)x.mu.size()};
  ctx.in[2] = Desc{const_cast<double*>(x.L.data()), (int64_t)x.L.size()};
  ctx.out = Desc{&density, 1};
  ctx.variant = variant;
  ctx.scratch = scratch.data();
  ctx.idata = dims;
  ctx.n_idata = 2;
  ctx.in_adj[0] = Desc{r.dy.data(), (int64_t)r.dy.size()};
  ctx.in_adj[1] = Desc{r.dmu.data(), (int64_t)r.dmu.size()};
  ctx.in_adj[2] = Desc{connect_L ? r.dL.data() : nullptr, (int64_t)r.dL.size()};
  ctx.out_adj = seed;
  kernel->forward(ctx);
  kernel->backward(ctx);
  r.density = density;
  r.total = density * seed;
  return r;
}

void compare_direct(const std::string& tag, const Inputs& x, uint8_t variant,
                    double seed, const Result& want, bool y_active,
                    bool mu_active, bool L_active = true) {
  const std::vector<double> dy0 = sentinel(x.y.size(), 7.25);
  const std::vector<double> dmu0 = sentinel(x.mu.size(), -5.5);
  const std::vector<double> dL0 = sentinel(x.L.size(), 0.375);
  const Result got = run_direct(x, variant, seed, dy0, dmu0, dL0, L_active);
  expect_bits(tag + " density", got.density, want.density);
  expect_bits(tag + " total", got.total, want.total);
  for (size_t i = 0; i < got.dy.size(); ++i) {
    const double expected = y_active ? dy0[i] + want.dy[i] : dy0[i];
    expect_bits(tag + " dy[" + std::to_string(i) + "]", got.dy[i], expected);
  }
  for (size_t i = 0; i < got.dmu.size(); ++i) {
    const double expected = mu_active ? dmu0[i] + want.dmu[i] : dmu0[i];
    expect_bits(tag + " dmu[" + std::to_string(i) + "]", got.dmu[i], expected);
  }
  for (size_t i = 0; i < got.dL.size(); ++i)
    expect_bits(tag + " dL[" + std::to_string(i) + "]", got.dL[i],
                L_active ? dL0[i] + want.dL[i] : dL0[i]);
}

stanli::Graph fast_graph(int n) {
  using namespace stanli;
  Graph g;
  const int y = g.add_slot(n, false);
  const int mu = g.add_slot(n, false);
  const int L = g.add_slot(n * n, true);
  const int density = g.add_slot(1, false);
  const int seed = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  const int op =
      g.add_op(OP_MULTI_NORMAL_CHOL_LPDF, {y, mu, L}, density, {n, 1});
  g.ops[(size_t)op].variant = 0x84u;
  g.add_op(OP_MUL, {density, seed}, total);
  g.result_slot = total;
  return g;
}

class Runner {
 public:
  explicit Runner(int n) : n_(n), ex_(fast_graph(n)) {}

  Result gradient(const Inputs& x, double seed) {
    fill(x, seed);
    Result r;
    r.dL.resize((size_t)n_ * n_);
    r.total = ex_.gradient(r.dL.data());
    r.density = ex_.value_ptr(3)[0];
    return r;
  }

  Result value_only(const Inputs& x, double seed) {
    fill(x, seed);
    Result r;
    r.total = ex_.forward_value_only();
    r.density = ex_.value_ptr(3)[0];
    return r;
  }

 private:
  void fill(const Inputs& x, double seed) {
    std::copy(x.y.begin(), x.y.end(), ex_.value_ptr(0));
    std::copy(x.mu.begin(), x.mu.end(), ex_.value_ptr(1));
    std::copy(x.L.begin(), x.L.end(), ex_.value_ptr(2));
    ex_.value_ptr(4)[0] = seed;
  }

  int n_;
  stanli::Executor ex_;
};

void compare_runner(const std::string& tag, const Result& got,
                    const Result& want, bool gradient) {
  expect_bits(tag + " density", got.density, want.density);
  expect_bits(tag + " total", got.total, want.total);
  if (gradient)
    for (size_t i = 0; i < got.dL.size(); ++i)
      expect_bits(tag + " dL[" + std::to_string(i) + "]", got.dL[i],
                  want.dL[i]);
}

int64_t scratch_for(uint8_t variant, int n, int m, int64_t y_len,
                    int64_t mu_len, int64_t L_len, int64_t n_idata = 2) {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_MULTI_NORMAL_CHOL_LPDF);
  if (!kernel || !kernel->scratch_size) return -1;
  int dims[2] = {n, m};
  Slot slots[4];
  slots[0].len = y_len;
  slots[1].len = mu_len;
  slots[2].len = L_len;
  slots[3].len = 1;
  Op op;
  op.opcode = OP_MULTI_NORMAL_CHOL_LPDF;
  op.variant = variant;
  op.in[0] = 0;
  op.in[1] = 1;
  op.in[2] = 2;
  op.n_in = 3;
  op.out = 3;
  op.idata = dims;
  op.n_idata = n_idata;
  return kernel->scratch_size(op, slots);
}

template <typename F>
std::string thrown(F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

std::string native_error(const Inputs& x) {
  return thrown([&] {
    const auto dy = sentinel(x.y.size(), 1.0);
    const auto dm = sentinel(x.mu.size(), 2.0);
    const auto dL = sentinel(x.L.size(), 3.0);
    (void)run_direct(x, 0x84u, 0.7, dy, dm, dL);
  });
}

std::string reference_error(const Inputs& x) {
  return thrown([&] { (void)reference_l_only<true>(x, 0.7); });
}

// OP_MULTI_NORMAL_LPDF is the only shape that reaches mn_eval's covariance
// overload. Non-unit output adjoint: the kernel seeds its nested tape with
// 1.0 and scales in the backward.
void check_multi_normal(const std::string& tag, int n, int m, uint8_t variant,
                        double seed) {
  using namespace stanli;
  const Inputs x = inputs(n, m, 0.21);
  Eigen::Map<const MatD> Lm(x.L.data(), n, n);
  const MatD Sm = Lm * Lm.transpose();
  const unsigned mask = variant == 0 ? 0x7u : (variant & 0x3fu);
  const bool ay = mask & 1u, am = mask & 2u, aS = mask & 4u;
  const bool propto = (variant & 0x80u) != 0;

  Graph g;
  const int y = g.add_slot(n * m, ay);
  const int mu = g.add_slot(n, am);
  const int S = g.add_slot(n * n, aS);
  const int density = g.add_slot(1, false);
  const int seed_slot = g.add_slot(1, false);
  const int total = g.add_slot(1, false);
  const int op = g.add_op(OP_MULTI_NORMAL_LPDF, {y, mu, S}, density, {n, m});
  g.ops[(size_t)op].variant = variant;
  g.add_op(OP_MUL, {density, seed_slot}, total);
  g.result_slot = total;

  Executor ex(std::move(g));
  auto fill = [&](int slot, bool param, const double* src, int len) {
    double* p = param ? ex.param_ptr(slot) : ex.value_ptr(slot);
    std::copy(src, src + len, p);
  };
  fill(y, ay, x.y.data(), n * m);
  fill(mu, am, x.mu.data(), n);
  fill(S, aS, Sm.data(), n * n);
  ex.value_ptr(seed_slot)[0] = seed;
  std::vector<double> grad(
      (size_t)(ay ? n * m : 0) + (am ? n : 0) + (aS ? n * n : 0), 0.0);
  const double got = ex.gradient(grad.data());

  stan::math::nested_rev_autodiff nested;
  std::vector<VarV> yv((size_t)m, VarV(n));
  std::vector<VecD> yd((size_t)m, VecD(n));
  for (int k = 0; k < m; ++k)
    for (int i = 0; i < n; ++i) {
      yv[(size_t)k](i) = x.y[(size_t)k * n + i];
      yd[(size_t)k](i) = x.y[(size_t)k * n + i];
    }
  VarV muv(n);
  VarM Sv(n, n);
  for (int i = 0; i < n; ++i) muv(i) = x.mu[(size_t)i];
  for (int i = 0; i < n * n; ++i) Sv.data()[i] = Sm.data()[i];
  Eigen::Map<const VecD> mud(x.mu.data(), n);
  Eigen::Map<const MatD> Sd(Sm.data(), n, n);
  auto call = [&](auto&& a, auto&& b, auto&& c) {
    return propto ? stan::math::multi_normal_lpdf<true>(a, b, c)
                  : stan::math::multi_normal_lpdf<false>(a, b, c);
  };
  auto dispatch = [&](auto&& yy) {
    return aS ? (am ? call(yy, muv, Sv) : call(yy, mud, Sv))
              : (am ? call(yy, muv, Sd) : call(yy, mud, Sd));
  };
  stan::math::var ref;
  if (m > 1)
    ref = ay ? dispatch(yv) : dispatch(yd);
  else
    ref = ay ? dispatch(yv[0]) : dispatch(yd[0]);
  stan::math::var scaled = ref * seed;
  stan::math::grad(scaled.vi_);

  expect_bits(tag + " total", got, scaled.val());
  size_t at = 0;
  if (ay)
    for (int k = 0; k < m; ++k)
      for (int i = 0; i < n; ++i)
        expect_bits(tag + " dy" + std::to_string(k * n + i), grad[at++],
                    yv[(size_t)k](i).adj());
  if (am)
    for (int i = 0; i < n; ++i)
      expect_bits(tag + " dmu" + std::to_string(i), grad[at++], muv(i).adj());
  if (aS)
    for (int i = 0; i < n * n; ++i)
      expect_bits(tag + " dS" + std::to_string(i), grad[at++],
                  Sv.data()[i].adj());
}

}  // namespace

int main() {
  using namespace stanli;
  const double seed = -0.73;

  // Direct non-unit-seed calls pin every n*n partial (including the upper
  // triangle), += into prefilled adjoints, and non-null inactive y/mu adjoint
  // buffers. n=1 catches scalar-size Eigen branches; n=11 is gp_regr's
  // corpus dimension and exercises blocked matrix products.
  const Inputs one = inputs(1, 1, 0.2);
  compare_direct("native n=1", one, 0x84u, seed,
                 reference_l_only<true>(one, seed), false, false);
  compare_direct("native null L adj", one, 0x84u, seed,
                 reference_l_only<true>(one, seed), false, false, false);
  const Inputs eleven_a = inputs(11, 1, -0.4);
  const Inputs eleven_b = inputs(11, 1, 0.9);
  compare_direct("native n=11", eleven_a, 0x84u, seed,
                 reference_l_only<true>(eleven_a, seed), false, false);

  // Empty inputs return a connected-type literal zero without touching any
  // adjoint. Keep the native empty branch aligned with the same Stan Math
  // instantiation.
  const Inputs empty = inputs(0, 1, 0.0);
  compare_direct("native empty", empty, 0x84u, seed,
                 reference_l_only<true>(empty, seed), false, false);

  // A value-only call may leave old partials in scratch, but the next
  // gradient must run a complete forward and replace all of them.
  Runner runner(11);
  const Result ref_a = reference_l_only<true>(eleven_a, seed);
  const Result ref_b = reference_l_only<true>(eleven_b, seed);
  compare_runner("reuse gradient a", runner.gradient(eleven_a, seed), ref_a,
                 true);
  compare_runner("reuse value-only b", runner.value_only(eleven_b, seed), ref_b,
                 false);
  compare_runner("reuse gradient b", runner.gradient(eleven_b, seed), ref_b,
                 true);
  compare_runner("reuse gradient a again", runner.gradient(eleven_a, seed),
                 ref_a, true);

  // Exact scratch sizing is the fast-path sentinel: the native gate asks for
  // the pullback's n*n, every near miss asks for the fallback's one partial
  // per input element. These near misses are real supported contracts and
  // must continue through mn_eval: non-propto, another active argument,
  // vectorized y, legacy activity, the hier_2pl all-active variant, a missing
  // repetition immediate, or malformed slots.
  check(scratch_for(0x84u, 11, 1, 11, 11, 121) == 121,
        "native gate gets n*n scratch");
  check(scratch_for(0x04u, 11, 1, 11, 11, 121) == 143,
        "non-propto refuses native path");
  check(scratch_for(0x85u, 11, 1, 11, 11, 121) == 143,
        "active y refuses native path");
  check(scratch_for(0x84u, 11, 2, 22, 11, 121) == 154,
        "vectorized y refuses native path");
  check(scratch_for(0x00u, 11, 1, 11, 11, 121) == 143,
        "legacy variant refuses native path");
  check(scratch_for(0x07u, 11, 1, 11, 11, 121) == 143,
        "hier_2pl variant refuses native path");
  check(scratch_for(0x84u, 11, 1, 11, 11, 121, 1) == 143,
        "missing repetition immediate refuses native path");
  check(scratch_for(0x84u, 11, 1, 11, 10, 121) == 142,
        "malformed mu refuses native path");

  // Functional fallback oracles accompany the sizing sentinels: one changed
  // variant, the hier_2pl all-active contract (also legacy variant zero), and
  // the vectorized-y overload.
  const Inputs three = inputs(3, 1, 0.35);
  compare_direct("fallback non-propto", three, 0x04u, seed,
                 reference_l_only<false>(three, seed), false, false);
  compare_direct("fallback hier activity", three, 0x07u, seed,
                 reference_all_active<false>(three, seed), true, true);
  compare_direct("fallback legacy activity", three, 0x00u, seed,
                 reference_all_active<false>(three, seed), true, true);
  const Inputs vectorized = inputs(3, 2, -0.15);
  compare_direct("fallback vectorized", vectorized, 0x84u, seed,
                 reference_vectorized_l<true>(vectorized, seed), false, false);

  // Preserve the exact observable checks of the pinned single-vector
  // overload on invalid data as well as its finite path.
  Inputs bad_mu = inputs(3, 1, 0.1);
  bad_mu.mu[1] = std::numeric_limits<double>::infinity();
  const std::string native_mu = native_error(bad_mu);
  const std::string reference_mu = reference_error(bad_mu);
  check(!native_mu.empty(), "native rejects infinite mu");
  check(native_mu == reference_mu, "infinite mu error matches fallback");
  Inputs bad_y = inputs(3, 1, 0.1);
  bad_y.y[2] = std::numeric_limits<double>::quiet_NaN();
  const std::string native_y = native_error(bad_y);
  const std::string reference_y = reference_error(bad_y);
  check(!native_y.empty(), "native rejects NaN y");
  check(native_y == reference_y, "NaN y error matches fallback");

  check_multi_normal("mn all active", 3, 1, 0x07u, seed);
  check_multi_normal("mn sigma only propto", 3, 1, 0x84u, seed);
  check_multi_normal("mn legacy activity", 3, 1, 0x00u, seed);
  check_multi_normal("mn vectorized", 3, 2, 0x87u, seed);

  if (failures == 0) std::printf("test_mnc OK\n");
  return failures == 0 ? 0 : 1;
}
