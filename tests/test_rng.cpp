// RNG lowering: the compiled graph, the MIR interpreter and a direct Stan
// Math call on the same generator must produce the same draw bytes, the same
// rejection class and message, and leave the stream in the same place, for
// scalar, vector and container families.
#include "expect.hpp"

#include <stanli/compile.hpp>
#include <stanli/execution_report.hpp>
#include <stanli/mir_decode.hpp>
#include <stanli/wa_interp.hpp>
#include <stan/math.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <typeinfo>

namespace {
using namespace stanli;
using Env = std::map<std::string, DataMap::Entry>;

void require(bool ok, const std::string& what) {
  if (!ok) throw std::runtime_error(what);
}
struct Failure {
  std::string kind, message;
  bool operator==(const Failure& other) const {
    return kind == other.kind && message == other.message;
  }
  std::string str() const { return kind.empty() ? "" : kind + ": " + message; }
};
template <class F>
Failure failure(F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    return {typeid(e).name(), e.what()};
  }
  return {};
}
bool same(const std::vector<double>& a, const std::vector<double>& b) {
  return a.size() == b.size() &&
         (a.empty() ||
          std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}
void emit_everything(Env& env) {
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    env[flag].is_int = true;
    env[flag].i = {1};
    env[flag].r = {1};
  }
}
void set_int(Env& env, const char* name, int value) {
  env[name].is_int = true;
  env[name].i = {value};
  env[name].r = {double(value)};
}
std::vector<double> row_of(const Executor& graph, const CompiledModel& cm) {
  std::vector<double> row;
  for (const auto& col : cm.write_array->columns)
    for (int64_t i = 0; i < col.len; ++i)
      row.push_back(graph.value_ptr(col.slot)[col.storage_index(i)]);
  return row;
}
// The graph run with the MIR interpreter forbidden.
std::vector<double> run_graph_only(Executor& graph, const CompiledModel& cm,
                                   WaRng& rng) {
  ExecutionTrace trace;
  trace.forbid_mir = true;
  ExecutionTraceScope scope(trace);
  graph.run_forward_only(EvalState{&rng});
  trace.check();
  return row_of(graph, cm);
}
std::shared_ptr<mir::Program> program_of(const std::string& text) {
  return std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
}

