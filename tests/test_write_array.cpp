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
#include "env_helpers.hpp"
#include "portable_mir_writer.hpp"
#include "stdout_capture.hpp"

#include <stanli/mir_decode.hpp>
#include <stanli/compile.hpp>
#include <stanli/execution_report.hpp>
#include <stanli/graph.hpp>
#include <stanli/graph_print.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>
#include <stan/model/indexing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

// A WaInterp base environment: the listed data entries plus both emit flags.
std::map<std::string, stanli::DataMap::Entry> emit_env(
    const stanli::DataMap& data = stanli::DataMap(),
    std::initializer_list<const char*> keys = {}) {
  std::map<std::string, stanli::DataMap::Entry> env;
  for (const char* key : keys) env[key] = data.at(key);
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

std::map<std::string, stanli::DataMap::Entry> bound_check_env(
    const stanli::DataMap& data) {
  return emit_env(
      data, {"d", "raw", "N", "M", "lo", "R", "C", "BR", "BC", "matrix_lo"});
}

std::vector<double> read_row(
    const std::vector<stanli::CompiledModel::ParamView>& columns,
    const stanli::Executor& ex) {
  std::vector<double> row;
  for (const auto& c : columns) {
    const double* p = ex.value_ptr(c.slot);
    for (int64_t i = 0; i < c.len; ++i) row.push_back(p[c.storage_index(i)]);
  }
  return row;
}

bool compiled_completely(const stanli::CompiledModel& cm,
                         const std::string& what) {
  if (cm.write_array && !cm.write_array->interp && !cm.write_array->truncated)
    return true;
  ++failures;
  std::printf("FAIL %s did not compile completely: %s\n", what.c_str(),
              cm.write_array ? cm.write_array->truncated.value_or("").c_str()
                             : "no write_array");
  return false;
}

// The decoder must refuse the mutated MIR with `reason` in its message. Many
// malformed shapes that lowering guards against never get past the portable
// decoder's checks.
bool expect_rejected(const std::string& text, const stanli::DataMap& data,
                     const std::string& reason, const std::string& what) {
  try {
    (void)stanli::compile_model(text, data);
  } catch (const std::exception& e) {
    if (std::string(e.what()).find(reason) != std::string::npos) return true;
    ++failures;
    std::printf("FAIL %s: rejected for another reason: %s\n", what.c_str(),
                e.what());
    return false;
  }
  ++failures;
  std::printf("FAIL %s: the mutation compiled\n", what.c_str());
  return false;
}

// The mutated MIR must compile, and write_array must fall back to the
// interpreter with `reason` (when non-empty) in its truncation note.
bool expect_interpreted(const std::string& text, const stanli::DataMap& data,
                        const std::string& reason, const std::string& what,
                        stanli::CompiledModel* out = nullptr) {
  stanli::CompiledModel cm;
  try {
    cm = stanli::compile_model(text, data);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: mutation did not parse/compile: %s\n", what.c_str(),
                e.what());
    return false;
  }
  if (!cm.write_array || !cm.write_array->interp ||
      (!reason.empty() && cm.write_array->truncated.value_or("").find(reason) ==
                              std::string::npos)) {
    ++failures;
    std::printf("FAIL %s: got %s\n", what.c_str(),
                cm.write_array ? cm.write_array->truncated.value_or("").c_str()
                               : "no write_array");
    return false;
  }
  if (out) *out = std::move(cm);
  return true;
}

void expect_compiled(const std::string& text, const stanli::DataMap& data,
                     const std::string& what) {
  try {
    const stanli::CompiledModel cm = stanli::compile_model(text, data);
    if (cm.write_array && !cm.write_array->interp && !cm.write_array->truncated)
      return;
    ++failures;
    std::printf("FAIL %s: got %s\n", what.c_str(),
                cm.write_array ? cm.write_array->truncated.value_or("").c_str()
                               : "no write_array");
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL %s: did not compile: %s\n", what.c_str(), e.what());
  }
}

bool same_double_bytes(const std::vector<double>& a,
                       const std::vector<double>& b) {
  return a.size() == b.size() &&
         (a.empty() ||
          std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

// Evaluates write_array at q on the graph and on its forced interpreter with
// equal seeds; the rows must match bitwise. A build without developer
// switches attaches no interpreter, and then only the graph runs. Returns the
// graph row.
std::vector<double> cross_checked_row(const stanli::CompiledModel& cm,
                                      stanli::Executor& pex,
                                      stanli::Executor& wex,
                                      const std::vector<double>& q,
                                      unsigned seed, const std::string& what) {
  std::copy(q.begin(), q.end(), pex.params_data());
  pex.run_forward_only();
  std::copy(q.begin(), q.end(), wex.params_data());
  stanli::WaRng graph_rng(seed);
  wex.run_forward_only(stanli::EvalState{&graph_rng});
  std::vector<double> row = read_row(cm.write_array->columns, wex);
  if (!cm.write_array->interp) {
    if (debug_switches_available()) {
      ++failures;
      std::printf("FAIL %s: no interpreter attached\n", what.c_str());
    } else {
      std::printf("SKIP %s interpreter comparison: needs developer switches\n",
                  what.c_str());
    }
    return row;
  }
  stanli::WaRng interp_rng(seed);
  if (!same_double_bytes(row, cm.write_array->interp->eval(
                                  cm.constrained_env(pex), interp_rng))) {
    ++failures;
    std::printf("FAIL %s: graph and interpreter rows differ\n", what.c_str());
  }
  return row;
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
  CompiledModel cm = compile_model(slurp("tests/fixtures/wanames.mir"), data);
  if (!cm.write_array || cm.write_array->columns.empty()) {
    ++failures;
    std::printf("FAIL wanames: no write_array\n");
    return;
  }
  expect_eq("wanames truncated", cm.write_array->truncated.value_or(""), "");
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
  auto prog = std::make_shared<mir::Program>(
      stanli::decode_program(slurp("tests/fixtures/wanames.mir")));

  std::map<std::string, DataMap::Entry> base = emit_env();
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

// CmdStan's write_array adds `target +=` to an accumulator that starts at 0,
// and target() reads it back: the generated C++ calls
// f_lp<propto__>(lp__, lp_accum__, pstream__) and then
// t = get_lp(lp__, lp_accum__). The interpreter used to refuse the statement.
void test_interpreter_target_increment() {
  using namespace stanli;
  auto prog = std::make_shared<mir::Program>(
      stanli::decode_program(slurp("tests/fixtures/wa_interp_target.mir")));
  WaInterp wi(prog, emit_env());
  std::map<std::string, DataMap::Entry> params;
  params["p"].r = {0.25};
  WaRng rng(1234);
  std::vector<double> row;
  try {
    row = wi.eval(params, rng);
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL interpreted target increment threw: %s\n", e.what());
    return;
  }
  expect_eq("interpreted target increment header", joined(wi.columns()), "p,t");
  if (row != std::vector<double>{0.25, 0.5}) {
    ++failures;
    std::printf("FAIL interpreted target increment row\n");
  }
}

// A bare real container is NaN in every element the block never assigns,
// which is what CmdStan writes and what the interpreter already produced.
void test_gq_bare_fill_is_nan() {
  using namespace stanli;
  DataMap data;
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/gqbarefill.mir"), data);
  if (!cm.write_array || cm.write_array->columns.empty()) {
    ++failures;
    std::printf("FAIL gqbarefill: no write_array\n");
    return;
  }
  expect_eq("gqbarefill truncated", cm.write_array->truncated.value_or(""), "");
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
  CompiledModel cm = compile_model(slurp("tests/fixtures/gqshadow.mir"), data);
  if (!cm.write_array || cm.write_array->columns.empty()) {
    ++failures;
    std::printf("FAIL gqshadow: no write_array\n");
    return;
  }
  expect_eq("gqshadow truncated", cm.write_array->truncated.value_or(""), "");
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
  auto prog = std::make_shared<mir::Program>(
      stanli::decode_program(slurp("tests/fixtures/gqshadow.mir")));
  std::map<std::string, DataMap::Entry> base = emit_env();
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

// CmdStan's write_array runs `target +=` into its own accumulator, which
// starts at zero, and skips `jacobian +=`. target() inside an _lp function
// reads that accumulator. Here t = 0.5 + 0.375 + 1 and u = 0.875 + 0.5 + 1.
void test_write_array_target_lp() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/wa_target_lp.mir");
  const std::vector<double> want{0.25, 1.875, 2.375};
  auto expect_row = [&](const char* what, const std::vector<double>& got) {
    if (got == want) return;
    ++failures;
    std::printf("FAIL wa_target_lp %s row\n", what);
    for (size_t i = 0; i < std::max(got.size(), want.size()); ++i)
      std::printf("  [%zu] got %.17g want %.17g\n", i,
                  i < got.size() ? got[i] : NAN,
                  i < want.size() ? want[i] : NAN);
  };

  CompiledModel cm = compile_model(text, DataMap());
  if (compiled_completely(cm, "wa_target_lp")) {
    Executor wex(std::move(cm.write_array->graph));
    cm.write_array->bind(wex);
    wex.params_data()[0] = 0.25;
    wex.run_forward_only();
    expect_row("compiled", read_row(cm.write_array->columns, wex));
  }

  auto prog = std::make_shared<mir::Program>(stanli::decode_program(text));
  WaInterp interpreted(prog, emit_env());
  std::map<std::string, DataMap::Entry> params;
  params["x"].r = {0.25};
  WaRng rng(1);
  try {
    expect_row("interpreted", interpreted.eval(params, rng));
  } catch (const std::exception& e) {
    ++failures;
    std::printf("FAIL wa_target_lp interpreted threw: %s\n", e.what());
  }
}

// The `~` statement adds a target term to the write_array graph before the
// `||` on a parameter. That `||` used to refuse, and the rollback had to
// remove the term with the slots it named: a stale term made CSE write past
// its slot tables, which crashed fuzz seeds 530 and 4962. The `||` now
// lowers, so the graph gives the row.
void test_write_array_tp_lp_refused() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/wa_tp_lp_refused.mir");
  CompiledModel cm = compile_model(text, DataMap());
  if (!compiled_completely(cm, "wa_tp_lp_refused")) return;
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  for (const double p : {0.25, 1.0, 3.0}) {
    wex.params_data()[0] = p;
    wex.run_forward_only();
    const double outside = p < 0.5 || p > 2;
    if (read_row(cm.write_array->columns, wex) !=
        std::vector<double>{p, outside, outside}) {
      ++failures;
      std::printf("FAIL wa_tp_lp_refused: row at p = %g\n", p);
    }
  }
}

// WaInterp remains the independent oracle for caller-owned RNG and runtime
// control now that the same section can also compile completely.
void test_interpreted_gq() {
  using namespace stanli;
  DataMap data;
  data.set_int("N", 5);
  const std::string text = slurp("tests/fixtures/gqrng.mir");
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "gqrng")) return;
  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base = emit_env(data, {"N"});
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
  const std::string text = slurp("tests/fixtures/gqrng_extra.mir");
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "gqrng_extra")) return;
  expect_eq("gqrng_extra columns", joined(cm.write_array->columns),
            "sigma,g,d.1,d.2,d.3,bb");

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  wex.params_data()[0] = std::log(1.3);  // sigma's unconstrained log_sd form
  const auto graph_row = [&](WaRng& rng) {
    wex.run_forward_only(EvalState{&rng});
    return read_row(cm.write_array->columns, wex);
  };

  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base = emit_env(data, {"N"});
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
  const std::string text = slurp("tests/fixtures/gqrngregion.mir");
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "gqrngregion")) return;
  expect_eq("gqrngregion columns", joined(cm.write_array->columns),
            "sigma,e,n,d.1,d.2,d.3,mn.1,mn.2,mn.3,i");

  const double sigma = 1.4;
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  wex.params_data()[0] = std::log(sigma);  // sigma's unconstrained form
  WaRng graph_rng(11);
  wex.run_forward_only(EvalState{&graph_rng});
  std::vector<double> row = read_row(cm.write_array->columns, wex);

  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base = emit_env(data, {"K"});
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
  const std::string text = slurp("tests/fixtures/gqrngfold.mir");
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "gqrngfold")) return;
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  WaRng rng(5);
  const auto draw_row = [&]() {
    wex.run_forward_only(EvalState{&rng});
    return read_row(cm.write_array->columns, wex);
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
  const std::string text = slurp("tests/fixtures/gqdensity.mir");
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || !cm.write_array->interp) {
    ++failures;
    std::printf("FAIL gqdensity: expected an attached interpreter\n");
    return;
  }
  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base =
      emit_env(data, {"K", "counts", "y", "Sigma"});
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
  const std::string text = slurp("tests/fixtures/gpmatern.mir");
  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base = emit_env();
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
  const std::string text = slurp("tests/fixtures/mlt.mir");
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "mlt")) return;
  // Column-major A with a non-zero upper triangle that must be ignored.
  const double a[9] = {1.0, 0.4, -0.2, 5.0, 2.0, 0.7, 9.0, 8.0, 3.0};

  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  std::copy(a, a + 9, wex.params_data());
  WaRng graph_rng(3);
  wex.run_forward_only(EvalState{&graph_rng});
  std::vector<double> row = read_row(cm.write_array->columns, wex);

  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base = emit_env(data, {"M"});
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
  CompiledModel cm = compile_model(slurp("tests/fixtures/amatwa.mir"), data);
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
      stanli::decode_program(slurp("tests/fixtures/amatwa.mir")));
  std::map<std::string, DataMap::Entry> base = emit_env(data, {"d"});
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
      stanli::decode_program(slurp("tests/fixtures/gqrng.mir")));
  std::map<std::string, DataMap::Entry> base = emit_env(data, {"N"});
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

