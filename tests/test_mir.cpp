// MIR reader over the transformed-MIR sexp of eight schools.
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/sexp.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
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
    // O1 contracts `mu + tau * theta_tilde` into fma(tau, theta_tilde, mu).
    const mir::Expr& r = theta_assign->rhs;
    check(r.kind == mir::Expr::FunApp && r.name == "fma", "theta rhs fma");
    check(r.args.size() == 3 && r.args[0].kind == mir::Expr::Var &&
              r.args[0].name == "tau" && r.args[1].kind == mir::Expr::Var &&
              r.args[1].name == "theta_tilde",
          "theta rhs fma operands");
    check(r.args.size() == 3 && r.args[2].kind == mir::Expr::Var &&
              r.args[2].name == "mu" && !r.args[2].data_only,
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

  // stanc3 prints the open-ended `x[4:]` index as Upfrom. Keep that node
  // intact for the interpreter rather than turning the whole expression
  // into Unsupported in the MIR reader.
  {
    const std::string upfrom_text = R"sexp(
((input_vars ())
 (log_prob
  (((pattern
     (Assignment ((LVariable y) ()) (UArray UReal)
      ((pattern
        (Indexed
         ((pattern (Var x))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
         ((Upfrom
           ((pattern (Lit Int 4))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>)))))
)sexp";
    const mir::Program upfrom_program =
        mir::read_program(sexp::parse(upfrom_text));
    const mir::Expr* parsed = upfrom_program.log_prob.size() == 1
                                  ? &upfrom_program.log_prob[0].rhs
                                  : nullptr;
    check(parsed && parsed->kind == mir::Expr::Indexed &&
              parsed->args.size() == 2 &&
              parsed->args[1].name == "IndexUpfrom" &&
              parsed->args[1].args.size() == 1 &&
              parsed->args[1].args[0].kind == mir::Expr::LitInt &&
              parsed->args[1].args[0].lit_i == 4,
          "MIR reader preserves an Upfrom index");
  }

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

    // Stan's bound transforms called as functions. The interpreter serves
    // transformed data, which reaches them directly, and it instantiates
    // `jacobian__ = false`, so the `_jacobian` direction is the constrained
    // value with no other effect. Bounds broadcast: shared or one per
    // element, exactly as the lowered kernels take them.
    real_value("bounds", {1.5, 2.5, 3.5}, {3});
    {
      const auto container = [](const std::string& name) {
        mir::Expr e;
        e.kind = mir::Expr::Var;
        e.name = name;
        e.type_ = "UVector";
        e.unsized.leaf = mir::UnsizedLeaf::Vector;
        e.data_only = true;
        return e;
      };
      const mir::Expr three = container("three");
      const mir::Expr bounds = container("bounds");
      mir::Expr upper = scalar;
      upper.lit = 5.0;
      // three = {1,2,3}, bounds = {1.5,2.5,3.5}, scalar = 2.0. The values
      // keep every call inside its transform's support: three < bounds for
      // the upper direction, and the lower direction's free argument is
      // built from its own constrained result.
      struct Case {
        const char* name;
        bool shared_bound;
        double (*want)(double, double, double);
      };
      const Case cases[] = {
          {"lower_bound_constrain", true,
           [](double x, double b, double) {
             return stan::math::lb_constrain(x, b);
           }},
          {"lower_bound_jacobian", false,
           [](double x, double b, double) {
             return stan::math::lb_constrain(x, b);
           }},
          {"upper_bound_constrain", false,
           [](double x, double b, double) {
             return stan::math::ub_constrain(x, b);
           }},
          {"upper_bound_unconstrain", false,
           [](double x, double b, double) {
             return stan::math::ub_free(x, b);
           }},
          {"lower_upper_bound_jacobian", true,
           [](double x, double b, double c) {
             return stan::math::lub_constrain(x, b, c);
           }},
          {"offset_multiplier_unconstrain", false,
           [](double x, double b, double c) {
             return stan::math::offset_multiplier_free(x, b, c);
           }},
      };
      for (const Case& c : cases) {
        const bool ternary = std::string(c.name).find("lower_upper") == 0 ||
                             std::string(c.name).find("offset_") == 0;
        mir::Expr call;
        call.kind = mir::Expr::FunApp;
        call.name = c.name;
        call.type_ = "UVector";
        call.unsized.leaf = mir::UnsizedLeaf::Vector;
        call.data_only = true;
        // The second bound is always shared, so the ternary transforms
        // cover a mixed pair as well as a uniform one.
        call.args = {three, c.shared_bound ? scalar : bounds};
        if (ternary) call.args.push_back(upper);
        const DataMap::Entry got = interp.eval(call);
        bool ok = got.r.size() == 3 && got.dims == std::vector<int64_t>{3};
        for (size_t i = 0; ok && i < 3; ++i) {
          const double b = c.shared_bound ? 2.0 : 1.5 + (double)i;
          ok = got.r[i] == c.want((double)(i + 1), b, 5.0);
        }
        check(ok, std::string(c.name) +
                      " is elementwise with a broadcast "
                      "bound");
      }
      // The scalar shape, where the jacobian direction still returns only
      // the constrained value.
      mir::Expr call;
      call.kind = mir::Expr::FunApp;
      call.name = "lower_bound_jacobian";
      call.type_ = "UReal";
      call.unsized.leaf = mir::UnsizedLeaf::Real;
      call.data_only = true;
      call.args = {scalar, scalar};
      const DataMap::Entry got = interp.eval(call);
      check(got.r.size() == 1 && got.r[0] == stan::math::lb_constrain(2.0, 2.0),
            "a scalar bound transform stays one value");
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

    // Positional routing is carried by the structural views. Type spellings
    // are retained for diagnostics and must not be searched for type names.
    mir::FunDef routed;
    routed.name = "routed";
    routed.arg_names = {"i", "x"};
    routed.arg_views = {{0, mir::UnsizedLeaf::Int},
                        {0, mir::UnsizedLeaf::Real}};
    routed.arg_types = {"OpaqueInt", "OpaqueRealWithUIntAnnotation"};
    mir::Stmt routed_return;
    routed_return.kind = mir::Stmt::Return;
    routed_return.has_init = true;
    routed_return.rhs.kind = mir::Expr::FunApp;
    routed_return.rhs.name = "Minus__";
    routed_return.rhs.type_ = "UReal";
    routed_return.rhs.args = {variable("i", "UInt"), variable("x", "UReal")};
    routed.body = {routed_return};
    const std::vector<double> routed_value =
        interp.call(routed, {{2.5}}, {{4}});
    check(routed_value == std::vector<double>({1.5}),
          "ODE call routes integer arguments from structural views");

    // The same positional entry point can evaluate a fallback RHS/UDF with a
    // product over a vector formal.  A formal can be bound to a shifted Eigen
    // view, so it must retain MirInterp's legacy ascending fold rather than
    // being classified as an owning top-level packet vector.  These factors
    // distinguish the two groupings: ascending gives 3, while packet pairing
    // forms inf*0 and produces NaN.
    mir::FunDef reduce;
    reduce.name = "reduce";
    reduce.arg_names = {"x"};
    reduce.arg_views = {{0, mir::UnsizedLeaf::Vector}};
    reduce.arg_types = {"UVector"};
    mir::Expr formal;
    formal.kind = mir::Expr::Var;
    formal.name = "x";
    formal.type_ = "UVector";
    formal.unsized.leaf = mir::UnsizedLeaf::Vector;
    formal.data_only = true;
    mir::Stmt reduced_return;
    reduced_return.kind = mir::Stmt::Return;
    reduced_return.has_init = true;
    reduced_return.rhs.kind = mir::Expr::FunApp;
    reduced_return.rhs.fn_lib = mir::Expr::Lib::StanLib;
    reduced_return.rhs.name = "prod";
    reduced_return.rhs.type_ = "UReal";
    reduced_return.rhs.unsized.leaf = mir::UnsizedLeaf::Real;
    reduced_return.rhs.data_only = true;
    reduced_return.rhs.args = {formal};
    reduce.body = {reduced_return};
    const std::vector<double> reduced =
        interp.call(reduce, {{1e200, 1e-200, 1e200, 1e-200, 3.0}}, {});
    check(reduced == std::vector<double>{3.0},
          "ODE call vector formal retains scalar product grouping");

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

  // A partial list of leading scalar indices removes exactly those array
  // dimensions and keeps the remaining container geometry.  `mother.stan`
  // reaches the two-index case as `array[4,5] matrix[2,3] x; x[i,j]`.
  // Storage is first-index-fast over all four dimensions, including the
  // matrix's column-major row/column pair.
  {
    std::map<std::string, const mir::FunDef*> functions;
    MirInterp<double> interp(functions, "leading index slice test");
    DataMap::Entry x;
    x.dims = {4, 5, 2, 3};
    for (int col = 1; col <= 3; ++col)
      for (int row = 1; row <= 2; ++row)
        for (int b = 1; b <= 5; ++b)
          for (int a = 1; a <= 4; ++a)
            x.r.push_back(1000.0 * a + 100.0 * b + 10.0 * row + col);
    interp.env()["x"] = std::move(x);

    auto indexed = [](const std::vector<long>& indices, const std::string& type,
                      mir::UnsizedView view) {
      mir::Expr base;
      base.kind = mir::Expr::Var;
      base.name = "x";
      base.type_ = "(UArray (UArray UMatrix))";
      base.unsized = {2, mir::UnsizedLeaf::Matrix};
      base.data_only = true;

      mir::Expr e;
      e.kind = mir::Expr::Indexed;
      e.args.push_back(std::move(base));
      for (long value : indices) {
        mir::Expr literal;
        literal.kind = mir::Expr::LitInt;
        literal.lit_i = value;
        literal.type_ = "UInt";
        literal.unsized.leaf = mir::UnsizedLeaf::Int;
        literal.data_only = true;
        mir::Expr index;
        index.kind = mir::Expr::FunApp;
        index.name = "IndexSingle";
        index.args.push_back(std::move(literal));
        e.args.push_back(std::move(index));
      }
      e.type_ = type;
      e.unsized = view;
      e.data_only = true;
      return e;
    };

    const DataMap::Entry outer = interp.eval(
        indexed({2}, "(UArray UMatrix)", {1, mir::UnsizedLeaf::Matrix}));
    std::vector<double> outer_values;
    for (int col = 1; col <= 3; ++col)
      for (int row = 1; row <= 2; ++row)
        for (int b = 1; b <= 5; ++b)
          outer_values.push_back(2000.0 + 100.0 * b + 10.0 * row + col);
    check(outer.dims == std::vector<int64_t>({5, 2, 3}) &&
              outer.r == outer_values,
          "one leading index preserves array[5] matrix[2,3]");

    const DataMap::Entry matrix =
        interp.eval(indexed({2, 3}, "UMatrix", {0, mir::UnsizedLeaf::Matrix}));
    check(matrix.dims == std::vector<int64_t>({2, 3}) &&
              matrix.r ==
                  std::vector<double>({2311, 2321, 2312, 2322, 2313, 2323}),
          "two leading indices return the selected matrix column-major");

    auto integer = [](long value) {
      mir::Expr e;
      e.kind = mir::Expr::LitInt;
      e.lit_i = value;
      e.type_ = "UInt";
      e.unsized.leaf = mir::UnsizedLeaf::Int;
      e.data_only = true;
      return e;
    };
    auto single = [&](long value) {
      mir::Expr e;
      e.kind = mir::Expr::FunApp;
      e.name = "IndexSingle";
      e.args = {integer(value)};
      return e;
    };
    auto between = [&](long lo, long hi) {
      mir::Expr e;
      e.kind = mir::Expr::FunApp;
      e.name = "IndexBetween";
      e.args = {integer(lo), integer(hi)};
      return e;
    };
    auto all = [] {
      mir::Expr e;
      e.kind = mir::Expr::FunApp;
      e.name = "IndexAll";
      return e;
    };
    auto multi = [](const std::string& name) {
      mir::Expr positions;
      positions.kind = mir::Expr::Var;
      positions.name = name;
      positions.type_ = "(UArray UInt)";
      positions.unsized = {1, mir::UnsizedLeaf::Int};
      positions.data_only = true;
      mir::Expr e;
      e.kind = mir::Expr::FunApp;
      e.name = "IndexMulti";
      e.args = {std::move(positions)};
      return e;
    };
    auto upfrom = [&](long lo) {
      mir::Expr e;
      e.kind = mir::Expr::FunApp;
      e.name = "IndexUpfrom";
      e.args = {integer(lo)};
      return e;
    };

    auto int_array = [&](std::vector<mir::Expr> values, uint8_t depth) {
      mir::Expr e;
      e.kind = mir::Expr::FunApp;
      e.fn_lib = mir::Expr::Lib::Internal;
      e.name = "FnMakeArray";
      e.type_ = "UArray";
      e.unsized = {depth, mir::UnsizedLeaf::Int};
      e.data_only = true;
      e.args = std::move(values);
      return e;
    };
    const DataMap::Entry nested_int_literal =
        interp.eval(int_array({int_array({integer(1), integer(2)}, 1),
                               int_array({integer(3), integer(4)}, 1)},
                              2));
    check(nested_int_literal.is_int &&
              nested_int_literal.dims == std::vector<int64_t>({2, 2}) &&
              nested_int_literal.i == std::vector<int>({1, 3, 2, 4}) &&
              nested_int_literal.r == std::vector<double>({1, 3, 2, 4}),
          "nested integer literals retain shape and integer provenance");

    // These are the five mixed-index reads in mother.stan's optimized
    // generated-quantities MIR. The runtime value has one unified
    // first-index-fast layout: outer array, matrix row, matrix column.
    DataMap::Entry indexing;
    indexing.dims = {5, 3, 4};
    for (int col = 1; col <= 4; ++col)
      for (int row = 1; row <= 3; ++row)
        for (int outer_ix = 1; outer_ix <= 5; ++outer_ix)
          indexing.r.push_back(100.0 * outer_ix + 10.0 * row + col);
    interp.env()["indexing"] = std::move(indexing);
    DataMap::Entry gather_indices;
    gather_indices.is_int = true;
    gather_indices.dims = {3};
    gather_indices.i = {2, 3, 1};
    gather_indices.r.assign(gather_indices.i.begin(), gather_indices.i.end());
    interp.env()["gather_indices"] = std::move(gather_indices);
    auto mixed = [](std::vector<mir::Expr> indices) {
      mir::Expr base;
      base.kind = mir::Expr::Var;
      base.name = "indexing";
      base.type_ = "(UArray UMatrix)";
      base.unsized = {1, mir::UnsizedLeaf::Matrix};
      base.data_only = true;
      mir::Expr e;
      e.kind = mir::Expr::Indexed;
      e.args.push_back(std::move(base));
      for (auto& index : indices) e.args.push_back(std::move(index));
      e.data_only = true;
      return e;
    };
    const auto cell = [](int outer_ix, int row, int col) {
      return 100.0 * outer_ix + 10.0 * row + col;
    };

    const DataMap::Entry gathered =
        interp.eval(mixed({multi("gather_indices"), multi("gather_indices")}));
    std::vector<double> gathered_want;
    for (int col = 1; col <= 4; ++col)
      for (int row : {2, 3, 1})
        for (int outer_ix : {2, 3, 1})
          gathered_want.push_back(cell(outer_ix, row, col));
    check(gathered.dims == std::vector<int64_t>({3, 3, 4}) &&
              gathered.r == gathered_want,
          "N-D Multi/Multi read preserves gather order and matrix columns");

    const DataMap::Entry all_gathered =
        interp.eval(mixed({all(), multi("gather_indices")}));
    std::vector<double> all_gathered_want;
    for (int col = 1; col <= 4; ++col)
      for (int row : {2, 3, 1})
        for (int outer_ix = 1; outer_ix <= 5; ++outer_ix)
          all_gathered_want.push_back(cell(outer_ix, row, col));
    check(all_gathered.dims == std::vector<int64_t>({5, 3, 4}) &&
              all_gathered.r == all_gathered_want,
          "N-D All/Multi read keeps the outer array extent");

    const DataMap::Entry gathered_all_gathered = interp.eval(
        mixed({multi("gather_indices"), all(), multi("gather_indices")}));
    std::vector<double> gathered_all_gathered_want;
    for (int col : {2, 3, 1})
      for (int row = 1; row <= 3; ++row)
        for (int outer_ix : {2, 3, 1})
          gathered_all_gathered_want.push_back(cell(outer_ix, row, col));
    check(gathered_all_gathered.dims == std::vector<int64_t>({3, 3, 3}) &&
              gathered_all_gathered.r == gathered_all_gathered_want,
          "N-D Multi/All/Multi read gathers nonadjacent dimensions");

    const DataMap::Entry ranged =
        interp.eval(mixed({between(1, 3), single(1)}));
    std::vector<double> ranged_want;
    for (int col = 1; col <= 4; ++col)
      for (int outer_ix = 1; outer_ix <= 3; ++outer_ix)
        ranged_want.push_back(cell(outer_ix, 1, col));
    check(
        ranged.dims == std::vector<int64_t>({3, 4}) && ranged.r == ranged_want,
        "N-D Between/Single read leaves the trailing row-vector axis");

    const DataMap::Entry tail =
        interp.eval(mixed({upfrom(4), between(2, 3), single(1)}));
    std::vector<double> tail_want;
    for (int row = 2; row <= 3; ++row)
      for (int outer_ix = 4; outer_ix <= 5; ++outer_ix)
        tail_want.push_back(cell(outer_ix, row, 1));
    check(tail.dims == std::vector<int64_t>({2, 2}) && tail.r == tail_want,
          "N-D Upfrom/Between/Single read drops the scalar column axis");

    // mother builds its loop reference results as idx_res[i,j] = row. A
    // partial list of leading scalar LHS indices therefore replaces the
    // complete trailing container at a first-index-fast stride. Exercise an
    // integer value so both storage mirrors must move together.
    DataMap::Entry partial_ints;
    partial_ints.is_int = true;
    partial_ints.dims = {3, 2, 4};
    for (int value = 1; value <= 24; ++value) partial_ints.i.push_back(value);
    partial_ints.r.assign(partial_ints.i.begin(), partial_ints.i.end());
    interp.env()["partial_ints"] = std::move(partial_ints);
    DataMap::Entry partial_replacement;
    partial_replacement.is_int = true;
    partial_replacement.dims = {4};
    partial_replacement.i = {901, 902, 903, 904};
    partial_replacement.r.assign(partial_replacement.i.begin(),
                                 partial_replacement.i.end());
    interp.env()["partial_replacement"] = std::move(partial_replacement);
    mir::Stmt partial_write;
    partial_write.kind = mir::Stmt::Assignment;
    partial_write.lhs = "partial_ints";
    partial_write.lhs_idx = {single(2), single(1)};
    partial_write.rhs.kind = mir::Expr::Var;
    partial_write.rhs.name = "partial_replacement";
    partial_write.rhs.type_ = "(UArray UInt)";
    partial_write.rhs.unsized = {1, mir::UnsizedLeaf::Int};
    partial_write.rhs.data_only = true;
    interp.run({partial_write});
    std::vector<int> partial_want;
    for (int value = 1; value <= 24; ++value) partial_want.push_back(value);
    for (size_t k = 0; k < 4; ++k) partial_want[1 + 6 * k] = 901 + (int)k;
    const DataMap::Entry& partial_written = interp.env().at("partial_ints");
    check(
        partial_written.dims == std::vector<int64_t>({3, 2, 4}) &&
            partial_written.i == partial_want &&
            partial_written.r ==
                std::vector<double>(partial_want.begin(), partial_want.end()),
        "partial leading-Single assignment writes the trailing int container");

    auto assignment_refused = [&](const mir::Stmt& assignment) {
      try {
        interp.run({assignment});
      } catch (const std::exception&) {
        return true;
      }
      return false;
    };

    // A one-dimensional All on the LHS replaces the complete value while
    // retaining the destination's geometry and integer payload. Exercise
    // both vector orientations, scalar/container arrays, and the mismatch
    // checks directly in the semantic fallback.
    auto put_real = [&](const std::string& name, std::vector<double> values,
                        std::vector<int64_t> dims) {
      DataMap::Entry entry;
      entry.r = std::move(values);
      entry.dims = std::move(dims);
      interp.env()[name] = std::move(entry);
    };
    auto full_write = [&](const std::string& lhs, const std::string& rhs,
                          const std::string& type, mir::UnsizedView view) {
      mir::Stmt assignment;
      assignment.kind = mir::Stmt::Assignment;
      assignment.lhs = lhs;
      assignment.lhs_idx = {all()};
      assignment.rhs.kind = mir::Expr::Var;
      assignment.rhs.name = rhs;
      assignment.rhs.type_ = type;
      assignment.rhs.unsized = view;
      assignment.rhs.data_only = true;
      return assignment;
    };

    put_real("span_vector", {0, 0, 0}, {3});
    put_real("span_vector_rhs", {1, 2, 3}, {3});
    interp.run({full_write("span_vector", "span_vector_rhs", "UVector",
                           {0, mir::UnsizedLeaf::Vector})});
    check(
        interp.env().at("span_vector").dims == std::vector<int64_t>({3}) &&
            interp.env().at("span_vector").r == std::vector<double>({1, 2, 3}),
        "full-span vector assignment");

    put_real("span_row", {0, 0, 0}, {3});
    put_real("span_row_rhs", {4, 5, 6}, {3});
    interp.run({full_write("span_row", "span_row_rhs", "URowVector",
                           {0, mir::UnsizedLeaf::RowVector})});
    check(interp.env().at("span_row").dims == std::vector<int64_t>({3}) &&
              interp.env().at("span_row").r == std::vector<double>({4, 5, 6}),
          "full-span row-vector assignment");

    DataMap::Entry span_ints;
    span_ints.is_int = true;
    span_ints.dims = {3};
    span_ints.i = {0, 0, 0};
    span_ints.r = {0, 0, 0};
    interp.env()["span_ints"] = std::move(span_ints);
    DataMap::Entry span_ints_rhs;
    span_ints_rhs.is_int = true;
    span_ints_rhs.dims = {3};
    span_ints_rhs.i = {7, 8, 9};
    span_ints_rhs.r = {7, 8, 9};
    interp.env()["span_ints_rhs"] = std::move(span_ints_rhs);
    interp.run({full_write("span_ints", "span_ints_rhs", "UArray",
                           {1, mir::UnsizedLeaf::Int})});
    const DataMap::Entry& full_ints = interp.env().at("span_ints");
    check(full_ints.is_int && full_ints.dims == std::vector<int64_t>({3}) &&
              full_ints.i == std::vector<int>({7, 8, 9}) &&
              full_ints.r == std::vector<double>({7, 8, 9}),
          "full-span int-array assignment retains both payloads");

    put_real("span_nested", {0, 0, 0, 0}, {2, 2});
    put_real("span_nested_rhs", {11, 12, 21, 22}, {2, 2});
    interp.run({full_write("span_nested", "span_nested_rhs", "UArray",
                           {1, mir::UnsizedLeaf::Vector})});
    check(interp.env().at("span_nested").dims == std::vector<int64_t>({2, 2}) &&
              interp.env().at("span_nested").r ==
                  std::vector<double>({11, 12, 21, 22}),
          "full-span array-container assignment");

    put_real("span_short", {0, 0, 0}, {3});
    put_real("span_short_rhs", {1, 2}, {2});
    check(
        assignment_refused(full_write("span_short", "span_short_rhs", "UVector",
                                      {0, mir::UnsizedLeaf::Vector})),
        "full-span assignment checks storage length");
    put_real("span_shape", {0, 0, 0, 0}, {2, 2});
    put_real("span_shape_rhs", {1, 2, 3, 4}, {4});
    check(
        assignment_refused(full_write("span_shape", "span_shape_rhs", "UArray",
                                      {1, mir::UnsizedLeaf::Vector})),
        "full-span assignment checks logical shape");

    // Keep the pre-existing matrix column form on its own two-index path.
    put_real("span_matrix", {1, 2, 3, 4}, {2, 2});
    put_real("span_column", {9, 8}, {2});
    mir::Stmt column_write = full_write("span_matrix", "span_column", "UVector",
                                        {0, mir::UnsizedLeaf::Vector});
    column_write.lhs_idx = {all(), single(2)};
    interp.run({column_write});
    check(interp.env().at("span_matrix").r == std::vector<double>({1, 2, 9, 8}),
          "matrix column assignment remains distinct from full-span");

    mir::Stmt out_of_bounds = partial_write;
    out_of_bounds.lhs_idx = {single(4), single(1)};
    check(assignment_refused(out_of_bounds),
          "partial leading-Single assignment checks each fixed index");
    DataMap::Entry short_replacement;
    short_replacement.is_int = true;
    short_replacement.dims = {3};
    short_replacement.i = {7, 8, 9};
    short_replacement.r.assign(short_replacement.i.begin(),
                               short_replacement.i.end());
    interp.env()["short_replacement"] = std::move(short_replacement);
    mir::Stmt wrong_size = partial_write;
    wrong_size.rhs.name = "short_replacement";
    check(assignment_refused(wrong_size),
          "partial leading-Single assignment checks trailing size");

    // Row-range assignment is strided in the same first-index-fast storage.
    // Pin both the real mirror and the integer payload: later integer reads
    // prefer `i`, so changing only `r` would appear to work until one of
    // those reads consumed the stale values.
    DataMap::Entry ints;
    ints.is_int = true;
    ints.dims = {2, 3};
    ints.i = {1, 4, 2, 5, 3, 6};
    ints.r.assign(ints.i.begin(), ints.i.end());
    interp.env()["ints"] = std::move(ints);
    DataMap::Entry replacement;
    replacement.is_int = true;
    replacement.dims = {2};
    replacement.i = {9, 8};
    replacement.r.assign(replacement.i.begin(), replacement.i.end());
    interp.env()["replacement"] = std::move(replacement);
    mir::Stmt row_write;
    row_write.kind = mir::Stmt::Assignment;
    row_write.lhs = "ints";
    row_write.lhs_idx = {single(1), between(1, 2)};
    row_write.rhs.kind = mir::Expr::Var;
    row_write.rhs.name = "replacement";
    row_write.rhs.type_ = "(UArray UInt)";
    row_write.rhs.unsized = {1, mir::UnsizedLeaf::Int};
    row_write.rhs.data_only = true;
    interp.run({row_write});
    const DataMap::Entry& written = interp.env().at("ints");
    check(written.dims == std::vector<int64_t>({2, 3}) &&
              written.i == std::vector<int>({9, 4, 8, 5, 3, 6}) &&
              written.r == std::vector<double>({9, 4, 8, 5, 3, 6}),
          "int array row-range assignment writes strided cells");
    DataMap::Entry full_row;
    full_row.is_int = true;
    full_row.dims = {3};
    full_row.i = {7, 8, 9};
    full_row.r.assign(full_row.i.begin(), full_row.i.end());
    interp.env()["full_row"] = std::move(full_row);
    mir::Stmt row_all_write = row_write;
    row_all_write.lhs_idx = {single(2), all()};
    row_all_write.rhs.name = "full_row";
    interp.run({row_all_write});
    const DataMap::Entry& row_all = interp.env().at("ints");
    check(row_all.i == std::vector<int>({9, 7, 8, 8, 3, 9}) &&
              row_all.r == std::vector<double>({9, 7, 8, 8, 3, 9}),
          "explicit row-All assignment writes strided cells");

    // Multi-index assignment is a scatter in index order.  In particular,
    // repeated indices are not deduplicated: the final RHS element wins, as
    // in the graph lowering and CmdStan's assignment semantics.
    DataMap::Entry scatter_target;
    scatter_target.dims = {5};
    scatter_target.r = {10, 20, 30, 40, 50};
    interp.env()["scatter_target"] = std::move(scatter_target);
    DataMap::Entry scatter_indices;
    scatter_indices.is_int = true;
    scatter_indices.dims = {3};
    scatter_indices.i = {4, 1, 4};
    scatter_indices.r.assign(scatter_indices.i.begin(),
                             scatter_indices.i.end());
    interp.env()["scatter_indices"] = std::move(scatter_indices);
    DataMap::Entry scatter_rhs;
    scatter_rhs.dims = {3};
    scatter_rhs.r = {7, 8, 9};
    interp.env()["scatter_rhs"] = std::move(scatter_rhs);
    mir::Stmt scatter_write;
    scatter_write.kind = mir::Stmt::Assignment;
    scatter_write.lhs = "scatter_target";
    scatter_write.lhs_idx = {multi("scatter_indices")};
    scatter_write.rhs.kind = mir::Expr::Var;
    scatter_write.rhs.name = "scatter_rhs";
    scatter_write.rhs.type_ = "UVector";
    scatter_write.rhs.unsized = {0, mir::UnsizedLeaf::Vector};
    scatter_write.rhs.data_only = true;
    interp.run({scatter_write});
    check(interp.env().at("scatter_target").dims == std::vector<int64_t>({5}) &&
              interp.env().at("scatter_target").r ==
                  std::vector<double>({8, 20, 30, 9, 50}),
          "multi-index assignment scatters in order with last duplicate win");

    DataMap::Entry int_scatter_target;
    int_scatter_target.is_int = true;
    int_scatter_target.dims = {5};
    int_scatter_target.i = {10, 20, 30, 40, 50};
    int_scatter_target.r.assign(int_scatter_target.i.begin(),
                                int_scatter_target.i.end());
    interp.env()["int_scatter_target"] = std::move(int_scatter_target);
    DataMap::Entry int_scatter_rhs;
    int_scatter_rhs.is_int = true;
    int_scatter_rhs.dims = {3};
    int_scatter_rhs.i = {7, 8, 9};
    int_scatter_rhs.r.assign(int_scatter_rhs.i.begin(),
                             int_scatter_rhs.i.end());
    interp.env()["int_scatter_rhs"] = std::move(int_scatter_rhs);
    mir::Stmt int_scatter_write = scatter_write;
    int_scatter_write.lhs = "int_scatter_target";
    int_scatter_write.lhs_idx = {multi("scatter_indices")};
    int_scatter_write.rhs.name = "int_scatter_rhs";
    int_scatter_write.rhs.type_ = "(UArray UInt)";
    int_scatter_write.rhs.unsized = {1, mir::UnsizedLeaf::Int};
    interp.run({int_scatter_write});
    const DataMap::Entry& int_scattered = interp.env().at("int_scatter_target");
    check(int_scattered.i == std::vector<int>({8, 20, 30, 9, 50}) &&
              int_scattered.r == std::vector<double>({8, 20, 30, 9, 50}),
          "multi-index assignment keeps integer destination mirrors aligned");

    DataMap::Entry matrix_scatter_target;
    matrix_scatter_target.is_int = true;
    matrix_scatter_target.dims = {3, 2};
    matrix_scatter_target.i = {11, 21, 31, 12, 22, 32};
    matrix_scatter_target.r.assign(matrix_scatter_target.i.begin(),
                                   matrix_scatter_target.i.end());
    interp.env()["matrix_scatter_target"] = std::move(matrix_scatter_target);
    DataMap::Entry matrix_scatter_indices;
    matrix_scatter_indices.is_int = true;
    matrix_scatter_indices.dims = {2};
    matrix_scatter_indices.i = {3, 1};
    matrix_scatter_indices.r.assign(matrix_scatter_indices.i.begin(),
                                    matrix_scatter_indices.i.end());
    interp.env()["matrix_scatter_indices"] = std::move(matrix_scatter_indices);
    DataMap::Entry matrix_scatter_rhs;
    matrix_scatter_rhs.is_int = true;
    matrix_scatter_rhs.dims = {2, 2};
    matrix_scatter_rhs.i = {90, 80, 70, 60};
    matrix_scatter_rhs.r.assign(matrix_scatter_rhs.i.begin(),
                                matrix_scatter_rhs.i.end());
    interp.env()["matrix_scatter_rhs"] = std::move(matrix_scatter_rhs);
    mir::Stmt matrix_scatter_write = scatter_write;
    matrix_scatter_write.lhs = "matrix_scatter_target";
    matrix_scatter_write.lhs_idx = {multi("matrix_scatter_indices"),
                                    between(1, 2)};
    matrix_scatter_write.rhs.name = "matrix_scatter_rhs";
    matrix_scatter_write.rhs.type_ = "(UArray (UArray UInt))";
    matrix_scatter_write.rhs.unsized = {2, mir::UnsizedLeaf::Int};
    interp.run({matrix_scatter_write});
    const DataMap::Entry& matrix_scattered =
        interp.env().at("matrix_scatter_target");
    check(
        matrix_scattered.i == std::vector<int>({80, 21, 90, 60, 22, 70}) &&
            matrix_scattered.r == std::vector<double>({80, 21, 90, 60, 22, 70}),
        "multi-index assignment preserves multidimensional layout");

    // A multi-index selection preserves its one-dimensional container shape:
    // the RHS must be a one-dimensional value with one element per selected
    // position.  Scalars are not implicitly broadcast.
    DataMap::Entry short_scatter_rhs;
    short_scatter_rhs.dims = {2};
    short_scatter_rhs.r = {1, 2};
    interp.env()["short_scatter_rhs"] = std::move(short_scatter_rhs);
    mir::Stmt short_scatter = scatter_write;
    short_scatter.rhs.name = "short_scatter_rhs";
    check(assignment_refused(short_scatter),
          "multi-index assignment checks RHS cardinality");
    DataMap::Entry scalar_scatter_rhs;
    scalar_scatter_rhs.r = {6};
    interp.env()["scalar_scatter_rhs"] = std::move(scalar_scatter_rhs);
    mir::Stmt scalar_scatter = scatter_write;
    scalar_scatter.rhs.name = "scalar_scatter_rhs";
    check(assignment_refused(scalar_scatter),
          "multi-index assignment rejects scalar RHS");

    mir::Expr nan_value;
    nan_value.kind = mir::Expr::LitReal;
    nan_value.lit = std::numeric_limits<double>::quiet_NaN();
    nan_value.type_ = "UReal";
    nan_value.unsized = {0, mir::UnsizedLeaf::Real};
    nan_value.data_only = true;
    mir::Expr is_nan;
    is_nan.kind = mir::Expr::FunApp;
    is_nan.fn_lib = mir::Expr::Lib::StanLib;
    is_nan.name = "is_nan";
    is_nan.type_ = "UInt";
    is_nan.unsized = {0, mir::UnsizedLeaf::Int};
    is_nan.data_only = true;
    is_nan.args = {nan_value};
    const DataMap::Entry nan_result = interp.eval(is_nan);
    is_nan.args[0].lit = 1.0;
    const DataMap::Entry finite_result = interp.eval(is_nan);
    check(nan_result.is_int && nan_result.i == std::vector<int>({1}) &&
              finite_result.is_int && finite_result.i == std::vector<int>({0}),
          "is_nan interpreter semantics");

    // stanc spells elementwise power as EltPow__.  Both operand orders
    // broadcast a scalar without losing the container's geometry.
    DataMap::Entry powers;
    powers.dims = {3};
    powers.r = {2, 3, 4};
    interp.env()["powers"] = std::move(powers);
    mir::Expr power_values;
    power_values.kind = mir::Expr::Var;
    power_values.name = "powers";
    power_values.type_ = "UVector";
    power_values.unsized.leaf = mir::UnsizedLeaf::Vector;
    power_values.data_only = true;
    mir::Expr elt_pow;
    elt_pow.kind = mir::Expr::FunApp;
    elt_pow.name = "EltPow__";
    elt_pow.type_ = "UVector";
    elt_pow.unsized.leaf = mir::UnsizedLeaf::Vector;
    elt_pow.data_only = true;
    elt_pow.args = {power_values, integer(2)};
    const DataMap::Entry bases = interp.eval(elt_pow);
    elt_pow.args = {integer(2), power_values};
    const DataMap::Entry exponents = interp.eval(elt_pow);
    check(bases.dims == std::vector<int64_t>({3}) &&
              bases.r == std::vector<double>({4, 9, 16}),
          "EltPow__ broadcasts a scalar exponent over a container");
    check(exponents.dims == std::vector<int64_t>({3}) &&
              exponents.r == std::vector<double>({4, 8, 16}),
          "EltPow__ broadcasts a scalar base over a container");
  }

  // The write-array fallback evaluates ctsem's likelihood contribution in
  // MIR rather than through the graph.  Pin both the single-container
  // semantics and the first-index-fast layout of array[N] vector[K]: the
  // logical observations below are {1,2} and {3,4}, not {1,3} and {2,4}.
  {
    auto variable = [](const std::string& name, const std::string& type,
                       uint8_t depth, mir::UnsizedLeaf leaf) {
      mir::Expr expression;
      expression.kind = mir::Expr::Var;
      expression.name = name;
      expression.type_ = type;
      expression.unsized = {depth, leaf};
      return expression;
    };
    mir::Expr density;
    density.kind = mir::Expr::FunApp;
    density.fn_lib = mir::Expr::Lib::StanLib;
    density.name = "multi_normal_cholesky_lpdf";
    density.type_ = "UReal";
    density.unsized = {0, mir::UnsizedLeaf::Real};
    density.args = {
        variable("y", "(UArray UVector)", 1, mir::UnsizedLeaf::Vector),
        variable("mu", "UVector", 0, mir::UnsizedLeaf::Vector),
        variable("L", "UMatrix", 0, mir::UnsizedLeaf::Matrix)};

    std::vector<Eigen::VectorXd> ys(2, Eigen::VectorXd(2));
    ys[0] << 1.0, 2.0;
    ys[1] << 3.0, 4.0;
    Eigen::VectorXd mu(2);
    mu << 0.4, -0.7;
    std::vector<Eigen::VectorXd> mus(2, Eigen::VectorXd(2));
    mus[0] << 0.4, -0.7;
    mus[1] << -0.2, 0.9;
    Eigen::MatrixXd L(2, 2);
    L << 1.4, 0.0, 0.3, 0.8;
    const double want =
        stan::math::multi_normal_cholesky_lpdf<false>(ys, mu, L);

    std::map<std::string, const mir::FunDef*> functions;
    MirInterp<double> interp(functions, "multi-normal Cholesky value test");
    DataMap::Entry y;
    y.dims = {2, 2};
    y.r = {1.0, 3.0, 2.0, 4.0};
    interp.env()["y"] = y;
    DataMap::Entry location;
    location.dims = {2};
    location.r = {0.4, -0.7};
    interp.env()["mu"] = location;
    DataMap::Entry factor;
    factor.dims = {2, 2};
    factor.r = {1.4, 0.3, 0.0, 0.8};
    interp.env()["L"] = factor;
    const DataMap::Entry got = interp.eval(density);
    check(got.r.size() == 1 && got.r[0] == want,
          "multi-normal Cholesky interpreter value and array layout");
    density.fn_propto = true;
    const DataMap::Entry got_propto = interp.eval(density);
    const double want_propto =
        stan::math::multi_normal_cholesky_lpdf<true>(ys, mu, L);
    check(got_propto.r.size() == 1 && got_propto.r[0] == want_propto,
          "multi-normal Cholesky interpreter propto value");
    density.fn_propto = false;

    mir::Expr vectorized_density = density;
    vectorized_density.args[1] =
        variable("mu_many", "(UArray UVector)", 1, mir::UnsizedLeaf::Vector);
    DataMap::Entry locations;
    locations.dims = {2, 2};
    locations.r = {0.4, -0.2, -0.7, 0.9};
    interp.env()["mu_many"] = locations;
    const DataMap::Entry vectorized = interp.eval(vectorized_density);
    const double vectorized_want =
        stan::math::multi_normal_cholesky_lpdf<false>(ys, mus, L);
    check(vectorized.r.size() == 1 && vectorized.r[0] == vectorized_want,
          "multi-normal Cholesky vectorized locations");

    DataMap::Entry empty_y;
    empty_y.dims = {0, 2};
    interp.env()["y"] = empty_y;
    const DataMap::Entry empty_result = interp.eval(density);
    check(empty_result.r.size() == 1 && empty_result.r[0] == 0.0,
          "multi-normal Cholesky empty observation array");
    interp.env()["y"] = y;

    using stan::math::var;
    stan::math::nested_rev_autodiff nested;
    MirInterp<var> rev(functions, "multi-normal Cholesky gradient test");
    MirVal<var> yv;
    yv.dims = {2, 2};
    for (double v : y.r) yv.r.emplace_back(v);
    rev.env()["y"] = yv;
    MirVal<var> muv;
    muv.dims = {2, 2};
    for (double v : locations.r) muv.r.emplace_back(v);
    rev.env()["mu_many"] = muv;
    MirVal<var> Lv;
    Lv.dims = {2, 2};
    for (double v : factor.r) Lv.r.emplace_back(v);
    rev.env()["L"] = Lv;
    MirVal<var> rev_got = rev.eval(vectorized_density);
    rev_got.r[0].grad();
    std::vector<double> got_adj;
    for (const var& v : rev.env().at("y").r) got_adj.push_back(v.adj());
    for (const var& v : rev.env().at("mu_many").r) got_adj.push_back(v.adj());
    for (const var& v : rev.env().at("L").r) got_adj.push_back(v.adj());

    std::vector<var> yr, mur, Lr;
    for (double v : y.r) yr.emplace_back(v);
    for (double v : locations.r) mur.emplace_back(v);
    for (double v : factor.r) Lr.emplace_back(v);
    std::vector<Eigen::Matrix<var, Eigen::Dynamic, 1>> yref(
        2, Eigen::Matrix<var, Eigen::Dynamic, 1>(2));
    for (int k = 0; k < 2; ++k)
      for (int i = 0; i < 2; ++i) yref[k](i) = yr[(size_t)(i * 2 + k)];
    std::vector<Eigen::Matrix<var, Eigen::Dynamic, 1>> muref(
        2, Eigen::Matrix<var, Eigen::Dynamic, 1>(2));
    for (int k = 0; k < 2; ++k)
      for (int i = 0; i < 2; ++i) muref[k](i) = mur[(size_t)(i * 2 + k)];
    Eigen::Matrix<var, Eigen::Dynamic, Eigen::Dynamic> Lref(2, 2);
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i) Lref(i, j) = Lr[(size_t)(j * 2 + i)];
    var rev_want =
        stan::math::multi_normal_cholesky_lpdf<false>(yref, muref, Lref);
    stan::math::set_zero_all_adjoints();
    rev_want.grad();
    std::vector<double> want_adj;
    for (const var& v : yr) want_adj.push_back(v.adj());
    for (const var& v : mur) want_adj.push_back(v.adj());
    for (const var& v : Lr) want_adj.push_back(v.adj());
    check(rev_got.r[0].val() == rev_want.val() && got_adj == want_adj,
          "multi-normal Cholesky interpreter gradients");
  }

  // rows()/cols() answer from the MIR type, not from the storage rank.
  // Both vector kinds are stored rank-1 -- orientation is type-level here
  // exactly as it is in the graph, where it lives in the slot view and not
  // in the dims -- so reading rows() off dims alone called every
  // row_vector[n] an n-by-1 column. That is a wrong int, and through a
  // transformed-data `int r = rows(rv)` it is a wrong log density.
  {
    std::map<std::string, const mir::FunDef*> functions;
    MirInterp<double> interp(functions, "rows/cols orientation test");
    auto real_value = [&](const std::string& name, std::vector<double> values,
                          std::vector<int64_t> dims) {
      DataMap::Entry value;
      value.r = std::move(values);
      value.dims = std::move(dims);
      interp.env()[name] = std::move(value);
    };
    real_value("v3", {1.0, 2.0, 3.0}, {3});
    real_value("rv3", {1.0, 2.0, 3.0}, {3});
    real_value("m23", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}, {2, 3});
    auto query = [&](const std::string& name, const std::string& arg,
                     const std::string& arg_type) {
      mir::Expr a;
      a.kind = mir::Expr::Var;
      a.name = arg;
      a.type_ = arg_type;
      a.data_only = true;
      mir::Expr call;
      call.kind = mir::Expr::FunApp;
      call.name = name;
      call.type_ = "UInt";
      call.data_only = true;
      call.args = {a};
      const DataMap::Entry got = interp.eval(call);
      return got.is_int && got.i.size() == 1 ? (long)got.i[0] : -1;
    };
    check(query("rows", "v3", "UVector") == 3 &&
              query("cols", "v3", "UVector") == 1,
          "vector[3] is 3 rows by 1 col");
    check(query("rows", "rv3", "URowVector") == 1 &&
              query("cols", "rv3", "URowVector") == 3,
          "row_vector[3] is 1 row by 3 cols");
    check(query("rows", "m23", "UMatrix") == 2 &&
              query("cols", "m23", "UMatrix") == 3,
          "matrix[2, 3] is 2 rows by 3 cols");
    // The task-shaped repro: `int r = rows(rv); int c = cols(rv);` folds to
    // 13 for a row_vector[3], and folded to 31 while orientation was lost.
    check(query("rows", "rv3", "URowVector") * 10 +
                  query("cols", "rv3", "URowVector") ==
              13,
          "row_vector shape query reaches transformed data as 13");
    // size()/num_elements() count elements and are orientation-blind; they
    // must not move when rows()/cols() start consulting the type.
    for (const std::string& name : {"size", "num_elements"})
      check(query(name, "rv3", "URowVector") == 3 &&
                query(name, "v3", "UVector") == 3,
            name + " counts elements of either vector kind");
  }

  // diagonal() is an interpreter primitive as well as a graph slice.  Pin
  // both rectangular orientations because they choose different sides of
  // min(rows, cols), while storage always advances by rows+1.
  {
    std::map<std::string, const mir::FunDef*> functions;
    MirInterp<double> interp(functions, "diagonal test");
    auto set_real = [&](const std::string& name, std::vector<double> values,
                        std::vector<int64_t> dims) {
      DataMap::Entry value;
      value.r = std::move(values);
      value.dims = std::move(dims);
      interp.env()[name] = std::move(value);
    };
    set_real("tall", {1, 2, 3, 4, 5, 6}, {3, 2});
    set_real("wide", {1, 2, 3, 4, 5, 6}, {2, 3});
    set_real("empty", {}, {0, 3});
    set_real("vector", {1, 2}, {2});
    auto call = [&](const std::string& name) {
      mir::Expr argument;
      argument.kind = mir::Expr::Var;
      argument.name = name;
      argument.type_ = name == "vector" ? "UVector" : "UMatrix";
      argument.data_only = true;
      mir::Expr expression;
      expression.kind = mir::Expr::FunApp;
      expression.name = "diagonal";
      expression.type_ = "UVector";
      expression.data_only = true;
      expression.args = {argument};
      return interp.eval(expression);
    };
    const DataMap::Entry tall = call("tall"), wide = call("wide"),
                         empty = call("empty");
    check(tall.dims == std::vector<int64_t>({2}) &&
              tall.r == std::vector<double>({1, 5}),
          "diagonal extracts tall rectangular matrix");
    check(wide.dims == std::vector<int64_t>({2}) &&
              wide.r == std::vector<double>({1, 4}),
          "diagonal extracts wide rectangular matrix");
    check(empty.dims == std::vector<int64_t>({0}) && empty.r.empty(),
          "diagonal preserves empty vector shape");
    bool refused_vector = false;
    try {
      (void)call("vector");
    } catch (const std::exception&) {
      refused_vector = true;
    }
    check(refused_vector, "diagonal rejects a non-matrix argument");
  }

  if (failures == 0) std::printf("test_mir OK\n");
  return failures == 0 ? 0 : 1;
}
