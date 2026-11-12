// Binding-owned logical views: zero extents, vector orientation, array and
// matrix slices, and lexical UDF shadowing must not depend on a slot's length
// or on global name tables.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/mir.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/optable.hpp>
#include <stanli/sexp.hpp>

#include "env_helpers.hpp"
#include "stdout_capture.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
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

void eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-42s got %.17g want %.17g\n", what.c_str(), got, want);
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
    std::printf("FAIL %-42s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
  }
}

template <class F>
void run_case(const std::string& what, F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s threw: %s\n", what.c_str(), e.what());
  }
}

std::string fixture(const std::string& stem) {
  return slurp("tests/fixtures/" + stem + ".tmir.sexp");
}

int count_islands(const stanli::CompiledModel& model) {
  int islands = 0;
  for (const stanli::Op& op : model.graph.ops)
    islands += op.opcode == stanli::OP_ISLAND;
  return islands;
}

struct Point {
  std::vector<double> q;
  double lp;
  std::vector<double> grad;
};

// Every point is evaluated in order on one executor.
struct ValueCase {
  const char* what;
  const char* stem;
  std::vector<Point> points;
  std::function<void(stanli::DataMap&)> data = nullptr;
  int n_unconstrained = -1;
  int islands = -1;
};

std::vector<double> iota(int n) {
  std::vector<double> v;
  for (int i = 1; i <= n; ++i) v.push_back(i);
  return v;
}

void set_A(stanli::DataMap& d) {
  d = stanli::DataMap::from_json(R"({"A":[[1,2,3],[4,5,6]]})");
}

const std::vector<ValueCase>& value_cases() {
  static const std::vector<ValueCase> cases = {
      {"zero matrix", "view_zero_matrix", {{{0.125}, 3.125, {1}}}},
      // vector[3] is 3x1, row_vector[3] is 1x3, vector[0] is 0x1, and
      // row_vector[0] is 1x0. These are language-level views, not
      // conclusions that can be recovered from their flat storage length.
      {"vector orientation",
       "view_vector_orientation",
       {{{0.125}, 1103113.125, {1}}},
       nullptr,
       1},
      // The three empty matrices occupy no unconstrained slots. In
      // declaration order the physical parameter vector is q, b[1], b[2],
      // tail. rows/cols contributions are 30 (0x3), 300 (3x0), and 0 (0x0).
      {"zero parameter views",
       "view_zero_params",
       {{{0.25, -0.5, 0.75, -1.0}, 324.25, {1, 2, 4, 8}}},
       nullptr,
       4},
      {"row ternary",
       "view_row_ternary",
       {{{0.25}, 133.25, {1}}, {{-0.25}, 138.75, {1}}},
       nullptr,
       -1,
       1},
      {"matrix ternary",
       "view_matrix_ternary",
       {{{0.25, -0.5, 0.75, -1.0, 1.25, -1.5}, -8.0, {6, 5, 7, 7, 9, 11}},
        {{-0.25, -0.5, 0.75, -1.0, 1.25, -1.5},
         11.0,
         {-6, -5, -7, -7, -9, -11}}},
       nullptr,
       6,
       2},
      // Generated Stan reads nested scalar arrays outer-first, so z[2,1] is
      // q[2]. A leaked matrix view from the callee would instead select q[1].
      {"UDF shadow",
       "view_udf_shadow",
       {{{0.25, -0.5, 0.75, -1.0}, 1.0, {1, 0, 1, 0}}}},
      {"nested UDF frame",
       "view_udf_int_frame",
       {{{0.25}, 8.0, {4}}, {{-0.5}, 5.0, {4}}},
       nullptr,
       1},
      {"scalar array data layout",
       "view_data_array_layout",
       {{{0.125}, 654321.125, {1}}},
       set_A},
      {"matrix Times__ island",
       "view_island_matrix_times",
       {{{2.0}, 23.0, {0}}, {{-2.0}, -9.0, {7}}}},
      {"row-matrix product",
       "view_row_matrix_times",
       {{{2, 3, 5, 7, 11, 13}, 641.0, {115, 137, 2, 3, 20, 30}}}},
      {"UDF local data branch",
       "view_udf_local_data_branch",
       {{{-0.375}, -0.375, {1}}},
       set_A},
      {"append row-vectors",
       "view_append_rows",
       {{{1, 2, 3, 4}, 32.0, {0, 1, 10, 0}}}},
      {"island UDF return",
       "view_island_udf_return",
       {{{0.25}, 0.5, {2}}, {{-0.25}, 0.5, {-2}}}},
      {"array container views",
       "view_array_container_extract",
       {{{0.25}, 1630.25, {1}}}},
      {"array size vs num_elements",
       "viewc_array_size",
       {{{0.25}, 206.25, {1}}}},
      {"matrix row range+gather",
       "viewc_matrix_row_slices",
       {{iota(6), 1037.0, {33, 2, 16, 132, 8, 64}}},
       [](stanli::DataMap& d) { d.set_int_array("idx", {3, 1}); }},
      {"whole matrix row assignment",
       "viewc_row_assignment",
       {{{2, 3}, 32.0, {1, 10}}}},
      {"append orientation",
       "viewc_append_orientation",
       {{std::vector<double>(10, 1.0),
         6174.0,
         {1, 2, 4, 8, 16, 32, 64, 128, 256, 512}}}},
      {"mixed append",
       "viewc_append_mixed_accept",
       {{iota(9), 1579.0, {26, 29, 32, 33, 38, 32, 34, 37, 39}}}},
      // A break promotes the loop to a runtime-control region, so the
      // register program compiles the array-of-matrix local rather than
      // refusing it. Reading x[2][2, 1] = 10 would pick a different cell
      // under a row-major leaf.
      {"region array-of-matrix",
       "viewc_region_array_matrix",
       {{{0.25}, 2.5, {10}}}},
      {"local array-matrix data branch",
       "viewc_local_array_matrix_observation",
       {{{0.25}, 0.25, {1}}}},
      {"zero row-vector ternary",
       "viewc_zero_row_ternary",
       {{{0.25}, 100.25, {1}}, {{-0.25}, 99.75, {1}}}},
      {"outer range a[2:4]",
       "viewa_outer_range",
       {{iota(5), 432, {0, 1, 10, 100, 0}}}},
      {"outer gather a[{5,2,4}]",
       "viewa_outer_gather",
       {{iota(5), 425, {0, 10, 0, 100, 1}}},
       [](stanli::DataMap& d) { d.set_int_array("idx", {5, 2, 4}); }},
      {"array[2,3] column a[:,2]",
       "viewa_scalar_column",
       {{iota(6), 225, {0, 10, 0, 0, 1, 0}}}},
      // dims() of an empty array is {0}: Stan Math does not look inside it.
      {"zero outer container dimensions",
       "viewa_contextual_empty",
       {{{0.25}, 5.0, {4}}},
       [](stanli::DataMap& d) {
         d.set_real_array("vectors", {}, {0, 4});
         d.set_real_array("rows", {}, {0, 5});
         d.set_real_array("matrices", {}, {0, 2, 3});
         d.set_real_array("scalars", {}, {0, 3});
       }},
      {"explicit array[2] vector[0] construction",
       "viewa_explicit_zero_vectors",
       {{{0.25}, 200.25, {1}}}},
  };
  return cases;
}