void test_transformed_parameter_checks() {
  using namespace stanli;
  const std::string mir = slurp("tests/fixtures/data_and_tp_checks.mir");
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

  auto prog = std::make_shared<mir::Program>(stanli::decode_program(mir));
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

namespace {
const stanli::StructuredLoop* find_retained_loop(const stanli::Graph& g) {
  for (const auto& op : g.ops)
    if (op.opcode == stanli::OP_LOOP)
      return static_cast<const stanli::StructuredLoop*>(op.udata);
  return nullptr;
}
}  // namespace

// write_array retains the same data-bounded loop log_prob retains as
// OP_LOOP, and its rows match the forced interpreter at three points.
void expect_retained_write_array_loop(const std::string& stem,
                                      const stanli::DataMap& data) {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/" + stem + ".mir");

  CompiledModel cm = compile_model(text, data);
  if (find_retained_loop(cm.graph) == nullptr) {
    ++failures;
    std::printf(
        "FAIL %s: log_prob did not retain the transformed-parameter loop\n",
        stem.c_str());
  }
  if (!cm.write_array) {
    ++failures;
    std::printf("FAIL %s: no write_array\n", stem.c_str());
    return;
  }
  if (cm.write_array->truncated.has_value()) {
    ++failures;
    std::printf("FAIL %s write_array truncated: %s\n", stem.c_str(),
                cm.write_array->truncated.value_or("").c_str());
  }
  if (find_retained_loop(cm.write_array->graph) == nullptr) {
    ++failures;
    std::printf(
        "FAIL %s: write_array did not lower the loop as a retained region\n",
        stem.c_str());
  }

  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  CompiledModel cross = compile_model(text, data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!cross.write_array || !cross.write_array->interp) {
    ++failures;
    std::printf("FAIL %s: no interpreter attached for cross-check\n",
                stem.c_str());
    return;
  }

  Executor pex(cross.graph);
  cross.bind(pex);
  Executor wex(std::move(cross.write_array->graph));
  cross.write_array->bind(wex);

  std::vector<std::vector<double>> graph_rows;
  for (double mu : {1.3, -0.7, 0.05})
    graph_rows.push_back(cross_checked_row(
        cross, pex, wex, {mu}, 7, stem + " at mu=" + std::to_string(mu)));
  if (graph_rows[0] == graph_rows[1] || graph_rows[1] == graph_rows[2]) {
    ++failures;
    std::printf("FAIL %s: draws did not vary\n", stem.c_str());
  }
}

stanli::DataMap alternating_y(int n) {
  stanli::DataMap data;
  data.set_int("N", n);
  std::vector<double> y(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    y[static_cast<size_t>(i)] = (i % 2 == 0) ? 1.5 : -1.5;
  data.set_real_array("y", y);
  return data;
}

// A data-bounded for loop, large enough that one iteration's if/else island
// alone exceeds mir_prog.hpp's register cap, computes a transformed
// parameter vector in the model log_prob already retains as OP_LOOP under
// the default automatic policy. write_array has to retain the same loop
// rather than fall back to the per-draw interpreter.
void test_write_array_retained_loop() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  expect_retained_write_array_loop("structured_wa_region", alternating_y(32));
}

// A loop that carries a value from one iteration to the next is retained in
// write_array once the graph is already large, and unrolled otherwise. Both
// forms produce the interpreter's rows.
// write_array only runs forward, so its executor keeps no adjoint for any
// slot, and a copy of it keeps none either. Asking it for a gradient is an
// error rather than a read of storage it never allocated.
void test_write_array_forward_only() {
  using namespace stanli;
  DataMap data;
  data.set_int("N", 6);
  data.set_int_array("y", {0, 3, 1, 7, 2, 4});
  data.set_real_array("x", {0.1, -0.4, 0.8, 0.3, -1.2, 0.5});
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/wa_scratch.mir"), data);
  if (!compiled_completely(cm, "forward-only write_array")) return;
  Executor wex(cm.write_array->graph);
  cm.write_array->bind(wex);
  const Executor copy(wex);
  if (wex.adjoint_storage_size() != 0 || copy.adjoint_storage_size() != 0) {
    ++failures;
    std::printf("FAIL forward-only write_array keeps %lld adjoints\n",
                (long long)wex.adjoint_storage_size());
  }
  // Nothing runs backward to read an op's scratch after the op returns, so
  // every op shares one region: as large as the largest op needs, plus up to
  // 7 doubles that keep each op's own alignment.
  int64_t largest = 0, total = 0;
  for (const Op& op : wex.graph().ops) {
    const Kernel* k = find_kernel(op.opcode);
    const int64_t need = k && k->scratch_size
                             ? k->scratch_size(op, wex.graph().slots.data())
                             : 0;
    largest = std::max(largest, need);
    total += need;
  }
  const auto shared = [&](const Executor& ex) {
    return ex.scratch_storage_size() >= largest &&
           ex.scratch_storage_size() < largest + 8;
  };
  if (total <= largest + 8 || !shared(wex) || !shared(copy)) {
    ++failures;
    std::printf(
        "FAIL forward-only write_array scratch %lld, largest op %lld, all ops "
        "%lld\n",
        (long long)wex.scratch_storage_size(), (long long)largest,
        (long long)total);
  }
  // A small executor is cheap to copy, so each chain gets its own.
  if (wex.storage_bytes() <= 0 || share_across_chains(wex, 4) ||
      share_across_chains(wex, 1)) {
    ++failures;
    std::printf("FAIL small write_array executor (%lld bytes) is shared\n",
                (long long)wex.storage_bytes());
  }
  bool threw = false;
  try {
    std::vector<double> g(static_cast<size_t>(wex.n_params()));
    wex.gradient(g.data());
  } catch (const std::logic_error&) {
    threw = true;
  }
  if (!threw) {
    ++failures;
    std::printf("FAIL forward-only write_array ran a gradient\n");
  }
}

// A forward-only executor runs its ops in one fixed order, so a slot that
// no later op reads can give its storage to a slot written after it. Columns
// stay where the caller reads them. Rows match an executor that gives every
// slot its own storage.
void test_write_array_reused_values() {
  using namespace stanli;
  DataMap data;
  data.set_int("N", 64);
  data.set_int("T", 8);
  std::vector<double> x(64);
  for (int i = 0; i < 64; ++i) x[(size_t)i] = 0.25 * (i % 5) - 0.5;
  data.set_real_array("x", x);
  CompiledModel cm =
      compile_model(slurp("tests/fixtures/structured_wa_carried.mir"), data);
  if (!compiled_completely(cm, "reused write_array values")) return;
  Graph separate = cm.write_array->graph;
  separate.forward_only = false;
  Executor reused(cm.write_array->graph), own(std::move(separate));
  cm.write_array->bind(reused);
  cm.write_array->bind(own);
  if (reused.mutable_value_size() >= own.mutable_value_size()) {
    ++failures;
    std::printf("FAIL reused write_array values: %lld of %lld doubles\n",
                (long long)reused.mutable_value_size(),
                (long long)own.mutable_value_size());
  }
  for (const double mu : {0.4, -1.1, 2.0}) {
    reused.params_data()[0] = own.params_data()[0] = mu;
    WaRng a(3), b(3);
    reused.run_forward_only(EvalState{&a});
    own.run_forward_only(EvalState{&b});
    if (!same_double_bytes(read_row(cm.write_array->columns, reused),
                           read_row(cm.write_array->columns, own))) {
      ++failures;
      std::printf("FAIL reused write_array values: rows differ at mu=%g\n", mu);
    }
  }
}

void test_write_array_carried_loop() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/structured_wa_carried.mir");
  for (const int t : {8, 20000}) {
    DataMap data;
    const int n = 64;
    data.set_int("N", n);
    data.set_int("T", t);
    std::vector<double> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      x[static_cast<size_t>(i)] = 0.25 * (i % 5) - 0.5;
    data.set_real_array("x", x);
    CompiledModel cm = compile_model(text, data);
    if (!cm.write_array || cm.write_array->truncated.has_value()) {
      ++failures;
      std::printf("FAIL structured_wa_carried T=%d: no compiled write_array\n",
                  t);
      continue;
    }
    const bool retained = find_retained_loop(cm.write_array->graph) != nullptr;
    if (retained != (t > 8)) {
      ++failures;
      std::printf("FAIL structured_wa_carried T=%d: retained=%d\n", t,
                  retained ? 1 : 0);
    }
    test_setenv("STANLI_WA_FORCE_INTERP", "1");
    CompiledModel cross = compile_model(text, data);
    test_unsetenv("STANLI_WA_FORCE_INTERP");
    if (!cross.write_array || !cross.write_array->interp) {
      ++failures;
      std::printf("FAIL structured_wa_carried T=%d: no interpreter\n", t);
      continue;
    }
    Executor pex(cm.graph);
    cm.bind(pex);
    Executor wex(std::move(cm.write_array->graph));
    cm.write_array->bind(wex);
    for (const double mu : {0.4, -1.1, 2.0}) {
      pex.params_data()[0] = mu;
      pex.run_forward_only();
      wex.params_data()[0] = mu;
      WaRng graph_rng(3);
      wex.run_forward_only(EvalState{&graph_rng});
      std::vector<double> row;
      for (const auto& c : cm.write_array->columns) {
        const double* p = wex.value_ptr(c.slot);
        for (int64_t i = 0; i < c.len; ++i)
          row.push_back(p[c.storage_index(i)]);
      }
      WaRng interp_rng(3);
      const std::vector<double> interp_row = cross.write_array->interp->eval(
          cross.constrained_env(pex), interp_rng);
      if (!same_double_bytes(row, interp_row)) {
        ++failures;
        std::printf(
            "FAIL structured_wa_carried T=%d: graph/interp rows differ at "
            "mu=%g\n",
            t, mu);
      }
    }
  }
}

// The transformed-parameter block carries a data-only sibling statement
// (an is_nan check on a value a preceding while loop produced, so it is
// not foldable) beside the 32-plus-trip parameter-branching loop. That
// sibling forced the whole block through one register-program island
// before write_array tried the loop as its own region.
void test_write_array_block_order() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  stanli::DataMap data = alternating_y(32);
  data.set_int("lim", 3);
  data.set_int_array("whenmat", std::vector<int>(40, 1), {8, 5});
  expect_retained_write_array_loop("structured_wa_block_order", data);
}

// normal_rng(vector, vector) and normal_rng(vector, real) both return
// array[] real. The scalar RNG lowering has to broadcast each argument
// (scalar or vector-length) and draw one element at a time from the same
// stream order the interpreter's vectorized fallback uses.
void test_write_array_vector_rng() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  const int k = 4;
  DataMap data;
  data.set_int("K", k);
  const std::string text = slurp("tests/fixtures/gqrng_vector.mir");

  CompiledModel cm = compile_model(text, data);
  compiled_completely(cm, "gqrng_vector");

  test_setenv("STANLI_STRUCTURED_LOOPS", "0");
  CompiledModel cm_no_loops = compile_model(text, data);
  test_unsetenv("STANLI_STRUCTURED_LOOPS");
  compiled_completely(cm_no_loops,
                      "gqrng_vector with STANLI_STRUCTURED_LOOPS=0");

  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  CompiledModel cross = compile_model(text, data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!cross.write_array || !cross.write_array->interp) {
    ++failures;
    std::printf("FAIL gqrng_vector: no interpreter attached for cross-check\n");
    return;
  }

  Executor pex(cross.graph);
  cross.bind(pex);
  Executor wex(std::move(cross.write_array->graph));
  cross.write_array->bind(wex);

  std::vector<std::vector<double>> graph_rows;
  for (int draw = 0; draw < 3; ++draw) {
    std::vector<double> q(static_cast<size_t>(pex.n_params()));
    for (size_t j = 0; j < q.size(); ++j)
      q[j] = 0.2 * static_cast<double>(draw + 1) + 0.1 * static_cast<double>(j);
    graph_rows.push_back(
        cross_checked_row(cross, pex, wex, q, 11 + draw,
                          "gqrng_vector draw " + std::to_string(draw)));
  }
  if (graph_rows[0] == graph_rows[1] || graph_rows[1] == graph_rows[2]) {
    ++failures;
    std::printf("FAIL gqrng_vector: draws did not vary\n");
  }
}

size_t retained_rng_loops(const stanli::Graph& g) {
  size_t loops = 0;
  for (const auto& op : g.ops) {
    if (op.opcode != stanli::OP_LOOP) continue;
    const auto& body =
        static_cast<const stanli::StructuredLoop*>(op.udata)->body.ops;
    loops += std::any_of(body.begin(), body.end(), [](const stanli::Op& o) {
      return o.opcode == stanli::OP_RNG;
    });
  }
  return loops;
}

