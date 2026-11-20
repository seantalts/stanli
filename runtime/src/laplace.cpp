#include <stanli/laplace.hpp>

#include <stan/optimization/bfgs.hpp>

#include <boost/random/normal_distribution.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/random/uniform_real_distribution.hpp>

#include <Eigen/SparseLU>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <utility>

namespace stanli::laplace {

using Eigen::MatrixXd;
using Eigen::VectorXd;

namespace {

using Triplets = std::vector<Eigen::Triplet<double>>;
using Pairs = std::vector<std::pair<int, int>>;

// Rounding noise of a gradient difference, relative to the gradients.
constexpr double probe_noise = 1e3 * std::numeric_limits<double>::epsilon();
constexpr int probe_groups = 48;
constexpr int probe_rounds = 4;

// The symmetric pattern with the diagonal of the (row, column) pairs.
Sparse pattern_of(int m, const Pairs& pairs) {
  Triplets t;
  t.reserve(2 * pairs.size() + (size_t)m);
  for (const auto& [i, j] : pairs) {
    t.emplace_back(i, j, 1.0);
    t.emplace_back(j, i, 1.0);
  }
  for (int i = 0; i < m; ++i) t.emplace_back(i, i, 1.0);
  Sparse p(m, m);
  p.setFromTriplets(t.begin(), t.end(), [](double, double) { return 1.0; });
  p.makeCompressed();
  return p;
}

bool all_finite(const Sparse& a) {
  for (Eigen::Index k = 0; k < a.nonZeros(); ++k)
    if (!std::isfinite(a.valuePtr()[k])) return false;
  return true;
}

VectorXd difference_steps(const VectorXd& q, const std::vector<int>& cols) {
  VectorXd step(cols.size());
  for (size_t k = 0; k < cols.size(); ++k)
    step[(Eigen::Index)k] = 1e-4 * std::max(1.0, std::fabs(q[cols[k]]));
  return step;
}

VectorXd restrict_to(const VectorXd& v, const std::vector<int>& index) {
  VectorXd out(index.size());
  for (size_t k = 0; k < index.size(); ++k) out[(Eigen::Index)k] = v[index[k]];
  return out;
}

std::vector<int> complement(int n, const std::vector<int>& index) {
  std::vector<char> in((size_t)n, 0);
  for (int i : index) in[(size_t)i] = 1;
  std::vector<int> out;
  for (int i = 0; i < n; ++i)
    if (!in[(size_t)i]) out.push_back(i);
  return out;
}

// Whether a forward difference of the gradient along random weights on the
// columns `js` moves each entry by more than rounding noise.
std::vector<char> probe_response(Gradient& grad, const VectorXd& q,
                                 const std::vector<int>& iu, const VectorXd& g0,
                                 const VectorXd& step,
                                 const std::vector<int>& js, Rng& rng) {
  boost::random::uniform_real_distribution<double> weight(0.5, 1.5);
  boost::random::uniform_int_distribution<int> sign(0, 1);
  VectorXd x = q;
  for (int j : js) {
    const double w = weight(rng) * (sign(rng) ? 1.0 : -1.0);
    x[iu[(size_t)j]] += step[j] * w;
  }
  const VectorXd gp = restrict_to(grad.at(x), iu);
  std::vector<char> resp(iu.size());
  for (size_t i = 0; i < iu.size(); ++i) {
    const auto k = (Eigen::Index)i;
    resp[i] = std::fabs(gp[k] - g0[k]) >
              probe_noise * (std::fabs(gp[k]) + std::fabs(g0[k]));
  }
  return resp;
}

// The pairs (i, j) for which row i responded to the probe of j's group in
// every round, or nothing when there are more than `budget`.
std::optional<Pairs> intersect(
    const std::vector<std::vector<std::vector<char>>>& resps,
    const std::vector<std::vector<int>>& labels, int groups, int64_t budget) {
  const int m = (int)labels[0].size();
  std::vector<std::vector<int>> members((size_t)groups);
  for (int j = 0; j < m; ++j)
    members[(size_t)labels[0][(size_t)j]].push_back(j);
  Pairs out;
  for (int g = 0; g < groups; ++g) {
    const auto& cols = members[(size_t)g];
    const auto& first = resps[0][(size_t)g];
    for (int i = 0; i < m; ++i) {
      if (!first[(size_t)i]) continue;
      for (int j : cols) {
        bool keep = true;
        for (size_t t = 1; t < resps.size() && keep; ++t)
          keep = resps[t][(size_t)labels[t][(size_t)j]][(size_t)i];
        if (!keep) continue;
        out.emplace_back(i, j);
        if ((int64_t)out.size() > budget) return std::nullopt;
      }
    }
  }
  return out;
}

std::vector<int> permutation(int m, Rng& rng) {
  std::vector<int> p((size_t)m);
  std::iota(p.begin(), p.end(), 0);
  for (int i = m - 1; i > 0; --i) {
    boost::random::uniform_int_distribution<int> pick(0, i);
    std::swap(p[(size_t)i], p[(size_t)pick(rng)]);
  }
  return p;
}

// Ritz values (descending) and vectors of the symmetric operator `op` from
// up to m Lanczos steps with full reorthogonalization, with the residual
// norm of each.
struct Lanczos {
  VectorXd values;
  MatrixXd vectors;
  VectorXd residual;
};
template <typename Op>
Lanczos lanczos(const Op& op, int n, int m, const VectorXd& v0) {
  MatrixXd v(n, m);
  VectorXd alpha(m), beta(m);
  VectorXd x = v0 / v0.norm();
  int steps = m;
  for (int j = 0; j < m; ++j) {
    v.col(j) = x;
    VectorXd w = op(x);
    alpha[j] = w.dot(x);
    const auto basis = v.leftCols(j + 1);
    for (int pass = 0; pass < 2; ++pass) w -= basis * (basis.transpose() * w);
    beta[j] = w.norm();
    const double scale =
        std::max(alpha.head(j + 1).cwiseAbs().maxCoeff(), 1e-300);
    if (beta[j] <= 1e-13 * scale) {
      steps = j + 1;
      beta[j] = 0;
      break;
    }
    x = w / beta[j];
  }
  MatrixXd t = MatrixXd::Zero(steps, steps);
  for (int j = 0; j < steps; ++j) {
    t(j, j) = alpha[j];
    if (j + 1 < steps) t(j, j + 1) = t(j + 1, j) = beta[j];
  }
  Eigen::SelfAdjointEigenSolver<MatrixXd> eig(t);
  Lanczos out;
  out.values = eig.eigenvalues().reverse();
  const MatrixXd s = eig.eigenvectors().rowwise().reverse();
  out.vectors = v.leftCols(steps) * s;
  out.residual = (beta[steps - 1] * s.row(steps - 1).transpose()).cwiseAbs();
  return out;
}

VectorXd normal_vector(int n, Rng& rng) {
  boost::random::normal_distribution<double> z(0, 1);
  VectorXd v(n);
  for (int i = 0; i < n; ++i) v[i] = z(rng);
  return v;
}

double top_eigenvalue(const Sparse& q, Rng& rng) {
  const int n = (int)q.rows();
  const auto op = [&](const VectorXd& v) { return VectorXd(q * v); };
  const VectorXd v0 = normal_vector(n, rng);
  for (int m : {40, 160, n}) {
    m = std::min(m, n);
    const Lanczos r = lanczos(op, n, m, v0);
    if (r.residual[0] <= 1e-4 * std::fabs(r.values[0]) || m == n)
      return r.values[0];
  }
  return 0;  // not reached: the last pass has m == n
}

// The lower triangle of a, as sparse_ldlt takes it.
SparseFactor factor_lower(const Sparse& a) {
  const int64_t n = a.cols();
  std::vector<int64_t> col_ptr{0}, row_idx;
  std::vector<double> values;
  for (int j = 0; j < a.outerSize(); ++j) {
    for (Sparse::InnerIterator it(a, j); it; ++it)
      if (it.row() >= j) {
        row_idx.push_back(it.row());
        values.push_back(it.value());
      }
    col_ptr.push_back((int64_t)row_idx.size());
  }
  return sparse_ldlt(n, col_ptr.data(), row_idx.data(), values.data(), nullptr);
}

// Stan's L-BFGS, minimizing a functor `f(x, value, grad)` that returns
// nonzero for a point it cannot evaluate.
struct Minimum {
  VectorXd x;
  double value = 0;
  bool ok = false;
  int64_t iterations = 0;
  std::string message;
};
template <typename F>
Minimum lbfgs(F f, VectorXd x0, int max_iterations, size_t history,
              double abs_grad, double rel_f) {
  stan::optimization::BFGSMinimizer<F, stan::optimization::LBFGSUpdate<>> opt(
      std::move(f));
  opt.get_qnupdate().set_history_size(history);
  opt._conv_opts.maxIts = (size_t)std::max(max_iterations, 1);
  opt._conv_opts.tolAbsGrad = abs_grad;
  opt._conv_opts.tolRelF = rel_f / std::numeric_limits<double>::epsilon();
  opt._conv_opts.tolAbsF = 0;
  opt._conv_opts.tolAbsX = 0;
  opt._conv_opts.tolRelGrad = 0;
  Minimum out;
  try {
    opt.initialize(x0);
  } catch (const std::runtime_error&) {
    out.x = std::move(x0);
    out.value = std::numeric_limits<double>::infinity();
    out.message = "the starting point could not be evaluated";
    return out;
  }
  // Stan's minimizer tests convergence only after a step. A start whose
  // largest gradient entry is below abs_grad stays where it is, as scipy's
  // L-BFGS-B, which the Python builder used, decides.
  const bool started =
      max_iterations > 0 && opt.curr_g().cwiseAbs().maxCoeff() >= abs_grad;
  int code = stan::optimization::TERM_ABSGRAD;
  if (started)
    while ((code = opt.step()) == stan::optimization::TERM_SUCCESS) {
    }
  out.x = opt.curr_x();
  out.value = opt.curr_f();
  out.ok = true;
  out.iterations = started ? (int64_t)opt.iter_num() : 0;
  out.message = opt.get_code_string(code);
  return out;
}

// -log p over the latent block with the hyperparameters fixed.
struct InnerObjective {
  Gradient* grad;
  VectorXd q;
  const std::vector<int>* iu;
  int operator()(const VectorXd& u, double& f, VectorXd& g) {
    for (size_t k = 0; k < iu->size(); ++k) q[(*iu)[k]] = u[(Eigen::Index)k];
    VectorXd full;
    double lp;
    try {
      lp = (*grad)(q, full);
    } catch (const EvalError&) {
      return 1;
    }
    g = -restrict_to(full, *iu);
    if (!std::isfinite(lp) || !g.allFinite()) return 1;
    f = -lp;
    return 0;
  }
  int operator()(const VectorXd& u, double& f) {
    VectorXd g;
    return (*this)(u, f, g);
  }
};

struct MarginalMode {
  VectorXd q, theta_start, theta;
  int64_t iterations = 0;
  int64_t evaluations = 0;
  std::string message;
};

// Maximizes over theta the Laplace marginal
//   log p(theta, u_hat(theta)) - log det(-H_uu) / 2,
// u_hat from an inner L-BFGS, H_uu from the coloured Hessian, theta from an
// outer L-BFGS on forward differences.
MarginalMode marginal_mode(Gradient& grad, const VectorXd& q_start,
                           const std::vector<int>& iu,
                           const std::vector<int>& it, const Sparse& pattern_uu,
                           const Options& o) {
  const int n = (int)q_start.size();
  const HessianPlan plan = hessian_plan(pattern_uu, {});
  VectorXd u_cache = restrict_to(q_start, iu);
  MarginalMode out;

  const auto inner = [&](const VectorXd& theta, double& lp) {
    VectorXd q = VectorXd::Zero(n);
    for (size_t k = 0; k < it.size(); ++k) q[it[k]] = theta[(Eigen::Index)k];
    const Minimum r = lbfgs(InnerObjective{&grad, q, &iu}, u_cache,
                            o.inner_iter, 20, 1e-8, 1e-14);
    for (size_t k = 0; k < iu.size(); ++k) q[iu[k]] = r.x[(Eigen::Index)k];
    lp = r.ok ? -r.value : -std::numeric_limits<double>::infinity();
    return q;
  };
  const auto neg_marginal =
      [&](const VectorXd& theta) -> std::optional<double> {
    ++out.evaluations;
    double lp;
    const VectorXd q = inner(theta, lp);
    if (!std::isfinite(lp)) return std::nullopt;
    u_cache = restrict_to(q, iu);
    Sparse h;
    try {
      h = sparse_hessian(grad, q, plan, iu);
    } catch (const EvalError&) {
      return std::nullopt;
    }
    if (!all_finite(h)) return std::nullopt;
    SparseFactor f;
    try {
      f = factor_lower(-h);
    } catch (const std::exception&) {
      return std::nullopt;
    }
    if (f.n_negative > 0) return std::nullopt;
    return -(lp - 0.5 * f.log_abs_det);
  };
  struct Outer {
    const decltype(neg_marginal)* nm;
    int operator()(const VectorXd& theta, double& f, VectorXd& g) {
      const auto f0 = (*nm)(theta);
      if (!f0) return 1;
      f = *f0;
      g.resize(theta.size());
      for (Eigen::Index i = 0; i < theta.size(); ++i) {
        VectorXd t = theta;
        t[i] += 1e-5;
        const auto fi = (*nm)(t);
        if (!fi) return 1;
        g[i] = (*fi - f) / 1e-5;
      }
      return 0;
    }
    int operator()(const VectorXd& theta, double& f) {
      VectorXd g;
      return (*this)(theta, f, g);
    }
  };

  out.theta_start = restrict_to(q_start, it);
  out.theta = out.theta_start;
  if (!it.empty()) {
    const Minimum r = lbfgs(Outer{&neg_marginal}, out.theta_start, o.max_outer,
                            10, 1e-5, 1e-10);
    out.theta = r.x;
    out.iterations = r.iterations;
    out.message = r.message;
  }
  double lp;
  out.q = inner(out.theta, lp);
  return out;
}

// Q unchanged when it is positive definite; otherwise its eigenvalues below
// floor times the largest are raised to that (clip) or replaced by their
// absolute values with the same floor (abs).
struct DenseRepair {
  MatrixXd q;
  int64_t count = 0;
  bool repaired = false;
  double min = 0;
  double max = 0;
};
DenseRepair repair_dense(const MatrixXd& q, double floor, Repair mode) {
  DenseRepair out;
  if (Eigen::LLT<MatrixXd>(q).info() == Eigen::Success) {
    out.q = q;
    return out;
  }
  Eigen::SelfAdjointEigenSolver<MatrixXd> eig(q);
  const VectorXd& w = eig.eigenvalues();
  VectorXd w2(w.size());
  if (mode == Repair::clip) {
    const double lo = floor * w.maxCoeff();
    if (!(lo > 0))
      throw std::runtime_error(
          "the negative Hessian has no positive eigenvalue; the starting "
          "point is not near a mode");
    for (Eigen::Index i = 0; i < w.size(); ++i) {
      w2[i] = std::max(w[i], lo);
      out.count += w[i] < lo;
    }
  } else {
    const double lo = floor * w.cwiseAbs().maxCoeff();
    for (Eigen::Index i = 0; i < w.size(); ++i) {
      w2[i] = std::max(std::fabs(w[i]), lo);
      out.count += w2[i] != w[i];
    }
  }
  const MatrixXd r =
      eig.eigenvectors() * w2.asDiagonal() * eig.eigenvectors().transpose();
  out.q = (r + r.transpose()) / 2;
  out.repaired = true;
  out.min = w.minCoeff();
  out.max = w.maxCoeff();
  return out;
}

void store_factor(Result& r, const SparseFactor& f, bool scale_by_d) {
  r.perm = f.perm;
  r.col_ptr = f.col_ptr;
  r.row_idx = f.row_idx;
  r.values = f.values;
  if (scale_by_d)
    for (int64_t j = 0; j < f.n; ++j) {
      const double s = std::sqrt(f.d[(size_t)j]);
      for (int64_t p = f.col_ptr[(size_t)j]; p < f.col_ptr[(size_t)j + 1]; ++p)
        r.values[(size_t)p] *= s;
    }
}

void dense_whiten(Result& r, const Sparse& q, const Options& o) {
  const int n = (int)q.rows();
  const DenseRepair rep = repair_dense(MatrixXd(q), o.clip, o.repair);
  r.n_clipped = rep.count;
  r.has_spectrum = rep.repaired;
  r.min_eigenvalue = rep.min;
  r.max_eigenvalue = rep.max;
  // The approximate minimum degree order of Q's pattern.
  Sparse shifted = q;
  for (Eigen::Index k = 0; k < shifted.nonZeros(); ++k)
    shifted.valuePtr()[k] = 1.0;
  Sparse diag(n, n);
  diag.setIdentity();
  shifted += (double)n * diag;
  r.perm = factor_lower(shifted).perm;
  MatrixXd permuted(n, n);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j)
      permuted(i, j) = rep.q(r.perm[(size_t)i], r.perm[(size_t)j]);
  Eigen::LLT<MatrixXd> llt(permuted);
  if (llt.info() != Eigen::Success)
    throw std::runtime_error(
        "the repaired negative Hessian is not positive definite");
  const MatrixXd l = llt.matrixL();
  const double keep = 1e-13 * l.cwiseAbs().maxCoeff();
  r.col_ptr.assign(1, 0);
  for (int j = 0; j < n; ++j) {
    for (int i = j; i < n; ++i)
      if (i == j || std::fabs(l(i, j)) > keep) {
        r.row_idx.push_back(i);
        r.values.push_back(l(i, j));
      }
    r.col_ptr.push_back((int64_t)r.row_idx.size());
  }
  if (o.keep_precision) r.precision = rep.q;
}

void sparse_whiten(Result& r, const Sparse& q, const Options& o) {
  SparseFactor f;
  try {
    f = factor_lower(q);
  } catch (const std::runtime_error& e) {
    throw std::runtime_error(std::string(e.what()) +
                             "; use the \"abs_dense\" repair");
  }
  const double ratio = o.clip > 0 ? o.clip : 1e-8;
  const auto [dmin, dmax] = std::minmax_element(f.d.begin(), f.d.end());
  const bool fine = f.n_negative == 0 && *dmin > ratio * *dmax;
  MatrixXd u;
  if (!fine) {
    const Eigenpairs bad = bad_eigenpairs(q, o.clip, f.n_negative);
    r.has_spectrum = true;
    r.min_eigenvalue = bad.min;
    r.max_eigenvalue = bad.max;
    r.n_clipped = bad.values.size();
    if (r.n_clipped > 0) {
      const double lo =
          o.clip * std::max(std::fabs(bad.min), std::fabs(bad.max));
      u = bad.vectors;
      for (Eigen::Index a = 0; a < u.cols(); ++a) {
        const double w = bad.values[a];
        u.col(a) *= std::sqrt(std::max(std::fabs(w), lo) - w);
      }
      LowRank low;
      try {
        low = sparse_lowrank(f, u.cols(), u.data());
      } catch (const std::runtime_error& e) {
        throw std::runtime_error(
            std::string(e.what()) +
            "; the unpivoted factorization of the indefinite negative "
            "Hessian lost accuracy: use the \"abs_dense\" repair");
      }
      r.rank = low.k;
      r.c = std::move(low.c);
      r.b = std::move(low.b);
      r.w = std::move(low.w);
    }
  }
  store_factor(r, f, r.rank == 0);
  if (o.keep_precision) {
    r.precision = MatrixXd(q);
    if (u.size() > 0) r.precision += u * u.transpose();
  }
}

std::string repair_warning(const Result& r, const Options& o) {
  char text[512];
  std::snprintf(
      text, sizeof text,
      "laplace_whiten: %lld of %lld eigenvalues of the negative Hessian were "
      "%s (floor %g times the largest; smallest %.3g, largest %.3g); the "
      "whitened coordinates are only approximately standard normal",
      (long long)r.n_clipped, (long long)r.n,
      o.repair == Repair::clip ? "clipped" : "replaced by absolute values",
      o.clip, r.min_eigenvalue, r.max_eigenvalue);
  return text;
}

}  // namespace