void run_value_case(const ValueCase& c) {
  const std::string what = c.what;
  stanli::DataMap data;
  if (c.data) c.data(data);
  stanli::CompiledModel model = stanli::compile_model(fixture(c.stem), data);
  if (c.n_unconstrained >= 0)
    check(model.n_unconstrained == c.n_unconstrained,
          what + " unconstrained parameter count");
  if (c.islands >= 0)
    check(count_islands(model) == c.islands, what + " island count");
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  for (size_t p = 0; p < c.points.size(); ++p) {
    const Point& point = c.points[p];
    const std::string at = what + " point " + std::to_string(p);
    check(ex.n_params() == static_cast<int64_t>(point.q.size()),
          at + " parameter count");
    for (size_t i = 0; i < point.q.size(); ++i)
      ex.params_data()[i] = point.q[i];
    std::vector<double> grad(point.q.size(), 0.0);
    eq(at + " lp", ex.gradient(grad.data()), point.lp);
    for (size_t i = 0; i < point.grad.size(); ++i)
      eq(at + " grad " + std::to_string(i), grad[i], point.grad[i]);
  }
}

void expect_view_refusal(const std::string& stem, const std::string& what) {
  bool refused = false;
  try {
    stanli::DataMap data;
    (void)stanli::compile_model(fixture(stem), data);
  } catch (const std::exception& e) {
    refused = true;
    check(
        std::string(e.what()).find(
            "conditional arms of different logical views") != std::string::npos,
        what + " reports logical-view refusal");
  }
  check(refused, what + " refuses instead of guessing");
}

void test_refusals() {
  expect_view_refusal("view_matrix_ternary_mismatch",
                      "equal-width 2x3/3x2 matrix ternary");
  expect_view_refusal("view_island_flat_array",
                      "flat container ternary in register island");
}

void test_write_array_receives_data_views() {
  using namespace stanli;
  DataMap data;
  data.set_real_array("M", {1, 2, 3, 4, 5, 6}, {2, 3});
  CompiledModel model = compile_model(fixture("view_gq_data_matrix"), data);
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
      eq("GQ data matrix value", got, 6.0);
    }
    if (names[i] == "shaped") {
      found_shaped = true;
      eq("GQ data row view", got, 1237.0);
    }
  }
  check(found_picked, "GQ data matrix column found");
  check(found_shaped, "GQ data row column found");
}

