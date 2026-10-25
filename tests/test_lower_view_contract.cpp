#include <stanli/compile.hpp>
#include <stanli/graph.hpp>

#include "env_helpers.hpp"
#include "stdout_capture.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
int failures = 0;

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

template <class F>
void run_case(const char* what, F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s threw: %s\n", what, e.what());
  }
}

void eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-34s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? std::numeric_limits<int64_t>::min() - i : i;
}
// Project parity budget: up to 2 ULP vs references is acceptable.
void eq_ulp(const std::string& what, double got, double want) {
  const int64_t d = std::llabs(ulp_key(got) - ulp_key(want));
  if (d > 2) {
    ++failures;
    std::printf("FAIL %-34s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
  }
}

void test_array_size_contract() {
  stanli::DataMap data;
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_array_size.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  ex.params_data()[0] = 0.25;
  double grad[1] = {};
  eq("array size vs num_elements lp", ex.gradient(grad), 206.25);
  eq("array size vs num_elements grad", grad[0], 1.0);
}

void test_matrix_row_selection() {
  stanli::DataMap data;
  data.set_int_array("idx", {3, 1});
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_matrix_row_slices.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  for (int i = 0; i < 6; ++i) ex.params_data()[i] = i + 1;
  double grad[6] = {};
  eq("matrix row range+gather lp", ex.gradient(grad), 1037.0);
  const double want[6] = {33, 2, 16, 132, 8, 64};
  for (int i = 0; i < 6; ++i)
    eq("matrix row selection grad " + std::to_string(i), grad[i], want[i]);
}

void test_row_assignment() {
  stanli::DataMap data;
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_row_assignment.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  ex.params_data()[0] = 2;
  ex.params_data()[1] = 3;
  double grad[2] = {};
  eq("whole matrix row assignment lp", ex.gradient(grad), 32.0);
  eq("whole matrix row assignment grad 0", grad[0], 1.0);
  eq("whole matrix row assignment grad 1", grad[1], 10.0);
}

void test_append_orientation() {
  stanli::DataMap data;
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_append_orientation.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  for (int i = 0; i < 10; ++i) ex.params_data()[i] = 1.0;
  double grad[10] = {};
  eq("append orientation lp", ex.gradient(grad), 6174.0);
  const double want[10] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
  for (int i = 0; i < 10; ++i)
    eq("append orientation grad " + std::to_string(i), grad[i], want[i]);
}

void test_append_mixed_contract() {
  stanli::DataMap data;
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_append_mixed_accept.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  for (int i = 0; i < 9; ++i) ex.params_data()[i] = i + 1;
  double grad[9] = {};
  eq("mixed append lp", ex.gradient(grad), 1579.0);
  const double want[9] = {26, 29, 32, 33, 38, 32, 34, 37, 39};
  for (int i = 0; i < 9; ++i)
    eq("mixed append grad " + std::to_string(i), grad[i], want[i]);
}

// A break promotes the loop to a runtime-control region, so the register
// program compiles the array-of-matrix local rather than refusing it. Reading
// x[2][2, 1] = 10 would pick a different cell under a row-major leaf.
void test_region_array_matrix() {
  stanli::DataMap data;
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_region_array_matrix.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  ex.params_data()[0] = 0.25;
  double grad[1] = {};
  eq("region array-of-matrix lp", ex.gradient(grad), 2.5);
  eq("region array-of-matrix grad", grad[0], 10.0);
}

void test_effectful_data_udf() {
  stanli::DataMap data;
  data.set_real("x", 2.5);
  std::optional<stanli::CompiledModel> model;
  {
    stanli_test::StdoutCapture captured;
    model = stanli::compile_model(
        slurp("tests/fixtures/viewc_effectful_udf.tmir.sexp"), data);
    check(captured.finish().empty(), "data-only UDF print absent at compile");
  }
  stanli::Executor ex(std::move(model->graph));
  model->bind(ex);
  ex.params_data()[0] = 0.25;
  double grad[1] = {};
  for (int i = 0; i < 2; ++i) {
    stanli_test::StdoutCapture captured;
    eq("effectful data UDF lp " + std::to_string(i), ex.gradient(grad), 2.75);
    check(captured.finish() == "effect 2.5\n",
          "data-only UDF prints once per evaluation " + std::to_string(i));
  }
  eq("effectful data UDF grad", grad[0], 1.0);
}

void effectful_int_udf(const std::string& fixture, const std::string& what) {
  stanli::DataMap data;
  data.set_int_array("x_i", {1});
  std::optional<stanli::CompiledModel> model;
  {
    stanli_test::StdoutCapture captured;
    model = stanli::compile_model(slurp(fixture), data);
    check(captured.finish().empty(),
          what + " is not evaluated while compiling");
  }
  stanli::Executor ex(std::move(model->graph));
  model->bind(ex);
  ex.params_data()[0] = 0.25;
  double grad[1] = {};
  for (int i = 0; i < 2; ++i) {
    stanli_test::StdoutCapture captured;
    eq_ulp(what + " lp " + std::to_string(i), ex.gradient(grad),
           -std::log1p(std::exp(-0.25)));
    check(captured.finish() == "int effect\n",
          what + " prints once per evaluation " + std::to_string(i));
  }
  eq_ulp(what + " grad", grad[0], 1.0 / (1.0 + std::exp(0.25)));
}

// O1 inlines the direct call, so the effect arrives as a print statement;
// routing it through a second UDF keeps it a UserDefined call that lowering
// meets as a compile-time int demand. Neither may run the effect while
// compiling.
void test_effectful_int_udf_is_not_observed() {
  effectful_int_udf("tests/fixtures/viewc_effectful_int_udf.tmir.sexp",
                    "inlined effectful int UDF");
  effectful_int_udf("tests/fixtures/viewc_effectful_int_udf_nested.tmir.sexp",
                    "uninlined effectful int UDF");
}

void test_local_array_matrix_observation() {
  stanli::DataMap data;
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_local_array_matrix_observation.tmir.sexp"),
      data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  ex.params_data()[0] = 0.25;
  double grad[1] = {};
  eq("local array-matrix data branch lp", ex.gradient(grad), 0.25);
  eq("local array-matrix data branch grad", grad[0], 1.0);
}

void test_zero_row_ternary() {
  stanli::DataMap data;
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_zero_row_ternary.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  for (double q : {0.25, -0.25}) {
    ex.params_data()[0] = q;
    double grad[1] = {};
    eq("zero row-vector ternary lp", ex.gradient(grad), 100.0 + q);
    eq("zero row-vector ternary grad", grad[0], 1.0);
  }
}

void test_udf_array_matrix_order() {
  stanli::DataMap data;
  data.set_real_array("A", {1, 7, 4, 10, 2, 8, 5, 11, 3, 9, 6, 12}, {2, 2, 3});
  auto model = stanli::compile_model(
      slurp("tests/fixtures/viewc_udf_array_matrix.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  ex.params_data()[0] = 0.25;
  double grad[1] = {};
  eq("UDF array-matrix graph order", ex.gradient(grad), 1301907635241.25);
  eq("UDF array-matrix graph grad", grad[0], 1.0);

  check(model.write_array.has_value(), "UDF array-matrix write_array exists");
  if (!model.write_array) return;
  check(model.write_array->truncated.empty(),
        "UDF array-matrix write_array compiled");
  stanli::Executor wx(std::move(model.write_array->graph));
  model.write_array->bind(wx);
  wx.params_data()[0] = 0.25;
  wx.run_forward_only();
  const auto names =
      stanli::CompiledModel::csv_names(model.write_array->columns);
  const std::vector<std::string> want_names = {
      "q",       "C.1.1.1", "C.2.1.1", "C.1.2.1", "C.2.2.1",
      "C.1.1.2", "C.2.1.2", "C.1.2.2", "C.2.2.2", "C.1.1.3",
      "C.2.1.3", "C.1.2.3", "C.2.2.3"};
  const std::vector<double> want_values = {0.25, 1,  7, 4, 10, 2, 8,
                                           5,    11, 3, 9, 6,  12};
  check(names == want_names, "write_array names have exact order");
  std::vector<double> values;
  for (const auto& c : model.write_array->columns) {
    const double* value = wx.value_ptr(c.slot);
    values.insert(values.end(), value, value + c.len);
  }
  check(values.size() == want_values.size(),
        "write_array values have exact length");
  for (size_t i = 0; i < values.size() && i < want_values.size(); ++i)
    eq("write_array ordered value " + std::to_string(i), values[i],
       want_values[i]);
}

void test_data_size_effect_runs_once() {
  auto data = stanli::DataMap::from_json_file(
      "tests/fixtures/data_size_print_once.json");
  std::optional<stanli::CompiledModel> model;
  std::string printed;
  {
    stanli_test::StdoutCapture captured;
    model = stanli::compile_model(
        slurp("tests/fixtures/data_size_print_once.tmir.sexp"), data);
    printed = captured.finish();
  }
  check(printed == "sizing 3\n",
        "data size print fires once, got '" + printed + "'");
  stanli::Executor ex(std::move(model->graph));
  model->bind(ex);
  ex.params_data()[0] = 0.5;
  double grad[1] = {};
  stanli_test::StdoutCapture captured;
  ex.gradient(grad);
  check(captured.finish().empty(), "data size print absent at evaluation");
}

std::string lower_dump(const std::string& stem, const std::string& dir) {
  std::filesystem::remove_all(dir);
  test_setenv("STANLI_DUMP_PASSES", dir.c_str(), 1);
  auto data = stanli::DataMap::from_json_file(
      "tests/fixtures/while_fallback_unroll.json");
  try {
    stanli::compile_model(slurp("tests/fixtures/" + stem + ".tmir.sexp"), data);
  } catch (...) {
    test_unsetenv("STANLI_DUMP_PASSES");
    throw;
  }
  test_unsetenv("STANLI_DUMP_PASSES");
  return slurp(dir + "/02-log_prob-lower.txt");
}

void test_while_fallback_leaves_no_trace() {
  const auto root = std::filesystem::temp_directory_path() / "stanli_while_fb";
  const std::string looped =
      lower_dump("while_fallback_unroll", (root / "loop").string());
  const std::string by_hand =
      lower_dump("while_fallback_unroll_by_hand", (root / "hand").string());
  check(!looped.empty() && looped == by_hand,
        "fallback-unrolled while lowers to the hand-unrolled graph");
}

}  // namespace

int main() {
  run_case("array size contract", test_array_size_contract);
  run_case("matrix row selection", test_matrix_row_selection);
  run_case("row assignment", test_row_assignment);
  run_case("append orientation", test_append_orientation);
  run_case("mixed append contract", test_append_mixed_contract);
  run_case("effectful data UDF", test_effectful_data_udf);
  run_case("effectful int UDF", test_effectful_int_udf_is_not_observed);
  run_case("local array-matrix observation",
           test_local_array_matrix_observation);
  run_case("region array-of-matrix", test_region_array_matrix);
  run_case("zero row ternary", test_zero_row_ternary);
  run_case("UDF array-matrix order", test_udf_array_matrix_order);
  run_case("data size effect runs once", test_data_size_effect_runs_once);
  run_case("while fallback leaves no trace",
           test_while_fallback_leaves_no_trace);
  if (failures == 0) std::printf("test_lower_view_contract OK\n");
  return failures == 0 ? 0 : 1;
}
