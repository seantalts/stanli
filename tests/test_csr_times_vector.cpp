#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/packet.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

struct Lcg {
  uint64_t s;
  double next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(s >> 11) / (double)(1ULL << 53);
  }
  double signed_unit() { return 2.0 * next() - 1.0; }
  double dyadic_unit() { return std::round(16.0 * next() - 8.0) / 8.0; }
  double unit(bool exact) { return exact ? dyadic_unit() : signed_unit(); }
};

struct Csr {
  int R = 0;
  int C = 0;
  std::vector<int> v;
  std::vector<int> u;
  std::vector<double> wd, bd, y;
  int nz() const { return (int)v.size(); }
};

Csr make_csr(int C, const std::vector<int>& lens, uint64_t seed,
             bool scramble = false, bool exact = false) {
  Lcg rng{seed};
  Csr m;
  m.R = (int)lens.size();
  m.C = C;
  m.u.push_back(1);
  for (int len : lens) {
    std::vector<int> cols;
    for (int k = 0; k < len; ++k) {
      int c = scramble ? (int)(rng.next() * C) + 1
                       : (int)((int64_t)(k + 1) * C / (len + 1)) + 1;
      if (!scramble && k > 0 && c <= cols.back()) c = cols.back() + 1;
      cols.push_back(std::min(c, C));
    }
    for (int c : cols) m.v.push_back(c);
    m.u.push_back(m.u.back() + len);
  }
  for (int k = 0; k < m.nz(); ++k) m.wd.push_back(rng.unit(exact) * 1.5);
  for (int c = 0; c < C; ++c) m.bd.push_back(rng.unit(exact) * 1.5);
  for (int r = 0; r < m.R; ++r) m.y.push_back(rng.unit(exact));
  return m;
}

template <typename T>
std::string json_array(const std::vector<T>& x) {
  std::ostringstream s;
  s.precision(17);
  s << "[";
  for (size_t i = 0; i < x.size(); ++i) s << (i ? "," : "") << x[i];
  s << "]";
  return s.str();
}

stanli::DataMap data_of(const Csr& m) {
  std::ostringstream s;
  s << "{\"R\":" << m.R << ",\"C\":" << m.C << ",\"NZ\":" << m.nz()
    << ",\"v\":" << json_array(m.v) << ",\"u\":" << json_array(m.u)
    << ",\"wd\":" << json_array(m.wd) << ",\"bd\":" << json_array(m.bd)
    << ",\"y\":" << json_array(m.y) << "}";
  return stanli::DataMap::from_json(s.str());
}

stanli::CompiledModel compile_csr(const Csr& m) {
  return stanli::compile_model(
      slurp("tests/fixtures/csr_times_vector.tmir.sexp"), data_of(m));
}

struct Eval {
  double lp = 0;
  std::vector<double> grad;
};

using stan::math::var;

enum class Oracle { Dense, Sparse, Upstream };

