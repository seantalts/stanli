// stanli_laplace_whiten through the shipped C ABI, on Stan models. The
// expected values come from stanli_hessian, the dense finite-difference
// Hessian that does not use the builder's colouring or probes. The builder's
// pieces are tested one by one in test_laplace.cpp.
#include <stanli/capi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect_true(const std::string& what, bool ok) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

std::string slurp(const char* path) {
  std::ifstream f(path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

stanli_model* from_stan(const std::string& code, const std::string& data) {
  char err[8192]{};
  stanli_model* m = stanli_model_new_from_stan_with_opts(
      code.c_str(), data.c_str(), nullptr, err, sizeof err);
  if (m == nullptr) {
    ++failures;
    std::printf("FAIL compiling a test model: %s\n", err);
  }
  return m;
}

const char* centred_code = R"(
data {
  int<lower=1> J;
  int<lower=1> N;
  array[N] int<lower=1, upper=J> g;
  vector[N] y;
}
parameters {
  real mu;
  real<lower=0> sigma_a;
  vector[J] a;
  real<lower=0> sigma_y;
}
model {
  sigma_a ~ normal(0, 1);
  sigma_y ~ normal(0, 1);
  mu ~ normal(0, 5);
  a ~ normal(mu, sigma_a);
  y ~ normal(a[g], sigma_y);
}
)";

std::string centred_data(int J, int per_group) {
  std::mt19937 gen(5);
  std::normal_distribution<double> z(0, 1);
  std::vector<double> a(J);
  for (auto& v : a) v = 1.3 + 0.4 * z(gen);
  std::ostringstream g, y;
  for (int j = 0; j < J; ++j)
    for (int k = 0; k < per_group; ++k) {
      g << (g.tellp() > 0 ? "," : "") << j + 1;
      y << (y.tellp() > 0 ? "," : "");
      y.precision(17);
      y << a[j] + 0.7 * z(gen);
    }
  std::ostringstream out;
  out << "{\"J\":" << J << ",\"N\":" << J * per_group << ",\"g\":[" << g.str()
      << "],\"y\":[" << y.str() << "]}";
  return out.str();
}

const char* saddle_code = R"(
parameters {
  real x;
  real y;
}
model {
  target += x^2 / 2 - x^4 / 4;
  y ~ normal(0.5 * x, 1);
}
)";

// The unconstrained index of the named parameter blocks.
std::vector<int64_t> latent_index(stanli_model* m,
                                  const std::vector<std::string>& names) {
  std::vector<int64_t> out;
  int64_t start = 0;
  for (int64_t b = 0; b < stanli_n_param_blocks(m); ++b) {
    const int64_t size = stanli_param_block_size(m, b);
    if (std::find(names.begin(), names.end(), stanli_param_block_name(m, b)) !=
        names.end())
      for (int64_t k = 0; k < size; ++k) out.push_back(start + k);
    start += size;
  }
  return out;
}

struct Built {
  stanli_whitening* r = nullptr;
  stanli_whitening_report rep{};
  std::vector<double> q_hat, values, c, b, w, precision;
  std::vector<int64_t> perm, col_ptr, row_idx;
  std::vector<std::string> warnings;
  std::string error;
  ~Built() { stanli_whitening_free(r); }
};

void build(Built& out, stanli_model* m, const std::vector<double>& start,
           const std::vector<int64_t>& latent,
           const stanli_laplace_whiten_opts& o) {
  char err[4096]{};
  int stopped = 0;
  out.r = stanli_laplace_whiten(m, start.data(), (int64_t)latent.size(),
                                latent.data(), &o, nullptr, nullptr, &stopped,
                                err, sizeof err);
  out.error = err;
  if (out.r == nullptr) return;
  stanli_whitening_get_report(out.r, &out.rep);
  const int64_t n = out.rep.n, k = out.rep.rank;
  out.q_hat.resize(n);
  out.perm.resize(n);
  out.col_ptr.resize(n + 1);
  out.row_idx.resize(out.rep.nnz);
  out.values.resize(out.rep.nnz);
  out.c.resize(n);
  out.b.resize(n * k);
  out.w.resize(n * k);
  stanli_whitening_get_spec(out.r, out.q_hat.data(), out.perm.data(),
                            out.col_ptr.data(), out.row_idx.data(),
                            out.values.data(), out.c.data(), out.b.data(),
                            out.w.data());
  if (out.rep.has_precision) {
    out.precision.resize(n * n);
    stanli_whitening_get_precision(out.r, out.precision.data());
  }
  for (int64_t i = 0; i < stanli_whitening_n_warnings(out.r); ++i)
    out.warnings.push_back(stanli_whitening_warning(out.r, i));
}

