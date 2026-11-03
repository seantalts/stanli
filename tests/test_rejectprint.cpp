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

  // ---- fatal_error throws std::runtime_error, never a rejection ---------
  {
    const std::string fatal = slurp("tests/fixtures/fatal_error.tmir.sexp");
    const auto n = [](int value) {
      return DataMap::from_json("{\"N\": " + std::to_string(value) + "}");
    };
    std::string msg;
    try {
      compile_model(fatal, n(-2));
    } catch (const std::runtime_error& e) {
      msg = e.what();
    }
    expect("a transformed-data fatal_error fails the compile: " + msg,
           msg.find("N is -2") != std::string::npos);
    for (const double x : {0.5, 20.0}) {
      CompiledModel cm = compile_model(fatal, n(x > 10 ? 3 : 200));
      Executor ex(std::move(cm.graph));
      cm.bind(ex);
      ex.params_data()[0] = x;
      std::vector<double> g(1);
      msg.clear();
      bool domain = false;
      try {
        ex.gradient(g.data());
      } catch (const std::domain_error&) {
        domain = true;
      } catch (const std::runtime_error& e) {
        msg = e.what();
      }
      const std::string want = x > 10 ? "x too large: 20" : "N too large: 200";
      expect("a model-block fatal_error throws runtime_error: " + msg,
             !domain && msg.find(want) != std::string::npos);
    }
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

  // ---- reject preserves prior output and prevents later effects -------
  {
    DataMap d = data_for(8, 2.5);
    CompiledModel cm = compile_model(mir, d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = 0.1;
    std::vector<double> g((size_t)ex.n_params());
    bool threw = false;
    std::string msg;
    std::vector<std::string> lines;

    set_message_sink([&lines](const char* text, size_t len) {
      lines.emplace_back(text, len);
    });
    try {
      ex.gradient(g.data());
    } catch (const std::domain_error& e) {
      threw = true;
      msg = e.what();
    }
    set_message_sink(nullptr);
    expect("ordered graph reject throws domain_error", threw);
    expect("graph reject renders its scalar message exactly: " + msg,
           msg == "stop scalar=0.1");
    expect("only one effect precedes graph reject", lines.size() == 1);
    if (lines.size() == 1)
      expect("effect before graph reject: " + lines[0],
             lines[0] == "before scalar=0.1");
  }

  // ---- parameter-dependent print executes only in the forward ---------
  // Necessity islands replay their register program for reverse mode. The
  // PRINT instruction is double-only, so the taken arm emits once during
  // the forward and is silent during the var replay.
  //
  // The generated-Stan oracle at x=0.1 and x=-0.04 is:
  //
  //   mode  x      target  grad  observable effect
  //      1   0.10    0.10     1  prints once
  //      1  -0.04   -0.04     1  none (print arm untaken)
  //      2   0.10    0.10     1  none (reject arm untaken)
  //      2  -0.04       -     -  throws domain_error once
  //
  {
    const std::string effect_mir =
        slurp("tests/fixtures/necessity_effects.tmir.sexp");
    DataMap d;
    d.set_int("mode", 1);
    CompiledModel cm = compile_model(effect_mir, d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    std::vector<double> g((size_t)ex.n_params());
    std::vector<std::string> lines;
    set_message_sink([&lines](const char* text, size_t len) {
      lines.emplace_back(text, len);
    });

    ex.params_data()[0] = 0.1;
    const double positive_lp = ex.gradient(g.data());
    expect("necessity print positive lp", positive_lp == 0.1);
    expect("necessity print positive gradient", g[0] == 1.0);
    expect("necessity print runs once, got " + std::to_string(lines.size()),
           lines.size() == 1);
    if (lines.size() == 1)
      expect("necessity print message: " + lines[0],
             lines[0] == "positive x=0.1");

    lines.clear();
    ex.params_data()[0] = -0.04;
    const double negative_lp = ex.gradient(g.data());
    expect("necessity print negative lp", negative_lp == -0.04);
    expect("necessity print negative gradient", g[0] == 1.0);
    expect("untaken necessity print is silent", lines.empty());
    set_message_sink(nullptr);
  }

  // An effect is sufficient reason to retain an island even when it has no
  // numeric live-out and contributes nothing to target.
  {
    const std::string print_only_mir =
        slurp("tests/fixtures/necessity_print_only.tmir.sexp");
    DataMap d;
    CompiledModel cm = compile_model(print_only_mir, d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    std::vector<double> g((size_t)ex.n_params());
    std::vector<std::string> lines;
    set_message_sink([&lines](const char* text, size_t len) {
      lines.emplace_back(text, len);
    });
    ex.params_data()[0] = 0.2;
    const double lp = ex.gradient(g.data());
    set_message_sink(nullptr);
    expect("print-only necessity island has zero lp", lp == 0.0);
    expect("print-only necessity island has zero gradient", g[0] == 0.0);
    expect("print-only necessity island is retained and executes once",
           lines.size() == 1 && lines[0] == "print-only x=0.2");
  }

  // A runtime-valued reject uses the same literal/value message template as
  // print. The untaken arm is silent and the taken arm preserves the rendered
  // value in the domain error.
  {
    DataMap d;
    d.set_int("mode", 2);
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/necessity_effects.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    std::vector<double> g((size_t)ex.n_params());

    ex.params_data()[0] = 0.1;
    const double accepted_lp = ex.gradient(g.data());
    expect("necessity untaken reject lp", accepted_lp == 0.1);
    expect("necessity untaken reject gradient", g[0] == 1.0);

    ex.params_data()[0] = -0.1;
    std::string message;
    try {
      (void)ex.gradient(g.data());
    } catch (const std::domain_error& e) {
      message = e.what();
    }
    expect("necessity dynamic reject message: " + message,
           message == "negative x=-0.1");
  }

  // A recursive cycle is not itself an effect, but an effect reachable in
  // that cycle still prevents compile-time evaluation. The untaken runtime
  // arm must therefore be silent, and the taken arm emits each print once.
  {
    std::vector<std::string> lines;
    set_message_sink([&lines](const char* text, size_t len) {
      lines.emplace_back(text, len);
    });
    DataMap d;
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/known_real_udf_effect.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    std::vector<double> g((size_t)ex.n_params());

    ex.params_data()[0] = 0.25;
    const double untaken_lp = ex.gradient(g.data());
    expect("effectful recursive UDF untaken lp",
           untaken_lp == -0.5 * 0.25 * 0.25);
    expect("effectful recursive UDF untaken gradient", g[0] == -0.25);
    expect("effectful recursive UDF is not evaluated during compilation",
           lines.empty());

    ex.params_data()[0] = 101.0;
    const double taken_lp = ex.gradient(g.data());
    set_message_sink(nullptr);
    expect("effectful recursive UDF taken lp",
           taken_lp == -0.5 * 101.0 * 101.0);
    expect("effectful recursive UDF taken gradient", g[0] == -101.0);
    expect("effectful recursive UDF emits exactly twice, got " +
               std::to_string(lines.size()),
           lines.size() == 2);
    if (lines.size() == 2) {
      expect("recursive effect remains ordered: " + lines[0],
             lines[0] == "recursive base");
      expect("outer recursive print remains ordered: " + lines[1],
             lines[1] == "known effectful recursive real: -0.5");
    }
  }

  if (failures == 0) std::printf("test_rejectprint: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
