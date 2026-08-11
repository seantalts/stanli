// Binding-owned logical views: zero extents, vector orientation, and lexical
// UDF shadowing must not depend on a slot's length or on global name tables.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-28s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

void test_zero_row_matrix() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model =
        compile_model(slurp("tests/fixtures/view_zero_matrix.tmir.sexp"), data);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    ex.params_data()[0] = 0.125;
    double grad[1] = {0};
    const double lp = ex.gradient(grad);
    expect_eq("zero matrix lp", lp, 3.125);
    expect_eq("zero matrix gradient", grad[0], 1.0);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL zero matrix compile: %s\n", e.what());
  }
}

void test_vector_orientation_including_zero_length() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_vector_orientation.tmir.sexp"), data);
    check(model.n_unconstrained == 1,
          "vector orientation has one unconstrained parameter");
    Executor ex(std::move(model.graph));
    model.bind(ex);
    ex.params_data()[0] = 0.125;
    double grad[1] = {0};
    const double lp = ex.gradient(grad);
    // vector[3] is 3x1, row_vector[3] is 1x3, vector[0] is 0x1,
    // and row_vector[0] is 1x0.  These are language-level views, not
    // conclusions that can be recovered from their flat storage length.
    expect_eq("vector orientation lp", lp, 1103113.125);
    expect_eq("vector orientation grad", grad[0], 1.0);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL vector orientation compile: %s\n", e.what());
  }
}

void test_zero_extent_parameter_alignment() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model =
        compile_model(slurp("tests/fixtures/view_zero_params.tmir.sexp"), data);
    // The three empty matrices occupy no unconstrained slots.  In
    // declaration order the physical parameter vector is q, b[1], b[2],
    // tail, even though zero-sized logical views surround those values.
    check(model.n_unconstrained == 4,
          "zero matrices do not shift later parameter slots");
    Executor ex(std::move(model.graph));
    model.bind(ex);
    const double params[4] = {0.25, -0.5, 0.75, -1.0};
    for (int i = 0; i < 4; ++i) ex.params_data()[i] = params[i];
    double grad[4] = {0, 0, 0, 0};
    const double lp = ex.gradient(grad);
    const double want_grad[4] = {1, 2, 4, 8};
    // rows/cols contributions are 30 (0x3), 300 (3x0), and 0 (0x0).
    expect_eq("zero parameter views lp", lp, 324.25);
    for (int i = 0; i < 4; ++i)
      expect_eq("zero parameter grad " + std::to_string(i), grad[i],
                want_grad[i]);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL zero parameter views compile: %s\n", e.what());
  }
}

void test_row_vector_ternary() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model =
        compile_model(slurp("tests/fixtures/view_row_ternary.tmir.sexp"), data);
    int islands = 0;
    for (const Op& op : model.graph.ops)
      if (op.opcode == OP_ISLAND) ++islands;
    check(islands == 1, "row ternary uses one parameter island");

    Executor ex(std::move(model.graph));
    model.bind(ex);
    const double q[2] = {0.25, -0.25};
    const double want[2] = {133.25, 138.75};
    for (int i = 0; i < 2; ++i) {
      ex.params_data()[0] = q[i];
      double grad[1] = {0};
      const double lp = ex.gradient(grad);
      expect_eq("row ternary lp " + std::to_string(i), lp, want[i]);
      expect_eq("row ternary grad " + std::to_string(i), grad[0], 1.0);
    }
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL row ternary compile: %s\n", e.what());
  }
}

void test_matrix_ternaries_preserve_view() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_matrix_ternary.tmir.sexp"), data);
    check(model.n_unconstrained == 6,
          "matrix ternary has six unconstrained parameters");
    int islands = 0;
    for (const Op& op : model.graph.ops)
      if (op.opcode == OP_ISLAND) ++islands;
    check(islands == 2,
          "declared and inline matrix ternaries each use an island");

    Executor ex(std::move(model.graph));
    model.bind(ex);
    const double params[2][6] = {{0.25, -0.5, 0.75, -1.0, 1.25, -1.5},
                                 {-0.25, -0.5, 0.75, -1.0, 1.25, -1.5}};
    const double want_lp[2] = {-8.0, 11.0};
    const double coeff[6] = {6, 5, 7, 7, 9, 11};
    for (int arm = 0; arm < 2; ++arm) {
      for (int i = 0; i < 6; ++i) ex.params_data()[i] = params[arm][i];
      double grad[6] = {0, 0, 0, 0, 0, 0};
      const double lp = ex.gradient(grad);
      expect_eq("matrix ternary lp " + std::to_string(arm), lp, want_lp[arm]);
      const double sign = arm == 0 ? 1.0 : -1.0;
      for (int i = 0; i < 6; ++i)
        expect_eq("matrix ternary grad " + std::to_string(arm) + ":" +
                      std::to_string(i),
                  grad[i], sign * coeff[i]);
    }
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL matrix ternary compile: %s\n", e.what());
  }
}