stanli_model* whitened(stanli_model* m, const Built& b) {
  char err[4096]{};
  stanli_model* out =
      b.rep.rank == 0
          ? stanli_model_whiten(m, b.q_hat.data(), b.rep.n, b.perm.data(),
                                b.col_ptr.data(), b.row_idx.data(),
                                b.values.data(), b.rep.nnz, err, sizeof err)
          : stanli_model_whiten_lowrank(
                m, b.q_hat.data(), b.rep.n, b.perm.data(), b.col_ptr.data(),
                b.row_idx.data(), b.values.data(), b.rep.nnz, b.rep.rank,
                b.c.data(), b.b.data(), b.w.data(), err, sizeof err);
  expect_true(std::string("whitening the model: ") + err, out != nullptr);
  return out;
}

std::vector<double> dense_hessian(stanli_model* m,
                                  const std::vector<double>& q) {
  const size_t n = q.size();
  std::vector<double> h(n * n), g(n);
  double lp;
  char err[4096]{};
  expect_true(std::string("stanli_hessian: ") + err,
              stanli_hessian(m, q.data(), &lp, g.data(), h.data(), err,
                             sizeof err) == 0);
  return h;
}

double max_abs(const std::vector<double>& v) {
  double out = 0;
  for (double x : v) out = std::max(out, std::fabs(x));
  return out;
}

bool mentions(const Built& b, const char* text) {
  for (const auto& w : b.warnings)
    if (w.find(text) != std::string::npos) return true;
  return false;
}

void test_centred_model() {
  stanli_model* m = from_stan(centred_code, centred_data(12, 10));
  if (!m) return;
  const int64_t n = stanli_n_unconstrained(m);
  const auto latent = latent_index(m, {"a"});
  expect_true("a is the latent block", latent.size() == 12 && latent[0] == 2);
  std::vector<double> start(n, 0.0);
  stanli_laplace_whiten_opts o;
  stanli_laplace_whiten_opts_init(&o);
  o.keep_precision = 1;
  Built probe, dense;
  build(probe, m, start, latent, o);
  o.dense_pattern = 1;
  build(dense, m, start, latent, o);
  expect_true("built with the probe: " + probe.error, probe.r != nullptr);
  expect_true("built with the dense pattern: " + dense.error,
              dense.r != nullptr);
  if (!probe.r || !dense.r) return stanli_model_free(m);

  double gap = 0;
  for (int64_t i = 0; i < n; ++i)
    gap = std::max(gap, std::fabs(probe.q_hat[i] - dense.q_hat[i]));
  expect_true("probe and dense patterns give the same q_hat", gap <= 1e-8);
  expect_true("and the same factor size", probe.rep.nnz == dense.rep.nnz);
  expect_true("the probe is cheaper",
              probe.rep.pattern_gradients < dense.rep.pattern_gradients);
  expect_true("the dense pattern costs 1 + 2n",
              dense.rep.pattern_gradients == 1 + 2 * n);
  expect_true("the coloured final Hessian costs less than 1 + 2n",
              probe.rep.final_gradients < 1 + 2 * n);
  expect_true("report sizes", probe.rep.n == n && probe.rep.n_latent == 12 &&
                                  probe.rep.n_theta == 3);
  std::vector<double> theta_start(3), theta_hat(3);
  stanli_whitening_get_theta(probe.r, theta_start.data(), theta_hat.data());
  expect_true(
      "theta_hat is q_hat outside the latent block",
      theta_hat[0] == probe.q_hat[0] && theta_hat[2] == probe.q_hat[n - 1]);
  expect_true("theta_start is the start", theta_start[1] == 0);
  expect_true("no warning", probe.warnings.empty() &&
                                probe.rep.n_clipped == 0 &&
                                !probe.rep.has_spectrum);

  // The precision is the dense negative Hessian at q_hat, up to its
  // finite-difference error and the dropped entries.
  const auto h = dense_hessian(m, probe.q_hat);
  std::vector<double> diff(n * n);
  for (int64_t i = 0; i < n * n; ++i) diff[i] = probe.precision[i] + h[i];
  const double rel = max_abs(diff) / max_abs(h);
  std::printf("  precision vs dense negative Hessian: relative %.2e\n", rel);
  expect_true("precision matches the dense negative Hessian", rel <= 1e-6);

  // The whitened model is close to standard normal at z = 0.
  stanli_model* wh = whitened(m, probe);
  if (wh) {
    const auto hz = dense_hessian(wh, std::vector<double>(n, 0.0));
    double worst = 0;
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < n; ++j)
        worst = std::max(worst, std::fabs(-hz[i * n + j] - (i == j)));
    expect_true("the whitened Hessian at 0 is -I", worst <= 1e-3);
    stanli_model_free(wh);
  }

  int64_t bad = n;
  char err[4096]{};
  stanli_whitening* r =
      stanli_laplace_whiten(m, start.data(), 1, &bad, nullptr, nullptr, nullptr,
                            nullptr, err, sizeof err);
  expect_true(
      "an index outside the vector is refused",
      r == nullptr && std::string(err).find("outside") != std::string::npos);
  const int64_t twice[] = {3, 3};
  err[0] = '\0';
  r = stanli_laplace_whiten(m, start.data(), 2, twice, nullptr, nullptr,
                            nullptr, nullptr, err, sizeof err);
  expect_true("a repeated index is refused", r == nullptr && err[0] != '\0');

  int stopped = 0;
  r = stanli_laplace_whiten(
      m, start.data(), (int64_t)latent.size(), latent.data(), nullptr,
      [](void*) { return 1; }, nullptr, &stopped, err, sizeof err);
  expect_true("a poll stops the call", r == nullptr && stopped == 1);
  stanli_model_free(m);
}

