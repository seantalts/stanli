// Differential checks against the previously used nested Stan Math tape.
#include <stanli/graph.hpp>
#include <stanli/gp_cov_fusion.hpp>
#include <stanli/optable.hpp>
#include <stan/math.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>
#include <string>

using Mat = Eigen::MatrixXd;
using Vec = Eigen::VectorXd;
using Var = stan::math::var;
using VarVec = Eigen::Matrix<Var, -1, 1>;
static int failures = 0;
static void check(double got, double want, const char* tag,
                  bool exact = false) {
  if (got == want || (std::isnan(got) && std::isnan(want))) return;
  // GP summation order changes; Cholesky reuses the exact pinned pullback.
  if (!exact && std::isfinite(got) && std::isfinite(want) &&
      std::abs(got - want) <= 2e-12 * (1 + std::abs(want)))
    return;
  if (++failures < 12) std::printf("FAIL %s %.17g != %.17g\n", tag, got, want);
}

static int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? std::numeric_limits<int64_t>::min() - i : i;
}
// A native pullback's own gate: bitwise, with a stated ULP allowance when
// it reassociates or -- as with matrix_exp's block-exponential identity --
// runs a different algorithm than the nested-tape replay it replaces.
static void expect_ulp(const std::string& what, double got, double want,
                       int64_t max_ulp) {
  if (got == want || (std::isnan(got) && std::isnan(want))) return;
  const int64_t dist = std::llabs(ulp_key(got) - ulp_key(want));
  if (dist > max_ulp) {
    if (++failures < 12)
      std::printf("FAIL %-28s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                  got, want, (long long)dist);
  }
}
static void gp(int n, int d, int active, uint8_t variant, bool repeated,
               double rho = 0.7, double sigma = 1.3, double seed_scale = 0.7,
               bool diagonal_update = false) {
  using namespace stanli;
  std::vector<double> x(n * d), xadj(n * d, 0.125), out(n * n), seed(n * n);
  double sadj = 0.125, radj = 0.125;
  double jitter = 1e-12, jadj = 0.125;
  for (int i = 0; i < n * d; ++i) x[i] = repeated ? 0.2 : std::sin(0.31 * i);
  for (int i = 0; i < n * n; ++i) seed[i] = std::cos(0.43 * i) * seed_scale;
  KernelCtx ctx;
  int dims[] = {n, d};
  ctx.n_in = diagonal_update ? 4 : 3;
  ctx.idata = dims;
  ctx.n_idata = 2;
  ctx.variant = variant;
  ctx.in[0] = {x.data(), n * d};
  ctx.in[1] = {&sigma, 1};
  ctx.in[2] = {&rho, 1};
  ctx.in_adj[0] = {active & 1 ? xadj.data() : nullptr, n * d};
  ctx.in_adj[1] = {active & 2 ? &sadj : nullptr, 1};
  ctx.in_adj[2] = {active & 4 ? &radj : nullptr, 1};
  ctx.in[3] = {&jitter, 1};
  ctx.in_adj[3] = {active & 8 ? &jadj : nullptr, 1};
  ctx.out = {out.data(), n * n};
  ctx.out_adj_vec = {seed.data(), n * n};
  const Kernel& k = *find_kernel(OP_GP_COV);
  k.forward(ctx);
  k.backward(ctx);
  if (n == 0) {
    // The empty covariance has no contributions. Stan Math's Matrix<var>
    // sum oracle binds a null element reference for a 0x0 matrix under UBSan.
    check(sadj, 0.125, "empty GP sigma", true);
    check(radj, 0.125, "empty GP rho", true);
    return;
  }
  stan::math::nested_rev_autodiff scope;
  std::vector<VarVec> points(n, VarVec(d));
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < d; ++j) points[i][j] = x[i * d + j];
  Var a = sigma, r = rho;
  Eigen::Matrix<Var, -1, -1> cov;
  const auto covariance = [&](const auto& locations) {
    switch (variant) {
      case kGpMatern32:
        return stan::math::gp_matern32_cov(locations, a, r);
      case kGpMatern52:
        return stan::math::gp_matern52_cov(locations, a, r);
      case kGpExponential:
        return stan::math::gp_exponential_cov(locations, a, r);
      default:
        return stan::math::gp_exp_quad_cov(locations, a, r);
    }
  };
  if (active & 1) {
    cov = covariance(points);
  } else {
    // CmdStan keeps data covariates as doubles. Promoting them to var chooses
    // a different exp-quad pullback and hides its accumulation-order contract.
    std::vector<Vec> fixed(n, Vec(d));
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < d; ++j) fixed[i][j] = x[i * d + j];
    cov = covariance(fixed);
  }
  Var j = jitter;
  if (diagonal_update)
    for (int i = 0; i < n; ++i) cov(i, i) += j;
  Var objective = stan::math::sum(
      stan::math::elt_multiply(cov, Eigen::Map<Mat>(seed.data(), n, n)));
  stan::math::grad(objective.vi_);
  for (int i = 0; i < n * n; ++i)
    check(out[i], cov.data()[i].val(), "GP value", true);
  if (active & 1)
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < d; ++j)
        check(xadj[i * d + j], 0.125 + points[i][j].adj(), "GP point");
  const bool exact = true;
  const std::string context = " n=" + std::to_string(n) +
                              " d=" + std::to_string(d) +
                              " mask=" + std::to_string(active) +
                              " repeated=" + std::to_string(repeated);
  if (active & 2)
    check(sadj, 0.125 + a.adj(), ("GP sigma" + context).c_str(), exact);
  if (active & 4)
    check(radj, 0.125 + r.adj(), ("GP rho" + context).c_str(), exact);
  if (diagonal_update && (active & 8))
    check(jadj, 0.125 + j.adj(), "GP diagonal increment", true);
}

