// CSV column naming and write_array structure.
//
// The column names are a contract with every downstream reader, and two of
// the rules were real bugs found only by diffing headers against CmdStan
// (tools/verify_refs.py --wa-headers, which needs CmdStan and cannot run in
// CI): a container is indexed even at length one (`vector[1] v` is v.1, not
// v), and a matrix carries two indices in column-major order (M.1.1, M.2.1,
// M.1.2). This test pins those rules in-repo, at two levels:
//
//   * csv_names on hand-built ParamViews, one per naming rule, and
//   * the whole pipeline on tests/fixtures/wanames.stan, whose expected
//     header was taken verbatim from a CmdStan run of the same model.
#include <stanli/compile.hpp>
#include <stanli/mir.hpp>
#include <stanli/optable.hpp>
#include <stanli/sexp.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect_eq(const std::string& what, const std::string& got,
               const std::string& want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %s\n  got  %s\n  want %s\n", what.c_str(), got.c_str(),
                want.c_str());
  }
}

void expect_idx(const std::string& what, size_t got, size_t want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %s: got %zu want %zu\n", what.c_str(), got, want);
  }
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

stanli::DataMap bound_check_data(int N = 1, int M = 1, int R = 1, int C = 1,
                                 int BR = 1, int BC = 1) {
  stanli::DataMap d;
  d.set_real("d", 0.0);
  d.set_real("raw", 0.0);
  d.set_int("N", N);
  d.set_int("M", M);
  d.set_real_array("lo", std::vector<double>((size_t)M, -10.0));
  d.set_int("R", R);
  d.set_int("C", C);
  d.set_int("BR", BR);
  d.set_int("BC", BC);
  d.set_real_array("matrix_lo",
                   std::vector<double>((size_t)BR * (size_t)BC, -10.0),
                   {BR, BC});
  return d;
}

std::map<std::string, stanli::DataMap::Entry> bound_check_env(
    const stanli::DataMap& data) {
  std::map<std::string, stanli::DataMap::Entry> env;
  for (const char* name :
       {"d", "raw", "N", "M", "lo", "R", "C", "BR", "BC", "matrix_lo"})
    env[name] = data.at(name);
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    stanli::DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    env[flag] = one;
  }
  return env;
}

std::string joined(const std::vector<stanli::CompiledModel::ParamView>& cols) {
  std::string h;
  for (const auto& n : stanli::CompiledModel::csv_names(cols)) {
    if (!h.empty()) h += ',';
    h += n;
  }
  return h;
}

void test_naming_rules() {
  using PV = stanli::CompiledModel::ParamView;
  using N = PV::Naming;

  expect_eq("scalar is bare", joined({PV{"s", 0, 1, N::Scalar, 0}}), "s");
  // The length-one container: the bug was emitting "v".
  expect_eq("vector[1] is v.1", joined({PV{"v", 0, 1, N::Container, 0}}),
            "v.1");
  expect_eq("vector[3]", joined({PV{"v", 0, 3, N::Container, 0}}),
            "v.1,v.2,v.3");
  // The matrix: the bug was emitting M.1 .. M.6.
  expect_eq("matrix[2,3] col-major", joined({PV{"M", 0, 6, N::Matrix, 2}}),
            "M.1.1,M.2.1,M.1.2,M.2.2,M.1.3,M.2.3");
  PV array_vector{"V", 0, 4, N::Container, 0};
  array_vector.set_serial_layout({2, 2}, false);
  expect_eq("array[2] vector[2] first index fastest", joined({array_vector}),
            "V.1.1,V.2.1,V.1.2,V.2.2");
  const std::vector<int64_t> vector_storage{0, 2, 1, 3};
  for (int64_t i = 0; i < array_vector.len; ++i)
    if (array_vector.storage_index(i) != vector_storage[(size_t)i]) {
      ++failures;
      std::printf("FAIL array-vector storage index %lld: got %lld want %lld\n",
                  static_cast<long long>(i),
                  static_cast<long long>(array_vector.storage_index(i)),
                  static_cast<long long>(vector_storage[(size_t)i]));
    }
  PV array_matrix{"A", 0, 12, N::Matrix, 2};
  array_matrix.set_serial_layout({2, 2, 3}, true);
  expect_eq("array[2] matrix[2,3] first index fastest", joined({array_matrix}),
            "A.1.1.1,A.2.1.1,A.1.2.1,A.2.2.1,A.1.1.2,A.2.1.2,"
            "A.1.2.2,A.2.2.2,A.1.1.3,A.2.1.3,A.1.2.3,A.2.2.3");
  const std::vector<int64_t> matrix_storage{0, 6, 1, 7,  2, 8,
                                            3, 9, 4, 10, 5, 11};
  for (int64_t i = 0; i < array_matrix.len; ++i)
    if (array_matrix.storage_index(i) != matrix_storage[(size_t)i]) {
      ++failures;
      std::printf("FAIL array-matrix storage index %lld: got %lld want %lld\n",
                  static_cast<long long>(i),
                  static_cast<long long>(array_matrix.storage_index(i)),
                  static_cast<long long>(matrix_storage[(size_t)i]));
    }
  // Auto (the log_prob views): bare at length one, indexed above.
  expect_eq("auto scalar", joined({PV{"a", 0, 1, N::Auto, 0}}), "a");
  expect_eq("auto vector", joined({PV{"a", 0, 2, N::Auto, 0}}), "a.1,a.2");
}

