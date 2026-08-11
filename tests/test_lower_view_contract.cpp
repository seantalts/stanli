#include <stanli/compile.hpp>
#include <stanli/graph.hpp>

#include "stdout_capture.hpp"

#include <cstdio>
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

void refuses(const std::string& fixture, const stanli::DataMap& data,
             const std::string& what) {
  bool refused = false;
  try {
    (void)stanli::compile_model(slurp(fixture), data);
  } catch (const std::exception&) {
    refused = true;
  }
  check(refused, what);
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

void test_no_length_one_container_broadcast() {
  stanli::DataMap data;
  refuses("tests/fixtures/viewc_vec_broadcast_graph.tmir.sexp", data,
          "Graph vector[1]+vector[2] refuses");
  refuses("tests/fixtures/viewc_row_broadcast_graph.tmir.sexp", data,
          "Graph row_vector[1]+row_vector[2] refuses");
  refuses("tests/fixtures/viewc_vec_broadcast_program.tmir.sexp", data,
          "Program vector[1]+vector[2] refuses");
  refuses("tests/fixtures/viewc_row_broadcast_program.tmir.sexp", data,
          "Program row_vector[1]+row_vector[2] refuses");
}

void test_shape_refusals() {
  stanli::DataMap none;
  refuses("tests/fixtures/viewc_cholesky_zero_rect.tmir.sexp", none,
          "cholesky matrix[0,3] refuses non-square");
  refuses("tests/fixtures/viewc_eigen_zero_rect.tmir.sexp", none,
          "eigen matrix[0,3] refuses non-square");
  refuses("tests/fixtures/viewc_array_bind_mismatch.tmir.sexp", none,
          "array[3,2] binding to array[2,3] refuses");
  refuses("tests/fixtures/viewc_to_matrix_mismatch.tmir.sexp", none,
          "to_matrix length mismatch refuses");

  stanli::DataMap x;
  x.set_real_array("X", {1, 2, 3, 4, 5, 6}, {2, 3});
  refuses("tests/fixtures/viewc_matvec_mismatch.tmir.sexp", x,
          "data MATVEC inner mismatch refuses");
  refuses("tests/fixtures/viewc_column_assignment_mismatch.tmir.sexp", none,
          "column assignment RHS width mismatch refuses");
  refuses("tests/fixtures/viewc_append_col_mismatch.tmir.sexp", none,
          "append_col vector row mismatch refuses");
  refuses("tests/fixtures/viewc_append_row_mismatch.tmir.sexp", none,
          "append_row row-vector column mismatch refuses");
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

  refuses("tests/fixtures/viewc_append_col_matrix_vector_bad.tmir.sexp", data,
          "append_col(matrix,wrong vector rows) refuses");
  refuses("tests/fixtures/viewc_append_row_row_matrix_bad.tmir.sexp", data,
          "append_row(row_vector,wrong matrix columns) refuses");
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

void test_effectful_int_udf_is_not_observed() {
  stanli::DataMap data;
  data.set_int_array("x_i", {1});
  bool refused = false;
  stanli_test::StdoutCapture captured;
  try {
    (void)stanli::compile_model(
        slurp("tests/fixtures/viewc_effectful_int_udf.tmir.sexp"), data);
  } catch (const std::exception&) {
    refused = true;
  }
  check(captured.finish().empty(),
        "effectful int UDF is not evaluated while compiling");
  check(refused, "effectful compile-time int demand refuses");
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
}  // namespace

int main() {
  run_case("array size contract", test_array_size_contract);
  run_case("no length-one container broadcast",
           test_no_length_one_container_broadcast);
  run_case("shape refusals", test_shape_refusals);
  run_case("matrix row selection", test_matrix_row_selection);
  run_case("row assignment", test_row_assignment);
  run_case("append orientation", test_append_orientation);
  run_case("mixed append contract", test_append_mixed_contract);
  run_case("effectful data UDF", test_effectful_data_udf);
  run_case("effectful int UDF", test_effectful_int_udf_is_not_observed);
  run_case("local array-matrix observation",
           test_local_array_matrix_observation);
  run_case("zero row ternary", test_zero_row_ternary);
  run_case("UDF array-matrix order", test_udf_array_matrix_order);
  if (failures == 0) std::printf("test_lower_view_contract OK\n");
  return failures == 0 ? 0 : 1;
}
