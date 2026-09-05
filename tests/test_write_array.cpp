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
#include <stanli/mir_interp.hpp>
#include <stanli/optable.hpp>
#include <stanli/sexp.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>
#include <stan/model/indexing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// A bare real container is NaN in every element the block never assigns,
// which is what CmdStan writes and what the interpreter already produced.
void test_gq_bare_fill_is_nan() {
  using namespace stanli;
  DataMap data;
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/gqbarefill.tmir.sexp"), data);
  if (!cm.write_array || cm.write_array->columns.empty()) {
    ++failures;
    std::printf("FAIL gqbarefill: no write_array\n");
    return;
  }
  expect_eq("gqbarefill truncated", cm.write_array->truncated, "");
  expect_eq("gqbarefill header", joined(cm.write_array->columns),
            "mu,q.1,q.2,q.3");

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  wex.params_data()[0] = 0.375;
  wex.run_forward_only();
  std::vector<double> row;
  for (const auto& c : cm.write_array->columns) {
    const double* p = wex.value_ptr(c.slot);
    row.insert(row.end(), p, p + c.len);
  }
  expect_idx("gqbarefill row width", row.size(), 4);
  if (row.size() != 4) return;
  if (row[0] != 0.375 || row[1] != 5.0) {
    ++failures;
    std::printf("FAIL gqbarefill assigned elements: got %.17g %.17g\n", row[0],
                row[1]);
  }
  for (size_t i = 2; i < 4; ++i)
    if (!std::isnan(row[i])) {
      ++failures;
      std::printf("FAIL gqbarefill q.%zu: got %.17g want nan\n", i, row[i]);
    }
}

// Generated quantities may reuse a model-block name at a different width.
// The CSV carries the generated-quantities declaration only.
void test_gq_name_shadowing() {
  using namespace stanli;
  DataMap data;
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/gqshadow.tmir.sexp"), data);
  if (!cm.write_array || cm.write_array->columns.empty()) {
    ++failures;
    std::printf("FAIL gqshadow: no write_array\n");
    return;
  }
  expect_eq("gqshadow truncated", cm.write_array->truncated, "");
  expect_eq("gqshadow header", joined(cm.write_array->columns), "mu,x.1,x.2");

  const std::vector<double> want{0.375, 3.75, 7.5};
  auto expect_row = [&](const char* what, const std::vector<double>& got) {
    if (got != want) {
      ++failures;
      std::printf("FAIL gqshadow %s row\n", what);
      const size_t n = std::max(got.size(), want.size());
      for (size_t i = 0; i < n; ++i)
        std::printf("  [%zu] got %.17g want %.17g\n", i,
                    i < got.size() ? got[i] : NAN,
                    i < want.size() ? want[i] : NAN);
    }
  };

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  wex.params_data()[0] = 0.375;
  wex.run_forward_only();
  std::vector<double> row;
  for (const auto& c : cm.write_array->columns) {
    const double* p = wex.value_ptr(c.slot);
    row.insert(row.end(), p, p + c.len);
  }
  expect_row("compiled", row);

  // A stale three-wide `x` in the base environment must not widen the row.
  auto prog = std::make_shared<mir::Program>(mir::read_program(
      sexp::parse(slurp("tests/fixtures/gqshadow.tmir.sexp"))));
  std::map<std::string, DataMap::Entry> base;
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  base["x"].r = {1.0, 1.0, 1.0};
  base["x"].dims = {3};
  WaInterp interpreted(prog, std::move(base));
  std::map<std::string, DataMap::Entry> params;
  params["mu"].r = {0.375};
  WaRng rng(1234);
  expect_row("interpreted", interpreted.eval(params, rng));
  expect_eq("gqshadow interpreted header", joined(interpreted.columns()),
            "mu,x.1,x.2");
}

// WaInterp remains the independent oracle for caller-owned RNG and runtime
// control now that the same section can also compile completely.
void test_interpreted_gq() {
  using namespace stanli;
  DataMap data;
  data.set_int("N", 5);
  const std::string text = slurp("tests/fixtures/gqrng.tmir.sexp");
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf("FAIL gqrng: runtime branch did not compile completely\n");
    return;
  }
  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> base;
  base["N"] = data.at("N");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp wi(program, std::move(base));
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

// gumbel_rng, dirichlet_rng and beta_binomial_rng all now have graph
// opcodes, so the whole generated-quantities section compiles completely --
// no WaInterp fallback. Drive the compiled graph and check every draw
// matches stan-math on a parallel WaRng consumed in the same source order:
// gumbel first, then the whole-vector dirichlet, then the integer
// beta_binomial. A separately constructed WaInterp on the same MIR is the
// second witness, so a graph/interpreter divergence cannot hide behind
// agreement with only one of the two.
void test_compiled_extra_rng() {
  using namespace stanli;
  DataMap data;
  data.set_int("N", 10);
  const std::string text = slurp("tests/fixtures/gqrng_extra.tmir.sexp");
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL gqrng_extra did not compile completely: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }
  expect_eq("gqrng_extra columns", joined(cm.write_array->columns),
            "sigma,g,d.1,d.2,d.3,bb");

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  wex.params_data()[0] = std::log(1.3);  // sigma's unconstrained log_sd form
  const auto graph_row = [&](WaRng& rng) {
    wex.run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& c : cm.write_array->columns) {
      const double* p = wex.value_ptr(c.slot);
      for (int64_t i = 0; i < c.len; ++i) row.push_back(p[c.storage_index(i)]);
    }
    return row;
  };

  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> base;
  base["N"] = data.at("N");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp wi(program, std::move(base));
  std::map<std::string, DataMap::Entry> params;
  DataMap::Entry sig;
  sig.r = {1.3};
  params["sigma"] = sig;

  WaRng graph_rng(7), interp_rng(7), ref(7);
  const std::vector<double> row = graph_row(graph_rng);
  const std::vector<double> interp_row = wi.eval(params, interp_rng);
  if (row != interp_row) {
    ++failures;
    std::printf("FAIL gqrng_extra: graph and interpreter rows differ\n");
  }
  if (row.size() != 6) {
    ++failures;
    std::printf("FAIL gqrng_extra: row size %zu\n", row.size());
    return;
  }
  auto& g = ref.gen();
  const double want_g = stan::math::gumbel_rng(0.5, 1.3, g);
  Eigen::VectorXd alpha(3);
  alpha << 1.0, 2.0, 3.0;
  const Eigen::VectorXd want_d = stan::math::dirichlet_rng(alpha, g);
  const int want_bb = stan::math::beta_binomial_rng(10, 2.0, 3.0, g);

  auto expect_val = [&](const char* what, double got, double want) {
    if (got != want) {
      ++failures;
      std::printf("FAIL gqrng_extra %s: got %.17g want %.17g\n", what, got,
                  want);
    }
  };
  expect_val("sigma passthrough", row[0], 1.3);
  expect_val("gumbel draw", row[1], want_g);
  expect_val("dirichlet[0]", row[2], want_d[0]);
  expect_val("dirichlet[1]", row[3], want_d[1]);
  expect_val("dirichlet[2]", row[4], want_d[2]);
  expect_val("beta_binomial draw", row[5], (double)want_bb);
  if (std::abs((row[2] + row[3] + row[4]) - 1.0) > 1e-12) {
    ++failures;
    std::printf("FAIL gqrng_extra: dirichlet draw is not a simplex\n");
  }
}

// RNG draws inside a runtime-control region: the section compiles to the
// register machine, which runs the graph's own OP_RNG kernel through
// Program::CALL. The interpreter is the reference for what the section used
// to do -- same draws, same order, same positions in one stream -- and
// stan-math for what each draw is.
void test_compiled_region_rng() {
  using namespace stanli;
  DataMap data;
  data.set_int("K", 3);
  const std::string text = slurp("tests/fixtures/gqrngregion.tmir.sexp");
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL gqrngregion did not compile completely: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }
  expect_eq("gqrngregion columns", joined(cm.write_array->columns),
            "sigma,e,n,d.1,d.2,d.3,mn.1,mn.2,mn.3,i");

  const double sigma = 1.4;
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  wex.params_data()[0] = std::log(sigma);  // sigma's unconstrained form
  WaRng graph_rng(11);
  wex.run_forward_only(EvalState{&graph_rng});
  std::vector<double> row;
  for (const auto& c : cm.write_array->columns) {
    const double* p = wex.value_ptr(c.slot);
    for (int64_t i = 0; i < c.len; ++i) row.push_back(p[c.storage_index(i)]);
  }

  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> base;
  base["K"] = data.at("K");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp wi(program, std::move(base));
  std::map<std::string, DataMap::Entry> params;
  DataMap::Entry sig;
  sig.r = {sigma};
  params["sigma"] = sig;
  WaRng interp_rng(11);
  const std::vector<double> interp_row = wi.eval(params, interp_rng);
  if (row != interp_row) {
    ++failures;
    std::printf("FAIL gqrngregion: graph and interpreter rows differ\n");
  }
  if (row.size() != 10) {
    ++failures;
    std::printf("FAIL gqrngregion: row size %zu\n", row.size());
    return;
  }

  WaRng ref(11);
  auto& g = ref.gen();
  const double want_e = stan::math::exponential_rng(sigma, g);
  const double want_n = stan::math::normal_rng(0.5, sigma, g);
  const Eigen::VectorXd want_d =
      stan::math::dirichlet_rng(Eigen::VectorXd::Constant(3, 1.5), g);
  const Eigen::VectorXd want_mn = stan::math::multi_normal_rng(
      Eigen::VectorXd::Constant(3, 0.25),
      Eigen::MatrixXd(Eigen::VectorXd::Constant(3, sigma).asDiagonal()), g);

  auto expect_val = [&](const char* what, double got, double want) {
    if (got != want) {
      ++failures;
      std::printf("FAIL gqrngregion %s: got %.17g want %.17g\n", what, got,
                  want);
    }
  };
  expect_val("sigma passthrough", row[0], sigma);
  expect_val("exponential draw", row[1], want_e);
  expect_val("normal draw", row[2], want_n);
  for (int i = 0; i < 3; ++i) {
    expect_val("dirichlet draw", row[(size_t)(3 + i)], want_d[i]);
    expect_val("multi_normal draw", row[(size_t)(6 + i)], want_mn[i]);
  }
  // The loop counter is a generated quantity like any other, and the region
  // has to carry its final value out rather than the initializer.
  expect_val("loop counter", row[9], 2.0);
}

// The same, with a region whose inputs are all data -- exactly what constant
// folding looks for. Folding an effect would run it once while compiling and
// hand every later evaluation the same stored number, so check end to end
// that two draws stay two draws and two rows stay two rows.
void test_region_rng_is_not_folded() {
  using namespace stanli;
  DataMap data = DataMap::from_json_file("tests/fixtures/gqrngfold.json");
  const std::string text = slurp("tests/fixtures/gqrngfold.tmir.sexp");
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL gqrngfold did not compile completely: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  WaRng rng(5);
  const auto draw_row = [&]() {
    wex.run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& c : cm.write_array->columns) {
      const double* p = wex.value_ptr(c.slot);
      for (int64_t i = 0; i < c.len; ++i) row.push_back(p[c.storage_index(i)]);
    }
    return row;
  };
  const std::vector<double> first = draw_row();
  const std::vector<double> second = draw_row();

  WaRng ref(5);
  auto& g = ref.gen();
  auto expect_val = [&](const char* what, double got, double want) {
    if (got != want) {
      ++failures;
      std::printf("FAIL gqrngfold %s: got %.17g want %.17g\n", what, got, want);
    }
  };
  for (const std::vector<double>* row : {&first, &second}) {
    expect_val("d1", (*row)[0], stan::math::exponential_rng(2.0, g));
    expect_val("d2", (*row)[1], stan::math::exponential_rng(2.0, g));
  }
  // Stated directly as well as through stan-math: a folded region would
  // repeat one stored value in both columns of both rows.
  if (first[0] == first[1] || first == second) {
    ++failures;
    std::printf("FAIL gqrngfold: the region's draws were folded away\n");
  }
}

// neg_binomial_2_lpmf and multi_normal_lpdf as value-returning densities in
// a runtime-control region: no graph opcode, so the whole section runs on
// WaInterp. Check the accumulated values against stan-math directly.
void test_interpreted_gq_densities() {
  using namespace stanli;
  DataMap data = DataMap::from_json_file("tests/fixtures/gqdensity.json");
  const std::string text = slurp("tests/fixtures/gqdensity.tmir.sexp");
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || !cm.write_array->interp) {
    ++failures;
    std::printf("FAIL gqdensity: expected an attached interpreter\n");
    return;
  }
  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> base;
  for (const char* key : {"K", "counts", "y", "Sigma"})
    base[key] = data.at(key);
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp wi(program, std::move(base));
  std::map<std::string, DataMap::Entry> params;
  DataMap::Entry mu;
  mu.dims = {2};
  mu.r = {0.4, -0.7};
  params["mu"] = mu;
  WaRng rng(11);
  const std::vector<double> row = wi.eval(params, rng);
  expect_eq("gqdensity header", joined(wi.columns()), "mu.1,mu.2,nb,mvn,i");

  const std::vector<int> counts = {3, 0};
  double want_nb = 0.0;
  for (int k = 0; k < 2; ++k)
    want_nb += stan::math::neg_binomial_2_lpmf(counts[k], 2.0, 1.5);
  Eigen::VectorXd y(2), m(2);
  y << 0.5, -1.0;
  m << 0.4, -0.7;
  Eigen::MatrixXd S(2, 2);
  S << 2.0, 0.3, 0.3, 1.0;
  const double want_mvn = 2.0 * stan::math::multi_normal_lpdf(y, m, S);

  auto expect_close = [&](const char* what, double got, double want) {
    if (std::abs(got - want) > 1e-12) {
      ++failures;
      std::printf("FAIL gqdensity %s: got %.17g want %.17g\n", what, got, want);
    }
  };
  expect_close("neg_binomial_2 accumulation", row[2], want_nb);
  expect_close("multi_normal accumulation", row[3], want_mvn);
}

// The interpreter's own closed forms for the GP covariances, over
// array[N] vector[D] coordinates.
void test_interpreted_gq_gp_covariances() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gpmatern.tmir.sexp");
  auto program =
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
  WaInterp wi(program, std::move(base));

  std::vector<Eigen::VectorXd> pts(3, Eigen::VectorXd(2));
  pts[0] << 0.3, -0.7;
  pts[1] << 1.1, -0.2;
  pts[2] << 0.4, 0.9;
  const double alpha = 0.85, rho = 1.3;
  std::map<std::string, DataMap::Entry> params;
  DataMap::Entry x;
  x.dims = {3, 2};
  for (int d = 0; d < 2; ++d)
    for (int n = 0; n < 3; ++n) x.r.push_back(pts[(size_t)n](d));
  params["x"] = x;
  params["alpha"].r = {alpha};
  params["rho"].r = {rho};

  WaRng rng(7);
  const std::vector<double> row = wi.eval(params, rng);
  expect_idx("gpmatern interpreted row width", row.size(), 44);
  if (row.size() != 44) return;

  const Eigen::MatrixXd want[4] = {
      stan::math::gp_exp_quad_cov(pts, alpha, rho),
      stan::math::gp_matern32_cov(pts, alpha, rho),
      stan::math::gp_matern52_cov(pts, alpha, rho),
      stan::math::gp_exponential_cov(pts, alpha, rho)};
  const char* labels[4] = {"Kq", "K32", "K52", "Kexp"};
  for (int k = 0; k < 4; ++k)
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) {
        const double got = row[(size_t)(8 + 9 * k + 3 * j + i)];
        const double expected = want[k](i, j);
        if (std::abs(got - expected) >
            1e-13 * std::max(1.0, std::abs(expected))) {
          ++failures;
          std::printf("FAIL gpmatern %s(%d,%d): got %.17g want %.17g\n",
                      labels[k], i + 1, j + 1, got, expected);
        }
      }
}