void test_wanames_pipeline() {
  using namespace stanli;
  DataMap data;  // the model takes no data
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/wanames.tmir.sexp"), data);
  if (!cm.write_array || cm.write_array->columns.empty()) {
    ++failures;
    std::printf("FAIL wanames: no write_array\n");
    return;
  }
  expect_eq("wanames truncated", cm.write_array->truncated, "");
  // Taken verbatim from CmdStan's CSV for this model (diagnostic columns
  // dropped). Note gq's order: Stan flattens an array of vectors with the
  // FIRST index varying fastest, like everything else it serializes.
  expect_eq("wanames header (CmdStan-verbatim)",
            joined(cm.write_array->columns),
            "s,v.1,M.1.1,M.2.1,M.1.2,M.2.2,M.1.3,M.2.3,"
            "gq.1.1,gq.2.1,gq.1.2,gq.2.2");

  // Generated quantities but no transformed parameters: the two section
  // boundaries coincide, right after the three constrained parameters.
  expect_idx("wanames n_tp_start", cm.write_array->n_tp_start, 3);
  expect_idx("wanames n_gq_start", cm.write_array->n_gq_start, 3);

  // And the write_array graph computes the right VALUES, not just names:
  // gq[i][j] = s + 10*i + j is exact arithmetic on the draw.
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  const double s = 0.375;
  wex.params_data()[0] = s;  // s is the first unconstrained parameter
  wex.run_forward_only();
  int64_t at = 0;
  double gq[4] = {0, 0, 0, 0};
  const auto names = CompiledModel::csv_names(cm.write_array->columns);
  for (const auto& c : cm.write_array->columns) {
    const double* p = wex.value_ptr(c.slot);
    for (int64_t k = 0; k < c.len; ++k, ++at)
      if (names[(size_t)at].rfind("gq.", 0) == 0) {
        const int i = names[(size_t)at][3] - '0';
        const int j = names[(size_t)at][5] - '0';
        gq[(i - 1) * 2 + (j - 1)] = p[k];
      }
  }
  for (int i = 1; i <= 2; ++i)
    for (int j = 1; j <= 2; ++j) {
      const double want = s + 10.0 * i + j;
      const double got = gq[(i - 1) * 2 + (j - 1)];
      if (got != want) {
        ++failures;
        std::printf("FAIL wanames gq.%d.%d: got %.17g want %.17g\n", i, j, got,
                    want);
      }
    }
}