// A retained loop that draws must leave the stream exactly where the unrolled
// loops leave it. One stream advances through every draw, so a draw that a
// later execution repeated or skipped would shift every later value.
void expect_rng_loop_parity(const std::string& label, int pad,
                            const char* structured, size_t want_loops) {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  const int n = 40, k = 3;
  DataMap data;
  data.set_int("N", n);
  data.set_int("K", k);
  data.set_int("T", pad);
  std::vector<double> x(n);
  std::vector<int> len(n);
  for (int i = 0; i < n; ++i) {
    x[static_cast<size_t>(i)] = 0.3 * std::sin(1.3 * i);
    len[static_cast<size_t>(i)] = 1 + (i * 7) % k;
  }
  data.set_real_array("x", x);
  data.set_int_array("len", len);
  const std::string text = slurp("tests/fixtures/wa_rng_loops.mir");
  if (structured)
    test_setenv("STANLI_STRUCTURED_LOOPS", structured);
  else
    test_unsetenv("STANLI_STRUCTURED_LOOPS");
  CompiledModel retained = compile_model(text, data);
  test_setenv("STANLI_STRUCTURED_LOOPS", "0");
  CompiledModel unrolled = compile_model(text, data);
  test_unsetenv("STANLI_STRUCTURED_LOOPS");
  if (!compiled_completely(retained, label) ||
      !compiled_completely(unrolled, label + " unrolled"))
    return;
  const size_t loops = retained_rng_loops(retained.write_array->graph);
  if (loops != want_loops) {
    ++failures;
    std::printf("FAIL %s: %zu retained loops draw, want %zu\n", label.c_str(),
                loops, want_loops);
  }
  Executor rex(retained.write_array->graph), uex(unrolled.write_array->graph);
  retained.write_array->bind(rex);
  unrolled.write_array->bind(uex);
  WaRng rrng(2026), urng(2026);
  for (int draw = 0; draw < 200; ++draw) {
    const double q[] = {0.8 * std::sin(0.37 * draw), 0.5 * std::cos(draw)};
    for (Executor* ex : {&rex, &uex}) std::copy_n(q, 2, ex->params_data());
    rex.run_forward_only(EvalState{&rrng});
    uex.run_forward_only(EvalState{&urng});
    const auto got = read_row(retained.write_array->columns, rex);
    const auto want = read_row(unrolled.write_array->columns, uex);
    if (!same_double_bytes(got, want)) {
      ++failures;
      std::printf("FAIL %s: draw %d differs from the unrolled loops\n",
                  label.c_str(), draw);
      return;
    }
  }
}

void test_write_array_rng_loops() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  expect_rng_loop_parity("wa_rng_loops prefer", 0, "1", 5);
  expect_rng_loop_parity("wa_rng_loops auto", 20000, nullptr, 5);
}

// Retains `want` carried loops once 20,000 padding loops make write_array
// large, none without them. Every draw matches the unrolled program, and
// every draw after the first replays each retained loop instead of
// recording it again.
void expect_replayed_write_array_loops(const std::string& stem,
                                       std::ptrdiff_t want) {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  const int n = 64;
  const std::string text = slurp("tests/fixtures/" + stem + ".mir");
  for (const int t : {8, 20000}) {
    const std::string label = stem + " T=" + std::to_string(t);
    DataMap data;
    data.set_int("N", n);
    data.set_int("T", t);
    std::vector<double> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      x[static_cast<size_t>(i)] = 0.25 * (i % 5) - 0.5;
    data.set_real_array("x", x);
    CompiledModel retained = compile_model(text, data);
    test_setenv("STANLI_STRUCTURED_LOOPS", "0");
    CompiledModel unrolled = compile_model(text, data);
    test_unsetenv("STANLI_STRUCTURED_LOOPS");
    if (!compiled_completely(retained, label) ||
        !compiled_completely(unrolled, label + " unrolled"))
      continue;
    const auto& ops = retained.write_array->graph.ops;
    const auto loops = std::count_if(ops.begin(), ops.end(), [](const Op& op) {
      return op.opcode == OP_LOOP;
    });
    if (loops != (t > 8 ? want : 0)) {
      ++failures;
      std::printf("FAIL %s: %td retained loops\n", label.c_str(), loops);
    }
    test_setenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS", "1");
    Executor rex(retained.write_array->graph);
    test_unsetenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS");
    Executor uex(unrolled.write_array->graph);
    retained.write_array->bind(rex);
    unrolled.write_array->bind(uex);
    for (int draw = 0; draw < 4; ++draw) {
      const double mu = 0.9 * std::sin(1.3 * draw + 0.2);
      rex.params_data()[0] = uex.params_data()[0] = mu;
      WaRng rrng(9), urng(9);
      stanli_test::StdoutCapture captured(stderr);
      rex.run_forward_only(EvalState{&rrng});
      const std::string diagnostics = captured.finish();
      uex.run_forward_only(EvalState{&urng});
      if (!same_double_bytes(read_row(retained.write_array->columns, rex),
                             read_row(unrolled.write_array->columns, uex))) {
        ++failures;
        std::printf("FAIL %s: draw %d differs from the unrolled loops\n",
                    label.c_str(), draw);
      }
      size_t replays = 0;
      for (size_t at = diagnostics.find("replay=1"); at != std::string::npos;
           at = diagnostics.find("replay=1", at + 1))
        ++replays;
      if (draw > 0 && replays != static_cast<size_t>(loops)) {
        ++failures;
        std::printf("FAIL %s: draw %d replayed %zu of %td loops\n",
                    label.c_str(), draw, replays, loops);
      }
    }
  }
}

// Each carried loop starts from a value that depends only on data (an
// accumulator set to zero, a copy of a data vector, an integer counter, a
// vector with only its first element set) and updates it with a value that
// depends on mu. The import holds the same bits on every draw.
void test_write_array_data_start() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  expect_replayed_write_array_loops("structured_wa_data_start", 4);
}

// Two carried loops branch on data only: on the sign of x[n], and on the
// inner index against the outer one. Their decisions are the same on every
// draw. A third loop branches on mu * x[n] and stays unrolled.
void test_write_array_data_branch() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  expect_replayed_write_array_loops("structured_wa_data_branch", 2);
}

// The first two carried loops count into integers that later declarations use
// as sizes. Retained, a loop produces its counts only at run time, too late to
// lay out those declarations, so write_array is lowered again with both loops
// unrolled. The third loop carries only a real and stays retained.
void test_write_array_counted_size() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  expect_replayed_write_array_loops("structured_wa_counted_size", 1);
}

// An inlined function declares a matrix whose row count multiplies a
// comparison of two fixed integers by a data sum. Inside a retained loop the
// comparison is a graph value, and its range has to be the one value it can
// take for the declaration to have a fixed size.
void test_write_array_compare_size() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  DataMap data;
  data.set_int("N", 40);
  data.set_int("R", 3);
  data.set_int_array("sizes", {2, 1, 3});
  const std::string text = slurp("tests/fixtures/structured_compare_size.mir");
  test_setenv("STANLI_STRUCTURED_LOOPS", "1");
  CompiledModel retained = compile_model(text, data);
  test_setenv("STANLI_STRUCTURED_LOOPS", "0");
  CompiledModel unrolled = compile_model(text, data);
  test_unsetenv("STANLI_STRUCTURED_LOOPS");
  if (!compiled_completely(retained, "structured_compare_size") ||
      !compiled_completely(unrolled, "structured_compare_size unrolled"))
    return;
  const auto& ops = retained.write_array->graph.ops;
  const auto loops = std::count_if(ops.begin(), ops.end(), [](const Op& op) {
    return op.opcode == OP_LOOP;
  });
  if (loops != 1) {
    ++failures;
    std::printf(
        "FAIL structured_compare_size: %td retained loops, want the outer "
        "one only\n",
        loops);
  }
  Executor rex(retained.write_array->graph), uex(unrolled.write_array->graph);
  retained.write_array->bind(rex);
  unrolled.write_array->bind(uex);
  for (const double mu : {-0.7, 0.2, 1.9}) {
    rex.params_data()[0] = uex.params_data()[0] = mu;
    WaRng rrng(5), urng(5);
    rex.run_forward_only(EvalState{&rrng});
    uex.run_forward_only(EvalState{&urng});
    if (!same_double_bytes(read_row(retained.write_array->columns, rex),
                           read_row(unrolled.write_array->columns, uex))) {
      ++failures;
      std::printf("FAIL structured_compare_size: rows differ at mu=%g\n", mu);
    }
  }
}

// Require both selection and repeated whole-row parity. The forced interpreter
// is an oracle alongside the complete graph, never evidence of admission.
void expect_bounded_output(const std::string& fixture,
                           const stanli::DataMap& data, bool compiled,
                           bool early_refusal = false) {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/" + fixture + ".mir");
  ExecutionTrace normal_trace, disabled_trace;
  const auto prepare = [&](ExecutionTrace& trace) {
    ExecutionTraceScope tracing(trace);
    return compile_model(text, data);
  };
  const auto normal = prepare(normal_trace);
  if (!normal.write_array || bool(normal.write_array->interp) == compiled ||
      (compiled && !find_retained_loop(normal.write_array->graph))) {
    ++failures;
    std::printf("FAIL %s bounded selection: expected compiled=%d, %s\n",
                fixture.c_str(), compiled,
                normal.write_array
                    ? normal.write_array->truncated.value_or("").c_str()
                    : "missing outputs");
    return;
  }
  if (!compiled) {
    test_setenv("STANLI_STRUCTURED_LOOPS", "0");
    const auto disabled = prepare(disabled_trace);
    test_unsetenv("STANLI_STRUCTURED_LOOPS");
    if (!disabled.write_array) {
      ++failures;
      std::printf("FAIL %s missing refusal baseline\n", fixture.c_str());
      return;
    }
    const auto describe = [](const CompiledModel::WriteArray& wa) {
      GraphPrintInfo info;
      info.fills = &wa.fills;
      for (const auto& column : wa.columns)
        info.views.emplace_back(column.name, column.slot);
      std::string result;
      print_graph(result, wa.graph, info);
      return result;
    };
    expect_eq(fixture + " refused trial preserves prefix",
              describe(*normal.write_array), describe(*disabled.write_array));
    if (early_refusal) {
      const auto event = std::make_pair(std::string("prepare_data"),
                                        std::string("construction"));
      expect_idx(fixture + " rejects budget before constructing a trial",
                 normal_trace.events[event], disabled_trace.events[event]);
    }
    return;
  }
  if (find_retained_loop(normal.write_array->graph)->cache_execution_path) {
    ++failures;
    std::printf("FAIL %s output block records a reusable execution path\n",
                fixture.c_str());
  }
  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  auto model = compile_model(text, data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  Executor pex(model.graph), wex(model.write_array->graph);
  model.bind(pex);
  model.write_array->bind(wex);
  WaRng graph_rng(1234), interp_rng(1234);
  for (double x : {-0.3, 0.2, 0.0, -0.4, 0.3, -0.3}) {
    for (int64_t i = 0; i < pex.n_params(); ++i)
      pex.params_data()[i] = wex.params_data()[i] = x;
    pex.run_forward_only();
    bool graph_error = false, interp_error = false;
    std::string graph_message, interp_message;
    std::vector<double> actual, expected;
    try {
      wex.run_forward_only(EvalState{&graph_rng});
      for (const auto& col : model.write_array->columns)
        for (int64_t i = 0; i < col.len; ++i)
          actual.push_back(wex.value_ptr(col.slot)[col.storage_index(i)]);
    } catch (const std::exception& e) {
      graph_error = true;
      graph_message = e.what();
    }
    try {
      expected = model.write_array->interp->eval(model.constrained_env(pex),
                                                 interp_rng);
    } catch (const std::exception& e) {
      interp_error = true;
      interp_message = e.what();
    }
    if (graph_error != interp_error ||
        (!graph_error && !same_double_bytes(actual, expected))) {
      ++failures;
      std::printf("FAIL %s bounded row/error parity at x=%g\n", fixture.c_str(),
                  x);
      std::printf("  graph: %s; interpreter: %s\n", graph_message.c_str(),
                  interp_message.c_str());
      for (const char* name : {"mode", "lower_size", "upper_size", "repeats",
                               "shift", "width", "depth"})
        if (data.has(name)) std::printf("  %s=%d", name, data.at(name).i[0]);
      std::printf("\n");
    }
    // Compare continuation state even after an error. An exception must not
    // retry MIR, duplicate the preceding draw, or execute the following draw.
    if (graph_rng.gen()() != interp_rng.gen()()) {
      ++failures;
      std::printf("FAIL %s bounded RNG continuation at x=%g\n", fixture.c_str(),
                  x);
    }
  }
}

void test_write_array_bounded_blocks() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  DataMap partial;
  partial.set_int("K", 3);
  partial.set_int_array("trials", {5, 6, 7});
  expect_bounded_output("gq_partial_fallback", partial, true);
  expect_bounded_output("gq_bounded_many", DataMap{}, true);
  const auto data = [](int mode, int lo, int hi, int repeats = 1) {
    DataMap d;
    d.set_int("mode", mode);
    d.set_int("lower_size", lo);
    d.set_int("upper_size", hi);
    d.set_int("repeats", repeats);
    return d;
  };
  for (int mode = 0; mode <= 4; ++mode)
    expect_bounded_output("gq_bounded_block", data(mode, 1, 2), true);
  expect_bounded_output("gq_bounded_block", data(0, 0, 2), true);
  expect_bounded_output("gq_bounded_block", data(0, 0, 1), true);
  expect_bounded_output("gq_bounded_block", data(0, 1, 8, 3), true);
  // Refuse before capacity/fill allocation or excessive recording. Negative
  // extents keep their runtime validation in MIR.
  expect_bounded_output("gq_bounded_block", data(0, -1, 2), false);
  expect_bounded_output("gq_bounded_block", data(0, 1, 100000000), false);
  expect_bounded_output("gq_bounded_block", data(0, 1, 2, 1000000), false);
  for (const std::string fixture :
       {"gq_bounded_vector", "gq_bounded_row_vector"}) {
    for (int mode = 0; mode <= 5; ++mode)
      expect_bounded_output(fixture, data(mode, 1, 8), true);
    expect_bounded_output(fixture, data(0, 0, 2), true);
    expect_bounded_output(fixture, data(0, 0, 1), true);
    expect_bounded_output(fixture, data(5, 0, 8, 3), true);
    expect_bounded_output(fixture, data(0, -1, 2), false);
    expect_bounded_output(fixture, data(0, 1, 100000000), false);
    expect_bounded_output(fixture, data(0, 1, 2, 1000000), false);
  }
  for (int mode = 0; mode <= 4; ++mode) {
    DataMap d;
    d.set_int("mode", mode);
    // Mode 1, an int array filled in a loop, is a bounded block.
    expect_bounded_output("gq_bounded_refusals", d, mode == 1);
  }
  for (int mode = 0; mode <= 4; ++mode) {
    for (int capacity : {1, 2, 8}) {
      auto d = data(mode, 0, capacity);
      d.set_int("shift", mode == 3 ? 0 : -3);
      expect_bounded_output("gq_bounded_integer", d, true);
    }
  }
  for (int mode : {0, 4}) {
    auto overflow = data(mode, 1, 2);
    for (int shift : {1073741823, INT32_MIN + 1}) {
      overflow.set_int("shift", shift);
      expect_bounded_output("gq_bounded_integer", overflow, false);
    }
  }
  for (int mode = 0; mode <= 9; ++mode) {
    DataMap d;
    d.set_int("mode", mode);
    expect_bounded_output("gq_bounded_integer_refusals", d, mode == 1);
    expect_bounded_output("gq_integer_fill_paths_refusals", d, false);
  }
  for (int mode = 0; mode <= 8; ++mode) {
    for (int capacity : {1, 2, 8, 32}) {
      auto d = data(mode, 0, capacity);
      d.set_int("shift", 3);
      expect_bounded_output("gq_integer_fill_paths", d, true);
    }
  }
  for (int mode : {0, 1}) {
    for (int capacity : {128, 512}) {
      auto d = data(mode, 0, capacity);
      d.set_int("shift", 3);
      expect_bounded_output("gq_integer_fill_paths", d, capacity == 128);
    }
  }
  for (int mode : {0, 1, 2, 3, 4}) {
    auto d = data(mode, 1, 3);
    for (int shift : {1073741824, -1073741825}) {
      d.set_int("shift", shift);
      expect_bounded_output("gq_integer_fill_paths", d, false);
    }
  }
  for (const std::string fixture :
       {"gq_bounded_nested", "gq_bounded_nested3", "gq_bounded_nested5"}) {
    for (int mode = 0; mode <= 7; ++mode) {
      for (int width : {0, 1, 3}) {
        auto d = data(mode, 0, 2);
        d.set_int("width", width);
        d.set_int("depth", 2);
        expect_bounded_output(fixture, d, true);
      }
    }
    for (int depth : {0, 1, 2}) {
      auto d = data(0, 1, 8, 2);
      d.set_int("width", 3);
      d.set_int("depth", depth);
      expect_bounded_output(fixture, d, true);
    }
    for (int mode : {0, 6}) {
      auto d = data(mode, 0, 1);
      d.set_int("width", 0);
      d.set_int("depth", 0);
      expect_bounded_output(fixture, d, true);
    }
    for (int width : {-1, 100000000}) {
      auto d = data(0, 1, 2);
      d.set_int("width", width);
      d.set_int("depth", 2);
      expect_bounded_output(fixture, d, false);
    }
    // Resource refusal must preserve the complete parent prefix too.
    auto over_budget = data(0, 0, 128);
    over_budget.set_int("width", 3);
    over_budget.set_int("depth", 2);
    expect_bounded_output(fixture, over_budget, false, true);
    // Capacity alone is not a refusal: empty loops and empty inner dimensions
    // remove the costly writes. Narrow containers also fit at this length.
    auto large = over_budget;
    large.set_int("repeats", 0);
    expect_bounded_output(fixture, large, true);
    large = over_budget;
    large.set_int("width", 0);
    expect_bounded_output(fixture, large, true);
    large = over_budget;
    large.set_int("width", 1);
    large.set_int("depth", 1);
    expect_bounded_output(fixture, large, true);
    // An iterator-dependent branch needs the normal trial and its rollback.
    large = over_budget;
    large.set_int("mode", 3);
    expect_bounded_output(fixture, large, false);
    auto negative = data(0, -1, 2);
    negative.set_int("width", 3);
    negative.set_int("depth", 2);
    expect_bounded_output(fixture, negative, false);
  }
  for (int mode = 0; mode <= 7; ++mode) {
    DataMap d;
    d.set_int("mode", mode);
    expect_bounded_output("gq_bounded_nested_refusals", d, false);
  }
  for (int mode = 0; mode < 4; ++mode) {
    DataMap d;
    d.set_int("mode", mode);
    expect_bounded_output("gq_bounded_vector_refusals", d, false);
  }
}