Sparse threshold_pattern(const Sparse& h, double tol) {
  const int m = (int)h.rows();
  VectorXd d = h.diagonal().cwiseAbs().cwiseSqrt();
  double fallback = 0;
  for (Eigen::Index k = 0; k < h.nonZeros(); ++k)
    fallback = std::max(fallback, std::fabs(h.valuePtr()[k]));
  Pairs keep;
  for (int j = 0; j < h.outerSize(); ++j)
    for (Sparse::InnerIterator it(h, j); it; ++it) {
      double scale = d[it.row()] * d[j];
      if (scale == 0) scale = fallback;
      if (std::fabs(it.value()) > tol * scale) keep.emplace_back(it.row(), j);
    }
  return pattern_of(m, keep);
}

Sparse submatrix(const Sparse& a, const std::vector<int>& index) {
  std::vector<int> where((size_t)a.rows(), -1);
  for (size_t k = 0; k < index.size(); ++k) where[(size_t)index[k]] = (int)k;
  Triplets t;
  for (size_t k = 0; k < index.size(); ++k)
    for (Sparse::InnerIterator it(a, index[k]); it; ++it)
      if (where[(size_t)it.row()] >= 0)
        t.emplace_back(where[(size_t)it.row()], (int)k, it.value());
  Sparse out((int)index.size(), (int)index.size());
  out.setFromTriplets(t.begin(), t.end());
  out.makeCompressed();
  return out;
}