// Run the same write-array MIR directly through the broad interpreter. The
// graph and interpreter intentionally store values differently; what they
// must share is the logical column schema and the observable row.
void test_wanames_interpreter_schema() {
  using namespace stanli;
  auto prog = std::make_shared<mir::Program>(mir::read_program(
      sexp::parse(slurp("tests/fixtures/wanames.tmir.sexp"))));

  std::map<std::string, DataMap::Entry> base;
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp wi(prog, std::move(base));

  std::map<std::string, DataMap::Entry> params;
  params["s"].r = {0.375};
  params["v"].r = {5.0};
  params["v"].dims = {1};
  params["M"].r = {11.0, 12.0, 13.0, 14.0, 15.0, 16.0};
  params["M"].dims = {2, 3};

  WaRng rng(1234);
  const std::vector<double> row = wi.eval(params, rng);
  expect_eq("wanames interpreted header", joined(wi.columns()),
            "s,v.1,M.1.1,M.2.1,M.1.2,M.2.2,M.1.3,M.2.3,"
            "gq.1.1,gq.2.1,gq.1.2,gq.2.2");
  const std::vector<double> want{0.375, 5.0,  11.0,   12.0,   13.0,   14.0,
                                 15.0,  16.0, 11.375, 21.375, 12.375, 22.375};
  if (row != want) {
    ++failures;
    std::printf("FAIL wanames interpreted row\n");
    const size_t n = std::max(row.size(), want.size());
    for (size_t i = 0; i < n; ++i) {
      const double got = i < row.size() ? row[i] : NAN;
      const double expected = i < want.size() ? want[i] : NAN;
      if (got != expected)
        std::printf("  [%zu] got %.17g want %.17g\n", i, got, expected);
    }
  }
}

// Generated quantities the graph cannot express (per-draw control flow) run
// through the interpreted fallback: same columns contract, per-draw
// evaluation, seeded RNG stream.
void test_interpreted_gq() {
  using namespace stanli;
  DataMap data;
  data.set_int("N", 5);
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/gqrng.tmir.sexp"), data);
  if (!cm.write_array || !cm.write_array->interp) {
    ++failures;
    std::printf("FAIL gqrng: no interpreted write_array\n");
    return;
  }
  WaInterp& wi = *cm.write_array->interp;
  std::map<std::string, DataMap::Entry> params;
  DataMap::Entry sig;
  sig.r = {1.7};
  params["sigma"] = sig;
  WaRng rng(42);
  const std::vector<double> r1 = wi.eval(params, rng);
  expect_eq("gqrng header", joined(wi.columns()), "sigma,yrep,crep,branchy,p");
  // The interpreter discovers the same section boundaries the graph
  // lowering records: one constrained parameter, no transformed
  // parameters, the rest generated quantities.
  expect_idx("gqrng interp n_tp_start", wi.n_tp_start(), 1);
  expect_idx("gqrng interp n_gq_start", wi.n_gq_start(), 1);
  if (r1.size() != 5) {
    ++failures;
    std::printf("FAIL gqrng: row size %zu\n", r1.size());
    return;
  }
  auto expect_val = [&](const char* what, double got, double want) {
    if (got != want) {
      ++failures;
      std::printf("FAIL gqrng %s: got %.17g want %.17g\n", what, got, want);
    }
  };
  expect_val("sigma passthrough", r1[0], 1.7);
  expect_val("branchy (sigma > 1)", r1[3], 1.0);
  expect_val("prod", r1[4], 6.0);
  const double crep = r1[2];
  if (crep != std::floor(crep) || crep < 0.0 || crep > 5.0) {
    ++failures;
    std::printf("FAIL gqrng crep: %.17g not an int in [0,5]\n", crep);
  }
  rng.seed(42);
  const std::vector<double> r2 = wi.eval(params, rng);
  if (r1 != r2) {
    ++failures;
    std::printf("FAIL gqrng: same seed, different row\n");
  }
  const std::vector<double> r3 = wi.eval(params, rng);
  if (r3[1] == r1[1]) {
    ++failures;
    std::printf("FAIL gqrng: RNG stream did not advance across draws\n");
  }
}