void test_matrix_ternary_view_mismatch_refuses() {
  using namespace stanli;
  bool refused = false;
  try {
    DataMap data;
    (void)compile_model(
        slurp("tests/fixtures/view_matrix_ternary_mismatch.tmir.sexp"), data);
  } catch (const std::exception& e) {
    refused = true;
    check(
        std::string(e.what()).find(
            "conditional arms of different logical views") != std::string::npos,
        "matrix ternary mismatch reports logical-view refusal");
  }
  check(refused,
        "equal-width 2x3/3x2 matrix ternary refuses instead of guessing");
}

void test_udf_name_shadowing() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model =
        compile_model(slurp("tests/fixtures/view_udf_shadow.tmir.sexp"), data);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    const double q[4] = {0.25, -0.5, 0.75, -1.0};
    for (int i = 0; i < 4; ++i) ex.params_data()[i] = q[i];
    double grad[4] = {0, 0, 0, 0};
    const double lp = ex.gradient(grad);
    // CmdStan reads array[2,2] with the first index fastest, so z[1,2] is
    // q[2]. A leaked matrix view from the callee would instead select q[1].
    const double want_grad[4] = {1, 0, 1, 0};
    expect_eq("UDF shadow lp", lp, 1.0);
    for (int i = 0; i < 4; ++i)
      expect_eq("UDF shadow grad " + std::to_string(i), grad[i], want_grad[i]);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL UDF shadow compile: %s\n", e.what());
  }
}

void test_nested_udf_int_real_frames() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_udf_int_frame.tmir.sexp"), data);
    check(model.n_unconstrained == 1,
          "nested UDF frame has one unconstrained parameter");
    Executor ex(std::move(model.graph));
    model.bind(ex);
    const double q[2] = {0.25, -0.5};
    const double want_lp[2] = {8.0, 5.0};
    for (int i = 0; i < 2; ++i) {
      ex.params_data()[0] = q[i];
      double grad[1] = {0};
      const double lp = ex.gradient(grad);
      expect_eq("nested UDF frame lp " + std::to_string(i), lp, want_lp[i]);
      expect_eq("nested UDF frame grad " + std::to_string(i), grad[0], 4.0);
    }
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL nested UDF frame compile: %s\n", e.what());
  }
}

void test_scalar_array_data_layout() {
  using namespace stanli;
  try {
    DataMap data = DataMap::from_json(R"({"A":[[1,2,3],[4,5,6]]})");
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_data_array_layout.tmir.sexp"), data);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    ex.params_data()[0] = 0.125;
    double grad[1] = {0};
    const double lp = ex.gradient(grad);
    expect_eq("scalar array data layout lp", lp, 654321.125);
    expect_eq("scalar array data layout grad", grad[0], 1.0);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL scalar array data layout compile: %s\n", e.what());
  }
}

void test_write_array_receives_data_views() {
  using namespace stanli;
  try {
    DataMap data;
    data.set_real_array("M", {1, 2, 3, 4, 5, 6}, {2, 3});
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_gq_data_matrix.tmir.sexp"), data);
    check(model.write_array.has_value(), "GQ data-view write_array exists");
    if (!model.write_array) return;
    check(model.write_array->truncated.empty(),
          "GQ data-view lowering stays on compiled path");
    check(!model.write_array->interp,
          "GQ data-view lowering needs no interpreter fallback");
    Executor ex(std::move(model.write_array->graph));
    model.write_array->bind(ex);
    ex.run_forward_only();
    const auto names = CompiledModel::csv_names(model.write_array->columns);
    bool found_picked = false, found_shaped = false;
    for (size_t i = 0; i < names.size(); ++i) {
      const auto& c = model.write_array->columns[i];
      const double got = ex.value_ptr(c.slot)[0];
      if (names[i] == "picked") {
        found_picked = true;
        expect_eq("GQ data matrix value", got, 6.0);
      }
      if (names[i] == "shaped") {
        found_shaped = true;
        expect_eq("GQ data row view", got, 1237.0);
      }
    }
    check(found_picked, "GQ data matrix column found");
    check(found_shaped, "GQ data row column found");
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL GQ data-view compile: %s\n", e.what());
  }
}