std::vector<int> distance2_colouring(const Sparse& pattern) {
  const int n = (int)pattern.cols();
  const int* ptr = pattern.outerIndexPtr();
  const int* idx = pattern.innerIndexPtr();
  std::vector<int> order((size_t)n);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    return ptr[a + 1] - ptr[a] > ptr[b + 1] - ptr[b];
  });
  std::vector<int> colour((size_t)n, -1);
  std::vector<int> seen((size_t)n + 1, -1);
  for (int j : order) {
    for (int p = ptr[j]; p < ptr[j + 1]; ++p) {
      const int i = idx[p];
      for (int r = ptr[i]; r < ptr[i + 1]; ++r) {
        const int c = colour[(size_t)idx[r]];
        if (c >= 0) seen[(size_t)c] = j;
      }
    }
    int c = 0;
    while (seen[(size_t)c] == j) ++c;
    colour[(size_t)j] = c;
  }
  return colour;
}

HessianPlan hessian_plan(const Sparse& pattern, const std::vector<int>& dense) {
  const int m = (int)pattern.cols();
  const int limit = std::max(64, (int)(4 * std::sqrt((double)m)));
  std::vector<char> is_dense((size_t)m, 0);
  for (int j : dense) is_dense[(size_t)j] = 1;
  for (int j = 0; j < m; ++j)
    if (pattern.outerIndexPtr()[j + 1] - pattern.outerIndexPtr()[j] > limit)
      is_dense[(size_t)j] = 1;
  HessianPlan plan;
  plan.pattern = pattern;
  plan.pattern.makeCompressed();
  for (int j = 0; j < m; ++j)
    (is_dense[(size_t)j] ? plan.dense : plan.free).push_back(j);
  if (!plan.free.empty()) {
    plan.colour = distance2_colouring(submatrix(plan.pattern, plan.free));
    plan.n_colours =
        *std::max_element(plan.colour.begin(), plan.colour.end()) + 1;
  }
  return plan;
}

