// Structured declaration checks against the exact Stan Math validators.
//
// These are not parameter transforms: stanc emits FnCheck after a data,
// transformed-parameter, or generated-quantity declaration. The value may be
// an array, whose JSON/interpreter storage interleaves leaves even though the
// lowered graph keeps each leaf contiguous.
#include "env_helpers.hpp"

#include <stanli/compile.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/sexp.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using stanli::CompiledModel;
using stanli::DataMap;
using stanli::Executor;
using stanli::MirInterp;
using stanli::WaInterp;
using stanli::WaRng;
using stanli::mir::Expr;
using stanli::mir::Stmt;
using stanli::mir::Transform;
using stanli::mir::UnsizedLeaf;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

enum class Outcome { Accepted, Domain, Invalid, Other };

Outcome outcome(const std::function<void()>& f) {
  try {
    f();
    return Outcome::Accepted;
  } catch (const std::domain_error&) {
    return Outcome::Domain;
  } catch (const std::invalid_argument&) {
    return Outcome::Invalid;
  } catch (const std::exception&) {
    return Outcome::Other;
  }
}

Expr var(std::string name, UnsizedLeaf leaf, uint8_t depth = 0) {
  Expr e;
  e.kind = Expr::Var;
  e.name = std::move(name);
  e.unsized = {depth, leaf};
  return e;
}

Stmt structured_check(Transform::Kind kind, UnsizedLeaf leaf,
                      uint8_t depth = 0) {
  Stmt s;
  s.kind = Stmt::NRFunApp;
  s.fn_name = "FnCheck";
  s.check_var_name = "x";
  Transform t;
  t.kind = kind;
  s.check_transform = std::move(t);
  s.fn_args = {var("x", leaf, depth)};
  return s;
}

MirInterp<double> interp() {
  static const std::map<std::string, const stanli::mir::FunDef*> no_funs;
  return MirInterp<double>(no_funs, "structured FnCheck test");
}

struct LeafCase {
  const char* name;
  Transform::Kind kind;
  UnsizedLeaf leaf;
  int64_t rows;
  int64_t cols;
  std::vector<double> good;
  std::vector<double> bad;
};

Outcome stan_outcome(const LeafCase& c, const std::vector<double>& x) {
  return outcome([&] {
    const double zero = 0.0;
    const double* p = x.empty() ? &zero : x.data();
    Eigen::Map<const Eigen::VectorXd> v(p, (Eigen::Index)c.rows);
    Eigen::Map<const Eigen::MatrixXd> m(p, (Eigen::Index)c.rows,
                                        (Eigen::Index)c.cols);
    switch (c.kind) {
      case Transform::Simplex:
        stan::math::check_simplex("oracle", c.name, v);
        return;
      case Transform::Ordered:
        stan::math::check_ordered("oracle", c.name, v);
        return;
      case Transform::PositiveOrdered:
        stan::math::check_positive_ordered("oracle", c.name, v);
        return;
      case Transform::UnitVector:
        stan::math::check_unit_vector("oracle", c.name, v);
        return;
      case Transform::SumToZero:
        if (c.leaf == UnsizedLeaf::Matrix)
          stan::math::check_sum_to_zero("oracle", c.name, m);
        else
          stan::math::check_sum_to_zero("oracle", c.name, v);
        return;
      case Transform::CholeskyCorr:
        stan::math::check_cholesky_factor_corr("oracle", c.name, m);
        return;
      case Transform::Correlation:
        stan::math::check_corr_matrix("oracle", c.name, m);
        return;
      case Transform::Covariance:
        stan::math::check_cov_matrix("oracle", c.name, m);
        return;
      case Transform::CholeskyCov:
        stan::math::check_cholesky_factor("oracle", c.name, m);
        return;
      default:
        throw std::logic_error("non-structured oracle kind");
    }
  });
}