// A loop region_auto_profitable selects (32-plus trips, a nested while so
// region_runtime_control sees retained control despite write_array's
// DataOnly-everywhere MIR) whose body calls normal_rng -- a region always
// refuses a stateful call. The refused trial must fall back to one
// whole-loop island rather than per-iteration unrolling: with the fix the
// op count does not grow with N, and it did before it.
void test_write_array_selected_loop_refused() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gq_selected_loop_refused.mir");
  const auto compile_at = [&](int n) {
    DataMap data;
    data.set_int("N", n);
    std::vector<double> y(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      y[static_cast<size_t>(i)] = (i % 2 == 0) ? 1.5 : -1.5;
    data.set_real_array("y", y);
    return compile_model(text, data);
  };
  CompiledModel small = compile_at(32);
  CompiledModel large = compile_at(64);
  if (!small.write_array || !large.write_array) {
    ++failures;
    std::printf("FAIL gq_selected_loop_refused: no write_array\n");
    return;
  }
  if (small.write_array->truncated.has_value()) {
    ++failures;
    std::printf("FAIL gq_selected_loop_refused: truncated at N=32: %s\n",
                small.write_array->truncated.value_or("").c_str());
  }
  const size_t small_ops = small.write_array->graph.ops.size();
  const size_t large_ops = large.write_array->graph.ops.size();
  if (large_ops > small_ops + small_ops / 2) {
    ++failures;
    std::printf(
        "FAIL gq_selected_loop_refused: op count scales with N (N=32 "
        "ops=%zu, N=64 ops=%zu)\n",
        small_ops, large_ops);
  }

  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  CompiledModel cross = compile_at(32);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!cross.write_array || !cross.write_array->interp) {
    ++failures;
    std::printf(
        "FAIL gq_selected_loop_refused: no interpreter attached for "
        "cross-check\n");
    return;
  }
  Executor pex(cross.graph);
  cross.bind(pex);
  Executor wex(std::move(cross.write_array->graph));
  cross.write_array->bind(wex);
  cross_checked_row(cross, pex, wex, {0.4}, 9, "gq_selected_loop_refused");
}

// The same shape, but the refused loop's body also declares a
// register-program-sized local. The whole-loop island fallback then
// overflows too, and write_array has to fall back to the interpreter for
// generated quantities specifically rather than raise a compile error.
// At N=1 the rollback leaves only the parameter's slot, and one of the
// slots it removed held the graph's first 0.0 constant.
void test_write_array_selected_loop_overflow() {
  using namespace stanli;
  const std::string text =
      slurp("tests/fixtures/gq_selected_loop_overflow.mir");
  for (const int n : {32, 1}) {
    const std::string what = "gq_selected_loop_overflow N=" + std::to_string(n);
    DataMap data;
    data.set_int("N", n);
    std::vector<double> y(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
      y[static_cast<size_t>(i)] = (i % 2 == 0) ? 1.5 : -1.5;
    data.set_real_array("y", y);
    CompiledModel cm = compile_model(text, data);
    if (!cm.write_array) {
      ++failures;
      std::printf("FAIL %s: no write_array\n", what.c_str());
      continue;
    }
    if (!cm.write_array->truncated ||
        cm.write_array->truncated.value_or("").find("generated quantities") ==
            std::string::npos) {
      ++failures;
      std::printf("FAIL %s: truncated message is [%s]\n", what.c_str(),
                  cm.write_array->truncated.value_or("").c_str());
    }
    if (!cm.write_array->interp) {
      ++failures;
      std::printf("FAIL %s: no interpreter attached\n", what.c_str());
    }
    // The truncated prefix is still a graph, and its result has to name
    // one of its own slots, holding 0.0.
    const Graph& wg = cm.write_array->graph;
    if (wg.result_slot < 0 || (size_t)wg.result_slot >= wg.slots.size()) {
      ++failures;
      std::printf("FAIL %s: result slot %d of %zu slots\n", what.c_str(),
                  wg.result_slot, wg.slots.size());
      continue;
    }
    for (const auto& [slot, values] : cm.write_array->fills)
      if (slot == wg.result_slot && values != std::vector<double>{0.0}) {
        ++failures;
        std::printf("FAIL %s: result slot %d is not 0.0\n", what.c_str(), slot);
      }
  }
}

// A generated quantity the optimizer folds to a constant: --O1 replaces
// the FnWriteParam's variable reference with the literal value, so the
// column's name has to come from the program's output_vars instead. Both
// engines name it that way; the graph lowers the value like any other
// expression.
void test_constant_folded_gq_column() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  DataMap data;
  data.set_real_array("rectangular", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}, {3, 2});
  const std::string text = slurp("tests/fixtures/gqconst.mir");
  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  CompiledModel cm = compile_model(text, data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!cm.write_array || !cm.write_array->interp ||
      cm.write_array->truncated.has_value()) {
    ++failures;
    std::printf("FAIL gqconst did not compile completely: %s\n",
                cm.write_array ? cm.write_array->truncated.value_or("").c_str()
                               : "no write_array");
    return;
  }
  expect_eq("gqconst header", joined(cm.write_array->columns),
            "x,z,extracted.1,extracted.2");

  Executor pex(cm.graph);
  cm.bind(pex);
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  const std::vector<double> row =
      cross_checked_row(cm, pex, wex, {0.25}, 1, "gqconst");
  if (row.size() != 4 || row[0] != 0.25 || row[1] != 3.0 || row[2] != 1.0 ||
      row[3] != 5.0) {
    ++failures;
    std::printf("FAIL gqconst row: got %zu values\n", row.size());
  }
}

// mean() groups its sum the way Stan Math's Eigen reduction does, so the
// result does not depend on where the slot sits. CmdStan gives lp 51.75 and
// these generated quantities at this data: a strided row adds in order
// (0.25), contiguous data in packets (0.5), and an autodiff value in order.
void test_mean_grouping() {
  using namespace stanli;
  DataMap data = DataMap::from_json_file("tests/fixtures/mean_grouping.json");
  const std::string text = slurp("tests/fixtures/mean_grouping.mir");
  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  CompiledModel cm = compile_model(text, data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!cm.write_array || cm.write_array->truncated.has_value()) {
    ++failures;
    std::printf("FAIL mean_grouping did not compile completely\n");
    return;
  }
  Executor pex(cm.graph);
  cm.bind(pex);
  pex.params_data()[0] = 0.1;
  double grad = 1.0;
  const double lp = pex.gradient(&grad);
  if (lp != 51.75 || grad != 0.0) {
    ++failures;
    std::printf("FAIL mean_grouping lp %.17g grad %.17g\n", lp, grad);
  }
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  const std::vector<double> row =
      cross_checked_row(cm, pex, wex, {0.1}, 1, "mean_grouping");
  const std::vector<double> want{0.5, 0.5, 0.5, 0.25, 0.5};
  if (row.size() != 18) {
    ++failures;
    std::printf("FAIL mean_grouping row: got %zu values\n", row.size());
    return;
  }
  for (size_t k = 0; k < want.size(); ++k)
    if (row[13 + k] != want[k]) {
      ++failures;
      std::printf("FAIL mean_grouping gq %zu: %.17g, CmdStan %.17g\n", k,
                  row[13 + k], want[k]);
    }
}

// A zero-size output before a folded one: its loop writes nothing, so
// naming the folded write "the variable after the last one written" calls
// it z. The writes' widths against output_vars name it w.
void test_constant_folded_gq_after_empty_output() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  DataMap data;
  data.set_int("N", 0);
  const std::string text = slurp("tests/fixtures/gqconst_after_empty.mir");
  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  CompiledModel cm = compile_model(text, data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!cm.write_array || !cm.write_array->interp) {
    ++failures;
    std::printf("FAIL gqconst_after_empty did not compile\n");
    return;
  }
  expect_eq("gqconst_after_empty graph header", joined(cm.write_array->columns),
            "mu,w");
  Executor pex(cm.graph);
  cm.bind(pex);
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  (void)cross_checked_row(cm, pex, wex, {0.25}, 1, "gqconst_after_empty");
  expect_eq("gqconst_after_empty interpreted header",
            joined(cm.write_array->interp->columns()), "mu,w");
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