// multiply_lower_tri_self_transpose in a runtime-control region. The register
// machine carries it now, so the whole section compiles; the interpreter runs
// beside it here because both still have to agree. The function zeros A's
// upper triangle before forming L L', so a full A * A' would disagree on
// every entry that touches a dropped element.
void test_compiled_multiply_lower_tri() {
  using namespace stanli;
  DataMap data;
  data.set_int("M", 3);
  const std::string text = slurp("tests/fixtures/mlt.tmir.sexp");
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL mlt did not compile completely: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }
  // Column-major A with a non-zero upper triangle that must be ignored.
  const double a[9] = {1.0, 0.4, -0.2, 5.0, 2.0, 0.7, 9.0, 8.0, 3.0};

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  std::copy(a, a + 9, wex.params_data());
  WaRng graph_rng(3);
  wex.run_forward_only(EvalState{&graph_rng});
  std::vector<double> row;
  for (const auto& c : cm.write_array->columns) {
    const double* p = wex.value_ptr(c.slot);
    for (int64_t i = 0; i < c.len; ++i) row.push_back(p[c.storage_index(i)]);
  }

  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> base;
  base["M"] = data.at("M");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp wi(program, std::move(base));
  std::map<std::string, DataMap::Entry> params;
  DataMap::Entry A;
  A.dims = {3, 3};
  A.r.assign(a, a + 9);
  params["A"] = A;
  WaRng interp_rng(3);
  const std::vector<double> interp_row = wi.eval(params, interp_rng);
  if (row != interp_row) {
    ++failures;
    std::printf("FAIL mlt: graph and interpreter rows differ\n");
  }

  Eigen::MatrixXd Am(3, 3);
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) Am(r, c) = a[c * 3 + r];
  const Eigen::MatrixXd want =
      stan::math::multiply_lower_tri_self_transpose(Am);
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) {
      const double got = row.at((size_t)(9 + c * 3 + r));
      if (got != want(r, c)) {
        ++failures;
        std::printf("FAIL mlt P(%d,%d): got %.17g want %.17g\n", r, c, got,
                    want(r, c));
      }
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
  auto program = std::make_shared<mir::Program>(
      mir::read_program(sexp::parse(slurp("tests/fixtures/gqrng.tmir.sexp"))));
  std::map<std::string, DataMap::Entry> base;
  base["N"] = data.at("N");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp wi(program, std::move(base));
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

// WaRng is the BridgeStan-compatible public stream: it must track Stan's
// current rng_t and its create_rng(seed, chain=0) seeding convention. This
// catches the historical drift where sampling moved to mixmax but generated
// quantities silently stayed on ecuyer1988.
void test_stan_rng_stream_contract() {
  using namespace stanli;
  constexpr unsigned seed = 1234;
  for (const unsigned chain : {0u, 1u, 17u}) {
    WaRng got(seed, chain);
    stan::rng_t want = stan::services::util::create_rng(seed, chain);
    for (int i = 0; i < 32; ++i) {
      if (got.gen()() != want()) {
        ++failures;
        std::printf(
            "FAIL WaRng does not match Stan rng_t at chain %u draw %d\n", chain,
            i);
        return;
      }
    }
    got.seed(seed, chain);
    want = stan::services::util::create_rng(seed, chain);
    if (got.gen()() != want()) {
      ++failures;
      std::printf(
          "FAIL WaRng reseed does not match Stan create_rng at chain %u\n",
          chain);
    }
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
  data.set_real_array("rectangular", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}, {3, 2});
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
  expect_eq("gqconst header", joined(wi.columns()),
            "x,z,extracted.1,extracted.2");
  if (row.size() != 4 || row[0] != 0.25 || row[1] != 3.0 || row[2] != 1.0 ||
      row[3] != 5.0) {
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

struct RngException {
  int kind = 0;
  std::string message;
};

template <typename F>
RngException capture_rng_exception(F&& f) {
  try {
    f();
  } catch (const std::invalid_argument& e) {
    return {1, e.what()};
  } catch (const std::domain_error& e) {
    return {2, e.what()};
  } catch (const std::logic_error& e) {
    return {3, e.what()};
  } catch (const std::exception& e) {
    return {4, e.what()};
  }
  return {};
}

static Eigen::VectorXd categorical_theta(const std::vector<double>& p) {
  Eigen::VectorXd theta(static_cast<Eigen::Index>(p.size()));
  for (size_t i = 0; i < p.size(); ++i)
    theta[static_cast<Eigen::Index>(i)] = p[i];
  return theta;
}

static Eigen::VectorXd multi_normal_location(const std::vector<double>& x) {
  Eigen::VectorXd out(static_cast<Eigen::Index>(x.size()));
  for (size_t i = 0; i < x.size(); ++i)
    out[static_cast<Eigen::Index>(i)] = x[i];
  return out;
}

static Eigen::MatrixXd multi_normal_covariance(const std::vector<double>& x,
                                               size_t rows, size_t cols) {
  Eigen::MatrixXd out(static_cast<Eigen::Index>(rows),
                      static_cast<Eigen::Index>(cols));
  for (size_t j = 0; j < cols; ++j)
    for (size_t i = 0; i < rows; ++i)
      out(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) =
          x.at(j * rows + i);
  return out;
}

static std::vector<double> direct_multi_normal_rng(
    const std::vector<double>& mu, const std::vector<double>& covariance,
    size_t rows, size_t cols, stanli::WaRng& rng) {
  const Eigen::VectorXd draw = stan::math::multi_normal_rng(
      multi_normal_location(mu),
      multi_normal_covariance(covariance, rows, cols), rng.gen());
  return std::vector<double>(draw.data(), draw.data() + draw.size());
}

static bool same_double_bytes(const std::vector<double>& a,
                              const std::vector<double>& b) {
  return a.size() == b.size() &&
         (a.empty() ||
          std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

// categorical_rng is unusual in this opcode family: one logical argument is
// a variable-length vector, while the result is still one Stan int. Keep the
// shared helper byte-for-byte on Stan Math's validation and engine schedule.
void test_categorical_rng_helper_contract() {
  using namespace stanli;
  const std::vector<std::vector<double>> valid = {
      {1.0},
      {0.0, 1.0, 0.0},
      {-0.0, 0.0, 1.0},
      {0.125, 0.25, 0.375, 0.25},
      {0.5, 0.5, 0.0},
      {1.0 + 5e-9},  // inside Stan Math's simplex tolerance
  };
  for (size_t c = 0; c < valid.size(); ++c) {
    for (unsigned seed : {1u, 17u, 2026u}) {
      WaRng got_rng(seed + static_cast<unsigned>(c));
      WaRng want_rng(seed + static_cast<unsigned>(c));
      const int got =
          categorical_rng_draw(valid[c].data(), valid[c].size(), got_rng);
      const int want = stan::math::categorical_rng(categorical_theta(valid[c]),
                                                   want_rng.gen());
      const auto got_next = got_rng.gen()();
      const auto want_next = want_rng.gen()();
      if (got != want || got_next != want_next) {
        ++failures;
        std::printf("FAIL categorical helper valid case %zu seed %u\n", c,
                    seed);
      }
    }
  }

  // Sum validation precedes the element scan in check_simplex. The two
  // negative cases distinguish those paths; empty also pins invalid_argument
  // rather than domain_error. No rejected input may advance the stream.
  const std::vector<std::vector<double>> invalid = {
      {},
      {0.0},
      {0.2, 0.2},
      {1.0, -0.25, 0.25},
      {1.0, -0.25, 0.5},
      {std::numeric_limits<double>::quiet_NaN()},
      {std::numeric_limits<double>::infinity()},
  };
  for (size_t c = 0; c < invalid.size(); ++c) {
    WaRng got_rng(static_cast<unsigned>(401 + c));
    WaRng want_rng(static_cast<unsigned>(401 + c));
    const RngException got = capture_rng_exception([&] {
      (void)categorical_rng_draw(
          invalid[c].empty() ? nullptr : invalid[c].data(), invalid[c].size(),
          got_rng);
    });
    const RngException want = capture_rng_exception([&] {
      (void)stan::math::categorical_rng(categorical_theta(invalid[c]),
                                        want_rng.gen());
    });
    const auto got_next = got_rng.gen()();
    const auto want_next = want_rng.gen()();
    if (got.kind == 0 || got.kind != want.kind || got.message != want.message ||
        got_next != want_next) {
      ++failures;
      std::printf("FAIL categorical helper invalid case %zu\n", c);
    }
  }

  // This is the graph/helper ABI guard, not a Stan language input: a nonempty
  // descriptor must carry storage. It must fail before touching the engine.
  WaRng malformed(991), untouched(991);
  const RngException null_input = capture_rng_exception(
      [&] { (void)categorical_rng_draw(nullptr, 1, malformed); });
  if (null_input.kind != 3 || malformed.gen()() != untouched.gen()()) {
    ++failures;
    std::printf("FAIL categorical helper malformed pointer contract\n");
  }
}

void test_multi_normal_rng_helper_contract() {
  using namespace stanli;
  struct Valid {
    std::vector<double> mu;
    std::vector<double> covariance;
  };
  const Valid valid[] = {
      {{-0.25}, {1.75}},
      {{0.5, -1.25}, {1.69, 0.364, 0.364, 0.64}},
      {{0.25, -0.5, 1.75}, {2.0, 0.25, -0.4, 0.25, 1.5, 0.3, -0.4, 0.3, 1.25}},
  };
  for (size_t c = 0; c < sizeof(valid) / sizeof(valid[0]); ++c) {
    for (unsigned seed : {1u, 29u, 2026u}) {
      const size_t k = valid[c].mu.size();
      std::vector<double> got(k);
      WaRng got_rng(seed + static_cast<unsigned>(c));
      WaRng want_rng(seed + static_cast<unsigned>(c));
      multi_normal_rng_draw(valid[c].mu.data(), k, valid[c].covariance.data(),
                            valid[c].covariance.size(), k, k, got.data(),
                            got.size(), got_rng);
      const std::vector<double> want = direct_multi_normal_rng(
          valid[c].mu, valid[c].covariance, k, k, want_rng);
      if (!same_double_bytes(got, want) ||
          got_rng.gen()() != want_rng.gen()()) {
        ++failures;
        std::printf("FAIL multi-normal helper valid case %zu seed %u\n", c,
                    seed);
      }
    }
  }

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  struct Invalid {
    std::vector<double> mu;
    std::vector<double> covariance;
    size_t rows;
    size_t cols;
    const char* label;
  };
  // The asymmetric case uses four distinct column-major positions. Its Stan
  // message names both off-diagonal values, catching a transposed rebuild.
  const Invalid invalid[] = {
      {{nan, 0.0}, {1.0, 0.0, 0.0, 1.0}, 2, 2, "NaN mean"},
      {{inf, 0.0}, {1.0, 0.0, 0.0, 1.0}, 2, 2, "infinite mean"},
      {{0.0, 0.0}, {1.0, 0.0, nan, 1.0}, 2, 2, "NaN covariance"},
      {{0.0, 0.0}, {1.25, 0.75, 0.125, 0.8}, 2, 2, "asymmetric covariance"},
      {{0.0, 0.0}, {1.0, 2.0, 2.0, 1.0}, 2, 2, "non-PD covariance"},
      {{0.0, 0.0}, {1.0, 0.0, 0.0, 1.0}, 1, 4, "dimension mismatch"},
      {{}, {}, 0, 0, "empty covariance"},
  };
  for (size_t c = 0; c < sizeof(invalid) / sizeof(invalid[0]); ++c) {
    std::vector<double> got(invalid[c].mu.size());
    WaRng got_rng(static_cast<unsigned>(501 + c));
    WaRng want_rng(static_cast<unsigned>(501 + c));
    const RngException got_error = capture_rng_exception([&] {
      multi_normal_rng_draw(invalid[c].mu.data(), invalid[c].mu.size(),
                            invalid[c].covariance.data(),
                            invalid[c].covariance.size(), invalid[c].rows,
                            invalid[c].cols, got.data(), got.size(), got_rng);
    });
    const RngException want_error = capture_rng_exception([&] {
      (void)direct_multi_normal_rng(invalid[c].mu, invalid[c].covariance,
                                    invalid[c].rows, invalid[c].cols, want_rng);
    });
    if (got_error.kind == 0 || got_error.kind != want_error.kind ||
        got_error.message != want_error.message ||
        got_rng.gen()() != want_rng.gen()()) {
      ++failures;
      std::printf("FAIL multi-normal helper %s contract\n", invalid[c].label);
    }
  }

  const std::vector<double> mu{0.5, -1.25};
  const std::vector<double> covariance{1.69, 0.364, 0.364, 0.64};
  std::vector<double> output(2);
  WaRng malformed(991), untouched(991);
  const RngException mismatch = capture_rng_exception([&] {
    multi_normal_rng_draw(mu.data(), mu.size(), covariance.data(),
                          covariance.size(), 1, 3, output.data(), output.size(),
                          malformed);
  });
  const RngException null_location = capture_rng_exception([&] {
    multi_normal_rng_draw(nullptr, mu.size(), covariance.data(),
                          covariance.size(), 2, 2, output.data(), output.size(),
                          malformed);
  });
  if (mismatch.kind != 3 || null_location.kind != 3 ||
      malformed.gen()() != untouched.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal malformed helper contract\n");
  }

  // A rejected helper call is reusable at the exact untouched draw.
  WaRng recovered(811), direct(811);
  std::vector<double> recovered_draw(2);
  (void)capture_rng_exception([&] {
    const std::vector<double> bad{1.0, 2.0, 2.0, 1.0};
    multi_normal_rng_draw(mu.data(), 2, bad.data(), 4, 2, 2,
                          recovered_draw.data(), 2, recovered);
  });
  multi_normal_rng_draw(mu.data(), 2, covariance.data(), 4, 2, 2,
                        recovered_draw.data(), 2, recovered);
  const std::vector<double> direct_draw =
      direct_multi_normal_rng(mu, covariance, 2, 2, direct);
  if (recovered_draw != direct_draw || recovered.gen()() != direct.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal helper rejected-call reuse\n");
  }
}

void test_categorical_rng_lowering_guards() {
  using namespace stanli;
  const std::string base = slurp("tests/fixtures/gq_categorical_rng.tmir.sexp");
  const size_t call = base.find("(FunApp (StanLib categorical_rng");
  const std::string vector_arg =
      "((pattern (Var p)) (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const size_t argument = base.find(vector_arg, call);
  if (call == std::string::npos || argument == std::string::npos) {
    ++failures;
    std::printf("FAIL categorical lowering guard fixture has no call\n");
    return;
  }

  const auto expect_interp = [](const std::string& mir,
                                const std::string& reason, const char* what) {
    DataMap data;
    data.set_int("K", 4);
    try {
      CompiledModel cm = compile_model(mir, data);
      if (!cm.write_array || !cm.write_array->interp ||
          cm.write_array->truncated.find(reason) == std::string::npos) {
        ++failures;
        std::printf("FAIL %s: got %s\n", what,
                    cm.write_array ? cm.write_array->truncated.c_str()
                                   : "no write_array");
      }
    } catch (const std::exception& e) {
      ++failures;
      std::printf("FAIL %s: mutation did not parse/compile: %s\n", what,
                  e.what());
    }
  };

  struct BadArgument {
    const char* type;
    const char* label;
  };
  const BadArgument bad_arguments[] = {
      {"UReal", "scalar"},
      {"URowVector", "row-vector"},
      {"(UArray UReal)", "array"},
      {"(UArray UVector)", "array-vector"},
  };
  for (const BadArgument& bad : bad_arguments) {
    std::string mutated = base;
    const std::string replacement =
        std::string("((pattern (Var p)) (meta ((type_ ") + bad.type +
        ") (loc <opaque>) (adlevel DataOnly))))";
    mutated.replace(argument, vector_arg.size(), replacement);
    const std::string label =
        std::string("categorical ") + bad.label + " input stays interpreted";
    expect_interp(mutated, "probability-vector argument", label.c_str());
  }

  std::string no_args = base;
  no_args.erase(argument, vector_arg.size());
  expect_interp(no_args, "expected one scalar int result",
                "zero-argument categorical stays interpreted");

  std::string two_args = base;
  two_args.insert(argument + vector_arg.size(), " " + vector_arg);
  expect_interp(two_args, "expected one scalar int result",
                "two-argument categorical stays interpreted");

  std::string wrong_result = base;
  const std::string result_type = "(meta ((type_ UInt)";
  const size_t result = wrong_result.find(result_type, call);
  if (result == std::string::npos) {
    ++failures;
    std::printf("FAIL categorical lowering guard cannot find result type\n");
  } else {
    wrong_result.replace(result, result_type.size(), "(meta ((type_ UReal)");
    expect_interp(wrong_result, "expected one scalar int result",
                  "categorical real result stays interpreted");

    std::string container_result = base;
    container_result.replace(result, result_type.size(),
                             "(meta ((type_ (UArray UInt))");
    expect_interp(container_result, "expected one scalar int result",
                  "categorical container result stays interpreted");
  }

  std::string logit = base;
  logit.replace(call + std::string("(FunApp (StanLib ").size(),
                std::string("categorical_rng").size(), "categorical_logit_rng");
  DataMap logit_data;
  logit_data.set_int("K", 4);
  CompiledModel logit_cm = compile_model(logit, logit_data);
  if (!logit_cm.write_array || logit_cm.write_array->interp ||
      !logit_cm.write_array->truncated.empty()) {
    ++failures;
    std::printf("FAIL categorical_logit_rng did not lower natively: %s\n",
                logit_cm.write_array ? logit_cm.write_array->truncated.c_str()
                                     : "no write_array");
    return;
  }

  Executor logit_graph(std::move(logit_cm.write_array->graph));
  logit_cm.write_array->bind(logit_graph);
  const std::vector<double> theta{-1.0, 0.5, 2.0, 0.0};
  for (size_t i = 0; i < theta.size(); ++i)
    logit_graph.params_data()[i] = theta[i];

  WaRng graph_rng(17), ref_rng(17);
  logit_graph.run_forward_only(EvalState{&graph_rng});
  const CompiledModel::ParamView* draw_column = nullptr;
  for (const auto& column : logit_cm.write_array->columns)
    if (column.name == "draw") draw_column = &column;
  const int want = stan::math::categorical_rng(
      stan::math::softmax(categorical_theta(theta)), ref_rng.gen());
  if (draw_column == nullptr ||
      logit_graph.value_ptr(draw_column->slot)[draw_column->storage_index(0)] !=
          want) {
    ++failures;
    std::printf("FAIL categorical_logit_rng draw mismatch vs softmax\n");
  }
}

static stanli::DataMap categorical_data(int k) {
  stanli::DataMap data;
  data.set_int("K", k);
  return data;
}

static std::map<std::string, stanli::DataMap::Entry> categorical_base(
    const stanli::DataMap& data) {
  std::map<std::string, stanli::DataMap::Entry> base;
  base["K"] = data.at("K");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    stanli::DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  return base;
}

static stanli::DataMap::Entry categorical_parameter(
    const std::vector<double>& probabilities) {
  stanli::DataMap::Entry p;
  p.r = probabilities;
  p.dims = {static_cast<int64_t>(probabilities.size())};
  return p;
}

static std::vector<double> categorical_direct_row(
    const std::vector<double>& probabilities, stanli::WaRng& rng,
    bool with_tail) {
  std::vector<double> row = probabilities;
  row.push_back(static_cast<double>(stan::math::categorical_rng(
      categorical_theta(probabilities), rng.gen())));
  if (with_tail) row.push_back(stan::math::uniform_rng(0.0, 1.0, rng.gen()));
  return row;
}

void test_compiled_categorical_rng() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gq_categorical_rng.tmir.sexp");
  const DataMap data = categorical_data(4);
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL categorical rng: write_array did not compile completely: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }

  int categorical_ops = 0, uniform_ops = 0, other_rng_ops = 0;
  for (const Op& op : cm.write_array->graph.ops) {
    if (op.opcode != OP_RNG) continue;
    if (op.variant == kCategoricalRngVariant) {
      ++categorical_ops;
      if (op.n_in != 1 || cm.write_array->graph.slots[op.in[0]].len != 4 ||
          cm.write_array->graph.slots[op.out].len != 1) {
        ++failures;
        std::printf("FAIL categorical rng: malformed graph descriptor\n");
      }
    } else if (op.variant == static_cast<uint8_t>(ScalarRng::Uniform)) {
      ++uniform_ops;
    } else {
      ++other_rng_ops;
    }
  }
  if (categorical_ops != 1 || uniform_ops != 1 || other_rng_ops != 0) {
    ++failures;
    std::printf(
        "FAIL categorical rng op census: categorical=%d uniform=%d "
        "other=%d\n",
        categorical_ops, uniform_ops, other_rng_ops);
  }
  expect_eq("categorical rng columns", joined(cm.write_array->columns),
            "p.1,p.2,p.3,p.4,draw,tail");

  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  const auto set_graph_probabilities = [&](const std::vector<double>& p) {
    for (size_t i = 0; i < p.size(); ++i) graph.params_data()[i] = p[i];
  };
  const auto graph_row = [&](WaRng& rng) {
    graph.run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& column : cm.write_array->columns) {
      const double* values = graph.value_ptr(column.slot);
      for (int64_t i = 0; i < column.len; ++i)
        row.push_back(values[column.storage_index(i)]);
    }
    return row;
  };

  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  WaInterp interpreted(program, categorical_base(data));
  std::map<std::string, DataMap::Entry> params;
  const std::vector<double> valid{0.125, 0.25, 0.375, 0.25};
  set_graph_probabilities(valid);
  params["p"] = categorical_parameter(valid);

  // Feed both modes the same already-materialized probability vector. Survey
  // computes that vector through softmax, whose graph/interpreter last bits
  // can differ independently of RNG correctness; that mode boundary is not
  // a categorical_rng oracle.
  WaRng graph_rng(42), interp_rng(42), direct_rng(42);
  const std::vector<double> graph_first = graph_row(graph_rng);
  const std::vector<double> interp_first = interpreted.eval(params, interp_rng);
  const std::vector<double> direct_first =
      categorical_direct_row(valid, direct_rng, true);
  const std::vector<double> graph_second = graph_row(graph_rng);
  const std::vector<double> interp_second =
      interpreted.eval(params, interp_rng);
  const std::vector<double> direct_second =
      categorical_direct_row(valid, direct_rng, true);
  const auto graph_next = graph_rng.gen()();
  const auto interp_next = interp_rng.gen()();
  const auto direct_next = direct_rng.gen()();
  if (graph_first != interp_first || graph_first != direct_first ||
      graph_second != interp_second || graph_second != direct_second ||
      graph_next != interp_next || graph_next != direct_next) {
    ++failures;
    std::printf("FAIL categorical rng sequential row/stream parity\n");
  }
  graph_rng.seed(42);
  if (graph_row(graph_rng) != graph_first) {
    ++failures;
    std::printf("FAIL categorical rng reseed did not reproduce first row\n");
  }

  // The invalid call must throw before consuming a uniform. Reusing the same
  // executor and all three streams must therefore recover at the exact draw
  // direct Stan Math would have produced from the original seed.
  const std::vector<double> invalid{1.0, -0.25, 0.25, 0.0};
  set_graph_probabilities(invalid);
  params["p"] = categorical_parameter(invalid);
  WaRng graph_failing(211), interp_failing(211), direct_failing(211);
  const RngException graph_error =
      capture_rng_exception([&] { (void)graph_row(graph_failing); });
  const RngException interp_error = capture_rng_exception(
      [&] { (void)interpreted.eval(params, interp_failing); });
  const RngException direct_error = capture_rng_exception([&] {
    (void)stan::math::categorical_rng(categorical_theta(invalid),
                                      direct_failing.gen());
  });
  if (graph_error.kind == 0 || graph_error.kind != interp_error.kind ||
      graph_error.kind != direct_error.kind ||
      graph_error.message != interp_error.message ||
      graph_error.message != direct_error.message) {
    ++failures;
    std::printf("FAIL categorical rng invalid exception parity\n");
  }

  set_graph_probabilities(valid);
  params["p"] = categorical_parameter(valid);
  const RngException missing_state =
      capture_rng_exception([&] { graph.run_forward_only(); });
  const std::vector<double> graph_recovered = graph_row(graph_failing);
  const std::vector<double> interp_recovered =
      interpreted.eval(params, interp_failing);
  const std::vector<double> direct_recovered =
      categorical_direct_row(valid, direct_failing, true);
  const auto graph_recovered_next = graph_failing.gen()();
  const auto interp_recovered_next = interp_failing.gen()();
  const auto direct_recovered_next = direct_failing.gen()();
  if (missing_state.kind != 3 ||
      missing_state.message.find("caller-owned") == std::string::npos ||
      graph_recovered != interp_recovered ||
      graph_recovered != direct_recovered ||
      graph_recovered_next != interp_recovered_next ||
      graph_recovered_next != direct_recovered_next) {
    ++failures;
    std::printf("FAIL categorical rng exception/reuse contract\n");
  }
}

void test_categorical_rng_empty_vector() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gq_categorical_rng.tmir.sexp");
  const DataMap data = categorical_data(0);
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL empty categorical vector did not compile: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }

  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  WaInterp interpreted(program, categorical_base(data));
  std::map<std::string, DataMap::Entry> params;
  params["p"] = categorical_parameter({});

  WaRng graph_rng(71), interp_rng(71), direct_rng(71);
  const RngException graph_error = capture_rng_exception(
      [&] { graph.run_forward_only(EvalState{&graph_rng}); });
  const RngException interp_error = capture_rng_exception(
      [&] { (void)interpreted.eval(params, interp_rng); });
  const RngException direct_error = capture_rng_exception([&] {
    (void)stan::math::categorical_rng(categorical_theta({}), direct_rng.gen());
  });
  const auto graph_next = graph_rng.gen()();
  const auto interp_next = interp_rng.gen()();
  const auto direct_next = direct_rng.gen()();
  if (graph_error.kind != 1 || graph_error.kind != interp_error.kind ||
      graph_error.kind != direct_error.kind ||
      graph_error.message != interp_error.message ||
      graph_error.message != direct_error.message ||
      graph_next != interp_next || graph_next != direct_next) {
    ++failures;
    std::printf("FAIL empty categorical validation/stream parity\n");
  }
}