void test_empty_data_array_matrix() {
  stanli::DataMap data;
  data.set_real_array("A", {}, {0, 2, 3});
  stanli::CompiledModel model =
      stanli::compile_model(fixture("viewa_data_empty_matrix"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  ex.params_data()[0] = 0.25;
  double grad[1] = {};
  eq("empty data array-matrix lp", ex.gradient(grad), 10.25);
  eq("empty data array-matrix grad", grad[0], 1.0);

  check(model.write_array.has_value(), "empty array-matrix write_array exists");
  if (!model.write_array) return;
  check(model.write_array->truncated.empty(),
        "empty array-matrix stays on graph write_array path");
  check(!model.write_array->interp,
        "empty array-matrix needs no write_array fallback");
  stanli::Executor wx(std::move(model.write_array->graph));
  model.write_array->bind(wx);
  wx.params_data()[0] = 0.25;
  wx.run_forward_only();
  const std::vector<std::string> names =
      stanli::CompiledModel::csv_names(model.write_array->columns);
  check(names == std::vector<std::string>({"q", "proof"}),
        "empty array-matrix exact CSV columns");
  if (model.write_array->columns.size() == 2) {
    eq("empty array-matrix write q",
       wx.value_ptr(model.write_array->columns[0].slot)[0], 0.25);
    eq("empty array-matrix write proof",
       wx.value_ptr(model.write_array->columns[1].slot)[0], 10.0);
  } else {
    check(false, "empty array-matrix has two scalar write views");
  }
}

void test_udf_array_matrix_order() {
  stanli::DataMap data;
  data.set_real_array("A", {1, 7, 4, 10, 2, 8, 5, 11, 3, 9, 6, 12}, {2, 2, 3});
  auto model = stanli::compile_model(fixture("viewc_udf_array_matrix"), data);
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

void test_effectful_data_udf() {
  stanli::DataMap data;
  data.set_real("x", 2.5);
  std::optional<stanli::CompiledModel> model;
  {
    stanli_test::StdoutCapture captured;
    model = stanli::compile_model(fixture("viewc_effectful_udf"), data);
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

void effectful_int_udf(const std::string& stem, const std::string& what) {
  stanli::DataMap data;
  data.set_int_array("x_i", {1});
  std::optional<stanli::CompiledModel> model;
  {
    stanli_test::StdoutCapture captured;
    model = stanli::compile_model(fixture(stem), data);
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
  effectful_int_udf("viewc_effectful_int_udf", "inlined effectful int UDF");
  effectful_int_udf("viewc_effectful_int_udf_nested",
                    "uninlined effectful int UDF");
}

void test_data_size_effect_runs_once() {
  auto data = stanli::DataMap::from_json_file(
      "tests/fixtures/data_size_print_once.json");
  std::optional<stanli::CompiledModel> model;
  std::string printed;
  {
    stanli_test::StdoutCapture captured;
    model = stanli::compile_model(fixture("data_size_print_once"), data);
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
    stanli::compile_model(fixture(stem), data);
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

void test_register_array_boundary() {
  using namespace stanli;
  mir::Program mir =
      mir::read_program(sexp::parse(fixture("viewa_program_arrays")));
  std::map<std::string, const mir::FunDef*> funs;
  for (const auto& f : mir.fun_defs) funs[f.name] = &f;

  auto compile = [&](const std::string& name) {
    auto it = funs.find(name);
    if (it == funs.end()) {
      check(false, "register fixture contains " + name);
      return RhsProgram{};
    }
    return compile_rhs(*it->second, funs, 1, 4, 1, {2});
  };

  RhsProgram flat = compile("rhs_flat");
  check(flat.ok,
        "register accepts depth-1 scalar real/int arrays: " + flat.why);
  if (flat.ok) {
    const double y[1] = {3.0};
    const double theta[4] = {4.0, 5.0, 6.0, 7.0};
    const double xr[1] = {1.5};
    std::vector<double> out;
    std::vector<double> rhs_registers;
    run_rhs<double>(flat, 0.0, y, theta, xr, out, rhs_registers);
    check(out.size() == 1, "register scalar-array output width");
    if (out.size() == 1) eq("register scalar-array value", out[0], 15.5);
  }

  for (const std::string& name :
       {"rhs_depth2", "rhs_depth2_int", "rhs_vectors", "rhs_matrices"}) {
    RhsProgram refused = compile(name);
    check(!refused.ok, "register refuses " + name);
    check(!refused.why.empty(), "register explains refusal for " + name);
  }
}

}  // namespace

int main() {
  for (const ValueCase& c : value_cases())
    run_case(c.what, [&] { run_value_case(c); });
  run_case("refusals", test_refusals);
  run_case("GQ data views", test_write_array_receives_data_views);
  run_case("empty data array matrix", test_empty_data_array_matrix);
  run_case("UDF array-matrix order", test_udf_array_matrix_order);
  run_case("effectful data UDF", test_effectful_data_udf);
  run_case("effectful int UDF", test_effectful_int_udf_is_not_observed);
  run_case("data size effect runs once", test_data_size_effect_runs_once);
  run_case("while fallback leaves no trace",
           test_while_fallback_leaves_no_trace);
  run_case("register array boundary", test_register_array_boundary);
  if (failures == 0) std::printf("test_lower_views OK\n");
  return failures == 0 ? 0 : 1;
}
