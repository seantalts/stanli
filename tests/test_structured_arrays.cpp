// Outer arrays of structured parameter transforms, against literals produced
// by generated Stan and Reference BridgeStan. This test keeps raw sampler
// order, constrained serialization order, and unconstrained naming separate
// across Graph, compiled/direct-interpreted write_array, and BridgeStan.
#include "structured_array_oracles.hpp"

#include "../runtime/third_party/bridgestan.h"

#include <stanli/bridgestan_internal.hpp>
#include <stanli/compile.hpp>
#include <stanli/mir.hpp>
#include <stanli/sexp.hpp>
#include <stanli/wa_interp.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace oracle = structured_array_oracle;

int failures = 0;

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}

std::string data_json(int B) { return "{\"B\":" + std::to_string(B) + "}"; }

std::string join(const std::vector<std::string>& xs) {
  std::string out;
  for (const auto& x : xs) {
    if (!out.empty()) out += ',';
    out += x;
  }
  return out;
}

bool replace_sexp(std::string& text, size_t start,
                  const std::string& replacement) {
  if (start == std::string::npos || text[start] != '(') return false;
  int depth = 0;
  for (size_t i = start; i < text.size(); ++i) {
    if (text[i] == '(') {
      ++depth;
    } else if (text[i] == ')' && --depth == 0) {
      text.replace(start, i - start + 1, replacement);
      return true;
    }
  }
  return false;
}

void fail(const std::string& what) {
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

void expect_count(const std::string& what, int64_t got, int64_t want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %s: got %lld want %lld\n", what.c_str(),
                static_cast<long long>(got), static_cast<long long>(want));
  }
}

void expect_near(const std::string& what, double got, double want) {
  const double tol = 2e-12 * std::max(1.0, std::fabs(want));
  if (!(std::fabs(got - want) <= tol)) {
    ++failures;
    std::printf("FAIL %s: got %.17g want %.17g (tol %.3g)\n", what.c_str(), got,
                want, tol);
  }
}

void expect_names(const std::string& what, const std::vector<std::string>& got,
                  const std::vector<std::string>& want) {
  if (got == want) return;
  ++failures;
  std::printf("FAIL %s names: got %zu want %zu\n", what.c_str(), got.size(),
              want.size());
  const size_t n = std::max(got.size(), want.size());
  for (size_t i = 0; i < n; ++i) {
    const std::string g = i < got.size() ? got[i] : "<missing>";
    const std::string w = i < want.size() ? want[i] : "<missing>";
    if (g != w)
      std::printf("  [%zu] got %s want %s\n", i, g.c_str(), w.c_str());
  }
}

void expect_values(const std::string& what, const std::vector<double>& got,
                   const std::vector<double>& want) {
  if (got.size() != want.size()) {
    expect_count(what + " width", (int64_t)got.size(), (int64_t)want.size());
    return;
  }
  for (size_t i = 0; i < got.size(); ++i)
    expect_near(what + "[" + std::to_string(i) + "]", got[i], want[i]);
}

std::vector<double> materialize(
    stanli::Executor& ex,
    const std::vector<stanli::CompiledModel::ParamView>& views) {
  std::vector<double> out;
  for (const auto& view : views) {
    const double* p = ex.value_ptr(view.slot);
    for (int64_t i = 0; i < view.len; ++i)
      out.push_back(p[view.storage_index(i)]);
  }
  return out;
}

void test_graph_and_write_array(int B, const std::string& mir) {
  using namespace stanli;
  const std::string tag = "B=" + std::to_string(B);
  DataMap data;
  data.set_int("B", B);
  CompiledModel cm = compile_model(mir, data);
  const std::vector<double> q = oracle::q(B);

  expect_count(tag + " compiled unconstrained", cm.n_unconstrained,
               oracle::n_unc(B));
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  expect_count(tag + " executor parameters", ex.n_params(), oracle::n_unc(B));
  for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
  std::vector<double> grad(q.size());
  expect_near(tag + " lp", ex.gradient(grad.data()), oracle::lp(B));
  expect_values(tag + " gradient", grad, oracle::grad(B));
  expect_names(tag + " constrained", CompiledModel::csv_names(cm.views),
               oracle::constrained_names(B));
  expect_values(tag + " constrained", materialize(ex, cm.views),
                oracle::constrained(B));

  if (!cm.write_array) {
    fail(tag + " has no compiled write_array");
    return;
  }
  if (!cm.write_array->truncated.empty() || cm.write_array->interp)
    fail(tag + " unexpectedly fell back from compiled write_array");
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  for (size_t i = 0; i < q.size(); ++i) wex.params_data()[i] = q[i];
  wex.run_forward_only();
  expect_names(tag + " compiled write_array",
               CompiledModel::csv_names(cm.write_array->columns),
               oracle::write_names(B));
  expect_values(tag + " compiled write_array",
                materialize(wex, cm.write_array->columns),
                oracle::write_values(B));

  auto prog =
      std::make_shared<mir::Program>(mir::read_program(sexp::parse(mir)));
  std::map<std::string, DataMap::Entry> base;
  DataMap::Entry b;
  b.is_int = true;
  b.i = {B};
  b.r = {(double)B};
  base["B"] = b;
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
  const std::vector<double> row = interpreted.eval(cm.constrained_env(ex), rng);
  expect_names(tag + " interpreted write_array",
               CompiledModel::csv_names(interpreted.columns()),
               oracle::write_names(B));
  expect_values(tag + " interpreted write_array", row, oracle::write_values(B));
}