// An array of matrices carries two layouts at once: the array index is
// outermost with each element contiguous, and within an element the storage
// is column-major. Reading m[k, i, j] with one row-major stride computation
// over all three dimensions transposes every element while leaving the
// column names right, which is invisible in the log density (nothing
// indexes a whole matrix by scalar) and wrong in every draw.
void test_array_of_matrix_columns() {
  using namespace stanli;
  DataMap data = DataMap::from_json_file("tests/fixtures/amatwa.json");
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/amatwa.tmir.sexp"), data);
  if (!cm.write_array || cm.write_array->columns.empty()) {
    ++failures;
    std::printf("FAIL amatwa: no write_array\n");
    return;
  }
  Executor pex(std::move(cm.graph));
  cm.bind(pex);
  for (int64_t k = 0; k < pex.n_params(); ++k) pex.params_data()[k] = (double)k;
  pex.run_forward_only();

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  // Distinct values, so any misplaced element names itself.
  for (int64_t k = 0; k < wex.n_params(); ++k) wex.params_data()[k] = (double)k;
  wex.run_forward_only();

  std::map<std::string, double> row;
  const auto names = CompiledModel::csv_names(cm.write_array->columns);
  int64_t at = 0;
  for (const auto& c : cm.write_array->columns) {
    const double* p = wex.value_ptr(c.slot);
    for (int64_t k = 0; k < c.len; ++k, ++at) row[names[(size_t)at]] = p[k];
  }
  auto expect_val = [&](const std::string& col, double want) {
    auto it = row.find(col);
    if (it == row.end()) {
      ++failures;
      std::printf("FAIL amatwa: no column %s\n", col.c_str());
    } else if (it->second != want) {
      ++failures;
      std::printf("FAIL amatwa %s: got %.17g want %.17g\n", col.c_str(),
                  it->second, want);
    }
  };
  for (int k = 1; k <= 2; ++k)
    for (int i = 1; i <= 2; ++i)
      for (int j = 1; j <= 3; ++j) {
        const std::string ix = "." + std::to_string(k) + "." +
                               std::to_string(i) + "." + std::to_string(j);
        // element (i, j) of matrix k sits at 6*(k-1) + 2*(j-1) + (i-1)
        expect_val("m" + ix, (double)(6 * (k - 1) + 2 * (j - 1) + (i - 1)));
        // the generated quantity says which cell it thinks it is
        expect_val("g" + ix, (double)(100 * k + 10 * i + j));
        // and so does the data it copied
        expect_val("gd" + ix, (double)(100 * k + 10 * i + j));
      }

  // The fallback interpreter has a different physical layout. Feed it
  // through CompiledModel's one arena-to-logical boundary and require the
  // exact same named row, including the rank-three parameter that the old
  // boundary rejected outright.
  auto prog = std::make_shared<mir::Program>(
      mir::read_program(sexp::parse(slurp("tests/fixtures/amatwa.tmir.sexp"))));
  std::map<std::string, DataMap::Entry> base;
  base["d"] = data.at("d");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp interpreted(prog, std::move(base));
  WaRng rng(1234);
  const std::vector<double> interpreted_values =
      interpreted.eval(cm.constrained_env(pex), rng);
  const std::vector<std::string> interpreted_names =
      CompiledModel::csv_names(interpreted.columns());
  std::map<std::string, double> interpreted_row;
  for (size_t i = 0; i < interpreted_values.size(); ++i)
    interpreted_row[interpreted_names.at(i)] = interpreted_values[i];
  if (interpreted_row != row) {
    ++failures;
    std::printf("FAIL amatwa: interpreted and compiled rows differ\n");
    for (const auto& [name, value] : row) {
      auto it = interpreted_row.find(name);
      if (it == interpreted_row.end())
        std::printf("  missing interpreted column %s\n", name.c_str());
      else if (it->second != value)
        std::printf("  %s got %.17g want %.17g\n", name.c_str(), it->second,
                    value);
    }
  }
}