double scalar_upstream(int family, double a, double b, WaRng& stream) {
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

// The container form uses the same vectorized Stan Math call.
void scalar_container_guard() {
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
  require(same(row_of(container_graph, container), expected),
          "container RNG changed");
  require(fallback_rng.gen() == direct_rng.gen(),
          "container fallback stream differs");
}

void scalar_family(int family, int region) {
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
  Env env;
  set_int(env, "family", family);
  set_int(env, "region", region);
  emit_everything(env);
  WaInterp interp(program_of(text), env);
  Env params;
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
    return run_graph_only(graph, cm, rng);
  };
  const auto direct = [&](WaRng& rng) {
    const double before = stan::math::normal_rng(0, 1, rng.gen());
    const double draw = scalar_upstream(family, a, b, rng);
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
}

Eigen::VectorXd vector_upstream(int family, const Eigen::VectorXd& p,
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

void vector_family(int family, int region, int n) {
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
  Env env;
  set_int(env, "family", family);
  set_int(env, "region", region);
  set_int(env, "N", n);
  emit_everything(env);
  WaInterp interp(program_of(text), env);
  Env params;
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
    return run_graph_only(graph, cm, rng);
  };
  const auto direct = [&](WaRng& rng) {
    std::vector<double> row;
    row.insert(row.end(), p.data(), p.data() + n);
    row.insert(row.end(), L.data(), L.data() + n * n);
    row.push_back(stan::math::normal_rng(0, 1, rng.gen()));
    Eigen::VectorXd draw = vector_upstream(family, p, L, rng);
    if (region) draw = vector_upstream(family, p, L, rng);
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
}

void udf_effect(bool early_exits) {
  const auto text =
      slurp(early_exits ? "tests/fixtures/gq_function_exits.tmir.sexp"
                        : "tests/fixtures/gq_rng_udf_effect.tmir.sexp");
  auto cm = compile_model(text, DataMap{});
  require(cm.write_array && !cm.write_array->interp,
          "UDF effect fixture did not compile");
  Env env;
  emit_everything(env);
  WaInterp interp(program_of(text), env);
  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  for (unsigned chain : {0u, 3u}) {
    WaRng gr(1234, chain), ir(1234, chain), dr(1234, chain);
    for (int i = 0; i < 12; ++i) {
      const double x = early_exits ? -1.1 + 0.2 * i : 0.1 * i;
      Env params;
      params["x"].r = {x};
      graph.params_data()[0] = x;
      std::vector<double> expected{x, stan::math::normal_rng(0, 1, dr.gen())};
      const double draw = !early_exits || x > 0
                              ? stan::math::normal_rng(x, 1, dr.gen())
                          : x < -0.5 ? stan::math::normal_rng(x, 2, dr.gen())
                          : x < -0.1 ? stan::math::uniform_rng(0, 1, dr.gen())
                                     : stan::math::normal_rng(0, 1, dr.gen());
      expected.push_back(draw);
      if (early_exits) expected.push_back(1);  // exported loop counter
      expected.push_back(stan::math::normal_rng(0, 1, dr.gen()));
      const auto interpreted = interp.eval(params, ir);
      require(same(interpreted, expected),
              "UDF interpreter argument evaluated more than once");
      require(same(run_graph_only(graph, cm, gr), expected),
              "UDF compiled argument evaluated more than once");
      require(gr.gen() == ir.gen() && gr.gen() == dr.gen(),
              "UDF argument stream differs");
    }
  }
}

void container_family(int family, int mode, int region, std::vector<double> a,
                      std::vector<double> b, std::vector<int> counts,
                      bool admitted = true) {
  const auto text = slurp("tests/fixtures/gq_container_rng_edges.tmir.sexp");
  DataMap data;
  data.set_int("N", a.size());
  data.set_int("M", b.size());
  data.set_int("family", family);
  data.set_int("mode", mode);
  data.set_int("region", region);
  data.set_int_array("counts", counts);
  data.set_real_array("b", b);
  auto cm = compile_model(text, data);
  const std::string label =
      "family=" + std::to_string(family) + " mode=" + std::to_string(mode) +
      " region=" + std::to_string(region) + " N=" + std::to_string(a.size()) +
      " M=" + std::to_string(b.size());
  require(cm.write_array.has_value(), label + ": missing output");
  require(bool(cm.write_array->interp) != admitted,
          label + ": unexpected admission: " + cm.write_array->truncated);
  if (admitted && region)
    require(execution_report(cm).find("register_program") != std::string::npos,
            label + ": missing register program");
  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  for (size_t k = 0; k < a.size(); ++k) graph.params_data()[k] = a[k];
  DataMap params;
  params.set_real_array("a", a);
  auto env = data.entries();
  emit_everything(env);
  WaInterp interp(program_of(text), env);
  const auto direct = [&](WaRng& stream) {
    auto& g = stream.gen();
    std::vector<double> row = a;
    row.push_back(stan::math::normal_rng(0, 1, g));
    const Eigen::VectorXd av =
        Eigen::Map<const Eigen::VectorXd>(a.data(), a.size());
    const auto append = [&](const auto& draws) {
      row.insert(row.end(), draws.begin(), draws.end());
    };
    if (family == 0) {
      if (mode == 1)
        append(stan::math::gamma_rng(av, 1.5, g));
      else if (mode == 2)
        append(stan::math::gamma_rng(1.5, b, g));
      else
        append(stan::math::gamma_rng(av, b, g));
    } else if (family == 1) {
      if (mode == 1)
        append(stan::math::binomial_rng(counts, 0.3, g));
      else if (mode == 2)
        append(stan::math::binomial_rng(7, b, g));
      else
        append(stan::math::binomial_rng(counts, b, g));
    } else if (family == 2)
      append(stan::math::normal_rng(av, b, g));
    else
      append(stan::math::beta_binomial_rng(counts, av, b, g));
    row.push_back(stan::math::normal_rng(0, 1, g));
    return row;
  };
  const auto compiled = [&](WaRng& stream) {
    if (!admitted)
      return cm.write_array->interp->eval(params.entries(), stream);
    graph.run_forward_only(EvalState{&stream});
    return row_of(graph, cm);
  };
  for (unsigned seed : {1u, 432u, 1234u}) {
    WaRng gr(seed), ir(seed), dr(seed);
    for (int repeat = 0; repeat < 3; ++repeat) {
      std::vector<double> actual, interpreted, expected;
      const auto ge = failure([&] { actual = compiled(gr); });
      const auto ie =
          failure([&] { interpreted = interp.eval(params.entries(), ir); });
      const auto de = failure([&] { expected = direct(dr); });
      require(ge == de && ie == de,
              label + ": rejection differs\ngraph: " + ge.str() +
                  "\ninterp: " + ie.str() + "\nupstream: " + de.str());
      if (de.kind.empty())
        require(same(actual, expected) && same(interpreted, expected),
                label + ": output differs");
      require(gr.gen() == dr.gen() && ir.gen() == dr.gen(),
              label + ": stream differs after call");
    }
  }
}

void container_families() {
  for (int region : {0, 1}) {
    for (int family = 0; family < 4; ++family) {
      for (int n : {0, 1, 3}) {
        for (int mode : {1, 2, 3}) {
          if (family >= 2 && mode != 3) continue;
          container_family(family, mode, region, std::vector<double>(n, 1.3),
                           std::vector<double>(n, 0.4), std::vector<int>(n, 7));
        }
      }
      // An invalid second element must not consume the first draw. Repeat
      // the call on the same stream to check rejection/continuation too.
      container_family(family, 3, region, {1.3, -1}, {0.4, -1}, {7, -2});
      container_family(family, 3, region, {1.3, 1.7},
                       {0.4, std::numeric_limits<double>::quiet_NaN()}, {7, 9});
      // Length-one containers do not broadcast. Refusal still reaches
      // upstream validation through MIR, rather than silently accepting.
      container_family(family, 3, region, {1.3}, {0.4, 0.7}, {7}, false);
      container_family(family, 3, region, {}, {0.4}, {}, false);
    }
  }
}

// WaRng is the BridgeStan-compatible public stream: it must track Stan's
// current rng_t and its create_rng(seed, chain=0) seeding convention. This
// catches the historical drift where sampling moved to mixmax but generated
// quantities silently stayed on ecuyer1988.
static void test_stan_rng_stream_contract() {
  using namespace stanli;
  constexpr unsigned seed = 1234;
  for (const unsigned chain : {0u, 1u, 17u}) {
    WaRng got(seed, chain);
    stan::rng_t want = stan::services::util::create_rng(seed, chain);
    for (int i = 0; i < 32; ++i) {
      if (got.gen()() != want()) {
        ++failures;
        std::printf(
            "FAIL WaRng does not match Stan rng_t at chain %u draw %d\n", chain,
            i);
        return;
      }
    }
    got.seed(seed, chain);
    want = stan::services::util::create_rng(seed, chain);
    if (got.gen()() != want()) {
      ++failures;
      std::printf(
          "FAIL WaRng reseed does not match Stan create_rng at chain %u\n",
          chain);
    }
  }
}

// Keep the shared helper a transparent call into Stan Math. In particular,
// endpoint results must not become shortcuts: Stan Math owns validation and
// however much of the engine it consumes, including at N=0 and p in {0,1}.
static void test_binomial_rng_helper_contract() {
  using namespace stanli;
  const auto helper_draw = [](int n, double p, WaRng& rng) {
    const double args[] = {static_cast<double>(n), p};
    return scalar_rng_draw(ScalarRng::Binomial, args, 2, rng);
  };
  const auto next = [](WaRng& rng) {
    return stan::math::uniform_rng(0.0, 1.0, rng.gen());
  };

  struct ValidCase {
    int n;
    double p;
  };
  const ValidCase valid[] = {
      {0, 0.0}, {0, 0.37}, {0, 1.0},  {9, -0.0},   {9, 0.0},
      {9, 1.0}, {20, 0.5}, {21, 0.5}, {100, 0.49}, {100, 0.51},
  };
  for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
    WaRng got_rng(static_cast<unsigned>(101 + i));
    WaRng want_rng(static_cast<unsigned>(101 + i));
    const double got = helper_draw(valid[i].n, valid[i].p, got_rng);
    const double want = static_cast<double>(
        stan::math::binomial_rng(valid[i].n, valid[i].p, want_rng.gen()));
    if (got != want || next(got_rng) != next(want_rng)) {
      ++failures;
      std::printf("FAIL binomial helper valid case %zu\n", i);
    }
  }

  // Both-invalid cases pin Stan Math's validation priority. The remaining
  // cases cover finite values just outside the interval, NaN, and infinities.
  const ValidCase invalid[] = {
      {-1, 0.4},
      {-1, -0.2},
      {4, -0.1},
      {4, std::nextafter(1.0, std::numeric_limits<double>::infinity())},
      {4, std::numeric_limits<double>::quiet_NaN()},
      {4, std::numeric_limits<double>::infinity()},
      {4, -std::numeric_limits<double>::infinity()},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    WaRng got_rng(static_cast<unsigned>(211 + i));
    WaRng want_rng(static_cast<unsigned>(211 + i));
    std::string got_message, want_message;
    try {
      (void)helper_draw(invalid[i].n, invalid[i].p, got_rng);
    } catch (const std::domain_error& e) {
      got_message = e.what();
    }
    try {
      (void)stan::math::binomial_rng(invalid[i].n, invalid[i].p,
                                     want_rng.gen());
    } catch (const std::domain_error& e) {
      want_message = e.what();
    }
    if (got_message.empty() || got_message != want_message ||
        next(got_rng) != next(want_rng)) {
      ++failures;
      std::printf("FAIL binomial helper invalid case %zu\n", i);
    }
  }
}

struct RngException {
  int kind = 0;
  std::string message;
};

template <typename F>
RngException capture_rng_exception(F&& f) {
  try {
    f();
  } catch (const std::invalid_argument& e) {
    return {1, e.what()};
  } catch (const std::domain_error& e) {
    return {2, e.what()};
  } catch (const std::logic_error& e) {
    return {3, e.what()};
  } catch (const std::exception& e) {
    return {4, e.what()};
  }
  return {};
}

static Eigen::VectorXd categorical_theta(const std::vector<double>& p) {
  Eigen::VectorXd theta(static_cast<Eigen::Index>(p.size()));
  for (size_t i = 0; i < p.size(); ++i)
    theta[static_cast<Eigen::Index>(i)] = p[i];
  return theta;
}

static Eigen::VectorXd multi_normal_location(const std::vector<double>& x) {
  Eigen::VectorXd out(static_cast<Eigen::Index>(x.size()));
  for (size_t i = 0; i < x.size(); ++i)
    out[static_cast<Eigen::Index>(i)] = x[i];
  return out;
}

static Eigen::MatrixXd multi_normal_covariance(const std::vector<double>& x,
                                               size_t rows, size_t cols) {
  Eigen::MatrixXd out(static_cast<Eigen::Index>(rows),
                      static_cast<Eigen::Index>(cols));
  for (size_t j = 0; j < cols; ++j)
    for (size_t i = 0; i < rows; ++i)
      out(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) =
          x.at(j * rows + i);
  return out;
}

static std::vector<double> direct_multi_normal_rng(
    const std::vector<double>& mu, const std::vector<double>& covariance,
    size_t rows, size_t cols, stanli::WaRng& rng) {
  const Eigen::VectorXd draw = stan::math::multi_normal_rng(
      multi_normal_location(mu),
      multi_normal_covariance(covariance, rows, cols), rng.gen());
  return std::vector<double>(draw.data(), draw.data() + draw.size());
}

// categorical_rng is unusual in this opcode family: one logical argument is
// a variable-length vector, while the result is still one Stan int. Keep the
// shared helper byte-for-byte on Stan Math's validation and engine schedule.
static void test_categorical_rng_helper_contract() {
  using namespace stanli;
  const std::vector<std::vector<double>> valid = {
      {1.0},
      {0.0, 1.0, 0.0},
      {-0.0, 0.0, 1.0},
      {0.125, 0.25, 0.375, 0.25},
      {0.5, 0.5, 0.0},
      {1.0 + 5e-9},  // inside Stan Math's simplex tolerance
  };
  for (size_t c = 0; c < valid.size(); ++c) {
    for (unsigned seed : {1u, 17u, 2026u}) {
      WaRng got_rng(seed + static_cast<unsigned>(c));
      WaRng want_rng(seed + static_cast<unsigned>(c));
      const int got =
          vector_integer_rng_draw(valid[c].data(), valid[c].size(), got_rng);
      const int want = stan::math::categorical_rng(categorical_theta(valid[c]),
                                                   want_rng.gen());
      const auto got_next = got_rng.gen()();
      const auto want_next = want_rng.gen()();
      if (got != want || got_next != want_next) {
        ++failures;
        std::printf("FAIL categorical helper valid case %zu seed %u\n", c,
                    seed);
      }
    }
  }

  // Sum validation precedes the element scan in check_simplex. The two
  // negative cases distinguish those paths; empty also pins invalid_argument
  // rather than domain_error. No rejected input may advance the stream.
  const std::vector<std::vector<double>> invalid = {
      {},
      {0.0},
      {0.2, 0.2},
      {1.0, -0.25, 0.25},
      {1.0, -0.25, 0.5},
      {std::numeric_limits<double>::quiet_NaN()},
      {std::numeric_limits<double>::infinity()},
  };
  for (size_t c = 0; c < invalid.size(); ++c) {
    WaRng got_rng(static_cast<unsigned>(401 + c));
    WaRng want_rng(static_cast<unsigned>(401 + c));
    const RngException got = capture_rng_exception([&] {
      (void)vector_integer_rng_draw(
          invalid[c].empty() ? nullptr : invalid[c].data(), invalid[c].size(),
          got_rng);
    });
    const RngException want = capture_rng_exception([&] {
      (void)stan::math::categorical_rng(categorical_theta(invalid[c]),
                                        want_rng.gen());
    });
    const auto got_next = got_rng.gen()();
    const auto want_next = want_rng.gen()();
    if (got.kind == 0 || got.kind != want.kind || got.message != want.message ||
        got_next != want_next) {
      ++failures;
      std::printf("FAIL categorical helper invalid case %zu\n", c);
    }
  }

  // This is the graph/helper ABI guard, not a Stan language input: a nonempty
  // descriptor must carry storage. It must fail before touching the engine.
  WaRng malformed(991), untouched(991);
  const RngException null_input = capture_rng_exception(
      [&] { (void)vector_integer_rng_draw(nullptr, 1, malformed); });
  if (null_input.kind != 3 || malformed.gen()() != untouched.gen()()) {
    ++failures;
    std::printf("FAIL categorical helper malformed pointer contract\n");
  }
}

static void test_multi_normal_rng_helper_contract() {
  using namespace stanli;
  struct Valid {
    std::vector<double> mu;
    std::vector<double> covariance;
  };
  const Valid valid[] = {
      {{-0.25}, {1.75}},
      {{0.5, -1.25}, {1.69, 0.364, 0.364, 0.64}},
      {{0.25, -0.5, 1.75}, {2.0, 0.25, -0.4, 0.25, 1.5, 0.3, -0.4, 0.3, 1.25}},
  };
  for (size_t c = 0; c < sizeof(valid) / sizeof(valid[0]); ++c) {
    for (unsigned seed : {1u, 29u, 2026u}) {
      const size_t k = valid[c].mu.size();
      std::vector<double> got(k);
      WaRng got_rng(seed + static_cast<unsigned>(c));
      WaRng want_rng(seed + static_cast<unsigned>(c));
      multi_normal_rng_draw(valid[c].mu.data(), k, valid[c].covariance.data(),
                            valid[c].covariance.size(), k, k, got.data(),
                            got.size(), got_rng);
      const std::vector<double> want = direct_multi_normal_rng(
          valid[c].mu, valid[c].covariance, k, k, want_rng);
      if (!same(got, want) || got_rng.gen()() != want_rng.gen()()) {
        ++failures;
        std::printf("FAIL multi-normal helper valid case %zu seed %u\n", c,
                    seed);
      }
    }
  }

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  struct Invalid {
    std::vector<double> mu;
    std::vector<double> covariance;
    size_t rows;
    size_t cols;
    const char* label;
  };
  // The asymmetric case uses four distinct column-major positions. Its Stan
  // message names both off-diagonal values, catching a transposed rebuild.
  const Invalid invalid[] = {
      {{nan, 0.0}, {1.0, 0.0, 0.0, 1.0}, 2, 2, "NaN mean"},
      {{inf, 0.0}, {1.0, 0.0, 0.0, 1.0}, 2, 2, "infinite mean"},
      {{0.0, 0.0}, {1.0, 0.0, nan, 1.0}, 2, 2, "NaN covariance"},
      {{0.0, 0.0}, {1.25, 0.75, 0.125, 0.8}, 2, 2, "asymmetric covariance"},
      {{0.0, 0.0}, {1.0, 2.0, 2.0, 1.0}, 2, 2, "non-PD covariance"},
      {{0.0, 0.0}, {1.0, 0.0, 0.0, 1.0}, 1, 4, "dimension mismatch"},
      {{}, {}, 0, 0, "empty covariance"},
  };
  for (size_t c = 0; c < sizeof(invalid) / sizeof(invalid[0]); ++c) {
    std::vector<double> got(invalid[c].mu.size());
    WaRng got_rng(static_cast<unsigned>(501 + c));
    WaRng want_rng(static_cast<unsigned>(501 + c));
    const RngException got_error = capture_rng_exception([&] {
      multi_normal_rng_draw(invalid[c].mu.data(), invalid[c].mu.size(),
                            invalid[c].covariance.data(),
                            invalid[c].covariance.size(), invalid[c].rows,
                            invalid[c].cols, got.data(), got.size(), got_rng);
    });
    const RngException want_error = capture_rng_exception([&] {
      (void)direct_multi_normal_rng(invalid[c].mu, invalid[c].covariance,
                                    invalid[c].rows, invalid[c].cols, want_rng);
    });
    if (got_error.kind == 0 || got_error.kind != want_error.kind ||
        got_error.message != want_error.message ||
        got_rng.gen()() != want_rng.gen()()) {
      ++failures;
      std::printf("FAIL multi-normal helper %s contract\n", invalid[c].label);
    }
  }

  const std::vector<double> mu{0.5, -1.25};
  const std::vector<double> covariance{1.69, 0.364, 0.364, 0.64};
  std::vector<double> output(2);
  WaRng malformed(991), untouched(991);
  const RngException mismatch = capture_rng_exception([&] {
    multi_normal_rng_draw(mu.data(), mu.size(), covariance.data(),
                          covariance.size(), 1, 3, output.data(), output.size(),
                          malformed);
  });
  const RngException null_location = capture_rng_exception([&] {
    multi_normal_rng_draw(nullptr, mu.size(), covariance.data(),
                          covariance.size(), 2, 2, output.data(), output.size(),
                          malformed);
  });
  if (mismatch.kind != 3 || null_location.kind != 3 ||
      malformed.gen()() != untouched.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal malformed helper contract\n");
  }

  // A rejected helper call is reusable at the exact untouched draw.
  WaRng recovered(811), direct(811);
  std::vector<double> recovered_draw(2);
  (void)capture_rng_exception([&] {
    const std::vector<double> bad{1.0, 2.0, 2.0, 1.0};
    multi_normal_rng_draw(mu.data(), 2, bad.data(), 4, 2, 2,
                          recovered_draw.data(), 2, recovered);
  });
  multi_normal_rng_draw(mu.data(), 2, covariance.data(), 4, 2, 2,
                        recovered_draw.data(), 2, recovered);
  const std::vector<double> direct_draw =
      direct_multi_normal_rng(mu, covariance, 2, 2, direct);
  if (recovered_draw != direct_draw || recovered.gen()() != direct.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal helper rejected-call reuse\n");
  }
}

static void test_multi_normal_rng_kernel_contract() {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_RNG);
  if (kernel == nullptr || kernel->forward == nullptr) {
    ++failures;
    std::printf("FAIL multi-normal RNG kernel is not registered\n");
    return;
  }
  double mu[] = {0.5, -1.25};
  double covariance[] = {1.69, 0.364, 0.364, 0.64};
  double output[] = {-99.0, -99.0};
  int k = 2;
  WaRng rng(123), direct_rng(123);
  EvalState state{&rng};
  KernelCtx valid;
  valid.variant = kMultiNormalRngVariant;
  valid.n_in = 2;
  valid.in[0] = Desc{mu, 2};
  valid.in[1] = Desc{covariance, 4};
  valid.out = Desc{output, 2};
  valid.idata = &k;
  valid.n_idata = 1;
  valid.eval_state = &state;
  kernel->forward(valid);
  const std::vector<double> want = direct_multi_normal_rng(
      {mu[0], mu[1]}, {1.69, 0.364, 0.364, 0.64}, 2, 2, direct_rng);
  if (std::vector<double>(output, output + 2) != want ||
      rng.gen()() != direct_rng.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal RNG valid kernel contract\n");
  }

  struct Mutation {
    const char* label;
    void (*apply)(KernelCtx&);
  };
  const Mutation malformed[] = {
      {"unknown variant", [](KernelCtx& c) { c.variant = 255; }},
      {"arity", [](KernelCtx& c) { c.n_in = 1; }},
      {"missing idata", [](KernelCtx& c) { c.n_idata = 0; }},
      {"null idata", [](KernelCtx& c) { c.idata = nullptr; }},
      {"mean length", [](KernelCtx& c) { c.in[0].len = 1; }},
      {"covariance length", [](KernelCtx& c) { c.in[1].len = 3; }},
      {"output length", [](KernelCtx& c) { c.out.len = 1; }},
      {"missing evaluation state",
       [](KernelCtx& c) { c.eval_state = nullptr; }},
  };
  for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
    WaRng got(static_cast<unsigned>(900 + i));
    WaRng untouched(static_cast<unsigned>(900 + i));
    EvalState got_state{&got};
    KernelCtx ctx = valid;
    ctx.eval_state = &got_state;
    output[0] = output[1] = -99.0;
    malformed[i].apply(ctx);
    const RngException error =
        capture_rng_exception([&] { kernel->forward(ctx); });
    if (error.kind != 3 || got.gen()() != untouched.gen()() ||
        output[0] != -99.0 || output[1] != -99.0) {
      ++failures;
      std::printf("FAIL multi-normal malformed kernel %s\n",
                  malformed[i].label);
    }
  }
}
}  // namespace

int main() {
  try {
    scalar_container_guard();
    for (int family = 0; family < 11; ++family)
      for (int region = 0; region < 2; ++region) scalar_family(family, region);
    udf_effect(false);
    udf_effect(true);
    for (int family = 0; family < 4; ++family)
      for (int region = 0; region < 2; ++region)
        for (int n : {0, 1, 3}) {
          std::fprintf(stderr, "case %d %d %d\n", family, region, n);
          vector_family(family, region, n);
        }
    container_families();
    test_stan_rng_stream_contract();
    test_binomial_rng_helper_contract();
    test_categorical_rng_helper_contract();
    test_multi_normal_rng_helper_contract();
    test_multi_normal_rng_kernel_contract();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
  return finish("test_rng");
}
