// reduce_sum's serial lowering.
//
// Stan Math without STAN_THREADS makes exactly one call over the whole slice
// and returns zero for an empty one (prim/functor/reduce_sum.hpp), so stanli
// lowers reduce_sum to that same single call. That is not an approximation to
// be reconciled later: it agrees with default CmdStan term for term. This
// test pins the three things a successful compile would not catch.
//
// 1. The rewritten call is BITWISE the same terms written without
//    reduce_sum. A tolerance here would hide a real change of partition.
// 2. propto rides on the functor's spelling. `partial_sum_lupdf` and
//    `partial_sum_lpdf` name one definition, and only the `_lupdf` form may
//    drop normal's normalizing constant -- the reference for that is the
//    generated rsfunctor's propto__ argument in CmdStan. Getting it
//    backwards still compiles, still samples, and is silently the wrong
//    density. Check (1) covers this because reduce_sum_equiv.stan spells
//    each term's normalization out, so the two models can only agree if
//    both spellings lowered the way CmdStan reads them.
// 3. An empty slice never lowers its callee. The fixture's callee rejects,
//    so a lowering that reached it leaves an OP_REJECT in the graph.
#include <stanli/compile.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/optable.hpp>
#include <stanli/reduce_sum.hpp>
#include <stanli/nuts.hpp>
#include <stanli/graph_print.hpp>
#include <thread>
#include <atomic>

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

// The same deterministic walk stanli_check uses, so a failure here can be
// reproduced from the command line against the same fixture.
double eval_point(int64_t i) {
  return 0.1 + 0.05 * (double)(i % 7) - 0.15 * (double)(i % 3);
}

struct Run {
  double lp = 0;
  std::vector<double> grad;
};

Run evaluate(stanli::CompiledModel& cm) {
  using namespace stanli;
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  const int64_t n = ex.n_params();
  Run run;
  run.grad.assign((size_t)n, 0.0);
  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = eval_point(i);
  run.lp = ex.gradient(run.grad.data());
  return run;
}

int count_opcode(const stanli::Graph& g, uint16_t opcode) {
  int n = 0;
  for (const stanli::Op& op : g.ops)
    if (op.opcode == opcode) ++n;
  return n;
}

void argument_evaluation(bool parallel = false) {
  using namespace stanli;
  const std::string mir =
      slurp("tests/fixtures/reduce_sum_arguments.tmir.sexp");
  for (int in_td : {0, 1})
    for (int n : {0, 2})
      for (int grain : {0, 1})
        for (int refuse : {0, 1}) {
          const std::string tag = "arguments td=" + std::to_string(in_td) +
                                  " n=" + std::to_string(n) +
                                  " grain=" + std::to_string(grain) +
                                  " refuse=" + std::to_string(refuse);
          DataMap data =
              DataMap::from_json("{\"N\":" + std::to_string(n) +
                                 ",\"y\":" + (n == 0 ? "[]" : "[0.5,-0.2]") +
                                 ",\"grainsize\":" + std::to_string(grain) +
                                 ",\"refuse\":" + std::to_string(refuse) +
                                 ",\"in_td\":" + std::to_string(in_td) + "}");
          std::vector<std::string> lines;
          std::string error;
          bool evaluating = false;
          bool domain_error = false;
          set_message_sink([&](const char* text, size_t len) {
            lines.emplace_back(text, len);
          });
          try {
            CompileOptions options;
            options.reduce_sum_threads = parallel ? 4 : 1;
            options.reduce_sum_min_elements = 0;
            CompiledModel cm = compile_model(mir, data, 1, options);
            if (!in_td)
              expect(tag + " no effects while lowering", lines.empty());
            Executor ex(std::move(cm.graph));
            cm.bind(ex);
            ex.params_data()[0] = 0.25;
            double grad;
            evaluating = true;
            expect(tag + " finite lp", std::isfinite(ex.gradient(&grad)));
            if (!in_td) {
              const auto first = lines;
              lines.clear();
              ex.gradient(&grad);
              expect(tag + " effects once per evaluation", lines == first);
            }
          } catch (const std::domain_error& e) {
            domain_error = true;
            error = e.what();
          } catch (const std::exception& e) {
            error = e.what();
          }
          set_message_sink(nullptr);
          expect(tag + " rejection", !error.empty() == (grain == 0 || refuse));
          if (!error.empty()) {
            expect(tag + " error: " + error,
                   error.find(refuse ? "shared argument rejected"
                                     : "grainsize") != std::string::npos);
            expect(tag + " correct execution phase", evaluating == !in_td);
            if (!in_td) expect(tag + " runtime domain_error", domain_error);
          }
          // C++ leaves argument order unspecified; require both evaluations
          // exactly once before entering the partial-sum body or validating.
          expect(tag + " grain evaluated once",
                 std::count(lines.begin(), lines.end(),
                            "grain=" + std::to_string(grain)) == 1);
          expect(tag + " shared evaluated once",
                 std::count(lines.begin(), lines.end(), "shared=0.25") == 1);
          const bool body_runs = n > 0 && grain > 0 && !refuse;
          expect(tag + " partial body only when required",
                 lines.size() == (body_runs ? 3u : 2u));
          if (body_runs && !lines.empty())
            expect(tag + " whole-slice bounds",
                   lines.back() == "partial bounds=1:2");
        }
}