Eval reference(const Csr& m, const std::vector<double>& q, Oracle oracle) {
  const int nz = m.nz();
  Eigen::Matrix<var, -1, 1> wp(nz), bp(m.C), wd(nz), bd(m.C);
  for (int k = 0; k < nz; ++k) {
    wp(k) = q[(size_t)k];
    wd(k) = m.wd[(size_t)k];
  }
  for (int c = 0; c < m.C; ++c) {
    bp(c) = q[(size_t)(nz + c)];
    bd(c) = m.bd[(size_t)c];
  }
  auto term = [&](const Eigen::Matrix<var, -1, 1>& w,
                  const Eigen::Matrix<var, -1, 1>& b) {
    Eigen::Matrix<var, -1, 1> mu(m.R);
    if (oracle == Oracle::Upstream) {
      mu = stan::math::csr_matrix_times_vector(m.R, m.C, w, m.v, m.u, b);
      Eigen::Map<const Eigen::VectorXd> y(m.y.data(), m.R);
      return stan::math::normal_lpdf<false>(y, mu, 1.0);
    }
    for (int r = 0; r < m.R; ++r) {
      const int begin = m.u[(size_t)r] - 1;
      const int end = m.u[(size_t)r + 1] - 1;
      var acc = 0.0;
      if (oracle == Oracle::Dense) {
        std::vector<var> row((size_t)m.C, var(0.0));
        for (int k = begin; k < end; ++k)
          row[(size_t)m.v[(size_t)k] - 1] += w(k);
        for (int c = 0; c < m.C; ++c) acc += row[(size_t)c] * b(c);
      } else {
        for (int k = begin; k < end; ++k) acc += w(k) * b(m.v[(size_t)k] - 1);
      }
      mu(r) = acc;
    }
    Eigen::Map<const Eigen::VectorXd> y(m.y.data(), m.R);
    return stan::math::normal_lpdf<false>(y, mu, 1.0);
  };
  var lp = term(wd, bp) + term(wp, bd) + term(wp, bp) + term(wd, bd);
  lp.grad();
  Eval out;
  out.lp = lp.val();
  for (int k = 0; k < nz; ++k) out.grad.push_back(wp(k).adj());
  for (int c = 0; c < m.C; ++c) out.grad.push_back(bp(c).adj());
  stan::math::recover_memory();
  return out;
}

std::vector<double> point(const Csr& m, uint64_t seed, bool exact = false) {
  Lcg rng{seed};
  std::vector<double> q;
  for (int i = 0; i < m.nz() + m.C; ++i) q.push_back(rng.unit(exact) * 1.5);
  return q;
}

Eval evaluate(const Csr& m, const std::vector<double>& q) {
  stanli::CompiledModel cm = compile_csr(m);
  check((size_t)cm.n_unconstrained == q.size(), "parameter count");
  stanli::Executor ex(std::move(cm.graph));
  cm.bind(ex);
  for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
  Eval out;
  out.grad.assign(q.size(), 0.0);
  out.lp = ex.gradient(out.grad.data());
  return out;
}

struct DD {
  double hi = 0;
  double lo = 0;
};

DD two_sum(double a, double b) {
  const double s = a + b;
  const double bb = s - a;
  return {s, (a - (s - bb)) + (b - bb)};
}

DD two_prod(double a, double b) {
  const double p = a * b;
  return {p, std::fma(a, b, -p)};
}

DD dd_add(DD x, DD y) {
  const DD s = two_sum(x.hi, y.hi);
  const DD t = two_sum(x.lo, y.lo);
  const DD v = two_sum(s.hi, s.lo + t.hi);
  return two_sum(v.hi, t.lo + v.lo);
}

DD dd_neg(DD x) { return {-x.hi, -x.lo}; }

DD dd_mul(DD x, DD y) {
  const DD p = two_prod(x.hi, y.hi);
  const double cross = std::fma(x.hi, y.lo, std::fma(x.lo, y.hi, x.lo * y.lo));
  return two_sum(p.hi, p.lo + cross);
}

// -log(sqrt(2 pi)) to about 108 bits.
const DD kNegLogSqrtTwoPi = {-0x1.d67f1c864beb5p-1, 0x1.65b5a1b7ff5dfp-55};

constexpr double kU = 0x1p-53;

double gamma_n(double n) { return n * kU / (1.0 - n * kU); }

struct Residual {
  DD d;
  double magnitude = 0;
  int terms = 0;
};

Residual residual(const Csr& m, const std::vector<double>& w,
                  const std::vector<double>& b, int r) {
  const int begin = m.u[(size_t)r] - 1;
  const int end = m.u[(size_t)r + 1] - 1;
  Residual out;
  DD mu;
  for (int k = begin; k < end; ++k) {
    const double wk = w[(size_t)k];
    const double bk = b[(size_t)m.v[(size_t)k] - 1];
    mu = dd_add(mu, two_prod(wk, bk));
    out.magnitude += std::fabs(wk * bk);
  }
  out.terms = end - begin;
  out.d = dd_add(DD{m.y[(size_t)r], 0.0}, dd_neg(mu));
  return out;
}