void test_categorical_rng_dynamic_index() {
  using namespace stanli;
  const std::string text =
      slurp("tests/fixtures/gq_categorical_dynamic.tmir.sexp");
  const DataMap data = categorical_data(3);
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL categorical dynamic index lowering: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }

  int categorical_ops = 0, dynamic_slices = 0;
  for (const Op& op : cm.write_array->graph.ops) {
    dynamic_slices += op.opcode == OP_DYNAMIC_SLICE;
    if (op.opcode != OP_RNG || op.variant != kCategoricalRngVariant) continue;
    ++categorical_ops;
    if (op.n_in != 1 || cm.write_array->graph.slots[op.in[0]].len != 3 ||
        cm.write_array->graph.slots[op.out].len != 1) {
      ++failures;
      std::printf("FAIL categorical dynamic prefix descriptor\n");
    }
  }
  if (categorical_ops != 1 || dynamic_slices != 1) {
    ++failures;
    std::printf(
        "FAIL categorical dynamic graph has %d categorical ops and %d "
        "dynamic slices\n",
        categorical_ops, dynamic_slices);
  }

  const std::vector<double> probabilities{0.25, 0.5, 0.25};
  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  std::copy(probabilities.begin(), probabilities.end(), graph.params_data());
  WaRng graph_rng(303), direct_rng(303);
  graph.run_forward_only(EvalState{&graph_rng});
  std::vector<double> got;
  for (const auto& column : cm.write_array->columns) {
    const double* values = graph.value_ptr(column.slot);
    for (int64_t i = 0; i < column.len; ++i)
      got.push_back(values[column.storage_index(i)]);
  }
  std::vector<double> want = probabilities;
  const int draw = stan::math::categorical_rng(categorical_theta(probabilities),
                                               direct_rng.gen());
  want.push_back(static_cast<double>(draw));
  want.push_back(probabilities[static_cast<size_t>(draw - 1)]);
  const auto graph_next = graph_rng.gen()();
  const auto direct_next = direct_rng.gen()();
  expect_eq("categorical dynamic columns", joined(cm.write_array->columns),
            "p.1,p.2,p.3,draw,picked");
  if (got != want || graph_next != direct_next) {
    ++failures;
    std::printf("FAIL categorical dynamic graph row/stream\n");
  }
}

static std::map<std::string, stanli::DataMap::Entry> multi_normal_base() {
  std::map<std::string, stanli::DataMap::Entry> base;
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    stanli::DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  return base;
}

static stanli::DataMap::Entry real_entry(double value) {
  stanli::DataMap::Entry out;
  out.r = {value};
  return out;
}

static stanli::DataMap::Entry vector_entry(std::vector<double> values) {
  stanli::DataMap::Entry out;
  out.dims = {static_cast<int64_t>(values.size())};
  out.r = std::move(values);
  return out;
}

static std::vector<double> multi_normal_direct_row(
    const std::vector<double>& mu, const std::vector<double>& sigma, double rho,
    stanli::WaRng& rng) {
  const double cross = sigma[0] * sigma[1] * rho;
  const std::vector<double> covariance = {sigma[0] * sigma[0], cross, cross,
                                          sigma[1] * sigma[1]};
  const std::vector<double> draw =
      direct_multi_normal_rng(mu, covariance, 2, 2, rng);
  return {mu[0],
          mu[1],
          sigma[0],
          sigma[1],
          rho,
          draw[0],
          draw[1],
          draw[0] - draw[1],
          stan::math::uniform_rng(0.0, 1.0, rng.gen())};
}

void test_multi_normal_rng_kernel_contract() {
  using namespace stanli;
  const Kernel* kernel = find_kernel(OP_RNG);
  if (kernel == nullptr || kernel->forward == nullptr) {
    ++failures;
    std::printf("FAIL multi-normal RNG kernel is not registered\n");
    return;
  }
  double mu[] = {0.5, -1.25};
  double covariance[] = {1.69, 0.364, 0.364, 0.64};
  double output[] = {-99.0, -99.0};
  int k = 2;
  WaRng rng(123), direct_rng(123);
  EvalState state{&rng};
  KernelCtx valid;
  valid.variant = kMultiNormalRngVariant;
  valid.n_in = 2;
  valid.in[0] = Desc{mu, 2};
  valid.in[1] = Desc{covariance, 4};
  valid.out = Desc{output, 2};
  valid.idata = &k;
  valid.n_idata = 1;
  valid.eval_state = &state;
  kernel->forward(valid);
  const std::vector<double> want = direct_multi_normal_rng(
      {mu[0], mu[1]}, {1.69, 0.364, 0.364, 0.64}, 2, 2, direct_rng);
  if (std::vector<double>(output, output + 2) != want ||
      rng.gen()() != direct_rng.gen()()) {
    ++failures;
    std::printf("FAIL multi-normal RNG valid kernel contract\n");
  }

  struct Mutation {
    const char* label;
    void (*apply)(KernelCtx&);
  };
  const Mutation malformed[] = {
      {"unknown variant", [](KernelCtx& c) { c.variant = 255; }},
      {"arity", [](KernelCtx& c) { c.n_in = 1; }},
      {"missing idata", [](KernelCtx& c) { c.n_idata = 0; }},
      {"null idata", [](KernelCtx& c) { c.idata = nullptr; }},
      {"mean length", [](KernelCtx& c) { c.in[0].len = 1; }},
      {"covariance length", [](KernelCtx& c) { c.in[1].len = 3; }},
      {"output length", [](KernelCtx& c) { c.out.len = 1; }},
      {"missing evaluation state",
       [](KernelCtx& c) { c.eval_state = nullptr; }},
  };
  for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
    WaRng got(static_cast<unsigned>(900 + i));
    WaRng untouched(static_cast<unsigned>(900 + i));
    EvalState got_state{&got};
    KernelCtx ctx = valid;
    ctx.eval_state = &got_state;
    output[0] = output[1] = -99.0;
    malformed[i].apply(ctx);
    const RngException error =
        capture_rng_exception([&] { kernel->forward(ctx); });
    if (error.kind != 3 || got.gen()() != untouched.gen()() ||
        output[0] != -99.0 || output[1] != -99.0) {
      ++failures;
      std::printf("FAIL multi-normal malformed kernel %s\n",
                  malformed[i].label);
    }
  }
}

void test_multi_normal_rng_lowering_guards() {
  using namespace stanli;
  const std::string base =
      slurp("tests/fixtures/gq_multi_normal_rng.tmir.sexp");
  const size_t call = base.find("(FunApp (StanLib multi_normal_rng");
  const std::string mean_arg =
      "((pattern (Var mu)) (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const std::string covariance_arg =
      "((pattern (Var inline_cov_matrix_2d_return_sym1__))\n"
      "           (meta ((type_ UMatrix) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const size_t mean = base.find(mean_arg, call);
  const size_t covariance = base.find(covariance_arg, call);
  if (call == std::string::npos || mean == std::string::npos ||
      covariance == std::string::npos) {
    ++failures;
    std::printf("FAIL multi-normal lowering guard fixture has no call\n");
    return;
  }

  const auto expect_interp = [](const std::string& mir,
                                const std::string& reason, const char* label) {
    try {
      CompiledModel cm = compile_model(mir, DataMap{});
      if (!cm.write_array || !cm.write_array->interp ||
          (!reason.empty() &&
           cm.write_array->truncated.find(reason) == std::string::npos)) {
        ++failures;
        std::printf("FAIL %s: got %s\n", label,
                    cm.write_array ? cm.write_array->truncated.c_str()
                                   : "no write_array");
      }
    } catch (const std::exception& e) {
      ++failures;
      std::printf("FAIL %s: mutation did not parse/compile: %s\n", label,
                  e.what());
    }
  };

  const auto replace_once = [](std::string text, size_t at, size_t count,
                               const std::string& replacement) {
    text.replace(at, count, replacement);
    return text;
  };
  for (const auto& bad : std::vector<std::pair<std::string, const char*>>{
           {"UReal", "scalar"},
           {"URowVector", "row-vector"},
           {"(UArray UVector)", "array-vector"}}) {
    const std::string replacement = "((pattern (Var mu)) (meta ((type_ " +
                                    bad.first +
                                    ") (loc <opaque>) (adlevel DataOnly))))";
    expect_interp(
        replace_once(base, mean, mean_arg.size(), replacement),
        "expected one vector location",
        (std::string("multi-normal ") + bad.second + " mean stays interpreted")
            .c_str());
  }
  for (const auto& bad : std::vector<std::pair<std::string, const char*>>{
           {"UVector", "vector"}, {"(UArray UMatrix)", "array-matrix"}}) {
    std::string replacement = covariance_arg;
    const size_t type = replacement.find("UMatrix");
    replacement.replace(type, std::string("UMatrix").size(), bad.first);
    expect_interp(
        replace_once(base, covariance, covariance_arg.size(), replacement),
        "expected one covariance matrix",
        (std::string("multi-normal ") + bad.second +
         " covariance stays interpreted")
            .c_str());
  }

  std::string one_arg = base;
  one_arg.erase(covariance, covariance_arg.size());
  expect_interp(one_arg, "expected one vector result",
                "one-argument multi-normal stays interpreted");
  std::string zero_args = base;
  zero_args.erase(covariance, covariance_arg.size());
  zero_args.erase(mean, mean_arg.size());
  expect_interp(zero_args, "expected one vector result",
                "zero-argument multi-normal stays interpreted");
  std::string three_args = base;
  three_args.insert(covariance + covariance_arg.size(), " " + mean_arg);
  expect_interp(three_args, "expected one vector result",
                "three-argument multi-normal stays interpreted");

  const size_t mean_type = base.find("(type_ UVector)", call);
  const size_t result_type = base.find("(type_ UVector)", mean_type + 1);
  if (result_type == std::string::npos) {
    ++failures;
    std::printf("FAIL multi-normal lowering guard cannot find result type\n");
  } else {
    std::string row_result = base;
    row_result.replace(result_type, std::string("(type_ UVector)").size(),
                       "(type_ URowVector)");
    expect_interp(row_result, "expected one vector result",
                  "multi-normal row-vector result stays interpreted");
    std::string array_result = base;
    array_result.replace(result_type, std::string("(type_ UVector)").size(),
                         "(type_ (UArray UVector))");
    expect_interp(array_result, "expected one vector result",
                  "multi-normal array result stays interpreted");
  }

  std::string unknown_covariance = base;
  const size_t covariance_var =
      unknown_covariance.find("(Var inline_cov_matrix_2d_return_sym1__)", call);
  unknown_covariance.replace(
      covariance_var,
      std::string("(Var inline_cov_matrix_2d_return_sym1__)").size(),
      "(Var mu)");
  expect_interp(unknown_covariance, "covariance has no known matrix shape",
                "multi-normal unknown covariance shape stays interpreted");

  std::string mismatched_covariance = base;
  const size_t matrix_decl = mismatched_covariance.find(
      "(decl_id inline_cov_matrix_2d_covariance_sym2__)");
  size_t first_extent =
      mismatched_covariance.find("((pattern (Lit Int 2))", matrix_decl);
  size_t second_extent =
      mismatched_covariance.find("((pattern (Lit Int 2))", first_extent + 1);
  if (matrix_decl == std::string::npos || first_extent == std::string::npos ||
      second_extent == std::string::npos) {
    ++failures;
    std::printf("FAIL multi-normal cannot locate covariance extents\n");
  } else {
    first_extent = mismatched_covariance.find("Int 2", first_extent) + 4;
    second_extent = mismatched_covariance.find("Int 2", second_extent) + 4;
    mismatched_covariance[first_extent] = '3';
    mismatched_covariance[second_extent] = '3';
    expect_interp(mismatched_covariance,
                  "covariance shape must match the location",
                  "multi-normal known dimension mismatch stays interpreted");
  }

  std::string mismatched_result = base;
  const size_t draw_decl = mismatched_result.find("(decl_id draw)");
  size_t draw_extent =
      mismatched_result.find("((pattern (Lit Int 2))", draw_decl);
  if (draw_decl == std::string::npos || draw_extent == std::string::npos) {
    ++failures;
    std::printf("FAIL multi-normal cannot locate result extent\n");
  } else {
    draw_extent = mismatched_result.find("Int 2", draw_extent) + 4;
    mismatched_result[draw_extent] = '3';
    expect_interp(mismatched_result, "",
                  "multi-normal result dimension mismatch stays interpreted");
  }

  std::string cholesky = base;
  cholesky.replace(call + std::string("(FunApp (StanLib ").size(),
                   std::string("multi_normal_rng").size(),
                   "multi_normal_cholesky_rng");
  expect_interp(cholesky, "unsupported function multi_normal_cholesky_rng",
                "multi-normal Cholesky stays interpreted");
}