Sparse sparse_hessian(Gradient& grad, const VectorXd& q,
                      const HessianPlan& plan, const std::vector<int>& cols) {
  const int m = (int)cols.size();
  const VectorXd step = difference_steps(q, cols);
  std::vector<char> in_free((size_t)m, 0);
  for (int j : plan.free) in_free[(size_t)j] = 1;
  const auto difference = [&](const std::vector<int>& js) {
    VectorXd up = q, down = q;
    for (int j : js) {
      up[cols[(size_t)j]] += step[j];
      down[cols[(size_t)j]] -= step[j];
    }
    const VectorXd gp = grad.at(up);
    const VectorXd gm = grad.at(down);
    return VectorXd(restrict_to(gp, cols) - restrict_to(gm, cols));
  };
  Triplets t;
  const auto add = [&](int i, int j, double x) {
    t.emplace_back(i, j, x);
    t.emplace_back(j, i, x);
  };
  std::vector<std::vector<int>> by_colour((size_t)plan.n_colours);
  for (size_t k = 0; k < plan.free.size(); ++k)
    by_colour[(size_t)plan.colour[k]].push_back(plan.free[k]);
  const Sparse& p = plan.pattern;
  for (const auto& js : by_colour) {
    const VectorXd dg = difference(js);
    for (int j : js)
      for (Sparse::InnerIterator it(p, j); it; ++it)
        if (in_free[(size_t)it.row()])
          add(it.row(), j, 0.5 * dg[it.row()] / (2 * step[j]));
  }
  for (int h : plan.dense) {
    const VectorXd dg = difference({h});
    for (int i = 0; i < m; ++i) {
      const double x = dg[i] / (2 * step[h]);
      add(i, h, in_free[(size_t)i] ? x : 0.5 * x);
    }
  }
  Sparse out(m, m);
  out.setFromTriplets(t.begin(), t.end());
  out.makeCompressed();
  return out;
}