void test_categorical_rng_lowering_guards() {
  using namespace stanli;
  using portable_writer::calls;
  const mir::Program base =
      decode_program(slurp("tests/fixtures/gq_categorical_rng.mir"));
  // The fixture with its categorical_rng call changed by `change`.
  const auto mutated = [&](const std::function<void(mir::Expr&)>& change) {
    mir::Program program = base;
    mir::Expr* call = portable_writer::find_expr(
        program,
        [](const mir::Expr& e) { return calls(e, "categorical_rng"); });
    if (call == nullptr || call->args.size() != 1)
      throw std::logic_error("categorical lowering guard fixture has no call");
    change(*call);
    return portable_writer::write_v2(program);
  };

  DataMap guard_data;
  guard_data.set_int("K", 4);
  const std::pair<mir::UnsizedView, const char*> bad_arguments[] = {
      {{0, mir::UnsizedLeaf::Real}, "scalar"},
      {{0, mir::UnsizedLeaf::RowVector}, "row-vector"},
      {{1, mir::UnsizedLeaf::Real}, "array"},
      {{1, mir::UnsizedLeaf::Vector}, "array-vector"},
  };
  for (const auto& entry : bad_arguments) {
    const std::string label =
        std::string("categorical ") + entry.second + " input is rejected";
    expect_rejected(
        mutated([&](mir::Expr& call) { call.args[0].unsized = entry.first; }),
        guard_data, "variable type disagrees with its binding for p", label);
  }

  expect_rejected(mutated([](mir::Expr& call) { call.args.clear(); }),
                  guard_data, "expected 1 argument(s), got 0",
                  "zero-argument categorical is rejected");
  expect_rejected(
      mutated([](mir::Expr& call) { call.args.push_back(call.args[0]); }),
      guard_data, "expected 1 argument(s), got 2",
      "two-argument categorical is rejected");
  expect_rejected(mutated([](mir::Expr& call) {
                    call.unsized = {0, mir::UnsizedLeaf::Real};
                  }),
                  guard_data, "assignment type disagrees with its binding",
                  "categorical real result is rejected");
  expect_rejected(mutated([](mir::Expr& call) {
                    call.unsized = {1, mir::UnsizedLeaf::Int};
                  }),
                  guard_data, "assignment type disagrees with its binding",
                  "categorical container result is rejected");

  const std::string logit =
      mutated([](mir::Expr& call) { call.name = "categorical_logit_rng"; });
  DataMap logit_data;
  logit_data.set_int("K", 4);
  CompiledModel logit_cm = compile_model(logit, logit_data);
  if (!compiled_completely(logit_cm, "categorical_logit_rng")) return;

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
  return emit_env(data, {"K"});
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
  const std::string text = slurp("tests/fixtures/gq_categorical_rng.mir");
  const DataMap data = categorical_data(4);
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "categorical rng")) return;

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
    return read_row(cm.write_array->columns, graph);
  };

  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
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
  const std::string text = slurp("tests/fixtures/gq_categorical_rng.mir");
  const DataMap data = categorical_data(0);
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "empty categorical vector")) return;

  Executor graph(std::move(cm.write_array->graph));
  cm.write_array->bind(graph);
  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
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
  const std::string text = slurp("tests/fixtures/gq_categorical_dynamic.mir");
  const DataMap data = categorical_data(3);
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "categorical dynamic index lowering")) return;

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
  std::vector<double> got = read_row(cm.write_array->columns, graph);
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
  return emit_env();
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

void test_multi_normal_rng_lowering_guards() {
  using namespace stanli;
  using portable_writer::calls;
  const mir::Program base =
      decode_program(slurp("tests/fixtures/gq_multi_normal_rng.mir"));
  // The fixture with its multi_normal_rng call, and the declarations of the
  // covariance and of the draw, changed by `change`.
  const auto mutated =
      [&](const std::function<void(mir::Program&, mir::Expr&)>& change) {
        mir::Program program = base;
        mir::Expr* call = portable_writer::find_expr(
            program,
            [](const mir::Expr& e) { return calls(e, "multi_normal_rng"); });
        if (call == nullptr || call->args.size() != 2)
          throw std::logic_error(
              "multi-normal lowering guard fixture has no call");
        change(program, *call);
        return portable_writer::write_v2(program);
      };
  const auto declaration = [](mir::Program& program, const std::string& name) {
    mir::Stmt* decl =
        portable_writer::find_stmt(program, [&](const mir::Stmt& s) {
          return s.kind == mir::Stmt::Decl && s.decl_id == name;
        });
    if (decl == nullptr) throw std::logic_error("no declaration of " + name);
    return decl;
  };
  const auto set_extents = [](mir::Stmt* decl, int64_t extent) {
    for (mir::Expr& dim : decl->decl_type.dims) dim.lit_i = extent;
  };

  const auto expect_interp = [](const std::string& mir,
                                const std::string& reason, const char* label) {
    expect_interpreted(mir, DataMap{}, reason, label);
  };
  const auto expect_reject = [](const std::string& mir,
                                const std::string& reason,
                                const std::string& label) {
    expect_rejected(mir, DataMap{}, reason, label);
  };

  for (const auto& entry :
       std::vector<std::pair<mir::UnsizedView, const char*>>{
           {{0, mir::UnsizedLeaf::Real}, "scalar"},
           {{0, mir::UnsizedLeaf::RowVector}, "row-vector"},
           {{1, mir::UnsizedLeaf::Vector}, "array-vector"}})
    expect_reject(
        mutated([&](mir::Program&, mir::Expr& call) {
          call.args[0].unsized = entry.first;
        }),
        "variable type disagrees with its binding for mu",
        std::string("multi-normal ") + entry.second + " mean is rejected");
  for (const auto& entry :
       std::vector<std::pair<mir::UnsizedView, const char*>>{
           {{0, mir::UnsizedLeaf::Vector}, "vector"},
           {{1, mir::UnsizedLeaf::Matrix}, "array-matrix"}})
    expect_reject(mutated([&](mir::Program&, mir::Expr& call) {
                    call.args[1].unsized = entry.first;
                  }),
                  "variable type disagrees with its binding",
                  std::string("multi-normal ") + entry.second +
                      " covariance is rejected");

  expect_reject(
      mutated([](mir::Program&, mir::Expr& call) { call.args.pop_back(); }),
      "expected 2 argument(s), got 1", "one-argument multi-normal is rejected");
  expect_reject(
      mutated([](mir::Program&, mir::Expr& call) { call.args.clear(); }),
      "expected 2 argument(s), got 0",
      "zero-argument multi-normal is rejected");
  expect_reject(mutated([](mir::Program&, mir::Expr& call) {
                  call.args.push_back(call.args[0]);
                }),
                "expected 2 argument(s), got 3",
                "three-argument multi-normal is rejected");
  for (const auto& entry :
       std::vector<std::pair<mir::UnsizedView, const char*>>{
           {{0, mir::UnsizedLeaf::RowVector}, "row-vector"},
           {{1, mir::UnsizedLeaf::Vector}, "array"}})
    expect_reject(
        mutated([&](mir::Program&, mir::Expr& call) {
          call.unsized = entry.first;
        }),
        "assignment type disagrees with its binding",
        std::string("multi-normal ") + entry.second + " result is rejected");

  expect_reject(
      mutated([](mir::Program&, mir::Expr& call) { call.args[1].name = "mu"; }),
      "variable type disagrees with its binding for mu",
      "multi-normal vector covariance variable is rejected");

  const auto mismatched_covariance = [&](mir::Program& program, mir::Expr&) {
    set_extents(declaration(program, "inline_cov_matrix_2d_covariance2__"), 3);
  };
  expect_interp(mutated(mismatched_covariance),
                "covariance shape must match the location",
                "multi-normal known dimension mismatch stays interpreted");

  {
    CompiledModel cm =
        compile_model(mutated([&](mir::Program& program, mir::Expr&) {
                        set_extents(declaration(program, "draw"), 3);
                      }),
                      DataMap{});
    std::string thrown;
    if (cm.write_array && !cm.write_array->interp) {
      Executor wex(std::move(cm.write_array->graph));
      cm.write_array->bind(wex);
      WaRng rng(1234);
      try {
        wex.run_forward_only(EvalState{&rng});
      } catch (const std::invalid_argument& e) {
        thrown = e.what();
      }
    }
    if (thrown !=
        "vector assign rows: assigning variable draw (3) and right hand side "
        "rows (2) must match in size") {
      ++failures;
      std::printf("FAIL multi-normal result dimension mismatch: got '%s'\n",
                  thrown.c_str());
    }
  }

  auto cholesky_cm = compile_model(mutated([](mir::Program&, mir::Expr& call) {
                                     call.name = "multi_normal_cholesky_rng";
                                   }),
                                   DataMap{});
  compiled_completely(cholesky_cm, "multi-normal Cholesky");
  // A new family must not weaken the shared geometry proof.
  expect_interp(mutated([&](mir::Program& program, mir::Expr& call) {
                  mismatched_covariance(program, call);
                  call.name = "multi_normal_cholesky_rng";
                }),
                "covariance shape must match the location",
                "Cholesky mismatched geometry stays interpreted");
}

void test_compiled_multi_normal_rng() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gq_multi_normal_rng.mir");
  CompiledModel cm = compile_model(text, DataMap{});
  if (!compiled_completely(cm, "multi-normal RNG")) return;
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
    return read_row(cm.write_array->columns, graph);
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
  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
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
  using portable_writer::calls;
  const mir::Program base =
      decode_program(slurp("tests/fixtures/gq_scalar_rng.mir"));
  // The fixture with its binomial_rng call changed by `change`.
  const auto mutated = [&](const std::function<void(mir::Expr&)>& change) {
    mir::Program program = base;
    mir::Expr* call = portable_writer::find_expr(
        program, [](const mir::Expr& e) { return calls(e, "binomial_rng"); });
    if (call == nullptr || call->args.empty() ||
        call->args[0].kind != mir::Expr::LitInt || call->args[0].lit_i != 5)
      throw std::logic_error("binomial lowering guard fixture has no call");
    change(*call);
    return portable_writer::write_v2(program);
  };
  const auto expect_interp = [](const std::string& mir,
                                const std::string& reason, const char* what) {
    expect_interpreted(mir, DataMap{}, reason, what);
  };

  expect_interp(mutated([](mir::Expr& call) {
                  mir::Expr& trials = call.args[0];
                  trials.kind = mir::Expr::LitReal;
                  trials.lit = 5.0;
                  trials.unsized = {0, mir::UnsizedLeaf::Real};
                }),
                "first argument must be int",
                "binomial real trials stay interpreted");
  expect_interp(mutated([](mir::Expr& call) {
                  mir::Expr array;
                  array.kind = mir::Expr::FunApp;
                  array.fn_lib = mir::Expr::Lib::Internal;
                  array.name = "FnMakeArray";
                  array.data_only = true;
                  array.unsized = {1, mir::UnsizedLeaf::Int};
                  array.args = {call.args[0]};
                  call.args[0] = array;
                }),
                "result shape does not match RNG arguments",
                "binomial container argument with scalar result refuses");
  expect_interp(mutated([](mir::Expr& call) {
                  call.unsized = {1, mir::UnsizedLeaf::Int};
                }),
                "result shape does not match RNG arguments",
                "binomial scalar arguments with container result refuses");
}

void test_compiled_scalar_rng() {
  using namespace stanli;
  const std::string path = "tests/fixtures/gq_scalar_rng.mir";
  const std::string text = slurp(path);
  DataMap data;
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "scalar rng")) return;
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
    return read_row(cm.write_array->columns, wex);
  };

  auto prog = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base = emit_env();
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
    return read_row(cm.write_array->columns, clone);
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
  return emit_env(data, {"d", "counts"});
}

void test_layout_materialization_boundaries() {
  using namespace stanli;
  const std::string text =
      slurp("tests/fixtures/gq_layout_materialization.mir");
  DataMap data;
  data.set_int_array("idx", {1, 2, 3, 4, 5});
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "layout materialization fixture")) return;

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

  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base = emit_env(data, {"idx"});
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

  const std::string udf_text = slurp("tests/fixtures/gq_udf_return_layout.mir");
  CompiledModel udf = compile_model(udf_text, data);
  int udf_packet_products = 0;
  if (udf.write_array)
    for (const Op& op : udf.write_array->graph.ops)
      udf_packet_products += op.opcode == OP_PROD_VEC && op.variant == 0;
  if (!udf.write_array || udf.write_array->interp ||
      udf.write_array->truncated.has_value() || udf_packet_products != 1) {
    ++failures;
    std::printf("FAIL container UDF return did not compile as owning: %s\n",
                udf.write_array
                    ? udf.write_array->truncated.value_or("").c_str()
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
  const std::string text = slurp("tests/fixtures/gq_main_index_layout.mir");
  DataMap data;
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "main index layout fixture")) return;

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
      slurp("tests/fixtures/gq_matrix_transpose_extrema.mir");
  DataMap data;
  CompiledModel cm = compile_model(text, data);
  if (!cm.write_array || !cm.write_array->interp ||
      cm.write_array->truncated.value_or("").find("grouping is not native") ==
          std::string::npos) {
    ++failures;
    std::printf("FAIL matrix transpose extrema did not fail closed: %s\n",
                cm.write_array ? cm.write_array->truncated.value_or("").c_str()
                               : "no write_array");
    return;
  }

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<double> values = {0.0, -0.0, 2.0, -3.0, 2.0,
                                      nan, -0.0, 0.0, -4.0, 9.0};
  Eigen::Matrix<double, 5, 2> matrix;
  std::copy(values.begin(), values.end(), matrix.data());
  const double want = stan::math::min(matrix.transpose());

  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  std::map<std::string, DataMap::Entry> base = emit_env();
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