static void gp_diagonal_fusion_proof() {
  using namespace stanli;
  for (int refusal = 0; refusal < 5; ++refusal) {
    Graph g;
    const int n = 3;
    const int x = g.add_slot(n, false), sigma = g.add_slot(1, true);
    const int rho = g.add_slot(1, true), jitter = g.add_slot(1, false);
    const int covariance = g.add_slot(n * n, false);
    g.add_op(OP_GP_COV, {x, sigma, rho}, covariance, {n, 1});
    g.ops.back().variant = kGpMatern32;
    int current = covariance, interior = -1;
    for (int i = 0; i < n; ++i) {
      const int index = refusal == 3 ? (n - 1 - i) * (n + 1) : i * (n + 1);
      const int read = g.add_slot(1, false), add = g.add_slot(1, false);
      const int write = g.add_slot(n * n, false);
      g.add_op(OP_INDEX, {current}, read, {index});
      g.add_op(refusal == 4 ? OP_SUB : OP_ADD, {read, jitter}, add);
      g.add_op(OP_SET_INDEX, {current, add}, write, {index});
      if (i == 0) interior = add;
      current = write;
    }
    g.result_slot = current;
    std::vector<int> roots;
    if (refusal == 1) roots.push_back(covariance);
    if (refusal == 2) roots.push_back(interior);
    const int fused = fuse_gp_diagonal_updates(g, roots);
    check(fused, refusal == 0 ? 1 : 0, "GP fusion proof", true);
    if (!refusal) {
      check(g.ops.size(), 1, "GP fused op count", true);
      check(g.ops[0].n_in, 4, "GP fused scalar input", true);
      check(g.ops[0].out, current, "GP output identity", true);
      check(g.slots[covariance].len, 0, "GP releases intermediate", true);
    }
  }
}
static void chol(int n) {
  using namespace stanli;
  Mat m(n, n), seed(n, n), out(n, n), adj = Mat::Constant(n, n, 0.125);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
      m(i, j) = std::sin(0.13 * (i + 1) * (j + 2));
      seed(i, j) = std::cos(0.21 * (i + 2) * (j + 1));
    }
  Mat a = (m * m.transpose()).eval();
  a.diagonal().array() += 2;
  KernelCtx ctx;
  int dims[] = {n};
  ctx.n_in = 1;
  ctx.idata = dims;
  ctx.n_idata = 1;
  ctx.in[0] = {a.data(), n * n};
  ctx.in_adj[0] = {adj.data(), n * n};
  ctx.out = {out.data(), n * n};
  ctx.out_adj_vec = {seed.data(), n * n};
  const Kernel& k = *find_kernel(OP_CHOLESKY);
  k.forward(ctx);
  k.backward(ctx);
  if (n == 0) return;  // No factor entries and no adjoint contributions.
  stan::math::nested_rev_autodiff scope;
  stan::math::var_value<Mat> av(a);
  auto factor = stan::math::cholesky_decompose(av);
  Var objective = stan::math::sum(stan::math::elt_multiply(factor, seed));
  stan::math::grad(objective.vi_);
  for (int i = 0; i < n * n; ++i) {
    check(out.data()[i], factor.val().data()[i], "Cholesky value", true);
    check(adj.data()[i], 0.125 + av.adj().data()[i], "Cholesky adjoint", true);
  }
  ctx.in_adj[0].data = nullptr;
  k.backward(ctx);
}