// The RNG state belongs to the CALLER, not to the model. One WaInterp
// serves any number of independent streams, which is what lets concurrent
// chains draw generated quantities through one shared model without
// sharing a stream -- the thing a model-owned member cannot express.
void test_caller_owned_rng() {
  using namespace stanli;
  DataMap data;
  data.set_int("N", 5);
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/gqrng.tmir.sexp"), data);
  if (!cm.write_array || !cm.write_array->interp) {
    ++failures;
    std::printf("FAIL caller-owned rng: no interpreted write_array\n");
    return;
  }
  WaInterp& wi = *cm.write_array->interp;
  std::map<std::string, DataMap::Entry> params;
  DataMap::Entry sig;
  sig.r = {1.7};
  params["sigma"] = sig;

  // Two states, same seed, interleaved through ONE interpreter: each
  // advances on its own, so the two sequences agree draw for draw.
  WaRng a(42), b(42);
  const std::vector<double> a1 = wi.eval(params, a);
  const std::vector<double> b1 = wi.eval(params, b);
  if (a1 != b1) {
    ++failures;
    std::printf("FAIL caller-owned rng: same seed, different first draw\n");
  }
  const std::vector<double> a2 = wi.eval(params, a);
  const std::vector<double> b2 = wi.eval(params, b);
  if (a2 != b2) {
    ++failures;
    std::printf("FAIL caller-owned rng: streams diverged on the second draw\n");
  }
  if (a2[1] == a1[1]) {
    ++failures;
    std::printf("FAIL caller-owned rng: stream did not advance\n");
  }
  // A distinct seed is a distinct stream.
  WaRng c(43);
  const std::vector<double> c1 = wi.eval(params, c);
  if (c1[1] == a1[1]) {
    ++failures;
    std::printf("FAIL caller-owned rng: seed 43 matched seed 42\n");
  }
}

void test_transformed_parameter_checks() {
  using namespace stanli;
  const std::string mir = slurp("tests/fixtures/data_and_tp_checks.tmir.sexp");
  DataMap data = bound_check_data();
  CompiledModel cm = compile_model(mir, data);
  if (!cm.write_array || cm.write_array->interp) {
    ++failures;
    std::printf("FAIL bound-check fixture has no compiled write_array\n");
    return;
  }

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  wex.params_data()[0] = -1.0;
  bool graph_rejected = false;
  try {
    wex.run_forward_only();
  } catch (const std::domain_error& e) {
    graph_rejected = std::string(e.what()).find("z") != std::string::npos;
  }
  if (!graph_rejected) {
    ++failures;
    std::printf("FAIL compiled write_array did not enforce z lower bound\n");
  }

  auto prog =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(mir)));
  WaInterp interpreted(prog, bound_check_env(data));
  std::map<std::string, DataMap::Entry> params;
  params["x"].r = {-1.0};
  WaRng rng(1234);
  bool interp_rejected = false;
  try {
    (void)interpreted.eval(params, rng);
  } catch (const std::domain_error& e) {
    interp_rejected = std::string(e.what()).find("z") != std::string::npos;
  }
  if (!interp_rejected) {
    ++failures;
    std::printf("FAIL interpreted write_array did not enforce z lower bound\n");
  }

  auto compiled_shape_rejects = [&](DataMap mismatch, double x,
                                    const std::string& name) {
    try {
      CompiledModel bad = compile_model(mir, mismatch);
      if (!bad.write_array || bad.write_array->interp) return false;
      Executor ex(std::move(bad.write_array->graph));
      bad.write_array->bind(ex);
      ex.params_data()[0] = x;
      try {
        ex.run_forward_only();
      } catch (const std::invalid_argument& e) {
        return std::string(e.what()).find(name) != std::string::npos;
      }
    } catch (const std::exception&) {
    }
    return false;
  };
  auto interpreted_shape_rejects = [&](DataMap mismatch, double x,
                                       const std::string& name) {
    try {
      WaInterp wi(prog, bound_check_env(mismatch));
      std::map<std::string, DataMap::Entry> p;
      p["x"].r = {x};
      WaRng local_rng(1234);
      try {
        (void)wi.eval(p, local_rng);
      } catch (const std::invalid_argument& e) {
        return std::string(e.what()).find(name) != std::string::npos;
      }
    } catch (const std::exception&) {
    }
    return false;
  };
  const DataMap vector_mismatch = bound_check_data(1, 2);
  const DataMap matrix_mismatch = bound_check_data(1, 1, 1, 2, 2, 1);
  if (!compiled_shape_rejects(vector_mismatch, 0.0, "bounded") ||
      !interpreted_shape_rejects(vector_mismatch, 0.0, "bounded")) {
    ++failures;
    std::printf("FAIL write_array vector-bound mismatch phase\n");
  }
  if (!compiled_shape_rejects(vector_mismatch, -1.0, "bounded") ||
      !interpreted_shape_rejects(vector_mismatch, -1.0, "bounded")) {
    ++failures;
    std::printf("FAIL write_array dimension-check ordering\n");
  }
  if (!compiled_shape_rejects(matrix_mismatch, 0.0, "bounded_matrix") ||
      !interpreted_shape_rejects(matrix_mismatch, 0.0, "bounded_matrix")) {
    ++failures;
    std::printf("FAIL write_array matrix-bound mismatch phase\n");
  }
}

}  // namespace

