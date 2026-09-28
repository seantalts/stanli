// Vector RNG migration: full lowering, upstream draws, failure and
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
Eigen::VectorXd upstream(int family, const Eigen::VectorXd& p,
                         const Eigen::MatrixXd& L, WaRng& stream) {
  auto& rng = stream.gen();
  int k = 0;
  switch (family) {
    case 0:
      k = stan::math::categorical_rng(p, rng);
      break;
    case 1:
      // The pinned upstream RNG has an unchecked empty-vector read. Stanli
      // adds this validation before calling it; never invoke its UB here.
      stan::math::check_nonzero_size("categorical_logit_rng",
                                     "Log odds parameter", p);
      k = stan::math::categorical_logit_rng(p, rng);
      break;
    case 2:
      k = stan::math::poisson_binomial_rng(p, rng);
      break;
    case 3:
      return stan::math::multi_normal_cholesky_rng(p, L, rng);
  }
  return Eigen::VectorXd::Constant(p.size(), k);
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
void exercise(int family, int region, int n, bool benchmark) {
  const auto text = slurp("tests/fixtures/gq_vector_rng_gap.tmir.sexp");
  const auto data = DataMap::from_json("{\"family\":" + std::to_string(family) +
                                       ",\"region\":" + std::to_string(region) +
                                       ",\"N\":" + std::to_string(n) + "}");
  auto cm = compile_model(text, data);
  if (cm.write_array && cm.write_array->interp)
    std::fprintf(stderr, "family %d region %d: %s\n", family, region,
                 cm.write_array->truncated.c_str());
  require(cm.write_array && !cm.write_array->interp &&
              cm.write_array->truncated.empty(),
          "vector RNG fell back during lowering");
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
  env["N"].is_int = true;
  env["N"].i = {n};
  env["N"].r = {double(n)};
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    env[flag].is_int = true;
    env[flag].i = {1};
    env[flag].r = {1};
  }
  WaInterp interp(prog, env);
  std::map<std::string, DataMap::Entry> params;
  Eigen::VectorXd p(n);
  Eigen::MatrixXd L(n, n);
  const auto set = [&] {
    for (int i = 0; i < n; ++i) graph.params_data()[i] = p[i];
    for (int i = 0; i < n * n; ++i) graph.params_data()[n + i] = L.data()[i];
    params["p"].r.assign(p.data(), p.data() + n);
    params["p"].dims = {n};
    params["L"].r.assign(L.data(), L.data() + n * n);
    params["L"].dims = {n, n};
  };
  const auto valid = [&](int i) {
    p = Eigen::VectorXd::Constant(n, 1.0 / std::max(1, n));
    if (family == 1 || family == 3) p.array() += 0.1 * i;
    if (family == 2) p.array() *= 0.1 * (i + 1);
    L = Eigen::MatrixXd::Identity(n, n);
    if (n > 1) L(1, 0) = 0.2 + 0.01 * i;
    set();
  };
  valid(0);
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
    std::vector<double> row;
    row.insert(row.end(), p.data(), p.data() + n);
    row.insert(row.end(), L.data(), L.data() + n * n);
    row.push_back(stan::math::normal_rng(0, 1, rng.gen()));
    Eigen::VectorXd draw = upstream(family, p, L, rng);
    if (region) draw = upstream(family, p, L, rng);
    row.insert(row.end(), draw.data(), draw.data() + n);
    row.push_back(stan::math::normal_rng(0, 1, rng.gen()));
    return row;
  };
  for (unsigned chain : {0u, 3u}) {
    WaRng gr(1234, chain), ir(1234, chain), dr(1234, chain);
    for (int i = 0; i < 12; ++i) {
      valid(i);
      std::vector<double> got, interpreted, expected;
      auto ge = failure([&] { got = compiled(gr); });
      auto ie = failure([&] { interpreted = interp.eval(params, ir); });
      auto de = failure([&] { expected = direct(dr); });
      require(ge == ie && ge == de, "valid/empty exception differs");
      if (family == 1 && n == 0)
        require(ge.kind == typeid(std::invalid_argument).name(),
                "empty logit RNG must reject with invalid_argument");
      require(same(got, interpreted), "compiled/interpreted draw bytes differ");
      require(same(got, expected), "compiled/upstream draw bytes differ");
      require(gr.gen() == ir.gen() && gr.gen() == dr.gen(),
              "RNG continuation differs");
    }
  }
  // Invalid input must preserve both the consumed prefix and the absence of
  // the suffix. Test recovery on the same executor and caller-owned stream.
  if (n)
    for (int mutation = 0; mutation < 8; ++mutation) {
      valid(0);
      const double inf = std::numeric_limits<double>::infinity();
      const double nan = std::numeric_limits<double>::quiet_NaN();
      if (mutation < 4)
        p[0] = std::vector<double>{-1, 2, inf, nan}[mutation];
      else if (mutation < 7)
        L(0, 0) = std::vector<double>{0, -1, nan}[mutation - 4];
      else if (n > 1)
        L(0, 1) = 1;
      set();
      WaRng gr(781), ir(781), dr(781);
      std::vector<double> mutated_graph, mutated_interp, mutated_direct;
      auto ge = failure([&] { mutated_graph = compiled(gr); });
      auto ie = failure([&] { mutated_interp = interp.eval(params, ir); });
      auto de = failure([&] { mutated_direct = direct(dr); });
      if (!(ge == ie && ge == de))
        std::fprintf(stderr,
                     "family %d region %d n %d mutation %d: %s / %s / %s\n",
                     family, region, n, mutation, ge.message.c_str(),
                     ie.message.c_str(), de.message.c_str());
      require(ge == ie && ge == de, "invalid-argument exception differs");
      if (ge.kind.empty())
        require(same(mutated_graph, mutated_interp) &&
                    same(mutated_graph, mutated_direct),
                "accepted mutation output bytes differ");
      require(gr.gen() == ir.gen() && gr.gen() == dr.gen(),
              "rejection stream differs");
      valid(0);
      const auto got = compiled(gr);
      require(same(got, interp.eval(params, ir)) && same(got, direct(dr)),
              "recovery row differs");
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
void udf_effect() {
  const auto text = slurp("tests/fixtures/gq_rng_udf_effect.tmir.sexp");
  auto cm = compile_model(text, DataMap{});
  require(cm.write_array && !cm.write_array->interp,
          "UDF effect fixture did not compile");
  auto prog =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> env;
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    env[flag].is_int = true;
    env[flag].i = {1};
    env[flag].r = {1};
  }
  WaInterp interp(prog, env);
  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  for (unsigned chain : {0u, 3u}) {
    WaRng gr(1234, chain), ir(1234, chain), dr(1234, chain);
    for (int i = 0; i < 12; ++i) {
      const double x = 0.1 * i;
      std::map<std::string, DataMap::Entry> params;
      params["x"].r = {x};
      graph.params_data()[0] = x;
      std::vector<double> expected{x, stan::math::normal_rng(0, 1, dr.gen()),
                                   stan::math::normal_rng(x, 1, dr.gen()),
                                   stan::math::normal_rng(0, 1, dr.gen())};
      const auto interpreted = interp.eval(params, ir);
      require(same(interpreted, expected),
              "UDF interpreter argument evaluated more than once");
      {
        ExecutionTrace trace;
        trace.forbid_mir = true;
        ExecutionTraceScope scope(trace);
        graph.run_forward_only(EvalState{&gr});
        trace.check();
      }
      std::vector<double> row;
      for (const auto& col : cm.write_array->columns)
        for (int64_t k = 0; k < col.len; ++k)
          row.push_back(graph.value_ptr(col.slot)[col.storage_index(k)]);
      require(same(row, expected),
              "UDF compiled argument evaluated more than once");
      require(gr.gen() == ir.gen() && gr.gen() == dr.gen(),
              "UDF argument stream differs");
    }
  }
}
}  // namespace
int main(int argc, char**) {
  try {
    udf_effect();
    for (int family = 0; family < 4; ++family)
      for (int region = 0; region < 2; ++region)
        for (int n : {0, 1, 3}) {
          std::fprintf(stderr, "case %d %d %d\n", family, region, n);
          exercise(family, region, n, argc > 1 && n == 3);
        }
    std::puts("vector RNG graph/program/interpreter/upstream checks passed");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