// A scaling-and-squaring matrix exponential coded independently of
// stan-math, in long double, as a finite-difference oracle for matrix_exp's
// block-identity backward. On this toolchain long double equals double, so
// this checks a second algorithm rather than a higher-precision one.
using MatLD = Eigen::Matrix<long double, -1, -1>;
static MatLD expm_ld(const MatLD& a) {
  const int n = (int)a.rows();
  if (n == 0) return MatLD(0, 0);
  long double norm = a.cwiseAbs().rowwise().sum().maxCoeff();
  int squarings = 0;
  long double scale = 1.0L;
  while (norm * scale > 0.5L) {
    scale *= 0.5L;
    ++squarings;
  }
  const MatLD as = a * scale;
  MatLD term = MatLD::Identity(n, n);
  MatLD total = MatLD::Identity(n, n);
  for (int k = 1; k <= 50; ++k) {
    term = (term * as) / (long double)k;
    total += term;
  }
  for (int i = 0; i < squarings; ++i) total = (total * total).eval();
  return total;
}

double g_max_matrix_exp_adj_rel = 0;
double g_max_matrix_exp_value_rel = 0;
double g_max_matrix_exp_fd_rel = 0;
double g_max_matrix_exp_tape_fd_rel = 0;
static void matrix_exp_case(int n, bool active, unsigned seed_val,
                            double max_rel) {
  using namespace stanli;
  std::mt19937 rng(seed_val);
  std::uniform_real_distribution<double> coef(-0.6, 0.6);
  Mat a(n, n), seed(n, n), out(n, n), adj = Mat::Constant(n, n, 0.125);
  for (int i = 0; i < n * n; ++i) {
    a.data()[i] = coef(rng);
    seed.data()[i] = coef(rng);
  }
  KernelCtx ctx;
  int dims[] = {n};
  ctx.n_in = 1;
  ctx.idata = dims;
  ctx.n_idata = 1;
  ctx.in[0] = {a.data(), n * n};
  ctx.in_adj[0] = {active ? adj.data() : nullptr, n * n};
  ctx.out = {out.data(), n * n};
  ctx.out_adj_vec = {seed.data(), n * n};
  const Kernel& k = *find_kernel(OP_MATRIX_EXP);
  k.forward(ctx);
  k.backward(ctx);
  if (n == 0) return;

  stan::math::nested_rev_autodiff scope;
  Eigen::Matrix<Var, -1, -1> av(n, n);
  for (int i = 0; i < n * n; ++i) av.data()[i] = a.data()[i];
  Eigen::Matrix<Var, -1, -1> ex = stan::math::matrix_exp(av);
  Var objective = stan::math::sum(stan::math::elt_multiply(ex, seed));
  stan::math::grad(objective.vi_);

  const std::string tag =
      " n=" + std::to_string(n) + " seed=" + std::to_string(seed_val);
  for (int i = 0; i < n * n; ++i) {
    // Forward is unchanged (double CMapM vs Matrix<var> instantiate the
    // same templated Pade/2x2 algorithm); Eigen picks different packet
    // arithmetic for the two scalar types, same as the pre-existing GP and
    // inverse_spd notes elsewhere in this file, so this is a relative check.
    check(out.data()[i], ex.data()[i].val(),
          ("matrix_exp value" + tag).c_str());
    const double want = ex.data()[i].val();
    const double rel = std::abs(out.data()[i] - want) / (1 + std::abs(want));
    g_max_matrix_exp_value_rel = std::max(g_max_matrix_exp_value_rel, rel);
  }
  if (!active) return;
  double adj_scale = 0;
  for (int i = 0; i < n * n; ++i)
    adj_scale = std::max(adj_scale, std::abs(0.125 + av.data()[i].adj()));
  for (int i = 0; i < n * n; ++i) {
    const double want = 0.125 + av.data()[i].adj();
    const double rel = std::abs(adj.data()[i] - want) / adj_scale;
    g_max_matrix_exp_adj_rel = std::max(g_max_matrix_exp_adj_rel, rel);
    if (rel > max_rel && ++failures < 12)
      std::printf(
          "FAIL matrix_exp adj%s i=%d got %.17g want %.17g (%.3g of scale)\n",
          tag.c_str(), i, adj.data()[i], want, rel);
  }

  MatLD ald(n, n), gld(n, n), dld(n, n);
  std::uniform_real_distribution<double> dir(-1.0, 1.0);
  for (int i = 0; i < n * n; ++i) {
    ald.data()[i] = (long double)a.data()[i];
    gld.data()[i] = (long double)seed.data()[i];
    dld.data()[i] = (long double)dir(rng);
  }
  const long double h = 1e-6L;
  const MatLD ep = expm_ld((ald + h * dld).eval());
  const MatLD em = expm_ld((ald - h * dld).eval());
  const MatLD fd = (ep - em) / (2 * h);
  const long double fd_dot = (gld.array() * fd.array()).sum();
  long double kernel_dot = 0, tape_dot = 0;
  for (int i = 0; i < n * n; ++i) {
    kernel_dot += (long double)(adj.data()[i] - 0.125) * dld.data()[i];
    tape_dot += (long double)av.data()[i].adj() * dld.data()[i];
  }
  const long double scale = std::max(1.0L, std::abs(fd_dot));
  const double kernel_rel = (double)(std::abs(kernel_dot - fd_dot) / scale);
  const double tape_rel = (double)(std::abs(tape_dot - fd_dot) / scale);
  g_max_matrix_exp_fd_rel = std::max(g_max_matrix_exp_fd_rel, kernel_rel);
  g_max_matrix_exp_tape_fd_rel =
      std::max(g_max_matrix_exp_tape_fd_rel, tape_rel);
  if (kernel_rel > 1e-6) {
    ++failures;
    std::printf(
        "FAIL matrix_exp fd%s kernel_rel=%.3e tape_rel=%.3e fd=%.10Lg\n",
        tag.c_str(), kernel_rel, tape_rel, fd_dot);
  }
}