// A generated quantity the optimizer folds to a constant: --O1 replaces
// the FnWriteParam's variable reference with the literal value, so the
// column's name has to come from the program's output_vars instead.
void test_constant_folded_gq_column() {
  using namespace stanli;
  DataMap data;
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/gqconst.tmir.sexp"), data);
  if (!cm.write_array || !cm.write_array->interp) {
    ++failures;
    std::printf("FAIL gqconst: no interpreted write_array\n");
    return;
  }
  WaInterp& wi = *cm.write_array->interp;
  std::map<std::string, DataMap::Entry> params;
  DataMap::Entry x;
  x.r = {0.25};
  params["x"] = x;
  WaRng rng(1);
  const std::vector<double> row = wi.eval(params, rng);
  expect_eq("gqconst header", joined(wi.columns()), "x,z");
  if (row.size() != 2 || row[0] != 0.25 || row[1] != 3.0) {
    ++failures;
    std::printf("FAIL gqconst row: got %zu values\n", row.size());
  }
}

// Keep the shared helper a transparent call into Stan Math. In particular,
// endpoint results must not become shortcuts: Stan Math owns validation and
// however much of the engine it consumes, including at N=0 and p in {0,1}.
void test_binomial_rng_helper_contract() {
  using namespace stanli;
  const auto helper_draw = [](int n, double p, WaRng& rng) {
    const double args[] = {static_cast<double>(n), p};
    return scalar_rng_draw(ScalarRng::Binomial, args, 2, rng);
  };
  const auto next = [](WaRng& rng) {
    return stan::math::uniform_rng(0.0, 1.0, rng.gen());
  };

  struct ValidCase {
    int n;
    double p;
  };
  const ValidCase valid[] = {
      {0, 0.0}, {0, 0.37}, {0, 1.0},  {9, -0.0},   {9, 0.0},
      {9, 1.0}, {20, 0.5}, {21, 0.5}, {100, 0.49}, {100, 0.51},
  };
  for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
    WaRng got_rng(static_cast<unsigned>(101 + i));
    WaRng want_rng(static_cast<unsigned>(101 + i));
    const double got = helper_draw(valid[i].n, valid[i].p, got_rng);
    const double want = static_cast<double>(
        stan::math::binomial_rng(valid[i].n, valid[i].p, want_rng.gen()));
    if (got != want || next(got_rng) != next(want_rng)) {
      ++failures;
      std::printf("FAIL binomial helper valid case %zu\n", i);
    }
  }

  // Both-invalid cases pin Stan Math's validation priority. The remaining
  // cases cover finite values just outside the interval, NaN, and infinities.
  const ValidCase invalid[] = {
      {-1, 0.4},
      {-1, -0.2},
      {4, -0.1},
      {4, std::nextafter(1.0, std::numeric_limits<double>::infinity())},
      {4, std::numeric_limits<double>::quiet_NaN()},
      {4, std::numeric_limits<double>::infinity()},
      {4, -std::numeric_limits<double>::infinity()},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    WaRng got_rng(static_cast<unsigned>(211 + i));
    WaRng want_rng(static_cast<unsigned>(211 + i));
    std::string got_message, want_message;
    try {
      (void)helper_draw(invalid[i].n, invalid[i].p, got_rng);
    } catch (const std::domain_error& e) {
      got_message = e.what();
    }
    try {
      (void)stan::math::binomial_rng(invalid[i].n, invalid[i].p,
                                     want_rng.gen());
    } catch (const std::domain_error& e) {
      want_message = e.what();
    }
    if (got_message.empty() || got_message != want_message ||
        next(got_rng) != next(want_rng)) {
      ++failures;
      std::printf("FAIL binomial helper invalid case %zu\n", i);
    }
  }
}