void test_compiled_multi_normal_rng() {
  using namespace stanli;
  const std::string text =
      slurp("tests/fixtures/gq_multi_normal_rng.tmir.sexp");
  CompiledModel cm = compile_model(text, DataMap{});
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL multi-normal RNG did not compile completely: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }
  int multi_ops = 0, uniform_ops = 0, other_rng_ops = 0;
  for (const Op& op : cm.write_array->graph.ops) {
    if (op.opcode != OP_RNG) continue;
    if (op.variant == kMultiNormalRngVariant) {
      ++multi_ops;
      if (op.n_in != 2 || op.n_idata != 1 || op.idata == nullptr ||
          op.idata[0] != 2 || cm.write_array->graph.slots[op.in[0]].len != 2 ||
          cm.write_array->graph.slots[op.in[1]].len != 4 ||
          cm.write_array->graph.slots[op.out].len != 2) {
        ++failures;
        std::printf("FAIL multi-normal RNG malformed graph descriptor\n");
      }
    } else if (op.variant == static_cast<uint8_t>(ScalarRng::Uniform)) {
      ++uniform_ops;
    } else {
      ++other_rng_ops;
    }
  }
  if (multi_ops != 1 || uniform_ops != 1 || other_rng_ops != 0) {
    ++failures;
    std::printf(
        "FAIL multi-normal RNG op census: multi=%d uniform=%d other=%d\n",
        multi_ops, uniform_ops, other_rng_ops);
  }
  expect_eq("multi-normal RNG columns", joined(cm.write_array->columns),
            "mu.1,mu.2,sigma.1,sigma.2,rho,draw.1,draw.2,shifted,tail");

  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  const std::vector<double> unconstrained = {0.5, -1.25, std::log(1.3),
                                             std::log(0.8), 0.73};
  std::copy(unconstrained.begin(), unconstrained.end(), graph.params_data());
  const auto graph_row = [&](WaRng& rng) {
    graph.run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& column : cm.write_array->columns) {
      const double* values = graph.value_ptr(column.slot);
      for (int64_t i = 0; i < column.len; ++i)
        row.push_back(values[column.storage_index(i)]);
    }
    return row;
  };

  // Obtain the exact constrained parameter bits produced by the graph; those
  // are the write_array interpreter's public input contract.
  WaRng probe_rng(999);
  const std::vector<double> probe = graph_row(probe_rng);
  const std::vector<double> mu{probe[0], probe[1]};
  const std::vector<double> sigma{probe[2], probe[3]};
  const double rho = probe[4];
  std::map<std::string, DataMap::Entry> params;
  params["mu"] = vector_entry(mu);
  params["sigma"] = vector_entry(sigma);
  params["rho"] = real_entry(rho);
  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  WaInterp interpreted(program, multi_normal_base());

  WaRng graph_rng(42), interp_rng(42), direct_rng(42);
  const std::vector<double> graph_first = graph_row(graph_rng);
  const std::vector<double> interp_first = interpreted.eval(params, interp_rng);
  const std::vector<double> direct_first =
      multi_normal_direct_row(mu, sigma, rho, direct_rng);
  const std::vector<double> graph_second = graph_row(graph_rng);
  const std::vector<double> interp_second =
      interpreted.eval(params, interp_rng);
  const std::vector<double> direct_second =
      multi_normal_direct_row(mu, sigma, rho, direct_rng);
  const auto graph_next = graph_rng.gen()();
  const auto interp_next = interp_rng.gen()();
  const auto direct_next = direct_rng.gen()();
  if (!same_double_bytes(graph_first, interp_first) ||
      !same_double_bytes(graph_first, direct_first) ||
      !same_double_bytes(graph_second, interp_second) ||
      !same_double_bytes(graph_second, direct_second) ||
      graph_next != interp_next || graph_next != direct_next) {
    ++failures;
    std::printf("FAIL multi-normal sequential row/stream parity\n");
  }
  graph_rng.seed(42);
  if (graph_row(graph_rng) != graph_first) {
    ++failures;
    std::printf("FAIL multi-normal reseed did not reproduce first row\n");
  }

  std::vector<double> invalid_q = unconstrained;
  invalid_q[0] = std::numeric_limits<double>::infinity();
  std::copy(invalid_q.begin(), invalid_q.end(), graph.params_data());
  params["mu"] = vector_entry({std::numeric_limits<double>::infinity(), mu[1]});
  WaRng graph_failing(211), interp_failing(211), direct_failing(211);
  const RngException graph_error =
      capture_rng_exception([&] { (void)graph_row(graph_failing); });
  const RngException interp_error = capture_rng_exception(
      [&] { (void)interpreted.eval(params, interp_failing); });
  const RngException direct_error = capture_rng_exception([&] {
    const std::vector<double> covariance = {
        sigma[0] * sigma[0], sigma[0] * sigma[1] * rho,
        sigma[0] * sigma[1] * rho, sigma[1] * sigma[1]};
    (void)direct_multi_normal_rng(
        {std::numeric_limits<double>::infinity(), mu[1]}, covariance, 2, 2,
        direct_failing);
  });
  if (graph_error.kind == 0 || graph_error.kind != interp_error.kind ||
      graph_error.kind != direct_error.kind ||
      graph_error.message != interp_error.message ||
      graph_error.message != direct_error.message) {
    ++failures;
    std::printf("FAIL multi-normal invalid exception parity\n");
  }

  std::copy(unconstrained.begin(), unconstrained.end(), graph.params_data());
  params["mu"] = vector_entry(mu);
  const RngException missing_state =
      capture_rng_exception([&] { graph.run_forward_only(); });
  const std::vector<double> graph_recovered = graph_row(graph_failing);
  const std::vector<double> interp_recovered =
      interpreted.eval(params, interp_failing);
  const std::vector<double> direct_recovered =
      multi_normal_direct_row(mu, sigma, rho, direct_failing);
  const auto graph_recovered_next = graph_failing.gen()();
  const auto interp_recovered_next = interp_failing.gen()();
  const auto direct_recovered_next = direct_failing.gen()();
  if (missing_state.kind != 3 ||
      missing_state.message.find("caller-owned") == std::string::npos ||
      !same_double_bytes(graph_recovered, interp_recovered) ||
      !same_double_bytes(graph_recovered, direct_recovered) ||
      graph_recovered_next != interp_recovered_next ||
      graph_recovered_next != direct_recovered_next) {
    ++failures;
    std::printf("FAIL multi-normal exception/reuse contract\n");
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
  if (rng_ops != 8) {
    ++failures;
    std::printf("FAIL scalar rng: got %d OP_RNG, want 8\n", rng_ops);
  }
  expect_eq("scalar rng columns", joined(cm.write_array->columns),
            "x,p,u,b,n,l,k,gm,ex");

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

static uint64_t reduction_bits(double x) {
  uint64_t out = 0;
  std::memcpy(&out, &x, sizeof(out));
  return out;
}

static bool same_reduction_value(double got, double want) {
  // Eigen deliberately leaves the sign and payload of a propagated NaN
  // unspecified.  Sanitizer instrumentation can therefore change those bits
  // without changing the reduction's value contract.  Everything else --
  // including signed zero and infinity -- remains a bitwise comparison.
  if (std::isnan(got) || std::isnan(want))
    return std::isnan(got) && std::isnan(want);
  return reduction_bits(got) == reduction_bits(want);
}

static stanli::DataMap reduction_data() {
  stanli::DataMap data;
  data.set_real_array("d", {0.5, -2.0, 4.0});
  data.set_int_array("counts", {1, 2, 3});
  return data;
}

static std::map<std::string, stanli::DataMap::Entry> reduction_base(
    const stanli::DataMap& data) {
  std::map<std::string, stanli::DataMap::Entry> base;
  base["d"] = data.at("d");
  base["counts"] = data.at("counts");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    stanli::DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  return base;
}

void test_product_exact_grouping() {
  using namespace stanli;
  // Direct kernel calls need the one-time registration normally performed by
  // Executor construction.
  {
    Graph graph;
    const int in = graph.add_slot(1, true);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_EXP, {in}, out);
    graph.result_slot = out;
    Executor warm(std::move(graph));
  }

  const Kernel& product = kernel(OP_PROD_VEC);
  if (product.backward == nullptr || product.scratch_size != nullptr) {
    ++failures;
    std::printf("FAIL product kernel implementation contract\n");
  }

  const double denorm = std::numeric_limits<double>::denorm_min();
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::vector<double>> cases = {
      {0.5},
      {-0.0},
      {denorm},
      {inf},
      {nan},
      {0.5, 0.5, 0.5},
      {0.0, inf, 2.0},
      {-0.0, inf, 2.0},
      {inf, 0.0, -2.0},
      {nan, 0.0, inf},
      {1.0 + 0x1p-52, 1.0 - 0x1p-52, 1.0 + 0x1p-51},
      {0.5, 0.5, 0.5, 0.5, 0.5},
      {1e200, 1e200, 1e-200, 1e-200, 3.0},
      {1e200, 1e-200, 1e200, 1e-200, 3.0},
      {denorm, 2.0, 2.0, 2.0, 2.0},
      {-0.0, -1.0, -1.0, 1.0, 1.0},
      {inf, -0.0, nan, -inf, 1.0},
  };

  // Exercise every packet boundary on the build host.  Fixed lengths 1/3/5
  // above pin the public contract; these sizes keep the same test meaningful
  // when Eigen is compiled for SSE, AVX2, or AVX-512.
  const int packet_width = std::max(
      1, static_cast<int>(Eigen::internal::packet_traits<double>::size));
  const int packet_lengths[] = {packet_width - 1, packet_width,
                                packet_width + 1, 2 * packet_width + 1};
  const int exponents[] = {500, -500, 400, -400, 300, -300, 0, 0};
  for (int n : packet_lengths) {
    if (n <= 0) continue;  // Scalar-only Eigen has packet width one.
    std::vector<double> values(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double mantissa =
          1.0 + static_cast<double>((i * 5 + 1) % 7) * 0x1p-52;
      values[static_cast<size_t>(i)] =
          std::ldexp((i % 3 == 0) ? -mantissa : mantissa, exponents[i % 8]);
    }
    cases.push_back(std::move(values));
  }

  std::map<std::string, const mir::FunDef*> funs;
  MirInterp<double> interp(funs, "product grouping test");
  mir::Expr variable;
  variable.kind = mir::Expr::Var;
  variable.name = "x";
  variable.type_ = "UVector";
  variable.unsized.leaf = mir::UnsizedLeaf::Vector;
  mir::Expr call;
  call.kind = mir::Expr::FunApp;
  call.name = "prod";
  call.fn_lib = mir::Expr::Lib::StanLib;
  call.type_ = "UReal";
  call.unsized.leaf = mir::UnsizedLeaf::Real;
  call.args = {variable};

  for (size_t c = 0; c < cases.size(); ++c) {
    const std::vector<double>& values = cases[c];
    Eigen::VectorXd pinned(static_cast<Eigen::Index>(values.size()));
    for (size_t i = 0; i < values.size(); ++i) pinned[i] = values[i];
    const double want = stan::math::prod(pinned);

    DataMap::Entry entry;
    entry.r = values;
    entry.dims = {static_cast<int64_t>(values.size())};
    interp.env()["x"] = std::move(entry);
    const double interpreted = interp.eval(call).r.at(0);
    if (!same_reduction_value(interpreted, want)) {
      ++failures;
      std::printf("FAIL product interpreter case %zu: got %llx want %llx\n", c,
                  static_cast<unsigned long long>(reduction_bits(interpreted)),
                  static_cast<unsigned long long>(reduction_bits(want)));
    }

    // Pin the address-independence seam at every packet-relevant shift.  A
    // direct Eigen::Map differs at odd shifts; OP_PROD_VEC must not.
    for (int offset = 0; offset < packet_width; ++offset) {
      Eigen::VectorXd storage(static_cast<Eigen::Index>(
          values.size() + static_cast<size_t>(packet_width)));
      storage.setZero();
      std::copy(values.begin(), values.end(), storage.data() + offset);
      double got = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] =
          Desc{storage.data() + offset, static_cast<int64_t>(values.size())};
      ctx.out = Desc{&got, 1};
      product.forward(ctx);
      if (!same_reduction_value(got, want)) {
        ++failures;
        std::printf(
            "FAIL product kernel case %zu offset %d: got %llx want "
            "%llx\n",
            c, offset, static_cast<unsigned long long>(reduction_bits(got)),
            static_cast<unsigned long long>(reduction_bits(want)));
      }
    }
  }

  // This tranche changes only Eigen vector/row-vector products.  Stan arrays
  // retain MirInterp's existing ascending scalar fold.
  mir::Expr array_variable = variable;
  array_variable.name = "a";
  array_variable.type_ = "(UArray UReal)";
  array_variable.unsized.leaf = mir::UnsizedLeaf::Real;
  array_variable.unsized.depth = 1;
  mir::Expr array_call = call;
  array_call.args = {array_variable};
  const std::vector<double> array_values = {1e200, 1e200, 1e-200, 1e-200, 3.0};
  DataMap::Entry array_entry;
  array_entry.r = array_values;
  array_entry.dims = {static_cast<int64_t>(array_values.size())};
  interp.env()["a"] = std::move(array_entry);
  double array_want = 1.0;
  for (double value : array_values) array_want *= value;
  const double array_got = interp.eval(array_call).r.at(0);
  if (reduction_bits(array_got) != reduction_bits(array_want)) {
    ++failures;
    std::printf("FAIL array product changed its scalar left fold\n");
  }

  // Pin the actual expression surfaces admitted by lower.cpp.  CmdStan
  // gives Eigen the unevaluated CwiseBinary expression; stanli materializes
  // subtraction into a graph slot before OP_PROD_VEC.  Basic subtraction is
  // lane-bit-transparent, so the two product groupings must still agree.
  const auto check_minus_surface = [&](int n, const char* label) {
    std::vector<Eigen::VectorXd> indexed(2, Eigen::VectorXd(n));
    const double xs[] = {0.5,  -0.25, 2.0, -1.0, std::nextafter(1.0, 0.0),
                         -0.5, 0.25,  1.5};
    for (int i = 0; i < n; ++i)
      indexed[1][i] = xs[i % static_cast<int>(sizeof(xs) / sizeof(xs[0]))];

    const uint64_t direct =
        reduction_bits(stan::math::prod(Eigen::VectorXd::Ones(n) - indexed[1]));
    const uint64_t indexed_transpose = reduction_bits(
        stan::math::prod(Eigen::RowVectorXd::Ones(n) - indexed[1].transpose()));
    if (direct != indexed_transpose) {
      ++failures;
      std::printf(
          "FAIL %s pinned outer-minus surfaces disagree: %llx vs "
          "%llx\n",
          label, static_cast<unsigned long long>(direct),
          static_cast<unsigned long long>(indexed_transpose));
    }

    std::vector<double> materialized(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      materialized[static_cast<size_t>(i)] = 1.0 - indexed[1][i];
    for (int offset = 0; offset < packet_width; ++offset) {
      Eigen::VectorXd storage(n + packet_width);
      storage.setZero();
      std::copy(materialized.begin(), materialized.end(),
                storage.data() + offset);
      double got = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] = Desc{storage.data() + offset, n};
      ctx.out = Desc{&got, 1};
      product.forward(ctx);
      const uint64_t bits = reduction_bits(got);
      if (bits != direct || bits != indexed_transpose) {
        ++failures;
        std::printf("FAIL %s offset %d: got %llx want %llx\n", label, offset,
                    static_cast<unsigned long long>(bits),
                    static_cast<unsigned long long>(direct));
      }
    }
  };
  check_minus_surface(5, "len5 outer-minus product");
  check_minus_surface(2 * packet_width + 1,
                      "packet-boundary outer-minus product");

  // The corpus surface indexes a row of a column-major matrix and transposes
  // it.  That Eigen block remains strided, disables packet access on the
  // outer subtraction, and makes prod reduce from coefficient zero in
  // ascending scalar order.  Search deterministic valid probabilities for a
  // case that distinguishes that order from the packet variant, then pin the
  // raw-expression oracle and the materialized graph kernel to the scalar
  // result.  Scalar-only Eigen builds have no distinct packet order.
  const int matrix_n = 2 * packet_width + 1;
  Eigen::MatrixXd matrix(2, matrix_n);
  std::vector<double> factors(static_cast<size_t>(matrix_n));
  bool distinguished = packet_width == 1;
  uint64_t state = 0x4d4154524958524fULL;
  for (int trial = 0; trial < 4096 && !distinguished; ++trial) {
    for (int i = 0; i < matrix_n; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const double p = 0.01 + 0.98 * static_cast<double>(state >> 11) /
                                  static_cast<double>(uint64_t{1} << 53);
      matrix(0, i) = 0.25;
      matrix(1, i) = p;
      factors[static_cast<size_t>(i)] = 1.0 - p;
    }
    double scalar = factors[0];
    for (int i = 1; i < matrix_n; ++i)
      scalar *= factors[static_cast<size_t>(i)];

    double packet = 0.0;
    KernelCtx packet_ctx;
    packet_ctx.n_in = 1;
    packet_ctx.in[0] = Desc{factors.data(), matrix_n};
    packet_ctx.out = Desc{&packet, 1};
    packet_ctx.variant = 0;
    product.forward(packet_ctx);
    if (reduction_bits(packet) == reduction_bits(scalar)) continue;

    const double direct = stan::math::prod(Eigen::VectorXd::Ones(matrix_n) -
                                           matrix.row(1).transpose());
    double got = 0.0;
    KernelCtx scalar_ctx = packet_ctx;
    scalar_ctx.out = Desc{&got, 1};
    scalar_ctx.variant = 1;
    product.forward(scalar_ctx);
    if (reduction_bits(direct) != reduction_bits(scalar) ||
        reduction_bits(got) != reduction_bits(scalar)) {
      ++failures;
      std::printf("FAIL matrix-row product did not use scalar grouping\n");
    }
    distinguished = true;
  }
  if (!distinguished) {
    ++failures;
    std::printf("FAIL matrix-row oracle did not distinguish packet order\n");
  }
}

