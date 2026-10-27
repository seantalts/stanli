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

int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? std::numeric_limits<int64_t>::min() - i : i;
}

void expect_ulp(const std::string& what, double got, double want,
                int64_t budget = 2) {
  const int64_t d = std::llabs(ulp_key(got) - ulp_key(want));
  if (d > budget) {
    ++failures;
    std::printf("FAIL %-32s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
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
             bool scramble = false) {
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
  for (int k = 0; k < m.nz(); ++k) m.wd.push_back(rng.signed_unit() * 1.5);
  for (int c = 0; c < C; ++c) m.bd.push_back(rng.signed_unit() * 1.5);
  for (int r = 0; r < m.R; ++r) m.y.push_back(rng.signed_unit());
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

std::vector<double> point(const Csr& m, uint64_t seed) {
  Lcg rng{seed};
  std::vector<double> q;
  for (int i = 0; i < m.nz() + m.C; ++i) q.push_back(rng.signed_unit() * 1.5);
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

double ulp_at(double x) {
  x = std::fabs(x);
  return std::nextafter(x, std::numeric_limits<double>::infinity()) - x;
}

double worst_block_ulps(const std::vector<double>& got,
                        const std::vector<double>& want, size_t begin,
                        size_t end) {
  double scale = 0;
  for (size_t i = begin; i < end; ++i)
    scale = std::max(scale, std::fabs(want[i]));
  double worst = 0;
  for (size_t i = begin; i < end; ++i)
    worst = std::max(worst, std::fabs(got[i] - want[i]) / ulp_at(scale));
  return worst;
}

void check_against_reference(const std::string& name, const Csr& m,
                             Oracle oracle, double b_budget) {
  const char* label = oracle == Oracle::Dense    ? " dense"
                      : oracle == Oracle::Sparse ? " sparse"
                                                 : " stan math";
  for (uint64_t seed : {11u, 29u}) {
    const std::vector<double> q = point(m, seed);
    const Eval got = evaluate(m, q);
    const Eval want = reference(m, q, oracle);
    expect_ulp(name + label + " lp", got.lp, want.lp);
    const size_t nz = (size_t)m.nz();
    const double w_ulps = worst_block_ulps(got.grad, want.grad, 0, nz);
    const double b_ulps = worst_block_ulps(got.grad, want.grad, nz, q.size());
    if (w_ulps > 2.0 || b_ulps > b_budget) {
      ++failures;
      std::printf("FAIL %s%s gradient: w block %.2f ulp, b block %.2f ulp\n",
                  name.c_str(), label, w_ulps, b_ulps);
    }
  }
}

void check_all(const std::string& name, const Csr& m, bool sorted,
               double b_budget = 2.0) {
  if (sorted) check_against_reference(name, m, Oracle::Upstream, b_budget);
  check_against_reference(name, m, sorted ? Oracle::Dense : Oracle::Sparse,
                          b_budget);
}

void test_values() {
  check_all("mixed rows", make_csr(5, {2, 0, 3, 1, 0, 2}, 1), true);
  check_all("leading empty rows", make_csr(5, {0, 0, 2, 3, 1}, 2), true);
  check_all("middle empty rows", make_csr(5, {2, 0, 0, 3, 0, 1}, 3), true);
  check_all("trailing empty rows", make_csr(5, {2, 3, 0, 0, 0}, 4), true);
  check_all("single row", make_csr(6, {4}, 5), true);
  check_all("single full row", make_csr(3, {3}, 6), true);
  check_all("single empty row", make_csr(4, {0}, 7), true);
  check_all("all rows empty", make_csr(4, {0, 0, 0, 0, 0}, 8), true);
  check_all("unsorted repeated columns", make_csr(3, {3, 4, 2, 0, 5}, 9, true),
            false);
  std::vector<int> lens;
  for (int r = 0; r < 1200; ++r) lens.push_back(r % 5 == 3 ? 0 : 1 + r % 4);
  check_all("large", make_csr(150, lens, 10), true, 10.0);
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