void test_binomial_rng_lowering_guards() {
  using namespace stanli;
  const std::string base = slurp("tests/fixtures/gq_scalar_rng.tmir.sexp");
  const size_t call = base.find("(FunApp (StanLib binomial_rng");
  if (call == std::string::npos) {
    ++failures;
    std::printf("FAIL binomial lowering guard fixture has no call\n");
    return;
  }

  const auto expect_interp = [](const std::string& mir,
                                const std::string& reason, const char* what) {
    DataMap data;
    CompiledModel cm = compile_model(mir, data);
    if (!cm.write_array || !cm.write_array->interp ||
        cm.write_array->truncated.find(reason) == std::string::npos) {
      ++failures;
      std::printf("FAIL %s: got %s\n", what,
                  cm.write_array ? cm.write_array->truncated.c_str()
                                 : "no write_array");
    }
  };

  const std::string scalar_trials =
      "((pattern (Lit Int 5))\n"
      "               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))";
  const size_t trials = base.find(scalar_trials, call);
  if (trials == std::string::npos) {
    ++failures;
    std::printf("FAIL binomial lowering guard cannot find trials\n");
    return;
  }

  std::string malformed = base;
  malformed.replace(trials, scalar_trials.size(),
                    "((pattern (Lit Real 5.0))\n"
                    "               (meta ((type_ UReal) (loc <opaque>) "
                    "(adlevel DataOnly))))");
  expect_interp(malformed, "first argument must be int",
                "binomial real trials stay interpreted");

  std::string container_arg = base;
  container_arg.replace(
      trials, scalar_trials.size(),
      "((pattern\n"
      "                (FunApp (CompilerInternal FnMakeArray)\n"
      "                 (((pattern (Lit Int 5))\n"
      "                   (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly)))))))\n"
      "               (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel "
      "DataOnly))))");
  expect_interp(container_arg, "container arguments stay on WaInterp",
                "binomial container argument stays interpreted");

  std::string container_result = base;
  const std::string result_meta =
      "\n           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))";
  const size_t result = container_result.find(result_meta, call);
  if (result == std::string::npos) {
    ++failures;
    std::printf("FAIL binomial lowering guard cannot find result type\n");
    return;
  }
  container_result.replace(result, result_meta.size(),
                           "\n           (meta ((type_ (UArray UInt)) (loc "
                           "<opaque>) (adlevel DataOnly)))");
  expect_interp(container_result, "expected scalar result",
                "binomial container result stays interpreted");
}

