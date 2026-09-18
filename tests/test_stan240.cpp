#include "graph_helpers.hpp"
#include "cross_path.hpp"
#include <stanli/density_registry.hpp>
#include <stanli/function_registry.hpp>
#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
int failures = 0;
void equal(const std::string& label, double got, double want) {
  if (got != want && !(std::isnan(got) && std::isnan(want))) {
    ++failures;
    std::printf("FAIL %s: %.17g != %.17g\n", label.c_str(), got, want);
  }
}
void quantiles() {
  using namespace stanli;
  using stan::math::var;
  // Every activity mask; broadcast probability/scale and vector nu/location.
  const std::vector<std::vector<double>> inputs{
      {0.3}, {4.0, 5.0, 6.0}, {0.1, 0.2, 0.3}, {1.2}};
  for (int mask = 1; mask < 16; ++mask) {
    std::vector<bool> active;
    for (int k = 0; k < 4; ++k) active.push_back(mask & (1 << k));
    auto got = testutil::run_op_sum(OP_STUDENT_T_QF, 3, inputs, active);
    // All-var reference has the same callback partial for each active input.
    std::vector<std::vector<var>> v(4);
    for (int k = 0; k < 4; ++k)
      for (double x : inputs[k]) v[k].emplace_back(x);
    var sum = 0.0;
    for (int j = 0; j < 3; ++j)
      sum += stan::math::student_t_qf(v[0][0], v[1][j], v[2][j], v[3][0]);
    sum.grad();
    equal("quantile value", got.value, sum.val());
    size_t at = 0;
    for (int k = 0; k < 4; ++k)
      if (active[k])
        for (const auto& x : v[k])
          equal("quantile gradient", got.grad[at++], x.adj());
    stan::math::recover_memory();
  }
  for (double p : {0.0, 0.5, 1.0}) {
    auto got = testutil::run_one_op(OP_STUDENT_T_QF, {{p}, {5.0}, {0.2}, {1.1}},
                                    {true, false, true, false});
    var vp = p, mu = 0.2;
    var want = stan::math::student_t_qf(vp, 5.0, mu, 1.1);
    want.grad();
    equal("quantile boundary", got.value, want.val());
    equal("quantile boundary p", got.grad[0], vp.adj());
    equal("quantile boundary mu", got.grad[1], mu.adj());
    stan::math::recover_memory();
  }
  bool rejected = false;
  try {
    testutil::run_one_op(OP_STUDENT_T_QF, {{1.1}, {5.0}, {0.2}, {1.1}},
                         {true, false, false, false});
  } catch (const std::domain_error&) {
    rejected = true;
  }
  equal("quantile invalid probability", rejected, true);
}
void poisson_binomial() {
  using namespace stanli;
  using stan::math::var;
  for (int kind = 0; kind < 4; ++kind) {
    const auto got =
        testutil::run_one_op(OP_POISSON_BINOMIAL, {{0.2, 0.4, 0.7}}, {true},
                             {3, 0, 0, 2}, 2 * kind + 1);
    Eigen::Matrix<var, -1, 1> p(3);
    p << 0.2, 0.4, 0.7;
    std::vector<int> y{0, 2};
    var want;
    if (kind == 0) want = stan::math::poisson_binomial_lpmf(y, p);
    if (kind == 1) want = stan::math::poisson_binomial_cdf(y, p);
    if (kind == 2) want = stan::math::poisson_binomial_lcdf(y, p);
    if (kind == 3) want = stan::math::poisson_binomial_lccdf(y, p);
    want.grad();
    equal("poisson binomial value", got.value, want.val());
    for (int j = 0; j < 3; ++j)
      equal("poisson binomial gradient", got.grad[j], p[j].adj());
    stan::math::recover_memory();
  }
  bool rejected = false;
  try {
    testutil::run_one_op(OP_POISSON_BINOMIAL, {{0.2, 1.1}}, {true}, {2, 1, 1},
                         1);
  } catch (const std::domain_error&) {
    rejected = true;
  }
  equal("poisson binomial invalid probability", rejected, true);
}
void fixtures() {
  using namespace stanli;
  for (const auto* name :
       {"stan240_functions", "stan240_containers", "stan240_glm"}) {
    std::ifstream in(std::string("tests/fixtures/") + name + ".tmir.sexp");
    std::ostringstream text;
    text << in.rdbuf();
    cross::Options opts;
    opts.model = name;
    const auto result =
        cross::run_matrix(text.str(), DataMap{}, opts, cross::Ledger{});
    if (!result.ok || !result.skipped.empty()) {
      ++failures;
      std::printf("FAIL %s: %s\n%s\n", name, result.skipped.c_str(),
                  result.report.c_str());
    }
  }
}
}  // namespace
int main() {
  quantiles();
  poisson_binomial();
  fixtures();
  if (!failures) std::puts("test_stan240 OK");
  return failures ? 1 : 0;
}