enum class SolveKindTag { Plain, Spd, TriLow };
double g_max_solve_ulp = 0;
static void solve_case(bool left, SolveKindTag kind, uint16_t opcode, int n,
                       int k, bool vec, int activity, unsigned seed_val,
                       int64_t max_ulp) {
  using namespace stanli;
  using stan::math::var;
  std::mt19937 rng(seed_val);
  std::uniform_real_distribution<double> off(-0.3, 0.3);
  Mat a(n, n);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) a(i, j) = off(rng);
  if (kind == SolveKindTag::Spd) {
    a = (a * a.transpose()).eval();
    a.diagonal().array() += n + 1.0;
  } else {
    a.diagonal().array() += n + 1.0;
  }
  const int64_t br = left ? n : k, bc = left ? k : n;
  Mat b(br, bc);
  for (int i = 0; i < br * bc; ++i) b.data()[i] = off(rng) + 0.4;
  const int64_t outr = left ? n : k, outc = left ? k : n;
  Mat out(outr, outc), seed(outr, outc);
  for (int i = 0; i < outr * outc; ++i) seed.data()[i] = off(rng);
  Mat a_adj = Mat::Constant(n, n, 0.125);
  Mat b_adj = Mat::Constant(br, bc, 0.125);
  const bool divisor_var = activity != 2, dividend_var = activity != 1;

  KernelCtx ctx;
  int dims[] = {n, k};
  ctx.n_in = 2;
  ctx.idata = dims;
  ctx.n_idata = 2;
  const int ai = left ? 0 : 1, bi = left ? 1 : 0;
  ctx.in[ai] = {a.data(), n * n};
  ctx.in[bi] = {b.data(), br * bc};
  ctx.in_adj[ai] = {divisor_var ? a_adj.data() : nullptr, n * n};
  ctx.in_adj[bi] = {dividend_var ? b_adj.data() : nullptr, br * bc};
  ctx.out = {out.data(), outr * outc};
  ctx.out_adj_vec = {seed.data(), outr * outc};
  const uint8_t detail = static_cast<uint8_t>(activity);
  ctx.variant = 1u | (vec ? 2u : 0u) | (detail << 2);
  const Kernel& kern = *find_kernel(opcode);
  Op shape;
  shape.variant = ctx.variant;
  shape.idata = dims;
  shape.n_idata = 2;
  std::vector<double> scratch(
      kern.scratch_size ? kern.scratch_size(shape, nullptr) : 0);
  ctx.scratch = scratch.empty() ? nullptr : scratch.data();
  kern.forward(ctx);
  kern.backward(ctx);
  if (!scratch.empty()) {
    const Mat cached_a = a_adj, cached_b = b_adj;
    a_adj.setConstant(0.125);
    b_adj.setConstant(0.125);
    ctx.scratch = nullptr;
    kern.backward(ctx);
    if (std::memcmp(cached_a.data(), a_adj.data(), n * n * sizeof(double)) ||
        std::memcmp(cached_b.data(), b_adj.data(), br * bc * sizeof(double))) {
      ++failures;
      std::printf("FAIL saved QR differs from recomputation n=%d k=%d\n", n, k);
    }
  }

  stan::math::nested_rev_autodiff scope;
  Eigen::Matrix<var, -1, -1> av(n, n), bv(br, bc);
  for (int i = 0; i < n * n; ++i) av.data()[i] = a.data()[i];
  for (int i = 0; i < br * bc; ++i) bv.data()[i] = b.data()[i];
  Eigen::Matrix<var, -1, -1> result;
  if (kind == SolveKindTag::Plain)
    result = left ? stan::math::mdivide_left(av, bv)
                  : stan::math::mdivide_right(bv, av);
  else if (kind == SolveKindTag::Spd)
    result = left ? stan::math::mdivide_left_spd(av, bv)
                  : stan::math::mdivide_right_spd(bv, av);
  else
    result = left ? stan::math::mdivide_left_tri_low(av, bv)
                  : stan::math::mdivide_right_tri_low(bv, av);
  var objective = stan::math::sum(stan::math::elt_multiply(
      result, Eigen::Map<Mat>(seed.data(), outr, outc)));
  stan::math::grad(objective.vi_);

  const std::string tag =
      " left=" + std::to_string(left) + " kind=" + std::to_string((int)kind) +
      " n=" + std::to_string(n) + " k=" + std::to_string(k) +
      " vec=" + std::to_string(vec) + " act=" + std::to_string(activity) +
      " seed=" + std::to_string(seed_val);
  for (int i = 0; i < outr * outc; ++i)
    // This legacy gradient oracle promotes both operands to var matrices.
    // test_solve_forwards separately enforces bitwise values with the exact
    // activity and vector/matrix types, which can differ from this oracle.
    check(out.data()[i], result.data()[i].val(), ("solve value" + tag).c_str());
  if (divisor_var)
    for (int i = 0; i < n * n; ++i) {
      const double want = 0.125 + av.data()[i].adj();
      expect_ulp("solve A adj" + tag + " i=" + std::to_string(i),
                 a_adj.data()[i], want, max_ulp);
      g_max_solve_ulp = std::max(
          g_max_solve_ulp,
          (double)std::llabs(ulp_key(a_adj.data()[i]) - ulp_key(want)));
    }
  if (dividend_var)
    for (int i = 0; i < br * bc; ++i) {
      const double want = 0.125 + bv.data()[i].adj();
      expect_ulp("solve B adj" + tag + " i=" + std::to_string(i),
                 b_adj.data()[i], want, max_ulp);
      g_max_solve_ulp = std::max(
          g_max_solve_ulp,
          (double)std::llabs(ulp_key(b_adj.data()[i]) - ulp_key(want)));
    }
}