void shapes_and_overloads(bool parallel = false) {
  using namespace stanli;
  CompileOptions options;
  options.reduce_sum_threads = parallel ? 4 : 1;
  options.reduce_sum_min_elements = 0;
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/reduce_sum_shapes.tmir.sexp"),
                    DataMap{}, 1, options);
  if (parallel && thread_safe_build())
    expect("nested outer reduction retained",
           count_opcode(cm.graph, OP_REDUCE_SUM) == 2);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  expect("shape fixture parameter count", ex.n_params() == 4);
  if (ex.n_params() != 4) return;
  for (double scale : {-0.5, 0.0, 1.3}) {
    double total = 0;
    for (int i = 0; i < 4; ++i) {
      ex.params_data()[i] = scale * (i + 1);
      if (i < 3) total += ex.params_data()[i];
    }
    const double b = ex.params_data()[3];
    double grad[4];
    const double lp = ex.gradient(grad);
    // The independent polynomial oracle is 3*b*sum(z) + 6*b. The tiny
    // absolute tolerance allows the different addition groupings only.
    expect("nested/overloaded lp", std::abs(lp - b * (3 * total + 6)) < 1e-12);
    for (int i = 0; i < 3; ++i)
      expect("active sliced gradient", std::abs(grad[i] - 3 * b) < 1e-12);
    expect("shared gradient", std::abs(grad[3] - (3 * total + 6)) < 1e-12);
  }
}

void native_review_regressions() {
  using namespace stanli;
  if (!thread_safe_build()) return;
  CompileOptions options;
  options.reduce_sum_threads = 4;
  options.reduce_sum_min_elements = 0;
  ReduceExecutionContext team(4);
  const auto discrete =
      slurp("tests/fixtures/reduce_sum_native_discrete.tmir.sexp");
  auto data = DataMap::from_json("{\"N\":7,\"y\":[0,1,2,3,4,5,6]}");
  auto cm = compile_model(discrete, data, 1, options);
  expect("integer density two retained sites",
         count_opcode(cm.graph, OP_REDUCE_SUM) == 2);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  ex.set_reduce_context(&team);
  for (double b : {-1.3, 0., .8}) {
    std::vector<std::string> messages;
    set_message_sink(
        [&](const char* p, size_t n) { messages.emplace_back(p, n); });
    ex.params_data()[0] = b;
    double grad;
    const double lp = ex.gradient(&grad);
    set_message_sink(nullptr);
    expect("accepted callback argument effects once",
           messages == std::vector<std::string>{"shared argument"});
    double normalizer = 0;
    for (int y = 0; y < 7; ++y) normalizer += std::lgamma(y + 1);
    const double expected =
        -.5 * b * b + 2 * (21 * b - 7 * std::exp(b)) - normalizer;
    expect("integer normalized/propto oracle lp",
           std::abs(lp - expected) < 1e-11);
    expect("integer normalized/propto oracle gradient",
           std::abs(grad - (-b + 2 * (21 - 7 * std::exp(b)))) < 1e-11);
  }
  const auto bounded =
      slurp("tests/fixtures/reduce_sum_native_bounded.tmir.sexp");
  auto bc = compile_model(bounded, data, 1, options);
  expect("bounded specialization retains threading option",
         count_opcode(bc.graph, OP_REDUCE_SUM) == 1);
  Executor be(std::move(bc.graph));
  bc.bind(be);
  be.set_reduce_context(&team);
  be.params_data()[0] = .2;
  double grad;
  expect("bounded reduction oracle lp",
         std::abs(be.gradient(&grad) - (-.5 * 1.4 * 1.4 + 21 * .2)) < 1e-12);
  expect("bounded reduction oracle grad",
         std::abs(grad - (-49 * .2 + 21)) < 1e-12);
  // A forced chunk budget refusal must remain visible to callers.
  options.reduce_sum_max_chunks = 1;
  auto refused = compile_model(bounded, data, 1, options);
  expect("budget refusal is reported", !refused.reduce_sum_fallbacks.empty());
  expect("budget refusal is serial",
         count_opcode(refused.graph, OP_REDUCE_SUM) == 0);
}

