// MIR reader over the transformed-MIR sexp of eight schools.
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/sexp.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

static std::string slurp(const char* path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  using namespace stanli;
  const char* fix = argc > 1 ? argv[1] : "tests/fixtures/es.tmir.sexp";
  const std::string text = slurp(fix);
  if (text.empty()) {
    std::printf("FAIL fixture missing: %s (run from repo root)\n", fix);
    return 1;
  }
  mir::Program p = mir::read_program(sexp::parse(text));

  // data block
  check(p.input_vars.size() == 3, "3 input vars");
  check(p.input_vars[0].first == "J" && p.input_vars[0].second.base == "SInt",
        "J is SInt");
  check(p.input_vars[1].first == "y" &&
            p.input_vars[1].second.base == "SVector" &&
            p.input_vars[1].second.dims.size() == 1 &&
            p.input_vars[1].second.dims[0].kind == mir::Expr::Var &&
            p.input_vars[1].second.dims[0].name == "J",
        "y is SVector[J]");

  // log_prob: find param decls
  int n_read = 0, n_target = 0;
  const mir::Stmt* tau_decl = nullptr;
  const mir::Stmt* theta_assign = nullptr;
  std::function<void(const mir::Stmt&)> walk = [&](const mir::Stmt& s) {
    if (s.kind == mir::Stmt::Decl && s.read_transform) {
      ++n_read;
      if (s.decl_id == "tau") tau_decl = &s;
    }
    if (s.kind == mir::Stmt::Assignment && s.lhs == "theta") theta_assign = &s;
    if (s.kind == mir::Stmt::TargetPE) ++n_target;
    for (const auto& k : s.body) walk(k);
  };
  for (const auto& s : p.log_prob) walk(s);

  check(n_read == 3, "3 parameter reads");
  check(tau_decl != nullptr, "tau decl found");
  if (tau_decl) {
    check(tau_decl->read_transform->kind == mir::Transform::Lower,
          "tau lower transform");
    check(tau_decl->read_transform->args.size() == 1 &&
              tau_decl->read_transform->args[0].kind == mir::Expr::LitInt &&
              tau_decl->read_transform->args[0].lit_i == 0,
          "tau lower bound 0");
  }
  check(theta_assign != nullptr, "theta assignment found");
  if (theta_assign) {
    const mir::Expr& r = theta_assign->rhs;
    check(r.kind == mir::Expr::FunApp && r.name == "Plus__", "theta rhs plus");
    check(r.args.size() == 2 && r.args[1].kind == mir::Expr::FunApp &&
              r.args[1].name == "Times__",
          "theta rhs times");
    check(r.args[0].kind == mir::Expr::Var && r.args[0].name == "mu" &&
              !r.args[0].data_only,
          "mu var autodiff");
  }
  check(n_target == 4, "4 TargetPE statements");

  // propto flags: all four tildes emit FnLpdf true
  int n_propto = 0;
  std::function<void(const mir::Expr&)> ewalk = [&](const mir::Expr& e) {
    if (e.kind == mir::Expr::FunApp && e.fn_propto) ++n_propto;
    for (const auto& a : e.args) ewalk(a);
  };
  std::function<void(const mir::Stmt&)> swalk = [&](const mir::Stmt& s) {
    if (s.kind == mir::Stmt::TargetPE) ewalk(s.target);
    for (const auto& k : s.body) swalk(k);
  };
  for (const auto& s : p.log_prob) swalk(s);
  check(n_propto == 4, "4 propto lpdf calls");

  // A scalar broadcasts over the container's logical geometry, including a
  // zero-width vector. Flat width alone cannot identify the scalar when the
  // vector has zero or one element.
  {
    std::map<std::string, const mir::FunDef*> functions;
    MirInterp<double> interp(functions, "broadcast test");
    auto real_value = [&](const std::string& name, std::vector<double> values,
                          std::vector<int64_t> dims) {
      DataMap::Entry value;
      value.r = std::move(values);
      value.dims = std::move(dims);
      interp.env()[name] = std::move(value);
    };
    DataMap::Entry empty;
    empty.dims = {0};
    interp.env()["empty"] = empty;
    real_value("one", {3.0}, {1});

    mir::Expr scalar;
    scalar.kind = mir::Expr::LitReal;
    scalar.lit = 2.0;
    scalar.type_ = "UReal";
    scalar.unsized.leaf = mir::UnsizedLeaf::Real;
    scalar.data_only = true;
    for (const std::string& name : {"Plus__", "Times__"}) {
      for (const std::string& variable : {"empty", "one"}) {
        mir::Expr vector;
        vector.kind = mir::Expr::Var;
        vector.name = variable;
        vector.type_ = "UVector";
        vector.unsized.leaf = mir::UnsizedLeaf::Vector;
        vector.data_only = true;
        for (bool scalar_first : {false, true}) {
          mir::Expr call;
          call.kind = mir::Expr::FunApp;
          call.name = name;
          call.type_ = "UVector";
          call.unsized.leaf = mir::UnsizedLeaf::Vector;
          call.data_only = true;
          call.args = scalar_first ? std::vector<mir::Expr>{scalar, vector}
                                   : std::vector<mir::Expr>{vector, scalar};
          const DataMap::Entry got = interp.eval(call);
          const size_t want_size = variable == "empty" ? 0 : 1;
          check(got.r.size() == want_size && got.dims.size() == 1 &&
                    got.dims[0] == (int64_t)want_size,
                name + " scalar/vector geometry");
          if (want_size)
            check(got.r[0] == (name == "Plus__" ? 5.0 : 6.0),
                  name + " scalar/vector value");
        }
      }
    }
    for (const std::string& variable : {"empty", "one"}) {
      mir::Expr vector;
      vector.kind = mir::Expr::Var;
      vector.name = variable;
      vector.type_ = "UVector";
      vector.unsized.leaf = mir::UnsizedLeaf::Vector;
      vector.data_only = true;
      for (int container_arg = 0; container_arg < 3; ++container_arg) {
        mir::Expr call;
        call.kind = mir::Expr::FunApp;
        call.name = "fma";
        call.type_ = "UVector";
        call.unsized.leaf = mir::UnsizedLeaf::Vector;
        call.data_only = true;
        call.args = {scalar, scalar, scalar};
        call.args[(size_t)container_arg] = vector;
        const DataMap::Entry got = interp.eval(call);
        const size_t want_size = variable == "empty" ? 0 : 1;
        check(got.r.size() == want_size && got.dims.size() == 1 &&
                  got.dims[0] == (int64_t)want_size,
              "fma scalar/vector geometry");
        if (want_size)
          check(got.r[0] == (container_arg == 2 ? 7.0 : 8.0),
                "fma scalar/vector value");
      }
    }

    // Two-argument log_sum_exp / log_diff_exp are elementwise with scalar
    // broadcast; only the one-argument log_sum_exp is a reduction. Taking
    // the two-argument form for a scalar op silently produced one value
    // where the declaration asked for N, which the transformed data block
    // reaches directly (log_prob then reads N-1 uninitialized elements).
    real_value("three", {1.0, 2.0, 3.0}, {3});
    {
      mir::Expr three;
      three.kind = mir::Expr::Var;
      three.name = "three";
      three.type_ = "UVector";
      three.unsized.leaf = mir::UnsizedLeaf::Vector;
      three.data_only = true;
      for (const std::string& name : {"log_sum_exp", "log_diff_exp"}) {
        for (bool scalar_first : {false, true}) {
          mir::Expr call;
          call.kind = mir::Expr::FunApp;
          call.name = name;
          call.type_ = "UVector";
          call.unsized.leaf = mir::UnsizedLeaf::Vector;
          call.data_only = true;
          call.args = scalar_first ? std::vector<mir::Expr>{scalar, three}
                                   : std::vector<mir::Expr>{three, scalar};
          const DataMap::Entry got = interp.eval(call);
          bool ok = got.r.size() == 3 && got.dims == std::vector<int64_t>{3};
          for (size_t i = 0; ok && i < 3; ++i) {
            // log_diff_exp is only finite in one operand order, so the
            // other direction is checked as the NaN stan-math returns.
            const double x = (double)(i + 1), y = 2.0;
            const double lo = scalar_first ? y : x, hi = scalar_first ? x : y;
            const double want = name == "log_sum_exp"
                                    ? stan::math::log_sum_exp(lo, hi)
                                    : stan::math::log_diff_exp(lo, hi);
            ok = got.r[i] == want || (std::isnan(got.r[i]) && std::isnan(want));
          }
          check(ok, name + " broadcasts over the container operand");
        }
      }
      // The one-argument form stays the reduction it always was.
      mir::Expr reduce;
      reduce.kind = mir::Expr::FunApp;
      reduce.name = "log_sum_exp";
      reduce.type_ = "UReal";
      reduce.unsized.leaf = mir::UnsizedLeaf::Real;
      reduce.data_only = true;
      reduce.args = {three};
      const DataMap::Entry got = interp.eval(reduce);
      // The interpreter shifts by the max and stan-math does not, so this
      // is a value check, not a bitwise one.
      check(got.r.size() == 1 &&
                std::abs(got.r[0] - stan::math::log_sum_exp(std::vector<double>{
                                        1.0, 2.0, 3.0})) < 1e-14,
            "one-argument log_sum_exp still reduces");
    }

    auto variable = [](const std::string& name, const std::string& type) {
      mir::Expr e;
      e.kind = mir::Expr::Var;
      e.name = name;
      e.type_ = type;
      e.data_only = true;
      return e;
    };
    auto times = [&](const std::string& lhs, const std::string& lhs_type,
                     const std::string& rhs, const std::string& rhs_type,
                     const std::string& result_type) {
      mir::Expr e;
      e.kind = mir::Expr::FunApp;
      e.name = "Times__";
      e.type_ = result_type;
      e.data_only = true;
      e.args = {variable(lhs, lhs_type), variable(rhs, rhs_type)};
      return interp.eval(e);
    };
    real_value("m11", {2.0}, {1, 1});
    real_value("m12", {3.0, 4.0}, {1, 2});
    DataMap::Entry matrix =
        times("m11", "UMatrix", "m12", "UMatrix", "UMatrix");
    check(matrix.dims == std::vector<int64_t>({1, 2}) &&
              matrix.r == std::vector<double>({6.0, 8.0}),
          "Times__ width-one matrix product");
    real_value("rv1", {2.0}, {1});
    real_value("v1", {3.0}, {1});
    DataMap::Entry dot = times("rv1", "URowVector", "v1", "UVector", "UReal");
    check(dot.dims.empty() && dot.r == std::vector<double>({6.0}),
          "Times__ width-one dot product");

    real_value("v2", {4.0, 5.0}, {2});
    auto refuses_elementwise =
        [&](const std::string& name, const std::string& lhs,
            const std::string& rhs, const std::string& type) {
          mir::Expr call;
          call.kind = mir::Expr::FunApp;
          call.name = name;
          call.type_ = type;
          call.data_only = true;
          call.args = {variable(lhs, type), variable(rhs, type)};
          if (name == "fma") call.args.push_back(scalar);
          bool refused = false;
          try {
            (void)interp.eval(call);
          } catch (const std::exception&) {
            refused = true;
          }
          return refused;
        };
    for (const std::string& name : {"Plus__", "fma"}) {
      check(refuses_elementwise(name, "one", "v2", "UVector"),
            name + " refuses vector[1]/vector[2] broadcast");
    }

    real_value("m23", {1, 2, 3, 4, 5, 6}, {2, 3});
    real_value("m32", {1, 2, 3, 4, 5, 6}, {3, 2});
    real_value("m03", {}, {0, 3});
    real_value("m02", {}, {0, 2});
    for (const std::string& name : {"Plus__", "fma"}) {
      check(refuses_elementwise(name, "m23", "m32", "UMatrix"),
            name + " refuses equal-width unequal matrix shapes");
      check(refuses_elementwise(name, "m03", "m02", "UMatrix"),
            name + " refuses unequal zero-width matrix shapes");
    }

    // The positional ODE fallback entry point reconstructs scalar/container
    // geometry from each formal rather than flattening every argument.
    mir::FunDef scale;
    scale.name = "scale";
    scale.arg_names = {"a", "x"};
    scale.arg_views = {{0, mir::UnsizedLeaf::Real},
                       {0, mir::UnsizedLeaf::Vector}};
    scale.arg_types = {"UReal", "UVector"};
    mir::Stmt returned;
    returned.kind = mir::Stmt::Return;
    returned.has_init = true;
    returned.rhs.kind = mir::Expr::FunApp;
    returned.rhs.name = "Times__";
    returned.rhs.type_ = "UVector";
    returned.rhs.args = {variable("a", "UReal"), variable("x", "UVector")};
    scale.body = {returned};
    const std::vector<double> scaled =
        interp.call(scale, {{2.0}, {3.0, 4.0}}, {});
    check(scaled == std::vector<double>({6.0, 8.0}),
          "ODE call scalar formal broadcasts over vector");

    // stanc may reconstruct a scalar data declaration through a flat
    // FnReadData assignment. The declaration still owns scalar geometry;
    // treating its one value as vector[1] breaks later scalar broadcasts.
    DataMap::Entry scale_data;
    scale_data.r = {2.0};
    MirHooks hooks;
    hooks.data = [&](const std::string& name) {
      return name == "scale_data" ? &scale_data : nullptr;
    };
    MirInterp<double> read_interp(functions, "scalar data read", hooks);
    mir::Stmt scale_decl;
    scale_decl.kind = mir::Stmt::Decl;
    scale_decl.decl_id = "scale_data";
    scale_decl.decl_type.base = "SReal";
    mir::Stmt scale_read;
    scale_read.kind = mir::Stmt::Assignment;
    scale_read.lhs = "scale_data";
    scale_read.rhs.kind = mir::Expr::FunApp;
    scale_read.rhs.name = "FnReadData";
    scale_read.rhs.fn_lib = mir::Expr::Lib::Internal;
    mir::Expr data_name;
    data_name.kind = mir::Expr::LitStr;
    data_name.lit_s = "scale_data";
    scale_read.rhs.args = {data_name};
    read_interp.run({scale_decl, scale_read});
    check(read_interp.env().at("scale_data").dims.empty(),
          "flat data read preserves declared scalar geometry");
  }

  if (failures == 0) std::printf("test_mir OK\n");
  return failures == 0 ? 0 : 1;
}
