// Container RNG admission, whole-argument validation and stream continuation.
#include <stanli/compile.hpp>
#include <stanli/execution_report.hpp>
#include <stanli/mir_decode.hpp>
#include <stanli/wa_interp.hpp>
#include <stan/math.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <typeinfo>

namespace {
using namespace stanli;
void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
std::string slurp(const char* path) {
  std::ifstream file(path);
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}
bool same(const std::vector<double>& a, const std::vector<double>& b) {
  return a.size() == b.size() &&
         (a.empty() ||
          std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}
template <class F>
std::string failure(F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    return std::string(typeid(e).name()) + ": " + e.what();
  }
  return {};
}

void exercise(int family, int mode, int region, std::vector<double> a,
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
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    env[flag].is_int = true;
    env[flag].i = {1};
    env[flag].r = {1};
  }
  auto prog =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  WaInterp interp(prog, env);
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
    std::vector<double> row;
    for (const auto& col : cm.write_array->columns)
      for (int64_t k = 0; k < col.len; ++k)
        row.push_back(graph.value_ptr(col.slot)[col.storage_index(k)]);
    return row;
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
              label + ": rejection differs\ngraph: " + ge + "\ninterp: " + ie +
                  "\nupstream: " + de);
      if (de.empty())
        require(same(actual, expected) && same(interpreted, expected),
                label + ": output differs");
      require(gr.gen() == dr.gen() && ir.gen() == dr.gen(),
              label + ": stream differs after call");
    }
  }
}
}  // namespace

int main() {
  try {
    for (int region : {0, 1}) {
      for (int family = 0; family < 4; ++family) {
        for (int n : {0, 1, 3}) {
          for (int mode : {1, 2, 3}) {
            if (family >= 2 && mode != 3) continue;
            exercise(family, mode, region, std::vector<double>(n, 1.3),
                     std::vector<double>(n, 0.4), std::vector<int>(n, 7));
          }
        }
        // An invalid second element must not consume the first draw. Repeat
        // the call on the same stream to check rejection/continuation too.
        exercise(family, 3, region, {1.3, -1}, {0.4, -1}, {7, -2});
        exercise(family, 3, region, {1.3, 1.7},
                 {0.4, std::numeric_limits<double>::quiet_NaN()}, {7, 9});
        // Length-one containers do not broadcast. Refusal still reaches
        // upstream validation through MIR, rather than silently accepting.
        exercise(family, 3, region, {1.3}, {0.4, 0.7}, {7}, false);
        exercise(family, 3, region, {}, {0.4}, {}, false);
      }
    }
    std::puts("container RNG graph/program/MIR/upstream checks passed");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