void native_lifecycle() {
  using namespace stanli;
  if (!thread_safe_build()) return;
  ReduceExecutionContext team(4);
  struct Jobs {
    std::atomic<int> completed{0};
  } jobs;
  bool threw = false;
  try {
    team.run(13, &jobs, [](void* state, size_t i) {
      ++static_cast<Jobs*>(state)->completed;
      if (i == 2 || i == 9) throw std::domain_error(std::to_string(i));
    });
  } catch (const std::domain_error& e) {
    threw = std::string(e.what()) == "2";
  }
  expect("team drains and selects first error", threw && jobs.completed == 13);
  threw = false;
  try {
    team.run(1, &team, [](void* state, size_t) {
      static_cast<ReduceExecutionContext*>(state)->run(0, nullptr, nullptr);
    });
  } catch (const std::logic_error&) {
    threw = true;
  }
  expect("team rejects reentry", threw);
  team.run(0, nullptr, nullptr);
  DataMap data;
  data.set_int("N", 257);
  data.set_int("P", 3);
  data.set_int("active_slice", 0);
  data.set_int("kind", 1);
  data.set_int("chunk", 0);
  data.set_int("start", 1);
  data.set_int("end", 257);
  data.set_real_array("x", std::vector<double>(257, .1));
  data.set_real_array("y", std::vector<double>(257, .3));
  CompileOptions options;
  options.reduce_sum_threads = 4;
  options.reduce_sum_min_elements = 0;
  auto cm =
      compile_model(slurp("tests/fixtures/reduce_sum_parallel_probe.tmir.sexp"),
                    data, 1, options);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  ex.set_reduce_context(&team);
  double grad[3];
  std::fill_n(ex.params_data(), 3, .1);
  const double good = ex.gradient(grad);
  ex.params_data()[1] = -1000;
  threw = false;
  try {
    ex.gradient(grad);
  } catch (const std::domain_error&) {
    threw = true;
  }
  expect("worker domain error reaches caller", threw);
  threw = false;
  try {
    ex.reverse(grad, 1);
  } catch (const std::logic_error&) {
    threw = true;
  }
  expect("failed forward cannot reverse", threw);
  ex.params_data()[1] = .1;
  expect("worker recovery", ex.gradient(grad) == good);
  Executor clone(ex);
  ReduceExecutionContext other(2);
  clone.set_reduce_context(&other);
  std::atomic<bool> same{true};
  auto stress = [&](Executor& executor) {
    double gradient[3];
    for (int i = 0; i < 500; ++i)
      if (executor.gradient(gradient) != good) same = false;
  };
  std::thread a([&] { stress(ex); }), b([&] { stress(clone); });
  a.join();
  b.join();
  expect("concurrent chains retain private state", same);
  NutsConfig config;
  config.warmup = 10;
  config.samples = 10;
  config.max_depth = 3;
  auto sequential = run_nuts_chains({&ex, &clone}, config, 1);
  auto parallel = run_nuts_chains({&ex, &clone}, config, 2);
  for (size_t i = 0; i < 2; ++i) {
    expect("threaded NUTS succeeds",
           sequential[i].error.empty() && parallel[i].error.empty());
    expect("threaded NUTS repeatability",
           sequential[i].draws == parallel[i].draws);
    expect("threaded NUTS draw count", parallel[i].draws.size() == 10);
  }
}

