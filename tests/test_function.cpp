#include <stanli/function.hpp>
#include <stanli/capi.h>
#include <cstdlib>
#include "stdout_capture.hpp"

#include <atomic>
#include <thread>

#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::printf("FAIL %s\n", message);
  }
}

template <typename F>
void throws_with(F&& f, const std::string& needle, const char* message) {
  try {
    f();
    check(false, message);
  } catch (const std::exception& e) {
    check(std::string(e.what()).find(needle) != std::string::npos, message);
  }
}

std::string slurp(const char* path) {
  std::ifstream input(path);
  std::ostringstream text;
  text << input.rdbuf();
  return text.str();
}

stanli::DataMap::Entry compiled_call(const stanli::Function& function,
                                     const stanli::DataMap& args) {
  stanli_test::StdoutCapture diagnostic(stderr);
  auto result = function(args);
  const auto report = diagnostic.finish();
  const bool compiled =
      report.find("\"value_engine\":\"register_program\"") !=
          std::string::npos &&
      report.find("\"interpreter_events\":[]") != std::string::npos &&
      report.find("\"event\":") == std::string::npos;
  if (!compiled)
    std::fprintf(stderr, "Unexpected selection: %s", report.c_str());
  check(compiled, "compiled first/repeated call has no interpreter entry");
  return result;
}

void typed_boundary(const std::string& mir) {
  char err[8192] = {};
  auto* f =
      stanli_function_new_from_mir(mir.c_str(), "choose", err, sizeof(err));
  check(f != nullptr, "typed boundary constructs from MIR");
  if (!f) return;
  double x = 4.0;
  int first = 1;
  stanli_function_argument args[] = {
      {"x", 0, &x, nullptr, 1, nullptr, 0},
      {"first", 1, nullptr, &first, 1, nullptr, 0}};
  double result = 0;
  const auto writer = [](void* context, int is_int, const double* reals,
                         size_t real_size, const int*, size_t, const int64_t*,
                         size_t dim_size) -> int {
    if (is_int || real_size != 1 || dim_size != 0) return 1;
    *static_cast<double*>(context) = reals[0];
    return 0;
  };
  const auto call = [&] {
    return stanli_function_call_values(f, args, 2, writer, &result, err,
                                       sizeof(err));
  };
  check(call() == 0 && result == 8.0,
        "typed boundary real and integer scalars");
  args[0].size = 0;
  check(call() != 0, "typed boundary rejects invalid scalar length");
  args[0].size = 1;
  args[1].ints = nullptr;
  check(call() != 0, "typed boundary rejects null nonempty integer buffer");
  args[1].ints = &first;
  args[0].is_int = 2;
  check(call() != 0, "typed boundary rejects invalid type discriminator");
  args[0].is_int = 0;
  args[0].name = "first";
  check(call() != 0, "typed boundary rejects duplicate names");
  args[0].name = "x";
  args[0].dim_size = 1;
  check(call() != 0, "typed boundary rejects null dimension buffer");
  int64_t dims[] = {-1, 2};
  args[0].dims = dims;
  check(call() != 0, "typed boundary rejects negative dimensions");
  dims[0] = std::numeric_limits<int64_t>::max();
  args[0].dim_size = 2;
  check(call() != 0, "typed boundary rejects overflowing dimensions");
  args[0].dims = nullptr;
  args[0].dim_size = 0;
  check(call() == 0 && result == 8.0, "typed boundary recovers after errors");
  check(stanli_function_call_values(f, nullptr, 2, writer, &result, err,
                                    sizeof(err)) != 0,
        "typed boundary rejects null argument table");
  stanli_function_free(f);
}

}  // namespace