Sparse probe_candidates(Gradient& grad, const VectorXd& q,
                        const std::vector<int>& iu, const VectorXd& g0,
                        const VectorXd& step, Rng& rng) {
  const int m = (int)iu.size();
  const int64_t budget = 32 * (int64_t)m + 100000;
  int groups = probe_groups;
  while (true) {
    if ((int64_t)groups * probe_rounds >= m) {
      // One column per probe, which is exact.
      Pairs pairs;
      for (int j = 0; j < m; ++j) {
        const auto resp = probe_response(grad, q, iu, g0, step, {j}, rng);
        for (int i = 0; i < m; ++i)
          if (resp[(size_t)i]) pairs.emplace_back(i, j);
      }
      return pattern_of(m, pairs);
    }
    std::vector<std::vector<int>> labels;
    for (int t = 0; t < probe_rounds; ++t) {
      auto p = permutation(m, rng);
      for (int& x : p) x %= groups;
      labels.push_back(std::move(p));
    }
    std::vector<std::vector<std::vector<char>>> resps;
    for (int t = 0; t < probe_rounds; ++t) {
      std::vector<std::vector<int>> members((size_t)groups);
      for (int j = 0; j < m; ++j)
        members[(size_t)labels[(size_t)t][(size_t)j]].push_back(j);
      std::vector<std::vector<char>> round((size_t)groups);
      for (int g = 0; g < groups; ++g)
        round[(size_t)g] = members[(size_t)g].empty()
                               ? std::vector<char>((size_t)m, 0)
                               : probe_response(grad, q, iu, g0, step,
                                                members[(size_t)g], rng);
      resps.push_back(std::move(round));
    }
    if (const auto pairs = intersect(resps, labels, groups, budget))
      return pattern_of(m, *pairs);
    groups *= 4;
  }
}