void native_corners() {
  using namespace stanli;
  if (!thread_safe_build()) return;
  const auto mir = slurp("tests/fixtures/reduce_sum_native.tmir.sexp");
  CompileOptions options;
  options.reduce_sum_threads = 4;
  options.reduce_sum_min_elements = 0;
  ReduceExecutionContext team(4);
  for (int fixed : {0, 1})
    for (int dynamic : {0, 1})
      for (int grain : {1, 4, 20}) {
        auto data = DataMap::from_json(
            "{\"N\":13,\"grain\":" + std::to_string(grain) +
            ",\"fixed_partition\":" + std::to_string(fixed) +
            ",\"dynamic_index\":" + std::to_string(dynamic) + "}");
        auto cm = compile_model(mir, data, 1, options);
        auto ref = compile_model(mir, data);
        const bool retained = !dynamic && grain < 13;
        expect("corners retain/refuse",
               count_opcode(cm.graph, OP_REDUCE_SUM) == int(retained));
        if (!retained) {
          expect("refusal reason is reported",
                 !cm.reduce_sum_fallbacks.empty());
          std::string a, b;
          GraphPrintInfo ai, bi;
          ai.fills = &cm.fills;
          bi.fills = &ref.fills;
          print_graph(a, cm.graph, ai);
          print_graph(b, ref.graph, bi);
          expect("refusal preserves graph and fills",
                 a == b && cm.fills == ref.fills);
        }
        Executor ex(std::move(cm.graph)), reference(std::move(ref.graph));
        cm.bind(ex);
        ref.bind(reference);
        ex.set_reduce_context(&team);
        std::vector<double> got(ex.n_params()), want(ex.n_params());
        for (double scale : {-0.7, 0.0, .3}) {
          double zsum = 0, bsum = 0, coefficient = 0;
          for (int i = 0; i < ex.n_params(); ++i) {
            const double value = scale + .02 * i;
            ex.params_data()[i] = reference.params_data()[i] = value;
            if (i < 13)
              zsum += value;
            else {
              bsum += value;
              coefficient += (i < 19 ? 3 : 2) * value;
            }
          }
          const double actual = ex.gradient(got.data());
          const double expected = reference.gradient(want.data());
          expect("nonlinear parent lp", std::abs(actual - expected) <
                                            1e-10 * (1 + std::abs(expected)));
          for (size_t i = 0; i < got.size(); ++i)
            expect(
                "nonlinear parent grad",
                std::abs(got[i] - want[i]) < 1e-10 * (1 + std::abs(want[i])));
          if (!dynamic) {
            const double r = zsum * coefficient;
            expect("polynomial oracle lp",
                   std::abs(actual - (-.5 * r * r + zsum + bsum)) <
                       1e-10 * (1 + r * r));
            for (size_t i = 0; i < got.size(); ++i) {
              const double oracle =
                  1 - r * (i < 13 ? coefficient : zsum * (i < 19 ? 3 : 2));
              expect(
                  "polynomial oracle grad",
                  std::abs(got[i] - oracle) < 1e-10 * (1 + std::abs(oracle)));
            }
          }
          const auto first = got;
          for (int repeat = 0; repeat < 10; ++repeat)
            expect("partition repeatability",
                   ex.gradient(got.data()) == actual && got == first);
        }
      }
  // Existing normalized/propto spellings and transformed-data behavior.
  auto data = DataMap::from_json_file("tests/fixtures/reduce_sum.json");
  auto cm = compile_model(slurp("tests/fixtures/reduce_sum.tmir.sexp"), data, 1,
                          options);
  expect("propto retained calls", count_opcode(cm.graph, OP_REDUCE_SUM) > 0);
  auto serial =
      compile_model(slurp("tests/fixtures/reduce_sum.tmir.sexp"), data);
  const auto got = evaluate(cm), want = evaluate(serial);
  expect("parallel propto lp",
         std::abs(got.lp - want.lp) < 1e-10 * (1 + std::abs(want.lp)));
  for (size_t i = 0; i < got.grad.size(); ++i)
    expect("parallel propto gradient",
           std::abs(got.grad[i] - want.grad[i]) <
               1e-10 * (1 + std::abs(want.grad[i])));
}