void test_bridgestan(int B, const std::string& mir) {
  const std::string tag = "Bridge B=" + std::to_string(B);
  const std::string data = data_json(B);
  char* err = nullptr;
  bs_model* model = bs_model_from_mir(mir.c_str(), data.c_str(), 1234, &err);
  if (model == nullptr) {
    fail(tag + " construction: " + (err ? std::string(err) : "no message"));
    bs_free_error_msg(err);
    return;
  }

  expect_count(tag + " unconstrained count", bs_param_unc_num(model),
               oracle::n_unc(B));
  const char* unc = bs_param_unc_names(model);
  if (unc == nullptr ||
      std::string(unc) != join(oracle::unconstrained_names(B)))
    fail(tag + " unconstrained names");
  expect_count(tag + " constrained count", bs_param_num(model, false, false),
               oracle::n_con(B));
  const char* con = bs_param_names(model, false, false);
  if (con == nullptr || std::string(con) != join(oracle::constrained_names(B)))
    fail(tag + " constrained names");

  const std::vector<double> q = oracle::q(B);
  double lp = 0;
  std::vector<double> grad(q.size());
  int rc = bs_log_density_gradient(model, true, true, q.data(), &lp,
                                   grad.data(), &err);
  if (rc != 0) {
    fail(tag + " gradient: " + (err ? std::string(err) : "no message"));
    bs_free_error_msg(err);
    err = nullptr;
  } else {
    expect_near(tag + " lp", lp, oracle::lp(B));
    expect_values(tag + " gradient", grad, oracle::grad(B));
  }

  std::vector<double> constrained((size_t)oracle::n_con(B));
  rc = bs_param_constrain(model, false, false, q.data(), constrained.data(),
                          nullptr, &err);
  if (rc != 0) {
    fail(tag + " constrain: " + (err ? std::string(err) : "no message"));
    bs_free_error_msg(err);
    err = nullptr;
  } else {
    expect_values(tag + " constrain", constrained, oracle::constrained(B));
  }

  expect_count(tag + " write count", bs_param_num(model, false, true),
               (int64_t)oracle::write_names(B).size());
  const char* write_names = bs_param_names(model, false, true);
  if (write_names == nullptr ||
      std::string(write_names) != join(oracle::write_names(B)))
    fail(tag + " write names");
  bs_rng* rng = bs_rng_construct(1234, &err);
  if (rng == nullptr) {
    fail(tag + " RNG construction");
  } else {
    std::vector<double> row(oracle::write_values(B).size());
    rc =
        bs_param_constrain(model, false, true, q.data(), row.data(), rng, &err);
    if (rc != 0) {
      fail(tag + " write row: " + (err ? std::string(err) : "no message"));
      bs_free_error_msg(err);
      err = nullptr;
    } else {
      expect_values(tag + " write row", row, oracle::write_values(B));
    }
    bs_rng_destruct(rng);
  }
  bs_model_destruct(model);
}

void test_stale_read_dimensions(const std::string& mir) {
  std::string stale = mir;
  size_t at = stale.find("FnReadParam (constrain Simplex)");
  if (at != std::string::npos) at = stale.find("(Lit Int 3)", at);
  if (at == std::string::npos) {
    fail("could not construct stale structured MIR fixture");
    return;
  }
  stale.replace(at, std::string("(Lit Int 3)").size(), "(Lit Int 4)");
  stanli::DataMap data;
  data.set_int("B", 1);
  try {
    (void)stanli::compile_model(stale, data);
    fail("stale structured FnReadParam dimensions compiled");
  } catch (const stanli::CompileError& e) {
    if (std::string(e.what()).find("read dimensions") == std::string::npos)
      fail("stale structured MIR failed for the wrong reason: " +
           std::string(e.what()));
  }
}

void test_malformed_vector_transform() {
  std::string malformed = slurp("tests/fixtures/newtrans.tmir.sexp");
  const size_t id = malformed.find("(decl_id u)");
  size_t type = malformed.find("(SVector AoS", id);
  if (!replace_sexp(malformed, type, "SReal")) {
    fail("could not replace malformed vector declaration");
    return;
  }
  const size_t read = malformed.find("FnReadParam (constrain UnitVector)", id);
  size_t dims = malformed.find("(dims", read);
  if (!replace_sexp(malformed, dims, "(dims ())")) {
    fail("could not replace malformed vector read dimensions");
    return;
  }
  stanli::DataMap data;
  data.set_real("m", 0.0);
  data.set_real("s", 1.0);
  try {
    (void)stanli::compile_model(malformed, data);
    fail("unit-vector transform with a scalar declaration compiled");
  } catch (const stanli::CompileError& e) {
    if (std::string(e.what()).find("non-vector declaration") ==
        std::string::npos)
      fail("malformed vector transform failed for the wrong reason: " +
           std::string(e.what()));
  }
}

}  // namespace

int main() {
  const std::string mir = slurp("tests/fixtures/structured_arrays.tmir.sexp");
  for (int B = 0; B <= 2; ++B) {
    try {
      test_graph_and_write_array(B, mir);
      test_bridgestan(B, mir);
    } catch (const std::exception& e) {
      fail("B=" + std::to_string(B) + " threw: " + e.what());
    }
  }
  test_stale_read_dimensions(mir);
  test_malformed_vector_transform();
  if (failures == 0) std::printf("test_structured_arrays OK\n");
  return failures == 0 ? 0 : 1;
}