void test_reduction_view_grouping() {
  using namespace stanli;
  const Kernel& product = kernel(OP_PROD_VEC);
  const Kernel& extrema = kernel(OP_EXTREMA_VEC);
  const int phase_modulus = static_cast<int>(extrema_phase_modulus());
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double tie_pool[] = {0.0, -0.0, 2.0, -3.0, 2.0, nan, -0.0,
                             0.0, -4.0, 9.0, -4.0, 1.0, nan, -2.0};
  const int tie_pool_size =
      static_cast<int>(sizeof(tie_pool) / sizeof(*tie_pool));

  // A graph value has already been materialized, so the reduction opcode
  // carries the source view's phase explicitly. Compare both native kernels
  // with Stan Math reducing the original Eigen segment.
  for (int n = 1; n <= 257; ++n) {
    for (int offset = 0; offset < phase_modulus; ++offset) {
      Eigen::VectorXd product_base(offset + n + 2);
      Eigen::VectorXd extrema_base(offset + n + 2);
      for (int i = 0; i < product_base.size(); ++i) {
        const int centered = (i * 37 + n * 11) % 29 - 14;
        product_base[i] = 1.0 + centered * 0x1p-16;
        extrema_base[i] = tie_pool[i % tie_pool_size];
      }
      const Eigen::VectorXd product_owned = product_base.segment(offset, n);
      const Eigen::VectorXd extrema_owned = extrema_base.segment(offset, n);
      const double product_want =
          stan::math::prod(product_base.segment(offset, n));
      const double extrema_wants[] = {
          stan::math::min(extrema_base.segment(offset, n)),
          stan::math::max(extrema_base.segment(offset, n))};

      int phase = offset;
      double product_got = 0.0;
      KernelCtx product_ctx;
      product_ctx.n_in = 1;
      product_ctx.in[0] = Desc{const_cast<double*>(product_owned.data()), n};
      product_ctx.out = Desc{&product_got, 1};
      product_ctx.variant = 4;
      product_ctx.idata = &phase;
      product_ctx.n_idata = 1;
      product.forward(product_ctx);
      if (!same_reduction_value(product_got, product_want)) {
        ++failures;
        std::printf("FAIL phased product n=%d offset=%d\n", n, offset);
      }

      for (int maximum = 0; maximum < 2; ++maximum) {
        double got = 0.0;
        KernelCtx ctx;
        ctx.n_in = 1;
        ctx.in[0] = Desc{const_cast<double*>(extrema_owned.data()), n};
        ctx.out = Desc{&got, 1};
        ctx.variant = static_cast<uint8_t>(maximum | 4);
        ctx.idata = &phase;
        ctx.n_idata = 1;
        extrema.forward(ctx);
        if (!same_reduction_value(got, extrema_wants[maximum])) {
          ++failures;
          std::printf("FAIL phased extrema n=%d offset=%d %s\n", n, offset,
                      maximum ? "max" : "min");
        }
      }
    }
  }

  // The contract here is exact parity with the original view. Some Eigen
  // configurations use the same scalar traversal for every tested size, so
  // requiring a value difference from an unphased reduction would be brittle.

  // Gathered arguments have no packet access. Repeated indices are retained
  // in index order and the scalar reduction's reverse path must scatter back
  // through OP_GATHER rather than treating the gather as a copy.
  bool gather_distinguished = false;
  for (int n = 1; n <= 20; ++n) {
    Eigen::VectorXd source(n);
    for (int i = 0; i < n; ++i) source[i] = tie_pool[i % tie_pool_size];
    std::vector<int> positions(static_cast<size_t>(n));
    std::vector<double> gathered(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      positions[static_cast<size_t>(i)] = n - i;
      gathered[static_cast<size_t>(i)] = source[n - i - 1];
    }
    const stan::model::index_multi index(positions);
    for (int maximum = 0; maximum < 2; ++maximum) {
      const double want =
          maximum
              ? stan::math::max(stan::model::rvalue(source, "source", index))
              : stan::math::min(stan::model::rvalue(source, "source", index));
      double got = 0.0;
      double selected = 0.0;
      KernelCtx ctx;
      ctx.n_in = 1;
      ctx.in[0] = Desc{gathered.data(), n};
      ctx.out = Desc{&got, 1};
      ctx.variant = static_cast<uint8_t>(maximum | 2);
      ctx.scratch = &selected;
      extrema.forward(ctx);
      if (!same_reduction_value(got, want)) {
        ++failures;
        std::printf("FAIL gathered extrema n=%d %s\n", n,
                    maximum ? "max" : "min");
      }
      const Eigen::Map<const Eigen::VectorXd> packet(gathered.data(), n);
      const double packet_want =
          maximum ? stan::math::max(packet) : stan::math::min(packet);
      gather_distinguished |= !same_reduction_value(packet_want, want);
    }
  }
  if (!gather_distinguished) {
    ++failures;
    std::printf(
        "FAIL gathered extrema oracle never distinguished packet order\n");
  }

  {
    Graph graph;
    const int source = graph.add_slot(3, true);
    const int gathered = graph.add_slot(4, false);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_GATHER, {source}, gathered, {2, 2, 0, 2});
    const int product_op = graph.add_op(OP_PROD_VEC, {gathered}, out);
    graph.ops[product_op].variant = 1;
    graph.result_slot = out;
    Executor executor(std::move(graph));
    const std::array<double, 3> values = {2.0, 3.0, 5.0};
    std::copy(values.begin(), values.end(), executor.params_data());
    double adj[3] = {};
    const double value = executor.gradient(adj);
    if (value != 250.0 || adj[0] != 125.0 || adj[1] != 0.0 || adj[2] != 150.0) {
      ++failures;
      std::printf("FAIL gathered product adjoints were not accumulated\n");
    }
  }
  {
    Graph graph;
    const int source = graph.add_slot(3, true);
    const int gathered = graph.add_slot(4, false);
    const int out = graph.add_slot(1, false);
    graph.add_op(OP_GATHER, {source}, gathered, {1, 1, 2, 0});
    const int extrema_op = graph.add_op(OP_EXTREMA_VEC, {gathered}, out);
    graph.ops[extrema_op].variant = 2;
    graph.result_slot = out;
    Executor executor(std::move(graph));
    const std::array<double, 3> values = {5.0, 2.0, 7.0};
    std::copy(values.begin(), values.end(), executor.params_data());
    double adj[3] = {};
    const double value = executor.gradient(adj);
    if (value != 2.0 || adj[0] != 0.0 || adj[1] != 1.0 || adj[2] != 0.0) {
      ++failures;
      std::printf(
          "FAIL gathered extrema adjoint did not route through gather\n");
    }
  }
}

void test_layout_materialization_boundaries() {
  using namespace stanli;
  const std::string text =
      slurp("tests/fixtures/gq_layout_materialization.tmir.sexp");
  DataMap data;
  data.set_int_array("idx", {1, 2, 3, 4, 5});
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL layout materialization fixture did not compile: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }

  int packet_products = 0, phased_products = 0, scalar_extrema = 0;
  for (const Op& op : cm.write_array->graph.ops) {
    if (op.opcode == OP_PROD_VEC) {
      packet_products += op.variant == 0;
      phased_products += op.variant == 4 && op.n_idata == 1 &&
                         op.idata != nullptr && op.idata[0] == 1;
    }
    scalar_extrema += op.opcode == OP_EXTREMA_VEC && op.variant == 2;
  }
  if (packet_products != 4 || phased_products != 1 || scalar_extrema != 1) {
    ++failures;
    std::printf(
        "FAIL materialization variants: packet=%d phased=%d scalar_min=%d\n",
        packet_products, phased_products, scalar_extrema);
  }

  const std::vector<double> x = {1e200, 1e200, 1e-200, 1e-200, 3.0};
  const std::vector<double> av0 = {0.5, -0.25, 2.0, -1.0, 4.0};
  const std::vector<double> av1 = x;
  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  if (graph.n_params() != 15) {
    ++failures;
    std::printf("FAIL materialization parameter width: got %lld want 15\n",
                static_cast<long long>(graph.n_params()));
    return;
  }
  std::copy(x.begin(), x.end(), graph.params_data());
  std::copy(av0.begin(), av0.end(), graph.params_data() + x.size());
  std::copy(av1.begin(), av1.end(),
            graph.params_data() + x.size() + av0.size());
  graph.run_forward_only();
  std::map<std::string, double> got;
  const std::vector<std::string> names =
      CompiledModel::csv_names(cm.write_array->columns);
  size_t position = 0;
  for (const auto& column : cm.write_array->columns) {
    const double* values = graph.value_ptr(column.slot);
    for (int64_t i = 0; i < column.len; ++i)
      got[names[position++]] = values[column.storage_index(i)];
  }

  Eigen::VectorXd owned(5);
  std::copy(x.begin(), x.end(), owned.data());
  const double owned_product = stan::math::prod(owned);
  const double tail_product = stan::math::prod(owned.segment(1, 4));
  const std::map<std::string, double> want = {
      {"initialized_prod", owned_product}, {"assigned_prod", owned_product},
      {"returned_prod", owned_product},    {"inner_prod", owned_product},
      {"inner_tail_prod", tail_product},
  };
  for (const auto& expected : want) {
    const auto actual = got.find(expected.first);
    if (actual != got.end() &&
        same_reduction_value(actual->second, expected.second))
      continue;
    ++failures;
    std::printf("FAIL materialization value %s\n", expected.first.c_str());
  }

  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  std::map<std::string, DataMap::Entry> base;
  base["idx"] = data.at("idx");
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  WaInterp interp(program, std::move(base));
  std::map<std::string, DataMap::Entry> params;
  params["x"].r = x;
  params["x"].dims = {5};
  for (size_t i = 0; i < av0.size(); ++i) {
    params["av"].r.push_back(av0[i]);
    params["av"].r.push_back(av1[i]);
  }
  params["av"].dims = {2, 5};
  WaRng rng(1234);
  const std::vector<double> interpreted = interp.eval(params, rng);
  if (interpreted.size() != names.size()) {
    ++failures;
    std::printf("FAIL materialization interpreter row width\n");
  } else {
    for (size_t i = 0; i < names.size(); ++i) {
      const auto actual = got.find(names[i]);
      if (actual != got.end() &&
          same_reduction_value(actual->second, interpreted[i]))
        continue;
      ++failures;
      std::printf("FAIL materialization graph/interpreter %s\n",
                  names[i].c_str());
    }
  }

  const std::string udf_text =
      slurp("tests/fixtures/gq_udf_return_layout.tmir.sexp");
  CompiledModel udf = compile_model(udf_text, data);
  int udf_packet_products = 0;
  if (udf.write_array)
    for (const Op& op : udf.write_array->graph.ops)
      udf_packet_products += op.opcode == OP_PROD_VEC && op.variant == 0;
  if (!udf.write_array || udf.write_array->interp ||
      !udf.write_array->truncated.empty() || udf_packet_products != 1) {
    ++failures;
    std::printf("FAIL container UDF return did not compile as owning: %s\n",
                udf.write_array ? udf.write_array->truncated.c_str()
                                : "no write_array");
  } else {
    Executor udf_graph(std::move(udf.write_array->graph));
    udf.write_array->bind(udf_graph);
    std::copy(x.begin(), x.end(), udf_graph.params_data());
    udf_graph.run_forward_only();
    const std::vector<std::string> udf_names =
        CompiledModel::csv_names(udf.write_array->columns);
    const auto returned =
        std::find(udf_names.begin(), udf_names.end(), "returned_prod");
    bool matched = false;
    if (returned != udf_names.end()) {
      const size_t target = static_cast<size_t>(returned - udf_names.begin());
      size_t cursor = 0;
      for (const auto& column : udf.write_array->columns) {
        for (int64_t i = 0; i < column.len; ++i, ++cursor) {
          if (cursor != target) continue;
          matched = same_reduction_value(
              udf_graph.value_ptr(column.slot)[column.storage_index(i)],
              owned_product);
        }
      }
    }
    if (!matched) {
      ++failures;
      std::printf("FAIL container UDF return product value\n");
    }
  }
}

void test_main_index_layout_metadata() {
  using namespace stanli;
  const std::string text =
      slurp("tests/fixtures/gq_main_index_layout.tmir.sexp");
  DataMap data;
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL main index layout fixture did not compile: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }

  int phased_products = 0, packet_extrema = 0;
  for (const Op& op : cm.write_array->graph.ops) {
    phased_products += op.opcode == OP_PROD_VEC && op.variant == 4 &&
                       op.n_idata == 1 && op.idata != nullptr &&
                       op.idata[0] == 1;
    packet_extrema += op.opcode == OP_EXTREMA_VEC && op.variant == 0;
  }
  if (phased_products != 1 || packet_extrema != 1) {
    ++failures;
    std::printf(
        "FAIL main index layout variants: phased_prod=%d "
        "packet_min=%d\n",
        phased_products, packet_extrema);
  }

  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  if (graph.n_params() != 32) {
    ++failures;
    std::printf("FAIL main index layout parameter width: got %lld want 32\n",
                static_cast<long long>(graph.n_params()));
    return;
  }
  std::vector<double> params(32);
  for (size_t i = 0; i < params.size(); ++i)
    params[i] = static_cast<double>(i + 1);
  std::copy(params.begin(), params.end(), graph.params_data());
  graph.run_forward_only();

  std::map<std::string, double> got;
  const std::vector<std::string> names =
      CompiledModel::csv_names(cm.write_array->columns);
  size_t position = 0;
  for (const auto& column : cm.write_array->columns) {
    const double* values = graph.value_ptr(column.slot);
    for (int64_t i = 0; i < column.len; ++i)
      got[names[position++]] = values[column.storage_index(i)];
  }

  Eigen::VectorXd nested_range(4);
  std::copy(params.begin() + 11, params.begin() + 15, nested_range.data());
  const std::map<std::string, double> want = {
      {"nested_range_prod", stan::math::prod(nested_range)},
      {"matrix_cell_min", std::min({params[20], params[24], params[28]})},
  };
  for (const auto& expected : want) {
    const auto actual = got.find(expected.first);
    if (actual != got.end() &&
        same_reduction_value(actual->second, expected.second))
      continue;
    ++failures;
    std::printf("FAIL main index layout value %s\n", expected.first.c_str());
  }
}

void test_matrix_transpose_extrema_fallback() {
  using namespace stanli;
  const std::string text =
      slurp("tests/fixtures/gq_matrix_transpose_extrema.tmir.sexp");
  DataMap data;
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || !cm.write_array->interp ||
      cm.write_array->truncated.find("grouping is not native") ==
          std::string::npos) {
    ++failures;
    std::printf(
        "FAIL matrix transpose extrema did not fail closed: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<double> values = {0.0, -0.0, 2.0, -3.0, 2.0,
                                      nan, -0.0, 0.0, -4.0, 9.0};
  Eigen::Matrix<double, 5, 2> matrix;
  std::copy(values.begin(), values.end(), matrix.data());
  const double want = stan::math::min(matrix.transpose());

  auto program =
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
  WaInterp interp(program, std::move(base));
  std::map<std::string, DataMap::Entry> params;
  params["m"].r = values;
  params["m"].dims = {5, 2};
  WaRng rng(1234);
  const std::vector<double> row = interp.eval(params, rng);
  const std::vector<std::string> names =
      CompiledModel::csv_names(interp.columns());
  const auto found = std::find(names.begin(), names.end(), "transpose_min");
  if (found == names.end() ||
      !same_reduction_value(row[static_cast<size_t>(found - names.begin())],
                            want)) {
    ++failures;
    const double got = found == names.end()
                           ? std::numeric_limits<double>::quiet_NaN()
                           : row[static_cast<size_t>(found - names.begin())];
    std::printf(
        "FAIL matrix transpose extrema interpreter value: got=%llx "
        "want=%llx\n",
        static_cast<unsigned long long>(reduction_bits(got)),
        static_cast<unsigned long long>(reduction_bits(want)));
  }
}