static void solve_family(bool left, SolveKindTag kind, uint16_t opcode,
                         int64_t max_ulp) {
  for (int n : {1, 2, 5, 10, 20})
    for (int k : {1, n})
      for (bool vec :
           (k == 1 ? std::vector<bool>{false, true} : std::vector<bool>{false}))
        for (int activity : {0, 1, 2, 3})
          for (unsigned s = 1; s <= 2; ++s)
            solve_case(left, kind, opcode, n, k, vec, activity, s, max_ulp);
}

double g_max_qf_ulp = 0;
static void qf_case(bool sym, uint16_t opcode, int n, int m, bool vec,
                    int activity, unsigned seed_val, int64_t max_ulp) {
  using namespace stanli;
  using stan::math::var;
  std::mt19937 rng(seed_val);
  std::uniform_real_distribution<double> off(-0.4, 0.4);
  Mat a(n, n);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) a(i, j) = off(rng);
  if (sym) a = (a + a.transpose()).eval();
  Mat b(n, m);
  for (int i = 0; i < n * m; ++i) b.data()[i] = off(rng);
  Mat out(m, m), seed(m, m);
  for (int i = 0; i < m * m; ++i) seed.data()[i] = off(rng);
  Mat a_adj = Mat::Constant(n, n, 0.125);
  Mat b_adj = Mat::Constant(n, m, 0.125);
  const bool a_var = activity != 2, b_var = activity != 1;

  KernelCtx ctx;
  int dims[] = {n, m};
  ctx.n_in = 2;
  ctx.idata = dims;
  ctx.n_idata = 2;
  ctx.in[0] = {a.data(), n * n};
  ctx.in[1] = {b.data(), (int64_t)n * m};
  ctx.in_adj[0] = {a_var ? a_adj.data() : nullptr, n * n};
  ctx.in_adj[1] = {b_var ? b_adj.data() : nullptr, (int64_t)n * m};
  ctx.variant = vec ? 1u : 0u;
  if (vec) {
    ctx.out = {out.data(), 1};
    ctx.out_adj = seed.data()[0];
  } else {
    ctx.out = {out.data(), (int64_t)m * m};
    ctx.out_adj_vec = {seed.data(), (int64_t)m * m};
  }
  const Kernel& kern = *find_kernel(opcode);
  kern.forward(ctx);
  kern.backward(ctx);

  stan::math::nested_rev_autodiff scope;
  Eigen::Matrix<var, -1, -1> av(n, n), bv(n, m);
  for (int i = 0; i < n * n; ++i) av.data()[i] = a.data()[i];
  for (int i = 0; i < n * m; ++i) bv.data()[i] = b.data()[i];
  var objective;
  std::vector<double> ref_val;
  if (vec) {
    Eigen::Matrix<var, -1, 1> bvec = bv.col(0);
    var r = sym ? stan::math::quad_form_sym(av, bvec)
                : stan::math::quad_form(av, bvec);
    objective = r * seed.data()[0];
    ref_val = {r.val()};
  } else {
    Eigen::Matrix<var, -1, -1> r =
        sym ? stan::math::quad_form_sym(av, bv) : stan::math::quad_form(av, bv);
    objective = stan::math::sum(
        stan::math::elt_multiply(r, Eigen::Map<Mat>(seed.data(), m, m)));
    ref_val.resize((size_t)r.size());
    for (Eigen::Index i = 0; i < r.size(); ++i)
      ref_val[(size_t)i] = r.data()[i].val();
  }
  stan::math::grad(objective.vi_);

  const std::string tag =
      " sym=" + std::to_string(sym) + " n=" + std::to_string(n) +
      " m=" + std::to_string(m) + " vec=" + std::to_string(vec) +
      " act=" + std::to_string(activity) + " seed=" + std::to_string(seed_val);
  const int64_t out_len = vec ? 1 : m * m;
  for (int i = 0; i < out_len; ++i)
    check(out.data()[i], ref_val[i], ("qf value" + tag).c_str());
  if (a_var)
    for (int i = 0; i < n * n; ++i) {
      const double want = 0.125 + av.data()[i].adj();
      expect_ulp("qf A adj" + tag + " i=" + std::to_string(i), a_adj.data()[i],
                 want, max_ulp);
      g_max_qf_ulp = std::max(
          g_max_qf_ulp,
          (double)std::llabs(ulp_key(a_adj.data()[i]) - ulp_key(want)));
    }
  if (b_var)
    for (int i = 0; i < n * m; ++i) {
      const double want = 0.125 + bv.data()[i].adj();
      expect_ulp("qf B adj" + tag + " i=" + std::to_string(i), b_adj.data()[i],
                 want, max_ulp);
      g_max_qf_ulp = std::max(
          g_max_qf_ulp,
          (double)std::llabs(ulp_key(b_adj.data()[i]) - ulp_key(want)));
    }
}