Outcome interp_outcome(const LeafCase& c, const std::vector<double>& x) {
  auto m = interp();
  DataMap::Entry e;
  e.r = x;
  e.dims = c.leaf == UnsizedLeaf::Matrix ? std::vector<int64_t>{c.rows, c.cols}
                                         : std::vector<int64_t>{c.rows};
  m.env()["x"] = std::move(e);
  const Stmt s = structured_check(c.kind, c.leaf);
  return outcome([&] { m.exec(s); });
}

void test_leaf_oracles() {
  const std::vector<LeafCase> cases{
      {"simplex",
       Transform::Simplex,
       UnsizedLeaf::Vector,
       3,
       1,
       {0.2, 0.3, 0.5},
       {0.2, 0.3, 0.6}},
      {"ordered",
       Transform::Ordered,
       UnsizedLeaf::Vector,
       3,
       1,
       {0, 1, 2},
       {0, 0, 2}},
      {"positive_ordered",
       Transform::PositiveOrdered,
       UnsizedLeaf::Vector,
       3,
       1,
       {0, 1, 2},
       {-1, 1, 2}},
      {"unit_vector",
       Transform::UnitVector,
       UnsizedLeaf::Vector,
       3,
       1,
       {1, 0, 0},
       {1, 1, 0}},
      {"sum_to_zero_vector",
       Transform::SumToZero,
       UnsizedLeaf::Vector,
       3,
       1,
       {1, -1, 0},
       {1, 0, 0}},
      {"cholesky_corr",
       Transform::CholeskyCorr,
       UnsizedLeaf::Matrix,
       2,
       2,
       {1, 0, 0, 1},
       {1, 0, 0.1, std::sqrt(0.99)}},
      {"corr",
       Transform::Correlation,
       UnsizedLeaf::Matrix,
       2,
       2,
       {1, 0, 0, 1},
       {0.9, 0, 0, 1}},
      {"cov",
       Transform::Covariance,
       UnsizedLeaf::Matrix,
       2,
       2,
       {1, 0, 0, 1},
       {1, 0, 1, 1}},
      {"cholesky_cov",
       Transform::CholeskyCov,
       UnsizedLeaf::Matrix,
       3,
       2,
       {1, 0, 0, 0, 1, 0},
       {1, 0, 0, 0.2, 1, 0}},
      // Stan Math checks the grand matrix total, not each row/column.
      {"sum_to_zero_matrix",
       Transform::SumToZero,
       UnsizedLeaf::Matrix,
       2,
       2,
       {1, -3, 2, 0},
       {1, -3, 2, 1}},
  };

  for (const auto& c : cases) {
    const Outcome good = stan_outcome(c, c.good);
    const Outcome bad = stan_outcome(c, c.bad);
    check(good == Outcome::Accepted, std::string(c.name) + " oracle good");
    check(bad != Outcome::Accepted, std::string(c.name) + " oracle bad");
    check(interp_outcome(c, c.good) == good,
          std::string(c.name) + " interpreter good parity");
    check(interp_outcome(c, c.bad) == bad,
          std::string(c.name) + " interpreter bad parity");

    std::vector<double> nan = c.good;
    if (!nan.empty()) nan[0] = std::numeric_limits<double>::quiet_NaN();
    check(interp_outcome(c, nan) == stan_outcome(c, nan),
          std::string(c.name) + " interpreter NaN parity");
  }

  // Tolerance is owned by Stan Math; the interpreter must not substitute a
  // hand-rolled equality test.
  LeafCase simplex = cases[0];
  simplex.good = {0.2, 0.3, 0.5 + 0.5e-8};
  simplex.bad = {0.2, 0.3, 0.5 + 2e-8};
  check(interp_outcome(simplex, simplex.good) ==
            stan_outcome(simplex, simplex.good),
        "simplex tolerance accepted parity");
  check(interp_outcome(simplex, simplex.bad) ==
            stan_outcome(simplex, simplex.bad),
        "simplex tolerance rejected parity");

  // Zero-leaf behavior intentionally differs by transform; pin the exact
  // direct Stan Math result rather than imposing a blanket rule.
  for (LeafCase c : cases) {
    c.rows = 0;
    c.cols = c.leaf == UnsizedLeaf::Matrix ? 0 : 1;
    c.good.clear();
    check(interp_outcome(c, c.good) == stan_outcome(c, c.good),
          std::string(c.name) + " zero-leaf parity");
  }

  for (const auto& shape : {std::pair<int64_t, int64_t>{1, 3}, {3, 1}}) {
    LeafCase thin{"thin_sum_to_zero", Transform::SumToZero, UnsizedLeaf::Matrix,
                  shape.first,        shape.second,         {1, -1, 0},
                  {1, 0, 0}};
    check(interp_outcome(thin, thin.good) == stan_outcome(thin, thin.good),
          "thin sum-to-zero matrix accepted parity");
    check(interp_outcome(thin, thin.bad) == stan_outcome(thin, thin.bad),
          "thin sum-to-zero matrix rejected parity");
  }
}