bool probe_agrees(Gradient& grad, const VectorXd& q, const std::vector<int>& iu,
                  const Sparse& h, const VectorXd& step, Rng& rng, double tol) {
  const int m = (int)iu.size();
  const VectorXd dh = h.diagonal().cwiseAbs();
  for (int round = 0; round < 3; ++round) {
    const VectorXd v = step.cwiseProduct(normal_vector(m, rng));
    VectorXd up = q, down = q;
    for (int k = 0; k < m; ++k) {
      up[iu[(size_t)k]] += v[k];
      down[iu[(size_t)k]] -= v[k];
    }
    const VectorXd gp = restrict_to(grad.at(up), iu);
    const VectorXd gm = restrict_to(grad.at(down), iu);
    const VectorXd hv = h * v;
    const double rms = std::sqrt(v.squaredNorm() / m);
    for (int k = 0; k < m; ++k) {
      const double miss = std::fabs((gp[k] - gm[k]) - 2.0 * hv[k]);
      const double noise = probe_noise * (std::fabs(gp[k]) + std::fabs(gm[k]));
      const double signal = 0.25 * tol * dh[k] * rms;
      if (miss > std::max(noise, signal)) return false;
    }
  }
  return true;
}

std::optional<Sparse> verified_pattern(Gradient& grad, const VectorXd& q,
                                       const std::vector<int>& iu,
                                       const VectorXd& step,
                                       const Sparse& candidates, double tol,
                                       Rng& rng) {
  const Sparse h = sparse_hessian(grad, q, hessian_plan(candidates, {}), iu);
  if (!all_finite(h)) return std::nullopt;
  if (!probe_agrees(grad, q, iu, h, step, rng, tol)) return std::nullopt;
  return threshold_pattern(h, tol);
}

