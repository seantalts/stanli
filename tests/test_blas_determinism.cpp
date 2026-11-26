// Same model, data and point, same bits, on a busy machine. A Gaussian
// process likelihood with a 151 x 151 covariance takes its gradient in 144
// separate processes, 24 at a time, the way a corpus replay or several
// sampling jobs run side by side, and every process must print the same
// bits. With STANLI_ACCELERATE the covariance goes through the accelerated
// multi_normal kernel, whose dpotri sometimes returns a wrong inverse when
// Accelerate uses threads of its own.
//
// Run with --child to print one gradient's hash.
#include <stanli/capi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kModel = R"(
data {
  int<lower=1> N;
  matrix[N, N] D;
  vector[N] y;
}
parameters {
  real a;
  real<lower=0> etasq;
  real<lower=0> rhosq;
}
model {
  matrix[N, N] K;
  for (i in 1:(N - 1)) {
    K[i, i] = etasq + 0.01;
    for (j in (i + 1):N) {
      K[i, j] = etasq * exp(-rhosq * D[i, j]);
      K[j, i] = K[i, j];
    }
  }
  K[N, N] = etasq + 0.01;
  rhosq ~ normal(3, 0.25);
  etasq ~ normal(1, 0.25);
  a ~ normal(0, 1);
  y ~ multi_normal(rep_vector(a, N), K);
}
)";

std::string data() {
  const int n = 151;
  std::string s = "{\"N\":151,\"D\":[";
  char buf[40];
  for (int i = 0; i < n; ++i) {
    s += i ? ",[" : "[";
    for (int j = 0; j < n; ++j) {
      const double dx = std::sin(1.3 * i) - std::sin(1.3 * j);
      const double dy = std::cos(0.7 * i) - std::cos(0.7 * j);
      std::snprintf(buf, sizeof buf, "%s%.17g", j ? "," : "",
                    std::sqrt(dx * dx + dy * dy));
      s += buf;
    }
    s += "]";
  }
  s += "],\"y\":[";
  for (int i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof buf, "%s%.17g", i ? "," : "", std::sin(0.37 * i));
    s += buf;
  }
  return s + "]}";
}

int child() {
  char err[4096] = {};
  stanli_model* m = stanli_model_new_from_stan_with_opts(
      kModel, data().c_str(), nullptr, err, sizeof err);
  if (m == nullptr) {
    std::printf("compile failed: %s\n", err);
    return 1;
  }
  const double q[3] = {0.1, 0.2, 0.3};
  double lp = 0, g[3] = {};
  if (stanli_grad(m, q, &lp, g) != 0) {
    std::printf("gradient failed\n");
    return 1;
  }
  std::printf("%.17g %.17g %.17g %.17g\n", lp, g[0], g[1], g[2]);
  stanli_model_free(m);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--child") == 0) return child();
  if (!stanli_has_embedded_stanc()) {
    std::printf("SKIP: this build does not embed stanc\n");
    return 77;
  }
  std::vector<std::string> results;
  for (int round = 0; round < 6; ++round) {
    std::vector<std::string> batch(24);
    std::vector<std::thread> runs;
    for (size_t k = 0; k < batch.size(); ++k)
      runs.emplace_back([&, k] {
        const std::string cmd = std::string("'") + argv[0] + "' --child";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return;
        char buf[256] = {};
        if (fgets(buf, sizeof buf, f)) batch[k] = buf;
        pclose(f);
      });
    for (auto& t : runs) t.join();
    results.insert(results.end(), batch.begin(), batch.end());
  }
  const std::set<std::string> distinct(results.begin(), results.end());
  std::printf("%zu processes, %zu different results\n", results.size(),
              distinct.size());
  for (const std::string& r : distinct) std::printf("  %s", r.c_str());
  const bool ok = distinct.size() == 1 && !distinct.begin()->empty() &&
                  distinct.begin()->find("failed") == std::string::npos;
  std::printf(ok ? "ok\n" : "FAIL the gradient differs between processes\n");
  return ok ? 0 : 1;
}