void test_extrema_exact_grouping() {
  using namespace stanli;
  const Kernel& extrema = kernel(OP_EXTREMA_VEC);
  if (extrema.backward == nullptr || extrema.scratch_size == nullptr) {
    ++failures;
    std::printf("FAIL extrema kernel has no differentiable implementation\n");
  }

  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<std::vector<double>> cases = {
      {},
      {1.0},
      {0.0, -0.0},
      {-0.0, 0.0},
      {inf, -inf, 3.0},
      {-inf, inf, -3.0},
      {2.0, 2.0, -4.0, -4.0, 2.0},
      {nan, 1.0, -2.0},
      {1.0, nan, -2.0},
      {1.0, -2.0, nan},
      {nan, nan, 0.0, -0.0},
  };
  const int packet_width = std::max(
      1, static_cast<int>(Eigen::internal::packet_traits<double>::size));
  for (int n : {packet_width - 1, packet_width, packet_width + 1,
                2 * packet_width - 1, 2 * packet_width, 2 * packet_width + 1}) {
    if (n < 0) continue;
    std::vector<double> values(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      const double magnitude =
          std::ldexp(1.0 + static_cast<double>((i * 7 + 3) % 11) * 0x1p-52,
                     (i % 7) * 137 - 411);
      values[static_cast<size_t>(i)] = i % 2 == 0 ? magnitude : -magnitude;
    }
    if (n > 2) values[static_cast<size_t>(n - 1)] = values[1];
    cases.push_back(std::move(values));
  }

  for (size_t c = 0; c < cases.size(); ++c) {
    const std::vector<double>& values = cases[c];
    Eigen::VectorXd pinned(static_cast<Eigen::Index>(values.size()));
    std::copy(values.begin(), values.end(), pinned.data());
    const double wants[] = {stan::math::min(pinned), stan::math::max(pinned)};
    for (int variant = 0; variant < 2; ++variant) {
      for (int offset = 0; offset < packet_width; ++offset) {
        Eigen::VectorXd storage(static_cast<Eigen::Index>(
            values.size() + static_cast<size_t>(packet_width)));
        storage.setZero();
        std::copy(values.begin(), values.end(), storage.data() + offset);
        double got = 0.0;
        KernelCtx ctx;
        ctx.n_in = 1;
        ctx.in[0] =
            Desc{storage.data() + offset, static_cast<int64_t>(values.size())};
        ctx.out = Desc{&got, 1};
        ctx.variant = static_cast<uint8_t>(variant);
        extrema.forward(ctx);
        if (!same_reduction_value(got, wants[variant])) {
          ++failures;
          std::printf(
              "FAIL extrema kernel case %zu variant %d offset %d: got %llx "
              "want %llx\n",
              c, variant, offset,
              static_cast<unsigned long long>(reduction_bits(got)),
              static_cast<unsigned long long>(reduction_bits(wants[variant])));
        }
      }

      std::vector<double> active_values = values;
      Eigen::Matrix<stan::math::var, -1, 1> active(
          static_cast<Eigen::Index>(values.size()));
      for (size_t i = 0; i < values.size(); ++i)
        active(static_cast<Eigen::Index>(i)) = values[i];
      stan::math::var want_active =
          variant == 0 ? stan::math::min(active) : stan::math::max(active);
      double got_active = 0.0;
      double selected = 0.0;
      std::vector<double> adj(values.size(), 0.0);
      KernelCtx active_ctx;
      active_ctx.n_in = 1;
      active_ctx.in[0] =
          Desc{active_values.data(), static_cast<int64_t>(values.size())};
      active_ctx.out = Desc{&got_active, 1};
      active_ctx.scratch = &selected;
      active_ctx.variant = static_cast<uint8_t>(variant | 2);
      extrema.forward(active_ctx);
      active_ctx.in_adj[0] = Desc{adj.data(), static_cast<int64_t>(adj.size())};
      active_ctx.out_adj = 1.0;
      extrema.backward(active_ctx);
      want_active.grad();
      if (!same_reduction_value(got_active, want_active.val())) {
        ++failures;
        std::printf("FAIL active extrema value case %zu variant %d\n", c,
                    variant);
      }
      for (size_t i = 0; i < adj.size(); ++i) {
        if (adj[i] == active(static_cast<Eigen::Index>(i)).adj()) continue;
        ++failures;
        std::printf(
            "FAIL active extrema adjoint case %zu variant %d index %zu\n", c,
            variant, i);
      }
      stan::math::recover_memory();
    }
  }

  // The generic interpreter keeps scalar-fold behavior outside the audited
  // direct write_array surface, except that Stan's defined empty-container
  // results/errors must not become an out_of_range accident.
  std::map<std::string, const mir::FunDef*> funs;
  MirInterp<double> interp(funs, "empty extrema legacy test");
  mir::Expr arg;
  arg.kind = mir::Expr::Var;
  arg.name = "a";
  arg.type_ = "(UArray UReal)";
  arg.unsized.leaf = mir::UnsizedLeaf::Real;
  arg.unsized.depth = 1;
  mir::Expr call;
  call.kind = mir::Expr::FunApp;
  call.name = "min";
  call.fn_lib = mir::Expr::Lib::StanLib;
  call.type_ = "UReal";
  call.unsized.leaf = mir::UnsizedLeaf::Real;
  call.args = {arg};
  DataMap::Entry empty_real;
  empty_real.dims = {0};
  interp.env()["a"] = empty_real;
  if (reduction_bits(interp.eval(call).r.at(0)) != reduction_bits(inf)) {
    ++failures;
    std::printf("FAIL empty real legacy min is not +infinity\n");
  }
  call.name = "max";
  if (reduction_bits(interp.eval(call).r.at(0)) != reduction_bits(-inf)) {
    ++failures;
    std::printf("FAIL empty real legacy max is not -infinity\n");
  }
  arg.type_ = "(UArray UInt)";
  arg.unsized.leaf = mir::UnsizedLeaf::Int;
  call.args = {arg};
  DataMap::Entry empty_int;
  empty_int.is_int = true;
  empty_int.dims = {0};
  interp.env()["a"] = empty_int;
  for (const char* name : {"min", "max"}) {
    call.name = name;
    bool invalid = false;
    try {
      (void)interp.eval(call);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    if (!invalid) {
      ++failures;
      std::printf(
          "FAIL empty int legacy %s did not preserve invalid_argument\n", name);
    }
  }
}

void test_compiled_gq_extrema() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gq_extrema.tmir.sexp");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<std::vector<double>> cases = {
      {},
      {0.0},
      {0.0, -0.0, 2.0, -3.0, 2.0},
      {-0.0, 0.0, -4.0, 9.0, -4.0},
      {nan, 1.0, -2.0, 8.0, -2.0},
      {1.0, nan, -2.0, 8.0, -2.0},
  };

  for (size_t c = 0; c < cases.size(); ++c) {
    const std::vector<double>& x = cases[c];
    std::vector<double> xr(x.rbegin(), x.rend());
    DataMap data;
    data.set_int("N", static_cast<long>(x.size()));
    data.set_real_array("d", x, {static_cast<int64_t>(x.size())});
    CompiledModel cm = compile_model(text, data);
    if (!cm.write_array || cm.write_array->interp ||
        !cm.write_array->truncated.empty()) {
      ++failures;
      std::printf("FAIL extrema fixture case %zu did not compile: %s\n", c,
                  cm.write_array ? cm.write_array->truncated.c_str()
                                 : "no write_array");
      continue;
    }
    int extrema_ops = 0, minima = 0, maxima = 0;
    for (const Op& op : cm.write_array->graph.ops) {
      if (op.opcode != OP_EXTREMA_VEC) continue;
      ++extrema_ops;
      minima += op.variant == 0;
      maxima += op.variant == 1;
    }
    if (extrema_ops != 4 || minima != 2 || maxima != 2) {
      ++failures;
      std::printf(
          "FAIL extrema fixture census case %zu: all=%d min=%d max=%d\n", c,
          extrema_ops, minima, maxima);
      continue;
    }

    Executor graph(std::move(cm.write_array->graph));
    cm.write_array->bind(graph);
    std::copy(x.begin(), x.end(), graph.params_data());
    std::copy(xr.begin(), xr.end(), graph.params_data() + x.size());
    graph.run_forward_only();
    std::vector<double> graph_row;
    for (const auto& column : cm.write_array->columns) {
      const double* values = graph.value_ptr(column.slot);
      for (int64_t i = 0; i < column.len; ++i)
        graph_row.push_back(values[column.storage_index(i)]);
    }

    auto program =
        std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
    std::map<std::string, DataMap::Entry> base;
    base["N"] = data.at("N");
    base["d"] = data.at("d");
    for (const char* flag :
         {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
      DataMap::Entry one;
      one.is_int = true;
      one.i = {1};
      one.r = {1.0};
      base[flag] = one;
    }
    WaInterp interp(program, std::move(base));
    std::map<std::string, DataMap::Entry> params;
    params["x"].r = x;
    params["x"].dims = {static_cast<int64_t>(x.size())};
    params["xr"].r = xr;
    params["xr"].dims = {static_cast<int64_t>(xr.size())};
    WaRng rng(1234);
    const std::vector<double> interp_row = interp.eval(params, rng);
    if (graph_row.size() != interp_row.size()) {
      ++failures;
      std::printf("FAIL extrema fixture row width case %zu\n", c);
      continue;
    }
    for (size_t i = 0; i < graph_row.size(); ++i) {
      if (same_reduction_value(graph_row[i], interp_row[i])) continue;
      ++failures;
      std::printf("FAIL extrema graph/interpreter case %zu column %zu\n", c, i);
      break;
    }

    if (x.empty()) {
      const std::vector<std::string> names =
          CompiledModel::csv_names(cm.write_array->columns);
      for (size_t i = 0; i < names.size(); ++i) {
        if ((names[i] == "x_min" || names[i] == "xr_min" ||
             names[i] == "d_min") &&
            reduction_bits(graph_row[i]) !=
                reduction_bits(std::numeric_limits<double>::infinity())) {
          ++failures;
          std::printf("FAIL empty %s is not +infinity\n", names[i].c_str());
        }
        if ((names[i] == "x_max" || names[i] == "xr_max" ||
             names[i] == "d_max") &&
            reduction_bits(graph_row[i]) !=
                reduction_bits(-std::numeric_limits<double>::infinity())) {
          ++failures;
          std::printf("FAIL empty %s is not -infinity\n", names[i].c_str());
        }
      }
    }
  }
}

static stanli::DataMap extrema_guard_data() {
  stanli::DataMap data;
  data.set_int("N", 5);
  data.set_real_array("d", {1.0, -2.0, 3.0, -4.0, 5.0}, {5});
  return data;
}

static void expect_extrema_interp(const std::string& text, const char* what) {
  using namespace stanli;
  try {
    CompiledModel cm = compile_model(text, extrema_guard_data());
    if (cm.write_array && cm.write_array->interp) return;
    ++failures;
    std::printf(
        "FAIL %s: got %s\n", what,
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: mutation did not parse/compile: %s\n", what,
                e.what());
  }
}

static void expect_extrema_compiled(const std::string& text,
                                    const stanli::DataMap& data,
                                    const char* what) {
  using namespace stanli;
  try {
    CompiledModel cm = compile_model(text, data);
    if (cm.write_array && !cm.write_array->interp &&
        cm.write_array->truncated.empty())
      return;
    ++failures;
    std::printf(
        "FAIL %s: got %s\n", what,
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: did not compile: %s\n", what, e.what());
  }
}

static void expect_extrema_fixture_interp(const std::string& fixture,
                                          const stanli::DataMap& data,
                                          const char* what) {
  using namespace stanli;
  try {
    CompiledModel cm = compile_model(slurp(fixture), data);
    if (cm.write_array && cm.write_array->interp &&
        cm.write_array->truncated.find("expression surface") !=
            std::string::npos)
      return;
    ++failures;
    std::printf(
        "FAIL %s: got %s\n", what,
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: fixture did not parse/compile: %s\n", what, e.what());
  }
}

static void expect_extrema_fixture_compiled(const std::string& fixture,
                                            const stanli::DataMap& data,
                                            const char* what) {
  using namespace stanli;
  try {
    CompiledModel cm = compile_model(slurp(fixture), data);
    if (cm.write_array && !cm.write_array->interp &&
        cm.write_array->truncated.empty())
      return;
    ++failures;
    std::printf(
        "FAIL %s: got %s\n", what,
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: fixture did not compile: %s\n", what, e.what());
  }
}

void test_gq_extrema_lowering_guards() {
  using namespace stanli;
  const std::string base = slurp("tests/fixtures/gq_extrema.tmir.sexp");
  const size_t wa = base.find("(generate_quantities");
  const size_t min_call = base.find("(FunApp (StanLib min", wa);
  const std::string vector_node =
      "((pattern (Var x)) (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const size_t arg_at = base.find(vector_node, min_call);
  if (wa == std::string::npos || min_call == std::string::npos ||
      arg_at == std::string::npos) {
    ++failures;
    std::printf("FAIL extrema guard fixture has no direct min call\n");
    return;
  }

  struct BadType {
    const char* type;
    const char* label;
  };
  for (const BadType& bad :
       {BadType{"UReal", "scalar"}, BadType{"UMatrix", "matrix"},
        BadType{"(UArray UReal)", "array"}}) {
    std::string mutated = base;
    const std::string from = "type_ UVector";
    const size_t at = mutated.find(from, arg_at);
    if (at == std::string::npos) {
      ++failures;
      std::printf("FAIL extrema mutation cannot find %s type\n", bad.label);
      continue;
    }
    mutated.replace(at, from.size(), std::string("type_ ") + bad.type);
    expect_extrema_interp(mutated, (std::string("extrema ") + bad.label +
                                    " argument stays interpreted")
                                       .c_str());
  }

  const std::string exp_vector =
      "((pattern\n"
      "            (FunApp (StanLib exp FnPlain AoS)\n"
      "             (" +
      vector_node +
      ")))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string expression = base;
  expression.replace(arg_at, vector_node.size(), exp_vector);
  expect_extrema_compiled(expression, extrema_guard_data(), "min(exp(x))");

  const std::string indexed =
      "((pattern\n"
      "            (Indexed\n"
      "             " +
      vector_node +
      "\n"
      "             ((Between\n"
      "               ((pattern (Lit Int 2))\n"
      "                (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly))))\n"
      "               ((pattern (Lit Int 5))\n"
      "                (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly))))))))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string view = base;
  view.replace(arg_at, vector_node.size(), indexed);
  expect_extrema_compiled(view, extrema_guard_data(), "min(x[2:5])");

  std::string zero_args = base;
  zero_args.erase(arg_at, vector_node.size());
  expect_extrema_interp(zero_args, "zero-argument min stays interpreted");

  std::string two_args = base;
  two_args.insert(arg_at + vector_node.size(), " " + vector_node);
  expect_extrema_interp(two_args, "two-argument dynamic min stays interpreted");

  std::string container_result = base;
  const std::string scalar_result =
      "(meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))";
  const size_t result_at = container_result.find(scalar_result, arg_at);
  if (result_at == std::string::npos) {
    ++failures;
    std::printf("FAIL extrema mutation cannot find result metadata\n");
  } else {
    container_result.replace(
        result_at, scalar_result.size(),
        "(meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))");
    expect_extrema_interp(container_result,
                          "container-result min stays interpreted");
  }

  // O1 normally inlines this UDF call.  Restore the retained call so the
  // direct formal reaches lowering at udf_depth > 0 and cannot acquire an
  // owning-vector provenance merely from its Var syntax.
  std::string udf = slurp("tests/fixtures/gq_extrema_udf.tmir.sexp");
  const size_t udf_wa = udf.find("(generate_quantities");
  const std::string stan_min = "(FunApp (StanLib min FnPlain AoS)";
  const size_t inlined = udf.find(stan_min, udf_wa);
  if (udf_wa == std::string::npos || inlined == std::string::npos) {
    ++failures;
    std::printf("FAIL extrema UDF fixture has no inlined call\n");
  } else {
    udf.replace(inlined, stan_min.size(),
                "(FunApp (UserDefined vector_min FnPlain)");
    expect_extrema_compiled(udf, extrema_guard_data(), "dynamic UDF extrema");
  }

  // The same opcode now serves active log_prob reductions through its stored
  // selected coefficient.
  bool reverse_compiled = false;
  try {
    DataMap no_data;
    CompiledModel reverse = compile_model(
        slurp("tests/fixtures/gq_extrema_reverse.tmir.sexp"), no_data);
    reverse_compiled =
        std::any_of(reverse.graph.ops.begin(), reverse.graph.ops.end(),
                    [](const Op& op) { return op.opcode == OP_EXTREMA_VEC; });
  } catch (const CompileError&) {
  }
  if (!reverse_compiled) {
    ++failures;
    std::printf("FAIL dynamic log_prob extrema was not compiled\n");
  }
}

void test_gq_extrema_view_lowering_guards() {
  using namespace stanli;
  DataMap row_data;
  row_data.set_int("N", 5);
  expect_extrema_fixture_compiled("tests/fixtures/gq_extrema_row.tmir.sexp",
                                  row_data, "strided matrix-row extrema");

  DataMap gather_data;
  gather_data.set_int("N", 5);
  gather_data.set_int_array("idx", {5, 2, 2, 1, 4});
  expect_extrema_fixture_compiled("tests/fixtures/gq_extrema_gather.tmir.sexp",
                                  gather_data, "gathered extrema");
}

void test_compiled_gq_reductions() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gq_reductions.tmir.sexp");
  const DataMap data = reduction_data();
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL reductions fixture did not compile completely: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }

  // The same fixture pins both legacy seams in log_prob: data-only prod is
  // folded rather than becoming a runtime opcode, and pure-data int
  // sum still compiles/evaluates outside the runtime GQ specialization.
  int log_products = 0;
  for (const Op& op : cm.graph.ops)
    if (op.opcode == OP_PROD_VEC) ++log_products;
  if (log_products != 0) {
    ++failures;
    std::printf("FAIL data-only log_prob prod used OP_PROD_VEC\n");
  }
  Executor log_ex(std::move(cm.graph));
  cm.bind(log_ex);
  log_ex.params_data()[0] = 0.25;
  for (int i = 0; i < 5; ++i) log_ex.params_data()[1 + i] = 0.5;
  if (!std::isfinite(log_ex.forward())) {
    ++failures;
    std::printf("FAIL legacy data prod/int sum log_prob evaluation\n");
  }

  int product_ops = 0, rng_ops = 0, sum_ops = 0;
  for (const Op& op : cm.write_array->graph.ops) {
    product_ops += op.opcode == OP_PROD_VEC;
    rng_ops += op.opcode == OP_RNG;
    sum_ops += op.opcode == OP_SUM_VEC;
  }
  if (product_ops != 1 || rng_ops != 2 || sum_ops != 1) {
    ++failures;
    std::printf("FAIL reductions op census: prod=%d rng=%d sum=%d\n",
                product_ops, rng_ops, sum_ops);
  }

  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  const Op* product_op = nullptr;
  for (const Op& op : graph.graph().ops)
    if (op.opcode == OP_PROD_VEC) product_op = &op;
  if (!product_op || graph.graph().slots[product_op->in[0]].offset != 1) {
    ++failures;
    std::printf("FAIL reductions product input is not the pinned odd offset\n");
  }
  graph.params_data()[0] = 0.25;
  for (int i = 0; i < 5; ++i) graph.params_data()[1 + i] = 0.5;
  const auto graph_row = [&](WaRng& rng) {
    graph.run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& column : cm.write_array->columns) {
      const double* values = graph.value_ptr(column.slot);
      for (int64_t i = 0; i < column.len; ++i)
        row.push_back(values[column.storage_index(i)]);
    }
    return row;
  };

  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  WaInterp interpreted(program, reduction_base(data));
  std::map<std::string, DataMap::Entry> params;
  params["pad"].r = {0.25};
  params["x"].r = std::vector<double>(5, 0.5);
  params["x"].dims = {5};

  WaRng graph_rng(2026), interp_rng(2026);
  const std::vector<double> got = graph_row(graph_rng);
  const std::vector<double> want = interpreted.eval(params, interp_rng);
  const auto graph_next = graph_rng.gen()();
  const auto interp_next = interp_rng.gen()();
  if (got != want || graph_next != interp_next) {
    ++failures;
    std::printf("FAIL reductions graph/interpreter row or next RNG state\n");
  }
  const std::vector<std::string> names =
      CompiledModel::csv_names(cm.write_array->columns);
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == "pr" && reduction_bits(got[i]) != reduction_bits(0.03125)) {
      ++failures;
      std::printf("FAIL reductions product output\n");
    }
    if (names[i] == "total" && (got[i] < 1.0 || got[i] > 3.0)) {
      ++failures;
      std::printf("FAIL reductions integer sum output %.17g\n", got[i]);
    }
    if (names[i].rfind("partial_matrix.", 0) == 0) {
      const double want =
          names[i] == "partial_matrix.1.1"
              ? 7.0
              : static_cast<double>(std::numeric_limits<int>::min());
      if (got[i] != want) {
        ++failures;
        std::printf("FAIL nested int-array sentinel for %s\n",
                    names[i].c_str());
      }
    }
  }

  // Consecutive rows consume exactly the same stream on both paths too.
  WaRng graph_stream(91), interp_stream(91);
  const std::vector<double> g1 = graph_row(graph_stream);
  const std::vector<double> i1 = interpreted.eval(params, interp_stream);
  const std::vector<double> g2 = graph_row(graph_stream);
  const std::vector<double> i2 = interpreted.eval(params, interp_stream);
  if (g1 != i1 || g2 != i2) {
    ++failures;
    std::printf("FAIL reductions consecutive RNG stream rows\n");
  }
}

static bool replace_reduction_after(std::string& text, size_t after,
                                    const std::string& from,
                                    const std::string& to, const char* what) {
  const size_t at = text.find(from, after);
  if (at == std::string::npos) {
    ++failures;
    std::printf("FAIL reduction mutation cannot find %s\n", what);
    return false;
  }
  text.replace(at, from.size(), to);
  return true;
}

static void expect_reduction_interp(const std::string& text,
                                    const std::string& reason, const char* what,
                                    bool sum_prefix = false) {
  using namespace stanli;
  CompiledModel cm;
  try {
    cm = compile_model(text, reduction_data());
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: mutation did not parse/compile: %s\n", what,
                e.what());
    return;
  }
  const bool fallback = cm.write_array && cm.write_array->interp &&
                        (reason.empty() || cm.write_array->truncated.find(
                                               reason) != std::string::npos);
  if (!fallback) {
    ++failures;
    std::printf(
        "FAIL %s: got %s\n", what,
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }
  if (sum_prefix) {
    int sums = 0;
    for (const Op& op : cm.write_array->graph.ops)
      if (op.opcode == OP_SUM_VEC) ++sums;
    if (sums != 1) {
      ++failures;
      std::printf("FAIL %s: runtime sum was not retained in graph prefix\n",
                  what);
    }
  }
}