std::optional<Sparse> probe_pattern(Gradient& grad, const VectorXd& q,
                                    const std::vector<int>& iu, double tol,
                                    uint32_t seed) {
  Rng rng = stan::services::util::create_rng(seed, 1);
  const VectorXd g0 = restrict_to(grad.at(q), iu);
  if (!g0.allFinite()) return std::nullopt;
  const VectorXd step = difference_steps(q, iu);
  const Sparse c = probe_candidates(grad, q, iu, g0, step, rng);
  return verified_pattern(grad, q, iu, step, c, tol, rng);
}

Sparse dense_pattern(const MatrixXd& hessian, const std::vector<int>& iu,
                     double tol) {
  return submatrix(threshold_pattern(hessian.sparseView(), tol), iu);
}

Eigenpairs bad_eigenpairs(const Sparse& q, double floor, int64_t n_negative) {
  const int n = (int)q.rows();
  Eigenpairs out;
  if (n <= 64) {
    Eigen::SelfAdjointEigenSolver<MatrixXd> eig{MatrixXd(q)};
    const VectorXd& w = eig.eigenvalues();
    const double lo = floor * w.cwiseAbs().maxCoeff();
    std::vector<int> bad;
    for (int i = 0; i < n; ++i)
      if (w[i] < lo) bad.push_back(i);
    out.values = w(bad);
    out.vectors = eig.eigenvectors()(Eigen::placeholders::all, bad);
    out.min = w[0];
    out.max = w[n - 1];
    return out;
  }
  Rng rng = stan::services::util::create_rng(1, 1);
  const double top = top_eigenvalue(q, rng);
  const double sigma = -floor * std::fabs(top);
  Sparse shifted = q;
  Sparse diag(n, n);
  diag.setIdentity();
  shifted -= sigma * diag;
  shifted.makeCompressed();
  Eigen::SparseLU<Sparse> lu(shifted);
  if (lu.info() != Eigen::Success)
    throw std::runtime_error(
        "could not factor the shifted negative Hessian; use the "
        "\"abs_dense\" repair");
  const auto op = [&](const VectorXd& v) { return VectorXd(lu.solve(v)); };
  int k = (int)std::min<int64_t>(
      std::max<int64_t>(2 * n_negative, n_negative + 4), n - 2);
  int m = std::min(n, 2 * k + 20);
  for (int attempt = 0; attempt < 8; ++attempt) {
    const VectorXd v0 = normal_vector(n, rng);
    Lanczos r;
    std::vector<int> by_size;
    int ready = 0;
    while (true) {
      r = lanczos(op, n, m, v0);
      by_size.resize((size_t)r.values.size());
      std::iota(by_size.begin(), by_size.end(), 0);
      std::stable_sort(by_size.begin(), by_size.end(), [&](int a, int b) {
        return std::fabs(r.values[a]) > std::fabs(r.values[b]);
      });
      const double biggest = std::fabs(r.values[by_size[0]]);
      ready = 0;
      while (ready < (int)by_size.size() &&
             r.residual[by_size[(size_t)ready]] <= 1e-9 * biggest)
        ++ready;
      if (ready >= k || m >= n) break;
      m = std::min(n, 2 * m);
    }
    const int take = std::min(k, ready);
    if (take > 0) {
      std::vector<std::pair<double, int>> w;
      double reach = 0;
      for (int a = 0; a < take; ++a) {
        const int i = by_size[(size_t)a];
        w.emplace_back(sigma + 1 / r.values[i], i);
        reach = std::max(reach, std::fabs(1 / r.values[i]));
      }
      std::sort(w.begin(), w.end());
      const double lo = floor * std::max(std::fabs(top), std::fabs(w[0].first));
      const int64_t found = std::count_if(
          w.begin(), w.end(), [](const auto& x) { return x.first < 0; });
      if (found >= n_negative && (sigma + reach >= lo || k >= n - 2)) {
        std::vector<int> bad;
        for (const auto& [value, i] : w)
          if (value < lo) bad.push_back(i);
        out.values.resize((Eigen::Index)bad.size());
        out.vectors.resize(n, (Eigen::Index)bad.size());
        for (size_t a = 0; a < bad.size(); ++a) {
          out.values[(Eigen::Index)a] = w[a].first;
          out.vectors.col((Eigen::Index)a) = r.vectors.col(bad[a]);
        }
        out.min = w[0].first;
        out.max = top;
        return out;
      }
    }
    k = std::min(2 * k, n - 2);
    m = std::min(n, std::max(m, 2 * k + 20));
  }
  throw std::runtime_error("could not find all " + std::to_string(n_negative) +
                           " negative eigenvalues of the negative Hessian; "
                           "use the \"abs_dense\" repair");
}