int main() {
  using stanli::DataMap;
  using stanli::Function;

  try {
#ifdef _WIN32
    _putenv_s("STANLI_EXECUTION_REPORT", "1");
#else
    setenv("STANLI_EXECUTION_REPORT", "1", 1);
#endif
    const std::string source = slurp("tests/fixtures/standalone_compiled.stan");
    // The MIR constructor works in every runtime build, including developer
    // builds which deliberately omit the embedded source compiler.
    Function cached = Function::from_mir(
        slurp("tests/fixtures/early_return.tmir.sexp"), "choose");
    DataMap cached_args;
    cached_args.set_real("x", 4.0);
    cached_args.set_int("first", 1);
    check(cached(cached_args).r == std::vector<double>({8.0}),
          "cached MIR function call");
    typed_boundary(slurp("tests/fixtures/early_return.tmir.sexp"));

    if (!stanli_has_embedded_stanc()) {
      throws_with([&] { Function unavailable(source, "affine"); },
                  "does not embed stanc3",
                  "source constructor reports unavailable compiler");
      if (failures == 0) std::printf("OK function\n");
      return failures == 0 ? 0 : 1;
    }

    Function affine(source, "affine");
    DataMap args;
    args.set_real_array("x", {1.0, 2.0, 4.0});
    args.set_real("a", 2.5);
    args.set_real("b", -1.0);
    const DataMap::Entry result = compiled_call(affine, args);
    check(!result.is_int, "vector result is real");
    check(result.dims == std::vector<int64_t>{3},
          "vector result keeps dimensions");
    check(result.r == std::vector<double>({1.5, 4.0, 9.0}),
          "vector result values");

    // A cache hit must read the current real values, including when the caller
    // supplied an integer that was promoted to a real formal.
    args.set_real("a", -2.0);
    check(compiled_call(affine, args).r == std::vector<double>({-3, -5, -9}),
          "cached program does not fold real argument values");
    Function exits(source, "branch_exit");
    for (double x : {2., -3., -1.5, -.5}) {
      DataMap input;
      input.set_real("x", x);
      const double want = x > 0    ? 2 * x
                          : x < -2 ? -3 * x
                          : x < -1 ? -4 * x
                                   : x + 7;
      check(compiled_call(exits, input).r == std::vector<double>{want},
            "standalone compiled early exits");
    }
    Function sized(source, "sized");
    for (int n : {0, 3, 1, 3, 8, 2, 4, 5, 6, 7, 9, 0}) {
      DataMap input;
      input.set_real("x", .5);
      input.set_int("n", n);
      (void)sized(
          input);  // A missing signature promotes after cache saturation.
      const auto got = compiled_call(sized, input);
      std::vector<double> want;
      for (int i = 1; i <= n; ++i) want.push_back(.5 + i);
      check(got.r == want && got.dims == std::vector<int64_t>{n},
            "specialization follows every integer value and zero extent");
    }
    // Isolated signatures after saturation use the established value engine;
    // a repeated new signature then promotes without repeated compilation.
    for (int n : {20, 21}) {
      DataMap input;
      input.set_real("x", .5);
      input.set_int("n", n);
      stanli_test::StdoutCapture diagnostic(stderr);
      const auto got = sized(input);
      const auto report = diagnostic.finish();
      check(
          got.r.size() == (size_t)n &&
              report.find("awaiting a repeated signature") != std::string::npos,
          "one-off signatures avoid recompilation after cache saturation");
    }
    {
      DataMap input;
      input.set_real("x", .5);
      input.set_int("n", 21);
      check(compiled_call(sized, input).r.size() == 21,
            "stable new signature promotes after a second miss");
    }
    Function array_identity(source, "array_identity");
    DataMap array_arg;
    array_arg.set_real_array("x", {1, 2, 3});
    check(compiled_call(array_identity, array_arg).r ==
              std::vector<double>({1, 2, 3}),
          "scalar arrays preserve public storage");
    Function nested(source, "nested_identity");
    DataMap nested_arg;
    nested_arg.set_real_array("x", {1, 2, 3, 4, 5, 6}, {2, 3});
    {
      stanli_test::StdoutCapture diagnostic(stderr);
      const auto got = nested(nested_arg);
      const auto report = diagnostic.finish();
      check(got.r == std::vector<double>({1, 2, 3, 4, 5, 6}) &&
                got.dims == std::vector<int64_t>({2, 3}) &&
                report.find("\"event\":") != std::string::npos,
            "array-of-vector storage retains interpreter semantics");
    }
    Function dynamic(source, "dynamic_result");
    for (double x : {1., -1., 2.}) {
      DataMap input;
      input.set_real("x", x);
      stanli_test::StdoutCapture diagnostic(stderr);
      const auto got = dynamic(input);
      const auto report = diagnostic.finish();
      const int n = x > 0 ? 2 : 3;
      check(got.r == std::vector<double>(n, x) &&
                got.dims == std::vector<int64_t>{n} &&
                report.find("\"event\":") != std::string::npos,
            "shape-changing returns refuse without caching a real value");
    }
    Function observed(source, "observed");
    for (double x : {1., -1., 2.}) {
      DataMap input;
      input.set_real("x", x);
      stanli_test::StdoutCapture capture;
      if (x < 0)
        throws_with([&] { (void)compiled_call(observed, input); }, "bad:-1",
                    "compiled rejection preserves message");
      else
        check(compiled_call(observed, input).r == std::vector<double>{x + 1},
              "compiled effects recover after rejection");
      check(capture.finish() == "seen:" + std::to_string((int)x) + "\n",
            "print executes exactly once, including first call and failure");
    }
    Function guarded(source, "guarded_constructor");
    for (double x : {-1., 1., -2.}) {
      DataMap input;
      input.set_real("x", x);
      input.set_int("n", -1);
      stanli_test::StdoutCapture capture;
      if (x > 0)
        throws_with([&] { (void)guarded(input); }, "linspaced_vector",
                    "constructor error occurs only on the executed branch");
      else
        check(guarded(input).r == std::vector<double>{x},
              "compile-time domain errors refuse without rejecting an untaken "
              "branch");
      check(capture.finish() == "checked:" + std::to_string((int)x) + "\n",
            "refused speculative compilation preserves exactly one print");
    }
    Function rng(source, "unseeded_rng");
    throws_with([&] { (void)rng(DataMap{}); }, "normal_rng",
                "unseeded standalone RNG keeps its existing error");

    // Concurrent calls can compile/evict plans while another call uses them.
    // Execution buffers are per call; admission is observed separately above.
    std::atomic<bool> concurrent_ok{true};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
      workers.emplace_back([&, worker] {
        try {
          for (int repeat = 0; repeat < 30; ++repeat) {
            DataMap input;
            const int n = (repeat + worker) % 12;
            input.set_real("x", worker + .25);
            input.set_int("n", n);
            const auto got = sized(input);
            if (got.r.size() != (size_t)n ||
                got.dims != std::vector<int64_t>{n})
              concurrent_ok = false;
            for (int i = 0; i < n; ++i)
              if (got.r[i] != worker + .25 + i + 1) concurrent_ok = false;
          }
        } catch (...) {
          concurrent_ok = false;
        }
      });
    for (auto& worker : workers) worker.join();
    check(concurrent_ok,
          "concurrent specialization and eviction preserve calls");

    Function plus_one(source, "plus_one");
    DataMap ints;
    ints.set_int("x", 41);
    const DataMap::Entry integer = compiled_call(plus_one, ints);
    check(integer.is_int && integer.i == std::vector<int>({42}) &&
              integer.r == std::vector<double>({42.0}) && integer.dims.empty(),
          "integer result keeps both representations");

    ints.set_int("x", std::numeric_limits<int>::max() - 1);
    const auto boundary = plus_one(ints);
    check(boundary.i == std::vector<int>{std::numeric_limits<int>::max()} &&
              boundary.r ==
                  std::vector<double>{(double)std::numeric_limits<int>::max()},
          "integer fallback retains result mirrors at the 32-bit boundary");
    ints.set_int("x", 41);
    for (const char* name : {"integer_divide", "integer_modulo"}) {
      Function operation(source, name);
      DataMap input;
      input.set_int("x", -7);
      input.set_int("y", 3);
      const auto got = compiled_call(operation, input);
      const int want = std::string(name) == "integer_divide" ? -2 : -1;
      check(got.i == std::vector<int>{want} &&
                got.r == std::vector<double>{(double)want},
            "integer fallback preserves negative division and remainder");
    }
    Function noop(source, "noop");
    const auto empty_result = noop(DataMap{});
    check(empty_result.r.empty() && empty_result.i.empty() &&
              empty_result.dims.empty(),
          "void fallback preserves the empty result writer call");

    Function real_identity(source, "real_identity");
    const DataMap::Entry promoted_real = compiled_call(real_identity, ints);
    check(!promoted_real.is_int && promoted_real.i.empty() &&
              promoted_real.r == std::vector<double>({41.0}),
          "integer argument is promoted to the real formal");

    Function scale(source, "scale_matrix");
    DataMap matrix;
    // Matrix storage is column-major: [1 3; 2 4].
    matrix.set_real_array("x", {1.0, 2.0, 3.0, 4.0}, {2, 2});
    matrix.set_real("a", 3.0);
    const DataMap::Entry scaled = compiled_call(scale, matrix);
    check(scaled.dims == std::vector<int64_t>({2, 2}),
          "matrix result keeps dimensions");
    check(scaled.r == std::vector<double>({3.0, 6.0, 9.0, 12.0}),
          "matrix result keeps column-major values");

    for (const auto& dims :
         std::vector<std::vector<int64_t>>{{0, 3}, {3, 0}, {0, 0}}) {
      matrix.set_real_array("x", {}, dims);
      const auto empty = compiled_call(scale, matrix);
      check(empty.r.empty() && empty.dims == dims,
            "zero matrices key and preserve full logical extents");
    }
    Function overloaded(source, "overloaded");
    DataMap scalar;
    scalar.set_real("x", 2.0);
    check(overloaded(scalar).r == std::vector<double>({2.5}),
          "overload selected by scalar rank");
    DataMap vector;
    vector.set_real_array("x", {1.0, 2.0, 3.0});
    check(overloaded(vector).r == std::vector<double>({6.0}),
          "overload selected by vector rank");

    Function numeric(source, "numeric");
    if (std::numeric_limits<long>::max() > std::numeric_limits<int>::max()) {
      const long wide = (long)((int64_t)std::numeric_limits<int>::max() + 1);
      DataMap input;
      input.set_int("x", wide);
      stanli_test::StdoutCapture diagnostic(stderr);
      Function promote_integer(source, "promote_integer");
      const auto got = promote_integer(input);
      const auto report = diagnostic.finish();
      check(!got.is_int && got.i.empty() &&
                got.r == std::vector<double>{1.5 * (double)wide} &&
                report.find("integer input mirrors") != std::string::npos,
            "noncanonical integer mirrors preserve the legacy C++ API result");
      input.set_int("x", (int)wide);
      check(compiled_call(promote_integer, input).r ==
                std::vector<double>{1.5 * (double)(int)wide},
            "canonical and noncanonical integer mirrors cannot share a "
            "compiled plan");
    }

    DataMap promoted;
    promoted.set_int("x", 2);
    Function promoted_return(source, "promoted_return");
    const auto promoted_result = promoted_return(promoted);
    check(!promoted_result.is_int && promoted_result.i.empty() &&
              promoted_result.r == std::vector<double>{12},
          "a typed real return clears interpreter integer mirrors too");
    check(numeric(promoted).r == std::vector<double>({12.0}),
          "integer overload wins over real promotion");
    for (int i = 0; i < 3; ++i) {
      check(numeric(scalar).r == std::vector<double>({22.0}),
            "cached candidates select a real overload after an integer");
      check(numeric(promoted).r == std::vector<double>({12.0}),
            "cached candidates select an integer overload after a real");
      throws_with([&] { (void)numeric(vector); }, "no overload",
                  "cached candidates still reject invalid ranks");
      check(overloaded(vector).r == std::vector<double>({6.0}) &&
                overloaded(scalar).r == std::vector<double>({2.5}),
            "cached candidates select by the current rank");
    }
    Function resolved(source, "numeric(real)");
    check(resolved(promoted).r == std::vector<double>({22.0}),
          "resolved signature preserves promotion despite integer overload");
    Function direction(source, "direction");
    throws_with([&] { (void)direction(vector); }, "ambiguously match",
                "cached candidates preserve vector/row-vector ambiguity");
    Function resolved_direction(source, "direction(vector)");
    check(resolved_direction(vector).r == std::vector<double>({6.0}),
          "resolved signature disambiguates identical host ranks");

    // Recursive calls cannot be inlined away: they exercise the cached full
    // definition table, not just the top-level candidate list.
    Function descend(source, "descend");
    DataMap recursive;
    recursive.set_real("x", 1.5);
    recursive.set_int("remaining", 3);
    check(descend(recursive).r == std::vector<double>({4.5}),
          "cached table supports nested user-function calls");
    recursive.set_int("remaining", 70);
    throws_with([&] { (void)descend(recursive); }, "recursion too deep",
                "cached table preserves recursion guard");
    recursive.set_int("remaining", 2);
    check(descend(recursive).r == std::vector<double>({3.5}),
          "cached table recovers after an interpreter failure");

    throws_with(
        [&] {
          DataMap missing;
          missing.set_real("a", 1.0);
          missing.set_real("b", 2.0);
          (void)affine(missing);
        },
        "variable not provided: x", "missing named argument reports its name");

    throws_with(
        [&] {
          DataMap wrong;
          wrong.set_real("x", 1.0);
          wrong.set_real("a", 1.0);
          wrong.set_real("b", 2.0);
          (void)affine(wrong);
        },
        "rank 0, expected 1", "argument rank mismatch is rejected");

    throws_with([&] { Function absent(source, "absent"); },
                "Stan function not found", "missing function is rejected");

    const DataMap::Entry one_shot =
        stanli::evaluate_function(source, "plus_one", ints);
    check(one_shot.i == std::vector<int>({42}),
          "one-shot evaluate_function entry point");
  } catch (const std::exception& e) {
    std::printf("FAIL unexpected exception: %s\n", e.what());
    ++failures;
  }

  if (failures == 0) std::printf("OK function\n");
  return failures == 0 ? 0 : 1;
}
