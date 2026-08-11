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
#include <stanli/message_sink.hpp>

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

  // ---- print goes wherever the host says --------------------------------
  // Both print paths are exercised: the transformed-data one runs in the
  // MIR interpreter at compile time, the model-block one in the OP_PRINT
  // kernel at evaluation time. They were two separate writes to stdout,
  // which a host embedding the runtime cannot redirect or interleave with
  // its own output.
  {
    std::vector<std::string> lines;
    set_message_sink([&lines](const char* text, size_t len) {
      lines.emplace_back(text, len);
    });
    DataMap d = data_for(7, 2.5);
    CompiledModel cm = compile_model(mir, d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = 0.25;
    std::vector<double> g((size_t)ex.n_params());
    ex.gradient(g.data());
    set_message_sink(nullptr);

    expect("the sink saw both prints, got " + std::to_string(lines.size()),
           lines.size() == 2);
    if (lines.size() == 2) {
      expect("transformed-data print: " + lines[0],
             lines[0] == "compiled with N = 7");
      // The container prints in brackets and the scalar bare, CmdStan's
      // formatting, and the sink is handed the line without a newline.
      expect("model-block print: " + lines[1],
             lines[1] == "drawing at x = 0.25 v = [0.25,0.25]");
    }
    // Restoring the default must actually restore it: a later evaluation
    // writes to stdout again rather than to a dangling sink.
    ex.gradient(g.data());
  }

  if (failures == 0) std::printf("test_rejectprint: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