Result whiten(Gradient& grad, const HessianFn& hessian, const VectorXd& q_start,
              const std::vector<int>& latent, const Options& o) {
  const int n = (int)q_start.size();
  if (latent.empty())
    throw std::invalid_argument("the latent block needs at least one index");
  for (size_t k = 0; k < latent.size(); ++k)
    if (latent[k] < 0 || latent[k] >= n ||
        (k > 0 && latent[k] <= latent[k - 1]))
      throw std::invalid_argument(
          "the latent index must be increasing and within the unconstrained "
          "vector");
  if (!q_start.allFinite())
    throw std::invalid_argument("the starting point is not finite");
  const std::vector<int>& iu = latent;
  const std::vector<int> it = complement(n, iu);
  Result r;
  r.n = n;

  std::optional<Sparse> pattern_uu;
  grad.count = 0;
  if (!o.dense_pattern) {
    pattern_uu = probe_pattern(grad, q_start, iu, o.pattern_tolerance, o.seed);
    if (!pattern_uu) {
      r.pattern_fallback = true;
      r.warnings.push_back(
          "laplace_whiten: the sparsity probe disagreed with a gradient "
          "difference (or met a non-finite value); using the dense Hessian "
          "pattern instead");
    }
  }
  if (!pattern_uu) {
    grad.count += 1 + 2 * (int64_t)n;
    const MatrixXd h = hessian(q_start);
    if (!h.allFinite())
      throw std::runtime_error(
          "the Hessian at the starting point is not finite");
    pattern_uu = dense_pattern(h, iu, o.pattern_tolerance);
  }
  r.pattern_gradients = grad.count;

  grad.count = 0;
  MarginalMode mode = marginal_mode(grad, q_start, iu, it, *pattern_uu, o);
  r.marginal_gradients = grad.count;
  r.q_hat = std::move(mode.q);
  r.theta_start = std::move(mode.theta_start);
  r.theta_hat = std::move(mode.theta);
  r.outer_iterations = mode.iterations;
  r.outer_evaluations = mode.evaluations;
  r.outer_message = std::move(mode.message);

  Sparse h;
  if (o.dense_hessian) {
    h = hessian(r.q_hat).sparseView();
    r.final_gradients = 1 + 2 * (int64_t)n;
    r.n_colours = n;
  } else {
    // The latent pattern on its block and only the diagonal elsewhere.
    Triplets t;
    for (int j = 0; j < pattern_uu->outerSize(); ++j)
      for (Sparse::InnerIterator x(*pattern_uu, j); x; ++x)
        t.emplace_back(iu[(size_t)x.row()], iu[(size_t)j], 1.0);
    for (int i : it) t.emplace_back(i, i, 1.0);
    Sparse full(n, n);
    full.setFromTriplets(t.begin(), t.end());
    const HessianPlan plan = hessian_plan(full, it);
    grad.count = 0;
    std::vector<int> all((size_t)n);
    std::iota(all.begin(), all.end(), 0);
    h = sparse_hessian(grad, r.q_hat, plan, all);
    r.final_gradients = grad.count;
    r.n_colours = plan.n_colours;
  }
  if (!all_finite(h))
    throw std::runtime_error("the Hessian at the marginal mode is not finite");
  Sparse q = -(Sparse(h.transpose()) + h) / 2.0;
  const VectorXd d = q.diagonal().cwiseAbs().cwiseSqrt();
  q.prune([&](int i, int j, double x) {
    return x != 0 && std::fabs(x) >= o.threshold * d[i] * d[j];
  });
  q.makeCompressed();

  if (o.repair == Repair::abs)
    sparse_whiten(r, q, o);
  else
    dense_whiten(r, q, o);
  if (r.n_clipped > 0) r.warnings.push_back(repair_warning(r, o));
  return r;
}

}  // namespace stanli::laplace