struct Terms {
  std::vector<double> wp, bp;
  struct One {
    const std::vector<double>* w;
    const std::vector<double>* b;
    bool w_param, b_param;
  };
  std::vector<One> list;
};

Terms terms_of(const Csr& m, const std::vector<double>& q) {
  Terms t;
  t.wp.assign(q.begin(), q.begin() + m.nz());
  t.bp.assign(q.begin() + m.nz(), q.end());
  t.list = {{&m.wd, &t.bp, false, true},
            {&t.wp, &m.bd, true, false},
            {&t.wp, &t.bp, true, true},
            {&m.wd, &m.bd, false, false}};
  return t;
}

struct Truth {
  DD lp;
  std::vector<DD> grad;
};

Truth truth(const Csr& m, const std::vector<double>& q) {
  const size_t nz = (size_t)m.nz();
  const Terms terms = terms_of(m, q);
  Truth t;
  t.grad.assign(q.size(), DD{});
  for (const auto& term : terms.list) {
    const auto& w = *term.w;
    const auto& b = *term.b;
    for (int r = 0; r < m.R; ++r) {
      const DD d = residual(m, w, b, r).d;
      const DD sq = dd_mul(d, d);
      t.lp = dd_add(t.lp,
                    dd_add(kNegLogSqrtTwoPi, DD{-0.5 * sq.hi, -0.5 * sq.lo}));
      for (int k = m.u[(size_t)r] - 1; k < m.u[(size_t)r + 1] - 1; ++k) {
        const size_t c = (size_t)m.v[(size_t)k] - 1;
        if (term.w_param)
          t.grad[(size_t)k] = dd_add(t.grad[(size_t)k], dd_mul(d, {b[c], 0.0}));
        if (term.b_param)
          t.grad[nz + c] =
              dd_add(t.grad[nz + c], dd_mul(d, {w[(size_t)k], 0.0}));
      }
    }
  }
  return t;
}

struct Bounds {
  double lp = 0;
  std::vector<double> grad;
  int longest_b_sum = 0;
};

Bounds bounds_of(const Csr& m, const std::vector<double>& q,
                 const Truth& truth) {
  const size_t nz = (size_t)m.nz();
  const Terms terms = terms_of(m, q);
  std::vector<double> abs_sum(q.size(), 0.0), carried(q.size(), 0.0);
  std::vector<int> addends(q.size(), 0);
  double lp_carried = 0, lp_magnitude = 0;
  for (const auto& term : terms.list) {
    const auto& w = *term.w;
    const auto& b = *term.b;
    for (int r = 0; r < m.R; ++r) {
      const Residual res = residual(m, w, b, r);
      const double d = std::fabs(res.d.hi);
      const double e = gamma_n(res.terms + 4) * res.magnitude + kU * d;
      lp_carried += 0.5 * (2 * d * e + e * e);
      lp_magnitude += 0.5 * (d + e) * (d + e) + std::fabs(kNegLogSqrtTwoPi.hi);
      lp_carried += 2 * kU * std::fabs(kNegLogSqrtTwoPi.hi);
      for (int k = m.u[(size_t)r] - 1; k < m.u[(size_t)r + 1] - 1; ++k) {
        const size_t c = (size_t)m.v[(size_t)k] - 1;
        if (term.w_param) {
          abs_sum[(size_t)k] += d * std::fabs(b[c]);
          carried[(size_t)k] += std::fabs(b[c]) * e;
          ++addends[(size_t)k];
        }
        if (term.b_param) {
          abs_sum[nz + c] += d * std::fabs(w[(size_t)k]);
          carried[nz + c] += std::fabs(w[(size_t)k]) * e;
          ++addends[nz + c];
        }
      }
    }
  }
  Bounds out;
  out.lp = (lp_carried + gamma_n(4.0 * m.R + 8) * lp_magnitude) * (1 + 1e-9);
  out.grad.resize(q.size());
  for (size_t i = 0; i < q.size(); ++i) {
    out.grad[i] =
        (gamma_n(addends[i] + 4) * (abs_sum[i] + carried[i]) + carried[i]) *
        (1 + 1e-9);
    if (i >= nz) out.longest_b_sum = std::max(out.longest_b_sum, addends[i]);
  }
  return out;
}