void test_nested_layout_and_malformed() {
  const std::vector<int64_t> dims{2, 3, 3};
  constexpr int64_t batch = 6;
  auto encoded = [&](int invalid_leaf) {
    std::vector<double> physical(18);
    for (int64_t leaf = 0; leaf < batch; ++leaf) {
      const int64_t i = leaf / 3;
      const int64_t j = leaf % 3;
      const int64_t outer_serial = i + 2 * j;
      const double a = 0.1 + 0.01 * (double)leaf;
      const double bump = leaf == invalid_leaf ? 0.1 : 0.0;
      const double values[3] = {a, 0.2, 0.8 - a + bump};
      for (int64_t k = 0; k < 3; ++k)
        physical[(size_t)(outer_serial + batch * k)] = values[k];
    }
    return physical;
  };

  const Stmt nested =
      structured_check(Transform::Simplex, UnsizedLeaf::Vector, 2);
  auto run = [&](std::vector<double> x, std::vector<int64_t> shape) {
    auto m = interp();
    DataMap::Entry e;
    e.r = std::move(x);
    e.dims = std::move(shape);
    m.env()["x"] = std::move(e);
    return outcome([&] { m.exec(nested); });
  };
  check(run(encoded(-1), dims) == Outcome::Accepted,
        "nested first-index-fast simplex leaves are gathered in Stan order");

  auto m = interp();
  DataMap::Entry bad;
  bad.r = encoded(1);
  bad.dims = dims;
  m.env()["x"] = std::move(bad);
  bool named_second = false;
  try {
    m.exec(nested);
  } catch (const std::domain_error& e) {
    named_second = std::string(e.what()).find("x[1][2]") != std::string::npos;
  }
  check(named_second, "nested structured checks retain leaf order and name");
  check(run({}, {2, 0, 3}) == Outcome::Accepted,
        "zero middle array extent executes no leaf checks");

  stanli::StructuredCheckSpec empty_outer;
  empty_outer.kind = Transform::Simplex;
  empty_outer.leaf = stanli::StructuredLeaf::Vector;
  empty_outer.storage = stanli::StructuredStorage::FirstIndexFast;
  empty_outer.name = "empty";
  empty_outer.dims = {0, std::numeric_limits<int64_t>::max()};
  check(outcome([&] {
          stanli::check_structured_value(nullptr, 0, empty_outer);
        }) == Outcome::Accepted,
        "zero outer batch does not allocate one leaf's scratch storage");

  stanli::StructuredCheckSpec graph_spec;
  graph_spec.kind = Transform::Simplex;
  graph_spec.leaf = stanli::StructuredLeaf::Vector;
  graph_spec.storage = stanli::StructuredStorage::ContiguousLeaves;
  graph_spec.name = "x";
  graph_spec.dims = dims;
  std::vector<double> contiguous;
  for (int64_t leaf = 0; leaf < batch; ++leaf) {
    const double a = 0.1 + 0.01 * (double)leaf;
    contiguous.insert(contiguous.end(), {a, 0.2, 0.8 - a});
  }
  check(outcome([&] {
          stanli::check_structured_value(
              contiguous.data(), (int64_t)contiguous.size(), graph_spec);
        }) == Outcome::Accepted,
        "graph-contiguous nested structured leaves are accepted");
  contiguous[3 + 2] += 0.1;
  bool graph_named_second = false;
  try {
    stanli::check_structured_value(contiguous.data(),
                                   (int64_t)contiguous.size(), graph_spec);
  } catch (const std::domain_error& e) {
    graph_named_second =
        std::string(e.what()).find("x[1][2]") != std::string::npos;
  }
  check(graph_named_second,
        "graph-contiguous nested leaves retain failure order and name");

  Stmt wrong_arity = nested;
  wrong_arity.fn_args.push_back(var("x", UnsizedLeaf::Vector, 2));
  auto malformed = interp();
  malformed.env()["x"].r = encoded(-1);
  malformed.env()["x"].dims = dims;
  check(outcome([&] { malformed.exec(wrong_arity); }) == Outcome::Other,
        "structured check rejects malformed arity");

  Stmt wrong_leaf =
      structured_check(Transform::Simplex, UnsizedLeaf::RowVector);
  auto wrong_type = interp();
  wrong_type.env()["x"].r = {0.2, 0.3, 0.5};
  wrong_type.env()["x"].dims = {3};
  check(outcome([&] { wrong_type.exec(wrong_leaf); }) == Outcome::Other,
        "simplex check rejects row-vector MIR");

  auto wrong_width = interp();
  wrong_width.env()["x"].r = {0.5, 0.5};
  wrong_width.env()["x"].dims = {3};
  const Stmt simplex =
      structured_check(Transform::Simplex, UnsizedLeaf::Vector);
  check(outcome([&] { wrong_width.exec(simplex); }) == Outcome::Other,
        "structured check rejects inconsistent stored width");

  LeafCase rectangular_corr{"rectangular_corr",
                            Transform::Correlation,
                            UnsizedLeaf::Matrix,
                            2,
                            3,
                            {1, 0, 0, 1, 0, 0},
                            {}};
  check(interp_outcome(rectangular_corr, rectangular_corr.good) ==
            stan_outcome(rectangular_corr, rectangular_corr.good),
        "rectangular correlation error parity");
  LeafCase short_cholesky{"short_cholesky",
                          Transform::CholeskyCov,
                          UnsizedLeaf::Matrix,
                          2,
                          3,
                          {1, 0, 0, 1, 0, 0},
                          {}};
  check(interp_outcome(short_cholesky, short_cholesky.good) ==
            stan_outcome(short_cholesky, short_cholesky.good),
        "M below N cholesky-factor error parity");
}