static void expect_reduction_compiled(const std::string& text,
                                      const stanli::DataMap& data,
                                      const char* what) {
  using namespace stanli;
  try {
    CompiledModel cm = compile_model(text, data);
    if (cm.write_array && !cm.write_array->interp &&
        cm.write_array->truncated.empty())
      return;
    ++failures;
    std::printf(
        "FAIL %s: got %s\n", what,
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: did not compile: %s\n", what, e.what());
  }
}

static std::vector<double> eval_reduction_interp(
    const std::string& text, std::vector<std::string>* names) {
  using namespace stanli;
  const DataMap data = reduction_data();
  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  WaInterp interp(program, reduction_base(data));
  std::map<std::string, DataMap::Entry> params;
  params["pad"].r = {0.25};
  params["x"].r = std::vector<double>(5, 0.5);
  params["x"].dims = {5};
  WaRng rng(44);
  std::vector<double> row = interp.eval(params, rng);
  *names = CompiledModel::csv_names(interp.columns());
  return row;
}

void test_gq_reduction_lowering_guards() {
  using namespace stanli;
  const std::string base = slurp("tests/fixtures/gq_reductions.tmir.sexp");

  // Generated optimized MIR for the exact corpus surface:
  // prod(rep_vector(1,T) - Transpose__(Indexed(Var p,i))).  Its matrix-row
  // operand deliberately selects the scalar product variant.
  const std::string udf_fixture = slurp("tests/fixtures/gq_prod_udf.tmir.sexp");
  {
    DataMap no_data;
    CompiledModel surface = compile_model(udf_fixture, no_data);
    int products = 0, packet_products = 0, scalar_products = 0;
    if (surface.write_array)
      for (const Op& op : surface.write_array->graph.ops) {
        if (op.opcode != OP_PROD_VEC) continue;
        ++products;
        packet_products += op.variant == 0;
        scalar_products += op.variant == 1;
      }
    if (!surface.write_array || surface.write_array->interp || products != 3 ||
        packet_products != 2 || scalar_products != 1) {
      ++failures;
      std::printf(
          "FAIL exact product surfaces did not select packet/scalar "
          "variants\n");
    } else {
      // Execute all three admitted syntactic surfaces through both engines.
      // In particular, `prod(1 - x')` must be classified Packet rather than
      // silently emitted as variant 0 from a Legacy classifier result.
      Executor graph(std::move(surface.write_array->graph));
      surface.write_array->bind(graph);
      const std::vector<double> x = {0.5, -0.25, 2.0, -1.0,
                                     std::nextafter(1.0, 0.0)};
      const std::vector<double> p = {0.25, 0.11, 0.25, 0.37, 0.25,
                                     0.43, 0.25, 0.59, 0.25, 0.73};
      std::copy(x.begin(), x.end(), graph.params_data());
      std::copy(p.begin(), p.end(), graph.params_data() + x.size());
      graph.run_forward_only();
      std::vector<double> graph_row;
      for (const auto& column : surface.write_array->columns) {
        const double* values = graph.value_ptr(column.slot);
        for (int64_t i = 0; i < column.len; ++i)
          graph_row.push_back(values[column.storage_index(i)]);
      }

      auto program = std::make_shared<mir::Program>(
          mir::read_program(sexp::parse(udf_fixture)));
      std::map<std::string, DataMap::Entry> base_env;
      for (const char* flag :
           {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
        DataMap::Entry one;
        one.is_int = true;
        one.i = {1};
        one.r = {1.0};
        base_env[flag] = one;
      }
      WaInterp interp(program, std::move(base_env));
      std::map<std::string, DataMap::Entry> params;
      params["x"].r = x;
      params["x"].dims = {5};
      params["p"].r = p;
      params["p"].dims = {2, 5};
      WaRng rng(1234);
      const std::vector<double> interp_row = interp.eval(params, rng);
      if (graph_row.size() != interp_row.size()) {
        ++failures;
        std::printf("FAIL exact product surface row widths differ\n");
      } else {
        for (size_t i = 0; i < graph_row.size(); ++i)
          if (reduction_bits(graph_row[i]) != reduction_bits(interp_row[i])) {
            ++failures;
            std::printf(
                "FAIL exact product surface graph/interpreter bits at %zu\n",
                i);
            break;
          }
      }
    }
  }

  // A row of a matrix-valued expression need not have the matrix variable's
  // col-major stride (transpose(M) is the simplest counterexample).  Only an
  // Indexed bare matrix variable belongs to the audited scalar surface.
  std::string matrix_expression = udf_fixture;
  const size_t matrix_wa =
      matrix_expression.find("(Assignment ((LVariable target_surface)");
  const std::string matrix_var = "(pattern (Var p))";
  const std::string transposed_matrix =
      "(pattern\n"
      "                       (FunApp (StanLib Transpose__ FnPlain AoS)\n"
      "                        (((pattern (Var p))\n"
      "                          (meta ((type_ UMatrix) (loc <opaque>) "
      "(adlevel DataOnly)))))))";
  if (matrix_wa == std::string::npos ||
      !replace_reduction_after(matrix_expression, matrix_wa, matrix_var,
                               transposed_matrix,
                               "matrix-expression product base")) {
    ++failures;
    std::printf("FAIL product fixture has no indexed matrix variable\n");
  } else {
    DataMap no_data;
    CompiledModel cm = compile_model(matrix_expression, no_data);
    if (!cm.write_array || !cm.write_array->interp) {
      ++failures;
      std::printf("FAIL matrix-expression product base did not fall back\n");
    }
  }

  // O1 normally inlines generated-quantities UDF calls.  Restore the call
  // at its retained function definition to pin the lower_call_udf guard: a
  // formal vector may be bound to a shifted Eigen Block, so dynamic prod in
  // a UDF must remain on WaInterp even when the caller's syntax is a Var.
  std::string dynamic_udf = udf_fixture;
  const size_t udf_wa = dynamic_udf.find("(generate_quantities");
  const std::string stan_prod = "(FunApp (StanLib prod FnPlain AoS)";
  const size_t inlined_prod = dynamic_udf.find(stan_prod, udf_wa);
  if (udf_wa == std::string::npos || inlined_prod == std::string::npos) {
    ++failures;
    std::printf("FAIL generated UDF product fixture has no inlined call\n");
  } else {
    dynamic_udf.replace(inlined_prod, stan_prod.size(),
                        "(FunApp (UserDefined vector_product FnPlain)");
    expect_reduction_compiled(dynamic_udf, reduction_data(),
                              "dynamic UDF product");
  }

  const size_t assignment = base.find("(Assignment ((LVariable pr)");
  const size_t prod_call = base.find("(FunApp (StanLib prod", assignment);
  const std::string vector_node =
      "((pattern (Var x)) (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const std::string vector_meta =
      "(meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))";
  if (assignment == std::string::npos || prod_call == std::string::npos ||
      base.find(vector_node, prod_call) == std::string::npos) {
    ++failures;
    std::printf("FAIL reductions guard fixture has no dynamic product\n");
    return;
  }

  struct BadInput {
    const char* type;
    const char* label;
  };
  const BadInput bad_inputs[] = {
      {"UReal", "scalar"},
      {"UMatrix", "matrix"},
      {"(UArray UReal)", "array"},
      {"(UArray (UArray UReal))", "nested array"},
  };
  for (const BadInput& bad : bad_inputs) {
    std::string mutated = base;
    const std::string replacement = std::string("(meta ((type_ ") + bad.type +
                                    ") (loc <opaque>) (adlevel DataOnly)))";
    if (!replace_reduction_after(mutated, prod_call, vector_meta, replacement,
                                 bad.label))
      continue;
    expect_reduction_compiled(
        mutated, reduction_data(),
        (std::string("product ") + bad.label + " uses lowered layout").c_str());
  }

  std::string container_result = base;
  const std::string real_result =
      "(meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))";
  const std::string vector_result =
      "(meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))";
  if (replace_reduction_after(container_result, prod_call, real_result,
                              vector_result, "container product result"))
    expect_reduction_interp(container_result, "scalar-real result",
                            "product container result stays interpreted");

  std::string two_args = base;
  const size_t one_arg = two_args.find(vector_node, prod_call);
  if (one_arg == std::string::npos) {
    ++failures;
    std::printf("FAIL reduction mutation cannot find product arity\n");
  } else {
    two_args.insert(one_arg + vector_node.size(), " " + vector_node);
    expect_reduction_interp(two_args, "scalar-real result",
                            "two-argument product stays interpreted");
  }

  const std::string empty_vector =
      "((pattern\n"
      "            (FunApp (StanLib rep_vector FnPlain AoS)\n"
      "             (((pattern (Var pad))\n"
      "               (meta ((type_ UReal) (loc <opaque>) (adlevel "
      "DataOnly))))\n"
      "              ((pattern (Lit Int 0))\n"
      "               (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly)))))))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string empty = base;
  if (replace_reduction_after(empty, prod_call, vector_node, empty_vector,
                              "empty product"))
    expect_reduction_interp(empty, "",
                            "empty dynamic product stays interpreted");

  const std::string segment =
      "((pattern\n"
      "            (FunApp (StanLib segment FnPlain AoS)\n"
      "             (((pattern (Var x))\n"
      "               (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))\n"
      "              ((pattern (Lit Int 2))\n"
      "               (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly))))\n"
      "              ((pattern (Lit Int 3))\n"
      "               (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly)))))))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string direct_view = base;
  if (replace_reduction_after(direct_view, prod_call, vector_node, segment,
                              "segment product"))
    expect_reduction_compiled(direct_view, reduction_data(), "segment product");

  const std::string transposed_segment =
      "((pattern\n"
      "            (FunApp (StanLib Transpose__ FnPlain AoS)\n"
      "             (" +
      segment +
      ")))\n"
      "           (meta ((type_ URowVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string wrapped_view = base;
  if (replace_reduction_after(wrapped_view, prod_call, vector_node,
                              transposed_segment, "transposed segment product"))
    expect_reduction_compiled(wrapped_view, reduction_data(),
                              "transposed segment product");

  // A transpose of a bare vector is an address-zero row-vector view in
  // CmdStan and remains in scope for the native path.
  const std::string transposed_bare =
      "((pattern\n"
      "            (FunApp (StanLib Transpose__ FnPlain AoS)\n"
      "             (" +
      vector_node +
      ")))\n"
      "           (meta ((type_ URowVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string row_vector = base;
  if (replace_reduction_after(row_vector, prod_call, vector_node,
                              transposed_bare, "row-vector product")) {
    CompiledModel cm = compile_model(row_vector, reduction_data());
    int products = 0;
    if (cm.write_array)
      for (const Op& op : cm.write_array->graph.ops)
        products += op.opcode == OP_PROD_VEC;
    if (!cm.write_array || cm.write_array->interp || products != 1) {
      ++failures;
      std::printf("FAIL bare row-vector product did not compile\n");
    }
  }

  const std::string exp_vector =
      "((pattern\n"
      "            (FunApp (StanLib exp FnPlain AoS)\n"
      "             (" +
      vector_node +
      ")))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string exp_product = base;
  if (replace_reduction_after(exp_product, prod_call, vector_node, exp_vector,
                              "exp product"))
    expect_reduction_compiled(exp_product, reduction_data(), "prod(exp(x))");

  const std::string scalar_one =
      "((pattern (Lit Real 1.0))\n"
      "              (meta ((type_ UReal) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const std::string safe_minus =
      "((pattern\n"
      "            (FunApp (StanLib Minus__ FnPlain AoS)\n"
      "             (" +
      scalar_one + " " + vector_node +
      ")))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string minus_product = base;
  if (replace_reduction_after(minus_product, prod_call, vector_node, safe_minus,
                              "audited subtraction product")) {
    CompiledModel cm = compile_model(minus_product, reduction_data());
    int products = 0;
    if (cm.write_array)
      for (const Op& op : cm.write_array->graph.ops)
        products += op.opcode == OP_PROD_VEC;
    if (!cm.write_array || cm.write_array->interp || products != 1) {
      ++failures;
      std::printf("FAIL audited outer-subtraction product did not compile\n");
    }
  }

  // The same outer subtraction with a transpose-of-bare-vector operand is
  // also packet-grouped.  This exact form previously passed the lowering
  // whitelist while the shared classifier returned Legacy.
  const std::string safe_transposed_minus =
      "((pattern\n"
      "            (FunApp (StanLib Minus__ FnPlain AoS)\n"
      "             (" +
      scalar_one + " " + transposed_bare +
      ")))\n"
      "           (meta ((type_ URowVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string transposed_minus_product = base;
  if (replace_reduction_after(transposed_minus_product, prod_call, vector_node,
                              safe_transposed_minus,
                              "transposed subtraction product")) {
    CompiledModel cm =
        compile_model(transposed_minus_product, reduction_data());
    int products = 0, packet_products = 0;
    if (cm.write_array)
      for (const Op& op : cm.write_array->graph.ops) {
        if (op.opcode != OP_PROD_VEC) continue;
        ++products;
        packet_products += op.variant == 0;
      }
    if (!cm.write_array || cm.write_array->interp || products != 1 ||
        packet_products != 1) {
      ++failures;
      std::printf(
          "FAIL transposed outer-subtraction product did not compile as "
          "packet\n");
    }
  }

  // Eigen's IndexedView for a gather clears PacketAccess. The layout carried
  // by the materialized graph value therefore selects scalar grouping.
  const std::string gather_vector =
      "((pattern\n"
      "            (Indexed\n"
      "             " +
      vector_node +
      "\n"
      "              ((MultiIndex\n"
      "               ((pattern (Var counts))\n"
      "                (meta ((type_ (UArray UInt)) (loc <opaque>) "
      "(adlevel DataOnly))))))))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const std::string gather_minus =
      "((pattern\n"
      "            (FunApp (StanLib Minus__ FnPlain AoS)\n"
      "             (" +
      scalar_one + " " + gather_vector +
      ")))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string gathered_product = base;
  if (replace_reduction_after(gathered_product, prod_call, vector_node,
                              gather_minus, "gather product"))
    expect_reduction_compiled(gathered_product, reduction_data(),
                              "prod(1-x[idx])");

  // The bare gathered vector uses the same scalar grouping.
  std::string bare_gathered_product = base;
  if (replace_reduction_after(bare_gathered_product, prod_call, vector_node,
                              gather_vector, "bare gather product"))
    expect_reduction_compiled(bare_gathered_product, reduction_data(),
                              "prod(x[idx])");

  const std::string unsafe_minus =
      "((pattern\n"
      "            (FunApp (StanLib Minus__ FnPlain AoS)\n"
      "             (" +
      scalar_one + " " + exp_vector +
      ")))\n"
      "           (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string nested_math = base;
  if (replace_reduction_after(nested_math, prod_call, vector_node, unsafe_minus,
                              "nested vector math product"))
    expect_reduction_compiled(nested_math, reduction_data(), "prod(1-exp(x))");

  // A dynamic product in log_prob uses the same opcode with stored partials.
  std::string reverse = base;
  const size_t log_prod = reverse.find("(FunApp (StanLib prod");
  const std::string data_node =
      "((pattern (Var d))\n"
      "               (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const std::string active_node =
      "((pattern (Var x))\n"
      "               (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "AutoDiffable))))";
  if (replace_reduction_after(reverse, log_prod, data_node, active_node,
                              "reverse-mode product")) {
    bool compiled = false;
    try {
      CompiledModel model = compile_model(reverse, reduction_data());
      compiled =
          std::any_of(model.graph.ops.begin(), model.graph.ops.end(),
                      [](const Op& op) { return op.opcode == OP_PROD_VEC; });
    } catch (const CompileError&) {
    }
    if (!compiled) {
      ++failures;
      std::printf("FAIL dynamic log_prob product was not compiled\n");
    }
  }

  const size_t sum_call = base.find("(FunApp (StanLib sum", prod_call);
  const std::string sum_arg =
      "((pattern (Var z))\n"
      "           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel "
      "DataOnly))))";
  if (sum_call == std::string::npos ||
      base.find(sum_arg, sum_call) == std::string::npos) {
    ++failures;
    std::printf("FAIL reductions guard fixture has no runtime int sum\n");
    return;
  }

  std::string nested_sum = base;
  const std::string nested_arg =
      "((pattern (Var z))\n"
      "           (meta ((type_ (UArray (UArray UInt))) (loc <opaque>) "
      "(adlevel DataOnly))))";
  if (replace_reduction_after(nested_sum, sum_call, sum_arg, nested_arg,
                              "nested int sum"))
    expect_reduction_interp(nested_sum, "", "nested int sum fallback");

  std::string unproved = base;
  const size_t first_runtime_rng =
      unproved.find("(FunApp (StanLib bernoulli_rng", prod_call);
  if (first_runtime_rng == std::string::npos) {
    ++failures;
    std::printf("FAIL reductions guard fixture has no Bernoulli source\n");
  } else {
    unproved.replace(first_runtime_rng,
                     std::string("(FunApp (StanLib bernoulli_rng").size(),
                     "(FunApp (StanLib poisson_log_rng");
    expect_reduction_interp(unproved, "unproved integral slot values",
                            "unproved runtime int sum fallback");
  }

  std::string overflow = base;
  const size_t first_z = overflow.find("((LVariable z)", prod_call);
  const size_t index_one = overflow.find("(Lit Int 1)", first_z);
  const size_t value_one = overflow.find("(Lit Int 1)", index_one + 1);
  if (value_one == std::string::npos) {
    ++failures;
    std::printf("FAIL reductions guard cannot find overflow literal\n");
  } else {
    overflow.replace(value_one, std::string("(Lit Int 1)").size(),
                     "(Lit Int 2147483647)");
    expect_reduction_interp(overflow, "overflow int32",
                            "overflowing runtime int sum fallback");
  }

  // Default local ints are INT_MIN sentinels, not zero.  Re-target the z[3]
  // write to z[2], leaving one element definitely uninitialized: a range hull
  // over the written values alone must not authorize the sum.
  std::string partial = base;
  size_t z_assignment = partial.find("((LVariable z)", prod_call);
  for (int occurrence = 1; occurrence < 3 && z_assignment != std::string::npos;
       ++occurrence)
    z_assignment = partial.find("((LVariable z)", z_assignment + 1);
  const size_t index_three = partial.find("(Lit Int 3)", z_assignment);
  if (z_assignment == std::string::npos || index_three == std::string::npos) {
    ++failures;
    std::printf("FAIL reductions guard cannot find z[3] write\n");
  } else {
    partial.replace(index_three, std::string("(Lit Int 3)").size(),
                    "(Lit Int 2)");
    expect_reduction_interp(partial, "not definitely initialized",
                            "partially initialized runtime int sum fallback");
    std::vector<std::string> names;
    const std::vector<double> row = eval_reduction_interp(partial, &names);
    double z_sum = 0.0, total = 0.0;
    bool found_uninitialized = false, found_total = false;
    for (size_t i = 0; i < row.size(); ++i) {
      if (names[i].rfind("z.", 0) == 0) z_sum += row[i];
      if (names[i] == "z.3")
        found_uninitialized =
            row[i] == static_cast<double>(std::numeric_limits<int>::min());
      if (names[i] == "total") {
        found_total = true;
        total = row[i];
      }
    }
    if (!found_uninitialized || !found_total || total != z_sum) {
      ++failures;
      std::printf(
          "FAIL partially initialized interpreter did not preserve the "
          "INT_MIN sentinel\n");
    }
  }

  const std::string empty_slice =
      "((pattern\n"
      "            (Indexed\n"
      "             ((pattern (Var z))\n"
      "              (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel "
      "DataOnly))))\n"
      "             ((Between\n"
      "               ((pattern (Lit Int 1))\n"
      "                (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly))))\n"
      "               ((pattern (Lit Int 0))\n"
      "                (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly))))))))\n"
      "           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel "
      "DataOnly))))";
  std::string empty_sum = base;
  if (replace_reduction_after(empty_sum, sum_call, sum_arg, empty_slice,
                              "empty int sum")) {
    // The empty slice folds to a compile-time zero and the model stays on
    // the graph path; row parity with the interpreter is the guard.
    CompiledModel em;
    bool compiled = true;
    try {
      em = compile_model(empty_sum, reduction_data());
    } catch (const std::exception& e) {
      ++failures;
      compiled = false;
      std::printf("FAIL empty direct int sum: %s\n", e.what());
    }
    if (compiled && (!em.write_array || em.write_array->interp)) {
      ++failures;
      std::printf("FAIL empty direct int sum stayed interpreted\n");
    } else if (compiled) {
      Executor eex(std::move(em.write_array->graph));
      em.write_array->bind(eex);
      eex.params_data()[0] = 0.25;
      for (int i = 0; i < 5; ++i) eex.params_data()[1 + i] = 0.5;
      WaRng erng(2026), irng(2026);
      eex.run_forward_only(EvalState{&erng});
      std::vector<double> got;
      for (const auto& column : em.write_array->columns) {
        const double* values = eex.value_ptr(column.slot);
        for (int64_t i = 0; i < column.len; ++i)
          got.push_back(values[column.storage_index(i)]);
      }
      auto eprogram = std::make_shared<mir::Program>(
          mir::read_program(sexp::parse(empty_sum)));
      WaInterp einterp(eprogram, reduction_base(reduction_data()));
      std::map<std::string, DataMap::Entry> eparams;
      eparams["pad"].r = {0.25};
      eparams["x"].r = std::vector<double>(5, 0.5);
      eparams["x"].dims = {5};
      const std::vector<double> want = einterp.eval(eparams, irng);
      if (got != want) {
        ++failures;
        std::printf("FAIL empty direct int sum graph/interpreter row\n");
      }
    }
  }

  // The generated fixture's reversed 10:0 write proves that arbitrary
  // reversed ranges are harmless empty updates.  Mutate its later 4:5 write
  // to pin both sides of the bounds check and a width mismatch; every case
  // must fail closed to WaInterp, which then reports the same invalid write.
  // z[4:5] is now spelled Upfrom(4); restore the explicit Between(4, 5).
  std::string range_base = base;
  {
    const std::string upfrom =
        "((Upfrom\n"
        "         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) "
        "(adlevel DataOnly))))))";
    const std::string between =
        "((Between\n"
        "         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) "
        "(adlevel DataOnly))))\n"
        "         ((pattern (Lit Int 5)) (meta ((type_ UInt) (loc <opaque>) "
        "(adlevel DataOnly))))))";
    const size_t at = range_base.find(upfrom);
    if (at == std::string::npos) {
      ++failures;
      std::printf("FAIL reductions guard cannot find the z Upfrom write\n");
    } else {
      range_base.replace(at, upfrom.size(), between);
    }
  }
  const size_t tail_range = range_base.rfind("((Between");
  const size_t tail_lo = range_base.find("(Lit Int 4)", tail_range);
  const size_t tail_hi = range_base.find("(Lit Int 5)", tail_lo);
  if (tail_range == std::string::npos || tail_lo == std::string::npos ||
      tail_hi == std::string::npos) {
    ++failures;
    std::printf("FAIL reductions guard cannot find the 4:5 range write\n");
  } else {
    const auto expect_invalid_range = [&](long lo, long hi, const char* reason,
                                          const char* what) {
      std::string mutated = range_base;
      mutated.replace(tail_hi, std::string("(Lit Int 5)").size(),
                      "(Lit Int " + std::to_string(hi) + ")");
      mutated.replace(tail_lo, std::string("(Lit Int 4)").size(),
                      "(Lit Int " + std::to_string(lo) + ")");
      expect_reduction_interp(mutated, reason, what);
      try {
        std::vector<std::string> names;
        (void)eval_reduction_interp(mutated, &names);
        ++failures;
        std::printf("FAIL %s: interpreter accepted invalid range\n", what);
      } catch (const std::exception&) {
      }
    };
    expect_invalid_range(0, 1, "out of bounds", "low OOB range assignment");
    expect_invalid_range(5, 6, "out of bounds", "high OOB range assignment");
    expect_invalid_range(4, 4, "size mismatch",
                         "range assignment RHS width mismatch");

    // A single range index on a 2-D value denotes rows, not a contiguous
    // flat slice. The shared index geometry lowers row-range writes now, so
    // this 4:5 selection is refused by the row bounds check (the base has
    // two rows) and the model falls closed to WaInterp as before.
    std::string matrix_rows = range_base;
    const size_t lhs_z = matrix_rows.rfind("(LVariable z)", tail_range);
    if (lhs_z == std::string::npos) {
      ++failures;
      std::printf("FAIL reductions guard cannot find range target\n");
    } else {
      matrix_rows.replace(lhs_z, std::string("(LVariable z)").size(),
                          "(LVariable partial_matrix)");
      expect_reduction_interp(matrix_rows, "out of bounds",
                              "matrix row-range assignment stays interpreted");
    }
  }
}