void native_parallel() {
  using namespace stanli;
  if (!thread_safe_build()) return;
  const auto mir = slurp("tests/fixtures/reduce_sum_parallel_probe.tmir.sexp");
  for (int n : {0, 1, 7, 257})
    for (int active : {0, 1})
      for (int kind : {0, 1}) {
        std::ostringstream json;
        json << "{\"N\":" << n << ",\"P\":4,\"active_slice\":" << active
             << ",\"kind\":" << kind
             << ",\"chunk\":0,\"start\":1,\"end\":" << n;
        for (auto name : {"x", "y"}) {
          json << ",\"" << name << "\":[";
          for (int i = 0; i < n; ++i) json << (i ? "," : "") << eval_point(i);
          json << "]";
        }
        json << "}";
        auto data = DataMap::from_json(json.str());
        CompileOptions options;
        options.reduce_sum_threads = 4;
        options.reduce_sum_min_elements = 0;
        auto cm = compile_model(mir, data, 1, options);
        expect("native retained opcode n=" + std::to_string(n),
               count_opcode(cm.graph, OP_REDUCE_SUM) == (n > 1 ? 1 : 0));
        auto serial = compile_model(mir, data);
        Executor reference(std::move(serial.graph));
        serial.bind(reference);
        Executor ex(std::move(cm.graph));
        cm.bind(ex);
        ReduceExecutionContext team(4);
        ex.set_reduce_context(&team);
        Executor clone(ex);
        ReduceExecutionContext clone_team(2);
        clone.set_reduce_context(&clone_team);
        std::vector<double> expected(ex.n_params()), got(ex.n_params());
        for (int point = 0; point < 4; ++point) {
          for (int i = 0; i < ex.n_params(); ++i)
            ex.params_data()[i] = reference.params_data()[i] =
                clone.params_data()[i] = eval_point(i + point) + .03 * point;
          const double lp = reference.gradient(expected.data());
          for (auto* candidate : {&ex, &clone}) {
            const double actual = candidate->gradient(got.data());
            expect("native lp",
                   std::abs(actual - lp) < 1e-10 * (1 + std::abs(lp)));
            for (size_t i = 0; i < got.size(); ++i)
              expect("native gradient " + std::to_string(i),
                     std::abs(got[i] - expected[i]) <
                         1e-10 * (1 + std::abs(expected[i])));
            expect("native value-only",
                   candidate->forward_value_only() == actual);
            bool refused = false;
            try {
              candidate->reverse(got.data(), 1);
            } catch (const std::logic_error&) {
              refused = true;
            }
            expect("value-only invalidates reverse", refused);
            candidate->forward();
            candidate->reverse(got.data(), -0.37);
            for (size_t i = 0; i < got.size(); ++i)
              expect("native seeded reverse",
                     std::abs(got[i] + .37 * expected[i]) <
                         1e-10 * (1 + std::abs(expected[i])));
            refused = false;
            try {
              candidate->reverse(got.data(), 1);
            } catch (const std::logic_error&) {
              refused = true;
            }
            expect("reverse consumes forward", refused);
            candidate->forward();
            candidate->reverse(got.data(), 0.0);
            expect("zero seed clears every gradient",
                   std::all_of(got.begin(), got.end(),
                               [](double value) { return value == 0.0; }));
          }
        }
      }
}

}  // namespace

int main() {
  using namespace stanli;

  native_review_regressions();
  native_lifecycle();
  native_corners();
  native_parallel();
  argument_evaluation();
  shapes_and_overloads();
  argument_evaluation(true);
  shapes_and_overloads(true);

  DataMap data = DataMap::from_json_file("tests/fixtures/reduce_sum.json");
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/reduce_sum.tmir.sexp"), data);

  // Ask the graph before the executor takes it. The empty slice's callee
  // rejects, so reaching it at all is the bug this catches.
  expect("an empty slice never lowers its partial-sum function",
         count_opcode(cm.graph, OP_REJECT) == 0);

  const Run got = evaluate(cm);

  DataMap equiv_data =
      DataMap::from_json_file("tests/fixtures/reduce_sum_equiv.json");
  CompiledModel equiv_cm = compile_model(
      slurp("tests/fixtures/reduce_sum_equiv.tmir.sexp"), equiv_data);
  const Run want = evaluate(equiv_cm);

  expect("lp is finite", std::isfinite(got.lp));
  expect("reduce_sum lp is bitwise the hand-written lp", got.lp == want.lp);
  expect("same parameter count", got.grad.size() == want.grad.size());
  if (got.grad.size() == want.grad.size())
    for (size_t i = 0; i < got.grad.size(); ++i)
      expect("reduce_sum gradient[" + std::to_string(i) +
                 "] is bitwise the hand-written gradient",
             got.grad[i] == want.grad[i]);

  // The equality above is the propto check only if the two spellings would
  // actually disagree at this point. They differ by normal's normalizing
  // constant over the five observations, at the sigma this point implies,
  // so pin that it is not zero and the check cannot pass vacuously.
  const double log_sqrt_2pi = 0.5 * std::log(8.0 * std::atan(1.0));
  const double sigma = std::exp(eval_point(1));  // parameters: mu, sigma, b
  const double normalizer = 5.0 * (log_sqrt_2pi + std::log(sigma));
  expect("the two propto spellings differ by a nonzero constant",
         std::fabs(normalizer) > 1.0);

  if (failures == 0) std::printf("test_reduce_sum: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