void test_saddle_repairs() {
  stanli_model* m = from_stan(saddle_code, "{}");
  if (!m) return;
  const std::vector<double> start = {0, 0};
  const std::vector<int64_t> latent = {1};
  stanli_laplace_whiten_opts o;
  stanli_laplace_whiten_opts_init(&o);
  o.keep_precision = 1;
  Built abs, clip;
  build(abs, m, start, latent, o);
  o.repair = STANLI_WHITEN_REPAIR_CLIP;
  o.clip = 1e-3;
  build(clip, m, start, latent, o);
  expect_true("abs built: " + abs.error, abs.r != nullptr);
  expect_true("clip built: " + clip.error, clip.r != nullptr);
  if (abs.r && clip.r) {
    expect_true("abs repairs one eigenvalue", abs.rep.n_clipped == 1 &&
                                                  abs.rep.has_spectrum &&
                                                  abs.rep.rank == 1);
    expect_true("abs warns", mentions(abs, "replaced by absolute values"));
    expect_true("clip warns", mentions(clip, "clipped"));
    for (const Built* b : {&abs, &clip}) {
      stanli_model* wh = whitened(m, *b);
      if (!wh) continue;
      const double z[] = {0.1, -0.2};
      double lp, g[2];
      expect_true("the repaired whitening evaluates",
                  stanli_grad(wh, z, &lp, g) == 0 && std::isfinite(lp) &&
                      std::isfinite(g[0]) && std::isfinite(g[1]));
      stanli_model_free(wh);
    }
  }
  Built all;
  stanli_laplace_whiten_opts_init(&o);
  build(all, m, start, {0, 1}, o);
  expect_true("an all-latent model builds: " + all.error, all.r != nullptr);
  if (all.r)
    expect_true("with no hyperparameters",
                all.rep.n_theta == 0 && all.rep.outer_iterations == 0 &&
                    std::string(stanli_whitening_outer_message(all.r)).empty());
  stanli_model_free(m);
}

void test_brms_probe_equals_dense(const char* name,
                                  const std::vector<std::string>& latent) {
  const std::string base = std::string("tests/brms/") + name;
  stanli_model* m = from_stan(slurp((base + ".stan").c_str()),
                              slurp((base + ".json").c_str()));
  if (!m) return;
  const int64_t n = stanli_n_unconstrained(m);
  std::vector<double> start(n);
  std::mt19937 gen(3);
  std::normal_distribution<double> z(0, 0.3);
  for (auto& v : start) v = z(gen);
  const auto iu = latent_index(m, latent);
  stanli_laplace_whiten_opts o;
  stanli_laplace_whiten_opts_init(&o);
  o.max_outer = 0;
  o.inner_iter = 1;
  Built probe, dense;
  build(probe, m, start, iu, o);
  o.dense_pattern = 1;
  build(dense, m, start, iu, o);
  expect_true(std::string(name) + " built: " + probe.error + dense.error,
              probe.r && dense.r);
  if (probe.r && dense.r) {
    expect_true(std::string(name) + ": the probe verified its pattern",
                !probe.rep.pattern_fallback);
    expect_true(std::string(name) + ": same coloured Hessian cost",
                probe.rep.final_gradients == dense.rep.final_gradients &&
                    probe.rep.n_colors == dense.rep.n_colors);
    expect_true(std::string(name) + ": same factor",
                probe.values == dense.values && probe.q_hat == dense.q_hat);
  }
  stanli_model_free(m);
}

}  // namespace

int main() {
  if (!stanli_has_embedded_stanc()) {
    std::printf("SKIP test_laplace_capi: this build has no embedded stanc\n");
    return 0;
  }
  test_centred_model();
  test_saddle_repairs();
  test_brms_probe_equals_dense("s2_mm", {"z_1"});
  test_brms_probe_equals_dense("sw_re_slope", {"z_1"});
  test_brms_probe_equals_dense("i319_pois_re", {"z_1"});
  if (failures) {
    std::printf("test_laplace_capi: %d failures\n", failures);
    return 1;
  }
  std::printf("test_laplace_capi OK\n");
  return 0;
}