void test_declaration_geometry_is_evaluated_once() {
  auto m = interp();
  DataMap::Entry k;
  k.is_int = true;
  k.i = {3};
  k.r = {3.0};
  m.env()["K"] = k;

  Stmt decl;
  decl.kind = Stmt::Decl;
  decl.decl_id = "x";
  decl.decl_type.base = "SVector";
  decl.decl_type.dims = {var("K", UnsizedLeaf::Int)};
  m.exec(decl);
  m.env()["x"].r = {0.2, 0.3, 0.5};
  m.env()["x"].dims = {3};

  m.env()["K"].i = {4};
  m.env()["K"].r = {4.0};
  const Stmt simplex =
      structured_check(Transform::Simplex, UnsizedLeaf::Vector);
  check(outcome([&] { m.exec(simplex); }) == Outcome::Accepted,
        "structured check reuses declaration-time dimensions");
}

std::string replace_once(std::string text, const std::string& from,
                         const std::string& to) {
  const size_t at = text.find(from);
  if (at == std::string::npos)
    throw std::logic_error("test replacement missing");
  text.replace(at, from.size(), to);
  return text;
}

void test_data_arrays() {
  const std::string mir = slurp("tests/fixtures/structured_checks.tmir.sexp");
  const std::string good = slurp("tests/fixtures/structured_checks.json");

  // These two leaves are interleaved in DataMap storage. A contiguous slice
  // would see [.2,.1,.3] and [0,10,1], rejecting valid Stan arrays.
  check(outcome([&] {
          (void)stanli::compile_model(mir, DataMap::from_json(good));
        }) == Outcome::Accepted,
        "interleaved structured data arrays are accepted");

  const std::vector<std::pair<std::string, std::string>> invalid{
      {"\"d_simplex\": [[0.2, 0.3, 0.5]", "\"d_simplex\": [[0.2, 0.3, 0.6]"},
      {"\"d_ordered\": [[0, 1, 2]", "\"d_ordered\": [[0, 0, 2]"},
      {"\"d_positive_ordered\": [[0, 1, 2]",
       "\"d_positive_ordered\": [[-1, 1, 2]"},
      {"\"d_unit_vector\": [[1, 0, 0]", "\"d_unit_vector\": [[1, 1, 0]"},
      {"\"d_sum_to_zero_vector\": [[1, -1, 0]",
       "\"d_sum_to_zero_vector\": [[1, 0, 0]"},
      {"\"d_corr\": [[[1, 0], [0, 1]]", "\"d_corr\": [[[0.9, 0], [0, 1]]"},
      {"\"d_cov\": [[[1, 0], [0, 1]]", "\"d_cov\": [[[1, 1], [0, 1]]"},
      {"\"d_cholesky_corr\": [[[1, 0], [0, 1]]",
       "\"d_cholesky_corr\": [[[1, 0.1], [0, 1]]"},
      {"\"d_cholesky_cov\": [\n    [[1, 0], [0, 1], [0, 0]]",
       "\"d_cholesky_cov\": [\n    [[1, 0.2], [0, 1], [0, 0]]"},
      {"\"d_sum_to_zero_matrix\": [\n    [[1, 2], [-3, 0]]",
       "\"d_sum_to_zero_matrix\": [\n    [[1, 2], [-3, 1]]"},
  };
  for (const auto& [from, to] : invalid) {
    const std::string bad = replace_once(good, from, to);
    check(outcome([&] {
            (void)stanli::compile_model(mir, DataMap::from_json(bad));
          }) != Outcome::Accepted,
          "invalid structured data rejects during construction: " + from);
  }

  const std::string empty = R"({
    "B":0,
    "d_simplex":[], "d_ordered":[], "d_positive_ordered":[],
    "d_unit_vector":[], "d_sum_to_zero_vector":[], "d_corr":[],
    "d_cov":[], "d_cholesky_corr":[], "d_cholesky_cov":[],
    "d_sum_to_zero_matrix":[]
  })";
  const auto compile_empty = [&](bool disable_preload) {
    if (disable_preload)
      test_setenv("STANLI_NO_DATA_PRELOAD", "1", 1);
    else
      test_unsetenv("STANLI_NO_DATA_PRELOAD");
    const Outcome result = outcome(
        [&] { (void)stanli::compile_model(mir, DataMap::from_json(empty)); });
    test_unsetenv("STANLI_NO_DATA_PRELOAD");
    return result;
  };
  const Outcome empty_fast = compile_empty(false);
  const Outcome empty_oracle = compile_empty(true);
  check(empty_fast == Outcome::Accepted,
        "zero outer batch executes zero structured leaf checks");
  check(empty_fast == empty_oracle,
        "preloaded empty structured arrays match interpreter oracle");

  std::string malformed = mir;
  const size_t gq = malformed.find("(decl_id gq_sum_to_zero)");
  const std::string structured_tag = "(trans SumToZero)";
  const size_t check_at = malformed.find(structured_tag, gq);
  if (gq == std::string::npos || check_at == std::string::npos) {
    check(false, "malformed generated-quantity fixture mutation");
  } else {
    malformed.replace(check_at, structured_tag.size(), "(trans Correlation)");
    check(outcome([&] {
            (void)stanli::compile_model(malformed, DataMap::from_json(good));
          }) == Outcome::Other,
          "malformed generated-quantity check fails model construction");

    std::string unsupported = mir;
    const size_t unsupported_at = unsupported.find(structured_tag, gq);
    unsupported.replace(unsupported_at, structured_tag.size(),
                        "(trans UnknownCheck)");
    check(outcome([&] {
            (void)stanli::compile_model(unsupported, DataMap::from_json(good));
          }) == Outcome::Other,
          "unknown generated-quantity check fails model construction");

    std::string missing = mir;
    const size_t missing_at = missing.find(structured_tag, gq);
    missing.erase(missing_at, structured_tag.size());
    check(outcome([&] {
            (void)stanli::compile_model(missing, DataMap::from_json(good));
          }) == Outcome::Other,
          "missing generated-quantity check tag fails model construction");

    std::string wrong_depth = mir;
    const size_t depth_check = wrong_depth.find(structured_tag, gq);
    const size_t depth_at = wrong_depth.find("(type_ UVector)", depth_check);
    if (depth_at == std::string::npos) {
      check(false, "generated-quantity check depth mutation");
    } else {
      wrong_depth.replace(depth_at, std::string("(type_ UVector)").size(),
                          "(type_ (UArray UVector))");
      check(outcome([&] {
              (void)stanli::compile_model(wrong_depth,
                                          DataMap::from_json(good));
            }) == Outcome::Other,
            "wrong-depth generated-quantity check fails construction");
    }
  }
}