void test_compiled_gq_extrema() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gq_extrema.mir");
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
        cm.write_array->truncated.has_value()) {
      ++failures;
      std::printf("FAIL extrema fixture case %zu did not compile: %s\n", c,
                  cm.write_array
                      ? cm.write_array->truncated.value_or("").c_str()
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
    std::vector<double> graph_row = read_row(cm.write_array->columns, graph);

    auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
    std::map<std::string, DataMap::Entry> base = emit_env(data, {"N", "d"});
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
  expect_interpreted(text, extrema_guard_data(), "", what);
}

void test_gq_extrema_lowering_guards() {
  using namespace stanli;
  using portable_writer::calls;
  const auto min_call = [](const mir::Expr& e) { return calls(e, "min"); };
  const mir::Program base =
      decode_program(slurp("tests/fixtures/gq_extrema.mir"));
  // The fixture with the first min call of generated quantities changed by
  // `change`.
  const auto mutated = [&](const std::function<void(mir::Expr&)>& change) {
    mir::Program program = base;
    mir::Expr* call =
        portable_writer::find_expr(program.generate_quantities, min_call);
    if (call == nullptr || call->args.size() != 1 ||
        call->args[0].kind != mir::Expr::Var || call->args[0].name != "x")
      throw std::logic_error("extrema guard fixture has no direct min call");
    change(*call);
    return portable_writer::write_v2(program);
  };
  const auto literal = [](long value) {
    mir::Expr e;
    e.kind = mir::Expr::LitInt;
    e.lit_i = value;
    e.data_only = true;
    e.unsized = {0, mir::UnsizedLeaf::Int};
    return e;
  };

  for (const auto& entry :
       std::vector<std::pair<mir::UnsizedView, const char*>>{
           {{0, mir::UnsizedLeaf::Real}, "scalar"},
           {{0, mir::UnsizedLeaf::Matrix}, "matrix"},
           {{1, mir::UnsizedLeaf::Real}, "array"}})
    expect_rejected(
        mutated([&](mir::Expr& call) { call.args[0].unsized = entry.first; }),
        extrema_guard_data(), "variable type disagrees with its binding for x",
        std::string("extrema ") + entry.second + " argument is rejected");

  expect_compiled(mutated([](mir::Expr& call) {
                    mir::Expr exp;
                    exp.kind = mir::Expr::FunApp;
                    exp.name = "exp";
                    exp.data_only = true;
                    exp.unsized = call.args[0].unsized;
                    exp.args = {call.args[0]};
                    call.args[0] = exp;
                  }),
                  extrema_guard_data(), "min(exp(x))");
  expect_compiled(mutated([&](mir::Expr& call) {
                    mir::Expr between;
                    between.kind = mir::Expr::IndexBetween;
                    between.args = {literal(2), literal(5)};
                    mir::Expr indexed;
                    indexed.kind = mir::Expr::Indexed;
                    indexed.data_only = true;
                    indexed.unsized = call.args[0].unsized;
                    indexed.args = {call.args[0], between};
                    call.args[0] = indexed;
                  }),
                  extrema_guard_data(), "min(x[2:5])");

  expect_rejected(mutated([](mir::Expr& call) { call.args.clear(); }),
                  extrema_guard_data(), "expected 1 or 2 argument(s), got 0",
                  "zero-argument min is rejected");
  expect_extrema_interp(
      mutated([](mir::Expr& call) { call.args.push_back(call.args[0]); }),
      "two-argument dynamic min stays interpreted");
  expect_rejected(mutated([](mir::Expr& call) {
                    call.unsized = {0, mir::UnsizedLeaf::Vector};
                  }),
                  extrema_guard_data(),
                  "assignment type disagrees with its binding for x_min",
                  "container-result min is rejected");

  // The fixture's compile inlines this UDF call. Restore the retained call
  // so the direct formal reaches lowering at udf_depth > 0 and cannot acquire
  // an owning-vector provenance merely from its Var syntax.
  mir::Program udf = decode_program(slurp("tests/fixtures/gq_extrema_udf.mir"));
  mir::Expr* inlined =
      portable_writer::find_expr(udf.generate_quantities, min_call);
  if (inlined == nullptr) {
    ++failures;
    std::printf("FAIL extrema UDF fixture has no inlined call\n");
  } else {
    inlined->fn_lib = mir::Expr::Lib::UserDefined;
    inlined->name = "vector_min";
    expect_compiled(portable_writer::write_v2(udf), extrema_guard_data(),
                    "dynamic UDF extrema");
  }

  // The same opcode now serves active log_prob reductions through its stored
  // selected coefficient.
  bool reverse_compiled = false;
  try {
    DataMap no_data;
    CompiledModel reverse =
        compile_model(slurp("tests/fixtures/gq_extrema_reverse.mir"), no_data);
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
  expect_compiled(slurp("tests/fixtures/gq_extrema_row.mir"), row_data,
                  "strided matrix-row extrema");

  DataMap gather_data;
  gather_data.set_int("N", 5);
  gather_data.set_int_array("idx", {5, 2, 2, 1, 4});
  expect_compiled(slurp("tests/fixtures/gq_extrema_gather.mir"), gather_data,
                  "gathered extrema");
}

void test_compiled_gq_reductions() {
  using namespace stanli;
  const std::string text = slurp("tests/fixtures/gq_reductions.mir");
  const DataMap data = reduction_data();
  CompiledModel cm = compile_model(text, data);
  if (!compiled_completely(cm, "reductions fixture")) return;

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
    return read_row(cm.write_array->columns, graph);
  };

  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
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
  if (!expect_interpreted(text, reduction_data(), reason, what, &cm)) return;
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

// A runtime statement that writes nothing outside itself compiles to an
// island after the graph's runtime sum, rather than folding the sum.
static void expect_reduction_island(const std::string& text, const char* what) {
  using namespace stanli;
  const CompiledModel cm = compile_model(text, reduction_data());
  int sums = 0, islands = 0;
  if (cm.write_array)
    for (const Op& op : cm.write_array->graph.ops) {
      sums += op.opcode == OP_SUM_VEC;
      islands += op.opcode == OP_ISLAND;
    }
  if (!cm.write_array || cm.write_array->interp || sums != 1 || islands != 1) {
    ++failures;
    std::printf("FAIL %s: sums %d, islands %d\n", what, sums, islands);
  }
}

static std::vector<double> eval_reduction_interp(
    const std::string& text, std::vector<std::string>* names) {
  using namespace stanli;
  const DataMap data = reduction_data();
  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
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
  const std::string base = slurp("tests/fixtures/gq_reductions.mir");

  // Generated optimized MIR for the exact corpus surface:
  // prod(rep_vector(1,T) - Transpose__(Indexed(Var p,i))).  Its matrix-row
  // operand deliberately selects the scalar product variant.
  const std::string udf_fixture = slurp("tests/fixtures/gq_prod_udf.mir");
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
      std::vector<double> graph_row =
          read_row(surface.write_array->columns, graph);

      auto program =
          std::make_shared<mir::Program>(stanli::decode_program(udf_fixture));
      std::map<std::string, DataMap::Entry> base_env = emit_env();
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
  // Here the transposed row has 2 elements against rep_vector(1.0, 5), so
  // the graph throws Stan's subtract size error instead of computing.
  namespace pw = portable_writer;
  using pw::calls;
  const auto vector = pw::view(mir::UnsizedLeaf::Vector);
  const auto row_vector = pw::view(mir::UnsizedLeaf::RowVector);
  const auto is_var = [](const std::string& name) {
    return [name](const mir::Expr& e) {
      return e.kind == mir::Expr::Var && e.name == name;
    };
  };
  const auto assignment_to = [](const std::string& lhs) {
    return [lhs](const mir::Stmt& s) {
      return s.kind == mir::Stmt::Assignment && s.lhs == lhs;
    };
  };
  {
    mir::Program program = decode_program(udf_fixture);
    mir::Stmt* target = pw::find_stmt(program, assignment_to("target_surface"));
    mir::Expr* p = target ? pw::find_expr(*target, is_var("p")) : nullptr;
    if (p == nullptr) {
      ++failures;
      std::printf("FAIL product fixture has no indexed matrix variable\n");
    } else {
      *p = pw::call("Transpose__", p->unsized, {*p});
      DataMap no_data;
      CompiledModel cm = compile_model(pw::write_v2(program), no_data);
      std::string thrown;
      if (cm.write_array && !cm.write_array->interp) {
        Executor wex(std::move(cm.write_array->graph));
        cm.write_array->bind(wex);
        WaRng rng(1234);
        try {
          wex.run_forward_only(EvalState{&rng});
        } catch (const std::invalid_argument& e) {
          thrown = e.what();
        }
      }
      if (thrown != "subtract: m1 (5, 1) and m2 (2, 1) must match in size") {
        ++failures;
        std::printf("FAIL matrix-expression product base: got '%s'\n",
                    thrown.c_str());
      }
    }
  }

  // The fixture's compile inlines generated-quantities UDF calls.  Restore
  // the call at its retained function definition to pin the lower_call_udf
  // guard: a formal vector may be bound to a shifted Eigen Block, so dynamic
  // prod in a UDF must remain on WaInterp even when the caller's syntax is a
  // Var.
  {
    mir::Program program = decode_program(udf_fixture);
    mir::Expr* inlined =
        pw::find_expr(program.generate_quantities,
                      [](const mir::Expr& e) { return calls(e, "prod"); });
    if (inlined == nullptr) {
      ++failures;
      std::printf("FAIL generated UDF product fixture has no inlined call\n");
    } else {
      inlined->fn_lib = mir::Expr::Lib::UserDefined;
      inlined->name = "vector_product";
      expect_compiled(pw::write_v2(program), reduction_data(),
                      "dynamic UDF product");
    }
  }

  const mir::Program base_program = decode_program(base);
  {
    mir::Program program = base_program;
    mir::Stmt* pr = pw::find_stmt(program, assignment_to("pr"));
    mir::Expr* prod =
        pr ? pw::find_expr(*pr,
                           [](const mir::Expr& e) { return calls(e, "prod"); })
           : nullptr;
    if (prod == nullptr || prod->args.size() != 1 ||
        prod->args[0].kind != mir::Expr::Var || prod->args[0].name != "x") {
      ++failures;
      std::printf("FAIL reductions guard fixture has no dynamic product\n");
      return;
    }
  }
  // The fixture with the product of pr = prod(x) changed by `change`.
  const auto with_product = [&](const std::function<void(mir::Expr&)>& change) {
    mir::Program program = base_program;
    mir::Expr* prod =
        pw::find_expr(*pw::find_stmt(program, assignment_to("pr")),
                      [](const mir::Expr& e) { return calls(e, "prod"); });
    change(*prod);
    return pw::write_v2(program);
  };
  // The same with its argument x replaced by `make(x)`.
  const auto product_of =
      [&](const std::function<mir::Expr(const mir::Expr&)>& make) {
        return with_product(
            [&](mir::Expr& prod) { prod.args[0] = make(prod.args[0]); });
      };
  const auto count_products = [](const CompiledModel& cm, int* packets) {
    int products = 0;
    if (cm.write_array)
      for (const Op& op : cm.write_array->graph.ops) {
        if (op.opcode != OP_PROD_VEC) continue;
        ++products;
        if (packets) *packets += op.variant == 0;
      }
    return products;
  };

  const std::pair<mir::UnsizedView, const char*> bad_inputs[] = {
      {pw::view(mir::UnsizedLeaf::Real), "scalar"},
      {pw::view(mir::UnsizedLeaf::Matrix), "matrix"},
      {pw::view(mir::UnsizedLeaf::Real, 1), "array"},
      {pw::view(mir::UnsizedLeaf::Real, 2), "nested array"},
  };
  for (const auto& entry : bad_inputs)
    expect_rejected(
        with_product(
            [&](mir::Expr& prod) { prod.args[0].unsized = entry.first; }),
        reduction_data(), "variable type disagrees with its binding for x",
        std::string("product ") + entry.second + " input is rejected");

  expect_rejected(with_product([&](mir::Expr& prod) { prod.unsized = vector; }),
                  reduction_data(),
                  "assignment type disagrees with its binding",
                  "product container result is rejected");
  expect_rejected(
      with_product([](mir::Expr& prod) { prod.args.push_back(prod.args[0]); }),
      reduction_data(), "expected 1 argument(s), got 2",
      "two-argument product is rejected");

  expect_compiled(
      product_of([&](const mir::Expr&) {
        return pw::call(
            "rep_vector", vector,
            {pw::var("pad", pw::view(mir::UnsizedLeaf::Real)), pw::lit_int(0)});
      }),
      reduction_data(), "empty dynamic product lowers to Stan's 1");

  const auto segment = [&](const mir::Expr& x) {
    return pw::call("segment", vector, {x, pw::lit_int(2), pw::lit_int(3)});
  };
  expect_compiled(product_of(segment), reduction_data(), "segment product");
  expect_compiled(product_of([&](const mir::Expr& x) {
                    return pw::call("Transpose__", row_vector, {segment(x)});
                  }),
                  reduction_data(), "transposed segment product");

  // A transpose of a bare vector is an address-zero row-vector view in
  // CmdStan and remains in scope for the native path.
  const auto transposed = [&](const mir::Expr& x) {
    return pw::call("Transpose__", row_vector, {x});
  };
  {
    CompiledModel cm = compile_model(product_of(transposed), reduction_data());
    if (!cm.write_array || cm.write_array->interp ||
        count_products(cm, nullptr) != 1) {
      ++failures;
      std::printf("FAIL bare row-vector product did not compile\n");
    }
  }

  const auto exp_of = [&](const mir::Expr& x) {
    return pw::call("exp", vector, {x});
  };
  expect_compiled(product_of(exp_of), reduction_data(), "prod(exp(x))");

  const auto one_minus = [&](const mir::Expr& e) {
    return pw::call("Minus__", e.unsized, {pw::lit_real(1.0), e});
  };
  {
    CompiledModel cm = compile_model(product_of(one_minus), reduction_data());
    if (!cm.write_array || cm.write_array->interp ||
        count_products(cm, nullptr) != 1) {
      ++failures;
      std::printf("FAIL audited outer-subtraction product did not compile\n");
    }
  }

  // The same outer subtraction with a transpose-of-bare-vector operand is
  // also packet-grouped.  This exact form previously passed the lowering
  // whitelist while the shared classifier returned Legacy.
  {
    CompiledModel cm = compile_model(product_of([&](const mir::Expr& x) {
                                       return one_minus(transposed(x));
                                     }),
                                     reduction_data());
    int packets = 0;
    if (!cm.write_array || cm.write_array->interp ||
        count_products(cm, &packets) != 1 || packets != 1) {
      ++failures;
      std::printf(
          "FAIL transposed outer-subtraction product did not compile as "
          "packet\n");
    }
  }

  // Eigen's IndexedView for a gather clears PacketAccess. The layout carried
  // by the materialized graph value therefore selects scalar grouping.
  const auto gathered = [&](const mir::Expr& x) {
    return pw::indexed(
        x,
        {pw::index(mir::Expr::IndexMulti,
                   {pw::var("counts", pw::view(mir::UnsizedLeaf::Int, 1))})},
        vector);
  };
  expect_compiled(
      product_of([&](const mir::Expr& x) { return one_minus(gathered(x)); }),
      reduction_data(), "prod(1-x[idx])");
  // The bare gathered vector uses the same scalar grouping.
  expect_compiled(product_of(gathered), reduction_data(), "prod(x[idx])");
  expect_compiled(
      product_of([&](const mir::Expr& x) { return one_minus(exp_of(x)); }),
      reduction_data(), "prod(1-exp(x))");

  // A dynamic product in log_prob uses the same opcode with stored partials.
  {
    mir::Program program = base_program;
    mir::Expr* log_prod = pw::find_expr(
        program.log_prob, [](const mir::Expr& e) { return calls(e, "prod"); });
    bool compiled = false;
    if (log_prod == nullptr || log_prod->args.size() != 1 ||
        log_prod->args[0].name != "d") {
      ++failures;
      std::printf("FAIL reduction mutation cannot find reverse-mode product\n");
    } else {
      log_prod->args[0].name = "x";
      log_prod->args[0].data_only = false;
      try {
        CompiledModel model =
            compile_model(pw::write_v2(program), reduction_data());
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
  }

  // The generated quantities after pr = prod(x), changed by `change`.
  const auto after_product =
      [&](const std::function<void(std::vector<mir::Stmt>&, size_t)>& change) {
        mir::Program program = base_program;
        const auto [body, at] =
            pw::find_position(program.generate_quantities, assignment_to("pr"));
        change(*body, at);
        return pw::write_v2(program);
      };
  // The assignments to z after the product, in order.
  const auto z_writes = [&](std::vector<mir::Stmt>& body, size_t from) {
    std::vector<mir::Stmt*> writes;
    for (size_t i = from; i < body.size(); ++i)
      pw::for_each_stmt(body[i], [&](mir::Stmt& s) {
        if (s.kind == mir::Stmt::Assignment && s.lhs == "z")
          writes.push_back(&s);
      });
    return writes;
  };
  const auto with_sum = [&](const std::function<void(mir::Expr&)>& change) {
    return after_product([&](std::vector<mir::Stmt>& body, size_t at) {
      mir::Stmt* total = nullptr;
      for (size_t i = at; i < body.size() && total == nullptr; ++i)
        total = pw::find_stmt(body[i], assignment_to("total"));
      change(*pw::find_expr(
          *total, [](const mir::Expr& e) { return calls(e, "sum"); }));
    });
  };
  {
    mir::Program program = base_program;
    mir::Stmt* total = pw::find_stmt(program, assignment_to("total"));
    mir::Expr* sum =
        total ? pw::find_expr(
                    *total, [](const mir::Expr& e) { return calls(e, "sum"); })
              : nullptr;
    if (sum == nullptr || sum->args.size() != 1 || sum->args[0].name != "z") {
      ++failures;
      std::printf("FAIL reductions guard fixture has no runtime int sum\n");
      return;
    }
  }

  expect_rejected(with_sum([](mir::Expr& sum) {
                    sum.args[0].unsized = pw::view(mir::UnsizedLeaf::Int, 2);
                  }),
                  reduction_data(),
                  "variable type disagrees with its binding for z",
                  "nested int sum is rejected");

  expect_reduction_interp(
      after_product([&](std::vector<mir::Stmt>& body, size_t at) {
        mir::Expr* rng = nullptr;
        for (size_t i = at; i < body.size() && rng == nullptr; ++i)
          rng = pw::find_expr(body[i], [](const mir::Expr& e) {
            return calls(e, "bernoulli_rng");
          });
        rng->name = "poisson_log_rng";
      }),
      "unproved integral slot values", "unproved runtime int sum fallback");

  expect_reduction_interp(
      after_product([&](std::vector<mir::Stmt>& body, size_t at) {
        z_writes(body, at).at(0)->rhs.lit_i = 2147483647;
      }),
      "overflow int32", "overflowing runtime int sum fallback");

  // Default local ints are INT_MIN sentinels, not zero.  Re-target the z[3]
  // write to z[2], leaving one element definitely uninitialized: a range hull
  // over the written values alone must not authorize the sum.
  {
    const std::string partial =
        after_product([&](std::vector<mir::Stmt>& body, size_t at) {
          z_writes(body, at).at(2)->lhs_idx.at(0).args.at(0).lit_i = 2;
        });
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

  {
    const std::string empty_sum = with_sum([&](mir::Expr& sum) {
      sum.args[0] = pw::indexed(sum.args[0],
                                {pw::index(mir::Expr::IndexBetween,
                                           {pw::lit_int(1), pw::lit_int(0)})},
                                sum.args[0].unsized);
    });
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
      std::vector<double> got = read_row(em.write_array->columns, eex);
      auto eprogram =
          std::make_shared<mir::Program>(stanli::decode_program(empty_sum));
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
  // z[4:5] is spelled Upfrom(4); write it as Between(lo, hi).
  const auto tail_write = [&](long lo, long hi, const std::string& lhs) {
    return after_product([&](std::vector<mir::Stmt>& body, size_t at) {
      mir::Stmt* write = nullptr;
      for (mir::Stmt* s : z_writes(body, at))
        if (!s->lhs_idx.empty() && s->lhs_idx[0].kind == mir::Expr::IndexUpfrom)
          write = s;
      write->lhs_idx[0] = pw::index(mir::Expr::IndexBetween,
                                    {pw::lit_int(lo), pw::lit_int(hi)});
      write->lhs = lhs;
    });
  };
  {
    mir::Program program = base_program;
    const bool found = pw::find_stmt(program, [](const mir::Stmt& s) {
      return s.kind == mir::Stmt::Assignment && s.lhs == "z" &&
             !s.lhs_idx.empty() &&
             s.lhs_idx[0].kind == mir::Expr::IndexUpfrom &&
             s.lhs_idx[0].args.size() == 1 && s.lhs_idx[0].args[0].lit_i == 4;
    });
    if (!found) {
      ++failures;
      std::printf("FAIL reductions guard cannot find the z Upfrom write\n");
      return;
    }
  }
  const auto expect_invalid_range = [&](long lo, long hi, const char* reason,
                                        const char* what) {
    const std::string mutated = tail_write(lo, hi, "z");
    expect_reduction_interp(mutated, reason, what);
    try {
      std::vector<std::string> names;
      (void)eval_reduction_interp(mutated, &names);
      ++failures;
      std::printf("FAIL %s: interpreter accepted invalid range\n", what);
    } catch (const std::exception&) {
    }
  };
  expect_invalid_range(0, 1,
                       "array[multi, ...] assign: accessing element out of "
                       "range. index 0 out of range",
                       "low OOB range assignment");
  expect_invalid_range(5, 6,
                       "array[multi, ...] assign: accessing element out of "
                       "range. index 6 out of range",
                       "high OOB range assignment");
  expect_invalid_range(4, 4, "width mismatch",
                       "range assignment RHS width mismatch");

  // A single range index on a 2-D value selects rows; 4:5 is outside the
  // base's two rows, so the write falls closed to WaInterp.
  expect_reduction_interp(
      tail_write(4, 5, "partial_matrix"),
      "array[multi, ...] assign: accessing element out of range. index 4 "
      "out of range",
      "out-of-bounds row-range assignment falls back");
}

// gq_reductions with `statement` inserted after int total = sum(z).
static std::string after_total(const stanli::mir::Stmt& statement) {
  using namespace stanli;
  mir::Program program =
      decode_program(slurp("tests/fixtures/gq_reductions.mir"));
  const auto [body, at] = portable_writer::find_position(
      program.generate_quantities, [](const mir::Stmt& s) {
        return s.kind == mir::Stmt::Assignment && s.lhs == "total";
      });
  if (body == nullptr)
    throw std::logic_error("reductions fixture has no total assignment");
  body->insert(body->begin() + static_cast<std::ptrdiff_t>(at) + 1, statement);
  return portable_writer::write_v2(program);
}

void test_runtime_int_sum_is_not_compile_time() {
  using namespace stanli;
  namespace pw = portable_writer;
  const mir::Expr total = pw::var("total", pw::view(mir::UnsizedLeaf::Int));
  {
    const CompiledModel cm = compile_model(
        after_total(pw::assign(
            "pr", {},
            pw::indexed(pw::var("x", pw::view(mir::UnsizedLeaf::Vector)),
                        {pw::index(mir::Expr::IndexSingle, {total})},
                        pw::view(mir::UnsizedLeaf::Real)))),
        reduction_data());
    int dynamic_slices = 0;
    if (cm.write_array)
      for (const Op& op : cm.write_array->graph.ops)
        dynamic_slices += op.opcode == OP_DYNAMIC_SLICE;
    if (!cm.write_array || cm.write_array->interp || dynamic_slices != 1) {
      ++failures;
      std::printf("FAIL runtime sum did not provide a dynamic index\n");
    }
  }

  expect_reduction_interp(
      after_total(pw::declare("shaped", "SVector", {total})),
      "unknown int total", "runtime sum cannot provide a shape", true);

  mir::Stmt loop;
  loop.kind = mir::Stmt::For;
  loop.loopvar = "i";
  loop.lower = pw::lit_int(1);
  loop.upper = total;
  loop.body = {pw::block({pw::skip()})};
  expect_reduction_island(after_total(loop),
                          "unused runtime loop runs as an island");

  mir::Stmt condition;
  condition.kind = mir::Stmt::IfElse;
  condition.cond = total;
  condition.body = {pw::block({pw::skip()})};
  expect_reduction_island(after_total(condition),
                          "runtime sum decides a runtime branch");
}

void test_runtime_int_sum_redeclaration_shadowing() {
  using namespace stanli;
  namespace pw = portable_writer;
  mir::Stmt literal_decl = pw::declare("total", "SInt");
  literal_decl.has_init = true;
  literal_decl.init = pw::lit_int(2);
  const std::string literal = after_total(literal_decl);
  CompiledModel cm = compile_model(literal, reduction_data());
  if (!cm.write_array || cm.write_array->interp) {
    ++failures;
    std::printf("FAIL same-id literal int redeclaration did not compile: %s\n",
                cm.write_array ? cm.write_array->truncated.value_or("").c_str()
                               : "no write_array");
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

  const std::string uninitialized = after_total(pw::declare("total", "SInt"));
  expect_compiled(uninitialized, reduction_data(),
                  "same-id uninitialized int sentinel");
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
  CompiledModel sentinel = compile_model(uninitialized, reduction_data());
  if (sentinel.write_array && !sentinel.write_array->interp) {
    Executor graph(std::move(sentinel.write_array->graph));
    sentinel.write_array->bind(graph);
    graph.params_data()[0] = 0.25;
    for (int i = 0; i < 5; ++i) graph.params_data()[1 + i] = 0.5;
    WaRng rng(44);
    graph.run_forward_only(EvalState{&rng});
    std::vector<double> compiled;
    for (const auto& column : sentinel.write_array->columns)
      for (int64_t i = 0; i < column.len; ++i)
        compiled.push_back(
            graph.value_ptr(column.slot)[column.storage_index(i)]);
    if (!same_double_bytes(compiled, row)) {
      ++failures;
      std::printf("FAIL scalar redeclaration graph/interpreter rows differ\n");
    }
  }
}

// choose() as a generated quantities extent and inside an index whose
// argument is the enclosing loop variable, which is how brms lays out the
// upper triangle of a group-level correlation matrix.
void test_choose_index_gq() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  DataMap data;
  data.set_int("M", 4);
  const std::string text = slurp("tests/fixtures/choosesize.mir");
  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  CompiledModel cm = compile_model(text, data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!cm.write_array || !cm.write_array->interp ||
      cm.write_array->truncated.has_value()) {
    ++failures;
    std::printf("FAIL choosesize did not compile completely: %s\n",
                cm.write_array ? cm.write_array->truncated.value_or("").c_str()
                               : "no write_array");
    return;
  }
  std::vector<double> q(22);
  for (size_t i = 0; i < q.size(); ++i) q[i] = 0.25 * (double)i - 1.0;

  Executor pex(cm.graph);
  cm.bind(pex);
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  const std::vector<double> row =
      cross_checked_row(cm, pex, wex, q, 5, "choosesize");
  expect_eq("choosesize header", joined(cm.write_array->columns),
            "z.1,z.2,z.3,z.4,z.5,z.6,A.1.1,A.2.1,A.3.1,A.4.1,A.1.2,A.2.2,"
            "A.3.2,A.4.2,A.1.3,A.2.3,A.3.3,A.4.3,A.1.4,A.2.4,A.3.4,A.4.4,"
            "cor.1,cor.2,cor.3,cor.4,cor.5,cor.6");
  // cor holds A's strict upper triangle, column by column.
  const double want[6] = {q[10], q[14], q[15], q[18], q[19], q[20]};
  for (int i = 0; i < 6; ++i)
    if (row.at((size_t)(22 + i)) != want[i]) {
      ++failures;
      std::printf("FAIL choosesize cor.%d: got %.17g want %.17g\n", i + 1,
                  row.at((size_t)(22 + i)), want[i]);
    }
}

// The LKJ densities on the per-draw path. lkj_corr_cholesky is what brms
// puts on every correlated group-level effect, so the interpreter meets it
// whenever the rest of the section sends the graph home.
void test_interpreted_lkj() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  DataMap data = DataMap::from_json_file("tests/fixtures/gqlkj.json");
  const std::string text = slurp("tests/fixtures/gqlkj.mir");
  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  CompiledModel cm = compile_model(text, data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!cm.write_array || !cm.write_array->interp) {
    ++failures;
    std::printf("FAIL gqlkj: expected an attached interpreter\n");
    return;
  }
  const double q[4] = {0.3, -0.6, 0.45, std::log(1.4)};
  Executor pex(cm.graph);
  cm.bind(pex);
  std::copy(q, q + 4, pex.params_data());
  pex.run_forward_only();
  WaRng rng(19);
  const std::vector<double> row =
      cm.write_array->interp->eval(cm.constrained_env(pex), rng);
  const std::vector<std::string> names =
      CompiledModel::csv_names(cm.write_array->interp->columns());
  const auto column = [&](const std::string& name) {
    for (size_t i = 0; i < names.size(); ++i)
      if (names[i] == name) return row.at(i);
    ++failures;
    std::printf("FAIL gqlkj: no column %s\n", name.c_str());
    return 0.0;
  };
  Eigen::MatrixXd L(3, 3);
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r)
      L(r, c) =
          column("L." + std::to_string(r + 1) + "." + std::to_string(c + 1));
  Eigen::MatrixXd R(3, 3);
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r)
      R(r, c) =
          column("R." + std::to_string(r + 1) + "." + std::to_string(c + 1));
  const double eta = column("eta");
  const double want_chol = stan::math::lkj_corr_cholesky_lpdf<false>(L, eta);
  const double want_corr = stan::math::lkj_corr_lpdf<false>(R, eta);
  const auto expect_close = [&](const char* what, double got, double want) {
    if (std::abs(got - want) > 1e-13 * std::max(1.0, std::abs(want))) {
      ++failures;
      std::printf("FAIL gqlkj %s: got %.17g want %.17g\n", what, got, want);
    }
  };
  expect_close("lkj_corr_cholesky_lpdf", column("lchol"), want_chol);
  expect_close("lkj_corr_lpdf", column("lcorr"), want_corr);
}

void test_runtime_control_write_array() {
  using namespace stanli;

  const std::string text = slurp("tests/fixtures/gq_runtime_control.mir");
  CompiledModel cm = compile_model(text, DataMap{});
  if (!compiled_completely(cm, "runtime-control write_array")) return;
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

  std::map<std::string, DataMap::Entry> base = emit_env();
  auto program = std::make_shared<mir::Program>(stanli::decode_program(text));
  WaInterp interpreted(program, std::move(base));
  const auto env = cm.constrained_env(params);
  const auto graph_row = [&](WaRng& rng) {
    graph.run_forward_only(EvalState{&rng});
    return read_row(cm.write_array->columns, graph);
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

void test_integer_rng_control() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  for (int n : {0, 1, 9}) {
    DataMap data;
    data.set_int("N", n);
    test_setenv("STANLI_WA_FORCE_INTERP", "1");
    auto cm =
        compile_model(slurp("tests/fixtures/gq_rng_integer_control.mir"), data);
    test_unsetenv("STANLI_WA_FORCE_INTERP");
    if (!cm.write_array || cm.write_array->truncated.has_value()) {
      ++failures;
      std::printf("FAIL integer RNG control did not compile: %s\n",
                  cm.write_array
                      ? cm.write_array->truncated.value_or("").c_str()
                      : "no write_array");
      continue;
    }
    Executor ex(cm.write_array->graph);
    cm.write_array->bind(ex);
    Executor copy(ex);
    WaRng graph_rng(91), interp_rng(91), copy_rng(91);
    for (int draw = 0; draw < 30; ++draw) {
      const double x = 0.5 * ((draw % 5) - 2);
      ex.params_data()[0] = copy.params_data()[0] = x;
      std::map<std::string, DataMap::Entry> params;
      params["x"].r = {x};
      const auto expected = cm.write_array->interp->eval(params, interp_rng);
      const auto evaluate = [&](Executor& engine, WaRng& rng) {
        engine.run_forward_only(EvalState{&rng});
        std::vector<double> row;
        for (const auto& c : cm.write_array->columns) {
          const double* values =
              static_cast<const Executor&>(engine).value_ptr(c.slot);
          for (int64_t i = 0; i < c.len; ++i)
            row.push_back(values[c.storage_index(i)]);
        }
        return row;
      };
      if (!same_double_bytes(evaluate(ex, graph_rng), expected) ||
          !same_double_bytes(evaluate(copy, copy_rng), expected)) {
        ++failures;
        std::printf("FAIL integer RNG control row/copy parity N=%d draw=%d\n",
                    n, draw);
      }
    }
    const auto next = interp_rng.gen()();
    if (graph_rng.gen()() != next || copy_rng.gen()() != next) {
      ++failures;
      std::printf("FAIL integer RNG control consumed the wrong stream\n");
    }
  }

  DataMap data;
  auto dynamic =
      compile_model(slurp("tests/fixtures/gq_rng_dynamic_shape.mir"), data);
  if (!dynamic.write_array || !dynamic.write_array->interp ||
      !dynamic.write_array->truncated) {
    ++failures;
    std::printf("FAIL RNG-dependent local extent must stay interpreted\n");
  } else {
    WaRng rng(8), oracle(8);
    std::map<std::string, DataMap::Entry> params;
    params["x"].r = {0.5};
    const auto row = dynamic.write_array->interp->eval(params, rng);
    const double expected =
        0.5 * (2 + stan::math::poisson_rng(std::exp(0.5), oracle.gen()) +
               stan::math::poisson_rng(std::exp(0.5), oracle.gen()));
    if (row.at(1) != expected || rng.gen()() != oracle.gen()()) {
      ++failures;
      std::printf("FAIL dynamic-shape fallback value/stream\n");
    }
  }

  test_setenv("STANLI_WA_FORCE_INTERP", "1");
  auto bad =
      compile_model(slurp("tests/fixtures/gq_rng_invalid_index.mir"), data);
  test_unsetenv("STANLI_WA_FORCE_INTERP");
  if (!bad.write_array || bad.write_array->truncated.has_value()) {
    ++failures;
    std::printf("FAIL RNG checked-index fixture did not compile\n");
  } else {
    Executor ex(bad.write_array->graph);
    bad.write_array->bind(ex);
    ex.params_data()[0] = 0;
    WaRng graph_rng(9), interp_rng(9);
    std::map<std::string, DataMap::Entry> params;
    params["x"].r = {0};
    bool graph_threw = false, interp_threw = false;
    try {
      ex.run_forward_only(EvalState{&graph_rng});
    } catch (const std::exception&) {
      graph_threw = true;
    }
    try {
      (void)bad.write_array->interp->eval(params, interp_rng);
    } catch (const std::exception&) {
      interp_threw = true;
    }
    if (!graph_threw || !interp_threw ||
        graph_rng.gen()() != interp_rng.gen()()) {
      ++failures;
      std::printf("FAIL RNG invalid index must throw after the same draw\n");
    }
  }
}

// Check compiled selection as well as values: the MIR comparison alone would
// otherwise allow the complete output block to fall back unnoticed.
void test_integer_expressions() {
  SKIP_WITHOUT_DEBUG_SWITCHES();
  using namespace stanli;
  const std::string source = slurp("tests/fixtures/gq_integer_expressions.mir");
  for (int divisor : {2, -2, 1, -1, std::numeric_limits<int>::max(),
                      std::numeric_limits<int>::min(), 0}) {
    DataMap data;
    data.set_int("divisor", divisor);
    test_setenv("STANLI_WA_FORCE_INTERP", "1");
    auto model = compile_model(source, data);
    test_unsetenv("STANLI_WA_FORCE_INTERP");
    if (!model.write_array || !model.write_array->interp ||
        model.write_array->truncated.has_value()) {
      ++failures;
      std::printf("FAIL integer expressions did not compile\n");
      continue;
    }
    Executor ex(model.write_array->graph);
    model.write_array->bind(ex);
    for (double x : {-3.0, 0.4, 3.0}) {
      ex.params_data()[0] = x;
      std::map<std::string, DataMap::Entry> params;
      params["x"].r = {x};
      for (unsigned seed : {1u, 1234u, 2026u}) {
        WaRng graph_rng(seed), interp_rng(seed), oracle(seed);
        const int draw =
            stan::math::binomial_rng(9, stan::math::inv_logit(x), oracle.gen());
        std::vector<double> graph_row, interp_row;
        bool graph_error = false, interp_error = false;
        try {
          ex.run_forward_only(EvalState{&graph_rng});
          for (const auto& column : model.write_array->columns)
            for (int64_t i = 0; i < column.len; ++i)
              graph_row.push_back(
                  ex.value_ptr(column.slot)[column.storage_index(i)]);
        } catch (const std::domain_error&) {
          graph_error = true;
        }
        try {
          interp_row = model.write_array->interp->eval(params, interp_rng);
        } catch (const std::domain_error&) {
          interp_error = true;
        }
        if (graph_error != (divisor == 0) || interp_error != graph_error ||
            graph_row != interp_row || !(graph_rng.gen() == interp_rng.gen())) {
          ++failures;
          std::printf("FAIL integer expression row/error/stream parity\n");
        }
        if (divisor == 0) {
          if (!(graph_rng.gen() == oracle.gen())) {
            ++failures;
            std::printf("FAIL zero divisor consumed a later RNG draw\n");
          }
          continue;
        }
        // Independent integer semantics, including truncation toward zero and
        // integer quotients/remainders subsequently assigned to real outputs.
        const int choice = stan::math::bernoulli_logit_rng(x, oracle.gen());
        const double after = stan::math::normal_rng(0, 1, oracle.gen());
        const std::vector<double> expected{x,
                                           double(draw),
                                           double(draw / divisor),
                                           double(-draw / divisor),
                                           double(draw / divisor),
                                           double(draw % divisor),
                                           double(-draw % divisor),
                                           double((draw + 1) / divisor),
                                           double((draw + 1) / divisor),
                                           double(-draw % divisor),
                                           double(choice),
                                           double(2 * choice + 1),
                                           double(choice),
                                           2,
                                           after};
        if (graph_row != expected || !(graph_rng.gen() == oracle.gen())) {
          ++failures;
          std::printf("FAIL integer expression Stan oracle\n");
        }
      }
    }
  }

  const auto refused = [&](const std::string& text, const char* reason) {
    DataMap data;
    data.set_int("divisor", 2);
    auto model = compile_model(text, data);
    if (!model.write_array || !model.write_array->interp ||
        model.write_array->truncated.value_or("").find(reason) ==
            std::string::npos) {
      ++failures;
      std::printf("FAIL direct integer sum lost guard: %s\n", reason);
    }
  };
  // The fixture with the first expression that `match` accepts changed by
  // `change`.
  const auto mutated = [&](const std::function<bool(const mir::Expr&)>& match,
                           const std::function<void(mir::Expr&)>& change) {
    mir::Program program = decode_program(source);
    mir::Expr* found = portable_writer::find_expr(program, match);
    if (found == nullptr) {
      ++failures;
      std::printf("FAIL integer expressions fixture lacks a mutation site\n");
      return std::string();
    }
    change(*found);
    return portable_writer::write_v2(program);
  };
  refused(mutated(
              [](const mir::Expr& e) {
                return portable_writer::calls(e, "bernoulli_logit_rng");
              },
              [](mir::Expr& e) { e.name = "poisson_log_rng"; }),
          "unproved integral slot values");
  refused(
      mutated(
          [](const mir::Expr& e) { return portable_writer::calls(e, "sum"); },
          [](mir::Expr& sum) {
            std::function<bool(mir::Expr&)> raise = [&](mir::Expr& e) {
              if (e.kind == mir::Expr::LitInt && e.lit_i == 1) {
                e.lit_i = 2147483647;
                return true;
              }
              for (mir::Expr& arg : e.args)
                if (raise(arg)) return true;
              return false;
            };
            raise(sum);
          }),
      "overflow int32");
}

int main() {
  test_integer_expressions();
  test_integer_rng_control();
  test_naming_rules();
  test_wanames_pipeline();
  test_wanames_interpreter_schema();
  test_interpreter_target_increment();
  test_array_of_matrix_columns();
  test_gq_bare_fill_is_nan();
  test_gq_name_shadowing();
  test_write_array_target_lp();
  test_write_array_tp_lp_refused();
  test_interpreted_gq();
  test_compiled_extra_rng();
  test_compiled_region_rng();
  test_region_rng_is_not_folded();
  test_interpreted_gq_densities();
  test_interpreted_gq_gp_covariances();
  test_compiled_multiply_lower_tri();
  test_choose_index_gq();
  test_interpreted_lkj();
  test_constant_folded_gq_column();
  test_constant_folded_gq_after_empty_output();
  test_mean_grouping();
  test_categorical_rng_lowering_guards();
  test_compiled_categorical_rng();
  test_categorical_rng_empty_vector();
  test_categorical_rng_dynamic_index();
  test_multi_normal_rng_lowering_guards();
  test_compiled_multi_normal_rng();
  test_binomial_rng_lowering_guards();
  test_compiled_scalar_rng();
  test_layout_materialization_boundaries();
  test_main_index_layout_metadata();
  test_matrix_transpose_extrema_fallback();
  test_compiled_gq_extrema();
  test_gq_extrema_lowering_guards();
  test_gq_extrema_view_lowering_guards();
  test_compiled_gq_reductions();
  test_gq_reduction_lowering_guards();
  test_runtime_int_sum_is_not_compile_time();
  test_runtime_int_sum_redeclaration_shadowing();
  test_runtime_control_write_array();
  test_caller_owned_rng();
  test_transformed_parameter_checks();
  test_write_array_retained_loop();
  test_write_array_forward_only();
  test_write_array_reused_values();
  test_write_array_carried_loop();
  test_write_array_block_order();
  test_write_array_vector_rng();
  test_write_array_rng_loops();
  test_write_array_data_start();
  test_write_array_data_branch();
  test_write_array_counted_size();
  test_write_array_compare_size();
  test_write_array_bounded_blocks();
  test_write_array_selected_loop_refused();
  test_write_array_selected_loop_overflow();
  if (failures == 0) std::printf("test_write_array OK\n");
  return failures == 0 ? 0 : 1;
}