void test_runtime_int_sum_is_not_compile_time() {
  const std::string base = slurp("tests/fixtures/gq_reductions.tmir.sexp");
  const size_t sum_call = base.find(
      "(FunApp (StanLib sum", base.find("(Assignment ((LVariable total)"));
  const size_t insertion = base.find("((pattern\n     (NRFunApp", sum_call);
  if (sum_call == std::string::npos || insertion == std::string::npos) {
    ++failures;
    std::printf("FAIL reductions fixture has no post-sum insertion point\n");
    return;
  }

  const std::string runtime_index =
      "((pattern\n"
      "     (Assignment ((LVariable pr) ()) UReal\n"
      "      ((pattern\n"
      "        (Indexed\n"
      "         ((pattern (Var x))\n"
      "          (meta ((type_ UVector) (loc <opaque>) (adlevel "
      "DataOnly))))\n"
      "         ((Single\n"
      "           ((pattern (Var total))\n"
      "            (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly))))))))\n"
      "       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))\n"
      "    (meta <opaque>))\n   ";
  std::string index = base;
  index.insert(insertion, runtime_index);
  {
    const stanli::CompiledModel cm =
        stanli::compile_model(index, reduction_data());
    int dynamic_slices = 0;
    if (cm.write_array)
      for (const stanli::Op& op : cm.write_array->graph.ops)
        dynamic_slices += op.opcode == stanli::OP_DYNAMIC_SLICE;
    if (!cm.write_array || cm.write_array->interp || dynamic_slices != 1) {
      ++failures;
      std::printf("FAIL runtime sum did not provide a dynamic index\n");
    }
  }

  const std::string runtime_shape =
      "((pattern\n"
      "     (Decl (decl_adtype DataOnly) (decl_id shaped)\n"
      "      (decl_type\n"
      "       (Sized\n"
      "        (SVector AoS\n"
      "         ((pattern (Var total))\n"
      "          (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly)))))))\n"
      "      (initialize Default)))\n"
      "    (meta <opaque>))\n   ";
  std::string shape = base;
  shape.insert(insertion, runtime_shape);
  expect_reduction_interp(shape, "unknown int total",
                          "runtime sum cannot provide a shape", true);

  const std::string runtime_loop =
      "((pattern\n"
      "     (For (loopvar i)\n"
      "      (lower\n"
      "       ((pattern (Lit Int 1))\n"
      "        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))\n"
      "      (upper\n"
      "       ((pattern (Var total))\n"
      "        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))\n"
      "      (body\n"
      "       ((pattern (Block (((pattern Skip) (meta <opaque>)))))\n"
      "        (meta <opaque>)))))\n"
      "    (meta <opaque>))\n   ";
  std::string loop = base;
  loop.insert(insertion, runtime_loop);
  expect_reduction_interp(loop, "unknown int total",
                          "runtime sum cannot provide a loop bound", true);

  const std::string runtime_condition =
      "((pattern\n"
      "     (IfElse\n"
      "      ((pattern (Var total))\n"
      "       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))\n"
      "      ((pattern (Block (((pattern Skip) (meta <opaque>)))))\n"
      "       (meta <opaque>))\n"
      "      ()))\n"
      "    (meta <opaque>))\n   ";
  std::string condition = base;
  condition.insert(insertion, runtime_condition);
  expect_reduction_interp(condition, "runtime-control region produces nothing",
                          "runtime sum cannot provide a condition", true);
}

void test_runtime_int_sum_redeclaration_shadowing() {
  using namespace stanli;
  const std::string base = slurp("tests/fixtures/gq_reductions.tmir.sexp");
  const size_t sum = base.find("(FunApp (StanLib sum",
                               base.find("(Assignment ((LVariable total)"));
  const size_t insertion = base.find("((pattern\n     (NRFunApp", sum);
  if (sum == std::string::npos || insertion == std::string::npos) {
    ++failures;
    std::printf(
        "FAIL reductions fixture has no post-sum redeclaration "
        "point\n");
    return;
  }

  const std::string literal_decl =
      "((pattern\n"
      "     (Decl (decl_adtype DataOnly) (decl_id total)\n"
      "      (decl_type (Sized SInt))\n"
      "      (initialize\n"
      "       (Assign\n"
      "        ((pattern (Lit Int 2))\n"
      "         (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly))))))))\n"
      "    (meta <opaque>))\n   ";
  std::string literal = base;
  literal.insert(insertion, literal_decl);
  CompiledModel cm = compile_model(literal, reduction_data());
  if (!cm.write_array || cm.write_array->interp) {
    ++failures;
    std::printf(
        "FAIL same-id literal int redeclaration did not compile: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
  } else {
    Executor graph(std::move(cm.write_array->graph));
    cm.write_array->bind(graph);
    graph.params_data()[0] = 0.25;
    for (int i = 0; i < 5; ++i) graph.params_data()[1 + i] = 0.5;
    WaRng rng(44);
    graph.run_forward_only(EvalState{&rng});
    const std::vector<std::string> names =
        CompiledModel::csv_names(cm.write_array->columns);
    bool found = false;
    size_t at = 0;
    for (const auto& column : cm.write_array->columns) {
      const double* values = graph.value_ptr(column.slot);
      for (int64_t i = 0; i < column.len; ++i, ++at) {
        if (names[at] != "total") continue;
        found = true;
        if (reduction_bits(values[column.storage_index(i)]) !=
            reduction_bits(2.0)) {
          ++failures;
          std::printf("FAIL stale runtime sum shadowed later literal int\n");
        }
      }
    }
    if (!found) {
      ++failures;
      std::printf("FAIL same-id literal test has no total column\n");
    }
  }

  const std::string uninitialized_decl =
      "((pattern\n"
      "     (Decl (decl_adtype DataOnly) (decl_id total)\n"
      "      (decl_type (Sized SInt)) (initialize Uninit)))\n"
      "    (meta <opaque>))\n   ";
  std::string uninitialized = base;
  uninitialized.insert(insertion, uninitialized_decl);
  expect_reduction_interp(uninitialized, "unknown variable total",
                          "same-id uninitialized int fails closed", true);
  std::vector<std::string> names;
  const std::vector<double> row = eval_reduction_interp(uninitialized, &names);
  bool found_sentinel = false;
  for (size_t i = 0; i < row.size(); ++i)
    if (names[i] == "total" &&
        row[i] == static_cast<double>(std::numeric_limits<int>::min()))
      found_sentinel = true;
  if (!found_sentinel) {
    ++failures;
    std::printf(
        "FAIL uninitialized scalar interpreter did not preserve the "
        "INT_MIN sentinel\n");
  }
}

void test_runtime_control_write_array() {
  using namespace stanli;

  // The structural kernel selects fixed-width outer-array elements and
  // scatters their adjoints back to the chosen block only.
  {
    const Kernel* k = find_kernel(OP_DYNAMIC_SLICE);
    double base[6] = {10, 11, 20, 21, 30, 31};
    double index = 2.0, out[2] = {0, 0};
    double base_adj[6] = {0, 0, 0, 0, 0, 0};
    double out_adj[2] = {3, 5};
    const int idata[1] = {3};
    KernelCtx ctx;
    ctx.n_in = 2;
    ctx.in[0] = Desc{base, 6};
    ctx.in[1] = Desc{&index, 1};
    ctx.out = Desc{out, 2};
    ctx.idata = idata;
    ctx.n_idata = 1;
    k->forward(ctx);
    if (out[0] != 20 || out[1] != 21) {
      ++failures;
      std::printf("FAIL dynamic slice selected the wrong block\n");
    }
    ctx.in_adj[0] = Desc{base_adj, 6};
    ctx.out_adj_vec = Desc{out_adj, 2};
    k->backward(ctx);
    const double want_adj[6] = {0, 0, 3, 5, 0, 0};
    if (!std::equal(std::begin(base_adj), std::end(base_adj), want_adj)) {
      ++failures;
      std::printf("FAIL dynamic slice scattered to the wrong block\n");
    }
    for (double bad :
         {0.0, 1.5, 4.0, std::numeric_limits<double>::quiet_NaN()}) {
      index = bad;
      bool threw = false;
      try {
        k->forward(ctx);
      } catch (const std::out_of_range&) {
        threw = true;
      }
      if (!threw) {
        ++failures;
        std::printf("FAIL dynamic slice accepted invalid index %.17g\n", bad);
      }
    }
    KernelCtx malformed = ctx;
    malformed.in[0].len = 5;
    bool malformed_threw = false;
    try {
      k->forward(malformed);
    } catch (const std::logic_error&) {
      malformed_threw = true;
    }
    if (!malformed_threw) {
      ++failures;
      std::printf("FAIL dynamic slice accepted malformed geometry\n");
    }
  }

  const std::string text = slurp("tests/fixtures/gq_runtime_control.tmir.sexp");
  CompiledModel cm = compile_model(text, DataMap{});
  if (!cm.write_array || cm.write_array->interp ||
      !cm.write_array->truncated.empty()) {
    ++failures;
    std::printf(
        "FAIL runtime-control write_array did not compile: %s\n",
        cm.write_array ? cm.write_array->truncated.c_str() : "no write_array");
    return;
  }
  int dynamic_slices = 0, control_regions = 0, concatenations = 0;
  for (const Op& op : cm.write_array->graph.ops) {
    dynamic_slices += op.opcode == OP_DYNAMIC_SLICE;
    control_regions += op.opcode == OP_ISLAND;
    concatenations += op.opcode == OP_CONCAT2;
    if (op.opcode == OP_ISLAND && op.n_in != 6) {
      ++failures;
      std::printf("FAIL packed runtime-control island has %d inputs\n",
                  op.n_in);
    }
  }
  if (dynamic_slices != 2 || control_regions != 1 || concatenations != 2) {
    ++failures;
    std::printf(
        "FAIL runtime-control op census: dynamic=%d regions=%d concat=%d\n",
        dynamic_slices, control_regions, concatenations);
  }

  Executor graph(cm.write_array->graph);
  cm.write_array->bind(graph);
  Executor params(cm.graph);
  cm.bind(params);
  const std::vector<double> q = {
      0.2,           -0.3,          0.1,  -0.2, 1.0,  2.0,  3.0,  4.0,
      std::log(1.1), std::log(1.7), 0.25, -0.5, 0.75, -1.0, 1.25, -1.5};
  if (graph.n_params() != (int64_t)q.size() ||
      params.n_params() != (int64_t)q.size()) {
    ++failures;
    std::printf("FAIL runtime-control unconstrained width\n");
    return;
  }
  std::copy(q.begin(), q.end(), graph.params_data());
  std::copy(q.begin(), q.end(), params.params_data());
  params.run_forward_only();

  std::map<std::string, DataMap::Entry> base;
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    base[flag] = one;
  }
  auto program =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(text)));
  WaInterp interpreted(program, std::move(base));
  const auto env = cm.constrained_env(params);
  const auto graph_row = [&](WaRng& rng) {
    graph.run_forward_only(EvalState{&rng});
    std::vector<double> row;
    for (const auto& column : cm.write_array->columns) {
      const double* values = graph.value_ptr(column.slot);
      for (int64_t i = 0; i < column.len; ++i)
        row.push_back(values[column.storage_index(i)]);
    }
    return row;
  };

  WaRng graph_rng(81), interp_rng(81);
  const std::vector<double> graph_first = graph_row(graph_rng);
  const std::vector<double> interp_first = interpreted.eval(env, interp_rng);
  const std::vector<double> graph_second = graph_row(graph_rng);
  const std::vector<double> interp_second = interpreted.eval(env, interp_rng);
  const auto graph_next = graph_rng.gen()();
  const auto interp_next = interp_rng.gen()();
  if (!same_double_bytes(graph_first, interp_first) ||
      !same_double_bytes(graph_second, interp_second) ||
      graph_next != interp_next) {
    ++failures;
    std::printf("FAIL runtime-control row/stream parity\n");
  }
}

int main() {
  test_naming_rules();
  test_wanames_pipeline();
  test_wanames_interpreter_schema();
  test_array_of_matrix_columns();
  test_gq_bare_fill_is_nan();
  test_gq_name_shadowing();
  test_interpreted_gq();
  test_compiled_extra_rng();
  test_compiled_region_rng();
  test_region_rng_is_not_folded();
  test_interpreted_gq_densities();
  test_interpreted_gq_gp_covariances();
  test_compiled_multiply_lower_tri();
  test_constant_folded_gq_column();
  test_binomial_rng_helper_contract();
  test_categorical_rng_helper_contract();
  test_multi_normal_rng_helper_contract();
  test_categorical_rng_lowering_guards();
  test_compiled_categorical_rng();
  test_categorical_rng_empty_vector();
  test_categorical_rng_dynamic_index();
  test_multi_normal_rng_kernel_contract();
  test_multi_normal_rng_lowering_guards();
  test_compiled_multi_normal_rng();
  test_binomial_rng_lowering_guards();
  test_compiled_scalar_rng();
  test_product_exact_grouping();
  test_reduction_view_grouping();
  test_layout_materialization_boundaries();
  test_main_index_layout_metadata();
  test_matrix_transpose_extrema_fallback();
  test_extrema_exact_grouping();
  test_compiled_gq_extrema();
  test_gq_extrema_lowering_guards();
  test_gq_extrema_view_lowering_guards();
  test_compiled_gq_reductions();
  test_gq_reduction_lowering_guards();
  test_runtime_int_sum_is_not_compile_time();
  test_runtime_int_sum_redeclaration_shadowing();
  test_runtime_control_write_array();
  test_stan_rng_stream_contract();
  test_caller_owned_rng();
  test_transformed_parameter_checks();
  if (failures == 0) std::printf("test_write_array OK\n");
  return failures == 0 ? 0 : 1;
}