double ulp_at(double x) {
  x = std::fabs(x);
  return std::nextafter(x, std::numeric_limits<double>::infinity()) - x;
}

double error_of(double got, DD want) {
  return std::fabs((got - want.hi) - want.lo);
}

double lp_ulps(double got, const Truth& t) {
  return error_of(got, t.lp) / ulp_at(t.lp.hi);
}

double block_scale(const Truth& t, size_t begin, size_t end) {
  double scale = 0;
  for (size_t i = begin; i < end; ++i)
    scale = std::max(scale, std::fabs(t.grad[i].hi));
  return scale;
}

double block_ulps(const std::vector<double>& got, const Truth& t, size_t begin,
                  size_t end) {
  const double scale = block_scale(t, begin, end);
  double worst = 0;
  for (size_t i = begin; i < end; ++i) {
    const double e = error_of(got[i], t.grad[i]);
    worst = std::max(
        worst, scale == 0 ? (e == 0 ? 0.0 : HUGE_VAL) : e / ulp_at(scale));
  }
  return worst;
}

double block_bound_ulps(const Bounds& bound, const Truth& t, size_t begin,
                        size_t end) {
  const double scale = block_scale(t, begin, end);
  double worst = 0;
  for (size_t i = begin; i < end; ++i)
    worst = std::max(worst, scale == 0 ? 0.0 : bound.grad[i] / ulp_at(scale));
  return worst;
}

struct Errors {
  double lp = 0;
  double w = 0;
  double b = 0;
  void worst(const Errors& o) {
    lp = std::max(lp, o.lp);
    w = std::max(w, o.w);
    b = std::max(b, o.b);
  }
};

Errors errors_of(const Eval& got, const Truth& t, size_t nz) {
  return {lp_ulps(got.lp, t), block_ulps(got.grad, t, 0, nz),
          block_ulps(got.grad, t, nz, got.grad.size())};
}

double worst_fraction_of_bound(const Eval& got, const Truth& t,
                               const Bounds& bound) {
  double worst = error_of(got.lp, t.lp) / bound.lp;
  for (size_t i = 0; i < got.grad.size(); ++i) {
    const double e = error_of(got.grad[i], t.grad[i]);
    worst = std::max(worst, bound.grad[i] == 0 ? (e == 0 ? 0.0 : HUGE_VAL)
                                               : e / bound.grad[i]);
  }
  return worst;
}

void check_case(const std::string& name, const Csr& m, bool sorted) {
  Errors stanli, stan_math, var_ref, bound_ulps;
  double fraction = 0;
  int longest = 0;
  for (uint64_t seed : {11u, 29u}) {
    const std::vector<double> q = point(m, seed);
    const Truth t = truth(m, q);
    const Bounds bound = bounds_of(m, q, t);
    const size_t nz = (size_t)m.nz();
    const Eval got = evaluate(m, q);
    stanli.worst(errors_of(got, t, nz));
    var_ref.worst(errors_of(
        reference(m, q, sorted ? Oracle::Dense : Oracle::Sparse), t, nz));
    if (sorted)
      stan_math.worst(errors_of(reference(m, q, Oracle::Upstream), t, nz));
    bound_ulps.worst({bound.lp / ulp_at(t.lp.hi),
                      block_bound_ulps(bound, t, 0, nz),
                      block_bound_ulps(bound, t, nz, q.size())});
    longest = bound.longest_b_sum;
    const double f = worst_fraction_of_bound(got, t, bound);
    fraction = std::max(fraction, f);
    check(f <= 1.0, name + ": stanli error exceeds the any-order bound");
  }
  std::printf("%-26s sum %4d | stanli %5.2f %5.2f %5.2f", name.c_str(), longest,
              stanli.lp, stanli.w, stanli.b);
  if (sorted)
    std::printf(" | stan math %5.2f %5.2f %5.2f", stan_math.lp, stan_math.w,
                stan_math.b);
  else
    std::printf(" | stan math   n/a   n/a   n/a");
  std::printf(" | var ref %5.2f %5.2f %5.2f | bound %7.1f %7.1f %7.1f | %.4f\n",
              var_ref.lp, var_ref.w, var_ref.b, bound_ulps.lp, bound_ulps.w,
              bound_ulps.b, fraction);
}

