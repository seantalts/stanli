#include "env_helpers.hpp"
#include <stanli/compile.hpp>
#include <stanli/data.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace stanli;

int failures = 0;

void expect(const std::string& what, bool ok) {
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

struct Run {
  size_t ops = 0;
  int bernoulli = 0;
  int normal = 0;
  int quadratic = 0;
  double lp = 0;
  std::vector<double> grad;
};

int count_opcode(const Graph& g, uint16_t opcode) {
  int n = 0;
  for (const Op& op : g.ops) n += op.opcode == opcode;
  return n;
}

Run run(const std::string& name, const CompileOptions& options) {
  const std::string stem = "tests/fixtures/" + name;
  const DataMap data = DataMap::from_json(slurp(stem + ".json"));
  CompiledModel cm =
      compile_model(slurp(stem + ".tmir.sexp"), data, 1, options);
  Run r;
  r.ops = cm.graph.ops.size();
  r.bernoulli = count_opcode(cm.graph, OP_BERNOULLI_LPMF);
  r.normal = count_opcode(cm.graph, OP_NORMAL_LPDF);
  r.quadratic = count_opcode(cm.graph, OP_LINEAR_GAUSSIAN_LPDF);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  const int64_t n = ex.n_params();
  r.grad.assign((size_t)n, 0.0);
  for (int64_t i = 0; i < n; ++i)
    ex.params_data()[i] = 0.3 - 0.45 * (double)(i % 3) + 0.1 * (double)i;
  r.lp = ex.gradient(r.grad.data());
  return r;
}

bool same_bits(const Run& a, const Run& b) {
  return a.ops == b.ops && a.grad.size() == b.grad.size() &&
         std::memcmp(&a.lp, &b.lp, sizeof(double)) == 0 &&
         std::memcmp(a.grad.data(), b.grad.data(),
                     a.grad.size() * sizeof(double)) == 0;
}

bool close(const Run& a, const Run& b) {
  if (a.grad.size() != b.grad.size()) return false;
  const auto near = [](double x, double y) {
    return std::abs(x - y) <= 1e-12 * std::max(1.0, std::abs(y));
  };
  bool ok = near(a.lp, b.lp);
  for (size_t i = 0; i < a.grad.size(); ++i) ok &= near(a.grad[i], b.grad[i]);
  return ok;
}

void test_active_duplicates_merge() {
  CompileOptions off, on;
  on.fast_math = true;
  const Run base = run("fast_math_cse", CompileOptions{});
  const Run slow = run("fast_math_cse", off);
  const Run fast = run("fast_math_cse", on);
  expect("default options match explicit false", same_bits(base, slow));
  expect("default keeps every Bernoulli term", slow.bernoulli == 12);
  expect("fast merges Bernoulli duplicates", fast.bernoulli == 2);
  expect("fast graph is smaller", fast.ops < slow.ops);
  expect("fast cse gradient matches default", close(fast, slow));
}

// The fusion on its own: the observation collapse would go on to replace
// the fused density, so it is switched off here.
void test_shared_parameter_fuses() {
  CompileOptions off, on;
  on.fast_math = true;
  const Run base = run("fast_math_shared", CompileOptions{});
  const Run slow = run("fast_math_shared", off);
  test_setenv("STANLI_NO_COLLAPSE", "1");
  const Run fast = run("fast_math_shared", on);
  test_unsetenv("STANLI_NO_COLLAPSE");
  expect("default options match explicit false", same_bits(base, slow));
  expect("default keeps every scalar density", slow.normal == 16);
  expect("fast fuses the loop", fast.normal == 1);
  expect("fast graph is smaller", fast.ops < slow.ops);
  expect("fast fused gradient matches default", close(fast, slow));
}

// Sixteen observations whose locations are mu * x[n] + mu: once fused, one
// quadratic form in mu.
void test_observations_collapse() {
  CompileOptions off, on;
  on.fast_math = true;
  const Run slow = run("fast_math_shared", off);
  const Run fast = run("fast_math_shared", on);
  expect("default has no quadratic form", slow.quadratic == 0);
  expect("fast collapses the fused density",
         fast.normal == 0 && fast.quadratic == 1);
  expect("fast collapsed gradient matches default", close(fast, slow));
}

}  // namespace

int main() {
  test_active_duplicates_merge();
  test_shared_parameter_fuses();
  test_observations_collapse();
  if (failures == 0) std::printf("test_fast_math OK\n");
  return failures == 0 ? 0 : 1;
}
