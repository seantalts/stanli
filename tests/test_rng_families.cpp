// Scalar RNG migration: full lowering, upstream draws, failure and
// continuation.
#include <stanli/compile.hpp>
#include <stanli/execution_report.hpp>
#include <stanli/wa_interp.hpp>
#include <stanli/mir_decode.hpp>
#include <stan/math.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

namespace {
using namespace stanli;
void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}
std::string slurp(const char* path) {
  std::ifstream file(path);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}
double upstream(int family, double a, double b, WaRng& stream) {
  auto& rng = stream.gen();
  switch (family) {
    case 0:
      return stan::math::std_normal_rng(rng);
    case 1:
      return stan::math::gamma_rng(a, b, rng);
    case 2:
      return stan::math::inv_gamma_rng(a, b, rng);
    case 3:
      return stan::math::beta_rng(a, b, rng);
    case 4:
      return stan::math::chi_square_rng(a, rng);
    case 5:
      return stan::math::cauchy_rng(a, b, rng);
    case 6:
      return stan::math::double_exponential_rng(a, b, rng);
    case 7:
      return stan::math::logistic_rng(a, b, rng);
    case 8:
      return stan::math::weibull_rng(a, b, rng);
    case 9:
      return stan::math::neg_binomial_2_rng(a, b, rng);
    case 10:
      return stan::math::neg_binomial_2_log_rng(a, b, rng);
  }
  throw std::logic_error("bad test family");
}
struct Failure {
  std::string kind, message;
  bool operator==(const Failure& other) const {
    return kind == other.kind && message == other.message;
  }
};
template <class F>
Failure failure(F f) {
  try {
    f();
  } catch (const std::exception& e) {
    return {typeid(e).name(), e.what()};
  }
  return {};
}
bool same(const std::vector<double>& a, const std::vector<double>& b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
}
void exercise(int family, int region, bool benchmark) {
  const auto text = slurp("tests/fixtures/gq_scalar_rng_gap.tmir.sexp");
  const auto data =
      DataMap::from_json("{\"family\":" + std::to_string(family) +
                         ",\"region\":" + std::to_string(region) + "}");
  auto cm = compile_model(text, data);
  if (cm.write_array && cm.write_array->interp)
    std::fprintf(stderr, "family %d region %d: %s\n", family, region,
                 cm.write_array->truncated.c_str());
  require(cm.write_array && !cm.write_array->interp &&
              cm.write_array->truncated.empty(),
          "scalar RNG fell back during lowering");
  const auto manifest = execution_report(cm);
  require(manifest.find("OP_RNG") != std::string::npos, "missing selected RNG");
  if (region)
    require(manifest.find("register_program") != std::string::npos,
            "runtime loop did not select register program");
  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  auto prog =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> env;
  env["family"].is_int = env["region"].is_int = true;
  env["family"].i = {family};
  env["family"].r = {double(family)};
  env["region"].i = {region};
  env["region"].r = {double(region)};
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    env[flag].is_int = true;
    env[flag].i = {1};
    env[flag].r = {1};
  }
  WaInterp interp(prog, env);
  std::map<std::string, DataMap::Entry> params;
  double a = 2.3, b = 1.7;
  const auto set = [&](double aa, double bb) {
    a = aa;
    b = bb;
    graph.params_data()[0] = a;
    graph.params_data()[1] = b;
    params["a"].r = {a};
    params["b"].r = {b};
  };
  set(a, b);
  const auto compiled = [&](WaRng& rng) {
    ExecutionTrace trace;
    trace.forbid_mir = true;
    ExecutionTraceScope scope(trace);
    graph.run_forward_only(EvalState{&rng});
    trace.check();
    std::vector<double> row;
    for (const auto& col : cm.write_array->columns)
      for (int64_t i = 0; i < col.len; ++i)
        row.push_back(graph.value_ptr(col.slot)[col.storage_index(i)]);
    return row;
  };
  const auto direct = [&](WaRng& rng) {
    const double before = stan::math::normal_rng(0, 1, rng.gen());
    const double draw = upstream(family, a, b, rng);
    const double after = stan::math::normal_rng(0, 1, rng.gen());
    return std::vector<double>{a, b, before, draw, after};
  };
  for (unsigned chain : {0u, 3u}) {
    WaRng gr(1234, chain), ir(1234, chain), dr(1234, chain);
    for (int i = 0; i < 12; ++i) {
      set(0.8 + i * 0.2, 1.7);
      auto got = compiled(gr);
      require(same(got, interp.eval(params, ir)),
              "compiled/interpreted draw bytes differ");
      require(same(got, direct(dr)), "compiled/upstream draw bytes differ");
      require(gr.gen() == ir.gen() && gr.gen() == dr.gen(),
              "RNG continuation differs");
    }
  }
  // Invalid parameters include nonfinite inputs and each family's domain.
  // A prefix draw must remain consumed; rejection must not consume a suffix.
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (auto pair : {std::pair<double, double>{-1, 1.7},
                    {2.3, -1},
                    {inf, 1.7},
                    {2.3, inf},
                    {nan, 1.7},
                    {2.3, nan},
                    {0, 0}}) {
    set(pair.first, pair.second);
    WaRng gr(781), ir(781), dr(781);
    const auto ge = failure([&] { compiled(gr); });
    const auto ie = failure([&] { interp.eval(params, ir); });
    const auto de = failure([&] { direct(dr); });
    require(ge == ie && ge == de, "RNG rejection class/message differs");
    require(gr.gen() == ir.gen() && gr.gen() == dr.gen(),
            "rejection stream differs");
    set(2.3, 1.7);
    const auto recovered = compiled(gr);
    require(
        same(recovered, interp.eval(params, ir)) && same(recovered, direct(dr)),
        "recovery after rejected draw differs");
    require(gr.gen() == ir.gen() && gr.gen() == dr.gen(),
            "recovery stream differs");
  }
  if (benchmark) {
    // Warm both paths; alternate order over repeated samples. Timing excludes
    // row allocation and trace scopes on the graph, but includes interpreter
    // output construction because it is intrinsic to that path.
    WaRng gr(1234), ir(1234);
    for (int i = 0; i < 100; ++i) {
      graph.run_forward_only(EvalState{&gr});
      interp.eval(params, ir);
    }
    for (int sample = 0; sample < 7; ++sample) {
      double times[2]{};
      for (int j = 0; j < 2; ++j) {
        int path = (sample + j) % 2;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) {
          if (path == 0)
            graph.run_forward_only(EvalState{&gr});
          else
            interp.eval(params, ir);
        }
        times[path] = std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - start)
                          .count() /
                      1000;
      }
      std::printf("BENCH %d %d %d %.6f %.6f\n", family, region, sample,
                  times[0], times[1]);
    }
  }
}
}  // namespace
int main(int argc, char**) {
  try {
    // The container form now uses the same vectorized Stan Math call.
    auto container = compile_model(
        slurp("tests/fixtures/gq_scalar_rng_container_guard.tmir.sexp"),
        DataMap{});
    require(container.write_array && !container.write_array->interp,
            "container RNG fell back");
    Executor container_graph(std::move(container.write_array->graph));
    container.write_array->bind(container_graph);
    container_graph.params_data()[0] = 0.1;
    container_graph.params_data()[1] = 0.2;
    WaRng fallback_rng(1234), direct_rng(1234);
    const std::vector<double> expected = {
        0.1, 0.2, stan::math::gamma_rng(std::exp(0.1), 1.5, direct_rng.gen()),
        stan::math::gamma_rng(std::exp(0.2), 1.5, direct_rng.gen())};
    container_graph.run_forward_only(EvalState{&fallback_rng});
    std::vector<double> actual;
    for (const auto& col : container.write_array->columns)
      for (int64_t i = 0; i < col.len; ++i)
        actual.push_back(
            container_graph.value_ptr(col.slot)[col.storage_index(i)]);
    require(same(actual, expected), "container RNG changed");
    require(fallback_rng.gen() == direct_rng.gen(),
            "container fallback stream differs");
    for (int family = 0; family < 11; ++family)
      for (int region = 0; region < 2; ++region)
        exercise(family, region, argc > 1);
    std::puts("scalar RNG graph/program/interpreter/upstream checks passed");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