void test_island_matrix_times_refuses() {
  using namespace stanli;
  bool refused = false;
  try {
    DataMap data;
    (void)compile_model(
        slurp("tests/fixtures/view_island_matrix_times.tmir.sexp"), data);
  } catch (const std::exception& e) {
    refused = true;
    check(std::string(e.what()).find("matrix multiplication") !=
              std::string::npos,
          "matrix island refusal names matrix multiplication");
  }
  check(refused, "matrix Times__ in register island refuses");
}

void test_island_flat_container_refuses() {
  using namespace stanli;
  bool refused = false;
  try {
    DataMap data;
    (void)compile_model(
        slurp("tests/fixtures/view_island_flat_array.tmir.sexp"), data);
  } catch (const std::exception& e) {
    refused = true;
    check(
        std::string(e.what()).find(
            "conditional arms of different logical views") != std::string::npos,
        "flat-container island refusal names logical views");
  }
  check(refused, "flat container ternary in register island refuses");
}

void test_row_vector_matrix_product() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_row_matrix_times.tmir.sexp"), data);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    const double q[6] = {2, 3, 5, 7, 11, 13};
    for (int i = 0; i < 6; ++i) ex.params_data()[i] = q[i];
    double grad[6] = {};
    const double lp = ex.gradient(grad);
    expect_eq("row-matrix product lp", lp, 641.0);
    const double want_grad[6] = {115, 137, 2, 3, 20, 30};
    for (int i = 0; i < 6; ++i)
      expect_eq("row-matrix product grad " + std::to_string(i), grad[i],
                want_grad[i]);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL row-matrix product compile: %s\n", e.what());
  }
}

void test_udf_local_data_branch() {
  using namespace stanli;
  try {
    DataMap data = DataMap::from_json(R"({"A":[[1,2,3],[4,5,6]]})");
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_udf_local_data_branch.tmir.sexp"), data);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    ex.params_data()[0] = -0.375;
    double grad[1] = {0};
    expect_eq("UDF local data branch lp", ex.gradient(grad), -0.375);
    expect_eq("UDF local data branch grad", grad[0], 1.0);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL UDF local data branch compile: %s\n", e.what());
  }
}

void test_append_row_vectors_make_matrix() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model =
        compile_model(slurp("tests/fixtures/view_append_rows.tmir.sexp"), data);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    const double q[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; ++i) ex.params_data()[i] = q[i];
    double grad[4] = {};
    expect_eq("append row-vectors lp", ex.gradient(grad), 32.0);
    const double want_grad[4] = {0, 1, 10, 0};
    for (int i = 0; i < 4; ++i)
      expect_eq("append row-vectors grad " + std::to_string(i), grad[i],
                want_grad[i]);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL append row-vectors compile: %s\n", e.what());
  }
}

void test_island_udf_return_frame() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_island_udf_return.tmir.sexp"), data);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    const double q[2] = {0.25, -0.25};
    const double grad_want[2] = {2, -2};
    for (int i = 0; i < 2; ++i) {
      ex.params_data()[0] = q[i];
      double grad[1] = {0};
      expect_eq("island UDF return lp " + std::to_string(i), ex.gradient(grad),
                0.5);
      expect_eq("island UDF return grad " + std::to_string(i), grad[0],
                grad_want[i]);
    }
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL island UDF return compile: %s\n", e.what());
  }
}

void test_array_container_element_views() {
  using namespace stanli;
  try {
    DataMap data;
    CompiledModel model = compile_model(
        slurp("tests/fixtures/view_array_container_extract.tmir.sexp"), data);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    ex.params_data()[0] = 0.25;
    double grad[1] = {0};
    expect_eq("array container views lp", ex.gradient(grad), 1630.25);
    expect_eq("array container views grad", grad[0], 1.0);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL array container views compile: %s\n", e.what());
  }
}

}  // namespace

int main() {
  test_zero_row_matrix();
  test_vector_orientation_including_zero_length();
  test_zero_extent_parameter_alignment();
  test_row_vector_ternary();
  test_matrix_ternaries_preserve_view();
  test_matrix_ternary_view_mismatch_refuses();
  test_udf_name_shadowing();
  test_nested_udf_int_real_frames();
  test_scalar_array_data_layout();
  test_write_array_receives_data_views();
  test_island_matrix_times_refuses();
  test_island_flat_container_refuses();
  test_row_vector_matrix_product();
  test_udf_local_data_branch();
  test_append_row_vectors_make_matrix();
  test_island_udf_return_frame();
  test_array_container_element_views();
  if (failures == 0) std::printf("test_lower_views OK\n");
  return failures == 0 ? 0 : 1;
}
