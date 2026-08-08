// reject() and print().
//
// reject is the one with semantics: it throws std::domain_error, which is
// what CmdStan's generated code throws and what the sampler already reads
// as "this proposal is not valid" rather than as a failed run. It has to
// work in both places it can appear, and they are different machinery:
//
//   transformed data -- evaluated eagerly by the MIR interpreter at
//     LOWERING time, so a taken reject must stop the model compiling at
//     all, the way CmdStan fails to construct the model.
//   model block -- lowered to an op, so a taken reject must throw during
//     the forward sweep, once per evaluation.
//
// The failure that matters is silence: a reject that is skipped rather
// than taken lets stanli sample happily from a model CmdStan refuses.
#include <stanli/compile.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static int failures = 0;
static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main() {
  using namespace stanli;
  const std::string mir = slurp("tests/fixtures/rejectprint.tmir.sexp");

  const auto data_for = [](int n, double lim) {
    return DataMap::from_json("{\"N\": " + std::to_string(n) +
                              ", \"lim\": " + std::to_string(lim) + "}");
  };

  // ---- neither reject taken: the model compiles and evaluates ----------
  {
    DataMap d = data_for(3, 2.5);
    CompiledModel cm = compile_model(mir, d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    expect("3 parameters", ex.n_params() == 3);
    for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = 0.1;
    std::vector<double> g((size_t)ex.n_params());
    const double lp = ex.gradient(g.data());
    expect("evaluates when no reject is taken", std::isfinite(lp));
  }

  // ---- transformed-data reject taken: compilation itself must fail ----
  {
    bool threw = false;
    std::string msg;
    try {
      DataMap d = data_for(-1, 2.5);
      CompiledModel cm = compile_model(mir, d);
    } catch (const std::exception& e) {
      threw = true;
      msg = e.what();
    }
    expect("a taken transformed-data reject fails the compile", threw);
    // And it carries the user's message with the value interpolated --
    // which is the whole point of reject over a bare throw.
    expect("with the user's message: " + msg,
           msg.find("N must be nonnegative") != std::string::npos &&
               msg.find("-1") != std::string::npos);
  }

  // ---- model-block reject taken: evaluation must throw ----------------
  {
    DataMap d = data_for(200, 2.5);
    CompiledModel cm = compile_model(mir, d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = 0.1;
    std::vector<double> g((size_t)ex.n_params());
    bool threw = false;
    std::string msg;
    try {
      ex.gradient(g.data());
    } catch (const std::domain_error& e) {
      // domain_error specifically: the sampler distinguishes a rejected
      // proposal from a broken run by the exception type.
      threw = true;
      msg = e.what();
    }
    expect("a taken model-block reject throws domain_error", threw);
    expect("with every chunk and value: " + msg,
           msg.find("N too large: 200") != std::string::npos &&
               msg.find("limit 2.5") != std::string::npos);
  }

  if (failures == 0) std::printf("test_rejectprint: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