static void qf_family(bool sym, uint16_t opcode, int64_t max_ulp) {
  for (int n : {1, 2, 5, 10, 20})
    for (int m : {1, n})
      for (bool vec :
           (m == 1 ? std::vector<bool>{false, true} : std::vector<bool>{false}))
        for (int activity : {1, 2, 3})
          for (unsigned s = 1; s <= 2; ++s)
            qf_case(sym, opcode, n, m, vec, activity, s, max_ulp);
}

int main(int argc, char** argv) {
  // Optional local UBSan diagnostic: the pinned Stan Math blocked routine
  // itself binds an empty Eigen block reference at its final panel. The
  // default (including CI's ASan-only run) always tests both algorithms.
  const bool unblocked_only =
      argc == 2 && std::strcmp(argv[1], "--unblocked-only") == 0;
  if (argc > 1 && !unblocked_only) return 2;
  for (int n : {0, 1, 5, 9, 10, 11, 12, 21})
    for (int d : {1, 3})
      for (int mask = 0; mask < 8; ++mask) {
        gp(n, d, mask, stanli::kGpExpQuad, false);
        gp(n, d, mask, stanli::kGpExpQuad, true);
      }
  for (auto variant :
       {stanli::kGpMatern32, stanli::kGpMatern52, stanli::kGpExponential})
    for (int n : {0, 1, 5, 12})
      for (int d : {1, 3})
        for (int mask = 0; mask < 8; ++mask) {
          gp(n, d, mask, variant, false);
          gp(n, d, mask, variant, true);
        }
  gp_diagonal_fusion_proof();
  for (auto variant :
       {stanli::kGpMatern32, stanli::kGpMatern52, stanli::kGpExponential})
    for (int n : {1, 7, 30})
      for (int active : {0, 2, 4, 6, 7, 8, 14, 15})
        gp(n, 2, active, variant, false, .7, 1.3, .7, true);
  gp(4, 2, 7, stanli::kGpExpQuad, false, 1e-110);
  gp(4, 2, 7, stanli::kGpExpQuad, false, 1e110);
  for (double sigma : {1e-155, 1e-100, 1e100, 1e150})
    gp(4, 2, 6, stanli::kGpExpQuad, false, 0.7, sigma);
  gp(4, 2, 6, stanli::kGpExpQuad, false, 0.7, 1e150, 1e100);
  for (int n : {0, 1, 5, 35, 36, 80})
    if (!unblocked_only || n <= 35) chol(n);
  for (int n : {0, 1, 2, 5, 10, 20})
    for (bool active : {false, true})
      for (unsigned seed_val = 1; seed_val <= 4; ++seed_val)
        matrix_exp_case(n, active, seed_val, 1e-14);
  std::printf(
      "matrix_exp: max adj rel=%.3e max value rel=%.3e "
      "max kernel fd_rel=%.3e max tape fd_rel=%.3e\n",
      g_max_matrix_exp_adj_rel, g_max_matrix_exp_value_rel,
      g_max_matrix_exp_fd_rel, g_max_matrix_exp_tape_fd_rel);

  using namespace stanli;
  solve_family(true, SolveKindTag::Plain, OP_MDIVIDE_LEFT, 16);
  solve_family(false, SolveKindTag::Plain, OP_MDIVIDE_RIGHT, 16);
  solve_family(true, SolveKindTag::Spd, OP_MDIVIDE_LEFT_SPD, 16);
  solve_family(false, SolveKindTag::Spd, OP_MDIVIDE_RIGHT_SPD, 16);
  solve_family(true, SolveKindTag::TriLow, OP_MDIVIDE_LEFT_TRI_LOW, 16);
  solve_family(false, SolveKindTag::TriLow, OP_MDIVIDE_RIGHT_TRI_LOW, 16);
  std::printf("solve: max adj ulp=%.0f\n", g_max_solve_ulp);

  qf_family(false, OP_QUAD_FORM, 2);
  qf_family(true, OP_QUAD_FORM_SYM, 2);
  std::printf("quad_form: max adj ulp=%.0f\n", g_max_qf_ulp);

  if (!failures) std::puts("test_native_matrix_pullbacks OK");
  return failures ? 1 : 0;
}
