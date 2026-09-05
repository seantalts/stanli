#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/mir.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/sexp.hpp>

#include <cstdio>
#include <fstream>
#include <map>
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

template <class F>
void run_case(const char* what, F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s threw: %s\n", what, e.what());
  }
}

void check_gradient(const std::string& fixture, const stanli::DataMap& data,
                    const std::vector<double>& q, double want_lp,
                    const std::vector<double>& want_grad,
                    const std::string& what) {
  stanli::CompiledModel model = stanli::compile_model(slurp(fixture), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  check(ex.n_params() == static_cast<int64_t>(q.size()),
        what + " parameter count");
  for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
  std::vector<double> grad(q.size(), 0.0);
  eq(what + " lp", ex.gradient(grad.data()), want_lp);
  for (size_t i = 0; i < want_grad.size(); ++i)
    eq(what + " grad " + std::to_string(i), grad[i], want_grad[i]);
}

void test_outer_range() {
  check_gradient("tests/fixtures/viewa_outer_range.tmir.sexp", {},
                 {1, 2, 3, 4, 5}, 432, {0, 1, 10, 100, 0},
                 "outer range a[2:4]");
}

void test_outer_gather() {
  stanli::DataMap data;
  data.set_int_array("idx", {5, 2, 4});
  check_gradient("tests/fixtures/viewa_outer_gather.tmir.sexp", data,
                 {1, 2, 3, 4, 5}, 425, {0, 10, 0, 100, 1},
                 "outer gather a[{5,2,4}]");
}

void test_scalar_array_column() {
  check_gradient("tests/fixtures/viewa_scalar_column.tmir.sexp", {},
                 {1, 2, 3, 4, 5, 6}, 225, {0, 10, 0, 0, 1, 0},
                 "array[2,3] column a[:,2]");
}

void test_contextual_zero_shapes() {
  stanli::DataMap data;
  data.set_real_array("vectors", {}, {0, 4});
  data.set_real_array("rows", {}, {0, 5});
  data.set_real_array("matrices", {}, {0, 2, 3});
  data.set_real_array("scalars", {}, {0, 3});
  check_gradient("tests/fixtures/viewa_contextual_empty.tmir.sexp", data,
                 // 32, not the 36 the pre-registry dims rule produced:
                 // Stan Math's dims() pushes rows and columns for any Eigen
                 // leaf, so dims(array[] row_vector)[2] is the inserted row
                 // extent 1, not the row length (CmdStan prints
                 // dims(array[4] vector[2]) as [4,2,1]).
                 {0.25}, 32.0, {4}, "zero outer container dimensions");
}

void test_explicit_zero_vectors() {
  check_gradient("tests/fixtures/viewa_explicit_zero_vectors.tmir.sexp", {},
                 {0.25}, 200.25, {1},
                 "explicit array[2] vector[0] construction");
}

void test_empty_data_array_matrix() {
  stanli::DataMap data;
  data.set_real_array("A", {}, {0, 2, 3});
  stanli::CompiledModel model = stanli::compile_model(
      slurp("tests/fixtures/viewa_data_empty_matrix.tmir.sexp"), data);
  stanli::Executor ex(std::move(model.graph));
  model.bind(ex);
  ex.params_data()[0] = 0.25;
  double grad[1] = {};
  eq("empty data array-matrix lp", ex.gradient(grad), 23.25);
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
       wx.value_ptr(model.write_array->columns[1].slot)[0], 23.0);
  } else {
    check(false, "empty array-matrix has two scalar write views");
  }
}

void test_register_array_boundary() {
  using namespace stanli;
  mir::Program mir = mir::read_program(
      sexp::parse(slurp("tests/fixtures/viewa_program_arrays.tmir.sexp")));
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
    run_rhs<double>(flat, 0.0, y, theta, xr, out);
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
  run_case("outer range", test_outer_range);
  run_case("outer gather", test_outer_gather);
  run_case("scalar array column", test_scalar_array_column);
  run_case("contextual zero shapes", test_contextual_zero_shapes);
  run_case("explicit zero-vector array", test_explicit_zero_vectors);
  run_case("empty data array matrix", test_empty_data_array_matrix);
  run_case("register array boundary", test_register_array_boundary);
  if (failures == 0) std::printf("test_lower_array_contract OK\n");
  return failures == 0 ? 0 : 1;
}