void test_compiled_scalar_rng() {
  using namespace stanli;
  const std::string path = "tests/fixtures/gq_scalar_rng.tmir.sexp";
  const std::string text = slurp(path);
  DataMap data;
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf("FAIL scalar rng: write_array did not compile completely\n");
    return;
  }
  int rng_ops = 0;
  for (const Op& op : cm.write_array->graph.ops)
    if (op.opcode == OP_RNG) ++rng_ops;
  if (rng_ops != 6) {
    ++failures;
    std::printf("FAIL scalar rng: got %d OP_RNG, want 6\n", rng_ops);
  }
  expect_eq("scalar rng columns", joined(cm.write_array->columns),
            "x,p,u,b,n,l,k");

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  const double x = 0.25;
  wex.params_data()[0] = x;
  const auto graph_row = [&](WaRng& rng) {
    wex.run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& c : cm.write_array->columns) {
      const double* p = wex.value_ptr(c.slot);
      for (int64_t i = 0; i < c.len; ++i) row.push_back(p[c.storage_index(i)]);
    }
    return row;
  };

  auto prog =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> base;
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp interp(prog, std::move(base));
  std::map<std::string, DataMap::Entry> params;
  params["x"].r = {x};

  WaRng graph_rng(42), interp_rng(42);
  const std::vector<double> graph_first = graph_row(graph_rng);
  const std::vector<double> interp_first = interp.eval(params, interp_rng);
  if (graph_first != interp_first) {
    ++failures;
    std::printf("FAIL scalar rng: graph/interpreter first rows differ\n");
  }
  const std::vector<double> graph_second = graph_row(graph_rng);
  const std::vector<double> interp_second = interp.eval(params, interp_rng);
  if (graph_second != interp_second || graph_second == graph_first) {
    ++failures;
    std::printf("FAIL scalar rng: consecutive stream rows disagree\n");
  }
  graph_rng.seed(42);
  if (graph_row(graph_rng) != graph_first) {
    ++failures;
    std::printf("FAIL scalar rng: reseed did not reproduce first row\n");
  }

  // Two caller-owned streams stay independent even when interleaved through
  // the same mutable executor.
  WaRng a(91), b(91);
  const auto a1 = graph_row(a);
  const auto b1 = graph_row(b);
  const auto a2 = graph_row(a);
  const auto b2 = graph_row(b);
  if (a1 != b1 || a2 != b2 || a1 == a2) {
    ++failures;
    std::printf("FAIL scalar rng: independent streams diverged or stalled\n");
  }
  Executor clone(wex);
  clone.params_data()[0] = x;
  const auto clone_row = [&](WaRng& rng) {
    clone.run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& c : cm.write_array->columns) {
      const double* p = clone.value_ptr(c.slot);
      for (int64_t i = 0; i < c.len; ++i) row.push_back(p[c.storage_index(i)]);
    }
    return row;
  };
  WaRng original_stream(123), clone_stream(123);
  if (graph_row(original_stream) != clone_row(clone_stream) ||
      graph_row(original_stream) != clone_row(clone_stream)) {
    ++failures;
    std::printf("FAIL scalar rng: executor copy did not rebind state\n");
  }

  // No implicit/default stream. A throwing evaluation must restore that
  // empty state too, so a pooled executor cannot retain the caller's pointer.
  bool missing = false;
  try {
    wex.run_forward_only();
  } catch (const std::logic_error& e) {
    missing = std::string(e.what()).find("caller-owned") != std::string::npos;
  }
  if (!missing) {
    ++failures;
    std::printf("FAIL scalar rng: missing evaluation state was accepted\n");
  }
  wex.params_data()[0] = std::numeric_limits<double>::infinity();
  params["x"].r = {std::numeric_limits<double>::infinity()};
  WaRng failing(17), interp_failing(17);
  bool domain = false, interp_domain = false;
  try {
    (void)graph_row(failing);
  } catch (const std::domain_error&) {
    domain = true;
  }
  try {
    (void)interp.eval(params, interp_failing);
  } catch (const std::domain_error&) {
    interp_domain = true;
  }
  wex.params_data()[0] = x;
  bool restored = false;
  try {
    wex.run_forward_only();
  } catch (const std::logic_error& e) {
    restored = std::string(e.what()).find("caller-owned") != std::string::npos;
  }
  params["x"].r = {x};
  const std::vector<double> recovered = graph_row(failing);
  const std::vector<double> interp_recovered =
      interp.eval(params, interp_failing);
  if (!domain || !interp_domain || !restored || recovered != interp_recovered) {
    ++failures;
    std::printf("FAIL scalar rng: exception/reuse contract\n");
  }
}

int main() {
  test_naming_rules();
  test_wanames_pipeline();
  test_wanames_interpreter_schema();
  test_array_of_matrix_columns();
  test_interpreted_gq();
  test_constant_folded_gq_column();
  test_binomial_rng_helper_contract();
  test_binomial_rng_lowering_guards();
  test_compiled_scalar_rng();
  test_caller_owned_rng();
  test_transformed_parameter_checks();
  if (failures == 0) std::printf("test_write_array OK\n");
  return failures == 0 ? 0 : 1;
}