void check_index_structure_exact(const std::string& name, const Csr& m) {
  const std::vector<double> q = point(m, 11, true);
  const Truth t = truth(m, q);
  for (const DD& g : t.grad)
    check(g.lo == 0.0, name + ": inputs are not exact");
  const Eval got = evaluate(m, q);
  size_t mismatches = 0;
  for (size_t i = 0; i < q.size(); ++i)
    if (got.grad[i] != t.grad[i].hi) ++mismatches;
  check(mismatches == 0, name + ": " + std::to_string(mismatches) +
                             " gradient entries differ bitwise");
  const double lp = lp_ulps(got.lp, t);
  check(lp <= 2.0, name + ": lp " + std::to_string(lp) + " ulp from exact");
  std::printf("%-26s exact inputs, gradient bitwise %s, lp %.2f ulp\n",
              name.c_str(), mismatches == 0 ? "equal" : "DIFFERENT", lp);
}

void test_values() {
  std::printf(
      "error against a double-double reference, worst of 2 points: lp in ULP "
      "of lp,\ngradient blocks in ULP of the block's largest entry; columns "
      "are lp, w, b\n");
  check_case("mixed rows", make_csr(5, {2, 0, 3, 1, 0, 2}, 1), true);
  check_case("leading empty rows", make_csr(5, {0, 0, 2, 3, 1}, 2), true);
  check_case("middle empty rows", make_csr(5, {2, 0, 0, 3, 0, 1}, 3), true);
  check_case("trailing empty rows", make_csr(5, {2, 3, 0, 0, 0}, 4), true);
  check_case("single row", make_csr(6, {4}, 5), true);
  check_case("single full row", make_csr(3, {3}, 6), true);
  check_case("single empty row", make_csr(4, {0}, 7), true);
  check_case("all rows empty", make_csr(4, {0, 0, 0, 0, 0}, 8), true);
  check_case("unsorted repeated columns", make_csr(3, {3, 4, 2, 0, 5}, 9, true),
             false);
  std::vector<int> lens;
  for (int r = 0; r < 1200; ++r) lens.push_back(r % 5 == 3 ? 0 : 1 + r % 4);
  check_case("large", make_csr(150, lens, 10), true);
  check_index_structure_exact("structure: large",
                              make_csr(150, lens, 10, false, true));
  check_index_structure_exact("structure: unsorted",
                              make_csr(3, {3, 4, 2, 0, 5}, 9, true, true));
}

size_t arena_length(const stanli::Graph& g) {
  size_t total = 0;
  for (const auto& s : g.slots) total += (size_t)s.len;
  return total;
}

void test_linear_growth() {
  auto shape = [](int rows) {
    std::vector<int> lens;
    for (int r = 0; r < rows; ++r) lens.push_back(r % 7 == 2 ? 0 : 1 + r % 4);
    const Csr m = make_csr(200, lens, 12);
    stanli::CompiledModel cm = compile_csr(m);
    return arena_length(cm.graph);
  };
  const auto small = shape(500);
  const auto large = shape(2000);
  const double growth = (double)large / (double)small;
  check(growth < 4.5, "slot storage grows linearly with rows (4x rows gave " +
                          std::to_string(growth) + "x)");
}

}  // namespace

int main() {
  stanli::set_packet_math(false);
  try {
    test_values();
    test_linear_growth();
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL exception: %s\n", e.what());
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