std::map<std::string, DataMap::Entry> wa_env(const DataMap& data) {
  std::map<std::string, DataMap::Entry> env;
  for (const char* name :
       {"B", "d_simplex", "d_ordered", "d_positive_ordered", "d_unit_vector",
        "d_sum_to_zero_vector", "d_corr", "d_cov", "d_cholesky_corr",
        "d_cholesky_cov", "d_sum_to_zero_matrix"})
    env[name] = data.at(name);
  for (const char* flag :
       {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
    DataMap::Entry one;
    one.is_int = true;
    one.i = {1};
    one.r = {1.0};
    env[flag] = std::move(one);
  }
  return env;
}

void test_runtime_phases() {
  const std::string mir = slurp("tests/fixtures/structured_checks.tmir.sexp");
  const DataMap data =
      DataMap::from_json_file("tests/fixtures/structured_checks.json");

  CompiledModel density = stanli::compile_model(mir, data);
  Executor dex(std::move(density.graph));
  density.bind(dex);
  dex.params_data()[0] = -0.25;
  dex.params_data()[1] = 0.0;
  double grad[2]{};
  bool first_check = false;
  try {
    (void)dex.gradient(grad);
  } catch (const std::domain_error& e) {
    first_check = std::string(e.what()).find("tp_simplex") != std::string::npos;
  }
  check(first_check,
        "structured check precedes a later failing scalar declaration");

  CompiledModel wa = stanli::compile_model(mir, data);
  check(wa.write_array && !wa.write_array->interp,
        "structured-check fixture has compiled write_array");
  if (wa.write_array && !wa.write_array->interp) {
    Executor wex(std::move(wa.write_array->graph));
    wa.write_array->bind(wex);
    wex.params_data()[0] = 0.0;
    wex.params_data()[1] = 1.0;
    check(outcome([&] { wex.run_forward_only(); }) == Outcome::Domain,
          "generated-quantity structured check rejects compiled write_array");
  }

  auto prog = std::make_shared<stanli::mir::Program>(
      stanli::mir::read_program(stanli::sexp::parse(mir)));
  WaInterp interpreted(prog, wa_env(data));
  std::map<std::string, DataMap::Entry> params;
  params["tp_shift"].r = {0.0};
  params["gq_shift"].r = {1.0};
  WaRng rng(1234);
  check(
      outcome([&] { (void)interpreted.eval(params, rng); }) == Outcome::Domain,
      "generated-quantity structured check rejects direct MirInterp");
}

}  // namespace

int main() {
  test_leaf_oracles();
  test_nested_layout_and_malformed();
  test_declaration_geometry_is_evaluated_once();
  test_data_arrays();
  test_runtime_phases();
  if (failures == 0) std::printf("test_structured_checks OK\n");
  return failures == 0 ? 0 : 1;
}
