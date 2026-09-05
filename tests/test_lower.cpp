// The graph compiler on eight schools: MIR text + data -> graph whose
// log_prob gradient matches a var reference that mirrors the lowering's
// evaluation order. Plus the unsupported-construct error path.
#include "env_helpers.hpp"
#include "categorical_check_mir.hpp"
#include "stdout_capture.hpp"
#include <stanli/compile.hpp>
#include <stanli/dae.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/ode_adjoint.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static void expect_eq(const std::string& what, double got, double want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g\n", what.c_str(), got, want);
  }
}
static int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? std::numeric_limits<int64_t>::min() - i : i;
}
// Project parity budget: up to 2 ULP vs references is acceptable.
static void expect_ulp(const std::string& what, double got, double want) {
  const int64_t d = std::llabs(ulp_key(got) - ulp_key(want));
  if (d > 2) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
  }
}
static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static stanli::CompiledModel compile_without_optional_islands(
    const std::string& mir, const stanli::DataMap& data) {
  test_setenv("STANLI_NO_ISLAND", "1", 1);
  try {
    stanli::CompiledModel cm = stanli::compile_model(mir, data);
    test_unsetenv("STANLI_NO_ISLAND");
    return cm;
  } catch (...) {
    test_unsetenv("STANLI_NO_ISLAND");
    throw;
  }
}

static stanli::CompiledModel compile_without_graph_passes(
    const std::string& mir, const stanli::DataMap& data) {
  static const char* flags[] = {"STANLI_NO_REROLL",  "STANLI_NO_PARTITION",
                                "STANLI_NO_INPLACE", "STANLI_NO_CONSTFOLD",
                                "STANLI_NO_CSE",     "STANLI_NO_ISLAND"};
  for (const char* flag : flags) test_setenv(flag, "1", 1);
  try {
    stanli::CompiledModel cm = stanli::compile_model(mir, data);
    for (const char* flag : flags) test_unsetenv(flag);
    return cm;
  } catch (...) {
    for (const char* flag : flags) test_unsetenv(flag);
    throw;
  }
}

static int count_opcode(const stanli::CompiledModel& cm, uint16_t opcode) {
  return static_cast<int>(
      std::count_if(cm.graph.ops.begin(), cm.graph.ops.end(),
                    [=](const stanli::Op& op) { return op.opcode == opcode; }));
}

static bool same_graph_structure(const stanli::CompiledModel& a,
                                 const stanli::CompiledModel& b) {
  if (a.graph.result_slot != b.graph.result_slot ||
      a.graph.slots.size() != b.graph.slots.size() ||
      a.graph.ops.size() != b.graph.ops.size() ||
      a.fills.size() != b.fills.size())
    return false;
  for (size_t i = 0; i < a.graph.slots.size(); ++i)
    if (a.graph.slots[i].len != b.graph.slots[i].len ||
        a.graph.slots[i].is_param != b.graph.slots[i].is_param)
      return false;
  for (size_t i = 0; i < a.fills.size(); ++i)
    if (a.fills[i].first != b.fills[i].first ||
        a.fills[i].second.size() != b.fills[i].second.size())
      return false;
  for (size_t i = 0; i < a.graph.ops.size(); ++i) {
    const stanli::Op& x = a.graph.ops[i];
    const stanli::Op& y = b.graph.ops[i];
    if (x.opcode != y.opcode || x.variant != y.variant || x.out != y.out ||
        x.out2 != y.out2 || x.n_in != y.n_in || x.n_idata != y.n_idata ||
        (x.udata == nullptr) != (y.udata == nullptr))
      return false;
    for (int k = 0; k < x.n_in; ++k)
      if (x.in[k] != y.in[k]) return false;
    for (int64_t k = 0; k < x.n_idata; ++k)
      if (x.idata[k] != y.idata[k]) return false;
  }
  return true;
}

// A minimal model whose only statement slices a declared 3-vector parameter
// v[2:] (Upfrom) or the pre-bump v[2:3] (Between) it is equivalent to.
static const char* kUpfromSliceMir = R"MIR(
((functions_block ())
 (input_vars ())
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id v)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (TargetPE
      ((pattern
        (FunApp (StanLib normal_lpdf (FnLpdf false) AoS)
         (((pattern
            (Indexed
             ((pattern (Var v))
              (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
             ((Upfrom
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Promotion
             ((pattern (Lit Int 1))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))))
 (output_vars
  ((v <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))))
 (prog_name upfrom_test_model)
 (prog_path upfrom_test.stan))
)MIR";

static const char* kBetweenSliceMir = R"MIR(
((functions_block ())
 (input_vars ())
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id v)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (TargetPE
      ((pattern
        (FunApp (StanLib normal_lpdf (FnLpdf false) AoS)
         (((pattern
            (Indexed
             ((pattern (Var v))
              (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
             ((Between
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Promotion
             ((pattern (Lit Int 1))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))))
 (output_vars
  ((v <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))))
 (prog_name upfrom_test_model)
 (prog_path upfrom_test.stan))
)MIR";

// Returns the parenthesized s-expression starting at or after `start`.
static std::string read_sexp_at(const std::string& text, size_t start) {
  size_t i = start;
  while (text[i] == ' ' || text[i] == '\n') ++i;
  int depth = 0;
  size_t j = i;
  for (; j < text.size(); ++j) {
    if (text[j] == '(') {
      ++depth;
    } else if (text[j] == ')') {
      if (--depth == 0) {
        ++j;
        break;
      }
    }
  }
  return text.substr(i, j - i);
}

// Rewrites a UDF-formal matrix's full-extent row range W[1:rows(W), k] into
// the Upfrom shape W[1:, k] it is equivalent to.
static size_t rewrite_udf_rows_slice_to_upfrom(std::string& text) {
  std::string out;
  size_t pos = 0, count = 0;
  while (true) {
    const size_t idx = text.find("(Between", pos);
    if (idx == std::string::npos) {
      out += text.substr(pos);
      break;
    }
    const std::string node = read_sexp_at(text, idx);
    const size_t node_end = idx + node.size();
    if (node.find("rows FnPlain AoS") != std::string::npos &&
        node.find("(Var W)") != std::string::npos) {
      const std::string lo = read_sexp_at(node, std::strlen("(Between"));
      out += text.substr(pos, idx - pos);
      out += "(Upfrom " + lo + ")";
      ++count;
    } else {
      out += text.substr(pos, node_end - pos);
    }
    pos = node_end;
  }
  text = std::move(out);
  return count;
}

static stanli::DataMap bound_check_data(double raw = 0.0, int N = 1, int M = 1,
                                        int R = 1, int C = 1, int BR = 1,
                                        int BC = 1) {
  stanli::DataMap d;
  d.set_real("d", 0.0);
  d.set_real("raw", raw);
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

// Compile and evaluate once with either the direct data preload or its
// interpreter oracle.  The escape hatch is compile-time only; always clear it
// before execution (and on exceptions) so one comparison cannot contaminate
// the next test in this process.
struct LowerSnapshot {
  double lp = 0.0;
  std::vector<double> grad;
  size_t ops = 0;
  size_t slots = 0;
  std::vector<std::pair<int, std::vector<double>>> fills;
};

static LowerSnapshot lower_snapshot(const std::string& mir,
                                    const stanli::DataMap& data,
                                    const std::vector<double>& q,
                                    bool disable_preload) {
  if (disable_preload)
    test_setenv("STANLI_NO_DATA_PRELOAD", "1", 1);
  else
    test_unsetenv("STANLI_NO_DATA_PRELOAD");
  stanli::CompiledModel cm;
  try {
    cm = stanli::compile_model(mir, data);
  } catch (...) {
    test_unsetenv("STANLI_NO_DATA_PRELOAD");
    throw;
  }
  test_unsetenv("STANLI_NO_DATA_PRELOAD");

  LowerSnapshot out;
  out.ops = cm.graph.ops.size();
  out.slots = cm.graph.slots.size();
  out.fills = cm.fills;
  stanli::Executor ex(std::move(cm.graph));
  cm.bind(ex);
  check(ex.n_params() == static_cast<int64_t>(q.size()),
        "data preload snapshot parameter count");
  for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
  out.grad.resize(q.size());
  out.lp = ex.gradient(out.grad.data());
  return out;
}

static std::string lower_error(const std::string& mir,
                               const stanli::DataMap& data,
                               bool disable_preload) {
  if (disable_preload)
    test_setenv("STANLI_NO_DATA_PRELOAD", "1", 1);
  else
    test_unsetenv("STANLI_NO_DATA_PRELOAD");
  try {
    (void)stanli::compile_model(mir, data);
  } catch (const std::exception& e) {
    test_unsetenv("STANLI_NO_DATA_PRELOAD");
    return e.what();
  }
  test_unsetenv("STANLI_NO_DATA_PRELOAD");
  return {};
}

static void expect_same_lowering(const std::string& tag,
                                 const LowerSnapshot& fast,
                                 const LowerSnapshot& oracle) {
  check(fast.ops == oracle.ops, tag + " op count");
  check(fast.slots == oracle.slots, tag + " slot count");
  check(fast.fills == oracle.fills, tag + " fills bitwise");
  expect_eq(tag + " lp", fast.lp, oracle.lp);
  check(fast.grad.size() == oracle.grad.size(), tag + " gradient size");
  const size_t n = std::min(fast.grad.size(), oracle.grad.size());
  for (size_t i = 0; i < n; ++i)
    expect_eq(tag + " g" + std::to_string(i), fast.grad[i], oracle.grad[i]);
}

static const double kY[8] = {28, 8, -3, 7, -1, 1, 18, 12};
static const double kSigma[8] = {15, 10, 16, 11, 9, 11, 10, 18};

// Mirrors the lowering: reads (with jacobian) in declaration order, then
// statements in program order, target = add_n(terms..., jacs...).
static void reference(const double* q, double* lp_out, double* grad_out) {
  using stan::math::var;
  const int J = 8;
  var mu = q[0];
  var log_tau = q[1];
  Eigen::Matrix<var, -1, 1> tilde(J);
  for (int i = 0; i < J; ++i) tilde(i) = q[2 + i];

  var jac = 0.0;
  var tau = stan::math::lb_constrain<true>(log_tau, 0.0, jac);

  // theta = mu + tau * tilde, which O1 contracts to fma(tau, tilde, mu).
  Eigen::Matrix<var, -1, 1> theta = stan::math::fma(tau, tilde, mu);

  // ~ statements lower propto=true with activity from MIR adlevels: exactly
  // the instantiations CmdStan's generated C++ uses (data args stay double).
  Eigen::Map<const Eigen::VectorXd> y(kY, J);
  Eigen::Map<const Eigen::VectorXd> sigma(kSigma, J);
  var t1 = stan::math::normal_lpdf<true>(mu, 0.0, 5.0);
  var t2 = stan::math::cauchy_lpdf<true>(tau, 0.0, 5.0);
  var t3 = stan::math::normal_lpdf<true>(tilde, 0.0, 1.0);
  var t4 = stan::math::normal_lpdf<true>(y, theta, sigma);
  var lp = ((((t1 + t2) + t3) + t4) + jac);
  lp.grad();

  *lp_out = lp.val();
  grad_out[0] = mu.adj();
  grad_out[1] = log_tau.adj();
  for (int i = 0; i < J; ++i) grad_out[2 + i] = tilde(i).adj();
  stan::math::recover_memory();
}

int main() {
  // These fixtures pin ULP-level parity with DEFAULT CmdStan, whose AoS
  // Matrix<var> paths run scalar libm per element; the packet path answers
  // to `stanc --O1` instead and is verified there.
  stanli::set_packet_math(false);
  using namespace stanli;

  // Section A1's five reductions must lower in the autodiff model graph, not
  // merely in the generated-quantities graph. A zero product and tied maximum
  // pin the non-generic reverse cases; singleton dispersion must remain a
  // disconnected constant just as it is in Stan Math.
  {
    DataMap d;
    CompiledModel reductions =
        compile_model(slurp("tests/fixtures/a1_reductions.tmir.sexp"), d);
    check(count_opcode(reductions, OP_PROD_VEC) == 1,
          "A1 product opcode census");
    for (const Op& op : reductions.graph.ops)
      if (op.opcode == OP_PROD_VEC)
        check(op.variant == 2, "A1 active product scalar grouping");
    check(count_opcode(reductions, OP_EXTREMA_VEC) == 2,
          "A1 extrema opcode census");
    check(count_opcode(reductions, OP_SD) == 2, "A1 sd opcode census");
    check(count_opcode(reductions, OP_VARIANCE) == 2,
          "A1 variance opcode census");

    Executor reduction_ex(std::move(reductions.graph));
    reductions.bind(reduction_ex);
    const double q[] = {0.25, 0.25, 0.0, 0.4};
    std::copy(q, q + 4, reduction_ex.params_data());
    double gradient[4];
    const double lp = reduction_ex.gradient(gradient);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> y(3), singleton(1);
    for (int i = 0; i < 3; ++i) y(i) = q[i];
    singleton(0) = q[3];
    var reference = stan::math::std_normal_lpdf<true>(y);
    reference += stan::math::std_normal_lpdf<true>(singleton);
    reference += stan::math::prod(y) + stan::math::min(y) + stan::math::max(y) +
                 stan::math::sd(y) + stan::math::variance(y);
    reference += stan::math::sd(singleton) + stan::math::variance(singleton);
    reference.grad();

    expect_ulp("A1 reductions lp", lp, reference.val());
    for (int i = 0; i < 3; ++i)
      expect_ulp("A1 reductions y" + std::to_string(i), gradient[i],
                 y(i).adj());
    expect_eq("A1 singleton gradient", gradient[3], singleton(0).adj());
    stan::math::recover_memory();
  }

  // Section A2's matrix functions share shapes but not implementations:
  // inverse_spd uses an LDLT, log_determinant a pivoted QR, and quad_form is
  // deliberately not the symmetrising quad_form_sym operation. Pin all five
  // as native opcodes and compare their composed reverse pass to Stan Math.
  {
    DataMap d;
    CompiledModel matrix_functions =
        compile_model(slurp("tests/fixtures/a2_matrix_functions.tmir.sexp"), d);
    check(count_opcode(matrix_functions, OP_INVERSE) == 1,
          "A2 inverse opcode census");
    check(count_opcode(matrix_functions, OP_INVERSE_SPD) == 1,
          "A2 inverse_spd opcode census");
    check(count_opcode(matrix_functions, OP_LOG_DETERMINANT) == 1,
          "A2 log determinant opcode census");
    check(count_opcode(matrix_functions, OP_QUAD_FORM) == 1,
          "A2 quad form opcode census");
    check(count_opcode(matrix_functions, OP_ADD_DIAG) == 1,
          "A2 add diag opcode census");

    Executor matrix_ex(std::move(matrix_functions.graph));
    matrix_functions.bind(matrix_ex);
    const double q[] = {0.2, -0.3, 0.1, -0.2, 0.4, -0.1};
    std::copy(q, q + 6, matrix_ex.params_data());
    double gradient[6];
    const double lp = matrix_ex.gradient(gradient);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> y(4), diagonal(2);
    for (int i = 0; i < 4; ++i) y(i) = q[i];
    for (int i = 0; i < 2; ++i) diagonal(i) = q[4 + i];
    Eigen::Matrix<var, -1, -1> A(2, 2), S(2, 2);
    A << stan::math::exp(y(0)), y(2), y(3), stan::math::exp(y(1));
    S.setZero();
    S(0, 0) = stan::math::exp(y(0));
    S(1, 1) = stan::math::exp(y(1));
    Eigen::VectorXd b(2);
    b << 1.0, 2.0;

    var reference = stan::math::std_normal_lpdf<true>(y);
    reference += stan::math::std_normal_lpdf<true>(diagonal);
    reference += stan::math::sum(stan::math::inverse(A));
    reference += stan::math::sum(stan::math::inverse_spd(S));
    reference += stan::math::log_determinant(A);
    reference += stan::math::quad_form(A, b);
    reference += stan::math::sum(stan::math::add_diag(A, diagonal));
    reference.grad();

    expect_ulp("A2 matrix functions lp", lp, reference.val());
    for (int i = 0; i < 4; ++i)
      expect_ulp("A2 matrix functions y" + std::to_string(i), gradient[i],
                 y(i).adj());
    for (int i = 0; i < 2; ++i)
      expect_ulp("A2 matrix functions d" + std::to_string(i), gradient[4 + i],
                 diagonal(i).adj());
    stan::math::recover_memory();
  }

  // A scalar add_diag argument receives one contribution per diagonal entry.
  // The additions are created from the first coefficient to the last, so the
  // reverse tape accumulates a deliberately cancellation-sensitive seed in
  // the opposite order.
  {
    const Kernel* add_diag = find_kernel(OP_ADD_DIAG);
    check(add_diag && add_diag->forward && add_diag->backward,
          "add_diag scalar kernel registration");
    if (add_diag && add_diag->forward && add_diag->backward) {
      constexpr int n = 3;
      double a[n * n] = {};
      double diagonal = 0.0;
      double out[n * n] = {};
      double out_adj[n * n] = {};
      out_adj[0] = 1e16;
      out_adj[4] = -1e16;
      out_adj[8] = 1.0;
      double diagonal_adj = 0.0;
      const int dims[] = {n, n};
      KernelCtx ctx;
      ctx.in[0] = Desc{a, n * n};
      ctx.in[1] = Desc{&diagonal, 1};
      ctx.n_in = 2;
      ctx.out = Desc{out, n * n};
      ctx.variant = 1;
      ctx.idata = dims;
      ctx.n_idata = 2;
      add_diag->forward(ctx);
      ctx.in_adj[0] = Desc{nullptr, n * n};
      ctx.in_adj[1] = Desc{&diagonal_adj, 1};
      ctx.out_adj_vec = Desc{out_adj, n * n};
      add_diag->backward(ctx);

      Eigen::MatrixXd matrix = Eigen::MatrixXd::Zero(n, n);
      Eigen::MatrixXd seed = Eigen::MatrixXd::Zero(n, n);
      seed(0, 0) = 1e16;
      seed(1, 1) = -1e16;
      seed(2, 2) = 1.0;
      stan::math::var d_ref = 0.0;
      stan::math::var reference = stan::math::sum(
          stan::math::elt_multiply(stan::math::add_diag(matrix, d_ref), seed));
      reference.grad();
      expect_eq("add_diag scalar reverse order", diagonal_adj, d_ref.adj());
      stan::math::recover_memory();
    }
  }

  // Direct input preload must be an exact replacement for stanc's generated
  // FnReadData reconstruction.  This one fixture covers a vector, a matrix,
  // and two array-of-vector inputs; y is deliberately written with integer
  // JSON tokens even though its Stan declaration is real, exercising the
  // schema-based removal of the JSON reader's int mirror.
  {
    const DataMap d = DataMap::from_json(
        R"({"N": 3, "K": 2,
             "t": [0.5, 1.75, 2.25],
             "y": [[1, 2], [3, 4], [5, 6]],
             "p": [[0.3, 0.7], [0.4, 0.6], [0.2, 0.8]],
             "Sigma": [[2.0, 0.5], [0.5, 1.0]]})");
    const std::vector<double> q = {0.35, -0.2,  0.4,  0.15, -0.3,
                                   0.5,  -0.45, 0.25, -0.1};
    const std::string mir = slurp("tests/fixtures/shapes.tmir.sexp");
    const LowerSnapshot fast = lower_snapshot(mir, d, q, false);
    const LowerSnapshot oracle = lower_snapshot(mir, d, q, true);
    expect_same_lowering("data preload shapes", fast, oracle);
  }

  // A future stanc version may put another effect in the same top-level block
  // as an input rebuild.  Make that shape synthetically by changing the inner
  // generated `pos__ = pos__ + 1` into `N = pos__ + 1`.  The conservative
  // classifier must interpret the whole mixed block; skipping it would leave
  // y as [3,4], while the interpreter rebuilds it as [3,3].
  {
    std::string mir = slurp("tests/fixtures/loopy.tmir.sexp");
    const std::string from = "(Assignment ((LVariable pos__) ()) UInt";
    const std::string to = "(Assignment ((LVariable N) ()) UInt";
    size_t pos = 0;
    for (int occurrence = 0; occurrence < 3 && pos != std::string::npos;
         ++occurrence) {
      pos = mir.find(from, pos);
      if (occurrence < 2 && pos != std::string::npos) pos += from.size();
    }
    check(pos != std::string::npos, "data preload mixed-block mutation");
    if (pos != std::string::npos) mir.replace(pos, from.size(), to);
    const DataMap d = DataMap::from_json(R"({"N": 2, "y": [3, 4]})");
    const LowerSnapshot fast = lower_snapshot(mir, d, {0.4}, false);
    const LowerSnapshot oracle = lower_snapshot(mir, d, {0.4}, true);
    expect_same_lowering("data preload mixed block", fast, oracle);
  }

  // A missing input and a real-valued JSON token for an integer input must
  // take the checked interpreter path, not become a partial preload or a
  // truncating cast.  The fast-path-on and escape-hatch diagnostics agree.
  {
    DataMap missing;
    missing.set_int("N", 2);
    const std::string mir = slurp("tests/fixtures/loopy.tmir.sexp");
    const std::string fast = lower_error(mir, missing, false);
    const std::string oracle = lower_error(mir, missing, true);
    check(!fast.empty() && fast.find("y") != std::string::npos,
          "data preload missing input rejected");
    check(fast == oracle, "data preload missing input diagnostic");

    const DataMap malformed = DataMap::from_json(R"({"K": 1.5})");
    const std::string simp = slurp("tests/fixtures/simp.tmir.sexp");
    const std::string fast_int = lower_error(simp, malformed, false);
    const std::string oracle_int = lower_error(simp, malformed, true);
    check(!fast_int.empty(), "data preload real token for int rejected");
    check(fast_int == oracle_int, "data preload int diagnostic");
  }

  DataMap data;
  data.set_int("J", 8);
  data.set_real_array("y", std::vector<double>(kY, kY + 8));
  data.set_real_array("sigma", std::vector<double>(kSigma, kSigma + 8));

  CompiledModel cm = compile_model(slurp("tests/fixtures/es.tmir.sexp"), data);
  check(cm.n_unconstrained == 10, "10 unconstrained params");
  check(cm.param_names.size() == 3 && cm.param_names[0] == "mu" &&
            cm.param_names[1] == "tau" && cm.param_names[2] == "theta_tilde",
        "param names");

  Executor ex(std::move(cm.graph));
  cm.bind(ex);

  const double qs[3][10] = {
      {4.0, 1.0, 0.1, -0.2, 0.3, -0.4, 0.5, -0.6, 0.7, -0.8},
      {0.0, -1.5, 1.2, 0.8, -1.1, 0.05, -0.3, 0.9, -1.4, 0.2},
      {-2.5, 0.3, -0.7, 1.5, 0.6, -0.9, 1.1, 0.4, -0.2, -1.3}};
  for (int c = 0; c < 3; ++c) {
    for (int i = 0; i < 10; ++i) ex.params_data()[i] = qs[c][i];
    double grad[10], lp_ref, grad_ref[10];
    const double lp = ex.gradient(grad);
    reference(qs[c], &lp_ref, grad_ref);
    const std::string tag = "case" + std::to_string(c);
    expect_eq(tag + " lp", lp, lp_ref);
    // mu's adjoint sums eight contributions; the graph and the reference
    // reverse pass reach it in different orders.
    for (int i = 0; i < 10; ++i)
      expect_ulp(tag + " g" + std::to_string(i), grad[i], grad_ref[i]);
  }

  // For loops unroll: scalar-loop normal model vs per-term var reference.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_real_array("y", {1.0, 2.0, 3.0});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/loopy.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.4;
    double g = 0, lp = lex.gradient(&g);

    using stan::math::var;
    var mu = 0.4;
    const double yv[3] = {1.0, 2.0, 3.0};
    var acc = 0.0;
    for (int n = 0; n < 3; ++n)
      acc = acc + stan::math::normal_lpdf<true>(yv[n], mu, 1.0);
    acc.grad();
    expect_eq("loopy lp", lp, acc.val());
    expect_eq("loopy dmu", g, mu.adj());
    stan::math::recover_memory();
  }

  // A target-only loop whose body does not read its iterator is one density
  // multiplied by the trip count. Disable every optional graph pass here:
  // the bounded graph must come directly from lowering, before CSE or
  // re-rolling could hide an unroll. The nested form accumulates N*M into
  // the same one multiply rather than leaving a chain of scalar scales.
  {
    const std::string mir =
        slurp("tests/fixtures/invariant_target_loop.tmir.sexp");
    const auto compile_loop = [&](int mode, int N, int M = 1) {
      DataMap d;
      d.set_int("N", N);
      d.set_int("M", M);
      d.set_int("mode", mode);
      return compile_without_graph_passes(mir, d);
    };

    CompiledModel ten = compile_loop(0, 10);
    CompiledModel many = compile_loop(0, 10000);
    check(same_graph_structure(ten, many),
          "invariant loop graph identical modulo N constant");
    check(count_opcode(ten, OP_NORMAL_LPDF) == 1 &&
              count_opcode(ten, OP_MUL) == 1 &&
              count_opcode(ten, OP_ADD_N) == 0,
          "invariant loop is one density times N");

    CompiledModel zero = compile_loop(0, 0);
    check(count_opcode(zero, OP_NORMAL_LPDF) == 0 &&
              count_opcode(zero, OP_MUL) == 0,
          "zero-trip invariant loop does not lower its body");
    CompiledModel one = compile_loop(0, 1);
    check(count_opcode(one, OP_NORMAL_LPDF) == 1 &&
              count_opcode(one, OP_MUL) == 0,
          "one-trip invariant loop keeps the ordinary path");

    CompiledModel nested = compile_loop(5, 20, 30);
    check(count_opcode(nested, OP_NORMAL_LPDF) == 1 &&
              count_opcode(nested, OP_MUL) == 1 &&
              count_opcode(nested, OP_ADD_N) == 0,
          "nested invariant loop is one density times N*M");

    CompiledModel iterator = compile_loop(1, 4);
    check(count_opcode(iterator, OP_NORMAL_LPDF) == 4,
          "loop iterator use refuses invariant collapse");
    CompiledModel assignment = compile_loop(2, 4);
    check(count_opcode(assignment, OP_NORMAL_LPDF) == 4,
          "outer assignment refuses invariant collapse");
    CompiledModel printing = compile_loop(3, 4);
    check(count_opcode(printing, OP_PRINT) == 4 &&
              count_opcode(printing, OP_NORMAL_LPDF) == 4,
          "print refuses invariant collapse");
    CompiledModel rejecting = compile_loop(4, 4);
    check(count_opcode(rejecting, OP_REJECT) == 4 &&
              count_opcode(rejecting, OP_NORMAL_LPDF) == 4,
          "reject refuses invariant collapse");
    CompiledModel local_iterator = compile_loop(6, 4);
    check(count_opcode(local_iterator, OP_NORMAL_LPDF) == 4,
          "iterator use through a local refuses invariant collapse");

    CompiledModel local_terms = compile_loop(7, 10);
    CompiledModel many_local_terms = compile_loop(7, 10000);
    check(same_graph_structure(local_terms, many_local_terms),
          "invariant local and multiple terms graph identical modulo N");

    Executor lex(std::move(ten.graph));
    ten.bind(lex);
    lex.params_data()[0] = 0.4;
    double grad = 0.0;
    const double lp = lex.gradient(&grad);
    using stan::math::var;
    var x = 0.4;
    var reference = 10.0 * stan::math::normal_lpdf<true>(x, 0.0, 1.0);
    reference.grad();
    expect_eq("invariant loop lp", lp, reference.val());
    expect_eq("invariant loop grad", grad, x.adj());
    stan::math::recover_memory();

    Executor local_ex(std::move(local_terms.graph));
    local_terms.bind(local_ex);
    local_ex.params_data()[0] = 0.4;
    grad = 0.0;
    const double local_lp = local_ex.gradient(&grad);
    x = 0.4;
    var twice_x = 2.0 * x;
    reference = 10.0 * stan::math::normal_lpdf<true>(twice_x, 0.0, 1.0) +
                10.0 * 0.25 * x;
    reference.grad();
    expect_eq("invariant local terms lp", local_lp, reference.val());
    expect_eq("invariant local terms grad", grad, x.adj());
    stan::math::recover_memory();
  }

  // Static if inside an unrolled loop, condition indexing data by the loop
  // variable (M0 capture-recapture pattern): the branch is resolved at
  // compile time per iteration.
  {
    DataMap d;
    d.set_int("M", 3);
    d.set_int_array("s", {2, 0, 1});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/staticif.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.3;
    double g = 0, lp = lex.gradient(&g);

    using stan::math::var;
    var pu = 0.3;
    // Mirror the lowering: model terms sum first, jacobians append last.
    var lj = 0.0;
    var pc = stan::math::lub_constrain(pu, 0.0, 1.0, lj);
    var acc = 0.0;
    const int sv[3] = {2, 0, 1};
    for (int i = 0; i < 3; ++i) {
      if (sv[i] > 0)
        acc = acc + stan::math::binomial_lpmf<false>(sv[i], 5, pc);
      else
        acc = acc + stan::math::bernoulli_lpmf<false>(0, pc);
    }
    acc = acc + lj;
    acc.grad();
    expect_eq("staticif lp", lp, acc.val());
    expect_eq("staticif dp", g, pu.adj());
    stan::math::recover_memory();
  }

  // Row of a 2-D int data array as a density outcome: y[i] reaches the
  // kernel as a T-length int array (Mb/Mt/irt_2pl pattern).
  {
    DataMap d =
        DataMap::from_json(R"({"M": 2, "T": 3, "y": [[1, 0, 1], [0, 0, 1]]})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/introw.tmir.sexp"), d);
    check(lm.n_unconstrained == 3, "introw 3 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    for (int i = 0; i < 3; ++i) lex.params_data()[i] = 0.2 * (i + 1) - 0.3;
    double grad[3] = {0, 0, 0};
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> pu(3);
    for (int i = 0; i < 3; ++i) pu(i) = 0.2 * (i + 1) - 0.3;
    var lj = 0.0;
    Eigen::Matrix<var, -1, 1> pc = stan::math::lub_constrain(pu, 0.0, 1.0, lj);
    const std::vector<std::vector<int>> yv = {{1, 0, 1}, {0, 0, 1}};
    var acc = 0.0;
    for (int i = 0; i < 2; ++i)
      acc = acc + stan::math::bernoulli_lpmf<false>(yv[i], pc);
    acc = acc + lj;
    acc.grad();
    expect_eq("introw lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("introw g" + std::to_string(i), grad[i], pu(i).adj());
    stan::math::recover_memory();
  }

  // Data-only expressions with no native lowering const-fold at compile
  // time: mean/sd bounds, negative_infinity in log_sum_exp, constant lccdf.
  {
    DataMap d;
    d.set_int("N", 2);
    d.set_real_array("y", {1.3, -0.7});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/dfold.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.25;
    lex.params_data()[1] = -0.6;
    double grad[2] = {0, 0};
    const double lp = lex.gradient(grad);

    using stan::math::var;
    const double yv[2] = {1.3, -0.7};
    const double m = (yv[0] + yv[1]) / 2.0;
    const double s = std::sqrt(
        ((yv[0] - m) * (yv[0] - m) + (yv[1] - m) * (yv[1] - m)) / 1.0);
    Eigen::Matrix<var, -1, 1> muu(2);
    muu << 0.25, -0.6;
    var lj = 0.0;
    Eigen::Matrix<var, -1, 1> muc =
        stan::math::lub_constrain(muu, m - 3 * s, m + 3 * s, lj);
    var acc = stan::math::normal_lpdf<false>(yv[0], muc(0), 1.0);
    acc = acc + stan::math::log_sum_exp(
                    stan::math::normal_lpdf<false>(yv[1], muc(1), 1.0),
                    var(-std::numeric_limits<double>::infinity()));
    acc = acc + stan::math::student_t_lccdf(0.0, 3.0, 0.0, 10.0);
    acc = acc + lj;
    acc.grad();
    expect_ulp("dfold lp", lp, acc.val());
    for (int i = 0; i < 2; ++i)
      expect_ulp("dfold g" + std::to_string(i), grad[i], muu(i).adj());
    stan::math::recover_memory();
  }

  // Transformed-data UDF (loops, range slices, 2-D writes, return) plus
  // ternary selection on data conditions, for both branch polarities.
  for (int flag = 0; flag <= 1; ++flag) {
    DataMap d = DataMap::from_json(
        std::string(R"({"N": 3, "K": 3, "flag": )") + std::to_string(flag) +
        R"(, "W": [[1.0, 2.0, 4.0], [1.0, 0.5, 2.0], [1.0, -1.0, 6.0]]})");
    CompiledModel lm = compile_model(slurp("tests/fixtures/udf.tmir.sexp"), d);
    check(lm.n_unconstrained == 3, "udf 3 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[3] = {0.4, -0.9, 1.7};
    for (int i = 0; i < 3; ++i) lex.params_data()[i] = q[i];
    double grad[3] = {0, 0, 0};
    const double lp = lex.gradient(grad);

    // adj column 2 exactly as the td interpreter computes it.
    const double c2[3] = {2.0, 0.5, -1.0};
    double m = 0;
    for (double v : c2) m += v;
    m /= 3.0;
    double s2 = 0;
    for (double v : c2) s2 += (v - m) * (v - m);
    const double sdv = std::sqrt(s2 / 2.0);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> b(3);
    b << q[0], q[1], q[2];
    var t1 = stan::math::normal_lpdf<false>(b, m, sdv * 2.0);
    var t2 = stan::math::normal_lpdf<false>(b(0), flag ? 0.5 : -0.5, 1.0);
    var t3 = flag ? stan::math::normal_lpdf<false>(b(1), b(0), 1.0)
                  : stan::math::normal_lpdf<false>(b(1), 0.0, 1.0);
    var acc = ((t1 + t2) + t3);
    acc.grad();
    const std::string tag = "udf f" + std::to_string(flag);
    expect_eq(tag + " lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq(tag + " g" + std::to_string(i), grad[i], b(i).adj());
    stan::math::recover_memory();
  }

  // A data container passed beside an active actual cannot be folded as one
  // call. It remains observable while the UDF is inlined and must bind with
  // the same value and activity as an ordinary data argument.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_real_array("x", {1.25, -0.5, 2.0}, {3});
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/call_argument_cache.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    ex.params_data()[0] = -0.75;
    double grad = 0.0;
    expect_eq("call argument cache lp", ex.gradient(&grad), 0.5);
    expect_eq("call argument cache grad", grad, 1.0);
  }

  // Transformed-data While loop with a short-circuit guard, append_row.
  {
    DataMap d;
    d.set_int("N", 4);
    d.set_real_array("y", {2.0, 1.5, -1.0, 3.0});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tdext.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.3;
    double g = 0, lp = lex.gradient(&g);

    // c = 2 (first two positive), s = sum of y twice + c + sum of A col 1
    // where A[j] = [j, j] (row writes into a matrix).
    double s = 0;
    const double yv[4] = {2.0, 1.5, -1.0, 3.0};
    for (int rep = 0; rep < 2; ++rep)
      for (double v : yv) s += v;
    s += 2.0;
    double colsum = 0;
    for (int j = 1; j <= 4; ++j) colsum += j;
    s += colsum;
    using stan::math::var;
    var mu = 0.3;
    var acc = stan::math::normal_lpdf<false>(mu, s, 1.0);
    acc.grad();
    expect_eq("tdext lp", lp, acc.val());
    expect_eq("tdext dmu", g, mu.adj());
    stan::math::recover_memory();
  }

  // Transformed-data use of fmax/fmin/inv/inv_logit: the function
  // vocabulary must be the same one the ODE-side interpreter accepts.
  {
    DataMap d;
    d.set_real("a", 1.25);
    d.set_real("b", -0.5);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tdvocab.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.3;
    double g = 0;
    const double lp = lex.gradient(&g);

    const double m = std::fmax(1.25, -0.5) + std::fmin(1.25, -0.5) +
                     stan::math::inv(-0.5) + stan::math::inv_logit(1.25);
    // The four the register-machine compiler knew and the interpreter did
    // not. The compiled path falls back to the interpreter, so the
    // interpreter's vocabulary has to cover the compiler's.
    const double dens = stan::math::inv_gamma_lpdf(1.5, 2, 3) +
                        stan::math::weibull_lpdf(1.5, 2, 3) +
                        stan::math::logistic_lpdf(0.25, 0, 1) +
                        stan::math::double_exponential_lpdf(0.25, 0, 1);
    using stan::math::var;
    var mu = 0.3;
    var acc = stan::math::normal_lpdf<false>(mu, m + dens, 1.0);
    acc.grad();
    expect_eq("tdvocab lp", lp, acc.val());
    expect_eq("tdvocab dmu", g, mu.adj());
    stan::math::recover_memory();
  }

  // Deep array literals in transformed data have to land in a slot with the
  // same layout the JSON reader gives a data-block array of the same shape.
  // Rank 2 cannot tell the two apart -- an array-of-arrays and a matrix share
  // a flattening -- so the disagreement only shows from rank 3 down, where a
  // literal reached its slot with the trailing two extents swapped: a wrong
  // log density and wrong gradients, no error and no NaN.
  {
    DataMap d = DataMap::from_json(R"({
      "d2": [[11.0,12.0],[21.0,22.0]],
      "d3": [[[111.0,112.0],[121.0,122.0]],[[211.0,212.0],[221.0,222.0]]],
      "d4": [[[[1111.0,1112.0],[1121.0,1122.0]],
              [[1211.0,1212.0],[1221.0,1222.0]]],
             [[[2111.0,2112.0],[2121.0,2122.0]],
              [[2211.0,2212.0],[2221.0,2222.0]]]],
      "dvv": [[[111.0,112.0,113.0],[121.0,122.0,123.0]],
              [[211.0,212.0,213.0],[221.0,222.0,223.0]]],
      "dm": [[[111.0,112.0,113.0],[121.0,122.0,123.0]],
             [[211.0,212.0,213.0],[221.0,222.0,223.0]]]
    })");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/ndlit.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[3] = {0.3, 0.5, -0.25};
    for (int i = 0; i < 3; ++i) lex.params_data()[i] = q[i];
    double grad[3] = {0, 0, 0};
    const double lp = lex.gradient(grad);

    // Every element of both the literal and the data holds its own decimal
    // index code, and each is weighted by that same code, so the reference is
    // a sum of squares and any permutation of a layout moves it. Codes are
    // small integers: the sums are exact, and expect_eq can be exact too.
    double wt = 0, s3 = 0;
    for (int i = 1; i <= 2; ++i)
      for (int j = 1; j <= 2; ++j) {
        const double c2 = 10 * i + j;
        wt += c2 * c2;
        for (int k = 1; k <= 2; ++k) {
          const double c3 = 100 * i + 10 * j + k;
          wt += c3 * c3;
          s3 += c3 * c3;
          for (int l = 1; l <= 2; ++l) {
            const double c4 = 1000 * i + 100 * j + 10 * k + l;
            wt += c4 * c4;
          }
        }
        for (int k = 1; k <= 3; ++k) {
          const double c = 100 * i + 10 * j + k;
          wt += 2 * c * c;  // the array of vectors and the array of matrices
        }
      }
    // literal minus data, element by element: zero only if both paths agree.
    expect_eq("ndlit ddiff", grad[0], 0.0);
    expect_eq("ndlit dwt", grad[1], wt);
    // The interpreter's own N-D read of the literal, inside transformed data.
    expect_eq("ndlit ds3", grad[2], s3);
    expect_eq("ndlit lp", lp, wt * q[1] + s3 * q[2]);
  }

  // A while loop keeps its loop form: a retained loop by default, a
  // control island on the legacy path.
  {
    const auto count_whiles = [](const CompiledModel& model) {
      size_t whiles = 0;
      for (const Op& op : model.graph.ops)
        if (op.opcode == OP_ISLAND || op.opcode == OP_LOOP) ++whiles;
      return whiles;
    };
    const DataMap d =
        DataMap::from_json(slurp("tests/fixtures/whileloop.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/whileloop.tmir.sexp"), d);
    check(count_whiles(lm) == 3, "while loops lower as three retained loops");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.1;
    double grad = 0;
    const double lp = lex.gradient(&grad);
    // N = 4 iterations in the integer loop, then ceil(N / 2) iterations in
    // the real loop whose data-only update controls its next condition. The
    // Integer division truncates 5 to 2 and then 1 in the third loop. The
    // final four terms come from gathering two rows with an integer array
    // populated by indexed assignments.
    const double sum = 1 + 2 + 3 + 4 + 2 + 2 + 1 + 4;
    expect_ulp("while lp", lp, -0.5 * 0.1 * 0.1 + sum * 0.1);
    expect_eq("while grad", grad, -0.1 + sum);

    // Building a graph must not execute or replicate a data-controlled
    // while.  This trip count is just beyond the old lowering-time cap; the
    // model is intentionally not run, because this assertion is about the
    // finite program representation rather than a million-step evaluation.
    DataMap long_d;
    long_d.set_int("N", 1000001);
    CompiledModel long_loop =
        compile_model(slurp("tests/fixtures/whileloop.tmir.sexp"), long_d);
    check(count_whiles(long_loop) == 3,
          "long while lowers without a compile-time iteration cap");
    test_setenv("STANLI_STRUCTURED_LOOPS", "0", 1);
    CompiledModel islands =
        compile_model(slurp("tests/fixtures/whileloop.tmir.sexp"), d);
    test_unsetenv("STANLI_STRUCTURED_LOOPS");
    check(count_opcode(islands, OP_ISLAND) == 3,
          "while loops lower as three control islands on the legacy path");
  }

  // An integer local can be assigned a comparison of data-only real locals.
  // This takes lowering's integer evaluator because the surrounding ternary
  // also controls compile-time state.
  {
    const DataMap d =
        DataMap::from_json(slurp("tests/fixtures/int_real_compare.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/int_real_compare.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.1;
    double grad = 0;
    const double lp = lex.gradient(&grad);
    expect_eq("int real compare lp", lp, -0.5 * 0.1 * 0.1 + 0.1);
    expect_eq("int real compare grad", grad, -0.1 + 1.0);
  }

  // The matrix vocabulary used by ctsem remains inside a genuinely runtime
  // while: no compile-time unroll participates in either the value or the
  // reverse pass.  Pin the shared register-program implementation against
  // the same Stan Math calls, and exercise it again through write_array.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/structured_matrix_ops.tmir.sexp"), DataMap());
    const double point[5] = {0.5, 0.2, -0.1, 0.3, 0.4};
    Executor ex(cm.graph);
    cm.bind(ex);
    for (int i = 0; i < 5; ++i) ex.params_data()[i] = point[i];
    double gradient[5] = {};
    const double lp = ex.gradient(gradient);

    using stan::math::var;
    var theta = point[0];
    Eigen::Matrix<var, -1, -1> x(2, 2);
    for (int i = 0; i < 4; ++i) x.data()[i] = point[i + 1];
    const Eigen::Matrix<var, -1, -1> a =
        stan::math::add_diag(stan::math::crossprod(x), 2.0);
    Eigen::RowVector2d diagonal;
    diagonal << 2.0, 3.0;
    const Eigen::Matrix<var, -1, -1> row_diag =
        stan::math::add_diag(stan::math::crossprod(x), diagonal);
    const Eigen::Matrix<var, -1, -1> e = stan::math::matrix_exp(-a);
    const Eigen::Matrix<var, -1, -1> solved = stan::math::mdivide_left(a, x);
    const Eigen::Matrix<var, -1, -1> right_spd =
        stan::math::mdivide_right_spd(x, a);
    const Eigen::Matrix<var, -1, -1> q = stan::math::quad_form_sym(a, x);
    const Eigen::Matrix<var, -1, -1> tc = stan::math::tcrossprod(x);
    var score = e(0, 0) - 0.7 * e(1, 0) + 1.3 * solved(0, 1) +
                0.6 * right_spd(1, 0) + 0.4 * q(1, 1) - 0.2 * tc(0, 1) +
                0.05 * row_diag(1, 1);
    var reference = theta + score;
    reference.grad();
    expect_ulp("structured matrix ops lp", lp, reference.val());
    expect_eq("structured matrix ops theta gradient", gradient[0], 1.0);
    for (int i = 0; i < 4; ++i)
      expect_ulp("structured matrix ops gradient " + std::to_string(i),
                 gradient[i + 1], x.data()[i].adj());
    stan::math::recover_memory();

    double repeated[5] = {};
    expect_eq("structured matrix ops repeated lp", ex.gradient(repeated), lp);
    for (int i = 0; i < 5; ++i)
      expect_eq("structured matrix ops repeated gradient " + std::to_string(i),
                repeated[i], gradient[i]);

    check(cm.write_array && cm.write_array->truncated.empty(),
          "structured matrix ops write_array compiled");
    if (cm.write_array && cm.write_array->truncated.empty()) {
      Executor wex(std::move(cm.write_array->graph));
      cm.write_array->bind(wex);
      for (int i = 0; i < 5; ++i) wex.params_data()[i] = point[i];
      wex.run_forward_only();
      bool found = false;
      for (const auto& column : cm.write_array->columns) {
        if (column.name != "score") continue;
        found = true;
        expect_ulp("structured matrix ops write_array score",
                   wex.value_ptr(column.slot)[0], lp - point[0]);
      }
      check(found, "structured matrix ops has score column");
    }
  }

  // x[idx] = rhs with a repeated index: the last write to a position wins,
  // so a scatter must be ordered element writes, not one fused store.
  {
    const DataMap d =
        DataMap::from_json(slurp("tests/fixtures/scatter_assign.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/scatter_assign.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[3] = {0.1, 0.0, -0.1};
    for (int i = 0; i < 3; ++i) lex.params_data()[i] = q[i];
    double grad[3] = {0, 0, 0};
    const double lp = lex.gradient(grad);
    // idx = {4, 1, 4}: x[1] = q[1], x[4] = q[2], and q[0] reaches nothing.
    expect_eq("scatter lp", lp, -0.5 * (0.01 + 0.01) + 0.01);
    expect_eq("scatter grad dead", grad[0], -q[0]);
    expect_eq("scatter grad x1", grad[1], -q[1] + 2 * q[1]);
    expect_eq("scatter grad x4", grad[2], -q[2] + 2 * q[2]);
  }

  // A[i, lo:hi] on a 2-D int array: graph order is outer-major, so a wrong
  // offset here reads a neighbouring row and stays silent.
  {
    const DataMap d =
        DataMap::from_json(slurp("tests/fixtures/arr2d_rowrange.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/arr2d_rowrange.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.1;
    double grad = 0;
    const double lp = lex.gradient(&grad);
    const double sum = 1 + 2 + 5 + 6 + 7 + 9;
    expect_eq("arr2d rowrange lp", lp, -0.5 * 0.1 * 0.1 + sum * 0.1);
    expect_eq("arr2d rowrange grad", grad, -0.1 + sum);
  }

  // An uninitialized UDF local still denotes a value before its first write.
  // A data-dependent count that comes out zero leaves the local zero-width,
  // so the guarded loop that fills it performs no indexed assignment and the
  // declaration is the only mention the lowering ever sees. The name still
  // has to return an (empty) array rather than fail as unknown.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_int_array("input", {4, 5, 6});  // nothing equals 9
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/udf_empty_local.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.1;
    double grad = 0;
    const double lp = lex.gradient(&grad);
    // No match means no `selected` term: std_normal on theta and nothing else.
    expect_eq("empty UDF local lp", lp, -0.5 * 0.1 * 0.1);
    expect_eq("empty UDF local grad", grad, -0.1);
  }

  // A data-only `if` on a UDF local that lives in the graph. DataOnly is an
  // adlevel, not a promise that the interpreter can see the values, so the
  // condition has to compile to a region program like any other unfoldable
  // guard.
  {
    DataMap d = DataMap::from_json(R"({"m": [[1.0,-2.0],[-3.0,4.0]]})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/localmat_cond.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.1;
    double grad = 0;
    const double lp = lex.gradient(&grad);
    // Positive entries doubled, the holes taken from the input unchanged:
    // 2 + (-2) + (-3) + 8.
    expect_eq("local matrix condition lp", lp, -0.5 * 0.1 * 0.1 + 5.0 * 0.1);
    expect_eq("local matrix condition grad", grad, -0.1 + 5.0);
  }

  // Matrix block assignment through index lists. ridx repeats index 2, so
  // the cell it names twice must hold the value written last.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/matblockassign.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/matblockassign.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.1;
    double grad = 0;
    const double lp = lex.gradient(&grad);
    // Surviving cells: b[3,1], b[2,1], b[3,2], b[2,2], 7 and 8, times theta.
    const double total = 5.0 + 3.0 + 6.0 + 4.0 + 7.0 + 8.0;
    expect_ulp("matrix block assignment lp", lp,
               -0.5 * 0.1 * 0.1 + total * 0.1);
    expect_eq("matrix block assignment grad", grad, -0.1 + total);
  }

  // Model-block UDFs on parameters, inlined: scalar chain, vector return,
  // statement body with local accumulator + loop.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_real_array("y", {1.1, -0.4, 2.2});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/udflp.tmir.sexp"), d);
    check(lm.n_unconstrained == 4, "udflp 4 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[4] = {0.7, 0.2, -1.1, 0.9};
    for (int i = 0; i < 4; ++i) lex.params_data()[i] = q[i];
    double grad[4] = {0, 0, 0, 0};
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var mu = q[0];
    Eigen::Matrix<var, -1, 1> dv(3);
    dv << q[1], q[2], q[3];
    const double yarr[3] = {1.1, -0.4, 2.2};
    Eigen::Map<const Eigen::VectorXd> ym(yarr, 3);
    var af = mu * 2.0 + 0.5;
    var t1 = stan::math::normal_lpdf<false>(ym, af, 1.0);
    Eigen::Matrix<var, -1, 1> hv = stan::math::divide(dv, 2.0);
    var t2 = stan::math::normal_lpdf<false>(hv, 0.0, 1.0);
    var s = 0.0;
    for (int i = 0; i < 3; ++i) s = s + dv(i);
    var t3 = stan::math::normal_lpdf<false>(s / 2.0, 0.0, 1.0);
    var acc = ((t1 + t2) + t3);
    acc.grad();
    expect_eq("udflp lp", lp, acc.val());
    expect_eq("udflp gmu", grad[0], mu.adj());
    for (int i = 0; i < 3; ++i)
      expect_eq("udflp gd" + std::to_string(i), grad[1 + i], dv(i).adj());
    stan::math::recover_memory();
  }

  // A write-array UDF may derive a local integer extent from the logical
  // shape of a parameter argument before using it in loop bounds and local
  // declarations.  The speculative runtime-control scan must follow those
  // scalar-int bindings in statement order instead of looking through the
  // whole block with an empty local environment.
  {
    DataMap d;
    d.set_int("D", 2);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/udf_local_shape.tmir.sexp"), d);
    check(lm.write_array && lm.write_array->truncated.empty(),
          "UDF local shape write_array compiled");
    if (lm.write_array && lm.write_array->truncated.empty()) {
      Executor wex(std::move(lm.write_array->graph));
      lm.write_array->bind(wex);
      const double q[4] = {0.25, -0.5, 0.75, -0.2};
      for (int i = 0; i < 4; ++i) wex.params_data()[i] = q[i];
      wex.run_forward_only();
      const double expected[4] = {1.25, -0.5, 0.75, -0.2};
      bool found = false;
      for (const auto& column : lm.write_array->columns) {
        if (column.name != "copied") continue;
        found = true;
        check(column.len == 4, "UDF local shape copied matrix width");
        const double* value = wex.value_ptr(column.slot);
        for (int i = 0; i < 4; ++i)
          expect_eq("UDF local shape write_array " + std::to_string(i),
                    value[i], expected[i]);
      }
      check(found, "UDF local shape has copied matrix");
    }
  }

  // Both arms of a runtime UDF condition return, and one arm mutates a local
  // before returning it.  Lower the arm effects and the returned value as
  // separate structured joins so value, gradients, and write_array all take
  // the same path as the generated C++.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/udf_conditional_return.tmir.sexp"), DataMap());
    const double points[2][2] = {{0.5, -0.2}, {-0.5, -0.2}};
    const double want_lp[2] = {1.3, -0.7};
    const double want_grad[2][2] = {{3.0, 1.0}, {1.0, 1.0}};
    for (int point = 0; point < 2; ++point) {
      Executor ex(cm.graph);
      cm.bind(ex);
      for (int i = 0; i < 2; ++i) ex.params_data()[i] = points[point][i];
      double gradient[2] = {};
      expect_eq("conditional UDF return lp " + std::to_string(point),
                ex.gradient(gradient), want_lp[point]);
      for (int i = 0; i < 2; ++i)
        expect_eq("conditional UDF return gradient " + std::to_string(point) +
                      "." + std::to_string(i),
                  gradient[i], want_grad[point][i]);
    }
    check(cm.write_array && cm.write_array->truncated.empty(),
          "conditional UDF return write_array compiled");
    if (cm.write_array && cm.write_array->truncated.empty()) {
      Executor wex(std::move(cm.write_array->graph));
      cm.write_array->bind(wex);
      wex.params_data()[0] = points[0][0];
      wex.params_data()[1] = points[0][1];
      wex.run_forward_only();
      bool found = false;
      for (const auto& column : cm.write_array->columns) {
        if (column.name != "chosen") continue;
        found = true;
        const double* value = wex.value_ptr(column.slot);
        expect_eq("conditional UDF return write_array 0", value[0], 1.0);
        expect_eq("conditional UDF return write_array 1", value[1], 0.3);
      }
      check(found, "conditional UDF return has chosen vector");
    }
  }

  // Overloaded user functions (issue #125): stanc3 keeps every overload of
  // cox_lccdf under the same fdname, and --O1 inlines the lcdf wrapper so
  // log_prob calls both the vector and the scalar overload directly.
  // Resolution must go by argument type, not last-definition-wins.
  {
    DataMap d = DataMap::from_json(
        R"({"N": 3, "Y": [1.0, 2.0, 3.0], "bhaz": [0.4, 0.5, 0.6],
            "cbhaz": [0.2, 0.3, 0.4]})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/overload.tmir.sexp"), d);
    check(lm.n_unconstrained == 1, "overload 1 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.7;
    double grad[1] = {0};
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var alpha = 0.7;
    Eigen::VectorXd bh(3), cb(3);
    bh << 0.4, 0.5, 0.6;
    cb << 0.2, 0.3, 0.4;
    Eigen::Matrix<var, -1, 1> mu = alpha * bh;
    var acc = -stan::math::dot_product(cb, mu) +
              stan::math::log1m_exp(-cb(0) * alpha) +
              stan::math::normal_lpdf<false>(alpha, 0.0, 1.0);
    acc.grad();
    expect_ulp("overload lp", lp, acc.val());
    // alpha collects adjoints from three terms; the graph and the var tape
    // associate that sum differently, so this is a few-ulp comparison.
    check(std::fabs(grad[0] - alpha.adj()) < 1e-12,
          "overload galpha " + std::to_string(grad[0]) + " vs " +
              std::to_string(alpha.adj()));
    stan::math::recover_memory();
  }

  // Data-dependent gather width (issue #133): the same program gathers a
  // slice whose length transformed data computes from the data. Nev > 0 is
  // an ordinary gather; Nev == 0 must compile and contribute exactly zero
  // to lp and the gradient.
  {
    const double yv[4] = {1.5, -0.5, 2.0, 0.25};
    const double q[4] = {0.3, -0.8, 0.1, 0.6};
    for (int empty = 0; empty < 2; ++empty) {
      DataMap d;
      d.set_int("N", 4);
      d.set_real_array("Y", {yv[0], yv[1], yv[2], yv[3]});
      d.set_int_array("cens", empty ? std::vector<int>{1, 1, 1, 1}
                                    : std::vector<int>{0, 1, 0, 1});
      CompiledModel lm =
          compile_model(slurp("tests/fixtures/emptygather.tmir.sexp"), d);
      check(lm.n_unconstrained == 4, "emptygather 4 unconstrained");
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      for (int i = 0; i < 4; ++i) lex.params_data()[i] = q[i];
      double grad[4] = {0, 0, 0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      Eigen::Matrix<var, -1, 1> mu(4);
      mu << q[0], q[1], q[2], q[3];
      var acc = 0.0;
      if (!empty) {
        Eigen::Matrix<var, -1, 1> mg(2);
        mg << mu(0) * 2.0, mu(2) * 2.0;
        Eigen::VectorXd yg(2);
        yg << yv[0], yv[2];
        acc += stan::math::dot_product(yg, stan::math::exp(mg)) +
               stan::math::sum(mg);
      }
      acc += stan::math::normal_lpdf<false>(mu, 0.0, 1.0);
      acc.grad();
      const std::string tag = empty ? "emptygather empty" : "emptygather full";
      expect_ulp(tag + " lp", lp, acc.val());
      for (int i = 0; i < 4; ++i)
        check(std::fabs(grad[i] - mu(i).adj()) < 1e-12,
              tag + " g" + std::to_string(i));
      stan::math::recover_memory();
    }
  }

  // An empty int data array as a gather index: JSON [] must stay
  // integer-typed, the empty density term must contribute nothing, and the
  // generated quantity over the empty gather must write 0.
  {
    const double yv[4] = {1.5, -0.5, 2.0, 0.25};
    for (int empty = 0; empty < 2; ++empty) {
      DataMap d = DataMap::from_json(
          empty ? R"({"M": 0, "idx": [], "Y": [1.5, -0.5, 2.0, 0.25]})"
                : R"({"M": 2, "idx": [3, 1], "Y": [1.5, -0.5, 2.0, 0.25]})");
      CompiledModel lm =
          compile_model(slurp("tests/fixtures/emptyidx.tmir.sexp"), d);
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      lex.params_data()[0] = 0.4;
      double grad[1] = {0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      var mu = 0.4;
      var acc = stan::math::normal_lpdf<false>(mu, 0.0, 2.0);
      if (!empty) {
        Eigen::Matrix<var, -1, 1> yg(2);
        yg << yv[2], yv[0];
        acc += stan::math::normal_lpdf<false>(yg, mu, 1.0);
      }
      acc.grad();
      const std::string tag = empty ? "emptyidx empty" : "emptyidx full";
      expect_ulp(tag + " lp", lp, acc.val());
      check(std::fabs(grad[0] - mu.adj()) < 1e-12, tag + " grad");
      stan::math::recover_memory();
      check(lm.write_array && lm.write_array->truncated.empty(),
            tag + " write_array compiled");
      if (lm.write_array && lm.write_array->truncated.empty()) {
        Executor wex(std::move(lm.write_array->graph));
        lm.write_array->bind(wex);
        wex.params_data()[0] = 0.4;
        wex.run_forward_only();
        bool found = false;
        for (const auto& col : lm.write_array->columns) {
          if (col.name != "gsum") continue;
          found = true;
          check(*wex.value_ptr(col.slot) == (empty ? 0.0 : yv[2] + yv[0]),
                tag + " gsum value");
        }
        check(found, tag + " has gsum");
      }
    }
  }

  // Data-dependent range bounds: hi < lo is an empty slice under CmdStan's
  // rvalue semantics, whatever the endpoints, on the array, vector, and
  // matrix row-range paths alike.
  {
    for (int empty = 0; empty < 2; ++empty) {
      DataMap d = DataMap::from_json(
          empty ? R"({"K": 0, "lo": 5, "hi": 2, "Y": [1.5, -0.5, 2.0, 0.25],
                      "Zm": [[1, 5], [2, 6], [3, 7], [4, 8]]})"
                : R"({"K": 2, "lo": 2, "hi": 3, "Y": [1.5, -0.5, 2.0, 0.25],
                      "Zm": [[1, 5], [2, 6], [3, 7], [4, 8]]})");
      CompiledModel lm =
          compile_model(slurp("tests/fixtures/rangeclamp.tmir.sexp"), d);
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      lex.params_data()[0] = -0.3;
      double grad[1] = {0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      var mu = -0.3;
      var acc = stan::math::normal_lpdf<false>(mu, 0.0, 2.0);
      if (empty) {
        // sum over an empty slice is 0 on the array and matrix paths; the
        // empty vector slice contributes nothing at all.
        acc += stan::math::normal_lpdf<false>(0.0, mu, 1.0) * 2.0;
      } else {
        acc += stan::math::normal_lpdf<false>(1.0 + 2.0, mu, 1.0);
        Eigen::Matrix<var, -1, 1> yg(2);
        yg << -0.5, 2.0;
        acc += stan::math::normal_lpdf<false>(yg, mu, 1.0);
        acc += stan::math::normal_lpdf<false>(1.0 + 2.0, mu, 1.0);
      }
      acc.grad();
      const std::string tag = empty ? "rangeclamp empty" : "rangeclamp full";
      expect_ulp(tag + " lp", lp, acc.val());
      check(std::fabs(grad[0] - mu.adj()) < 1e-12, tag + " grad");
      stan::math::recover_memory();
    }
  }

  // Index bounds are data, so the lowering must reject an out-of-bounds
  // index at bind time the way CmdStan rejects it at runtime; the silent
  // alternative is reading a neighboring arena slot. hi < lo ranges stay
  // empty. One fixture; the base dataset is in bounds and each violation
  // overrides one index.
  {
    const auto base_data = [] {
      DataMap d = DataMap::from_json(
          R"({"k": 2, "idx": [1, 4], "lo": 2, "hi": 3, "i1": 1, "j1": 2,
              "rl": 2, "rh": 3, "m": 3, "Y": [1.5, -0.5, 2.0, 0.25],
              "Zm": [[1, 5], [2, 6], [3, 7], [4, 8]]})");
      return d;
    };
    const std::string mir = slurp("tests/fixtures/oob.tmir.sexp");

    const auto reference = [&](bool rows_empty, double q) {
      using stan::math::var;
      var mu = q;
      const double yv[4] = {1.5, -0.5, 2.0, 0.25};
      const double zm[4][2] = {{1, 5}, {2, 6}, {3, 7}, {4, 8}};
      Eigen::Matrix<var, -1, 1> v(4);
      for (int i = 0; i < 4; ++i) v(i) = yv[i] + mu;
      Eigen::Matrix<var, -1, -1> M(4, 2);
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j) M(i, j) = zm[i][j] + mu;
      var acc = stan::math::normal_lpdf<false>(v(1), 0.0, 1.0);
      Eigen::Matrix<var, -1, 1> vg(2);
      vg << v(0), v(3);
      acc += stan::math::normal_lpdf<false>(vg, 0.0, 1.0);
      Eigen::Matrix<var, -1, 1> vs(2);
      vs << v(1), v(2);
      acc += stan::math::normal_lpdf<false>(vs, 0.0, 1.0);
      acc += stan::math::normal_lpdf<false>(M(0, 0) + M(0, 1), 0.0, 1.0);
      acc += stan::math::normal_lpdf<false>(
          M(0, 1) + M(1, 1) + M(2, 1) + M(3, 1), 0.0, 1.0);
      if (rows_empty) {
        // sum over the empty row range is 0 on both terms.
        acc += stan::math::normal_lpdf<false>(0.0, 0.0, 1.0) * 2.0;
      } else {
        acc += stan::math::normal_lpdf<false>(
            M(1, 0) + M(2, 0) + M(1, 1) + M(2, 1), 0.0, 1.0);
        acc += stan::math::normal_lpdf<false>(M(1, 1) + M(2, 1), 0.0, 1.0);
      }
      acc += stan::math::normal_lpdf<false>(3.0, 0.0, 1.0);  // tds = xs[3]
      acc += stan::math::normal_lpdf<false>(mu, 0.0, 2.0);
      return acc;
    };

    const auto run_case = [&](DataMap d, bool rows_empty,
                              const std::string& tag) {
      CompiledModel lm = compile_model(mir, d);
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      lex.params_data()[0] = 0.35;
      double grad[1] = {0};
      const double lp = lex.gradient(grad);
      using stan::math::var;
      var acc = reference(rows_empty, 0.35);
      acc.grad();
      expect_ulp(tag + " lp", lp, acc.val());
      stan::math::recover_memory();
    };
    run_case(base_data(), false, "oob ctrl");
    {
      DataMap d = base_data();
      d.set_int("rl", 5);
      d.set_int("rh", 2);
      run_case(std::move(d), true, "oob rows-empty");
    }

    const auto expect_oob = [&](const char* name, long v,
                                const std::string& tag) {
      DataMap d = base_data();
      d.set_int(name, v);
      bool threw = false;
      try {
        compile_model(mir, d);
      } catch (const std::exception& ex) {
        threw =
            std::string(ex.what()).find("out of bounds") != std::string::npos;
        if (!threw)
          std::printf("  %s threw without 'out of bounds': %s\n", tag.c_str(),
                      ex.what());
      }
      check(threw, tag + " rejected");
    };
    expect_oob("k", 7, "oob v[7]");
    expect_oob("hi", 9, "oob v[2:9]");
    expect_oob("i1", 5, "oob M[5]");
    expect_oob("j1", 5, "oob M[:,5]");
    expect_oob("rh", 9, "oob M[2:9]");
    expect_oob("m", 9, "oob interp xs[9]");
    {
      DataMap d = base_data();
      d.set_int_array("idx", {1, 9});
      bool threw = false;
      try {
        compile_model(mir, d);
      } catch (const std::exception& ex) {
        threw =
            std::string(ex.what()).find("out of bounds") != std::string::npos;
      }
      check(threw, "oob v[[1,9]] rejected");
    }
  }

  // Index forms on parameters: gather, Between read/write, matrix row and
  // column slices, column writes.
  {
    DataMap d = DataMap::from_json(R"({"N": 4, "idx": [2, 1, 3, 2]})");
    CompiledModel lm = compile_model(slurp("tests/fixtures/idx.tmir.sexp"), d);
    check(lm.n_unconstrained == 9, "idx 9 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[9] = {0.5, -0.7, 1.2,                   // v
                         0.1, -0.3, 0.6, 0.9, -1.1, 0.2};  // M col-major
    for (int i = 0; i < 9; ++i) lex.params_data()[i] = q[i];
    double grad[9];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> v(3);
    v << q[0], q[1], q[2];
    Eigen::Matrix<var, -1, 1> M(6);  // col-major flat
    for (int i = 0; i < 6; ++i) M(i) = q[3 + i];
    const int idx[4] = {2, 1, 3, 2};
    Eigen::Matrix<var, -1, 1> g(4);
    for (int k = 0; k < 4; ++k) g(k) = v(idx[k] - 1);
    Eigen::Matrix<var, -1, 1> w(2);
    w << v(1), v(2);
    Eigen::Matrix<var, -1, 1> row1(3), col3(2), lcol2(2), v12(2);
    row1 << M(0), M(2), M(4);  // row 1 of 2x3 col-major: stride 2
    col3 << M(4), M(5);        // column 3: offset 4
    lcol2 = w;                 // L[:,2] = w
    v12 << v(0), v(1);
    var t1 = stan::math::normal_lpdf<false>(g, 0.0, 1.0);
    var t2 = stan::math::normal_lpdf<false>(v12, 0.0, 2.0);
    var t3 = stan::math::normal_lpdf<false>(row1, 0.0, 1.0);
    var t4 = stan::math::normal_lpdf<false>(col3, 0.0, 3.0);
    var t5 = stan::math::normal_lpdf<false>(lcol2, 0.0, 2.0);
    var t6 = stan::math::normal_lpdf<false>(w, 1.0, 1.0);
    var acc = (((((t1 + t2) + t3) + t4) + t5) + t6);
    acc.grad();
    expect_eq("idx lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("idx gv" + std::to_string(i), grad[i], v(i).adj());
    for (int i = 0; i < 6; ++i)
      expect_eq("idx gM" + std::to_string(i), grad[3 + i], M(i).adj());
    stan::math::recover_memory();
  }

  // stanc3's partial evaluator rewrites x[a:N] to an Upfrom index when N is
  // x's declared extent. Upfrom must lower exactly like Between(a, extent),
  // both on a declared local and on a UDF's unsized formal.
  {
    CompiledModel upfrom_model = compile_model(kUpfromSliceMir, DataMap{});
    CompiledModel between_model = compile_model(kBetweenSliceMir, DataMap{});
    check(same_graph_structure(upfrom_model, between_model),
          "v[k:] and v[k:N] on a declared vector lower identically");
  }
  {
    DataMap d = DataMap::from_json(
        R"({"N": 3, "K": 3, "flag": 0, )"
        R"("W": [[1.0, 2.0, 4.0], [1.0, 0.5, 2.0], [1.0, -1.0, 6.0]]})");
    std::string between_mir = slurp("tests/fixtures/udf.tmir.sexp");
    std::string upfrom_mir = between_mir;
    check(rewrite_udf_rows_slice_to_upfrom(upfrom_mir) == 2,
          "udf fixture has two W[1:rows(W), k] slices to rewrite");
    CompiledModel upfrom_model = compile_model(upfrom_mir, d);
    CompiledModel between_model = compile_model(between_mir, d);
    check(same_graph_structure(upfrom_model, between_model),
          "W[k:] and W[k:rows(W)] on a UDF formal lower identically");
  }

  // A gathered container remains an ordinary graph value for downstream
  // arithmetic. Duplicate selectors must accumulate through OP_GATHER's
  // scatter-add backward rather than overwrite the repeated source entry.
  {
    DataMap d;
    d.set_int_array("idx", {5, 2, 2});
    CompiledModel gm =
        compile_model(slurp("tests/fixtures/viewa_outer_gather.tmir.sexp"), d);
    check(count_opcode(gm, OP_GATHER) == 1,
          "duplicate outer gather has one explicit gather");
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    const double q[5] = {0.25, -0.5, 0.75, -1.0, 1.5};
    for (int i = 0; i < 5; ++i) gex.params_data()[i] = q[i];
    double gradient[5] = {};
    expect_eq("duplicate outer gather lp", gex.gradient(gradient),
              q[4] + 110.0 * q[1]);
    const double want_gradient[5] = {0.0, 110.0, 0.0, 0.0, 1.0};
    for (int i = 0; i < 5; ++i)
      expect_eq("duplicate outer gather g" + std::to_string(i), gradient[i],
                want_gradient[i]);
  }

  // A logical view belongs to the name binding, not to the flat parameter
  // slot it aliases. M, r, v, and q are simultaneously live here and all
  // share storage with different matrix/vector labels.
  {
    try {
      DataMap d;
      d.set_real_array("A", {1.0, 3.0, 2.0, 4.0}, {2, 2});
      d.set_real_array("y", {0.5, -0.25}, {2});
      CompiledModel lm =
          compile_model(slurp("tests/fixtures/viewalias.tmir.sexp"), d);
      check(lm.n_unconstrained == 6, "viewalias 6 unconstrained");
      Executor lex(std::move(lm.graph));
      lm.bind(lex);
      const double points[2][6] = {{0.25, -0.5, 0.75, -1.0, 1.25, -1.5},
                                   {-0.25, 0.5, -0.75, 1.0, -1.25, 1.5}};
      for (int c = 0; c < 2; ++c) {
        const double* q = points[c];
        for (int i = 0; i < 6; ++i) lex.params_data()[i] = q[i];
        double grad[6] = {0, 0, 0, 0, 0, 0};
        const double lp = lex.gradient(grad);

        // M is 2x3 column-major. Its first row is q[0], q[2] + q[5],
        // q[4] after the indexed update, while the still-live flat v alias
        // remains q. D pins provenance after a parameter element write.
        // E pins both parameter-dependent whole-assignment arms while
        // retaining its matrix shape. B is updated from a data-only
        // reduction and must remain eligible as normal_id_glm's data matrix.
        // All inputs are binary fractions, so this independent closed form
        // and its full gradient are exact.
        const double aq0 = std::abs(q[0]);
        const double r0 = 0.5 - (q[0] + 3.0 * q[1]);
        const double r1 = -0.25 - (3.0 * q[0] + 4.0 * q[1]);
        const double want_lp =
            q[0] * q[0] + (q[2] + q[5]) * q[1] + q[4] * q[2] +
            2.0 * (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] +
                   q[4] * q[4] + q[5] * q[5]) +
            4.0 * q[0] + 4.0 * q[1] + q[0] * q[1] +
            aq0 * (4.0 * q[0] + 6.0 * q[1]) - 0.5 * r0 * r0 - 0.5 * r1 * r1;
        const double e_g0 =
            q[0] > 0 ? 8.0 * q[0] + 6.0 * q[1] : -8.0 * q[0] - 6.0 * q[1];
        const double want_grad[6] = {
            6.0 * q[0] + 4.0 + q[1] + e_g0 + r0 + 3.0 * r1,
            4.0 * q[1] + q[2] + q[5] + 4.0 + q[0] + 6.0 * aq0 + 3.0 * r0 +
                4.0 * r1,
            4.0 * q[2] + q[1] + q[4],
            4.0 * q[3],
            4.0 * q[4] + q[2],
            4.0 * q[5] + q[1]};
        const std::string tag = "viewalias" + std::to_string(c);
        expect_eq(tag + " lp", lp, want_lp);
        for (int i = 0; i < 6; ++i)
          expect_eq(tag + " g" + std::to_string(i), grad[i], want_grad[i]);
      }

      // The same logical matrix alias must survive the separately lowered
      // write_array graph used for transformed-parameter CSV columns.
      check(lm.write_array && lm.write_array->truncated.empty(),
            "viewalias write_array compiled");
      if (lm.write_array && lm.write_array->truncated.empty()) {
        Executor wex(std::move(lm.write_array->graph));
        lm.write_array->bind(wex);
        for (int i = 0; i < 6; ++i) wex.params_data()[i] = points[0][i];
        wex.run_forward_only();
        bool found_w = false;
        for (const auto& col : lm.write_array->columns) {
          if (col.name != "W") continue;
          found_w = true;
          check(col.naming == CompiledModel::ParamView::Naming::Matrix &&
                    col.rows == 2,
                "viewalias write_array matrix metadata");
          const double* w = wex.value_ptr(col.slot);
          for (int i = 0; i < 6; ++i)
            expect_eq("viewalias write W" + std::to_string(i), w[i],
                      points[0][i]);
        }
        check(found_w, "viewalias write_array has W");
      }
    } catch (const std::exception& e) {
      ++failures;
      std::printf("FAIL viewalias compile: %s\n", e.what());
    }
  }

  // categorical_lpmf, scalar and array outcomes, on a simplex parameter.
  {
    DataMap d = DataMap::from_json(R"({"K": 3, "y": 2, "ys": [3, 1, 3]})");
    CompiledModel lm = compile_model(slurp("tests/fixtures/cat.tmir.sexp"), d);
    check(lm.n_unconstrained == 2, "cat 2 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.3;
    lex.params_data()[1] = -0.8;
    double grad[2];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> q(2);
    q << 0.3, -0.8;
    var lj = 0.0;
    Eigen::Matrix<var, -1, 1> theta = stan::math::simplex_constrain(q, lj);
    var t1 = stan::math::categorical_lpmf<false>(2, theta);
    const std::vector<int> ys = {3, 1, 3};
    var t2 = stan::math::categorical_lpmf<false>(ys, theta);
    var acc = (t1 + t2) + lj;
    acc.grad();
    expect_eq("cat lp", lp, acc.val());
    for (int i = 0; i < 2; ++i)
      expect_eq("cat g" + std::to_string(i), grad[i], q(i).adj());
    stan::math::recover_memory();
  }

  // An empty array outcome validates a parameter vector but contributes a
  // disconnected zero. In particular, an unselected zero probability must
  // not be reached by a zero-adjoint log backward and turn into 0 / 0.
  {
    std::string mir = slurp("tests/fixtures/cat.tmir.sexp");
    auto replace_all = [&](const std::string& from, const std::string& to) {
      for (size_t pos = mir.find(from); pos != std::string::npos;
           pos = mir.find(from, pos + to.size()))
        mir.replace(pos, from.size(), to);
    };
    replace_all("(pattern (Lit Int 3))", "(pattern (Lit Int 0))");
    replace_all("(constrain Simplex)", "(constrain Identity)");

    DataMap d;
    d.set_int("K", 2);
    d.set_int("y", 1);
    d.set_int_array("ys", {});
    CompiledModel lm = compile_model(mir, d);
    check(lm.n_unconstrained == 2, "empty categorical identity width");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 1.0;
    lex.params_data()[1] = 0.0;
    double grad[2];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> theta(2);
    theta << 1.0, 0.0;
    const std::vector<int> empty;
    var acc = stan::math::categorical_lpmf<false>(1, theta) +
              stan::math::categorical_lpmf<false>(empty, theta);
    acc.grad();
    expect_eq("empty categorical lp", lp, acc.val());
    for (int i = 0; i < 2; ++i)
      expect_eq("empty categorical g" + std::to_string(i), grad[i],
                theta(i).adj());
    stan::math::recover_memory();
  }

  // Parameter-dependent categorical-logit calls keep their exact value and
  // pullback. Exercise both scalar
  // and array outcomes under normalized and propto MIR flags.
  for (bool propto : {false, true}) {
    std::string mir = slurp("tests/fixtures/cat.tmir.sexp");
    auto replace_all = [&](const std::string& from, const std::string& to) {
      for (size_t pos = mir.find(from); pos != std::string::npos;
           pos = mir.find(from, pos + to.size()))
        mir.replace(pos, from.size(), to);
    };
    replace_all("categorical_lpmf", "categorical_logit_lpmf");
    replace_all("(constrain Simplex)", "(constrain Identity)");
    if (propto) replace_all("(FnLpmf false)", "(FnLpmf true)");

    DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
    CompiledModel lm = compile_model(mir, d);
    check(lm.n_unconstrained == 3,
          "categorical logit identity parameter width");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double point[3] = {-0.4, 0.2, 0.9};
    std::copy(std::begin(point), std::end(point), lex.params_data());
    double grad[3];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> beta(3);
    for (int i = 0; i < 3; ++i) beta(i) = point[i];
    const std::vector<int> ys = {3, 1, 3};
    var acc = propto ? stan::math::categorical_logit_lpmf<true>(2, beta) +
                           stan::math::categorical_logit_lpmf<true>(ys, beta)
                     : stan::math::categorical_logit_lpmf<false>(2, beta) +
                           stan::math::categorical_logit_lpmf<false>(ys, beta);
    acc.grad();
    const std::string mode = propto ? "propto" : "normalized";
    expect_eq("categorical logit parameter " + mode + " lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq(
          "categorical logit parameter " + mode + " g" + std::to_string(i),
          grad[i], beta(i).adj());
    stan::math::recover_memory();
  }

  // Reverse replay must accumulate into an adjoint already written by a
  // later consumer in the same order as Stan's tape. Grouping the density's
  // two logit contributions before adding that existing 0.1 changes one bit.
  {
    Graph g;
    const int beta = g.add_slot(3, true);
    const int outcome = g.add_slot(1, false);
    const int cat = g.add_slot(1, false);
    const int cat_op = g.add_op(OP_CATEGORICAL, {outcome, beta}, cat);
    g.ops[(size_t)cat_op].variant =
        kCategoricalLogit | kCategoricalScalarOutcome | kCategoricalArgAutodiff;
    const int beta0 = g.add_slot(1, false);
    g.add_op(OP_INDEX, {beta}, beta0, {0});
    const int scale = g.add_slot(1, false);
    const int linear = g.add_slot(1, false);
    g.add_op(OP_MUL, {beta0, scale}, linear);
    const int total = g.add_slot(1, false);
    g.add_op(OP_ADD, {cat, linear}, total);
    g.result_slot = total;

    Executor ex(std::move(g));
    std::fill(ex.params_data(), ex.params_data() + 3, -2.0);
    ex.value_ptr(outcome)[0] = 1.0;
    ex.value_ptr(scale)[0] = 0.1;
    double got_grad[3];
    const double got = ex.gradient(got_grad);

    Eigen::Matrix<stan::math::var, -1, 1> ref_beta(3);
    ref_beta << -2.0, -2.0, -2.0;
    stan::math::var ref_cat =
        stan::math::categorical_logit_lpmf<false>(1, ref_beta);
    stan::math::var ref_linear = ref_beta(0) * 0.1;
    stan::math::var ref = ref_cat + ref_linear;
    ref.grad();
    expect_eq("categorical existing-adjoint value", got, ref.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("categorical existing-adjoint g" + std::to_string(i),
                got_grad[i], ref_beta(i).adj());
    stan::math::recover_memory();
  }

  // The native scalar probability rule has one selected reciprocal. Pin its
  // non-unit upstream seed, += into an adjoint a later consumer already
  // wrote, propto/type behavior, and value-only -> gradient reuse against the
  // exact Stan tape it replaces.
  {
    Graph g;
    const int theta = g.add_slot(3, true);
    const int outcome = g.add_slot(1, false);
    const int cat = g.add_slot(1, false);
    const int cat_op = g.add_op(OP_CATEGORICAL, {outcome, theta}, cat);
    g.ops[(size_t)cat_op].variant =
        kCategoricalScalarOutcome | kCategoricalArgAutodiff | 0x80u;
    const int cat_scale = g.add_slot(1, false);
    const int scaled_cat = g.add_slot(1, false);
    g.add_op(OP_MUL, {cat, cat_scale}, scaled_cat);
    const int selected = g.add_slot(1, false);
    g.add_op(OP_INDEX, {theta}, selected, {1});
    const int linear_scale = g.add_slot(1, false);
    const int linear = g.add_slot(1, false);
    g.add_op(OP_MUL, {selected, linear_scale}, linear);
    const int total = g.add_slot(1, false);
    g.add_op(OP_ADD, {scaled_cat, linear}, total);
    g.result_slot = total;

    Executor ex(std::move(g));
    const double point[3] = {0.2, 0.7, 0.1};
    std::copy(std::begin(point), std::end(point), ex.params_data());
    ex.value_ptr(outcome)[0] = 2.0;
    ex.value_ptr(cat_scale)[0] = 0.3;
    ex.value_ptr(linear_scale)[0] = -0.2;
    expect_eq("categorical native value-only propto", ex.forward_value_only(),
              point[1] * -0.2);
    double got_grad[3];
    const double got = ex.gradient(got_grad);

    Eigen::Matrix<stan::math::var, -1, 1> ref_theta(3);
    ref_theta << point[0], point[1], point[2];
    stan::math::var ref =
        stan::math::categorical_lpmf<true>(2, ref_theta) * 0.3 +
        ref_theta(1) * -0.2;
    ref.grad();
    expect_eq("categorical native scalar value", got, ref.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("categorical native scalar g" + std::to_string(i), got_grad[i],
                ref_theta(i).adj());
    stan::math::recover_memory();
  }

  // Direct kernel edges for the same rule. A scalar categorical log only
  // connects the selected probability: an infinite seed must leave an
  // unselected signed zero untouched, while a zero seed over a selected zero
  // still follows the connected log edge and forms 0 / 0. Reusing the context
  // also pins that the native path carries no hidden scratch state.
  {
    const Kernel* kernel = find_kernel(OP_CATEGORICAL);
    check(kernel != nullptr, "categorical native kernel registered");
    check(kernel && kernel->scratch_size == nullptr,
          "categorical native kernel scratchless");
    if (kernel) {
      double outcome = 1.0;
      double theta[2] = {1.0, 0.0};
      double theta_adj[2] = {0.0, -0.0};
      double out = 0.0;
      KernelCtx ctx{};
      ctx.n_in = 2;
      ctx.in[0] = Desc{&outcome, 1};
      ctx.in[1] = Desc{theta, 2};
      ctx.in_adj[0] = Desc{nullptr, 1};
      ctx.in_adj[1] = Desc{theta_adj, 2};
      ctx.out = Desc{&out, 1};
      ctx.variant = kCategoricalScalarOutcome | kCategoricalArgAutodiff | 0x80u;

      kernel->forward(ctx);
      ctx.out_adj = std::numeric_limits<double>::infinity();
      kernel->backward(ctx);
      check(std::isinf(theta_adj[0]) && theta_adj[0] > 0.0,
            "categorical native infinite selected adjoint");
      check(theta_adj[1] == 0.0 && std::signbit(theta_adj[1]),
            "categorical native unselected signed zero");

      theta[0] = 0.0;
      theta[1] = 1.0;
      theta_adj[0] = 0.0;
      theta_adj[1] = -0.0;
      kernel->forward(ctx);
      check(std::isinf(out) && out < 0.0,
            "categorical native selected zero value");
      ctx.out_adj = 0.0;
      kernel->backward(ctx);
      check(std::isnan(theta_adj[0]),
            "categorical native selected zero topology");
      check(theta_adj[1] == 0.0 && std::signbit(theta_adj[1]),
            "categorical native repeated unselected zero");

      outcome = 2.0;
      theta[0] = 0.2;
      theta[1] = 0.8;
      ctx.in_adj[1] = Desc{nullptr, 2};
      kernel->forward(ctx);
      expect_eq("categorical native null-adjoint value", out, std::log(0.8));
      ctx.out_adj = 1.0;
      kernel->backward(ctx);  // active source type, but no graph adjoint edge
    }
  }

  // Array outcomes add one callback contribution per observation. Preserve
  // that exact accumulation order under a non-unit upstream adjoint; replacing
  // it with count * adjoint changes the selected gradient's low bit. The zero
  // unselected probability also pins Stan Math's disconnected-log topology.
  {
    Graph g;
    const int theta = g.add_slot(2, true);
    const int outcomes = g.add_slot(6, false);
    const int cat = g.add_slot(1, false);
    const int cat_op = g.add_op(OP_CATEGORICAL, {outcomes, theta}, cat);
    g.ops[(size_t)cat_op].variant = kCategoricalArgAutodiff;
    const int scale = g.add_slot(1, false);
    const int total = g.add_slot(1, false);
    g.add_op(OP_MUL, {cat, scale}, total);
    g.result_slot = total;

    Executor ex(std::move(g));
    ex.params_data()[0] = 1.0;
    ex.params_data()[1] = 0.0;
    std::fill(ex.value_ptr(outcomes), ex.value_ptr(outcomes) + 6, 1.0);
    ex.value_ptr(scale)[0] = 0.1;
    double got_grad[2];
    const double got = ex.gradient(got_grad);

    Eigen::Matrix<stan::math::var, -1, 1> ref_theta(2);
    ref_theta << 1.0, 0.0;
    const std::vector<int> ref_outcomes(6, 1);
    stan::math::var ref =
        stan::math::categorical_lpmf<false>(ref_outcomes, ref_theta) * 0.1;
    ref.grad();
    expect_eq("categorical array scaled value", got, ref.val());
    expect_eq("categorical array scaled selected", got_grad[0],
              ref_theta(0).adj());
    check(std::isnan(got_grad[1]) && std::isnan(ref_theta(1).adj()),
          "categorical array scaled unselected topology");
    stan::math::recover_memory();
  }

  // A user density is templated on each actual scalar type. The same
  // unqualified vector formal is double when called with data and var when
  // called with an autodiff local, even when both graph values are constant.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    for (bool propto : {false, true}) {
      for (bool local : {false, true}) {
        for (bool body_local : {false, true}) {
          DataMap d;
          d.set_int("outcome", 2);
          const std::vector<double> arg =
              fn == "categorical_lpmf" ? std::vector<double>{0.2, 0.3, 0.5}
                                       : std::vector<double>{-1.0, 0.0, 1.0};
          d.set_real_array("arg", arg);
          CompiledModel lm = compile_model(
              categorical_udf_mir(fn, propto, local, body_local), d);
          Executor ex(std::move(lm.graph));
          lm.bind(ex);
          ex.params_data()[0] = 0.75;
          double grad = 0.0;
          const double got = ex.gradient(&grad);
          Eigen::VectorXd v(3);
          for (int i = 0; i < 3; ++i) v(i) = arg[(size_t)i];
          const double full =
              fn == "categorical_lpmf"
                  ? stan::math::categorical_lpmf<false>(2, v)
                  : stan::math::categorical_logit_lpmf<false>(2, v);
          const double want = 0.75 + (propto && !local ? 0.0 : full);
          const std::string tag =
              fn + std::string(propto ? " propto " : " normalized ") +
              (local ? "autodiff actual" : "data actual") +
              (body_local ? " through UDF local" : " direct formal");
          expect_ulp(tag + " value", got, want);
          expect_eq(tag + " anchor gradient", grad, 1.0);
        }
      }
    }
  }

  // Speculative constant folding must not move a data-only UDF's Stan Math
  // validation to model construction. On an invalid call it declines the
  // fold, leaving the exact runtime op and its original exception intact.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    DataMap d;
    d.set_int("outcome", 4);
    const std::vector<double> arg = fn == "categorical_lpmf"
                                        ? std::vector<double>{0.2, 0.3, 0.5}
                                        : std::vector<double>{-1.0, 0.0, 1.0};
    d.set_real_array("arg", arg);
    try {
      CompiledModel lm =
          compile_model(categorical_udf_mir(fn, false, false), d);
      check(
          std::any_of(lm.graph.ops.begin(), lm.graph.ops.end(),
                      [](const Op& op) { return op.opcode == OP_CATEGORICAL; }),
          fn + " invalid data UDF retains runtime check");
      Executor ex(std::move(lm.graph));
      lm.bind(ex);
      ex.params_data()[0] = 0.75;
      double grad = 0.0;
      std::string got_message;
      try {
        (void)ex.gradient(&grad);
      } catch (const std::domain_error& e) {
        got_message = e.what();
      }
      Eigen::Map<const Eigen::VectorXd> v(arg.data(), (Eigen::Index)arg.size());
      std::string want_message;
      try {
        if (fn == "categorical_lpmf")
          (void)stan::math::categorical_lpmf<false>(4, v);
        else
          (void)stan::math::categorical_logit_lpmf<false>(4, v);
      } catch (const std::domain_error& e) {
        want_message = e.what();
      }
      check(!got_message.empty(), fn + " invalid data UDF rejects at runtime");
      check(got_message == want_message,
            fn + " invalid data UDF exception message");
    } catch (const std::exception& e) {
      ++failures;
      std::printf("FAIL %s invalid data UDF constructed late check: %s\n",
                  fn.c_str(), e.what());
    }

    try {
      CompiledModel lm = compile_model(categorical_udf_actual_mir(fn), d);
      check(
          std::any_of(lm.graph.ops.begin(), lm.graph.ops.end(),
                      [](const Op& op) { return op.opcode == OP_CATEGORICAL; }),
          fn + " invalid nested UDF actual retains runtime check");
      Executor ex(std::move(lm.graph));
      lm.bind(ex);
      ex.params_data()[0] = 0.75;
      double grad = 0.0;
      std::string got_message;
      try {
        (void)ex.gradient(&grad);
      } catch (const std::domain_error& e) {
        got_message = e.what();
      }
      check(!got_message.empty(),
            fn + " invalid nested UDF actual rejects at runtime");
    } catch (const std::exception& e) {
      ++failures;
      std::printf(
          "FAIL %s invalid nested UDF actual constructed late check: "
          "%s\n",
          fn.c_str(), e.what());
    }
  }

  // A generic UDF's return scalar type is selected from every actual, not
  // just from the expression on its return statement. Here the returned data
  // vector becomes a vector<var> because an otherwise-unused actual is var,
  // so an outer propto categorical call retains its summand.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    DataMap d;
    d.set_int("outcome", 2);
    const std::vector<double> arg = fn == "categorical_lpmf"
                                        ? std::vector<double>{0.2, 0.3, 0.5}
                                        : std::vector<double>{-1.0, 0.0, 1.0};
    d.set_real_array("arg", arg);
    CompiledModel lm = compile_model(categorical_udf_return_mir(fn), d);
    Executor ex(std::move(lm.graph));
    lm.bind(ex);
    ex.params_data()[0] = 0.75;
    double grad = 0.0;
    const double got = ex.gradient(&grad);
    Eigen::Map<const Eigen::VectorXd> v(arg.data(), (Eigen::Index)arg.size());
    const double density =
        fn == "categorical_lpmf"
            ? stan::math::categorical_lpmf<false>(2, v)
            : stan::math::categorical_logit_lpmf<false>(2, v);
    expect_ulp(fn + " UDF instantiated return value", got, 0.75 + density);
    expect_eq(fn + " UDF instantiated return gradient", grad, 1.0);
  }

  // Each generic UDF formal keeps its own scalar type. An unrelated var
  // actual promotes locals and the return, but it must not turn a ternary
  // whose two arms derive from a data-instantiated vector formal into var.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    DataMap d;
    d.set_int("outcome", 2);
    d.set_real_array("arg", fn == "categorical_lpmf"
                                ? std::vector<double>{0.2, 0.3, 0.5}
                                : std::vector<double>{-1.0, 0.0, 1.0});
    CompiledModel lm = compile_model(categorical_udf_ternary_type_mir(fn), d);
    Executor ex(std::move(lm.graph));
    lm.bind(ex);
    ex.params_data()[0] = 0.75;
    double grad = 0.0;
    expect_eq(fn + " per-formal ternary value", ex.gradient(&grad), 0.75);
    expect_eq(fn + " per-formal ternary gradient", grad, 1.0);

    CompiledModel promoted_lm =
        compile_model(categorical_udf_promoted_actual_mir(fn), d);
    Executor promoted_ex(std::move(promoted_lm.graph));
    promoted_lm.bind(promoted_ex);
    promoted_ex.params_data()[0] = 0.75;
    grad = 0.0;
    expect_eq(fn + " promoted int UDF actual value",
              promoted_ex.gradient(&grad), 0.75);
    expect_eq(fn + " promoted int UDF actual gradient", grad, 1.0);
  }

  // A data condition selects one arm at model construction, but the C++ type
  // of a mixed data/autodiff ternary is promoted before that selection. Its
  // propto term is therefore retained even when the data arm wins, directly
  // and when the ternary supplies an unqualified UDF formal.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    for (bool through_udf : {false, true}) {
      for (int flag : {0, 1}) {
        DataMap d;
        d.set_int("flag", flag);
        d.set_int("outcome", 2);
        const std::vector<double> arg =
            fn == "categorical_lpmf" ? std::vector<double>{0.2, 0.3, 0.5}
                                     : std::vector<double>{-1.0, 0.0, 1.0};
        d.set_real_array("arg", arg);
        CompiledModel lm =
            compile_model(categorical_promoted_ternary_mir(fn, through_udf), d);
        Executor ex(std::move(lm.graph));
        lm.bind(ex);
        ex.params_data()[0] = 0.75;
        double grad = 0.0;
        const double got = ex.gradient(&grad);
        Eigen::Map<const Eigen::VectorXd> v(arg.data(),
                                            (Eigen::Index)arg.size());
        const double density =
            fn == "categorical_lpmf"
                ? stan::math::categorical_lpmf<false>(2, v)
                : stan::math::categorical_logit_lpmf<false>(2, v);
        const std::string tag =
            fn + std::string(through_udf ? " nested" : " direct") +
            (flag ? " data arm" : " autodiff arm");
        expect_ulp(tag + " promoted ternary value", got, 0.75 + density);
        expect_eq(tag + " promoted ternary gradient", grad, 1.0);
      }
    }
  }

  // An effectful data-only outcome is a runtime value, not a compile-time
  // immediate. It executes once, then the exact categorical op consumes that
  // value and supplies the same pullback as Stan Math.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    std::string mir = slurp("tests/fixtures/cat.tmir.sexp");
    if (fn == "categorical_logit_lpmf") {
      for (size_t pos = mir.find("categorical_lpmf"); pos != std::string::npos;
           pos = mir.find("categorical_lpmf", pos + fn.size()))
        mir.replace(pos, std::strlen("categorical_lpmf"), fn);
      for (size_t pos = mir.find("(constrain Simplex)");
           pos != std::string::npos;
           pos = mir.find("(constrain Simplex)", pos + 20))
        mir.replace(pos, std::strlen("(constrain Simplex)"),
                    "(constrain Identity)");
    }
    mir = categorical_parameter_effect_mir(std::move(mir), fn);
    DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
    std::optional<CompiledModel> lm;
    {
      stanli_test::StdoutCapture captured;
      lm = compile_model(mir, d);
      check(captured.finish().empty(), fn + " effect absent while compiling");
    }
    Executor ex(std::move(lm->graph));
    lm->bind(ex);

    std::vector<double> point;
    std::vector<double> want_grad;
    double want = 0.0;
    using stan::math::var;
    if (fn == "categorical_lpmf") {
      point = {0.3, -0.8};
      Eigen::Matrix<var, -1, 1> q(2);
      q << point[0], point[1];
      var jac = 0.0;
      const auto theta = stan::math::simplex_constrain(q, jac);
      const std::vector<int> ys = {3, 1, 3};
      var lp = jac + stan::math::categorical_lpmf<false>(2, theta) +
               stan::math::categorical_lpmf<false>(ys, theta);
      lp.grad();
      want = lp.val();
      want_grad = {q(0).adj(), q(1).adj()};
    } else {
      point = {-0.4, 0.2, 0.9};
      Eigen::Matrix<var, -1, 1> beta(3);
      for (int i = 0; i < 3; ++i) beta(i) = point[(size_t)i];
      const std::vector<int> ys = {3, 1, 3};
      var lp = stan::math::categorical_logit_lpmf<false>(2, beta) +
               stan::math::categorical_logit_lpmf<false>(ys, beta);
      lp.grad();
      want = lp.val();
      want_grad = {beta(0).adj(), beta(1).adj(), beta(2).adj()};
    }
    stan::math::recover_memory();
    std::copy(point.begin(), point.end(), ex.params_data());
    for (int run = 0; run < 2; ++run) {
      std::vector<double> grad(point.size());
      stanli_test::StdoutCapture captured;
      expect_eq(fn + " effect value " + std::to_string(run),
                ex.gradient(grad.data()), want);
      check(captured.finish() == "categorical effect\n",
            fn + " outcome effect executes once " + std::to_string(run));
      for (size_t i = 0; i < grad.size(); ++i)
        expect_eq(fn + " effect gradient " + std::to_string(i), grad[i],
                  want_grad[i]);
    }
  }

  // write_array is instantiated on doubles even though its parameter slots
  // vary with q. Normalized calls retain their value; propto calls validate
  // and return zero. The same rule holds when parameter control flow produces
  // the vector, because graph dependency and instantiated scalar type differ.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    for (bool propto : {false, true}) {
      for (bool island : {false, true}) {
        DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
        CompiledModel lm = compile_model(
            categorical_write_array_mir(slurp("tests/fixtures/cat.tmir.sexp"),
                                        fn, propto, island),
            d);
        const std::string tag =
            fn + std::string(propto ? " propto" : " normalized") +
            (island ? " island" : " direct");
        check(lm.write_array && lm.write_array->truncated.empty(),
              tag + " write_array compiles");
        if (!lm.write_array || !lm.write_array->truncated.empty()) continue;
        Executor ex(std::move(lm.write_array->graph));
        lm.write_array->bind(ex);
        ex.params_data()[0] = 0.0;
        ex.params_data()[1] = 0.0;
        ex.run_forward_only();
        bool found = false;
        for (const auto& col : lm.write_array->columns) {
          if (col.name != "categorical_value") continue;
          found = true;
          expect_ulp(tag + " write_array value", ex.value_ptr(col.slot)[0],
                     propto ? 0.0 : -std::log(3.0));
        }
        check(found, tag + " write_array column");
      }
    }
  }

  // A runtime RNG outcome cannot become categorical's compile-time integer
  // payload, so it selects the interpreted write_array for the whole section.
  // Scalar/array and normalized/propto semantics remain intact there.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    for (bool propto : {false, true}) {
      for (int outcome_count : {1, 3}) {
        DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
        const std::string interpreted_mir = categorical_write_array_mir(
            slurp("tests/fixtures/cat.tmir.sexp"), fn, propto, false, false,
            true, outcome_count);
        CompiledModel lm = compile_model(interpreted_mir, d);
        const std::string tag =
            fn + std::string(propto ? " propto" : " normalized") +
            (outcome_count == 1 ? " scalar" : " array");
        check(lm.write_array && lm.write_array->interp,
              tag + " interpreted write_array selected");
        if (!lm.write_array || !lm.write_array->interp) continue;
        Executor ex(std::move(lm.graph));
        lm.bind(ex);
        std::fill(ex.params_data(), ex.params_data() + ex.n_params(), 0.0);
        ex.run_forward_only();
        WaRng rng(1234);
        const std::vector<double> row =
            lm.write_array->interp->eval(lm.constrained_env(ex), rng);
        bool found = false;
        for (const auto& col : lm.write_array->interp->columns()) {
          if (col.name != "categorical_value") continue;
          found = true;
          Eigen::VectorXd arg(3);
          if (fn == "categorical_lpmf")
            arg.setConstant(1.0 / 3.0);
          else
            arg.setZero();
          const std::vector<int> outcomes((size_t)outcome_count, 1);
          double want = 0.0;
          if (fn == "categorical_lpmf") {
            if (outcome_count == 1)
              want = propto ? stan::math::categorical_lpmf<true>(1, arg)
                            : stan::math::categorical_lpmf<false>(1, arg);
            else
              want = propto
                         ? stan::math::categorical_lpmf<true>(outcomes, arg)
                         : stan::math::categorical_lpmf<false>(outcomes, arg);
          } else if (outcome_count == 1) {
            want = propto ? stan::math::categorical_logit_lpmf<true>(1, arg)
                          : stan::math::categorical_logit_lpmf<false>(1, arg);
          } else {
            want =
                propto
                    ? stan::math::categorical_logit_lpmf<true>(outcomes, arg)
                    : stan::math::categorical_logit_lpmf<false>(outcomes, arg);
          }
          expect_eq(tag + " interpreted value", row.at((size_t)col.slot), want);
        }
        check(found, tag + " interpreted column");

        // The outcome expression is evaluated exactly once. Compare the next
        // draw with a stream on which the same callbacks were invoked once.
        WaRng reference_rng(1234);
        for (int i = 0; i < outcome_count; ++i)
          (void)stan::math::binomial_rng(1, 1.0, reference_rng.gen());
        expect_eq(tag + " interpreted RNG position",
                  stan::math::uniform_rng(0.0, 1.0, rng.gen()),
                  stan::math::uniform_rng(0.0, 1.0, reference_rng.gen()));
      }
    }
  }

  // Propto still validates on the interpreted route. A runtime RNG outcome
  // keeps the section in WaInterp while producing an out-of-range category.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    std::string mir = categorical_write_array_mir(
        slurp("tests/fixtures/cat.tmir.sexp"), fn, true, false, false, true);
    const size_t gq = mir.find("(generate_quantities");
    const size_t density = mir.find("(FunApp (StanLib " + fn, gq);
    const size_t rng = mir.find("(FunApp (StanLib binomial_rng", density);
    const std::string one = "(pattern (Lit Int 1))";
    const size_t trials = mir.find(one, rng);
    check(gq != std::string::npos && density != std::string::npos &&
              rng != std::string::npos && trials != std::string::npos,
          fn + " invalid interpreted mutation found target");
    if (trials != std::string::npos)
      mir.replace(trials, one.size(), "(pattern (Lit Int 4))");
    DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
    CompiledModel lm = compile_model(mir, d);
    check(lm.write_array && lm.write_array->interp,
          fn + " invalid interpreted write_array selected");
    if (!lm.write_array || !lm.write_array->interp) continue;
    Executor ex(std::move(lm.graph));
    lm.bind(ex);
    std::fill(ex.params_data(), ex.params_data() + ex.n_params(), 0.0);
    ex.run_forward_only();
    WaRng rng_state(1234);
    std::string got_message;
    try {
      (void)lm.write_array->interp->eval(lm.constrained_env(ex), rng_state);
    } catch (const std::domain_error& e) {
      got_message = e.what();
    }
    Eigen::VectorXd arg = Eigen::VectorXd::Constant(3, 1.0 / 3.0);
    std::string want_message;
    try {
      if (fn == "categorical_lpmf")
        (void)stan::math::categorical_lpmf<true>(4, arg);
      else
        (void)stan::math::categorical_logit_lpmf<true>(4, arg);
    } catch (const std::domain_error& e) {
      want_message = e.what();
    }
    check(!got_message.empty(), fn + " interpreted propto validates");
    check(got_message == want_message,
          fn + " interpreted propto exception message");
  }

  // Equal flattened widths do not make a scalar the shape owner. With K=1,
  // scalar-left multiplication must retain the vector's logical geometry in
  // the interpreter before the categorical runtime check.
  for (const std::string& fn : {"categorical_lpmf", "categorical_logit_lpmf"}) {
    std::string mir = categorical_write_array_mir(
        slurp("tests/fixtures/cat.tmir.sexp"), fn, false, false, false, true);
    const size_t gq = mir.find("(generate_quantities");
    const size_t density = mir.find("(FunApp (StanLib " + fn, gq);
    const std::string vector_arg =
        "((pattern (Var theta))\n"
        "             (meta ((type_ UVector) (adlevel DataOnly)))))";
    const size_t arg = mir.find(vector_arg, density);
    check(gq != std::string::npos && density != std::string::npos &&
              arg != std::string::npos,
          fn + " scalar-left vector mutation found target");
    if (arg != std::string::npos) {
      const std::string scaled_arg = R"(((pattern
             (FunApp (StanLib Times__ FnPlain AoS)
              (((pattern (Lit Real 1.0))
                (meta ((type_ UReal) (adlevel DataOnly))))
               ((pattern (Var theta))
                (meta ((type_ UVector) (adlevel DataOnly)))))))
            (meta ((type_ UVector) (adlevel DataOnly))))))";
      mir.replace(arg, vector_arg.size(), scaled_arg);
    }
    DataMap d = DataMap::from_json(R"({"K":1,"y":1,"ys":[1,1,1]})");
    CompiledModel lm = compile_model(mir, d);
    check(lm.write_array && lm.write_array->interp,
          fn + " scalar-left interpreted write_array selected");
    if (!lm.write_array || !lm.write_array->interp) continue;
    Executor ex(std::move(lm.graph));
    lm.bind(ex);
    ex.run_forward_only();
    WaRng rng(1234);
    const std::vector<double> row =
        lm.write_array->interp->eval(lm.constrained_env(ex), rng);
    bool found = false;
    for (const auto& col : lm.write_array->interp->columns()) {
      if (col.name != "categorical_value") continue;
      found = true;
      expect_eq(fn + " scalar-left vector value", row.at((size_t)col.slot),
                0.0);
    }
    check(found, fn + " scalar-left vector column");
  }

  // A shaped zero-width operand also owns the broadcast geometry. An
  // unsupported gamma RNG keeps this interpreter-specific oracle on WaInterp;
  // Stan's empty-outcome logit overload returns zero without indexing either
  // the outcomes or the empty vector.
  {
    const std::string fn = "categorical_logit_lpmf";
    std::string mir =
        categorical_write_array_mir(slurp("tests/fixtures/cat.tmir.sexp"), fn,
                                    false, false, false, true, 0);
    const size_t gq = mir.find("(generate_quantities");
    const size_t density = mir.find("(FunApp (StanLib " + fn, gq);
    const std::string vector_arg =
        "((pattern (Var theta))\n"
        "             (meta ((type_ UVector) (adlevel DataOnly)))))";
    const size_t arg = mir.find(vector_arg, density);
    check(gq != std::string::npos && density != std::string::npos &&
              arg != std::string::npos,
          "empty scalar-left vector mutation found target");
    if (arg != std::string::npos) {
      const std::string scaled_empty = R"(((pattern
             (FunApp (StanLib Times__ FnPlain AoS)
              (((pattern
                 (FunApp (StanLib gamma_rng FnRng AoS)
                  (((pattern (Lit Real 1.0))
                    (meta ((type_ UReal) (adlevel DataOnly))))
                   ((pattern (Lit Real 1.0))
                    (meta ((type_ UReal) (adlevel DataOnly)))))))
                (meta ((type_ UReal) (adlevel DataOnly))))
               ((pattern
                 (FunApp (StanLib head FnPlain AoS)
                  (((pattern (Var theta))
                    (meta ((type_ UVector) (adlevel DataOnly))))
                   ((pattern (Lit Int 0))
                    (meta ((type_ UInt) (adlevel DataOnly)))))))
                (meta ((type_ UVector) (adlevel DataOnly)))))))
            (meta ((type_ UVector) (adlevel DataOnly))))))";
      mir.replace(arg, vector_arg.size(), scaled_empty);
    }
    DataMap d = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
    CompiledModel lm = compile_model(mir, d);
    check(lm.write_array && lm.write_array->interp,
          "empty scalar-left interpreted write_array selected");
    if (lm.write_array && lm.write_array->interp) {
      Executor ex(std::move(lm.graph));
      lm.bind(ex);
      ex.run_forward_only();
      WaRng rng(1234);
      const std::vector<double> row =
          lm.write_array->interp->eval(lm.constrained_env(ex), rng);
      bool found = false;
      for (const auto& col : lm.write_array->interp->columns()) {
        if (col.name != "categorical_value") continue;
        found = true;
        expect_eq("empty scalar-left vector value", row.at((size_t)col.slot),
                  0.0);
      }
      check(found, "empty scalar-left vector column");
    }
  }

  // rep_matrix on parameters (row-vector across rows, scalar fill) with
  // to_vector flattening.
  {
    DataMap d;
    d.set_int("R", 2);
    CompiledModel lm = compile_model(slurp("tests/fixtures/repm.tmir.sexp"), d);
    check(lm.n_unconstrained == 4, "repm 4 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[4] = {0.4, -0.2, 0.9, 0.1};
    for (int i = 0; i < 4; ++i) lex.params_data()[i] = q[i];
    double grad[4];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> a(3);
    a << q[0], q[1], q[2];
    var s = q[3];
    // L[i, j] = a[j]; col-major flat: column j is R copies of a[j].
    Eigen::Matrix<var, -1, 1> Lf(6), Sf(6);
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 2; ++i) {
        Lf(j * 2 + i) = a(j);
        Sf(j * 2 + i) = s;
      }
    var t1 = stan::math::normal_lpdf<false>(Lf, 0.0, 1.0);
    var t2 = stan::math::normal_lpdf<false>(Sf, 0.0, 2.0);
    var acc = (t1 + t2);
    acc.grad();
    expect_eq("repm lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("repm ga" + std::to_string(i), grad[i], a(i).adj());
    expect_eq("repm gs", grad[3], s.adj());
    stan::math::recover_memory();
  }

  // append_row stacking parameter matrices: col-major storage interleaves
  // columns, so the lowering concatenates then gathers into place.
  {
    DataMap d;
    d.set_int("C", 2);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/aprow.tmir.sexp"), d);
    check(lm.n_unconstrained == 8, "aprow 8 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[8] = {0.5, -0.4, 1.1, 0.2, -0.9, 0.7, 0.3, -0.6};
    for (int i = 0; i < 8; ++i) lex.params_data()[i] = q[i];
    double grad[8];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    // A is 2xC col-major (q0..q3), B is 1xC (q4,q5), r is C (q6,q7).
    Eigen::Matrix<var, -1, 1> p(8);
    for (int i = 0; i < 8; ++i) p(i) = q[i];
    auto A = [&](int i, int j) { return p(j * 2 + i); };  // 2 rows
    auto B = [&](int j) { return p(4 + j); };
    auto r = [&](int j) { return p(6 + j); };
    Eigen::Matrix<var, -1, 1> S(6), T(6);
    for (int j = 0; j < 2; ++j) {
      S(j * 3 + 0) = A(0, j);
      S(j * 3 + 1) = A(1, j);
      S(j * 3 + 2) = B(j);
      T(j * 3 + 0) = r(j);
      T(j * 3 + 1) = A(0, j);
      T(j * 3 + 2) = A(1, j);
    }
    var acc = stan::math::normal_lpdf<false>(S, 0.0, 1.0) +
              stan::math::normal_lpdf<false>(T, 0.0, 2.0);
    acc.grad();
    expect_eq("aprow lp", lp, acc.val());
    for (int i = 0; i < 8; ++i)
      expect_eq("aprow g" + std::to_string(i), grad[i], p(i).adj());
    stan::math::recover_memory();
  }

  // reverse on both sides of the data boundary: transformed data reverses a
  // literal, an integer array and an array of vectors in the interpreter,
  // while the log density reverses a parameter vector, row-vector and array
  // of vectors as gathers in the graph. Reversing twice is the identity, so
  // every term pairs a reversed parameter against data reversed once, and
  // the array cases pin the axis: `reverse` flips only the outer dimension,
  // and each element vector must survive with its own order intact. The two
  // sides disagree on storage -- DataMap is first-index-fast, graph arrays
  // are outer-major -- so the same source function needs two lowerings.
  {
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/reverse.tmir.sexp"), DataMap());
    check(lm.n_unconstrained == 10, "reverse 10 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[10] = {0.5, -1.2, 0.8, 0.3, 1.4, -0.7, 0.9, -0.2, 0.6, 1.1};
    for (int i = 0; i < 10; ++i) lex.params_data()[i] = q[i];
    double grad[10];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> p(10);
    for (int i = 0; i < 10; ++i) p(i) = q[i];
    Eigen::Matrix<var, -1, 1> x = p.head(3);
    Eigen::Matrix<var, 1, -1> rx = p.segment(3, 3).transpose();
    // ax is array[2] vector[2]; the writer lists whole elements in order.
    std::vector<Eigen::Matrix<var, -1, 1>> ax(2, Eigen::Matrix<var, -1, 1>(2));
    for (int n = 0; n < 2; ++n)
      for (int i = 0; i < 2; ++i) ax[n](i) = p(6 + n * 2 + i);

    Eigen::VectorXd literal(3);
    literal << 1.0, 2.0, 3.0;
    // Eigen's reverse is lazy, so the transformed-data value needs its own
    // destination; reversing into `literal` would alias and self-overwrite.
    const Eigen::VectorXd td = stan::math::reverse(literal);
    const std::vector<int> ti = stan::math::reverse(std::vector<int>{1, 2, 3});
    Eigen::VectorXd ta0(2), ta1(2);
    ta0 << 1.0, 2.0;
    ta1 << 3.0, 4.0;
    const std::vector<Eigen::VectorXd> ta =
        stan::math::reverse(std::vector<Eigen::VectorXd>{ta0, ta1});
    const std::vector<Eigen::Matrix<var, -1, 1>> rax = stan::math::reverse(ax);

    var acc = stan::math::dot_product(stan::math::reverse(x), td) +
              stan::math::reverse(rx) * td;
    for (int n = 0; n < 2; ++n)
      acc += ti[n] * stan::math::dot_product(rax[n], ta[n]);
    acc.grad();
    expect_eq("reverse lp", lp, acc.val());
    for (int i = 0; i < 10; ++i)
      expect_eq("reverse g" + std::to_string(i), grad[i], p(i).adj());
    stan::math::recover_memory();
  }

  // The linspaced_* family is data-only, so transformed data folds it to
  // constants that the log density then multiplies parameters by. Pin all
  // four overloads against stan-math rather than against transcribed
  // literals: the integer spacing inherits Eigen's rule that `high` drops
  // until the range divides evenly (K=5 over [2,3] repeats, and K=1 yields
  // `high`, not `low`), which is exactly what open-coding gets wrong.
  {
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/linspaced.tmir.sexp"), DataMap());
    check(lm.n_unconstrained == 7, "linspaced 7 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[7] = {0.4, -1.1, 0.7, 0.2, -0.5, 1.3, 0.9};
    for (int i = 0; i < 7; ++i) lex.params_data()[i] = q[i];
    double grad[7];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> p(7);
    for (int i = 0; i < 7; ++i) p(i) = q[i];
    const Eigen::Matrix<var, -1, 1> x = p.head(4);
    const Eigen::Matrix<var, -1, 1> y = p.tail(3);

    const std::vector<int> ia = stan::math::linspaced_int_array(4, 1, 7);
    const std::vector<int> ir = stan::math::linspaced_int_array(5, 2, 3);
    const std::vector<int> io = stan::math::linspaced_int_array(1, 3, 9);
    const std::vector<double> ra = stan::math::linspaced_array(3, -1.5, 2.5);
    const Eigen::VectorXd v = stan::math::linspaced_vector(4, 0.0, 1.0);
    const Eigen::RowVectorXd rv = stan::math::linspaced_row_vector(3, 2.0, 8.0);

    var acc = stan::math::dot_product(x, v) + (rv * y);
    for (int n = 0; n < 4; ++n) acc += ia[n] * x(n);
    for (int n = 0; n < 3; ++n) acc += ra[n] * y(n);
    for (int n = 0; n < 5; ++n) acc += ir[n];
    acc += io[0];
    acc.grad();
    expect_eq("linspaced lp", lp, acc.val());
    for (int i = 0; i < 7; ++i)
      expect_eq("linspaced g" + std::to_string(i), grad[i], p(i).adj());
    stan::math::recover_memory();
  }

  // block() on both sides of the data boundary. A 2-D window is the case
  // sub_col's single slice cannot express: each result column is
  // contiguous, but consecutive columns sit M.rows apart, so the lowering
  // gathers. The two windows overlap in the source and differ in shape, so
  // a transposed or row-major offset would still fill the right number of
  // slots while reading the wrong ones.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/blockfn.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/blockfn.tmir.sexp"), d);
    check(lm.n_unconstrained == 20, "blockfn 20 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    double q[20];
    for (int k = 0; k < 20; ++k) q[k] = 0.1 * (k + 1) - 0.7;
    for (int k = 0; k < 20; ++k) lex.params_data()[k] = q[k];
    double grad[20];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> flat(20);
    for (int k = 0; k < 20; ++k) flat(k) = q[k];
    // Parameters arrive col-major, matching the matrix's storage order.
    Eigen::Matrix<var, -1, -1> p(4, 5);
    for (int c = 0; c < 5; ++c)
      for (int rr = 0; rr < 4; ++rr) p(rr, c) = flat(c * 4 + rr);
    Eigen::MatrixXd m(4, 5);
    for (int rr = 0; rr < 4; ++rr)
      for (int c = 0; c < 5; ++c) m(rr, c) = rr * 5 + c + 1;

    const Eigen::MatrixXd td = stan::math::block(m, 2, 3, 2, 3);
    var acc = stan::math::sum(stan::math::elt_multiply(
                  stan::math::block(p, 2, 3, 2, 3), td)) +
              stan::math::sum(stan::math::block(p, 1, 4, 3, 2));
    acc.grad();
    expect_eq("blockfn lp", lp, acc.val());
    for (int k = 0; k < 20; ++k)
      expect_eq("blockfn g" + std::to_string(k), grad[k], flat(k).adj());
    stan::math::recover_memory();
  }

  // to_matrix: transformed data reshapes a data vector and converts a
  // 2-D array in the interpreter,
  // while the log density reshapes a parameter vector in the graph. The
  // 3 x 2 vs 2 x 3 shapes and the a * c product would all still typecheck
  // under a wrong (row-major) fill, so the reference pins the ordering.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/tomatrix.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tomatrix.tmir.sexp"), d);
    check(lm.n_unconstrained == 6, "tomatrix 6 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    double q[6];
    for (int k = 0; k < 6; ++k) q[k] = 0.2 * (k + 1) - 0.5;
    for (int k = 0; k < 6; ++k) lex.params_data()[k] = q[k];
    double grad[6];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> x(6);
    for (int k = 0; k < 6; ++k) x(k) = q[k];
    Eigen::VectorXd v(6);
    v << 1, 2, 3, 4, 5, 6;
    Eigen::MatrixXd ar(2, 3);
    ar << 10, 20, 30, 40, 50, 60;
    const Eigen::MatrixXd a = stan::math::to_matrix(v, 2, 3);
    const Eigen::MatrixXd b = ar;
    const Eigen::RowVectorXd w = stan::math::to_row_vector(a);
    Eigen::Matrix<var, -1, -1> c = stan::math::to_matrix(x, 3, 2);
    var acc = stan::math::sum(stan::math::multiply(a, c)) + stan::math::sum(b) +
              stan::math::dot_product(x, stan::math::to_vector(b)) +
              stan::math::sum(w) +
              stan::math::dot_product(x, stan::math::to_vector(a));
    acc.grad();
    expect_eq("tomatrix lp", lp, acc.val());
    for (int k = 0; k < 6; ++k)
      expect_eq("tomatrix g" + std::to_string(k), grad[k], x(k).adj());
    stan::math::recover_memory();
  }

  // Compare acceptance against Stan Math independently of the flat gather
  // offsets. In particular, a row overrun may still fit in the source slot.
  {
    const std::string text = slurp("tests/fixtures/block_bounds.tmir.sexp");
    const Eigen::MatrixXd reference = Eigen::MatrixXd::Ones(2, 3);
    for (int row : {0, 1, 2, 3})
      for (int col : {0, 1, 3, 4})
        for (int nr : {0, 1, 2, 3})
          for (int nc : {0, 1, 2}) {
            bool expected = true, accepted = true;
            try {
              const Eigen::MatrixXd block =
                  stan::math::block(reference, row, col, nr, nc);
              (void)block;
            } catch (const std::exception&) {
              expected = false;
            }
            DataMap data;
            data.set_int("row", row);
            data.set_int("col", col);
            data.set_int("nr", nr);
            data.set_int("nc", nc);
            try {
              CompiledModel cm = compile_model(text, data);
              Executor ex(std::move(cm.graph));
              cm.bind(ex);
              std::fill_n(ex.params_data(), 6, 1.0);
              double grad[6];
              const double lp = ex.gradient(grad);
              if (expected) {
                expect_eq("block boundary lp", lp, nr * nc);
                for (int c = 0; c < 3; ++c)
                  for (int r = 0; r < 2; ++r)
                    expect_eq("block boundary gradient", grad[c * 2 + r],
                              r >= row - 1 && r < row - 1 + nr &&
                                      c >= col - 1 && c < col - 1 + nc
                                  ? 1.0
                                  : 0.0);
              }
            } catch (const std::exception&) {
              accepted = false;
            }
            check(accepted == expected,
                  "block graph acceptance " + std::to_string(row) + "," +
                      std::to_string(col) + "," + std::to_string(nr) + "," +
                      std::to_string(nc));
          }
  }

  // Nonuniform coefficients distinguish each matrix lane. Container-valued
  // replication/reversal must preserve the inner axes in both engines;
  // the fixture's generated quantities also join the cross-path suite.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/container_shapes.tmir.sexp"), DataMap{});
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const double weights[6] = {1, 4, 3.5, 5, 3, 8};
    for (double scale : {-0.25, 0.0, 0.5}) {
      double want = 0;
      for (int k = 0; k < 6; ++k) {
        ex.params_data()[k] = scale * (k + 1);
        want += weights[k] * ex.params_data()[k];
      }
      double grad[6];
      expect_eq("container shapes lp", ex.gradient(grad), want);
      for (int k = 0; k < 6; ++k)
        expect_eq("container shapes gradient", grad[k], weights[k]);
    }
  }

  // zeros_*/ones_* constructors: data-sized, so transformed data folds the
  // integer array and identity matrix in the interpreter while the log
  // density broadcasts a ones/zeros vector in the graph. The constant terms
  // (s = 0 + N + 0 + N) and the zeros row-vector contribute nothing, so a
  // wrong fill would show up as a nonzero lp or gradient.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/zeros_ones.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/zeros_ones.tmir.sexp"), d);
    check(lm.n_unconstrained == 3, "zeros_ones 3 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[3] = {0.4, -1.1, 0.7};
    for (int k = 0; k < 3; ++k) lex.params_data()[k] = q[k];
    double grad[3];
    const double lp = lex.gradient(grad);

    // s = sum(zeros) + sum(ones_row) + zeros_int[0] + trace(I) = 0+3+0+3.
    const double want_lp = (q[0] + q[1] + q[2]) + 6.0;
    expect_eq("zeros_ones lp", lp, want_lp);
    for (int k = 0; k < 3; ++k)
      expect_eq("zeros_ones g" + std::to_string(k), grad[k], 1.0);
    stan::math::recover_memory();
  }

  // csr_extract_v / csr_extract_u: the integer companions to
  // csr_extract_w, folded in the interpreter over a row-major sparse view.
  // A = [[10,0,20,0],[0,30,0,0],[0,0,40,50]] has w=[10,20,30,40,50],
  // v=[1,3,2,3,4] (1-indexed column ids), u=[1,3,4,6] (1-indexed row
  // starts, length rows+1). The log density only reads w, so the gradient
  // pins w's order and the constant lp pins the two index arrays.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/csrextract.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/csrextract.tmir.sexp"), d);
    check(lm.n_unconstrained == 5, "csrextract 5 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[5] = {0.4, -1.1, 0.7, 0.2, -0.5};
    for (int k = 0; k < 5; ++k) lex.params_data()[k] = q[k];
    double grad[5];
    const double lp = lex.gradient(grad);

    const double w[5] = {10, 20, 30, 40, 50};
    double dot = 0.0;
    for (int k = 0; k < 5; ++k) dot += q[k] * w[k];
    // usum = 1+3+4+6 = 14, vsum = 1+3+2+3+4 = 13.
    expect_eq("csrextract lp", lp, dot + 27.0);
    for (int k = 0; k < 5; ++k)
      expect_eq("csrextract g" + std::to_string(k), grad[k], w[k]);
    stan::math::recover_memory();
  }

  // rep_array tiling a parameter vector and a scalar. The element keeps its
  // shape; rep_array prepends the outer axes. Graph arrays are outer-major
  // so the lowering tiles the element buffer, while the interpreter (first-
  // index-fast) strides it -- the cross_path fixture pins that the two
  // agree. Here: lp = 3 * sum(a) + 2 * s, so grad a = {3,3,3}, grad s = 2.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/reparray.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/reparray.tmir.sexp"), d);
    check(lm.n_unconstrained == 4, "reparray 4 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[4] = {0.4, -1.1, 0.7, 0.9};
    for (int k = 0; k < 4; ++k) lex.params_data()[k] = q[k];
    double grad[4];
    const double lp = lex.gradient(grad);

    expect_eq("reparray lp", lp, 3.0 * (q[0] + q[1] + q[2]) + 2.0 * q[3]);
    expect_eq("reparray ga0", grad[0], 3.0);
    expect_eq("reparray ga1", grad[1], 3.0);
    expect_eq("reparray ga2", grad[2], 3.0);
    expect_eq("reparray gs", grad[3], 2.0);
    stan::math::recover_memory();
  }

  // .^ (EltPow__): the elementwise-power operator was missing from the
  // binary-op table even though scalar Pow__ and the interpreter already
  // had it. Both operand orders exercise the base and exponent gradients.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/eltpow.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/eltpow.tmir.sexp"), d);
    check(lm.n_unconstrained == 3, "eltpow 3 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[3] = {0.6, 1.4, 0.9};
    for (int k = 0; k < 3; ++k) lex.params_data()[k] = q[k];
    double grad[3];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> x(3);
    for (int k = 0; k < 3; ++k) x(k) = q[k];
    Eigen::VectorXd base(3);
    base << 2.0, 3.0, 1.5;
    var acc = stan::math::sum(stan::math::pow(x, base)) +
              stan::math::sum(stan::math::pow(base, x));
    acc.grad();
    expect_eq("eltpow lp", lp, acc.val());
    for (int k = 0; k < 3; ++k)
      expect_eq("eltpow g" + std::to_string(k), grad[k], x(k).adj());
    stan::math::recover_memory();
  }

  // gp_exp_quad_cov with a parameter x: the lowering used to refuse a
  // non-data first argument. gp_cov_bwd now rebuilds the points from the
  // promoted input, so x's adjoints propagate. Check every gradient
  // (x, alpha, rho) against the var path with the same constrains.
  {
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/gpcov.tmir.sexp"), DataMap());
    check(lm.n_unconstrained == 5, "gpcov 5 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[5] = {0.3, -0.7, 1.1, -0.2, 0.4};
    for (int k = 0; k < 5; ++k) lex.params_data()[k] = q[k];
    double grad[5];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> qv(5);
    for (int k = 0; k < 5; ++k) qv(k) = q[k];
    var jac = 0.0;
    std::vector<var> x{qv(0), qv(1), qv(2)};
    var alpha = stan::math::lb_constrain<true>(qv(3), 0.0, jac);
    var rho = stan::math::lb_constrain<true>(qv(4), 0.0, jac);
    Eigen::Matrix<var, -1, -1> K = stan::math::gp_exp_quad_cov(x, alpha, rho);
    var acc = stan::math::sum(K) + K(0, 1) * K(1, 2) + jac;
    acc.grad();
    expect_eq("gpcov lp", lp, acc.val());
    for (int k = 0; k < 5; ++k)
      expect_eq("gpcov g" + std::to_string(k), grad[k], qv(k).adj());
    stan::math::recover_memory();
  }

  // The Matern and exponential covariances over array[N] vector[D]
  // coordinates, with active coordinates, scale and length scale.
  {
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/gpmatern.tmir.sexp"), DataMap());
    check(lm.n_unconstrained == 8, "gpmatern 8 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[8] = {0.3, -0.7, 1.1, -0.2, 0.4, 0.9, -0.15, 0.25};
    for (int k = 0; k < 8; ++k) lex.params_data()[k] = q[k];
    double grad[8];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> qv(8);
    for (int k = 0; k < 8; ++k) qv(k) = q[k];
    var jac = 0.0;
    std::vector<Eigen::Matrix<var, -1, 1>> pts(3, Eigen::Matrix<var, -1, 1>(2));
    for (int n = 0; n < 3; ++n)
      for (int d = 0; d < 2; ++d) pts[n](d) = qv(n * 2 + d);
    var alpha = stan::math::lb_constrain<true>(qv(6), 0.0, jac);
    var rho = stan::math::lb_constrain<true>(qv(7), 0.0, jac);
    Eigen::Matrix<var, -1, -1> a = stan::math::gp_matern32_cov(pts, alpha, rho);
    Eigen::Matrix<var, -1, -1> b = stan::math::gp_matern52_cov(pts, alpha, rho);
    Eigen::Matrix<var, -1, -1> c =
        stan::math::gp_exponential_cov(pts, alpha, rho);
    var acc = stan::math::sum(a) + 2 * stan::math::sum(b) +
              3 * stan::math::sum(c) + a(0, 1) * b(1, 2) * c(0, 2) + jac;
    acc.grad();
    expect_ulp("gpmatern lp", lp, acc.val());
    for (int k = 0; k < 8; ++k)
      expect_ulp("gpmatern g" + std::to_string(k), grad[k], qv(k).adj());
    stan::math::recover_memory();
  }

  // is_inf / is_nan on data-time scalars and the nullary math constants
  // (negative_infinity / positive_infinity / not_a_number, which stanc
  // will not fold) all reduce to integer constants: the finite reads give
  // 0, and 1.0/0.0, 0.0/0.0, the infinities, the NaN and eps > 0 give 1,
  // so the normal mean is a + b + c + d + e + f + g + h = 6.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/isinf.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/isinf.tmir.sexp"), d);
    check(lm.n_unconstrained == 1, "isinf 1 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.6;
    double grad[1];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var theta = 0.6;
    var acc = stan::math::normal_lpdf<true>(theta, 6.0, 1.0);
    acc.grad();
    expect_eq("isinf lp", lp, acc.val());
    expect_eq("isinf g0", grad[0], theta.adj());
    stan::math::recover_memory();
  }

  // normal_id_glm_lpdf with a parameter design matrix X: the lowering used
  // to require a data X. The kernel now promotes X to var and scatters its
  // adjoints from the scratch section between y and alpha. Check every
  // gradient (X, beta, alpha, sigma) against the var path.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/glmparamx.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/glmparamx.tmir.sexp"), d);
    check(lm.n_unconstrained == 12, "glmparamx 12 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    double q[12];
    for (int k = 0; k < 12; ++k) q[k] = 0.15 * (k + 1) - 0.8;
    for (int k = 0; k < 12; ++k) lex.params_data()[k] = q[k];
    double grad[12];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> p(12);
    for (int k = 0; k < 12; ++k) p(k) = q[k];
    // Layout: X (4x2 col-major), beta (2), alpha, sigma>0.
    Eigen::Matrix<var, -1, -1> X(4, 2);
    for (int c = 0; c < 2; ++c)
      for (int r = 0; r < 4; ++r) X(r, c) = p(c * 4 + r);
    Eigen::Matrix<var, -1, 1> beta(2);
    beta << p(8), p(9);
    var alpha = p(10);
    var jac = 0.0;
    var sigma = stan::math::lb_constrain<true>(p(11), 0.0, jac);
    Eigen::VectorXd y(4);
    y << 0.5, -1.0, 2.0, 0.3;
    var acc =
        stan::math::normal_id_glm_lpdf<false>(y, X, alpha, beta, sigma) + jac;
    acc.grad();
    expect_eq("glmparamx lp", lp, acc.val());
    for (int k = 0; k < 12; ++k)
      expect_eq("glmparamx g" + std::to_string(k), grad[k], p(k).adj());
    stan::math::recover_memory();
  }

  // beta[:, 1, 1]: an outer array range kept in full, with fixed row/column
  // indices into every element's matrix (array[2] matrix[2,2]). The fixture
  // binds the slice to an array local before summing it, so its logical array
  // view is checked along with the value and every gradient against the var
  // path.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/idxarray3d.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/idxarray3d.tmir.sexp"), d);
    check(lm.n_unconstrained == 8, "idxarray3d 8 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    double q[8];
    for (int k = 0; k < 8; ++k) q[k] = 0.1 * (k + 1) - 0.4;
    for (int k = 0; k < 8; ++k) lex.params_data()[k] = q[k];
    double grad[8];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    // Column-major within each matrix, array-major overall. Filled by
    // element rather than Eigen's comma initializer, which always reads
    // row-major regardless of the matrix's own storage order.
    Eigen::Matrix<var, -1, -1> b1(2, 2), b2(2, 2);
    for (int j = 0; j < 2; ++j)
      for (int i = 0; i < 2; ++i) {
        b1(i, j) = q[j * 2 + i];
        b2(i, j) = q[4 + j * 2 + i];
      }
    var acc =
        stan::math::normal_lpdf<true>(stan::math::to_vector(b1), 0.0, 1.0) +
        stan::math::normal_lpdf<true>(stan::math::to_vector(b2), 0.0, 1.0) +
        b1(0, 0) + b2(0, 0);
    acc.grad();
    expect_eq("idxarray3d lp", lp, acc.val());
    Eigen::Matrix<var, -1, -1> both[2] = {b1, b2};
    for (int e = 0, k = 0; e < 2; ++e)
      for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i, ++k)
          expect_eq("idxarray3d g" + std::to_string(k), grad[k],
                    both[e](i, j).adj());
    stan::math::recover_memory();
  }

  // state_probs[1, 1, 1:2]: a full array-index prefix pins one row_vector
  // leaf element (array[2, 2] row_vector[3]), then a trailing range reads
  // inside it. Check the gathered sum and every gradient against the var
  // path.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/idxrowvec.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/idxrowvec.tmir.sexp"), d);
    check(lm.n_unconstrained == 12, "idxrowvec 12 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    double q[12];
    for (int k = 0; k < 12; ++k) q[k] = 0.1 * (k + 1) - 0.6;
    for (int k = 0; k < 12; ++k) lex.params_data()[k] = q[k];
    double grad[12];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    // Array-major (row, then column), each row_vector[3] contiguous.
    Eigen::Matrix<var, 1, -1> sp[2][2];
    for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 2; ++j) {
        Eigen::Matrix<var, 1, -1> v(3);
        const int base = (i * 2 + j) * 3;
        v << q[base], q[base + 1], q[base + 2];
        sp[i][j] = v;
      }
    var acc = sp[0][0](0) + sp[0][0](1);
    for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 2; ++j)
        acc += stan::math::normal_lpdf<true>(sp[i][j], 0.0, 1.0);
    acc.grad();
    expect_eq("idxrowvec lp", lp, acc.val());
    for (int i = 0, k = 0; i < 2; ++i)
      for (int j = 0; j < 2; ++j)
        for (int c = 0; c < 3; ++c, ++k)
          expect_eq("idxrowvec g" + std::to_string(k), grad[k],
                    sp[i][j](c).adj());
    stan::math::recover_memory();
  }

  // H[1, :, :] = seed * theta: an explicit `:` for every leaf dimension
  // after a full array-index prefix, on array[2] matrix[2, 2] H. Checks lp
  // and the gradient against the var path built from the same statements
  // (H[1] = theta*I, H[2] = I, both normal(0,1) on to_vector, plus
  // theta ~ normal(0,1)).
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/idxassign.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/idxassign.tmir.sexp"), d);
    check(lm.n_unconstrained == 1, "idxassign 1 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = -0.6;
    double grad[1];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var theta = -0.6;
    Eigen::Matrix<var, -1, -1> seed =
        Eigen::Matrix<double, -1, -1>::Identity(2, 2);
    Eigen::Matrix<var, -1, -1> h1 = seed * theta, h2 = seed;
    var acc =
        stan::math::normal_lpdf<true>(stan::math::to_vector(h1), 0.0, 1.0) +
        stan::math::normal_lpdf<true>(stan::math::to_vector(h2), 0.0, 1.0) +
        stan::math::normal_lpdf<true>(theta, 0.0, 1.0);
    acc.grad();
    expect_eq("idxassign lp", lp, acc.val());
    expect_eq("idxassign g0", grad[0], theta.adj());
    stan::math::recover_memory();
  }

  // m[1, :] = r: an explicit `:` for the one remaining dimension after a
  // single-index prefix, on a plain flat array[2, 3] real -- no container
  // leaf at all, unlike idxassign above. Covers
  // unsupported_inline_ode_index_assignment's UDF-body write shape.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/rowassign2d.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/rowassign2d.tmir.sexp"), d);
    check(lm.n_unconstrained == 3, "rowassign2d 3 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    double q[3] = {0.3, -0.7, 1.1};
    for (int k = 0; k < 3; ++k) lex.params_data()[k] = q[k];
    double grad[3];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> r(3);
    for (int k = 0; k < 3; ++k) r(k) = q[k];
    var acc = r(0) + r(1) + r(2) + stan::math::normal_lpdf<true>(r, 0.0, 1.0);
    acc.grad();
    expect_eq("rowassign2d lp", lp, acc.val());
    for (int k = 0; k < 3; ++k)
      expect_eq("rowassign2d g" + std::to_string(k), grad[k], r(k).adj());
    stan::math::recover_memory();
  }

  // unsupported_undeclared_inline_solve, verbatim (see the fixture comment):
  // a triply-nested inlined UDF chain whose innermost while loop has a
  // compile-time-dead early-return branch (`if (N == 0) return 0;`, N is
  // always >= 1 here). The graph lowering folds that branch away, so the
  // while loop's carved runtime-control region has to write its own
  // inlined return-temp with no live-in binding to import. `y` never
  // reaches target, so lp is exactly normal(0,1) on theta regardless of
  // whether the ODE math inside is right; this checks that it still
  // compiles and grades correctly.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/inlinesolve.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/inlinesolve.tmir.sexp"), d);
    check(lm.n_unconstrained == 1, "inlinesolve 1 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = -1.664154007900521;
    double grad[1];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var theta = -1.664154007900521;
    var acc = stan::math::normal_lpdf<true>(theta, 0.0, 1.0);
    acc.grad();
    expect_eq("inlinesolve lp", lp, acc.val());
    expect_eq("inlinesolve g0", grad[0], theta.adj());
    stan::math::recover_memory();
  }

  // reject() with a literal-only message, inside a UDF's data-dependent
  // branch (unsupported_FnReject, verbatim). z >= 0 never takes the reject
  // arm, so lp is exactly normal(0, 1) on z; the model's effective support
  // is z >= 0 (checked() rejects the rest), which is what makes it proper.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/rejectguard.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/rejectguard.tmir.sexp"), d);
    check(lm.n_unconstrained == 1, "rejectguard 1 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.7;
    double grad[1];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var z = 0.7;
    var acc = stan::math::normal_lpdf<false>(z, 0.0, 1.0);
    acc.grad();
    expect_eq("rejectguard lp", lp, acc.val());
    expect_eq("rejectguard g0", grad[0], z.adj());
    stan::math::recover_memory();
  }
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/rejectguard.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/rejectguard.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = -0.115;
    double grad[1];
    bool threw = false;
    try {
      lex.gradient(grad);
    } catch (const std::exception&) {
      threw = true;
    }
    check(threw, "rejectguard throws for negative z");
  }

  // min/max of a contiguous `v[lo:hi]` range slice of a parameter vector or
  // row-vector (unsupported_minmax_expression, generalized): the classifier
  // used to accept only a bare vector variable for the native OP_EXTREMA_VEC
  // opcode, so a sliced argument fell all the way back to WaInterp, which has
  // no log_prob path at all.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/sliceminmax.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/sliceminmax.tmir.sexp"), d);
    check(count_opcode(lm, OP_EXTREMA_VEC) == 4,
          "sliceminmax extrema opcode census");
    check(lm.n_unconstrained == 8, "sliceminmax 8 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[] = {0.4, -0.2, 0.9, 0.1, -0.5, 0.3, 0.8, -0.7};
    std::copy(q, q + 8, lex.params_data());
    double grad[8];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> x(4);
    Eigen::Matrix<var, 1, -1> r(4);
    for (int i = 0; i < 4; ++i) x(i) = q[i];
    for (int i = 0; i < 4; ++i) r(i) = q[4 + i];
    var vspan =
        stan::math::max(x.segment(0, 3)) - stan::math::min(x.segment(0, 3));
    var rspan =
        stan::math::max(r.segment(1, 3)) - stan::math::min(r.segment(1, 3));
    var acc = stan::math::std_normal_lpdf<true>(x) +
              stan::math::std_normal_lpdf<true>(r) +
              stan::math::normal_lpdf<false>(vspan, 0.0, 1.0) +
              stan::math::normal_lpdf<false>(rspan, 0.0, 1.0);
    acc.grad();
    expect_eq("sliceminmax lp", lp, acc.val());
    for (int i = 0; i < 4; ++i)
      expect_eq("sliceminmax gx" + std::to_string(i), grad[i], x(i).adj());
    for (int i = 0; i < 4; ++i)
      expect_eq("sliceminmax gr" + std::to_string(i), grad[4 + i], r(i).adj());
    stan::math::recover_memory();
  }

  // prod of a contiguous `v[lo:hi]` range slice of a parameter vector or
  // row-vector (unsupported_prod, generalized): same gap as the extrema
  // classifier above, in the surface/grouping classifiers `prod` lowering
  // uses instead.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/sliceprod.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/sliceprod.tmir.sexp"), d);
    check(count_opcode(lm, OP_PROD_VEC) == 2, "sliceprod opcode census");
    for (const Op& op : lm.graph.ops)
      if (op.opcode == OP_PROD_VEC)
        check(op.variant == 2, "sliceprod packet grouping, active");
    check(lm.n_unconstrained == 8, "sliceprod 8 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[] = {0.4, -0.2, 0.9, 0.1, -0.5, 0.3, 0.8, -0.7};
    std::copy(q, q + 8, lex.params_data());
    double grad[8];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> x(4);
    Eigen::Matrix<var, 1, -1> r(4);
    for (int i = 0; i < 4; ++i) x(i) = q[i];
    for (int i = 0; i < 4; ++i) r(i) = q[4 + i];
    var vprod = stan::math::prod(x.segment(0, 3));
    var rprod = stan::math::prod(r.segment(1, 3));
    var acc = stan::math::std_normal_lpdf<true>(x) +
              stan::math::std_normal_lpdf<true>(r) +
              stan::math::normal_lpdf<false>(vprod, 0.0, 1.0) +
              stan::math::normal_lpdf<false>(rprod, 0.0, 1.0);
    acc.grad();
    expect_eq("sliceprod lp", lp, acc.val());
    for (int i = 0; i < 4; ++i)
      expect_eq("sliceprod gx" + std::to_string(i), grad[i], x(i).adj());
    for (int i = 0; i < 4; ++i)
      expect_eq("sliceprod gr" + std::to_string(i), grad[4 + i], r(i).adj());
    stan::math::recover_memory();
  }

  // A container-valued normal_lpdf inside a data-dependent while loop
  // (unsupported_normal_lpdf_container, verbatim): the loop forces the call
  // through the register machine's runtime-control region compiler, whose
  // DENSITY opcode only ever bound one scalar per argument. DENSITY_VEC adds
  // the container form, calling stan-math's own vectorized normal_lpdf over
  // an Eigen::Map per container argument -- the same call CmdStan's
  // generated code makes -- rather than summing N scalar calls by hand,
  // which would round differently. No parameters, so the model has one lp:
  // N identical calls (the loop body does not touch y/mu/sigma), matched
  // against N stan::math::normal_lpdf<false> calls over the same data.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/densityvec.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/densityvec.tmir.sexp"), d);
    check(lm.n_unconstrained == 0, "densityvec 0 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    double grad[1];
    const double lp = lex.gradient(grad);

    const double ky[] = {0.5, -1.2, 2.0};
    const double kmu[] = {0.1, -0.3, 1.5};
    const double ksigma[] = {1.0, 0.7, 2.2};
    Eigen::Map<const Eigen::VectorXd> y(ky, 3);
    Eigen::Map<const Eigen::RowVectorXd> mu(kmu, 3);
    Eigen::Map<const Eigen::RowVectorXd> sigma(ksigma, 3);
    const double reference = 3 * stan::math::normal_lpdf<false>(y, mu, sigma);
    expect_eq("densityvec lp", lp, reference);
  }

  // The same, with a parameter feeding one container argument (`mu`, via
  // `rep_row_vector`): DENSITY_VEC has to run correctly as var replay too,
  // not just reproduce a data-only lp.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/densityvecgrad.json"));
    test_setenv("STANLI_STRUCTURED_LOOPS", "0", 1);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/densityvecgrad.tmir.sexp"), d);
    test_unsetenv("STANLI_STRUCTURED_LOOPS");
    check(count_opcode(lm, OP_ISLAND) >= 1, "densityvecgrad has an island");
    check(lm.n_unconstrained == 1, "densityvecgrad 1 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.6;
    double grad[1];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    const double ky[] = {0.5, -1.2, 2.0};
    const double ksigma[] = {1.0, 0.7, 2.2};
    Eigen::Map<const Eigen::VectorXd> y(ky, 3);
    Eigen::Map<const Eigen::RowVectorXd> sigma(ksigma, 3);
    var a = 0.6;
    Eigen::Matrix<var, 1, -1> mu = Eigen::Matrix<var, 1, -1>::Constant(3, a);
    var acc = 3 * stan::math::normal_lpdf<false>(y, mu, sigma) +
              stan::math::std_normal_lpdf<true>(a);
    acc.grad();
    expect_eq("densityvecgrad lp", lp, acc.val());
    expect_eq("densityvecgrad g0", grad[0], a.adj());
    stan::math::recover_memory();
  }

  // A threshold count taken with num_elements of an expression, the shape
  // brms's category-specific ordinal families write, against the same model
  // with the expression bound to a local first.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/csthres.json"));
    CompiledModel expression =
        compile_model(slurp("tests/fixtures/csthres.tmir.sexp"), d);
    CompiledModel bound =
        compile_model(slurp("tests/fixtures/csthresbound.tmir.sexp"), d);
    check(expression.n_unconstrained == 12, "csthres 12 unconstrained");
    check(bound.n_unconstrained == 12, "csthresbound 12 unconstrained");
    Executor ex(std::move(expression.graph)), bx(std::move(bound.graph));
    expression.bind(ex);
    bound.bind(bx);
    double point[12];
    for (int i = 0; i < 12; ++i) point[i] = 0.3 * i - 1.1;
    std::copy(point, point + 12, ex.params_data());
    std::copy(point, point + 12, bx.params_data());
    double ga[12], gb[12];
    expect_eq("csthres lp", ex.gradient(ga), bx.gradient(gb));
    for (int i = 0; i < 12; ++i)
      expect_eq("csthres g" + std::to_string(i), ga[i], gb[i]);
  }

  // multiply_lower_tri_self_transpose on a matrix parameter whose upper
  // triangle is not zero (unsupported_multiply_lower_tri_self_transpose).
  // The graph used to spell it TRANSPOSE + GEMM, which is A * A' and reads
  // the entries stan-math drops; a dedicated opcode calls the same overload
  // CmdStan does, so both the value and the triangular pullback match.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/mltmask.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/mltmask.tmir.sexp"), d);
    check(count_opcode(lm, OP_MULT_LOWER_TRI_SELF_TRANSPOSE) == 1,
          "mltmask opcode census");
    check(count_opcode(lm, OP_GEMM) == 0, "mltmask does not reach GEMM");
    check(lm.n_unconstrained == 9, "mltmask 9 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    // Column-major, with a deliberately large upper triangle: A * A' would
    // differ from the answer in the first entry alone by 5^2 + 9^2.
    const double a[9] = {1.0, 0.4, -0.2, 5.0, 2.0, 0.7, 9.0, 8.0, 3.0};
    std::copy(a, a + 9, lex.params_data());
    double grad[9];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, -1> A(3, 3);
    for (int c = 0, k = 0; c < 3; ++c)
      for (int r = 0; r < 3; ++r, ++k) A(r, c) = a[k];
    auto P = stan::math::multiply_lower_tri_self_transpose(A);
    var acc = P(0, 0) - P(1, 2) + P(2, 1) +
              stan::math::std_normal_lpdf<true>(
                  Eigen::Matrix<var, -1, 1>(stan::math::to_vector(A)));
    acc.grad();
    expect_eq("mltmask lp", lp, acc.val());
    for (int c = 0, k = 0; c < 3; ++c)
      for (int r = 0; r < 3; ++r, ++k)
        expect_eq("mltmask g" + std::to_string(k), grad[k], A(r, c).adj());
    stan::math::recover_memory();
  }

  // The same call inside a while loop: the region compiles to the register
  // machine, where one MULT_LOWER_TRI_SELF_TRANSPOSE instruction re-executed
  // under var rebuilds stan-math's own tape. Expanding it into scalar MULs
  // the way crossprod does would read the dropped triangle and accumulate
  // the input adjoints in tape order rather than through the pullback.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/mltgrad.json"));
    test_setenv("STANLI_STRUCTURED_LOOPS", "0", 1);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/mltgrad.tmir.sexp"), d);
    test_unsetenv("STANLI_STRUCTURED_LOOPS");
    check(count_opcode(lm, OP_ISLAND) >= 1, "mltgrad has an island");
    check(lm.n_unconstrained == 9, "mltgrad 9 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double a[9] = {1.0, 0.4, -0.2, 5.0, 2.0, 0.7, 9.0, 8.0, 3.0};
    std::copy(a, a + 9, lex.params_data());
    double grad[9];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, -1> A(3, 3);
    for (int c = 0, k = 0; c < 3; ++c)
      for (int r = 0; r < 3; ++r, ++k) A(r, c) = a[k];
    var acc = 0.0;
    // The instruction sits in the loop body, so the product is formed once
    // per iteration; one call scaled by two would be a different tape.
    for (int it = 0; it < 2; ++it) {
      auto P = stan::math::multiply_lower_tri_self_transpose(A);
      acc += P(0, 0) - P(1, 2) + P(2, 1);
    }
    acc += stan::math::std_normal_lpdf<true>(
        Eigen::Matrix<var, -1, 1>(stan::math::to_vector(A)));
    acc.grad();
    expect_eq("mltgrad lp", lp, acc.val());
    for (int c = 0, k = 0; c < 3; ++c)
      for (int r = 0; r < 3; ++r, ++k)
        expect_eq("mltgrad g" + std::to_string(k), grad[k], A(r, c).adj());
    stan::math::recover_memory();
  }

  // A parameter sized by a transformed-data for-loop accumulator
  // (sumnt2 += nts[i] * nts[i]) rather than a bare data value: the
  // transformed-data interpreter used to lose the accumulator's int-ness on
  // the first whole-variable reassignment, so its later use as a size
  // expression looked like an unresolvable runtime value. nots=3,
  // nts=[1,2,3] makes sumnt2 = 1+4+9 = 14.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/tdintsize.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tdintsize.tmir.sexp"), d);
    check(lm.n_unconstrained == 14, "tdintsize 14 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    for (int k = 0; k < 14; ++k) lex.params_data()[k] = 0.1 * (k + 1) - 0.7;
    double grad[14];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> x(14);
    for (int k = 0; k < 14; ++k) x(k) = 0.1 * (k + 1) - 0.7;
    var acc = stan::math::normal_lpdf<true>(x, 0.0, 1.0);
    acc.grad();
    expect_eq("tdintsize lp", lp, acc.val());
    for (int k = 0; k < 14; ++k)
      expect_eq("tdintsize g" + std::to_string(k), grad[k], x(k).adj());
    stan::math::recover_memory();
  }

  // profile("name") { ... } wraps ordinary statements purely for stanc's own
  // timing output; the reader unwraps it to a plain block, so this should
  // compile and grade exactly as if the wrapper were absent.
  {
    DataMap d = DataMap::from_json(slurp("tests/fixtures/profile.json"));
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/profile.tmir.sexp"), d);
    check(lm.n_unconstrained == 1, "profile 1 unconstrained");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    lex.params_data()[0] = 0.4;
    double grad[1];
    const double lp = lex.gradient(grad);

    using stan::math::var;
    var x = 0.4;
    var acc = stan::math::normal_lpdf<false>(x, 0.0, 1.0) +
              stan::math::normal_lpdf<false>(0.7, x, 1.0);
    acc.grad();
    expect_eq("profile lp", lp, acc.val());
    expect_eq("profile g0", grad[0], x.adj());
    stan::math::recover_memory();
  }

  // Simplex + dirichlet: gradient vs the var path (simplex_constrain and
  // dirichlet_lpdf composed exactly as the lowering emits them).
  {
    DataMap d;
    d.set_int("K", 3);
    CompiledModel sm = compile_model(slurp("tests/fixtures/simp.tmir.sexp"), d);
    check(sm.n_unconstrained == 2, "simplex K-1 unconstrained");
    Executor sex(std::move(sm.graph));
    sm.bind(sex);
    sex.params_data()[0] = 0.3;
    sex.params_data()[1] = -0.8;
    double sg[2], slp = sex.gradient(sg);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> y(2);
    y(0) = 0.3;
    y(1) = -0.8;
    var jac = 0.0;
    auto theta = stan::math::simplex_constrain(y, jac);
    Eigen::VectorXd alpha(3);
    for (int i = 0; i < 3; ++i) alpha(i) = 2.0;
    var lp = stan::math::dirichlet_lpdf<true>(theta, alpha) + jac;
    lp.grad();
    expect_eq("simplex lp", slp, lp.val());
    expect_ulp("simplex g0", sg[0], y(0).adj());
    expect_ulp("simplex g1", sg[1], y(1).adj());
    stan::math::recover_memory();
  }

  // cholesky_factor_corr[K] lowers to K*(K-1)/2 unconstrained parameters.
  // The unsupported-transform error path is covered by the
  // unsupported-function check below.
  {
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/chol.tmir.sexp"), [] {
          DataMap d;
          d.set_int("K", 3);
          return d;
        }());
    check(cm.n_unconstrained == 3, "cholesky_corr unconstrained size is 3");
  }
  // Control flow on a parameter: an `if` and a ternary whose conditions
  // depend on mu and sigma cannot pick an arm at load time, so lowering
  // compiles each into a necessity island. The gradient must be the one
  // the taken arm implies -- the reference below branches on the same
  // values, which is exactly what CmdStan's generated C++ does.
  //
  // Both arms of both constructs are covered: case 0 takes the if's THEN
  // and the ternary's THEN, case 1 takes both ELSEs.
  {
    // An optimization pass may be switched off; this is not one. Without
    // the island the model does not compile at all, so the disable flag
    // must not reach it.
    test_setenv("STANLI_NO_ISLAND", "1", 1);
    DataMap d;
    d.set_real("y", 1.75);
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/paramcond.tmir.sexp"), d);
    int islands = 0;
    for (const Op& op : lm.graph.ops)
      if (op.opcode == OP_ISLAND) ++islands;
    check(islands == 3, "paramcond: one island per parameter condition");

    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double pts[2][2] = {{0.3, -0.2}, {-0.4, 0.5}};
    for (int c = 0; c < 2; ++c) {
      lex.params_data()[0] = pts[c][0];
      lex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      var u_mu = pts[c][0], u_sig = pts[c][1];
      var sigma = stan::math::exp(u_sig);
      var acc = u_sig;  // lower=0 jacobian
      var m = stan::math::value_of(u_mu) > 0 ? u_mu * 2.0 : -u_mu;
      var w = stan::math::value_of(u_mu) > 0 ? 1.0 + sigma : 3.0 * sigma;
      var s = stan::math::value_of(sigma) < 1 ? 1.0 / sigma : sigma;
      // `~` lowers propto, so the reference drops the same constant.
      acc += stan::math::normal_lpdf<true>(1.75, m, s * w);
      // The target-increment branch: propto OFF, as written.
      acc += stan::math::value_of(u_mu) > stan::math::value_of(sigma)
                 ? stan::math::normal_lpdf<false>(1.75, u_mu, 1.0)
                 : stan::math::normal_lpdf<false>(1.75, sigma, 1.0);
      acc.grad();
      const std::string tag = "paramcond" + std::to_string(c);
      // lp sums three target terms; the reference accumulates them in one
      // var chain, so the last bit is an ordering artifact. Gradients are
      // exact.
      expect_ulp(tag + " lp", lp, acc.val());
      expect_eq(tag + " dmu", grad[0], u_mu.adj());
      expect_eq(tag + " dsigma", grad[1], u_sig.adj());
      stan::math::recover_memory();
    }
    test_unsetenv("STANLI_NO_ISLAND");
  }

  // A shape-only guard on a parameter matrix is fixed when the data is
  // bound, despite the matrix values remaining autodiffable.  It must fold
  // before the UDF's synthetic early-return loop is lowered: the resulting
  // design matrix is parameter-free (as normal_id_glm requires) and there is
  // no runtime-control island.
  {
    const std::string mir = slurp("tests/fixtures/shape_named_guard.tmir.sexp");
    for (int n : {0, 3}) {
      DataMap d;
      d.set_int("n", n);
      d.set_real_array("y", {5.0});
      CompiledModel cm = compile_without_optional_islands(mir, d);
      const std::string tag = "named shape guard " + std::to_string(n);
      check(cm.n_unconstrained == 2 * n + 2, tag + " parameter count");
      check(count_opcode(cm, OP_ISLAND) == 0, tag + " has no island");

      Executor ex(std::move(cm.graph));
      cm.bind(ex);
      for (int i = 0; i < 2 * n; ++i) ex.params_data()[i] = 0.1 * (i + 1);
      ex.params_data()[2 * n] = 0.5;
      ex.params_data()[2 * n + 1] = 0.25;
      std::vector<double> grad(static_cast<size_t>(2 * n + 2));
      const double lp = ex.gradient(grad.data());
      const double x = n == 0 ? 1.0 : 2.0;
      const double residual = 5.0 - (0.5 + x * 0.25);
      expect_eq(tag + " lp", lp, -0.5 * residual * residual);
      for (int i = 0; i < 2 * n; ++i)
        expect_eq(tag + " matrix grad " + std::to_string(i), grad[i], 0.0);
      expect_eq(tag + " alpha grad", grad[2 * n], residual);
      expect_eq(tag + " beta grad", grad[2 * n + 1], x * residual);
    }
  }

  // The same proof through a matrix view.  Empty and duplicate gather lists
  // determine geometry without materializing the two-dimensional gather,
  // while a reached out-of-bounds selector retains Stan's bind-time error.
  {
    const std::string mir =
        slurp("tests/fixtures/shape_indexed_guard.tmir.sexp");
    const auto run = [&](const std::vector<int>& idx, double scale,
                         const std::string& tag) {
      DataMap d;
      d.set_int("K", static_cast<int>(idx.size()));
      d.set_int_array("idx", idx);
      CompiledModel cm = compile_without_optional_islands(mir, d);
      check(cm.n_unconstrained == 10, tag + " parameter count");
      check(count_opcode(cm, OP_ISLAND) == 0, tag + " has no island");
      Executor ex(std::move(cm.graph));
      cm.bind(ex);
      for (int i = 0; i < 9; ++i) ex.params_data()[i] = 0.1 * (i + 1);
      ex.params_data()[9] = 0.5;
      double grad[10] = {};
      expect_eq(tag + " lp", ex.gradient(grad), 0.5 * scale);
      for (int i = 0; i < 9; ++i)
        expect_eq(tag + " matrix grad " + std::to_string(i), grad[i], 0.0);
      expect_eq(tag + " theta grad", grad[9], scale);
    };
    run({}, 1.0, "indexed shape guard empty");
    run({1}, 1.0, "indexed shape guard row and column");
    run({2, 2}, 2.0, "indexed shape guard duplicate");

    DataMap bad;
    bad.set_int("K", 2);
    bad.set_int_array("idx", {1, 4});
    bool threw = false;
    try {
      (void)compile_without_optional_islands(mir, bad);
    } catch (const CompileError& e) {
      threw = std::string(e.what()).find("out of bounds") != std::string::npos;
    }
    check(threw, "indexed shape guard rejects reached out-of-bounds index");
  }

  // Static-shape specialization is lazy across && and ||.  Both bad gathers
  // below are unreachable through boolean, statement, and expression-ternary
  // control, so they neither become eager bounds errors nor force runtime
  // control.
  {
    DataMap d;
    d.set_int_array("empty", {});
    d.set_int_array("valid", {1});
    d.set_int_array("bad", {3});
    CompiledModel cm = compile_without_optional_islands(
        slurp("tests/fixtures/shape_guard_lazy.tmir.sexp"), d);
    check(cm.n_unconstrained == 5, "lazy shape guard parameter count");
    check(count_opcode(cm, OP_ISLAND) == 0, "lazy shape guard has no island");
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    for (int i = 0; i < 4; ++i) ex.params_data()[i] = 0.1 * (i + 1);
    ex.params_data()[4] = 0.5;
    double grad[5] = {};
    expect_eq("lazy shape guard lp", ex.gradient(grad), 0.5);
    for (int i = 0; i < 4; ++i)
      expect_eq("lazy shape guard matrix grad " + std::to_string(i), grad[i],
                0.0);
    expect_eq("lazy shape guard theta grad", grad[4], 1.0);
  }

  // A partially specialized || has two distinct outcomes.  With an empty
  // matrix the shape lhs is true and a genuine parameter rhs is dead, so no
  // island is needed.  With a nonempty matrix the same rhs remains a runtime
  // condition.  The preceding flag guard also pins the opposite walk: a
  // false shape lhs followed by a true data-only rhs still folds completely.
  {
    const std::string mir =
        slurp("tests/fixtures/shape_partial_guard.tmir.sexp");
    for (int n : {0, 2}) {
      DataMap d;
      d.set_int("n", n);
      d.set_int("flag", 1);
      CompiledModel cm = compile_without_optional_islands(mir, d);
      const std::string tag = "partial shape guard " + std::to_string(n);
      check(cm.n_unconstrained == 2 * n + 1, tag + " parameter count");
      check(count_opcode(cm, OP_ISLAND) == (n == 0 ? 0 : 1),
            tag + " island boundary");
      Executor ex(std::move(cm.graph));
      cm.bind(ex);
      for (int i = 0; i < 2 * n; ++i) ex.params_data()[i] = 0.1 * (i + 1);
      for (double theta : {0.5, -0.5}) {
        ex.params_data()[2 * n] = theta;
        std::vector<double> grad(static_cast<size_t>(2 * n + 1));
        const double scale = n == 0 || theta > 0.0 ? 11.0 : 21.0;
        expect_eq(tag + " lp " + std::to_string(theta),
                  ex.gradient(grad.data()), scale * theta);
        for (int i = 0; i < 2 * n; ++i)
          expect_eq(tag + " matrix grad " + std::to_string(i), grad[i], 0.0);
        expect_eq(tag + " theta grad " + std::to_string(theta), grad[2 * n],
                  scale);
      }
    }
  }

  // A read-only live-in can still be a declared local with no preceding
  // assignment. Ordinary lowering gives it Stan's uninitialized NaN value;
  // the necessity-island binder must materialize the same slot rather than
  // rejecting the name. The negative arm avoids reading it and remains a
  // finite identity function, while the positive arm proves the NaN fill is
  // visible when selected.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_uninitialized.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    ex.params_data()[0] = -0.25;
    const double finite_lp = ex.gradient(&grad);
    expect_eq("uninitialized island finite arm lp", finite_lp, -0.25);
    expect_eq("uninitialized island finite arm grad", grad, 1.0);
    ex.params_data()[0] = 0.25;
    const double nan_lp = ex.gradient(&grad);
    check(std::isnan(nan_lp), "uninitialized island selected arm is NaN");
  }

  // stanc's O1 inliner lowers a UDF return through an early-return flag and
  // Break statements inside a single-iteration loop. Both return paths must
  // leave that synthetic loop without escaping the caller's surrounding
  // control flow.
  for (int first : {0, 1}) {
    DataMap d;
    d.set_int("first", first);
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/early_return.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    ex.params_data()[0] = 0.4;
    double grad = 0.0;
    const double lp = ex.gradient(&grad);
    const double scale = first ? 2.0 : 3.0;
    expect_eq("inlined early return lp " + std::to_string(first), lp,
              scale * 0.4);
    expect_eq("inlined early return grad " + std::to_string(first), grad,
              scale);
  }

  // rows() in a necessity island reads the logical matrix geometry rather
  // than being rejected as an unknown register-machine function.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_rows.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const int64_t n = ex.n_params();
    check(n == 7, "parameter-condition rows parameter count");
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = 0.0;
    double grad[7] = {};
    ex.params_data()[0] = 0.4;
    const double positive_lp = ex.gradient(grad);
    expect_eq("parameter-condition rows positive lp", positive_lp, 0.8);
    expect_eq("parameter-condition rows positive theta grad", grad[0], 2.0);
    for (int i = 1; i < 7; ++i)
      expect_eq("parameter-condition rows matrix grad " + std::to_string(i),
                grad[i], 0.0);
    ex.params_data()[0] = -0.4;
    const double negative_lp = ex.gradient(grad);
    expect_eq("parameter-condition rows negative lp", negative_lp, -0.4);
    expect_eq("parameter-condition rows negative theta grad", grad[0], 1.0);
  }

  // The rest of the shape queries, in the integer positions: a declared
  // extent, an integer local, a loop bound. Each is a constant the region
  // reads off the logical view; before, only a shape query in a
  // real-valued context was answered, and `matrix[rows(m), cols(m)] out;`
  // inside a parameter-dependent region was refused as an unknown integer
  // function.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_shape.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const int64_t n = ex.n_params();
    check(n == 11, "parameter-condition shape parameter count");
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = 0.0;
    double grad[11] = {};
    // num_elements(y) + cols(y) over a 2x3 matrix is 9, and the loop runs
    // size(a) = 4 times over the array.
    ex.params_data()[0] = 0.5;
    for (int i = 7; i < 11; ++i) ex.params_data()[i] = 0.25;
    const double positive_lp = ex.gradient(grad);
    expect_eq("parameter-condition shape positive lp", positive_lp, 5.5);
    expect_eq("parameter-condition shape theta grad", grad[0], 9.0);
    for (int i = 1; i < 7; ++i)
      expect_eq("parameter-condition shape matrix grad " + std::to_string(i),
                grad[i], 0.0);
    for (int i = 7; i < 11; ++i)
      expect_eq("parameter-condition shape array grad " + std::to_string(i),
                grad[i], 1.0);
    ex.params_data()[0] = -0.5;
    const double negative_lp = ex.gradient(grad);
    expect_eq("parameter-condition shape negative lp", negative_lp, -0.5);
    expect_eq("parameter-condition shape negative theta grad", grad[0], 1.0);
    for (int i = 7; i < 11; ++i)
      expect_eq(
          "parameter-condition shape negative array grad " + std::to_string(i),
          grad[i], 0.0);
  }

  // An integer assigned inside a parameter-dependent region. `n = 2` runs
  // before the break in a loop that runs once, so the fold is sound and
  // the lowering takes the value back for `target += n * theta + z`:
  // without that, the statements after the region kept reading the
  // pre-region 1, silently, in both lp and the gradient. `k` is folded
  // within the region, where the reads are the ones the fold belongs to.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_int.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    // The break skips k and z: 2 * theta.
    ex.params_data()[0] = 0.5;
    expect_eq("parameter-condition int break lp", ex.gradient(&grad), 1.0);
    expect_eq("parameter-condition int break grad", grad, 2.0);
    // Without the break, z is k * theta with k folded to 4: 6 * theta.
    ex.params_data()[0] = -0.5;
    expect_eq("parameter-condition int no-break lp", ex.gradient(&grad), -3.0);
    expect_eq("parameter-condition int no-break grad", grad, 6.0);
  }

  // The same integer where the assignment may not run. It becomes a runtime
  // scalar live-out: the taken arm writes 2, while the untaken arm preserves
  // the pre-region 1 for the target statement that follows.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_intbranch.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    ex.params_data()[0] = 0.5;
    expect_eq("conditional integer live-out taken lp", ex.gradient(&grad), 3.5);
    expect_eq("conditional integer live-out taken grad", grad, 7.0);
    ex.params_data()[0] = -0.5;
    expect_eq("conditional integer live-out untaken lp", ex.gradient(&grad),
              -0.5);
    expect_eq("conditional integer live-out untaken grad", grad, 1.0);
  }

  // The comparisons and the logical operators as compile-time integers:
  // the `while` condition decides the trip count, and each integer local
  // is a comparison. Before this the region compiler knew five integer
  // functions -- the four arithmetic ones and unary minus -- so any of
  // these refused the whole region by name.
  {
    DataMap d;
    d.set_int("k", 2);
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/paramcond_intops.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    // The loop stops on `hits < 3` with hits = 3, so the coefficient is
    // 3 + 1 + 1 + 1 + 0.
    ex.params_data()[0] = 0.5;
    expect_eq("parameter-condition int ops lp", ex.gradient(&grad), 3.0);
    expect_eq("parameter-condition int ops grad", grad, 6.0);
    ex.params_data()[0] = -0.5;
    expect_eq("parameter-condition int ops else lp", ex.gradient(&grad), -0.5);
    expect_eq("parameter-condition int ops else grad", grad, 1.0);
  }

  // Integer reads the region cannot answer from its own tables: an
  // element of a data array at rank two, and a literal array's size and
  // elements -- the form stanc's inliner leaves where it substituted an
  // `array[] int` argument. Before this the region compiler could index
  // only a one-dimensional integer it had folded itself, so each of these
  // refused the whole region.
  {
    DataMap d;
    // Column-major, as the JSON reader stores a two-dimensional array.
    d.set_int_array("idx", {1, 2, 3, 5, 7, 5}, {3, 2});
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/paramcond_intarray.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    // Rows 1 and 3 match on the second column, contributing 1 + 3; the
    // literal array contributes 8, row two contributes 2 + 7, and the
    // strided row replacement contributes 9 + 8.
    ex.params_data()[0] = 0.5;
    expect_eq("parameter-condition int array lp", ex.gradient(&grad), 323992.0);
    expect_eq("parameter-condition int array grad", grad, 647984.0);
    ex.params_data()[0] = -0.5;
    expect_eq("parameter-condition int array else lp", ex.gradient(&grad),
              -0.5);
    expect_eq("parameter-condition int array else grad", grad, 1.0);
  }

  // A parameter-dependent region whose live-outs are all zero-width. The
  // dimension table gives the matrices no extent, so the region has
  // nothing to carry out and no island runs; with an extent the same
  // model compiles the ordinary way. Refusing the empty one as a region
  // that "produces nothing" turned a model's own data into a compile
  // error -- that message is for a region that found no live-out at all.
  {
    for (int extent : {0, 2}) {
      DataMap d;
      d.set_int("n", extent);
      CompiledModel cm =
          compile_model(slurp("tests/fixtures/paramcond_empty.tmir.sexp"), d);
      Executor ex(std::move(cm.graph));
      cm.bind(ex);
      double grad = 0.0;
      const std::string tag =
          "parameter-condition empty " + std::to_string(extent) + " ";
      ex.params_data()[0] = 0.5;
      expect_eq(tag + "lp", ex.gradient(&grad), 0.5);
      expect_eq(tag + "grad", grad, 1.0);
      ex.params_data()[0] = -0.5;
      expect_eq(tag + "else lp", ex.gradient(&grad), -0.5);
      expect_eq(tag + "else grad", grad, 1.0);
    }
  }

  // A data-only UDF returns an integer array whose size depends on the
  // values, not only on the argument's declared extent.  The structured
  // while keeps its runtime trip count, while the MIR interpreter supplies
  // the constant array value and shape at the region boundary. Exercise the
  // same result through generated quantities for multiple and empty matches.
  {
    const auto check_case = [&](const std::string& tag, const DataMap& d,
                                const std::vector<int>& selected,
                                double positive_lp, double positive_grad) {
      CompiledModel cm = compile_model(
          slurp("tests/fixtures/runtime_int_array_udf.tmir.sexp"), d);
      Executor ex(cm.graph);
      cm.bind(ex);
      double grad = 0.0;
      ex.params_data()[0] = 0.5;
      expect_eq(tag + " lp", ex.gradient(&grad), positive_lp);
      expect_eq(tag + " grad", grad, positive_grad);
      ex.params_data()[0] = -0.5;
      expect_eq(tag + " no-loop lp", ex.gradient(&grad), -0.5);
      expect_eq(tag + " no-loop grad", grad, 1.0);

      check(cm.write_array && cm.write_array->truncated.empty(),
            tag + " write_array compiled");
      if (!cm.write_array || !cm.write_array->truncated.empty()) return;
      Executor wex(std::move(cm.write_array->graph));
      cm.write_array->bind(wex);
      wex.params_data()[0] = 0.5;
      wex.run_forward_only();
      std::map<std::string, double> got;
      bool found_selected = false;
      for (const auto& column : cm.write_array->columns) {
        if (column.name == "selected") {
          found_selected = true;
          check(column.len == (int64_t)selected.size(),
                tag + " write_array selected extent");
          const double* value = wex.value_ptr(column.slot);
          for (size_t k = 0; k < selected.size(); ++k)
            expect_eq(tag + " write_array selected " + std::to_string(k),
                      value[k], selected[k]);
        } else {
          got[column.name] = wex.value_ptr(column.slot)[0];
        }
      }
      check(found_selected, tag + " write_array has selected");
      const auto expect_column = [&](const std::string& name, double want) {
        const auto it = got.find(name);
        check(it != got.end(), tag + " write_array has " + name);
        if (it != got.end())
          expect_eq(tag + " write_array " + name, it->second, want);
      };
      expect_column("selected_count", selected.size());
      double sum = 0;
      for (int value : selected) sum += value;
      expect_column("selected_sum", sum);
    };

    check_case(
        "runtime int-array UDF",
        DataMap::from_json(slurp("tests/fixtures/runtime_int_array_udf.json")),
        {1, 3}, 3.5, 7.0);
    DataMap multiple;
    multiple.set_int("N", 5);
    multiple.set_int_array("x", {1, 0, 1, 1, 0});
    check_case("runtime int-array UDF multiple", multiple, {1, 3, 4}, 6.0,
               12.0);
    DataMap empty;
    empty.set_int("N", 0);
    empty.set_int_array("x", {});
    check_case("runtime int-array UDF empty", empty, {}, 0.5, 1.0);
  }

  // rep_vector inside a region: a run the compiler fills, at an extent it
  // knows. The repeated parameter is the part worth checking -- the
  // gradient is the broadcast's, every element adding into the one cell
  // the value came from -- and the empty extent has to stay empty.
  {
    DataMap d;
    d.set_int("n", 3);
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/paramcond_repvector.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    // 3 * theta from the repeated parameter, 3 * 0.5 from the constant.
    ex.params_data()[0] = 0.5;
    expect_eq("parameter-condition rep_vector lp", ex.gradient(&grad), 3.0);
    expect_eq("parameter-condition rep_vector grad", grad, 3.0);
    ex.params_data()[0] = -0.5;
    expect_eq("parameter-condition rep_vector else lp", ex.gradient(&grad),
              -0.5);
    expect_eq("parameter-condition rep_vector else grad", grad, 1.0);
  }
  {
    DataMap d;
    d.set_int("n", 0);
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/paramcond_repvector.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    ex.params_data()[0] = 0.5;
    expect_eq("parameter-condition rep_vector empty lp", ex.gradient(&grad),
              0.0);
    expect_eq("parameter-condition rep_vector empty grad", grad, 0.0);
  }

  // log1p_exp in a region, on its own opcode. Both the value and the
  // derivative are the expressions stan-math uses, which is what keeps
  // the region's backward, the var replay and the graph's OP_LOG1P_EXP
  // agreeing to the bit -- so the expectations here are those same calls
  // rather than decimals.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_log1pexp.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    ex.params_data()[0] = 0.5;
    expect_eq("parameter-condition log1p_exp lp", ex.gradient(&grad),
              stan::math::log1p_exp(1.0));
    expect_eq("parameter-condition log1p_exp grad", grad,
              2.0 * stan::math::inv_logit(1.0));
    ex.params_data()[0] = -0.5;
    expect_eq("parameter-condition log1p_exp else lp", ex.gradient(&grad),
              stan::math::log1p_exp(-0.5));
    expect_eq("parameter-condition log1p_exp else grad", grad,
              stan::math::inv_logit(-0.5));
  }

  // A matrix row in a region. Two facts to pin: which elements the row is
  // -- three apart, in column-major storage -- and the order the sum
  // accumulates them, which has to be the ascending one stan-math uses on
  // a strided block. The expectation is built in that order rather than
  // written as a decimal.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_matrixrow.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const int64_t n = ex.n_params();
    check(n == 13, "parameter-condition matrix row parameter count");
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = 0.1 * (double)(i + 1);
    ex.params_data()[12] = 0.5;
    std::vector<double> grad((size_t)n);
    double want = stan::math::square(ex.params_data()[1]);
    for (int j = 1; j < 4; ++j)
      want += stan::math::square(ex.params_data()[1 + 3 * j]);
    expect_eq("parameter-condition matrix row lp", ex.gradient(grad.data()),
              want);
    for (int64_t i = 0; i < 12; ++i)
      expect_eq("parameter-condition matrix row grad " + std::to_string(i),
                grad[(size_t)i], i % 3 == 1 ? 2.0 * ex.params_data()[i] : 0.0);
    expect_eq("parameter-condition matrix row theta grad", grad[12], 0.0);
    // The other arm never reads the row.
    ex.params_data()[12] = -0.5;
    expect_eq("parameter-condition matrix row else lp",
              ex.gradient(grad.data()), -0.5);
    for (int64_t i = 0; i < 12; ++i)
      expect_eq("parameter-condition matrix row else grad " + std::to_string(i),
                grad[(size_t)i], 0.0);
  }

  // Diagonal pre/post multiplication in a runtime region must replay the
  // stan-math operations themselves. Their callbacks reduce all coefficients
  // for one vector element at once; scalarizing the products changes both the
  // accumulation order and when a prior adjoint is added.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_diag_multiply.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    constexpr int kDim = 4;
    constexpr int kMatrix = kDim * kDim;
    constexpr int kLeft = kMatrix;
    constexpr int kRight = kLeft + kDim;
    constexpr int kTheta = kRight + kDim;
    constexpr int kParams = kTheta + 1;
    check(ex.n_params() == kParams,
          "parameter-condition diagonal multiply parameter count");
    std::vector<double> grad(kParams);
    auto check_math = [&](const std::string& tag,
                          const std::vector<double>& values) {
      check(values.size() == kParams, tag + " input size");
      for (int i = 0; i < kParams; ++i) ex.params_data()[i] = values[(size_t)i];
      const double lp = ex.gradient(grad.data());

      using stan::math::var;
      Eigen::Matrix<var, Eigen::Dynamic, Eigen::Dynamic> m(kDim, kDim);
      Eigen::Matrix<var, Eigen::Dynamic, 1> left(kDim), right(kDim);
      for (int j = 0; j < kDim; ++j)
        for (int i = 0; i < kDim; ++i) m(i, j) = values[(size_t)(kDim * j + i)];
      for (int i = 0; i < kDim; ++i) {
        left(i) = values[(size_t)(kLeft + i)];
        right(i) = values[(size_t)(kRight + i)];
      }
      var ref_lp = stan::math::sum(stan::math::diag_pre_multiply(left, m)) +
                   stan::math::sum(stan::math::diag_post_multiply(m, right));
      if (values[kTheta] > 1.0) ref_lp += left(0) + right(0);
      ref_lp.grad();
      expect_ulp(tag + " lp", lp, ref_lp.val());
      for (int j = 0; j < kDim; ++j)
        for (int i = 0; i < kDim; ++i)
          expect_eq(tag + " matrix grad " + std::to_string(kDim * j + i),
                    grad[(size_t)(kDim * j + i)], m(i, j).adj());
      for (int i = 0; i < kDim; ++i) {
        expect_eq(tag + " pre vector grad " + std::to_string(i),
                  grad[(size_t)(kLeft + i)], left(i).adj());
        expect_eq(tag + " post vector grad " + std::to_string(i),
                  grad[(size_t)(kRight + i)], right(i).adj());
      }
      expect_eq(tag + " theta grad", grad[kTheta], 0.0);
      stan::math::recover_memory();
    };

    check_math(
        "parameter-condition diagonal multiply regular",
        {0.2, 0.5, -0.3, 0.7, 1.1,  -0.4, 1.3, -0.8, 0.6, 1.5, -0.2, 0.4, -0.6,
         0.9, 1.2, -0.5, 0.4, -1.1, 0.8,  1.3, -0.7, 0.3, 1.1, -0.2, 0.4});

    // With no prior adjoint, the rowwise callback keeps the trailing unit;
    // reversing four independent product callbacks loses it.
    std::vector<double> pre_cancel(kParams, 0.0);
    pre_cancel[0] = 1e16;
    pre_cancel[4] = -1e16;
    pre_cancel[8] = 1.0;
    pre_cancel[kTheta] = 0.4;
    check_math("parameter-condition diagonal pre cancellation", pre_cancel);

    // The row and column both reduce to zero. With theta > 1, stan-math adds
    // each grouped reduction once to the direct vector term's prior adjoint;
    // independent scalar callbacks instead lose that prior unit.
    std::vector<double> prior_cancel(kParams, 0.0);
    prior_cancel[0] = 1e16;
    prior_cancel[1] = -1e16;
    prior_cancel[4] = -1e16;
    prior_cancel[kTheta] = 2.0;
    check_math("parameter-condition diagonal prior cancellation", prior_cancel);

    // A contiguous four-row column takes Eigen's packet reduction path.
    // This input also distinguishes that grouping from either scalar fold.
    std::vector<double> post_cancel(kParams, 0.0);
    post_cancel[0] = 0x1.0000000000001p+0;
    post_cancel[1] = 0x1p+53;
    post_cancel[2] = -0x1p+53;
    post_cancel[3] = 0x1p-53;
    post_cancel[kTheta] = 0.4;
    check_math("parameter-condition diagonal post packet cancellation",
               post_cancel);

    ex.params_data()[kTheta] = -0.4;
    expect_eq("parameter-condition diagonal multiply else lp",
              ex.gradient(grad.data()), -0.4);
    for (int i = 0; i < kTheta; ++i)
      expect_eq("parameter-condition diagonal multiply else grad " +
                    std::to_string(i),
                grad[(size_t)i], 0.0);
    expect_eq("parameter-condition diagonal multiply else theta grad",
              grad[kTheta], 1.0);
  }

  // diagonal, which no path supported before: the region copies the
  // elements rows + 1 apart, the graph spells the same extraction as a
  // strided slice, and this model uses both -- the region for the sum of
  // squares, the graph for the single element added afterwards. The
  // expectation accumulates in the order the region emits.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_diagonal.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const int64_t n = ex.n_params();
    check(n == 13, "parameter-condition diagonal parameter count");
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = 0.1 * (double)(i + 1);
    ex.params_data()[12] = 0.5;
    std::vector<double> grad((size_t)n);
    // A 3x4 matrix's diagonal is three long and steps four registers.
    double want = stan::math::square(ex.params_data()[0]);
    want += stan::math::square(ex.params_data()[4]);
    want += stan::math::square(ex.params_data()[8]);
    want += ex.params_data()[8];
    expect_eq("parameter-condition diagonal lp", ex.gradient(grad.data()),
              want);
    for (int64_t i = 0; i < 12; ++i) {
      const bool on_diagonal = i % 4 == 0;
      const double from_square = on_diagonal ? 2.0 * ex.params_data()[i] : 0.0;
      expect_eq("parameter-condition diagonal grad " + std::to_string(i),
                grad[(size_t)i], i == 8 ? from_square + 1.0 : from_square);
    }
    expect_eq("parameter-condition diagonal theta grad", grad[12], 0.0);
    // The other arm keeps the graph's element and drops the region's sum.
    ex.params_data()[12] = -0.5;
    expect_eq("parameter-condition diagonal else lp", ex.gradient(grad.data()),
              -0.5 + ex.params_data()[8]);
    for (int64_t i = 0; i < 12; ++i)
      expect_eq("parameter-condition diagonal else grad " + std::to_string(i),
                grad[(size_t)i], i == 8 ? 1.0 : 0.0);
    expect_eq("parameter-condition diagonal else theta grad", grad[12], 1.0);
  }

  // A runtime-selected break targets the surrounding loop, so the loop and
  // conditional must share one necessity island. The positive arm exits
  // before the increment; the negative arm executes all three iterations.
  {
    CompiledModel cm = compile_model(
        slurp("tests/fixtures/paramcond_break.tmir.sexp"), DataMap());
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    double grad = 0.0;
    ex.params_data()[0] = 0.4;
    const double breaking_lp = ex.gradient(&grad);
    expect_eq("parameter-condition break lp", breaking_lp, 0.0);
    expect_eq("parameter-condition break grad", grad, 0.0);
    ex.params_data()[0] = -0.4;
    const double looping_lp = ex.gradient(&grad);
    expect_eq("parameter-condition no-break lp", looping_lp, 3.0 * -0.4);
    expect_eq("parameter-condition no-break grad", grad, 3.0);
  }

  // The scan that decides whether a data-bounded for loop must share a
  // structured region with a runtime-selected break evaluates data-only
  // conditions under each loop-variable binding. In particular, a zero-trip
  // loop must never inspect idx[ri], and a later iteration can be the first
  // one that contains the runtime break.
  {
    const std::string mir =
        slurp("tests/fixtures/runtime_break_data_loop.tmir.sexp");
    DataMap empty;
    empty.set_int("N", 0);
    empty.set_int_array("idx", {});
    CompiledModel em = compile_model(mir, empty);
    Executor eex(std::move(em.graph));
    em.bind(eex);
    eex.params_data()[0] = 0.4;
    double gradient = -1.0;
    expect_eq("zero-trip runtime-break scan lp", eex.gradient(&gradient), 0.0);
    expect_eq("zero-trip runtime-break scan grad", gradient, 0.0);

    DataMap two;
    two.set_int("N", 2);
    two.set_int_array("idx", {0, 1});
    CompiledModel tm = compile_model(mir, two);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    tex.params_data()[0] = 0.4;
    expect_eq("later runtime break lp", tex.gradient(&gradient), 0.4);
    expect_eq("later runtime break grad", gradient, 1.0);
    tex.params_data()[0] = -0.4;
    expect_eq("later runtime no-break lp", tex.gradient(&gradient), -0.8);
    expect_eq("later runtime no-break grad", gradient, 2.0);
  }

  // The same region written with `~`. Runtime density calls now use the
  // graph kernel ABI, including its argument-activity mask, so the propto
  // instantiation drops exactly the constants that generated Stan does.
  {
    DataMap d;
    d.set_real("y", 1.75);
    CompiledModel pm =
        compile_model(slurp("tests/fixtures/paramcond_tilde.tmir.sexp"), d);
    Executor pex(std::move(pm.graph));
    pm.bind(pex);
    double grad = 0.0;
    pex.params_data()[0] = 0.4;
    expect_eq("runtime propto positive lp", pex.gradient(&grad),
              -0.5 * (1.75 - 0.4) * (1.75 - 0.4));
    expect_eq("runtime propto positive grad", grad, 1.75 - 0.4);
    pex.params_data()[0] = -0.4;
    expect_eq("runtime propto negative lp", pex.gradient(&grad),
              -0.5 * (1.75 - 0.4) * (1.75 - 0.4));
    expect_eq("runtime propto negative grad", grad, -(1.75 - 0.4));
  }

  // The same CALL bridge also carries integer groups and structured GLM
  // layout. Check two different descriptors in one parameter-selected region
  // against Stan Math, including the pullback through both calls.
  {
    DataMap d = DataMap::from_json(
        R"({"y": 2, "N": 5, "z": [1, 0], "c": 2, "cats": [1, 3], "X": [[0.4], [-0.7]]})");
    CompiledModel dm = compile_model(
        slurp("tests/fixtures/density_runtime_shared.tmir.sexp"), d);
    Executor dex(std::move(dm.graph));
    dm.bind(dex);
    double grad = 0.0;
    dex.params_data()[0] = -0.3;
    expect_eq("shared runtime density untaken lp", dex.gradient(&grad), 0.0);
    expect_eq("shared runtime density untaken grad", grad, 0.0);

    dex.params_data()[0] = 0.3;
    const double got = dex.gradient(&grad);
    using stan::math::var;
    var theta = 0.3;
    Eigen::MatrixXd X(2, 1);
    X << 0.4, -0.7;
    Eigen::VectorXd beta(1);
    beta << 0.3;
    std::vector<int> z{1, 0};
    Eigen::Matrix<var, Eigen::Dynamic, 1> logits(3);
    logits << theta, 0.0, -theta;
    std::vector<int> cats{1, 3};
    var want =
        stan::math::beta_binomial_lpmf<false>(2, 5, exp(theta), 2.2) +
        stan::math::bernoulli_logit_glm_lpmf<false>(z, X, theta, beta) +
        stan::math::categorical_lpmf<false>(2, stan::math::softmax(logits)) +
        stan::math::categorical_logit_lpmf<false>(cats, logits) +
        stan::math::hypergeometric_lpmf<false>(2, 5, 4, 4) +
        stan::math::discrete_range_lpmf<false>(2, 1, 5);
    want.grad();
    // The kernels and this replay are separately compiled evaluations of the
    // same math; clang and gcc round their sums a ULP apart.
    expect_ulp("shared runtime density lp", got, want.val());
    expect_ulp("shared runtime density grad", grad, theta.adj());
    stan::math::recover_memory();

    // The integer arguments are known while the region is compiled, but its
    // support checks still belong to the selected runtime branch.
    DataMap invalid = DataMap::from_json(
        R"({"y": 5, "N": 5, "z": [1, 0], "c": 2, "cats": [1, 3], "X": [[0.4], [-0.7]]})");
    CompiledModel invalid_model = compile_model(
        slurp("tests/fixtures/density_runtime_shared.tmir.sexp"), invalid);
    Executor invalid_executor(std::move(invalid_model.graph));
    invalid_model.bind(invalid_executor);
    invalid_executor.params_data()[0] = -0.3;
    expect_eq("untaken all-integer validation",
              invalid_executor.gradient(&grad), 0.0);
    invalid_executor.params_data()[0] = 0.3;
    bool runtime_rejected = false;
    try {
      (void)invalid_executor.gradient(&grad);
    } catch (const std::domain_error& error) {
      runtime_rejected =
          std::string(error.what()).find("hypergeometric_lpmf") !=
          std::string::npos;
    }
    check(runtime_rejected, "taken all-integer validation executes");
  }

  // propto through a user-defined density. CmdStan compiles one as a
  // template on propto__ and hands the CALLER's value to the body, so a
  // body written with `_lupdf` normalizes when it was reached through
  // `_lpdf`. Reading the body's own flag made every user density
  // unnormalized: the gradient is unchanged, lp__ is wrong by the
  // constant, and so is any transformed parameter computed that way.
  //
  // Four terms, four rules, and the whole lp in closed form.
  {
    DataMap d;
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/proptoudf.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const double mu = 0.375;
    ex.params_data()[0] = mu;
    std::vector<double> grad(1);
    const double lp = ex.gradient(grad.data());
    const double kLog2Pi = std::log(2.0 * std::acos(-1.0));
    const double want =
        // target += f_lpdf(mu | 1.0)   normalized
        (-0.5 * kLog2Pi - 0.5 * (mu - 1.0) * (mu - 1.0))
        // target += g_lpdf(mu | )      normalized through a second UDF
        + (-0.5 * kLog2Pi - 0.5 * (mu - 0.5) * (mu - 0.5))
        // target += f_lupdf(mu | 2.0)  unnormalized, as asked
        + (-0.5 * (mu - 2.0) * (mu - 2.0))
        // mu ~ f(3.0)                  unnormalized by definition
        + (-0.5 * (mu - 3.0) * (mu - 3.0));
    check(std::fabs(lp - want) < 1e-12, "user density propto: lp " +
                                            std::to_string(lp) + " want " +
                                            std::to_string(want));
    // The gradient is the same either way, which is why lp had to be the
    // test: d/dmu of the four quadratics.
    const double want_g = -(mu - 1.0) - (mu - 0.5) - (mu - 2.0) - (mu - 3.0);
    check(std::fabs(grad[0] - want_g) < 1e-12,
          "user density propto: gradient " + std::to_string(grad[0]));
  }

  // Densities the register machine did not used to speak, inside
  // parameter-dependent branches. The region has to compile to the
  // register machine or not at all, so a density missing from its
  // vocabulary was a compile error for a model the runtime otherwise
  // handles everywhere -- `student_t_lpdf` most sharply, because four
  // arguments did not fit an instruction with three operand fields.
  {
    DataMap d;
    d.set_real("y", 1.75);
    CompiledModel dm =
        compile_model(slurp("tests/fixtures/paramcond_density.tmir.sexp"), d);
    Executor dex(std::move(dm.graph));
    dm.bind(dex);
    // Both arms, so every density is evaluated and differentiated.
    const double pts[2][2] = {{0.35, 0.6}, {-0.2, -0.45}};
    for (int c = 0; c < 2; ++c) {
      dex.params_data()[0] = pts[c][0];
      dex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = dex.gradient(grad);

      using stan::math::var;
      var u_nu = pts[c][0], u_mu = pts[c][1];
      var nu = stan::math::exp(u_nu), mu = u_mu;
      var acc = u_nu;  // lower=0 jacobian
      if (stan::math::value_of(mu) > 0) {
        acc += stan::math::chi_square_lpdf<false>(1.75, nu);
        acc += stan::math::student_t_lpdf<false>(1.75, nu, mu, 1.0);
      } else {
        acc += stan::math::gumbel_lpdf<false>(1.75, mu, 1.0);
        acc += stan::math::rayleigh_lpdf<false>(1.75, nu);
      }
      acc.grad();
      const std::string tag = "paramcond_density" + std::to_string(c);
      expect_eq(tag + " dnu", grad[0], u_nu.adj());
      expect_eq(tag + " dmu", grad[1], u_mu.adj());
      expect_ulp(tag + " lp", lp, acc.val());
      stan::math::recover_memory();
    }
  }

  // Truncation, against an independent var-path reference. CI has no
  // CmdStan, so harnesses/fn_sweep.py -- which checks all 72 distribution
  // functions bitwise -- cannot run there; this is what guards the
  // rewrite and the cdf kernels on every push.
  {
    DataMap d;
    d.set_real("y", 1.75);
    CompiledModel tm =
        compile_model(slurp("tests/fixtures/trunc.tmir.sexp"), d);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    const double pts[2][2] = {{0.6, -0.3}, {-0.25, 0.4}};
    for (int c = 0; c < 2; ++c) {
      tex.params_data()[0] = pts[c][0];
      tex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = tex.gradient(grad);

      using stan::math::var;
      var u_mu = pts[c][0], u_sig = pts[c][1];
      var mu = u_mu, sigma = stan::math::exp(u_sig);
      var acc = u_sig;  // lower=0 jacobian
      // The propto instantiation CmdStan would pick: y is data, the
      // parameters are var. Getting this wrong shows up as a constant.
      acc += stan::math::normal_lpdf<true>(1.75, mu, sigma);
      acc -= stan::math::log_diff_exp(stan::math::normal_lcdf(10.0, mu, sigma),
                                      stan::math::normal_lcdf(0.0, mu, sigma));
      acc += stan::math::normal_lcdf(1.5, mu, sigma);
      acc += stan::math::normal_lccdf(0.5, mu, sigma);
      acc += stan::math::gamma_lcdf(1.2, 2.0, sigma);
      acc.grad();

      // Gradients are held to the bit; lp is not. The truncation rewrite
      // adds its terms in a different order than this reference does, and
      // than CmdStan's lp_accum__ does -- measured at 1 ULP against
      // CmdStan on this model, with every gradient exact. Reassociating a
      // sum is the one difference allowed to survive here.
      check(grad[0] == u_mu.adj() && grad[1] == u_sig.adj(),
            "trunc: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "trunc: lp matches the var path");
    }
  }

  // Vectorized truncation. The fixture above truncates a scalar, which is
  // the shape that kept working while both vectorized forms failed to
  // compile at all: one needs the FnLength compiler-internal, the other a
  // decl stanc3 leaves unsized. Checked against CmdStan at 0 ULP on lp and
  // every gradient when this went in; the var path is what CI can run.
  {
    const std::vector<double> yv = {0.5, 1.75, 2.25};
    DataMap d;
    d.set_int("N", 3);
    d.set_real_array("y", yv);
    CompiledModel tm =
        compile_model(slurp("tests/fixtures/truncvec.tmir.sexp"), d);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    // Declaration order: mu, theta[3], sigma.
    const double pts[2][5] = {{0.6, -0.2, 0.35, 0.1, -0.3},
                              {-0.25, 0.4, -0.15, 0.5, 0.2}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 5; ++i) tex.params_data()[i] = pts[c][i];
      double grad[5] = {0, 0, 0, 0, 0};
      const double lp = tex.gradient(grad);

      using stan::math::var;
      var u_mu = pts[c][0], u_sig = pts[c][4];
      Eigen::Matrix<var, Eigen::Dynamic, 1> theta(3);
      for (int i = 0; i < 3; ++i) theta(i) = pts[c][1 + i];
      var mu = u_mu, sigma = stan::math::exp(u_sig);
      Eigen::VectorXd y = Eigen::Map<const Eigen::VectorXd>(yv.data(), 3);

      var acc = u_sig;  // lower=0 jacobian
      // Scalar location: one log_diff_exp scaled by the element count.
      acc += stan::math::normal_lpdf<true>(y, mu, sigma);
      acc -= 3.0 *
             stan::math::log_diff_exp(stan::math::normal_lcdf(10.0, mu, sigma),
                                      stan::math::normal_lcdf(0.0, mu, sigma));
      // Container location: one log_diff_exp per element, accumulated.
      acc += stan::math::normal_lpdf<true>(y, theta, 1.0);
      for (int i = 0; i < 3; ++i)
        acc -= stan::math::log_diff_exp(
            stan::math::normal_lcdf(10.0, theta(i), 1.0),
            stan::math::normal_lcdf(0.0, theta(i), 1.0));
      acc.grad();

      bool gok = grad[0] == u_mu.adj() && grad[4] == u_sig.adj();
      for (int i = 0; i < 3; ++i) gok = gok && grad[1 + i] == theta(i).adj();
      check(gok, "truncvec: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "truncvec: lp matches the var path");
    }
  }

  // A scalar int outcome against vectorized real arguments. See
  // tests/fixtures/intbcast.stan: the outcome rides in idata, which had no
  // way to say "this was one int, not an array of one", so the kernel handed
  // stan-math a size-1 container and every such call threw a size-consistency
  // error against the longer real arguments. The reference here is the call
  // CmdStan would instantiate -- a bare int -- so it checks the broadcast
  // itself, not just that the throw stopped.
  {
    const int n = 3;
    DataMap d;
    d.set_int("n", n);
    CompiledModel bm =
        compile_model(slurp("tests/fixtures/intbcast.tmir.sexp"), d);
    Executor bex(std::move(bm.graph));
    bm.bind(bex);
    // Declaration order: theta[2], lambda.
    const double pts[2][3] = {{0.3, -0.45, 0.7}, {-0.6, 0.15, -1.1}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) bex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = bex.gradient(grad);

      using stan::math::var;
      Eigen::Matrix<var, Eigen::Dynamic, 1> th(2);
      th(0) = pts[c][0];
      th(1) = pts[c][1];
      var lam = pts[c][2];
      Eigen::Matrix<var, Eigen::Dynamic, 1> r = stan::math::exp(th);
      Eigen::VectorXd a(2), b(2), cut(3);
      a << 2.0, 3.0;
      b << 1.3, 1.4;
      cut << -1.0, 0.5, 2.0;
      var acc = stan::math::beta_neg_binomial_lpmf<false>(n, r, a, b);
      acc += stan::math::beta_neg_binomial_lcdf(n, r, a, b);
      acc += stan::math::poisson_log_lpmf<false>(n, th);
      // The cutpoints are one whole argument, not lanes: broadcasting the
      // outcome to their length would multiply this term by three.
      acc += stan::math::ordered_logistic_lpmf<false>(2, lam, cut);
      acc.grad();

      // Measured bitwise on arm64/clang at both points, which is what the
      // replication predicts: N copies of the outcome reduce in the same
      // order as one broadcast scalar. The few-ULP window is the headroom
      // the other var-path comparisons in this file carry, for a toolchain
      // that reassociates the shared subexpressions differently.
      const double tol = 8 * 2.220446049250313e-16;
      const double want[3] = {th(0).adj(), th(1).adj(), lam.adj()};
      bool gok = true;
      for (int i = 0; i < 3; ++i)
        gok = gok && std::abs(grad[i] - want[i]) <=
                         tol * std::max(1.0, std::abs(want[i]));
      check(gok, "intbcast: gradients match the scalar-outcome var path");
      check(
          std::abs(lp - acc.val()) <= tol * std::max(1.0, std::abs(acc.val())),
          "intbcast: lp matches the scalar-outcome var path");
    }
  }

  // The cumulative-distribution side of the discrete densities. See
  // tests/fixtures/dcdf.stan: the lpmfs had been listed for a long time
  // and their cdfs had not, so every one of these was an unsupported
  // function. neg_binomial_2_cdf is the one-integer-group shape the
  // generated int cdfs already had; binomial and beta_binomial carry two
  // groups (outcome and trials) and needed the [len, vals...] layout
  // their lpmfs use. The reference is the call CmdStan would instantiate,
  // so the scalar lines check the broadcast rather than just the absence
  // of a throw.
  {
    DataMap d;
    d.set_int("n", 3);
    d.set_int_array("ns", {1, 2, 3});
    d.set_int_array("N", {5, 7, 9});
    CompiledModel bm = compile_model(slurp("tests/fixtures/dcdf.tmir.sexp"), d);
    Executor bex(std::move(bm.graph));
    bm.bind(bex);
    // Both points keep every parameter inside (0, 1), which is a
    // probability for binomial and a positive mean or shape for the other
    // two, so one unconstrained vector serves all three families.
    const double pts[2][3] = {{0.3, 0.45, 0.7}, {0.6, 0.15, 0.9}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) bex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = bex.gradient(grad);

      using stan::math::var;
      Eigen::Matrix<var, Eigen::Dynamic, 1> th(3);
      for (int i = 0; i < 3; ++i) th(i) = pts[c][i];
      Eigen::Matrix<var, Eigen::Dynamic, 1> b = stan::math::exp(th);
      // The integer groups reach the kernels as Eigen vectors of int.
      // The real arguments all reach them as the recording scalar, data
      // or not: the cdfs are tier 0, so there is one all-active
      // instantiation rather than one per activity mask, and the
      // reference has to bind the same way to be the same computation.
      Eigen::VectorXi ns(3), NN(3);
      ns << 1, 2, 3;
      NN << 5, 7, 9;
      Eigen::Matrix<var, Eigen::Dynamic, 1> phi(3), bb(3);
      phi << 2.0, 3.0, 4.0;
      bb << 1.3, 1.4, 1.5;
      const int n = 3;
      var acc = stan::math::neg_binomial_2_cdf(ns, b, phi);
      acc += stan::math::binomial_lcdf(ns, NN, th);
      acc += stan::math::binomial_lccdf(n, 9, th);
      acc += stan::math::binomial_cdf(ns, NN, th);
      acc += stan::math::beta_binomial_lcdf(ns, NN, b, bb);
      acc += stan::math::beta_binomial_lccdf(n, 9, b, bb);
      acc += stan::math::beta_binomial_cdf(ns, NN, b, bb);
      acc.grad();

      // Same activity on both sides, so this is bitwise: no tolerance.
      for (int i = 0; i < 3; ++i)
        expect_eq("dcdf g" + std::to_string(i), grad[i], th(i).adj());
      expect_eq("dcdf lp", lp, acc.val());
    }
  }

  // The five distribution functions the recorder cannot evaluate at all.
  // See tests/fixtures/tcdf.stan: these go to the nested var tape in
  // matrix_fns.cpp, which is the tier ordered_probit and wiener already
  // use, so what this checks past the value is the plumbing -- argument
  // counts, the one integer group, and a length-1 slot entering stan-math
  // as a scalar rather than a one-element vector.
  {
    DataMap d;
    d.set_int("n", 3);
    d.set_int_array("ns", {1, 2, 3});
    CompiledModel tm = compile_model(slurp("tests/fixtures/tcdf.tmir.sexp"), d);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    // Declaration order: y1[3], k1[3], y2[3], k2[3], y3, k3, m4[3], p4[3],
    // m5, m6[3].
    const int kNP = 24;
    const double pts[2][kNP] = {
        {0.30, 0.45, 0.70,  -0.20, 0.15, 0.35, -0.40, 0.25,
         0.60, 0.10, -0.30, 0.50,  0.20, 0.55, -0.10, 0.40,
         0.05, 0.28, -0.12, 0.34,  0.18, 0.22, -0.18, 0.46},
        {-0.25, 0.60,  -0.15, 0.30, -0.45, 0.20, 0.35, -0.05,
         0.50,  -0.35, 0.15,  0.65, -0.20, 0.45, 0.25, -0.30,
         0.55,  -0.08, 0.42,  0.12, -0.22, 0.38, 0.08, -0.28}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < kNP; ++i) tex.params_data()[i] = pts[c][i];
      double grad[kNP] = {0};
      const double lp = tex.gradient(grad);

      using stan::math::var;
      using VecV = Eigen::Matrix<var, Eigen::Dynamic, 1>;
      std::vector<var> th((size_t)kNP);
      for (int i = 0; i < kNP; ++i) th[(size_t)i] = pts[c][i];
      const auto seg = [&](int at) {
        VecV v(3);
        for (int i = 0; i < 3; ++i) v(i) = th[(size_t)(at + i)];
        return v;
      };
      // The tail kernels bind every argument as var, data or not, so the
      // reference binds the literals as var too and this stays bitwise.
      VecV mu3(3);
      mu3 << 0.1, 0.2, 0.3;
      Eigen::VectorXi ns(3);
      ns << 1, 2, 3;
      Eigen::VectorXi n1(1);
      n1 << 3;
      var acc = stan::math::von_mises_cdf(seg(0), var(0.1),
                                          VecV(stan::math::exp(seg(3))));
      acc += stan::math::von_mises_lcdf(seg(6), mu3,
                                        VecV(stan::math::exp(seg(9))));
      acc += stan::math::von_mises_lccdf(th[12], var(0.1),
                                         stan::math::exp(th[13]));
      acc += stan::math::neg_binomial_2_lcdf(ns, VecV(stan::math::exp(seg(14))),
                                             VecV(stan::math::exp(seg(17))));
      acc += stan::math::neg_binomial_2_lccdf(n1, stan::math::exp(th[20]),
                                              var(2.0));
      // The scalar outcome replicated to the lane count, which is what the
      // lowering does before the kernel ever sees the integer group.
      Eigen::VectorXi n3(3);
      n3 << 3, 3, 3;
      acc += stan::math::neg_binomial_2_lccdf(
          n3, VecV(stan::math::exp(seg(21))), var(2.0));
      acc.grad();

      // Same activity on both sides and no parameter shared between two
      // densities, so this is bitwise: no tolerance.
      for (int i = 0; i < kNP; ++i)
        expect_eq("tcdf g" + std::to_string(i), grad[i], th[(size_t)i].adj());
      expect_eq("tcdf lp", lp, acc.val());
    }
  }

  // Two-argument log_sum_exp / log_diff_exp on containers. See
  // tests/fixtures/lsepair.stan: Stan vectorizes both elementwise with
  // scalar broadcast, and the lowering used to emit them at width 1, so
  // every container form died at the assignment that consumed the result.
  {
    // mat is column-major: the JSON reader stores the first index fastest.
    const std::vector<double> matv = {0.5, 1.5, 1.25, 0.25, 2.0, 2.5};
    DataMap d;
    d.set_int_array("counts", {1, 2, 3});
    d.set_real_array("mat", matv, {2, 3});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/lsepair.tmir.sexp"), d);
    check(count_opcode(lm, OP_LSE2) > 0,
          "lsepair: two-argument log_sum_exp uses its direct opcode");
    check(count_opcode(lm, OP_LOG_DIFF_EXP) > 0,
          "lsepair: log_diff_exp uses its direct opcode");
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    // Declaration order: a[3], b.
    const double pts[2][4] = {{0.4, -0.7, 0.25, 0.6},
                              {-0.3, 0.9, -0.15, -0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i) lex.params_data()[i] = pts[c][i];
      double grad[4] = {0, 0, 0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::log_diff_exp;
      using stan::math::log_sum_exp;
      using stan::math::var;
      std::vector<var> a = {pts[c][0], pts[c][1], pts[c][2]};
      var b = pts[c][3];
      std::vector<var> hi = {a[0] + 8, a[1] + 8, a[2] + 8};
      // Each `sum` is one reduction over its own result, so the reference
      // reassociates the same way the graph does and can be held bitwise.
      var acc = 0;
      var s = 0;
      for (int i = 0; i < 3; ++i) s += log_diff_exp(hi[i], a[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += log_diff_exp(hi[i], b);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += log_sum_exp(b, hi[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += log_sum_exp(var(i + 1), b);
      acc += s;
      s = 0;
      for (size_t i = 0; i < matv.size(); ++i)
        s += log_diff_exp(matv[i] + 8, b);
      acc += s;
      // `sum(na[1]) + sum(na[2])` is one target term, so the two element
      // sums join each other before they reach the accumulator.
      var n1 = 0, n2 = 0;
      for (int i = 0; i < 3; ++i) n1 += log_sum_exp(b, hi[i]);
      for (int i = 0; i < 3; ++i) n2 += log_sum_exp(b, a[i]);
      acc += n1 + n2;
      acc.grad();

      bool gok = true;
      for (int i = 0; i < 3; ++i) gok = gok && grad[i] == a[i].adj();
      check(gok, "lsepair: element gradients bitwise against the var path");
      // b is the broadcast operand of six of these ops. mixture.cpp sums
      // an op's N broadcast contributions into a local before touching the
      // adjoint, where the scalar var path folds them in one at a time, so
      // this one lands inside the project's 2 ULP budget rather than on it.
      expect_ulp("lsepair b grad", grad[3], b.adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "lsepair: lp matches the var path");
    }
  }

  // The named spellings of the binary operators, plus squared_distance.
  // See tests/fixtures/opalias.stan: every call is made twice, once on
  // data in transformed data and once on parameters in the model block,
  // and the transformed-data results are summed into the target. That is
  // what makes this test cover both halves -- the graph lowering and the
  // MIR interpreter -- rather than only the one the log density walks.
  // Teaching a function to one of them and not the other is silent: a
  // vectorized log_sum_exp taught only to the lowering reported lp
  // 2.6252 where the answer was 2.9302, with no exception raised.
  {
    const std::vector<double> dvv = {0.4, -0.7};
    const std::vector<double> drvv = {0.25, 0.6};
    // Column-major: dm = [[1.5, 0.5], [-0.25, 2.0]].
    const std::vector<double> dmv = {1.5, -0.25, 0.5, 2.0};
    DataMap d;
    d.set_real_array("dv", dvv, {2});
    d.set_real_array("drv", drvv, {2});
    d.set_real_array("dm", dmv, {2, 2});
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/opalias.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);

    using stan::math::var;
    using MatV = Eigen::Matrix<var, Eigen::Dynamic, Eigen::Dynamic>;
    using VecV = Eigen::Matrix<var, Eigen::Dynamic, 1>;
    Eigen::VectorXd DV(2), DRVc(2);
    Eigen::RowVectorXd DRV(2);
    Eigen::MatrixXd DM(2, 2);
    DV << dvv[0], dvv[1];
    DRV << drvv[0], drvv[1];
    DRVc << drvv[0], drvv[1];
    DM << dmv[0], dmv[2], dmv[1], dmv[3];

    // Declaration order: p[2], q.
    const double pts[2][3] = {{0.4, -0.7, 0.6}, {-0.3, 0.9, -0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) lex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::add;
      using stan::math::divide;
      using stan::math::elt_divide;
      using stan::math::elt_multiply;
      using stan::math::multiply;
      using stan::math::squared_distance;
      using stan::math::subtract;
      using stan::math::sum;
      // stan-math's own answer for both halves. Transformed data is
      // doubles on either side, so it is the same call with the same
      // types the interpreter makes.
      double td = sum(add(DV, 0.5)) + sum(add(DV, DV)) +
                  sum(subtract(DV, 0.25)) + sum(subtract(0.75, DV)) +
                  sum(multiply(DM, DV)) + multiply(DRV, DV) +
                  sum(multiply(DV, DRV)) + sum(multiply(DM, DM)) +
                  sum(elt_multiply(DV, DV)) + sum(elt_multiply(2.0, DV)) +
                  sum(divide(DV, 2.0)) + sum(divide(1.5, DM)) +
                  sum(elt_divide(DV, DV)) + sum(elt_divide(1.0, DV)) +
                  squared_distance(DV, DRV) + squared_distance(dvv[0], dvv[1]);

      VecV P(2);
      P << pts[c][0], pts[c][1];
      var q = pts[c][2];
      var acc = td;
      acc += sum(add(P, q));
      acc += sum(add(P, DV));
      acc += sum(subtract(q, P));
      acc += sum(elt_multiply(P, P));
      acc += sum(elt_multiply(q, P));
      acc += sum(divide(P, q));
      acc += sum(elt_divide(q, P));
      acc += sum(divide(q, DM));
      acc += sum(multiply(DM, P));
      acc += multiply(P.transpose().eval(), P);
      acc += sum(MatV(multiply(P, P.transpose().eval())));
      acc += squared_distance(P, DRVc);
      acc += squared_distance(P, DV);
      acc += squared_distance(q, P(0));
      acc += add(P(0), q) - divide(P(1), q);
      acc.grad();

      const std::string tag = "opalias c" + std::to_string(c);
      expect_ulp(tag + " gp0", grad[0], P(0).adj());
      expect_ulp(tag + " gp1", grad[1], P(1).adj());
      expect_ulp(tag + " gq", grad[2], q.adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, tag + " lp");
    }
  }

  // Two-argument scalar math with an int argument. See
  // tests/fixtures/binint.stan. The int side has no derivative, so the
  // whole gradient belongs to the real side, and the interesting case is
  // the pairing: a matrix is column-major, an int array's trailing extents
  // are row-major, and stan-math pairs n[i][j] with m(i, j).
  {
    const int kk = 2;
    const std::vector<int> counts = {0, 1, 2};
    // {{0, 3}, {1, 2}}: distinct, so a swapped pairing is a factor of two.
    const int expo[2][2] = {{0, 3}, {1, 2}};
    const int expo3[2][2][2] = {{{0, 3}, {1, 2}}, {{2, 0}, {3, 1}}};
    // Through the JSON reader, because expo3 is rank three and the data
    // path is the only thing that stores an array of that rank. `nn` is
    // the same shape as `expo` below but read rather than built from a
    // literal: the interpreter reaches a data variable and a literal by
    // different paths, and only one of them is covered by `expo`.
    DataMap d = DataMap::from_json(R"({"k": )" + std::to_string(kk) + R"(,
            "counts": [0, 1, 2],
            "expo3": [[[0, 3], [1, 2]], [[2, 0], [3, 1]]],
            "nn": [[2, 0], [3, 1]]})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/binint.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    // Declaration order: a[3], b.
    const double pts[2][4] = {{0.4, -0.7, 0.25, 0.6},
                              {-0.3, 0.9, -0.15, -0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 4; ++i) lex.params_data()[i] = pts[c][i];
      double grad[4] = {0, 0, 0, 0};
      const double lp = lex.gradient(grad);

      using stan::math::var;
      std::vector<var> a = {pts[c][0], pts[c][1], pts[c][2]};
      var b = pts[c][3];
      std::vector<var> p;
      for (int i = 0; i < 3; ++i) p.push_back(stan::math::exp(a[i]) + 1);
      // One reduction per target statement, in statement order, so the
      // reference reassociates the way the graph does.
      var acc = 0, s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::bessel_first_kind(kk, a[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i)
        s += stan::math::modified_bessel_first_kind(kk, a[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::bessel_second_kind(kk, p[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i)
        s += stan::math::modified_bessel_second_kind(kk, p[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::lmgamma(kk, p[i]);
      acc += s;
      acc += stan::math::binary_log_loss(1, stan::math::inv_logit(b));
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::falling_factorial(p[i], kk);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i)
        s += stan::math::rising_factorial(p[i], counts[i]);
      acc += s;
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::ldexp(a[i], counts[i]);
      acc += s;
      // m = [[a1, a2], [a3, b]], summed column-major, each entry against
      // the int array entry at the SAME logical position.
      const std::vector<var> mcol = {a[0], a[2], a[1], b};
      const int ecol[4] = {expo[0][0], expo[1][0], expo[0][1], expo[1][1]};
      s = 0;
      for (int i = 0; i < 4; ++i) s += stan::math::ldexp(mcol[i], ecol[i]);
      acc += s;
      // An array of matrices against a deeper int array. The extents that
      // set the pairing are the leaf's, so each element repeats the case
      // above with its own slice of expo3; `sum(am[1]) + sum(am[2])` is one
      // target statement, so the two element sums join before the
      // accumulator sees them.
      var e1 = 0, e2 = 0;
      for (int i = 0; i < 4; ++i) {
        const int lo[4] = {expo3[0][0][0], expo3[0][1][0], expo3[0][0][1],
                           expo3[0][1][1]};
        const int hi[4] = {expo3[1][0][0], expo3[1][1][0], expo3[1][0][1],
                           expo3[1][1][1]};
        e1 += stan::math::ldexp(mcol[i], lo[i]);
        e2 += stan::math::ldexp(mcol[i] + 1, hi[i]);
      }
      acc += e1 + e2;
      s = 0;
      for (int i = 0; i < 3; ++i) s += stan::math::ldexp(b, counts[i]);
      acc += s;
      // Transformed data: the MIR interpreter's own copies of the same
      // shapes. 39 is [[1, 2], [3, 4]] against the literal {{0, 3}, {1, 2}}
      // and 38 the same matrix against the data array {{2, 0}, {3, 1}},
      // both paired n[i][j] with m(i, j). A flat pairing would answer 45
      // and 31.
      acc += stan::math::ldexp(1.5, 0) + stan::math::ldexp(2.5, 1) +
             stan::math::ldexp(3.5, 2) + stan::math::lmgamma(2, 1.75) + 39.0 +
             38.0;
      acc.grad();

      // Measured bitwise at both points: the graph builds the same lanes
      // in the same order the reference tape does.
      bool gok = grad[3] == b.adj();
      for (int i = 0; i < 3; ++i) gok = gok && grad[i] == a[i].adj();
      check(gok, "binint: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "binint: lp matches the var path");
      stan::math::recover_memory();
    }
  }

  // Regular scalar-function metadata is shared with the runtime-control
  // compiler. The branch forces ordinary unary/binary calls and all nine
  // int/real binary families through Program::CALL, including array results
  // and both int argument positions.
  {
    CompiledModel rm = compile_model(
        slurp("tests/fixtures/paramcond_regular_calls.tmir.sexp"), DataMap());
    check(rm.n_unconstrained == 7, "regular-call island parameter width");
    check(count_opcode(rm, OP_ISLAND) > 0, "regular-call runtime island");
    Executor rex(std::move(rm.graph));
    rm.bind(rex);

    const double xv[3] = {3.2, 3.6, 4.1};
    const double pv[3] = {0.2, 0.4, 0.7};
    rex.params_data()[0] = -0.5;
    for (int i = 0; i < 3; ++i) {
      rex.params_data()[1 + i] = xv[i];
      rex.params_data()[4 + i] = pv[i];
    }
    double gradient[7] = {};
    expect_eq("regular-call untaken lp", rex.gradient(gradient), 0.0);
    for (double g : gradient)
      expect_eq("regular-call untaken gradient", g, 0.0);

    rex.params_data()[0] = 0.5;
    const double lp = rex.gradient(gradient);
    using stan::math::var;
    std::vector<var> x = {xv[0], xv[1], xv[2]};
    std::vector<var> probability = {pv[0], pv[1], pv[2]};
    const int order[3] = {0, 1, 2};
    const int label[3] = {0, 1, 0};
    const int count[3] = {0, 1, 2};
    var acc = 0, term = 0;
    acc += stan::math::exp(x[0]) + stan::math::exp(x[1]);
    acc += stan::math::atan2(x[2], probability[2]) +
           stan::math::atan2(x[0], probability[0]);
#define REGULAR_TERM(expr)                  \
  term = 0;                                 \
  for (int i = 0; i < 3; ++i) term += expr; \
  acc += term;
    REGULAR_TERM(stan::math::atan2(x[i], probability[i]))
    REGULAR_TERM(stan::math::tgamma(x[i]))
    REGULAR_TERM(stan::math::bessel_first_kind(order[i], x[i]))
    REGULAR_TERM(stan::math::bessel_second_kind(order[i], x[i]))
    REGULAR_TERM(stan::math::binary_log_loss(label[i], probability[i]))
    REGULAR_TERM(stan::math::falling_factorial(x[i], count[i]))
    REGULAR_TERM(stan::math::ldexp(x[i], count[i]))
    REGULAR_TERM(stan::math::lmgamma(2, x[i]))
    REGULAR_TERM(stan::math::modified_bessel_first_kind(order[i], x[i]))
    REGULAR_TERM(stan::math::modified_bessel_second_kind(order[i], x[i]))
    REGULAR_TERM(stan::math::rising_factorial(x[i], count[i]))
#undef REGULAR_TERM
    acc += stan::math::choose(2, 0);
    acc.grad();

    check(std::abs(lp - acc.val()) <=
              16 * 2.220446049250313e-16 * std::abs(acc.val()),
          "regular-call island lp matches stan-math");
    expect_eq("regular-call branch gradient", gradient[0], 0.0);
    for (int i = 0; i < 3; ++i) {
      expect_ulp("regular-call real gradient", gradient[1 + i], x[i].adj());
      expect_ulp("regular-call probability gradient", gradient[4 + i],
                 probability[i].adj());
    }
    stan::math::recover_memory();
  }

  // The one-argument scalar math library over containers. See
  // tests/fixtures/unaryfns.stan: transformed data runs the MIR interpreter
  // on doubles and the model block runs the graph kernels, so a name wired
  // into one and not the other is caught here rather than as a wrong lp.
  {
    const std::vector<double> matv = {0.5, 1.5, 1.25, 0.25, 2.0, 2.5};
    DataMap d;
    d.set_real_array("mat", matv, {2, 3});
    CompiledModel um =
        compile_model(slurp("tests/fixtures/unaryfns.tmir.sexp"), d);
    Executor uex(std::move(um.graph));
    um.bind(uex);
    // Transformed data: evaluated once by the interpreter, on doubles. Its
    // reference is spelled the way the block is, left to right, each sum
    // ascending from zero, because that is the arithmetic the interpreter
    // performs.
    const double tdx[3] = {0.5, 1.5, 2.5};
    const double tdm[3] = {1.5, 2.5, 3.5};
    double s_gamma = 0, s_tri = 0, s_minus = 0, s_plus = 0;
    for (int i = 0; i < 3; ++i) {
      s_gamma += stan::math::tgamma(tdx[i]);
      s_tri += stan::math::trigamma(tdx[i]);
      s_minus += -tdm[i];
      s_plus += tdm[i];
    }
    const double td_sum =
        s_gamma + s_tri + stan::math::inv_Phi(0.6) +
        stan::math::std_normal_log_qf(-0.5) + stan::math::lambert_w0(0.5) +
        stan::math::lambert_wm1(-0.2) + stan::math::inv_erfc(0.75) +
        stan::math::Phi_approx(0.25) + s_minus + s_plus;

    const double pts[2][3] = {{0.4, -0.7, 0.25}, {-0.3, 0.9, -0.15}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) uex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = uex.gradient(grad);

      using stan::math::var;
      std::vector<var> a = {pts[c][0], pts[c][1], pts[c][2]};
      std::vector<var> u, lu, wm, pos;
      for (int i = 0; i < 3; ++i) {
        u.push_back(stan::math::inv_logit(a[i]));
        lu.push_back(stan::math::log(u[i]));
        wm.push_back(-0.2 * u[i]);
        pos.push_back(stan::math::exp(a[i]));
      }
      var acc = 0, s = 0;
#define TERM(expr)                         \
  s = 0;                                   \
  for (int i = 0; i < 3; ++i) s += (expr); \
  acc += s;
      TERM(stan::math::tgamma(pos[i]))
      TERM(stan::math::trigamma(pos[i]))
      TERM(stan::math::lambert_w0(pos[i]))
      TERM(stan::math::lambert_wm1(wm[i]))
      TERM(stan::math::inv_Phi(u[i]))
      TERM(stan::math::std_normal_log_qf(lu[i]))
      TERM(stan::math::inv_erfc(u[i]))
      TERM(stan::math::Phi_approx(a[i]))
      TERM(-a[i])
      TERM(a[i])
#undef TERM
      s = 0;
      for (double m : matv) s += stan::math::Phi_approx(m + a[0]);
      acc += s;
      // `sum(na[1]) + sum(na[2])` is one target term, so the two element
      // sums join each other before they reach the accumulator.
      var n1 = 0, n2 = 0;
      for (int i = 0; i < 3; ++i) n1 += stan::math::tgamma(pos[i]);
      for (int i = 0; i < 3; ++i) n2 += stan::math::tgamma(u[i]);
      acc += n1 + n2;
      acc += td_sum;
      acc.grad();

      // Each element of `a` feeds a dozen terms through `u`, `pos` and
      // `lu`, and the graph folds a slot's contributions in op order where
      // the var chain folds them one vari at a time, so this lands inside
      // the project's 2 ULP budget rather than on it -- measured at 1 ULP
      // on one element of one point and 0 everywhere else. The per-function
      // pullbacks themselves are pinned bitwise in
      // tests/test_mir_unary_fallback.cpp.
      for (int i = 0; i < 3; ++i)
        expect_ulp("unaryfns grad", grad[i], a[i].adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "unaryfns: lp matches the var path");
    }
  }

  // Stan's bound transforms called as functions. See
  // tests/fixtures/boundfn.stan: all twelve names, over the container
  // shapes and both bound widths, inside the `_jacobian` user function the
  // conformance sweep generates. The lowering refused every one of them,
  // and the jacobian direction is the half that can be silently wrong --
  // it has to add the log absolute jacobian determinant in log_prob and
  // nothing at all in write_array.
  {
    // md in slot order (first index fastest), which is also what a 2x2
    // Eigen matrix stores.
    const std::vector<double> mdv = {0.5, 1.5, 1.25, 0.25};
    DataMap d;
    d.set_real_array("md", mdv, {2, 2});
    CompiledModel bm =
        compile_model(slurp("tests/fixtures/boundfn.tmir.sexp"), d);
    Executor bex(std::move(bm.graph));
    bm.bind(bex);
    // Declaration order: a[2], s.
    const double pts[2][3] = {{0.4, -0.7, 1.25}, {-0.3, 0.9, -0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 3; ++i) bex.params_data()[i] = pts[c][i];
      double grad[3] = {0, 0, 0};
      const double lp = bex.gradient(grad);

      using stan::math::var;
      using Vec = Eigen::Matrix<var, -1, 1>;
      Vec a(2);
      a << pts[c][0], pts[c][1];
      var s = pts[c][2];
      Vec lo(2), hi(2);
      for (int i = 0; i < 2; ++i) {
        lo(i) = a(i) - 4.0;
        hi(i) = stan::math::exp(a(i)) + 1.0;
      }
      Eigen::Matrix<var, -1, -1> m(2, 2);
      for (int i = 0; i < 4; ++i) m(i) = mdv[i];
      // Every `sum` in the fixture is one ascending reduction over its own
      // result, and OP_SUM_VEC accumulates the same way.
      const auto vsum = [](const auto& v) {
        var t = 0;
        for (int i = 0; i < v.size(); ++i) t += v(i);
        return t;
      };
      // The four jacobian increments land on lp__, which the model adds to
      // the returned contribution.
      var jac = 0;
      var cc = 0;
      cc += vsum(stan::math::lb_constrain(a, s));
      cc += vsum(stan::math::lb_constrain<true>(a, lo, jac));
      cc += vsum(stan::math::lb_free(stan::math::lb_constrain(a, s), s));
      cc += vsum(stan::math::ub_constrain(m, s));
      cc += vsum(stan::math::ub_constrain<true>(a, hi, jac));
      cc += vsum(stan::math::ub_free(stan::math::ub_constrain(a, s), s));
      cc += vsum(stan::math::lub_constrain(a, lo, hi));
      cc += vsum(stan::math::lub_constrain<true>(a, var(-3.0), var(3.0), jac));
      cc += vsum(
          stan::math::lub_free(stan::math::lub_constrain(a, lo, hi), lo, hi));
      cc += vsum(stan::math::offset_multiplier_constrain(a, s, var(2.5)));
      cc += vsum(stan::math::offset_multiplier_constrain(a, s, var(2.5)));
      cc += vsum(stan::math::offset_multiplier_constrain<true>(a, lo, hi, jac));
      cc += vsum(stan::math::offset_multiplier_free(a, lo, hi));
      var acc = cc + jac;
      acc.grad();

      for (int i = 0; i < 2; ++i)
        expect_ulp("boundfn a grad", grad[i], a(i).adj());
      expect_ulp("boundfn s grad", grad[2], s.adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "boundfn: lp matches the var path");
      stan::math::recover_memory();
    }
    // write_array runs the same calls with `jacobian__ = false`, so the
    // `total` column is the returned contribution alone. log_prob adds the
    // four jacobian increments to it, and each of those is nonzero here.
    check(bm.write_array && bm.write_array->truncated.empty(),
          "boundfn write_array compiled");
    if (bm.write_array && bm.write_array->truncated.empty()) {
      Executor wex(std::move(bm.write_array->graph));
      bm.write_array->bind(wex);
      for (int i = 0; i < 3; ++i) wex.params_data()[i] = pts[0][i];
      wex.run_forward_only();
      for (int i = 0; i < 3; ++i) bex.params_data()[i] = pts[0][i];
      const double lp = bex.forward();
      bool found = false;
      for (const auto& col : bm.write_array->columns) {
        if (col.name != "total") continue;
        found = true;
        check(*wex.value_ptr(col.slot) != lp,
              "boundfn: write_array drops the jacobian increments");
      }
      check(found, "boundfn write_array has total");
    }
  }

  // Runtime-control Programs materialize matrix rows into contiguous
  // registers, but min/max must retain the source Matrix<var> scalar
  // traversal. Interior NaNs and signed-zero ties make that grouping
  // observable in both the value and the selected adjoint.
  {
    DataMap data;
    data.set_int("mode", 1);
    CompiledModel mm = compile_model(
        slurp("tests/fixtures/runtime_extrema_grouping.tmir.sexp"), data);
    check(count_opcode(mm, OP_ISLAND) > 0,
          "runtime extrema grouping uses a control-flow island");
    Executor mex(std::move(mm.graph));
    mm.bind(mex);
    const double q[] = {0.1, -4.0, -4.0,
                        std::numeric_limits<double>::quiet_NaN(), -4.0};
    for (int i = 0; i < 5; ++i) mex.params_data()[i] = q[i];
    double grad[5] = {0};
    expect_eq("runtime row min lp", mex.gradient(grad), -0.25);
    expect_eq("runtime row min selected grad", grad[1], -0.0625);
    expect_eq("runtime row min NaN grad", grad[3], 0.0);
  }
  {
    DataMap data;
    data.set_int("mode", 2);
    CompiledModel mm = compile_model(
        slurp("tests/fixtures/runtime_extrema_grouping.tmir.sexp"), data);
    Executor mex(std::move(mm.graph));
    mm.bind(mex);
    const double q[] = {0.1, -0.0, -2.0, +0.0, -3.0};
    for (int i = 0; i < 5; ++i) mex.params_data()[i] = q[i];
    double grad[5] = {0};
    const double lp = mex.gradient(grad);
    check(std::isinf(lp) && std::signbit(lp),
          "runtime row max retains the first signed-zero tie");
    check(std::isinf(grad[1]) && std::signbit(grad[1]),
          "runtime row max routes the tied adjoint to the first coefficient");
    expect_eq("runtime row max second tie grad", grad[3], 0.0);
  }

  // Every callable Jacobian transform, including the two stochastic-matrix
  // families and representative array overloads. The unconditional call is
  // graph-lowered; the parameter-dependent transformed-parameter branch is
  // compiled into a typed Program::TRANSFORM and replayed under var.
  {
    CompiledModel jm = compile_model(
        slurp("tests/fixtures/callable_jacobians.tmir.sexp"), DataMap());
    check(jm.n_unconstrained == 37, "callable jacobians parameter width");
    check(count_opcode(jm, OP_CONSTRAIN_STOCHASTIC_COLUMN) > 0,
          "callable stochastic-column graph kernel");
    check(count_opcode(jm, OP_CONSTRAIN_STOCHASTIC_ROW) > 0,
          "callable stochastic-row graph kernel");
    check(count_opcode(jm, OP_ISLAND) > 0,
          "callable jacobians runtime-control island");
    Executor jex(std::move(jm.graph));
    jm.bind(jex);
    std::vector<double> gradient((size_t)jm.n_unconstrained);
    double lp[2] = {0, 0};
    for (int side = 0; side < 2; ++side) {
      for (int64_t i = 0; i < jm.n_unconstrained; ++i)
        jex.params_data()[i] = 0.025 * (double)(i + 1);
      jex.params_data()[0] = side ? 0.5 : -0.5;
      lp[side] = jex.gradient(gradient.data());
      check(std::isfinite(lp[side]), "callable jacobians finite log density");
      check(std::all_of(gradient.begin(), gradient.end(),
                        [](double x) { return std::isfinite(x); }),
            "callable jacobians finite gradient");
    }
    check(lp[0] != lp[1], "callable jacobians runtime branch changes target");
  }

  // A transformed-data real passed to a recursive UDF inside parameter-
  // dependent control flow remains known. A complete pure call is evaluated
  // once by the data interpreter instead of requiring a finite expansion of
  // its recursive call tree, while the surrounding branch remains an island.
  {
    CompiledModel km = compile_model(
        slurp("tests/fixtures/known_real_udf.tmir.sexp"), DataMap());
    check(km.n_unconstrained == 1, "known-real UDF parameter width");
    check(count_opcode(km, OP_ISLAND) > 0, "known-real UDF runtime island");
    Executor kex(std::move(km.graph));
    km.bind(kex);
    kex.params_data()[0] = 0.25;
    double gradient[1];
    const double lp = kex.gradient(gradient);
    expect_eq("known-real UDF lp", lp, -0.5 * 0.25 * 0.25);
    expect_eq("known-real UDF gradient", gradient[0], -0.25);
  }

  // Forty recursive steps exceed ProgramCompiler's inlining budget but stay
  // within the MIR interpreter's supported call depth. This is the regression
  // boundary: concrete pure recursion must use the latter rather than fail
  // while trying to manufacture a finite register call stack.
  {
    CompiledModel km = compile_model(
        slurp("tests/fixtures/known_real_udf_deep.tmir.sexp"), DataMap());
    check(km.n_unconstrained == 1, "deep known-real UDF parameter width");
    check(count_opcode(km, OP_ISLAND) > 0,
          "deep known-real UDF runtime island");
    Executor kex(std::move(km.graph));
    km.bind(kex);
    kex.params_data()[0] = 0.25;
    double gradient[1];
    const double lp = kex.gradient(gradient);
    expect_eq("deep known-real UDF lp", lp, -0.5 * 0.25 * 0.25);
    expect_eq("deep known-real UDF gradient", gradient[0], -0.25);
  }

  // target() is a stateful read of the density accumulated at that source
  // point. The first read lowers directly into the graph; the reads in the
  // parameter-dependent branch (including one in an inlined _lp UDF) receive
  // the graph prefix as an island live-in while publishing only their local
  // delta back to the model target.
  {
    CompiledModel tm =
        compile_model(slurp("tests/fixtures/target_read.tmir.sexp"), DataMap());
    check(tm.n_unconstrained == 1, "target-read parameter width");
    check(count_opcode(tm, OP_ISLAND) > 0, "target-read runtime island");
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    double gradient[1] = {};

    tex.params_data()[0] = -2.0;
    expect_eq("target-read untaken lp", tex.gradient(gradient), -2.5);
    expect_eq("target-read untaken gradient", gradient[0], 2.5);

    tex.params_data()[0] = 2.0;
    expect_eq("target-read taken lp", tex.gradient(gradient), 2.0);
    expect_eq("target-read taken gradient", gradient[0], -5.0);
  }

  // A higher-order callback inside parameter-dependent control flow uses the
  // same argument binder and UDF inliner as an ordinary call. Serial
  // reduce_sum supplies the complete slice and synthesized bounds while its
  // shared real argument remains differentiable.
  {
    CompiledModel rm = compile_model(
        slurp("tests/fixtures/reduce_sum_region.tmir.sexp"), DataMap());
    check(rm.n_unconstrained == 1, "reduce_sum region parameter width");
    check(count_opcode(rm, OP_ISLAND) > 0, "reduce_sum runtime island");
    Executor rex(std::move(rm.graph));
    rm.bind(rex);
    double gradient[1] = {};

    rex.params_data()[0] = -2.0;
    expect_eq("reduce_sum untaken lp", rex.gradient(gradient), -2.0);
    expect_eq("reduce_sum untaken gradient", gradient[0], 2.0);

    rex.params_data()[0] = 2.0;
    expect_eq("reduce_sum taken lp", rex.gradient(gradient), 10.0);
    expect_eq("reduce_sum taken gradient", gradient[0], 10.0);
  }

  // The preparation interpreter routes retained higher-order calls through
  // the same registered kernels as graph and runtime-control execution.
  // map_rect and reduce_sum remain shared structural interpreter operations.
  {
    CompiledModel hm = compile_model(
        slurp("tests/fixtures/higher_order_transformed_data.tmir.sexp"),
        DataMap());
    check(hm.n_unconstrained == 1, "transformed-data HOF parameter width");
    Executor hex(std::move(hm.graph));
    hm.bind(hex);
    hex.params_data()[0] = 0.0;
    double gradient[1] = {};
    const double lp = hex.gradient(gradient);
    const double expected = 3.0 + 10.0 + 2.0 + 0.5 + 0.2 + 0.2 + 0.2;
    check(std::fabs(lp + 0.5 * expected * expected) < 1e-7,
          "transformed-data HOF lp");
    check(std::fabs(gradient[0] - expected) < 1e-7,
          "transformed-data HOF gradient");
    check(hm.write_array && !hm.write_array->interp &&
              hm.write_array->truncated.empty(),
          "higher-order write_array compiled completely");
    if (hm.write_array && !hm.write_array->interp &&
        hm.write_array->truncated.empty()) {
      Executor wex(std::move(hm.write_array->graph));
      hm.write_array->bind(wex);
      wex.params_data()[0] = 0.0;
      wex.run_forward_only();
      std::vector<double> row;
      for (const auto& column : hm.write_array->columns) {
        const double* values = wex.value_ptr(column.slot);
        for (int64_t i = 0; i < column.len; ++i)
          row.push_back(values[column.storage_index(i)]);
      }
      const std::vector<double> want{0.0, 10.0, 2.0, 0.5, 0.2, 0.2, 1e-10, 0.2};
      check(row.size() == want.size(), "higher-order write_array row width");
      if (row.size() == want.size())
        for (size_t i = 0; i < row.size(); ++i)
          check(std::fabs(row[i] - want[i]) < 1e-7,
                "higher-order write_array value " + std::to_string(i));
    }
  }

  // The complete stanc higher-order inventory, plus every compiler-special
  // solver variant, must cross all four runtime paths in one model.  Generic
  // integrate_ode is the deprecated RK45 spelling; keeping it in this census
  // prevents the flat --dump-stan-math-signatures inventory from drifting
  // away from the shared family classifier.  write_array deliberately uses
  // its interpreter when transformed parameters make data-level callback
  // arguments available only at draw time.
  {
    CompiledModel hm = compile_model(
        slurp("tests/fixtures/higher_order_all_contexts.tmir.sexp"), DataMap());
    check(hm.n_unconstrained == 7, "all-context HOF parameter width");
    check(count_opcode(hm, OP_ISLAND) > 0, "all-context HOF runtime island");
    Executor hex(std::move(hm.graph));
    hm.bind(hex);
    double gradient[7] = {};
    const auto set_point = [&](double gate) {
      hex.params_data()[0] = gate;
      hex.params_data()[1] = 0.2;
      hex.params_data()[2] = 0.4;
      hex.params_data()[3] = 0.0;
      hex.params_data()[4] = std::log(0.1);
      hex.params_data()[5] = 0.4;
      hex.params_data()[6] = 0.4;
    };
    const double graph_total = 9.782;
    const double data_total = 9.882;
    const double prior = -0.5 * (1.0 + std::pow(std::log(0.1) + 2.0, 2)) -
                         3.5 * std::log(2.0 * std::acos(-1.0));

    set_point(-1.0);
    const double untaken = hex.gradient(gradient);
    check(std::fabs(untaken - (prior + data_total + graph_total)) < 1e-6,
          "all-context HOF untaken lp");
    for (double value : gradient)
      check(std::isfinite(value), "all-context HOF untaken gradient");

    set_point(1.0);
    const double taken = hex.gradient(gradient);
    check(std::fabs(taken - (untaken + graph_total)) < 1e-6,
          "all-context HOF taken lp");
    for (double value : gradient)
      check(std::isfinite(value), "all-context HOF taken gradient");

    check(hm.write_array && hm.write_array->interp,
          "all-context HOF write_array interpreter selected");
    if (hm.write_array && hm.write_array->interp) {
      WaRng rng(123);
      const auto row =
          hm.write_array->interp->eval(hm.constrained_env(hex), rng);
      check(row.size() == 10, "all-context HOF write_array row width");
      for (double value : row)
        check(std::isfinite(value), "all-context HOF write_array value");
      if (row.size() == 10) {
        check(std::fabs(row[7] - graph_total) < 1e-6,
              "all-context HOF transformed-parameter total");
        check(std::fabs(row[8] - graph_total) < 1e-6,
              "all-context HOF generated-quantity total");
        check(std::fabs(row[9] - data_total) < 1e-6,
              "all-context HOF transformed-data total");
      }
    }
  }

  // Serial map_rect has statically-shaped jobs, so a runtime-control program
  // expands them into ordinary callback invocations and concatenates their
  // vector results. Shared and per-job parameters retain their adjoints;
  // each real/integer data row is bound to the matching callback call.
  {
    CompiledModel mm = compile_model(
        slurp("tests/fixtures/map_rect_region.tmir.sexp"), DataMap());
    check(mm.n_unconstrained == 2, "map_rect region parameter width");
    check(count_opcode(mm, OP_ISLAND) > 0, "map_rect runtime island");
    Executor mex(std::move(mm.graph));
    mm.bind(mex);
    double gradient[2] = {};

    mex.params_data()[0] = -1.0;
    mex.params_data()[1] = 2.0;
    expect_eq("map_rect untaken lp", mex.gradient(gradient), 265.5);
    expect_eq("map_rect untaken gate gradient", gradient[0], 1.0);
    expect_eq("map_rect untaken x gradient", gradient[1], 102.0);

    mex.params_data()[0] = 1.0;
    mex.params_data()[1] = 2.0;
    expect_eq("map_rect taken lp", mex.gradient(gradient), 533.5);
    expect_eq("map_rect taken gate gradient", gradient[0], -1.0);
    expect_eq("map_rect taken x gradient", gradient[1], 206.0);
  }

  // Retained higher-order algorithms use the same graph-kernel CALL adapter
  // as regular functions. Its owned callback specification remains attached
  // to the runtime region and its input activity mask preserves the solver's
  // contract that only theta, not the initial guess, is differentiated.
  {
    CompiledModel am = compile_model(
        slurp("tests/fixtures/algebra_region.tmir.sexp"), DataMap());
    check(am.n_unconstrained == 2, "algebra region parameter width");
    check(count_opcode(am, OP_ISLAND) > 0, "algebra runtime island");
    Executor aex(std::move(am.graph));
    am.bind(aex);
    double gradient[2] = {};

    aex.params_data()[0] = -1.0;
    aex.params_data()[1] = 2.0;
    expect_eq("algebra untaken lp", aex.gradient(gradient), -2.5);
    expect_eq("algebra untaken gate gradient", gradient[0], 1.0);
    expect_eq("algebra untaken theta gradient", gradient[1], -2.0);

    aex.params_data()[0] = 1.0;
    expect_eq("algebra taken lp", aex.gradient(gradient), 5.5);
    expect_eq("algebra taken gate gradient", gradient[0], -1.0);
    expect_eq("algebra taken theta gradient", gradient[1], 2.0);
  }

  // Modern algebra solvers, including explicit-control variants, use their
  // runtime-control implementation both as a direct graph producer and under
  // a parameter-dependent branch.
  {
    CompiledModel am = compile_model(
        slurp("tests/fixtures/algebra_shared.tmir.sexp"), DataMap());
    check(am.n_unconstrained == 2, "shared algebra parameter width");
    Executor aex(std::move(am.graph));
    am.bind(aex);
    double gradient[2] = {};

    aex.params_data()[0] = -1.0;
    aex.params_data()[1] = 2.0;
    expect_eq("shared algebra untaken lp", aex.gradient(gradient), 5.5);
    expect_eq("shared algebra untaken gate gradient", gradient[0], 1.0);
    expect_eq("shared algebra untaken wanted gradient", gradient[1], 2.0);

    aex.params_data()[0] = 1.0;
    expect_eq("shared algebra taken lp", aex.gradient(gradient), 13.5);
    expect_eq("shared algebra taken gate gradient", gradient[0], -1.0);
    expect_eq("shared algebra taken wanted gradient", gradient[1], 6.0);
  }

  // Every one-dimensional integration frontend shares one retained callback
  // ABI and one quadrature kernel. The legacy call exercises ordinary graph
  // lowering; the four modern method/tolerance forms exercise Program CALL
  // lowering inside runtime control. Bounds and packed callback reals remain
  // independently differentiable in both backends.
  {
    CompiledModel qm = compile_model(
        slurp("tests/fixtures/quadrature_region.tmir.sexp"), DataMap());
    check(qm.n_unconstrained == 3, "quadrature region parameter width");
    check(count_opcode(qm, OP_QUADRATURE) > 0, "quadrature graph kernel");
    check(count_opcode(qm, OP_ISLAND) > 0, "quadrature runtime island");
    Executor qex(std::move(qm.graph));
    qm.bind(qex);
    double gradient[3] = {};

    qex.params_data()[0] = -1.0;
    qex.params_data()[1] = 2.0;
    qex.params_data()[2] = 3.0;
    double quadrature_lp = qex.gradient(gradient);
    check(std::fabs(quadrature_lp - 6.0) < 1e-10,
          "quadrature untaken lp " + std::to_string(quadrature_lp));
    check(std::fabs(gradient[0]) < 1e-12, "quadrature untaken gate gradient");
    check(std::fabs(gradient[1] - 6.0) < 1e-10,
          "quadrature untaken bound gradient");
    check(std::fabs(gradient[2] - 2.0) < 1e-10,
          "quadrature untaken callback gradient");

    qex.params_data()[0] = 1.0;
    qex.params_data()[1] = 2.0;
    qex.params_data()[2] = 3.0;
    quadrature_lp = qex.gradient(gradient);
    check(std::fabs(quadrature_lp - 30.0) < 1e-9,
          "quadrature taken lp " + std::to_string(quadrature_lp));
    check(std::fabs(gradient[0]) < 1e-12, "quadrature taken gate gradient");
    check(std::fabs(gradient[1] - 30.0) < 1e-8,
          "quadrature taken bound gradient");
    check(std::fabs(gradient[2] - 10.0) < 1e-8,
          "quadrature taken callback gradient");
  }

  // Every ODE frontend uses the existing OP_ODE kernel from runtime control.
  // A zero RHS makes all solver answers exact and exposes whether initial
  // state and callback-parameter activity are wired correctly.
  {
    CompiledModel om =
        compile_model(slurp("tests/fixtures/ode_region.tmir.sexp"), DataMap());
    check(om.n_unconstrained == 3, "ODE region parameter width");
    check(count_opcode(om, OP_ISLAND) > 0, "ODE runtime island");
    Executor oex(std::move(om.graph));
    om.bind(oex);
    double gradient[3] = {};

    oex.params_data()[0] = -1.0;
    oex.params_data()[1] = 2.0;
    oex.params_data()[2] = 3.0;
    expect_eq("ODE untaken lp", oex.gradient(gradient), -7.0);
    expect_eq("ODE untaken gate gradient", gradient[0], 1.0);
    expect_eq("ODE untaken initial gradient", gradient[1], -2.0);
    expect_eq("ODE untaken rate gradient", gradient[2], -3.0);

    oex.params_data()[0] = 1.0;
    expect_eq("ODE taken lp", oex.gradient(gradient), 15.0);
    expect_eq("ODE taken gate gradient", gradient[0], -1.0);
    expect_eq("ODE taken initial gradient", gradient[1], 9.0);
    expect_eq("ODE taken rate gradient", gradient[2], -3.0);
  }

  // DAE and DAE_tol share the retained callback ABI and OP_DAE kernel. The
  // unconditional call enters through ordinary expression lowering; the
  // tolerance call is compiled inside a parameter-dependent Program region.
  {
    CompiledModel dm =
        compile_model(slurp("tests/fixtures/dae_region.tmir.sexp"), DataMap());
    check(dm.n_unconstrained == 3, "DAE region parameter width");
    check(count_opcode(dm, OP_ISLAND) > 0, "DAE shared runtime island");
    int dae_calls = 0;
    for (const Op& op : dm.graph.ops) {
      if (op.opcode != OP_ISLAND) continue;
      const auto* program = static_cast<const IslandProg*>(op.udata);
      for (const Program::Call& call : program->calls) {
        if (call.opcode != OP_DAE) continue;
        ++dae_calls;
        const auto* spec = static_cast<const DaeSpec*>(call.udata_owner.get());
        check(spec != nullptr && spec->prog.ok,
              "DAE retained callback register program");
      }
    }
    check(dae_calls == 2, "DAE and DAE_tol use shared kernel calls");
    Executor dex(std::move(dm.graph));
    dm.bind(dex);
    double gradient[3] = {};

    dex.params_data()[0] = -1.0;
    dex.params_data()[1] = 2.0;
    dex.params_data()[2] = 3.0;
    const double dae_value_only = dex.forward_value_only();
    check(std::fabs(dae_value_only + 4.4) < 1e-8,
          "DAE value-only lp " + std::to_string(dae_value_only));
    double dae_lp = dex.gradient(gradient);
    check(std::fabs(dae_lp + 4.4) < 1e-8,
          "DAE untaken lp " + std::to_string(dae_lp));
    check(std::fabs(gradient[0] - 1.0) < 1e-8, "DAE untaken gate gradient");
    check(std::fabs(gradient[1] + 1.0) < 1e-7, "DAE untaken initial gradient");
    check(std::fabs(gradient[2] + 2.8) < 1e-7, "DAE untaken rate gradient");

    dex.params_data()[0] = 1.0;
    dae_lp = dex.gradient(gradient);
    check(std::fabs(dae_lp + 1.8) < 1e-8,
          "DAE taken lp " + std::to_string(dae_lp));
    check(std::fabs(gradient[0] + 1.0) < 1e-8, "DAE taken gate gradient");
    check(std::fabs(gradient[1]) < 1e-7, "DAE taken initial gradient");
    check(std::fabs(gradient[2] + 2.6) < 1e-7, "DAE taken rate gradient");
  }

  // Modern forward ODE times are AutoDiffable in Stan. Runtime-valued t0 and
  // ts therefore travel as kernel operands, sharing the same graph/Program
  // call in ordinary expressions and parameter-dependent control flow.
  {
    CompiledModel om = compile_model(
        slurp("tests/fixtures/ode_active_times.tmir.sexp"), DataMap());
    check(om.n_unconstrained == 5, "active-time ODE parameter width");
    check(count_opcode(om, OP_ISLAND) > 0, "active-time ODE runtime island");
    Executor oex(std::move(om.graph));
    om.bind(oex);
    double gradient[5] = {};
    const double duration = std::log(0.3);

    oex.params_data()[0] = -1.0;
    oex.params_data()[1] = 2.0;
    oex.params_data()[2] = 0.1;
    oex.params_data()[3] = duration;
    oex.params_data()[4] = 3.0;
    double lp = oex.gradient(gradient);
    const double prior = -0.5 * (1.0 + 4.0 + 0.01 + duration * duration + 9.0);
    check(std::fabs(lp - (prior + 2.9)) < 1e-8, "active-time ODE untaken lp");
    check(std::fabs(gradient[0] - 1.0) < 1e-7,
          "active-time ODE untaken gate gradient");
    check(std::fabs(gradient[1] + 1.0) < 1e-7,
          "active-time ODE untaken initial gradient");
    check(std::fabs(gradient[2] + 0.1) < 1e-7,
          "active-time ODE untaken translated-time gradient");
    check(std::fabs(gradient[3] - (-duration + 0.9)) < 1e-6,
          "active-time ODE untaken duration gradient");
    check(std::fabs(gradient[4] + 2.7) < 1e-6,
          "active-time ODE untaken rate gradient");

    oex.params_data()[0] = 1.0;
    lp = oex.gradient(gradient);
    check(std::fabs(lp - (prior + 5.8)) < 1e-8, "active-time ODE taken lp");
    check(std::fabs(gradient[0] + 1.0) < 1e-7,
          "active-time ODE taken gate gradient");
    check(std::fabs(gradient[1]) < 1e-7,
          "active-time ODE taken initial gradient");
    check(std::fabs(gradient[2] + 0.1) < 1e-7,
          "active-time ODE taken translated-time gradient");
    check(std::fabs(gradient[3] - (-duration + 1.8)) < 1e-6,
          "active-time ODE taken duration gradient");
    check(std::fabs(gradient[4] + 2.4) < 1e-6,
          "active-time ODE taken rate gradient");
  }

  // Preserve Stan's mixed scalar instantiations too: making an inactive time
  // a var changes the coupled adaptive system, so t0-only and ts-only calls
  // have distinct activity bits rather than one combined "time" bit.
  {
    CompiledModel om = compile_model(
        slurp("tests/fixtures/ode_time_activity.tmir.sexp"), DataMap());
    Executor oex(std::move(om.graph));
    om.bind(oex);
    const double start_log = std::log(0.3);
    const double finish_log = std::log(0.4);
    oex.params_data()[0] = start_log;
    oex.params_data()[1] = finish_log;
    double gradient[2] = {};
    const double lp = oex.gradient(gradient);
    const double want_lp =
        -0.5 * (start_log * start_log + finish_log * finish_log) + 0.7;
    check(std::fabs(lp - want_lp) < 1e-8, "mixed-time ODE lp");
    check(std::fabs(gradient[0] - (-start_log + 0.3)) < 1e-6,
          "t0-only ODE gradient");
    check(std::fabs(gradient[1] - (-finish_log + 0.4)) < 1e-6,
          "ts-only ODE gradient");
  }

  // The CVODES adjoint frontend uses the same retained callback packing in an
  // ordinary expression and inside runtime control. Its reverse kernel runs
  // one weighted adjoint solve and propagates y0, t0, ts, and callback args.
  {
    CompiledModel am = compile_model(
        slurp("tests/fixtures/ode_adjoint_region.tmir.sexp"), DataMap());
    check(am.n_unconstrained == 5, "adjoint ODE parameter width");
    int adjoint_calls = 0;
    for (const Op& op : am.graph.ops) {
      if (op.opcode != OP_ISLAND) continue;
      const auto* program = static_cast<const IslandProg*>(op.udata);
      for (const Program::Call& call : program->calls) {
        if (call.opcode != OP_ODE_ADJOINT) continue;
        ++adjoint_calls;
        const auto* spec =
            static_cast<const OdeAdjointSpec*>(call.udata_owner.get());
        check(spec != nullptr && spec->prog.ok,
              "adjoint ODE retained callback register program");
      }
    }
    check(adjoint_calls == 2, "adjoint ODE shared kernel calls");
    Executor aex(std::move(am.graph));
    am.bind(aex);
    double gradient[5] = {};
    aex.params_data()[0] = -1.0;
    aex.params_data()[1] = 2.0;
    aex.params_data()[2] = 0.1;
    aex.params_data()[3] = 0.4;
    aex.params_data()[4] = 3.0;

    const double value_only = aex.forward_value_only();
    check(std::fabs(value_only + 4.185) < 1e-8,
          "adjoint ODE value-only lp " + std::to_string(value_only));
    double lp = aex.gradient(gradient);
    check(std::fabs(lp + 4.185) < 1e-8,
          "adjoint ODE untaken lp " + std::to_string(lp));
    check(std::fabs(gradient[0] - 1.0) < 1e-7,
          "adjoint ODE untaken gate gradient");
    check(std::fabs(gradient[1] + 1.0) < 1e-7,
          "adjoint ODE untaken y0 gradient");
    check(std::fabs(gradient[2] + 3.1) < 1e-6,
          "adjoint ODE untaken t0 gradient");
    check(std::fabs(gradient[3] - 2.6) < 1e-6,
          "adjoint ODE untaken ts gradient");
    check(std::fabs(gradient[4] + 2.7) < 1e-6,
          "adjoint ODE untaken callback gradient");

    aex.params_data()[0] = 1.0;
    lp = aex.gradient(gradient);
    check(std::fabs(lp + 1.285) < 1e-8,
          "adjoint ODE taken lp " + std::to_string(lp));
    check(std::fabs(gradient[0] + 1.0) < 1e-7,
          "adjoint ODE taken gate gradient");
    check(std::fabs(gradient[1]) < 1e-7, "adjoint ODE taken y0 gradient");
    check(std::fabs(gradient[2] + 6.1) < 1e-6, "adjoint ODE taken t0 gradient");
    check(std::fabs(gradient[3] - 5.6) < 1e-6, "adjoint ODE taken ts gradient");
    check(std::fabs(gradient[4] + 2.4) < 1e-6,
          "adjoint ODE taken callback gradient");
  }

  // solve_*_tol controls reach Stan Math. One Powell evaluation cannot reach
  // the root of x^2 - 4 from x = 1, so max_num_steps = 1 raises the solver's
  // iteration-limit error in both the value and the gradient pass.
  {
    CompiledModel tm = compile_model(
        slurp("tests/fixtures/algebra_tol_controls.tmir.sexp"), DataMap());
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    tex.params_data()[0] = 4.0;
    double gradient[1] = {};
    bool value_threw = false;
    try {
      (void)tex.forward_value_only();
    } catch (const std::domain_error&) {
      value_threw = true;
    }
    check(value_threw, "solve_powell_tol max_num_steps in the value pass");
    bool gradient_threw = false;
    try {
      (void)tex.gradient(gradient);
    } catch (const std::domain_error&) {
      gradient_threw = true;
    }
    check(gradient_threw,
          "solve_powell_tol max_num_steps in the gradient pass");
  }

  // A one-element vector produced by a kernel call inside runtime control
  // receives its adjoint the same way the graph delivers every one-element
  // output.
  {
    CompiledModel vm = compile_model(
        slurp("tests/fixtures/region_vector1_binary.tmir.sexp"), DataMap());
    check(count_opcode(vm, OP_ISLAND) > 0, "vector[1] region island");
    Executor vex(std::move(vm.graph));
    vm.bind(vex);
    double gradient[1] = {};

    vex.params_data()[0] = -1.0;
    expect_eq("vector[1] region untaken lp", vex.gradient(gradient), -0.5);
    expect_eq("vector[1] region untaken gradient", gradient[0], 1.0);

    vex.params_data()[0] = 4.0;
    const double lp = vex.gradient(gradient);
    check(std::fabs(lp + 3.0) < 1e-12, "vector[1] region taken lp");
    check(std::fabs(gradient[0] + 3.2) < 1e-12,
          "vector[1] region taken gradient " + std::to_string(gradient[0]));
  }

  // tests/fixtures/infbounds.stan: infinite bounds on the declarations
  // themselves. An infinite bound is no bound -- the element is the
  // identity and adds no jacobian term -- and the kernels used to
  // exponentiate through it, so every one of these landed on inf and lp
  // came out -inf. The vector bounds mix infinite and finite entries, so
  // the elements that do transform are checked in the same pass.
  {
    CompiledModel im =
        compile_model(slurp("tests/fixtures/infbounds.tmir.sexp"), DataMap());
    Executor iex(std::move(im.graph));
    im.bind(iex);
    // Declaration order: a[4], b[4], c[4], d, e.
    check(iex.n_params() == 14, "infbounds unconstrained size is 14");
    const double pts[2][14] = {{0.4, -0.7, 1.25, -0.2, 0.9, -1.1, 0.35, 2.0,
                                -0.45, 0.6, 1.8, -1.6, 0.7, -0.9},
                               {-0.3, 0.9, -0.45, 1.4, 0.15, 0.5, -2.1, 0.8,
                                1.05, -0.75, 0.25, 1.9, -0.55, 0.3}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 14; ++i) iex.params_data()[i] = pts[c][i];
      double grad[14] = {0};
      const double lp = iex.gradient(grad);

      using stan::math::var;
      using Vec = Eigen::Matrix<var, -1, 1>;
      const double ninf = -std::numeric_limits<double>::infinity();
      const double pinf = std::numeric_limits<double>::infinity();
      Eigen::VectorXd lo(4), hi(4);
      lo << ninf, -0.5, ninf, 0.75;
      hi << 3.25, pinf, pinf, 4.5;
      Vec a(4), b(4), cc(4);
      for (int i = 0; i < 4; ++i) {
        a(i) = pts[c][i];
        b(i) = pts[c][4 + i];
        cc(i) = pts[c][8 + i];
      }
      var d = pts[c][12], e = pts[c][13];
      // Each `sum` is one ascending reduction over its own result, and
      // OP_SUM_VEC accumulates the same way.
      const auto vsum = [](const auto& v) {
        var t = 0;
        for (int i = 0; i < v.size(); ++i) t += v(i);
        return t;
      };
      var jac = 0;
      var acc = 0;
      acc += vsum(stan::math::lb_constrain<true>(a, lo, jac));
      acc += vsum(stan::math::ub_constrain<true>(b, hi, jac));
      acc += vsum(stan::math::lub_constrain<true>(cc, lo, hi, jac));
      acc += stan::math::lb_constrain<true>(d, ninf, jac);
      acc += stan::math::ub_constrain<true>(e, pinf, jac);
      acc += jac;
      acc.grad();

      check(std::isfinite(lp), "infbounds: lp is finite");
      for (int i = 0; i < 4; ++i)
        expect_ulp("infbounds a grad", grad[i], a(i).adj());
      for (int i = 0; i < 4; ++i)
        expect_ulp("infbounds b grad", grad[4 + i], b(i).adj());
      for (int i = 0; i < 4; ++i)
        expect_ulp("infbounds c grad", grad[8 + i], cc(i).adj());
      expect_ulp("infbounds d grad", grad[12], d.adj());
      expect_ulp("infbounds e grad", grad[13], e.adj());
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol,
            "infbounds: lp matches the var path");
      stan::math::recover_memory();
    }
    // The identity elements have to reach write_array untransformed too:
    // an unbounded declaration writes its unconstrained value straight out.
    check(im.write_array && im.write_array->truncated.empty(),
          "infbounds write_array compiled");
    if (im.write_array && im.write_array->truncated.empty()) {
      Executor wex(std::move(im.write_array->graph));
      im.write_array->bind(wex);
      for (int i = 0; i < 14; ++i) wex.params_data()[i] = pts[0][i];
      wex.run_forward_only();
      int found = 0;
      for (const auto& col : im.write_array->columns) {
        // a[1] and a[3] carry the -inf lower bound, d the shared one.
        if (col.name == "a") {
          ++found;
          expect_ulp("infbounds wa a1", wex.value_ptr(col.slot)[0], pts[0][0]);
          expect_ulp("infbounds wa a3", wex.value_ptr(col.slot)[2], pts[0][2]);
        }
        if (col.name == "d") {
          ++found;
          expect_ulp("infbounds wa d", *wex.value_ptr(col.slot), pts[0][12]);
        }
      }
      check(found == 2, "infbounds write_array has a and d");
    }
  }

  // Every multivariate density with the (vector-or-array-of-vectors, ...,
  // square matrix) signature gets vectorized y/mu handling from one registry
  // policy. Exercise all five users through both graph lowering and the
  // runtime-control CALL bridge; the fixture contributes each term once in
  // each context.
  {
    DataMap d = DataMap::from_json(R"({
      "N": 2, "K": 2,
      "y": [[1.0, -0.5], [0.3, 1.2]],
      "Sigma": [[1.44, 0.24], [0.24, 0.68]],
      "precision": [[0.7378472222222222, -0.2604166666666667],
                    [-0.2604166666666667, 1.5625]],
      "L": [[1.2, 0.0], [0.2, 0.8]],
      "nu": 4.5
    })");
    CompiledModel model = compile_model(
        slurp("tests/fixtures/mvt_vectorized_inputs.tmir.sexp"), d);
    Executor executor(std::move(model.graph));
    model.bind(executor);
    const double point[4] = {0.2, -0.4, -0.1, 0.7};
    std::copy(point, point + 4, executor.params_data());
    double gradient[4] = {};
    const double got = executor.gradient(gradient);

    using stan::math::var;
    std::vector<Eigen::VectorXd> y(2, Eigen::VectorXd(2));
    y[0] << 1.0, -0.5;
    y[1] << 0.3, 1.2;
    std::vector<Eigen::Matrix<var, Eigen::Dynamic, 1>> mu(
        2, Eigen::Matrix<var, Eigen::Dynamic, 1>(2));
    for (int k = 0; k < 2; ++k)
      for (int i = 0; i < 2; ++i) mu[(size_t)k](i) = point[k * 2 + i];
    Eigen::MatrixXd L(2, 2);
    L << 1.2, 0.0, 0.2, 0.8;
    const Eigen::MatrixXd Sigma = L * L.transpose();
    const Eigen::MatrixXd precision = Sigma.inverse();
    var one = stan::math::multi_normal_lpdf<false>(y, mu, Sigma) +
              stan::math::multi_normal_prec_lpdf<false>(y, mu, precision) +
              stan::math::multi_normal_cholesky_lpdf<false>(y, mu, L) +
              stan::math::multi_student_t_lpdf<false>(y, 4.5, mu, Sigma) +
              stan::math::multi_student_t_cholesky_lpdf<false>(y, 4.5, mu, L);
    var want = 2.0 * one;
    want.grad();
    const auto near = [](double a, double b) {
      return std::abs(a - b) <= 64 * std::numeric_limits<double>::epsilon() *
                                    std::max(1.0, std::abs(b));
    };
    check(near(got, want.val()), "shared vectorized-density value");
    for (int k = 0; k < 2; ++k)
      for (int i = 0; i < 2; ++i)
        check(near(gradient[k * 2 + i], mu[(size_t)k](i).adj()),
              "shared vectorized-density gradient");
    stan::math::recover_memory();
  }

  // array[N] vector[K] data into a vectorized multivariate density. See
  // tests/fixtures/mnarr.stan: the data path stores this shape the way it
  // stores a matrix, and the kernel wants each element contiguous, so the
  // slot has to be repacked. Wrong is silent, and no corpus model has the
  // shape.
  {
    // Logical y = {{1,2},{3,4},{5,6}}, stored first index fastest, which
    // is what the JSON reader produces for [[1,2],[3,4],[5,6]].
    DataMap d;
    d.set_int("N", 3);
    d.set_int("K", 2);
    d.set_real_array("y", {1, 3, 5, 2, 4, 6}, {3, 2});
    d.set_real_array("Sigma", {2.0, 0.5, 0.5, 1.0}, {2, 2});
    CompiledModel am =
        compile_model(slurp("tests/fixtures/mnarr.tmir.sexp"), d);
    Executor aex(std::move(am.graph));
    am.bind(aex);
    const double pts[2][2] = {{0.4, -0.7}, {-0.3, 0.9}};
    for (int c = 0; c < 2; ++c) {
      aex.params_data()[0] = pts[c][0];
      aex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = aex.gradient(grad);

      using stan::math::var;
      // y and Sigma are data, mu is a parameter: the same instantiation
      // the kernel picks, so this is exact rather than merely close.
      std::vector<Eigen::VectorXd> ys(3, Eigen::VectorXd(2));
      ys[0] << 1, 2;
      ys[1] << 3, 4;
      ys[2] << 5, 6;
      Eigen::MatrixXd Sd(2, 2);
      Sd << 2.0, 0.5, 0.5, 1.0;
      Eigen::Matrix<var, Eigen::Dynamic, 1> mu(2);
      mu(0) = pts[c][0];
      mu(1) = pts[c][1];
      var acc = stan::math::multi_normal_lpdf<true>(ys, mu, Sd);
      acc.grad();

      check(grad[0] == mu(0).adj() && grad[1] == mu(1).adj(),
            "mnarr: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "mnarr: lp matches the var path");
    }
  }

  // An array-valued location, which set_rescor(TRUE) multivariate brms
  // models emit, and the same location broadcast against a single random
  // variable.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_int("K", 2);
    d.set_real_array("y", {1, 3, 5, 2, 4, 6}, {3, 2});
    d.set_real_array("y1", {0.5, -1.0}, {2});
    d.set_real_array("L", {1.2, 0.4, 0.0, 0.9}, {2, 2});
    CompiledModel am =
        compile_model(slurp("tests/fixtures/mnarrmu.tmir.sexp"), d);
    check(am.n_unconstrained == 7, "mnarrmu 7 unconstrained");
    Executor aex(std::move(am.graph));
    am.bind(aex);
    const double q[7] = {0.4, -0.7, -0.3, 0.9, 0.15, -0.55, 0.2};
    for (int k = 0; k < 7; ++k) aex.params_data()[k] = q[k];
    double grad[7];
    const double lp = aex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> qv(7);
    for (int k = 0; k < 7; ++k) qv(k) = q[k];
    var jac = 0.0;
    std::vector<Eigen::Matrix<var, -1, 1>> mus(3, Eigen::Matrix<var, -1, 1>(2));
    for (int n = 0; n < 3; ++n)
      for (int i = 0; i < 2; ++i) mus[(size_t)n](i) = qv(n * 2 + i);
    var s = stan::math::lb_constrain<true>(qv(6), 0.0, jac);
    Eigen::MatrixXd Ld(2, 2);
    Ld << 1.2, 0.0, 0.4, 0.9;
    Eigen::Matrix<var, -1, -1> Ls = s * Ld;
    std::vector<Eigen::VectorXd> ys(3, Eigen::VectorXd(2));
    ys[0] << 1, 2;
    ys[1] << 3, 4;
    ys[2] << 5, 6;
    Eigen::VectorXd y1(2);
    y1 << 0.5, -1.0;
    var acc = stan::math::multi_normal_cholesky_lpdf<false>(ys, mus, Ls) +
              stan::math::multi_normal_cholesky_lpdf<false>(y1, mus, Ls) + jac;
    acc.grad();
    expect_ulp("mnarrmu lp", lp, acc.val());
    for (int k = 0; k < 7; ++k)
      expect_ulp("mnarrmu g" + std::to_string(k), grad[k], qv(k).adj());
    stan::math::recover_memory();
  }

  // multi_student_t shares the shape derivation and, through the encoded
  // per-argument layout, the array-location kernel path: stan-math's
  // vectorized overload takes the array of locations directly.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_int("K", 2);
    d.set_real_array("y", {1, 3, 5, 2, 4, 6}, {3, 2});
    d.set_real_array("S", {2.0, 0.5, 0.5, 1.0}, {2, 2});
    CompiledModel am =
        compile_model(slurp("tests/fixtures/mstarrmu.tmir.sexp"), d);
    check(am.n_unconstrained == 6, "mstarrmu 6 unconstrained");
    Executor aex(std::move(am.graph));
    am.bind(aex);
    const double q[6] = {0.4, -0.7, -0.3, 0.9, 0.15, -0.55};
    for (int k = 0; k < 6; ++k) aex.params_data()[k] = q[k];
    double grad[6];
    const double lp = aex.gradient(grad);

    using stan::math::var;
    std::vector<Eigen::Matrix<var, -1, 1>> mus(3, Eigen::Matrix<var, -1, 1>(2));
    for (int n = 0; n < 3; ++n)
      for (int i = 0; i < 2; ++i) mus[(size_t)n](i) = q[n * 2 + i];
    std::vector<Eigen::VectorXd> ys(3, Eigen::VectorXd(2));
    ys[0] << 1, 2;
    ys[1] << 3, 4;
    ys[2] << 5, 6;
    Eigen::MatrixXd Sd(2, 2);
    Sd << 2.0, 0.5, 0.5, 1.0;
    var acc = stan::math::multi_student_t_lpdf<false>(ys, 3.0, mus, Sd);
    acc.grad();
    expect_ulp("mstarrmu lp", lp, acc.val());
    for (int k = 0; k < 6; ++k)
      expect_ulp("mstarrmu g" + std::to_string(k), grad[k],
                 mus[(size_t)(k / 2)](k % 2).adj());
    stan::math::recover_memory();
  }

  // `p ~ dirichlet(a)` over an array of simplexes. See
  // tests/fixtures/dirvec.stan: the kernel read the whole slot as one
  // theta, so the vectorized form threw on a length mismatch and only the
  // explicit loop worked.
  {
    // Logical p = {{0.3,0.7},{0.4,0.6},{0.2,0.8}}, first index fastest.
    DataMap d;
    d.set_int("N", 3);
    d.set_int("K", 2);
    d.set_real_array("p", {0.3, 0.4, 0.2, 0.7, 0.6, 0.8}, {3, 2});
    CompiledModel dm =
        compile_model(slurp("tests/fixtures/dirvec.tmir.sexp"), d);
    Executor dex(std::move(dm.graph));
    dm.bind(dex);
    const double pts[2][2] = {{0.3, -0.4}, {-0.6, 0.8}};
    for (int c = 0; c < 2; ++c) {
      dex.params_data()[0] = pts[c][0];
      dex.params_data()[1] = pts[c][1];
      double grad[2] = {0, 0};
      const double lp = dex.gradient(grad);

      using stan::math::var;
      var u0 = pts[c][0], u1 = pts[c][1];
      Eigen::Matrix<var, Eigen::Dynamic, 1> alpha(2);
      alpha(0) = stan::math::exp(u0);
      alpha(1) = stan::math::exp(u1);
      std::vector<Eigen::VectorXd> th(3, Eigen::VectorXd(2));
      th[0] << 0.3, 0.7;
      th[1] << 0.4, 0.6;
      th[2] << 0.2, 0.8;
      // lower=0 jacobian on both elements of a, then the vectorized lpdf.
      var acc = u0 + u1;
      acc += stan::math::dirichlet_lpdf<true>(th, alpha);
      acc.grad();

      check(grad[0] == u0.adj() && grad[1] == u1.adj(),
            "dirvec: gradients bitwise against the var path");
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "dirvec: lp matches the var path");
    }
  }

  // Every shape 0.4.0 got wrong, in one model and one gradient. See
  // tests/fixtures/shapes.stan. The three fixtures above each pin one
  // construct and name the fix that broke; this one shares a graph, a data
  // block and a parameter vector between them, so it is the check that one
  // construct does not disturb another. All nine gradients have to come out
  // for it to pass.
  {
    DataMap d;
    d.set_int("N", 3);
    d.set_int("K", 2);
    d.set_real_array("t", {0.5, 1.75, 2.25});
    // Both 2-D data blocks are stored first index fastest, which is what
    // the JSON reader produces: y = {{1,2},{3,4},{5,6}} and
    // p = {{0.3,0.7},{0.4,0.6},{0.2,0.8}}.
    d.set_real_array("y", {1, 3, 5, 2, 4, 6}, {3, 2});
    d.set_real_array("p", {0.3, 0.4, 0.2, 0.7, 0.6, 0.8}, {3, 2});
    d.set_real_array("Sigma", {2.0, 0.5, 0.5, 1.0}, {2, 2});
    CompiledModel sm =
        compile_model(slurp("tests/fixtures/shapes.tmir.sexp"), d);
    Executor sex(std::move(sm.graph));
    sm.bind(sex);
    // Declaration order: mu, theta[3], sigma, m[2], a[2].
    const double pts[2][9] = {
        {0.35, -0.2, 0.4, 0.15, -0.3, 0.5, -0.45, 0.25, -0.1},
        {-0.4, 0.55, -0.15, 0.3, 0.2, -0.35, 0.6, -0.2, 0.45}};
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < 9; ++i) sex.params_data()[i] = pts[c][i];
      double grad[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
      const double lp = sex.gradient(grad);

      using stan::math::var;
      var u_mu = pts[c][0], u_sig = pts[c][4];
      var au0 = pts[c][7], au1 = pts[c][8];
      Eigen::Matrix<var, Eigen::Dynamic, 1> theta(3), m(2), alpha(2);
      for (int i = 0; i < 3; ++i) theta(i) = pts[c][1 + i];
      m(0) = pts[c][5];
      m(1) = pts[c][6];
      var mu = u_mu, sigma = stan::math::exp(u_sig);
      alpha(0) = stan::math::exp(au0);
      alpha(1) = stan::math::exp(au1);

      Eigen::VectorXd t(3);
      t << 0.5, 1.75, 2.25;
      std::vector<Eigen::VectorXd> ys(3, Eigen::VectorXd(2));
      ys[0] << 1, 2;
      ys[1] << 3, 4;
      ys[2] << 5, 6;
      std::vector<Eigen::VectorXd> ps(3, Eigen::VectorXd(2));
      ps[0] << 0.3, 0.7;
      ps[1] << 0.4, 0.6;
      ps[2] << 0.2, 0.8;
      Eigen::MatrixXd Sd(2, 2);
      Sd << 2.0, 0.5, 0.5, 1.0;

      // Jacobians in declaration order, then the four statements in the
      // order the model writes them.
      var acc = u_sig + au0 + au1;
      acc += stan::math::normal_lpdf<true>(t, mu, sigma);
      acc -= 3.0 *
             stan::math::log_diff_exp(stan::math::normal_lcdf(10.0, mu, sigma),
                                      stan::math::normal_lcdf(0.0, mu, sigma));
      acc += stan::math::normal_lpdf<true>(t, theta, 1.0);
      for (int i = 0; i < 3; ++i)
        acc -= stan::math::log_diff_exp(
            stan::math::normal_lcdf(10.0, theta(i), 1.0),
            stan::math::normal_lcdf(0.0, theta(i), 1.0));
      acc += stan::math::multi_normal_lpdf<true>(ys, m, Sd);
      acc += stan::math::dirichlet_lpdf<true>(ps, alpha);
      acc.grad();

      const double want[9] = {u_mu.adj(),     theta(0).adj(), theta(1).adj(),
                              theta(2).adj(), u_sig.adj(),    m(0).adj(),
                              m(1).adj(),     au0.adj(),      au1.adj()};
      // Eight of the nine are bitwise. d/d_mu carries the truncation
      // normalizer, which the graph computes as one scaled log_diff_exp
      // where this reference and CmdStan both accumulate, so it lands
      // inside the 2 ULP budget rather than on it. The failure this test
      // exists for is not subtle: permuting the observations against the
      // components moves a gradient by whole digits.
      for (int i = 0; i < 9; ++i)
        expect_ulp("shapes g" + std::to_string(i), grad[i], want[i]);
      const double tol = 8 * 2.220446049250313e-16 * std::abs(acc.val());
      check(std::abs(lp - acc.val()) <= tol, "shapes: lp matches the var path");
    }
  }

  // Ordinal regression, against an independent var-path reference. Same
  // reason as the truncation case: CI has no CmdStan, and this is the one
  // density whose cutpoint argument is a shared vector rather than a
  // per-lane value, so it is the only check that VecMask and the
  // vector-of-vectors partials edge stay wired.
  {
    DataMap d;
    d.set_int("N", 4);
    d.set_int("K", 4);
    d.set_int_array("y", {1, 3, 2, 4});
    CompiledModel om =
        compile_model(slurp("tests/fixtures/ordlog.tmir.sexp"), d);
    Executor oex(std::move(om.graph));
    om.bind(oex);
    // 4 lambdas + 3 unconstrained cutpoints.
    const double q[7] = {0.3, -0.2, 0.55, -0.4, -0.5, 0.2, 0.1};
    for (int i = 0; i < 7; ++i) oex.params_data()[i] = q[i];
    double grad[7] = {0, 0, 0, 0, 0, 0, 0};
    const double lp = oex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> lambda(4);
    for (int i = 0; i < 4; ++i) lambda(i) = q[i];
    // ordered[3] from the unconstrained triple, with its jacobian.
    Eigen::Matrix<var, -1, 1> c(3);
    var acc = 0;
    c(0) = q[4];
    for (int k = 1; k < 3; ++k) {
      c(k) = c(k - 1) + stan::math::exp(var(q[4 + k]));
      acc += q[4 + k];
    }
    const std::vector<int> y{1, 3, 2, 4};
    acc += stan::math::normal_lpdf<true>(lambda, 0, 2);
    acc += stan::math::ordered_logistic_lpmf<true>(y, lambda, c);
    acc.grad();

    bool grads_ok = true;
    for (int i = 0; i < 4; ++i)
      if (grad[i] != lambda(i).adj()) grads_ok = false;
    check(grads_ok, "ordered_logistic: lambda gradients bitwise");
    check(std::abs(lp - acc.val()) <=
              8 * 2.220446049250313e-16 * std::abs(acc.val()),
          "ordered_logistic: lp matches the var path");
  }

  // The GLM fast paths. fn_sweep cannot reach these -- it generates
  // all-scalar models and a GLM needs a data matrix -- so this is the only
  // check that their propto bit and activity mask arrive.
  {
    // Through the JSON reader, so the data-matrix layout convention is
    // part of what this checks: x[i][j] is row i, column j, and the
    // reference below fills X the same way.
    DataMap d = DataMap::from_json(
        R"({"N": 4, "K": 2,
            "x": [[0.3, -0.2], [1.1, 0.4], [-0.5, 0.9], [0.2, 0.7]],
            "y": [2, 5, 1, 3]})");
    CompiledModel gm =
        compile_model(slurp("tests/fixtures/glmpois.tmir.sexp"), d);
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    const double q[4] = {0.15, 0.3, -0.25, -0.4};  // alpha, beta[2], log phi
    for (int i = 0; i < 4; ++i) gex.params_data()[i] = q[i];
    double grad[4] = {0, 0, 0, 0};
    const double lp = gex.gradient(grad);

    using stan::math::var;
    var alpha = q[0];
    Eigen::Matrix<var, -1, 1> beta(2);
    beta(0) = q[1];
    beta(1) = q[2];
    var u_phi = q[3], phi = stan::math::exp(u_phi);
    Eigen::MatrixXd X(4, 2);
    X << 0.3, -0.2, 1.1, 0.4, -0.5, 0.9, 0.2, 0.7;
    const std::vector<int> y{2, 5, 1, 3};
    var acc = u_phi;  // lower=0 jacobian on phi
    acc += stan::math::normal_lpdf<true>(alpha, 0, 2);
    acc += stan::math::normal_lpdf<true>(beta, 0, 2);
    acc += stan::math::exponential_lpdf<true>(phi, 1);
    acc += stan::math::poisson_log_glm_lpmf<true>(y, X, alpha, beta);
    acc +=
        stan::math::neg_binomial_2_log_glm_lpmf<true>(y, X, alpha, beta, phi);
    acc.grad();

    const bool g_ok = grad[0] == alpha.adj() && grad[1] == beta(0).adj() &&
                      grad[2] == beta(1).adj() && grad[3] == u_phi.adj();
    check(g_ok, "glm: gradients bitwise against the var path");
    check(std::abs(lp - acc.val()) <=
              8 * 2.220446049250313e-16 * std::abs(acc.val()),
          "glm: lp matches the var path (propto reached the kernel)");
  }

  // A GLM whose outcome is a language-level scalar rather than an array of
  // one value per row. Scalar-vs-array is retained in the shared GLM payload:
  // merely replicating the value changes poisson_log_glm's <false> constant.
  {
    DataMap d = DataMap::from_json(
        R"({"N": 4, "K": 2,
            "x": [[0.3, -0.2], [1.1, 0.4], [-0.5, 0.9], [0.2, 0.7]],
            "y": 3})");
    CompiledModel gm =
        compile_model(slurp("tests/fixtures/glmscalary.tmir.sexp"), d);
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    const double q[3] = {0.15, 0.3, -0.25};
    std::copy(q, q + 3, gex.params_data());
    double grad[3] = {};
    const double lp = gex.gradient(grad);

    using stan::math::var;
    var alpha = q[0];
    Eigen::Matrix<var, -1, 1> beta(2);
    beta << q[1], q[2];
    Eigen::MatrixXd X(4, 2);
    X << 0.3, -0.2, 1.1, 0.4, -0.5, 0.9, 0.2, 0.7;
    var want = stan::math::poisson_log_glm_lpmf<false>(3, X, alpha, beta);
    want.grad();
    check(lp == want.val(), "glm scalar outcome value");
    check(grad[0] == alpha.adj() && grad[1] == beta(0).adj() &&
              grad[2] == beta(1).adj(),
          "glm scalar outcome gradients");
    stan::math::recover_memory();
  }

  // normal_id_glm with the OUTCOME as a parameter. stanc3's --O1 partial
  // evaluator rewrites `theta ~ normal(x * b, 1)` into
  // normal_id_glm_lupdf(theta | x, 0, b, 1), a shape the source language
  // never hands the kernel directly: y is the thing being sampled, so its
  // gradient must come back too, not only alpha/beta/sigma's.
  {
    DataMap d = DataMap::from_json(
        R"({"N": 3, "K": 2,
            "x": [[1.0, 2.0], [0.5, -1.0], [2.0, 0.25]]})");
    CompiledModel gm = compile_model(slurp("tests/fixtures/glmy.tmir.sexp"), d);
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    const double q[5] = {0.3, -0.2, 1.1, 0.7, -0.4};  // theta[3], b[2]
    for (int i = 0; i < 5; ++i) gex.params_data()[i] = q[i];
    double grad[5] = {0, 0, 0, 0, 0};
    const double lp = gex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> theta(3), b(2);
    for (int i = 0; i < 3; ++i) theta(i) = q[i];
    for (int i = 0; i < 2; ++i) b(i) = q[3 + i];
    Eigen::MatrixXd X(3, 2);
    X << 1.0, 2.0, 0.5, -1.0, 2.0, 0.25;
    var acc = stan::math::normal_id_glm_lpdf<true>(theta, X, 0, b, 1);
    acc.grad();

    bool g_ok = true;
    for (int i = 0; i < 3; ++i)
      if (grad[i] != theta(i).adj()) g_ok = false;
    for (int i = 0; i < 2; ++i)
      if (grad[3 + i] != b(i).adj()) g_ok = false;
    check(g_ok, "glm param y: gradients bitwise against the var path");
    check(lp == acc.val(), "glm param y: lp matches the var path");
  }

  // An inlined user function's return variable: --O1 declares it
  // zero-length (`vector[0] inline_..._return_sym__`) and sizes it by
  // assignment, so the lowering adopts the assigned shape instead of
  // rejecting the width mismatch.
  {
    DataMap d = DataMap::from_json(R"({"N": 4, "y": [1.0, 2.0, 4.0, -0.5]})");
    CompiledModel im =
        compile_model(slurp("tests/fixtures/inlret.tmir.sexp"), d);
    Executor iex(std::move(im.graph));
    im.bind(iex);
    iex.params_data()[0] = 0.4;  // mu
    double grad[1] = {0};
    const double lp = iex.gradient(grad);

    using stan::math::var;
    var mu = 0.4;
    Eigen::VectorXd y(4);
    y << 1.0, 2.0, 4.0, -0.5;
    Eigen::VectorXd c = y.array() - y.mean();
    var acc = stan::math::normal_lpdf<false>(c, mu, 1);
    acc.grad();
    const double tol = 64 * 2.220446049250313e-16;
    check(std::abs(lp - acc.val()) <= tol * std::abs(acc.val()),
          "inlined return: lp matches the var path");
    check(
        std::abs(grad[0] - mu.adj()) <= tol * std::max(1.0, std::abs(mu.adj())),
        "inlined return: gradient matches the var path");
  }

  // A declaration sized by a shape query on a COMPUTED value:
  // `vector[rows(segment(beta, 1, 2)) + 1]` after --O1 inlines a callee
  // and substitutes the call argument into its size expressions.
  {
    DataMap d = DataMap::from_json(R"({"N": 3})");
    CompiledModel sm =
        compile_model(slurp("tests/fixtures/inlseg.tmir.sexp"), d);
    Executor sex(std::move(sm.graph));
    sm.bind(sex);
    const double q[3] = {0.6, -0.3, 0.2};
    for (int i = 0; i < 3; ++i) sex.params_data()[i] = q[i];
    double grad[3] = {0, 0, 0};
    const double lp = sex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> beta(3);
    for (int i = 0; i < 3; ++i) beta(i) = q[i];
    Eigen::Matrix<var, -1, 1> u(3);
    u << 0.0, beta(0), beta(1);
    var acc = stan::math::sum(stan::math::cumulative_sum(u));
    acc += stan::math::normal_lpdf<true>(beta, 0, 1);
    acc.grad();
    const double tol = 64 * 2.220446049250313e-16;
    check(std::abs(lp - acc.val()) <= tol * std::max(1.0, std::abs(acc.val())),
          "shape query on expression: lp matches the var path");
    bool g_ok = true;
    for (int i = 0; i < 3; ++i)
      if (std::abs(grad[i] - beta(i).adj()) >
          tol * std::max(1.0, std::abs(beta(i).adj())))
        g_ok = false;
    check(g_ok, "shape query on expression: gradients match the var path");
  }

  // A shape query on a scalar `int` input, alone on the right of a
  // real-valued assignment: `real p = size(n)`. eval_int answers rows/cols/
  // size from the graph's slot metadata or from the declaration table, and
  // bind_data fills both from a declared shape, which a scalar int does not
  // have -- so this is the one name that falls past both. tests/fixtures/
  // intsize.stan says what else has to line up to reach it. The lowering
  // used to refuse the whole model here, which is stanc3's
  // function-signatures/math/matrix/size.stan and nothing smaller.
  {
    DataMap d = DataMap::from_json(R"({"n": 7})");
    CompiledModel im =
        compile_model(slurp("tests/fixtures/intsize.tmir.sexp"), d);
    Executor iex(std::move(im.graph));
    im.bind(iex);
    iex.params_data()[0] = 0.4;
    double grad[1] = {0};
    const double lp = iex.gradient(grad);
    // size(n) is 1 for any scalar, and td_n is the interpreter's copy of
    // the same 1, so the mean is 2 -- not 7, and not 8.
    using stan::math::var;
    var y = 0.4;
    var acc = stan::math::normal_lpdf<true>(y, 2.0, 1);
    acc.grad();
    expect_eq("int size: lp", lp, acc.val());
    expect_eq("int size: gradient", grad[0], y.adj());
  }

  // Declaration extents may use data-only conditional expressions.  The
  // condition and only its selected arm are compile-time integer expressions;
  // compound conditions retain Stan's short-circuit semantics.
  {
    const std::string mir = slurp("tests/fixtures/conditional_size.tmir.sexp");

    DataMap on =
        DataMap::from_json(R"({"enabled": 1, "n": 2, "x": [[1, 2], [3, 4]]})");
    CompiledModel on_model = compile_model(mir, on);
    check(on_model.n_unconstrained == 4,
          "conditional size: true arms size parameters");

    DataMap off;
    off.set_int("enabled", 0);
    off.set_int("n", 2);
    off.set_real_array("x", {}, {0, 0});
    CompiledModel off_model = compile_model(mir, off);
    check(off_model.n_unconstrained == 1,
          "conditional size: false arms allow zero extents");
  }

  // Matrix functions used in transformed data run through MirInterp rather
  // than the graph kernels. diag_matrix must preserve column-major layout and
  // zero every off-diagonal entry on that path too; diagonal must extract a
  // rectangular matrix with the rows+1 column-major stride.
  {
    DataMap d;
    d.set_real_array("diagonal", {1.0, 2.0, 3.0}, {3});
    d.set_real_array("rectangular", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}, {3, 2});
    CompiledModel dm =
        compile_model(slurp("tests/fixtures/diagtd.tmir.sexp"), d);
    Executor dex(std::move(dm.graph));
    dm.bind(dex);
    dex.params_data()[0] = 0.4;
    double gradient[1] = {0.0};
    const double lp = dex.gradient(gradient);

    using stan::math::var;
    var theta = 0.4;
    var reference_lp = stan::math::normal_lpdf<true>(theta, 7.0, 1.0);
    reference_lp.grad();
    expect_eq("transformed-data diagonal: lp", lp, reference_lp.val());
    expect_eq("transformed-data diagonal: gradient", gradient[0], theta.adj());
  }

  // tcrossprod(A) is A * A'. It reuses the transpose and GEMM graph ops, so
  // this pins both the shared input's accumulated adjoint and matrix layout.
  {
    DataMap d;
    CompiledModel tm =
        compile_model(slurp("tests/fixtures/tcross.tmir.sexp"), d);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    const double q[6] = {0.2, -0.4, 0.7, 0.1, -0.3, 0.8};
    for (int i = 0; i < 6; ++i) tex.params_data()[i] = q[i];
    double gradient[6] = {0, 0, 0, 0, 0, 0};
    const double lp = tex.gradient(gradient);

    using stan::math::var;
    Eigen::Matrix<var, -1, -1> a(2, 3);
    for (int i = 0; i < 6; ++i) a(i) = q[i];
    Eigen::Matrix<var, -1, -1> gram = stan::math::tcrossprod(a);
    var reference_lp =
        stan::math::normal_lpdf<true>(stan::math::to_vector(a), 0, 1) +
        stan::math::normal_lpdf<true>(stan::math::to_vector(gram), 0, 1);
    reference_lp.grad();
    expect_eq("tcrossprod: lp", lp, reference_lp.val());
    for (int i = 0; i < 6; ++i)
      expect_ulp("tcrossprod: g" + std::to_string(i), gradient[i], a(i).adj());
  }

  // crossprod(A) is A' * A. As with tcrossprod, the shared matrix reaches
  // both the original density and the Gram matrix, which pins accumulated
  // gradients as well as the 3 x 3 column-major result layout.
  {
    DataMap d;
    const double observed[6] = {0.6, -0.2, 0.4, 0.9, -0.5, 0.3};
    d.set_real_array("d", std::vector<double>(observed, observed + 6), {2, 3});
    CompiledModel cm =
        compile_model(slurp("tests/fixtures/crossprod.tmir.sexp"), d);
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const double q[6] = {0.2, -0.4, 0.7, 0.1, -0.3, 0.8};
    for (int i = 0; i < 6; ++i) ex.params_data()[i] = q[i];
    double gradient[6] = {};
    const double lp = ex.gradient(gradient);
    double repeated_gradient[6] = {};
    const double repeated_lp = ex.gradient(repeated_gradient);
    expect_eq("crossprod: repeated lp", repeated_lp, lp);
    for (int i = 0; i < 6; ++i)
      expect_eq("crossprod: repeated g" + std::to_string(i),
                repeated_gradient[i], gradient[i]);

    using stan::math::var;
    Eigen::Matrix<var, -1, -1> a(2, 3);
    for (int i = 0; i < 6; ++i) a(i) = q[i];
    Eigen::Matrix<var, -1, -1> gram = stan::math::crossprod(a);
    Eigen::Matrix<double, -1, -1> data_matrix(2, 3);
    for (int i = 0; i < 6; ++i) data_matrix(i) = observed[i];
    Eigen::Matrix<double, -1, -1> data_gram =
        stan::math::crossprod(data_matrix);
    var reference_lp =
        stan::math::normal_lpdf<true>(stan::math::to_vector(a), 0, 1) +
        stan::math::normal_lpdf<true>(stan::math::to_vector(gram),
                                      stan::math::to_vector(data_gram), 1);
    reference_lp.grad();
    expect_ulp("crossprod: lp", lp, reference_lp.val());
    for (int i = 0; i < 6; ++i)
      expect_ulp("crossprod: g" + std::to_string(i), gradient[i], a(i).adj());

    check(cm.write_array && cm.write_array->truncated.empty(),
          "crossprod: write_array compiled");
    if (cm.write_array && cm.write_array->truncated.empty()) {
      Executor wex(std::move(cm.write_array->graph));
      cm.write_array->bind(wex);
      for (int i = 0; i < 6; ++i) wex.params_data()[i] = q[i];
      wex.run_forward_only();
      bool found = false;
      for (const auto& column : cm.write_array->columns) {
        if (column.name != "gq_gram") continue;
        found = true;
        const double* value = wex.value_ptr(column.slot);
        for (int i = 0; i < 9; ++i)
          expect_ulp("crossprod: write_array " + std::to_string(i), value[i],
                     gram(i).val());
      }
      check(found, "crossprod: write_array has gq_gram");
    }
    stan::math::recover_memory();
  }

  // Two dimension-preserving selectors on a matrix form their Cartesian
  // submatrix. Different reordered Multi lists pin column-major gather order,
  // Between/Between covers matrix ranges, and the mixed Multi/Single and
  // Single/Multi forms keep the surviving axis. All route each selected
  // cell's adjoint back to the source matrix.
  {
    DataMap d;
    d.set_int_array("row_indices", {3, 1});
    d.set_int_array("column_indices", {2, 3});
    CompiledModel gm =
        compile_model(slurp("tests/fixtures/matrix_multi_multi.tmir.sexp"), d);
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    const double q[9] = {0.2, -0.4, 0.7, 0.1, -0.3, 0.8, 0.5, 0.9, -0.6};
    for (int i = 0; i < 9; ++i) gex.params_data()[i] = q[i];
    double gradient[9] = {};
    const double lp = gex.gradient(gradient);
    const double want = 2.0 * (q[5] + q[3] + q[8] + q[6]) +
                        2.0 * (q[3] + q[4] + q[6] + q[7]) +
                        2.0 * (q[2] + q[0]) + 2.0 * (q[3] + q[6]);
    expect_eq("matrix selectors: lp", lp, want);
    const int times[9] = {1, 0, 1, 3, 1, 1, 3, 1, 1};
    for (int i = 0; i < 9; ++i)
      expect_eq("matrix selectors: g" + std::to_string(i), gradient[i],
                2.0 * times[i]);
  }

  // O1 folds a full-span read's All indices away and leaves an Indexed node
  // with no indices, on matrices and vectors alike.
  {
    const DataMap d;
    CompiledModel gm =
        compile_model(slurp("tests/fixtures/full_span_read.tmir.sexp"), d);
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    // Dyadic values, so no summation order can round differently.
    const double q[10] = {0.5,   -0.25, 0.75, 0.125,  -0.375,
                          0.875, 0.5,   1.0,  -0.625, 0.25};
    for (int i = 0; i < 10; ++i) gex.params_data()[i] = q[i];
    double gradient[10] = {};
    const double lp = gex.gradient(gradient);
    double want = 0;
    for (int i = 0; i < 6; ++i) want += q[i];
    for (int i = 6; i < 10; ++i) want += 2.0 * q[i];
    expect_eq("full span read: lp", lp, want);
    for (int i = 0; i < 10; ++i)
      expect_eq("full span read: g" + std::to_string(i), gradient[i],
                i < 6 ? 1.0 : 2.0);
  }

  // O1 composes a scalar UDF index through a two-axis gather as a nested
  // Indexed node whose empty outer layer carries UReal. The selected cell is
  // row_indices[1], column_indices[2] = source (3,3).
  {
    DataMap d;
    d.set_int_array("row_indices", {3, 1});
    d.set_int_array("column_indices", {2, 3});
    CompiledModel gm = compile_model(
        slurp("tests/fixtures/matrix_multi_multi_nested.tmir.sexp"), d);
    Executor gex(std::move(gm.graph));
    gm.bind(gex);
    const double q[9] = {0.2, -0.4, 0.7, 0.1, -0.3, 0.8, 0.5, 0.9, -0.6};
    for (int i = 0; i < 9; ++i) gex.params_data()[i] = q[i];
    double gradient[9] = {};
    expect_eq("matrix nested index: lp", gex.gradient(gradient), 2.0 * q[8]);
    for (int i = 0; i < 9; ++i)
      expect_eq("matrix nested index: g" + std::to_string(i), gradient[i],
                i == 8 ? 2.0 : 0.0);
  }

  // A scalar replicated as a row vector uses the vector replication opcode,
  // but must retain its row-vector type for downstream expression lowering.
  {
    DataMap d;
    CompiledModel tm =
        compile_model(slurp("tests/fixtures/rep_row_vector.tmir.sexp"), d);
    Executor tex(std::move(tm.graph));
    tm.bind(tex);
    const double q = 0.3;
    tex.params_data()[0] = q;
    double gradient = 0;
    const double lp = tex.gradient(&gradient);

    using stan::math::var;
    var theta = q;
    Eigen::Matrix<var, 1, -1> x = stan::math::rep_row_vector(theta, 4);
    var reference_lp = stan::math::normal_lpdf<true>(x, 0, 1);
    reference_lp.grad();
    expect_eq("rep_row_vector: lp", lp, reference_lp.val());
    expect_ulp("rep_row_vector: gradient", gradient, theta.adj());
  }

  // Vector fma from --O1 partial evaluation (`k .* t + c` becomes
  // fma(k, t, c)): OP_FMA is FUSED, so the reference is stan::math::fma,
  // and the data avoids powers of two -- with t = 2.0/-1.0/0.5 the
  // products are exact and fused equals unfused bitwise, which once let
  // both implementations pass this test.
  {
    DataMap d = DataMap::from_json(R"({"N": 3, "t": [1.7, -0.3, 0.9]})");
    CompiledModel fm =
        compile_model(slurp("tests/fixtures/vecfma.tmir.sexp"), d);
    Executor fex(std::move(fm.graph));
    fm.bind(fex);
    const double q[6] = {0.3, -0.2, 1.1, 0.7, -0.4, 0.9};  // k[3], c[3]
    for (int i = 0; i < 6; ++i) fex.params_data()[i] = q[i];
    double grad[6] = {0, 0, 0, 0, 0, 0};
    const double lp = fex.gradient(grad);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> k(3), c(3);
    for (int i = 0; i < 3; ++i) k(i) = q[i];
    for (int i = 0; i < 3; ++i) c(i) = q[3 + i];
    Eigen::VectorXd t(3);
    t << 1.7, -0.3, 0.9;
    Eigen::Matrix<var, -1, 1> mu(3);
    for (int i = 0; i < 3; ++i) mu(i) = stan::math::fma(k(i), t(i), c(i));
    var acc = stan::math::normal_lpdf<false>(mu, 0, 1);
    acc.grad();
    check(lp == acc.val(), "vector fma: lp bitwise against the var path");
    bool g_ok = true;
    for (int i = 0; i < 3; ++i)
      if (grad[i] != k(i).adj() || grad[3 + i] != c(i).adj()) g_ok = false;
    check(g_ok, "vector fma: gradients bitwise against the var path");
  }

  // Where the CSV's three sections meet. stanc marks the boundaries with
  // early-return guards on emit_transformed_parameters__ /
  // emit_generated_quantities__; lowering pins both flags on, so the
  // recorded indices are the only thing left that says which column is a
  // constrained parameter and which a generated quantity.
  {
    // conj's header, verified against CmdStan in tests/test_sampling.cpp,
    // is mu_c,sigma,prec,mu,sd_from_prec,resid.<i>: two constrained
    // parameters, one transformed parameter, three generated quantities.
    DataMap d = DataMap::from_json_file("tests/fixtures/conj.json");
    CompiledModel wm = compile_model(slurp("tests/fixtures/conj.tmir.sexp"), d);
    check(wm.write_array.has_value(), "conj: write_array compiled");
    if (wm.write_array) {
      expect_eq("conj columns", (double)wm.write_array->columns.size(), 6);
      expect_eq("conj n_tp_start", (double)wm.write_array->n_tp_start, 2);
      expect_eq("conj n_gq_start", (double)wm.write_array->n_gq_start, 3);
    }
  }
  {
    // Parameters only: every column is a constrained parameter, so both
    // boundaries sit at the end.
    DataMap d;
    d.set_int("K", 3);
    CompiledModel wm = compile_model(slurp("tests/fixtures/simp.tmir.sexp"), d);
    check(wm.write_array.has_value(), "simp: write_array compiled");
    if (wm.write_array) {
      expect_eq("simp columns", (double)wm.write_array->columns.size(), 1);
      expect_eq("simp n_tp_start", (double)wm.write_array->n_tp_start, 1);
      expect_eq("simp n_gq_start", (double)wm.write_array->n_gq_start, 1);
    }

    // The unconstrained layout: simplex[3] is two free values behind three
    // constrained ones, and its metadata carries that exact naming shape.
    check(wm.unc_params.size() == 1, "simp: one unconstrained parameter");
    if (wm.unc_params.size() == 1) {
      const auto& u = wm.unc_params[0];
      check(u.name == "theta", "simp unc name");
      expect_eq("simp unc len", (double)u.len, 2);
      check(u.dims == std::vector<int64_t>{2}, "simp unc dims");
      check(u.transform == mir::Transform::Simplex, "simp unc transform");
    }
  }
  {
    // One of every transform: the unconstrained lengths are what the
    // reader has to slice the draw with, and only the declared dims tell
    // corr_matrix[3] from cov_matrix[3].
    DataMap d = DataMap::from_json_file("tests/fixtures/newtrans.json");
    CompiledModel nm =
        compile_model(slurp("tests/fixtures/newtrans.tmir.sexp"), d);
    check(nm.unc_params.size() == nm.param_names.size(),
          "newtrans: one unc_param per declared parameter");
    int64_t total = 0;
    bool names_ok = nm.unc_params.size() == nm.param_names.size();
    for (size_t i = 0; i < nm.unc_params.size(); ++i) {
      total += nm.unc_params[i].len;
      if (i < nm.param_names.size() &&
          nm.unc_params[i].name != nm.param_names[i])
        names_ok = false;
    }
    check(names_ok, "newtrans: unc_params agree with param_names");
    expect_eq("newtrans unc total", (double)total, (double)nm.n_unconstrained);
    struct Want {
      const char* name;
      int64_t len;
      std::vector<int64_t> dims;
      mir::Transform::Kind kind;
    };
    const std::vector<Want> want{
        {"a", 1, {}, mir::Transform::OffsetMultiplier},
        {"b", 3, {3}, mir::Transform::OffsetMultiplier},
        {"c", 1, {}, mir::Transform::Offset},
        {"d", 1, {}, mir::Transform::Multiplier},
        {"mu_p", 3, {3}, mir::Transform::Identity},
        {"sg_p", 3, {3}, mir::Transform::Lower},
        {"e", 3, {3}, mir::Transform::OffsetMultiplier},
        {"u", 3, {3}, mir::Transform::UnitVector},
        {"z", 3, {3}, mir::Transform::SumToZero},
        {"R", 3, {3}, mir::Transform::Correlation},
        {"S", 6, {6}, mir::Transform::Covariance},
        {"Lc", 6, {6}, mir::Transform::CholeskyCov},
        {"Lr", 9, {9}, mir::Transform::CholeskyCov}};
    check(nm.unc_params.size() == want.size(), "newtrans: 13 parameters");
    for (size_t i = 0; i < want.size() && i < nm.unc_params.size(); ++i) {
      const auto& u = nm.unc_params[i];
      const std::string tag = std::string("newtrans ") + want[i].name;
      check(u.name == want[i].name, tag + " name");
      expect_eq(tag + " len", (double)u.len, (double)want[i].len);
      check(u.dims == want[i].dims, tag + " dims");
      check(u.transform == want[i].kind, tag + " transform");
    }
  }

  // stanc's generated constructor executes data constraints before it
  // constructs the model. These are construction errors, not bad draws that
  // may survive until a density or transform happens to notice them.
  {
    const std::string mir = slurp("tests/fixtures/newtrans.tmir.sexp");
    auto rejected_s = [&](double s) {
      DataMap d;
      d.set_real("m", 0.3);
      d.set_real("s", s);
      try {
        (void)compile_model(mir, d);
      } catch (const std::domain_error& e) {
        return std::string(e.what()).find("s") != std::string::npos;
      }
      return false;
    };
    check(rejected_s(-1.0), "data lower bound rejects before construction");
    check(rejected_s(std::numeric_limits<double>::quiet_NaN()),
          "data lower bound rejects NaN");

    DataMap at_bound;
    at_bound.set_real("m", 0.3);
    at_bound.set_real("s", 0.5);
    bool accepted = true;
    try {
      (void)compile_model(mir, at_bound);
    } catch (const std::exception&) {
      accepted = false;
    }
    check(accepted, "data lower bound is inclusive");
  }
  {
    // An upper check over an array must inspect every element. The fourth
    // value is outside 1:K; flattening or checking only the first lane
    // accepts data generated Stan rejects in its constructor.
    DataMap d;
    d.set_int("K", 3);
    d.set_int("y", 2);
    d.set_int_array("ys", {1, 2, 4});
    bool rejected = false;
    try {
      (void)compile_model(slurp("tests/fixtures/cat.tmir.sexp"), d);
    } catch (const std::domain_error& e) {
      rejected = std::string(e.what()).find("ys") != std::string::npos;
    }
    check(rejected, "container data upper bound rejects a later element");
  }

  {
    // Generated Stan checks constrained transformed parameters while it
    // evaluates a draw, not while it constructs the model. The data check
    // above and this runtime check share FnCheck in MIR but must retain their
    // distinct execution phases.
    DataMap d = bound_check_data();
    CompiledModel checked =
        compile_model(slurp("tests/fixtures/data_and_tp_checks.tmir.sexp"), d);
    Executor cex(std::move(checked.graph));
    checked.bind(cex);
    cex.params_data()[0] = -1.0;
    double grad = 0.0;
    bool rejected = false;
    try {
      (void)cex.gradient(&grad);
    } catch (const std::domain_error& e) {
      rejected = std::string(e.what()).find("z") != std::string::npos;
    }
    check(rejected, "transformed-parameter bound rejects the draw");

    cex.params_data()[0] = 0.0;
    bool accepted = true;
    double lp = 0.0;
    try {
      lp = cex.gradient(&grad);
    } catch (const std::exception&) {
      accepted = false;
    }
    check(accepted && lp == 0.0 && grad == 3.0,
          "transformed-parameter bound is inclusive");
  }
  {
    // Parameter-free does not mean construction-time: a transformed
    // parameter derived only from unconstrained data is still checked at the
    // generated log_prob/write_array statement where it was declared.
    DataMap d = bound_check_data(-1.0);
    bool constructed = true;
    bool rejected = false;
    try {
      CompiledModel checked = compile_model(
          slurp("tests/fixtures/data_and_tp_checks.tmir.sexp"), d);
      Executor cex(std::move(checked.graph));
      checked.bind(cex);
      cex.params_data()[0] = 0.0;
      double grad = 0.0;
      try {
        (void)cex.gradient(&grad);
      } catch (const std::domain_error& e) {
        rejected = std::string(e.what()).find("from_data") != std::string::npos;
      }
    } catch (const std::exception&) {
      constructed = false;
    }
    check(constructed && rejected,
          "data-derived transformed bound rejects only at evaluation");
  }
  {
    // Runtime-sized container bounds fail in the generated statement, not
    // while the graph is being compiled. Equal flat widths are likewise not
    // enough to make matrix orientations compatible.
    auto rejects_at_evaluation = [&](DataMap d, double x,
                                     const std::string& name) {
      bool constructed = true;
      bool rejected = false;
      try {
        CompiledModel checked = compile_model(
            slurp("tests/fixtures/data_and_tp_checks.tmir.sexp"), d);
        Executor cex(std::move(checked.graph));
        checked.bind(cex);
        cex.params_data()[0] = x;
        double grad = 0.0;
        try {
          (void)cex.gradient(&grad);
        } catch (const std::invalid_argument& e) {
          rejected = std::string(e.what()).find(name) != std::string::npos;
        }
      } catch (const std::exception&) {
        constructed = false;
      }
      return constructed && rejected;
    };
    check(rejects_at_evaluation(bound_check_data(0.0, 1, 2), 0.0, "bounded"),
          "vector-bound shape mismatch is a runtime error");
    check(rejects_at_evaluation(bound_check_data(0.0, 1, 2), -1.0, "bounded"),
          "dimension check precedes later scalar bound checks");
    check(rejects_at_evaluation(bound_check_data(0.0, 1, 1, 1, 2, 2, 1), 0.0,
                                "bounded_matrix"),
          "equal-width matrix-bound mismatch is a runtime error");
  }

  {
    // All-integer densities have no differentiable graph edge, but their
    // shared registry policy still evaluates them during model preparation.
    // In particular, Stan Math performs support checks before dropping an
    // all-data propto term: valid data becomes zero and invalid data throws.
    const std::string hypergeometric_mir = R"(
((functions_block ())
 (input_vars
  ((n <opaque> SInt) (N <opaque> SInt) (a <opaque> SInt)
   (b <opaque> SInt)))
 (prepare_data ())
 (log_prob
  (((pattern
     (TargetPE
      ((pattern
        (FunApp (StanLib hypergeometric_lpmf (FnLpmf true) AoS)
         (((pattern (Var n))
           (meta ((type_ UInt) (adlevel DataOnly))))
          ((pattern (Var N))
           (meta ((type_ UInt) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UInt) (adlevel DataOnly))))
          ((pattern (Var b))
           (meta ((type_ UInt) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (adlevel DataOnly))))))
    (meta <opaque>)))))
)";

    check(stan::math::hypergeometric_lpmf<true>(1, 2, 3, 4) == 0.0,
          "all-data propto oracle drops a valid density");
    bool oracle_rejected = false;
    try {
      (void)stan::math::hypergeometric_lpmf<true>(4, 4, 3, 3);
    } catch (const std::domain_error&) {
      oracle_rejected = true;
    }
    check(oracle_rejected,
          "all-data propto oracle still checks invalid support");

    auto lower = [&](int n, int N, int a, int b) {
      DataMap d;
      d.set_int("n", n);
      d.set_int("N", N);
      d.set_int("a", a);
      d.set_int("b", b);
      CompiledModel model = compile_model(hypergeometric_mir, d);
      Executor executor(std::move(model.graph));
      model.bind(executor);
      return executor.gradient(nullptr);
    };
    expect_eq("valid all-data propto density", lower(1, 2, 3, 4), 0.0);
    bool lowering_rejected = false;
    try {
      (void)lower(4, 4, 3, 3);
    } catch (const std::domain_error& e) {
      lowering_rejected = std::string(e.what()).find("hypergeometric_lpmf") !=
                          std::string::npos;
    }
    check(lowering_rejected,
          "all-data propto density preserves support validation");

    const DensitySpec* range = density_spec("discrete_range_lpmf");
    check(range != nullptr &&
              range->evaluation == DensityEvaluationPolicy::AllInteger,
          "discrete range uses the shared all-integer policy");
    std::vector<IntegerDensityArgument> range_args{
        {{2, 4}, false}, {{1}, true}, {{3, 5}, false}};
    expect_eq("all-integer scalar/array dispatch",
              evaluate_all_integer_density(*range, range_args, false),
              stan::math::discrete_range_lpmf(std::vector<int>{2, 4}, 1,
                                              std::vector<int>{3, 5}));
  }

  {
    // An all-data propto categorical term returns zero, but only after Stan
    // Math validates the outcome and probability/logit vector. Compare the
    // whole observable contract, including which competing error wins and
    // the scalar-vs-array overload selected for a length-one outcome.
    struct Outcome {
      std::string kind;
      std::string message;
      double value = 0.0;
    };
    auto observe = [](auto&& call) {
      Outcome out;
      try {
        out.value = call();
        out.kind = "value";
      } catch (const std::domain_error& e) {
        out.kind = "domain_error";
        out.message = e.what();
      } catch (const std::invalid_argument& e) {
        out.kind = "invalid_argument";
        out.message = e.what();
      } catch (const std::exception& e) {
        out.kind = "other";
        out.message = e.what();
      }
      return out;
    };
    auto oracle = [&](const std::string& fn, bool scalar,
                      const std::vector<int>& outcome,
                      const std::vector<double>& arg, bool propto) {
      return observe([&] {
        Eigen::Map<const Eigen::VectorXd> v(arg.data(),
                                            (Eigen::Index)arg.size());
        if (fn == "categorical_lpmf") {
          if (propto)
            return scalar ? stan::math::categorical_lpmf<true>(outcome.at(0), v)
                          : stan::math::categorical_lpmf<true>(outcome, v);
          return scalar ? stan::math::categorical_lpmf<false>(outcome.at(0), v)
                        : stan::math::categorical_lpmf<false>(outcome, v);
        }
        if (propto)
          return scalar ? stan::math::categorical_logit_lpmf<true>(
                              outcome.at(0), v)
                        : stan::math::categorical_logit_lpmf<true>(outcome, v);
        return scalar
                   ? stan::math::categorical_logit_lpmf<false>(outcome.at(0), v)
                   : stan::math::categorical_logit_lpmf<false>(outcome, v);
      });
    };
    auto lowered = [&](const std::string& fn, bool scalar,
                       const std::vector<int>& outcome,
                       const std::vector<double>& arg, bool propto) {
      return observe([&] {
        DataMap d;
        if (scalar)
          d.set_int("outcome", outcome.at(0));
        else
          d.set_int_array("outcome", outcome);
        d.set_real_array("arg", arg);
        CompiledModel model =
            compile_model(categorical_check_mir(fn, scalar, (int)outcome.size(),
                                                (int)arg.size(), propto),
                          d);
        Executor ex(std::move(model.graph));
        model.bind(ex);
        return ex.forward();
      });
    };
    auto same_as_stan = [&](const std::string& label, const std::string& fn,
                            bool scalar, std::vector<int> outcome,
                            std::vector<double> arg, bool propto = true) {
      const Outcome want = oracle(fn, scalar, outcome, arg, propto);
      const Outcome got = lowered(fn, scalar, outcome, arg, propto);
      check(got.kind == want.kind, label + " exception class");
      check(got.message == want.message, label + " exception message");
      if (want.kind == "value")
        expect_eq(label + " value", got.value, want.value);
    };
    auto local_autodiff_oracle = [&](const std::string& fn, bool scalar,
                                     const std::vector<int>& outcome,
                                     const std::vector<double>& arg) {
      return observe([&] {
        Eigen::Matrix<stan::math::var, -1, 1> v((Eigen::Index)arg.size());
        for (size_t i = 0; i < arg.size(); ++i) v((Eigen::Index)i) = arg[i];
        try {
          stan::math::var lp =
              fn == "categorical_lpmf"
                  ? (scalar
                         ? stan::math::categorical_lpmf<true>(outcome.at(0), v)
                         : stan::math::categorical_lpmf<true>(outcome, v))
                  : (scalar ? stan::math::categorical_logit_lpmf<true>(
                                  outcome.at(0), v)
                            : stan::math::categorical_logit_lpmf<true>(outcome,
                                                                       v));
          const double value = lp.val();
          stan::math::recover_memory();
          return value;
        } catch (...) {
          stan::math::recover_memory();
          throw;
        }
      });
    };
    auto local_autodiff_lowered = [&](const std::string& fn, bool scalar,
                                      const std::vector<int>& outcome,
                                      const std::vector<double>& arg) {
      return observe([&] {
        DataMap d;
        if (scalar)
          d.set_int("outcome", outcome.at(0));
        else
          d.set_int_array("outcome", outcome);
        d.set_real_array("arg", arg);
        CompiledModel model =
            compile_model(categorical_check_mir(fn, scalar, (int)outcome.size(),
                                                (int)arg.size(), true, true),
                          d);
        Executor ex(std::move(model.graph));
        model.bind(ex);
        ex.params_data()[0] = 0.0;
        return ex.forward();
      });
    };
    auto same_local_autodiff_as_stan =
        [&](const std::string& label, const std::string& fn, bool scalar,
            std::vector<int> outcome, std::vector<double> arg) {
          const Outcome want = local_autodiff_oracle(fn, scalar, outcome, arg);
          const Outcome got = local_autodiff_lowered(fn, scalar, outcome, arg);
          check(got.kind == want.kind, label + " exception class");
          check(got.message == want.message, label + " exception message");
          if (want.kind == "value")
            expect_eq(label + " value", got.value, want.value);
        };

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    same_as_stan("categorical scalar valid", "categorical_lpmf", true, {2},
                 {0.2, 0.3, 0.5});
    same_as_stan("categorical zero probability", "categorical_lpmf", true, {2},
                 {1.0, 0.0, 0.0});
    same_as_stan("categorical scalar outcome", "categorical_lpmf", true, {4},
                 {0.2, 0.3, 0.5});
    same_as_stan("categorical outcome before simplex", "categorical_lpmf", true,
                 {4}, {0.2, 0.2, 0.2});
    same_as_stan("categorical later array outcome", "categorical_lpmf", false,
                 {1, 4, 2}, {0.2, 0.3, 0.5});
    same_as_stan("categorical length-one array", "categorical_lpmf", false, {4},
                 {0.2, 0.3, 0.5});
    same_as_stan("categorical empty outcome", "categorical_lpmf", false, {},
                 {0.2, 0.3, 0.5});
    same_as_stan("categorical empty still checks simplex", "categorical_lpmf",
                 false, {}, {0.2, 0.2, 0.2});
    same_as_stan("categorical scalar empty probabilities", "categorical_lpmf",
                 true, {1}, {});
    same_as_stan("categorical empty probabilities", "categorical_lpmf", false,
                 {}, {});
    same_as_stan("categorical normalized valid", "categorical_lpmf", false,
                 {1, 3}, {0.2, 0.3, 0.5}, false);
    same_as_stan("categorical normalized bad simplex", "categorical_lpmf", true,
                 {1}, {0.2, 0.2, 0.2}, false);
    same_as_stan("categorical normalized invalid outcome", "categorical_lpmf",
                 false, {1, 4}, {0.2, 0.3, 0.5}, false);
    same_as_stan("categorical normalized empty outcome", "categorical_lpmf",
                 false, {}, {0.2, 0.3, 0.5}, false);

    same_as_stan("categorical logit scalar valid", "categorical_logit_lpmf",
                 true, {2}, {-1.0, 0.0, 1.0});
    same_as_stan("categorical logit scalar outcome", "categorical_logit_lpmf",
                 true, {4}, {-1.0, 0.0, 1.0});
    same_as_stan("categorical logit outcome before finite",
                 "categorical_logit_lpmf", true, {4}, {0.0, nan, 1.0});
    same_as_stan("categorical logit later array outcome",
                 "categorical_logit_lpmf", false, {1, 4, 2}, {-1.0, 0.0, 1.0});
    same_as_stan("categorical logit length-one array", "categorical_logit_lpmf",
                 false, {4}, {-1.0, 0.0, 1.0});
    same_as_stan("categorical logit empty checks NaN", "categorical_logit_lpmf",
                 false, {}, {0.0, nan, 1.0});
    same_as_stan("categorical logit checks infinity", "categorical_logit_lpmf",
                 true, {1}, {0.0, inf, 1.0});
    same_as_stan("categorical logit empty vectors", "categorical_logit_lpmf",
                 false, {}, {});
    same_as_stan("categorical logit scalar empty vector",
                 "categorical_logit_lpmf", true, {1}, {});
    same_as_stan("categorical logit normalized valid", "categorical_logit_lpmf",
                 false, {1, 3}, {-1.0, 0.0, 1.0}, false);
    same_as_stan("categorical logit normalized infinite",
                 "categorical_logit_lpmf", true, {1}, {0.0, inf, 1.0}, false);
    same_as_stan("categorical logit normalized invalid outcome",
                 "categorical_logit_lpmf", false, {1, 4}, {-1.0, 0.0, 1.0},
                 false);
    same_as_stan("categorical logit normalized empty outcome",
                 "categorical_logit_lpmf", false, {}, {-1.0, 0.0, 1.0}, false);
    same_as_stan("categorical logit normalized empty vectors",
                 "categorical_logit_lpmf", false, {}, {}, false);

    // A local declared AutoDiffable remains a var in generated C++ even when
    // assigned entirely from data. Its graph slot is parameter-free, so the
    // exact op can own the value, but propto must still retain the summand.
    same_local_autodiff_as_stan("categorical local autodiff valid",
                                "categorical_lpmf", true, {2}, {0.2, 0.3, 0.5});
    same_local_autodiff_as_stan("categorical local autodiff length-one array",
                                "categorical_lpmf", false, {2},
                                {0.2, 0.3, 0.5});
    {
      DataMap local_data;
      local_data.set_int_array("outcome", {2});
      local_data.set_real_array("arg", {0.2, 0.3, 0.5});
      CompiledModel local_model = compile_model(
          categorical_check_mir("categorical_lpmf", false, 1, 3, true, true),
          local_data);
      Executor local_ex(std::move(local_model.graph));
      local_model.bind(local_ex);
      local_ex.params_data()[0] = 0.0;
      expect_eq("categorical propto value-only double instantiation",
                local_ex.forward_value_only(), 0.0);
    }
    same_local_autodiff_as_stan("categorical local autodiff invalid",
                                "categorical_lpmf", true, {4}, {0.2, 0.3, 0.5});
    same_local_autodiff_as_stan("categorical logit local autodiff valid",
                                "categorical_logit_lpmf", false, {1, 3},
                                {-1.0, 0.0, 1.0});
    same_local_autodiff_as_stan("categorical logit local autodiff invalid",
                                "categorical_logit_lpmf", false, {1, 4},
                                {-1.0, 0.0, 1.0});
    auto check_local_exact_graph = [&](const std::string& label,
                                       const std::string& fn, bool scalar,
                                       const std::vector<int>& outcome,
                                       const std::vector<double>& arg) {
      DataMap local_data;
      if (scalar)
        local_data.set_int("outcome", outcome.at(0));
      else
        local_data.set_int_array("outcome", outcome);
      local_data.set_real_array("arg", arg);
      CompiledModel local_model =
          compile_model(categorical_check_mir(fn, scalar, (int)outcome.size(),
                                              (int)arg.size(), true, true),
                        local_data);
      bool found_exact = false;
      int exact_slot = -1;
      bool found_decomposition = false;
      for (const Op& op : local_model.graph.ops) {
        if (op.opcode == OP_CATEGORICAL) {
          found_exact = true;
          exact_slot = op.out;
        }
        found_decomposition = found_decomposition || op.opcode == OP_INDEX ||
                              op.opcode == OP_GATHER || op.opcode == OP_LOGV ||
                              op.opcode == OP_LOG_SOFTMAX ||
                              op.opcode == OP_SUM_VEC;
      }
      bool exact_value_used = exact_slot == local_model.graph.result_slot;
      for (const Op& op : local_model.graph.ops)
        for (int i = 0; i < op.n_in; ++i)
          exact_value_used = exact_value_used || op.in[i] == exact_slot;
      check(found_exact, label + " exact op");
      check(exact_value_used, label + " exact op owns target value");
      check(!found_decomposition, label + " no unchecked decomposition");
    };
    check_local_exact_graph("categorical local autodiff", "categorical_lpmf",
                            true, {4}, {0.2, 0.3, 0.5});
    check_local_exact_graph("categorical logit local autodiff",
                            "categorical_logit_lpmf", false, {1, 4},
                            {-1.0, 0.0, 1.0});

    auto malformed_refused = [](const std::string& mir, const DataMap& data) {
      try {
        (void)compile_model(mir, data);
      } catch (const std::exception& e) {
        return std::string(e.what()).find("categorical") != std::string::npos;
      }
      return false;
    };
    {
      std::string mir = categorical_check_mir("categorical_lpmf", true, 1, 3);
      const std::string from = "(Var outcome)";
      mir.replace(mir.find(from), from.size(), "(Var anchor)");
      DataMap malformed_data;
      malformed_data.set_int("outcome", 2);
      malformed_data.set_real_array("arg", {0.2, 0.3, 0.5});
      check(malformed_refused(mir, malformed_data),
            "categorical rejects data-only parameter outcome metadata");
    }
    {
      std::string mir = categorical_check_mir("categorical_lpmf", true, 1, 3);
      const std::string from = "outcome <opaque> SInt";
      mir.replace(mir.find(from), from.size(), "outcome <opaque> SReal");
      DataMap malformed_data;
      malformed_data.set_real("outcome", 2.0);
      malformed_data.set_real_array("arg", {0.2, 0.3, 0.5});
      check(malformed_refused(mir, malformed_data),
            "categorical rejects real binding with integer metadata");
    }
    {
      std::string mir = slurp("tests/fixtures/cat.tmir.sexp");
      const std::string from =
          "((pattern (Var theta))\n"
          "               (meta ((type_ UVector) (loc <opaque>) (adlevel "
          "AutoDiffable))))";
      const size_t pos = mir.find(from);
      check(pos != std::string::npos,
            "categorical malformed arg mutation found target");
      if (pos != std::string::npos) {
        std::string to = from;
        to.replace(to.find("AutoDiffable"), std::strlen("AutoDiffable"),
                   "DataOnly");
        mir.replace(pos, from.size(), to);
      }
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(mir, malformed_data),
            "categorical rejects data-only parameter vector metadata");
    }
    {
      const std::string mir = categorical_check_mir("categorical_lpmf", false,
                                                    2, 3, true, false, true);
      DataMap malformed_data =
          DataMap::from_json(R"({"outcome":[[1,2]],"arg":[0.2,0.3,0.5]})");
      check(malformed_refused(mir, malformed_data),
            "categorical rejects flattened rank-two outcome metadata");
    }
    for (bool in_gq : {false, true}) {
      const std::string malformed =
          categorical_transformed_data_mismatch_mir(in_gq);
      check(malformed.find("(Var td_outcome)") != std::string::npos,
            "categorical transformed-data fixture rewrites outcome");
      DataMap malformed_data;
      malformed_data.set_real("source", 2.0);
      malformed_data.set_int("outcome", 2);
      malformed_data.set_real_array("arg", {0.2, 0.3, 0.5});
      check(malformed_refused(malformed, malformed_data),
            std::string("categorical rejects transformed-data binding in ") +
                (in_gq ? "generated quantities" : "log probability"));
    }
    {
      std::string malformed = categorical_write_array_mir(
          slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false);
      const size_t gq = malformed.find("(generate_quantities");
      const size_t call =
          malformed.find("(FunApp (StanLib categorical_lpmf", gq);
      const size_t outcome = malformed.find("(Var y)", call);
      check(gq != std::string::npos && call != std::string::npos &&
                outcome != std::string::npos,
            "categorical missing-binding mutation found target");
      if (outcome != std::string::npos)
        malformed.replace(outcome, std::strlen("(Var y)"), "(Var missing)");
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(malformed, malformed_data),
            "categorical rejects missing GQ binding before fallback");
    }
    {
      std::string malformed = categorical_write_array_mir(
          slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false,
          false, false, true);
      const size_t gq = malformed.find("(generate_quantities");
      const size_t call =
          malformed.find("(FunApp (StanLib categorical_lpmf", gq);
      const std::string vector_arg =
          "((pattern (Var theta))\n"
          "             (meta ((type_ UVector) (adlevel DataOnly)))))";
      const size_t arg = malformed.find(vector_arg, call);
      check(gq != std::string::npos && call != std::string::npos &&
                arg != std::string::npos,
            "categorical malformed fallback mutation found target");
      if (arg != std::string::npos) {
        const std::string scalar_arg =
            "((pattern (Lit Real 1.0))\n"
            "             (meta ((type_ UReal) (adlevel DataOnly)))))";
        malformed.replace(arg, vector_arg.size(), scalar_arg);
      }
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(malformed, malformed_data),
            "categorical rejects malformed GQ signature before fallback");
    }
    {
      std::string malformed = categorical_write_array_mir(
          slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false,
          false, true, true);
      const size_t gq = malformed.find("(generate_quantities");
      const size_t call =
          malformed.find("(FunApp (UserDefined data_categorical", gq);
      const std::string vector_arg =
          "((pattern (Var theta))\n"
          "             (meta ((type_ UVector) (adlevel DataOnly)))))";
      const size_t arg = malformed.find(vector_arg, call);
      check(gq != std::string::npos && call != std::string::npos &&
                arg != std::string::npos,
            "categorical malformed UDF fallback mutation found target");
      if (arg != std::string::npos) {
        const std::string scalar_arg =
            "((pattern (Lit Real 1.0))\n"
            "             (meta ((type_ UReal) (adlevel DataOnly)))))";
        malformed.replace(arg, vector_arg.size(), scalar_arg);
      }
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(malformed, malformed_data),
            "categorical rejects malformed UDF actual before fallback");
    }
    {
      std::string malformed = categorical_write_array_mir(
          slurp("tests/fixtures/cat.tmir.sexp"), "categorical_lpmf", false,
          false, true, true);
      const std::string formal = "(DataOnly p UVector)";
      const size_t formal_at = malformed.find(formal);
      const size_t gq = malformed.find("(generate_quantities");
      const size_t call =
          malformed.find("(FunApp (UserDefined data_categorical", gq);
      const std::string actual =
          "((pattern (Var theta))\n"
          "             (meta ((type_ UVector) (adlevel DataOnly)))))";
      const size_t actual_at = malformed.find(actual, call);
      check(formal_at != std::string::npos && gq != std::string::npos &&
                call != std::string::npos && actual_at != std::string::npos,
            "categorical malformed UDF binding mutation found target");
      if (actual_at != std::string::npos) {
        std::string scalar_actual = actual;
        scalar_actual.replace(scalar_actual.find("UVector"),
                              std::strlen("UVector"), "UReal");
        malformed.replace(actual_at, actual.size(), scalar_actual);
      }
      if (formal_at != std::string::npos)
        malformed.replace(formal_at, formal.size(), "(DataOnly p UReal)");
      DataMap malformed_data =
          DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      check(malformed_refused(malformed, malformed_data),
            "categorical rejects UDF metadata that contradicts its binding");
    }
    for (const std::string& fn :
         {"categorical_lpmf", "categorical_logit_lpmf"}) {
      DataMap malformed_data;
      malformed_data.set_int("outcome", 2);
      malformed_data.set_real_array("arg",
                                    fn == "categorical_lpmf"
                                        ? std::vector<double>{0.2, 0.3, 0.5}
                                        : std::vector<double>{-1.0, 0.0, 1.0});
      bool direct_refused = false;
      try {
        (void)compile_model(categorical_udf_mir(fn, true, true, false, true),
                            malformed_data);
      } catch (const std::exception& e) {
        const std::string msg = e.what();
        direct_refused = msg.find("data-only") != std::string::npos &&
                         msg.find("parameter") != std::string::npos;
      }
      check(direct_refused, fn + " rejects parameter at data UDF formal");

      DataMap gq_data = DataMap::from_json(R"({"K":3,"y":2,"ys":[3,1,3]})");
      bool gq_accepted = true;
      try {
        CompiledModel gq_model = compile_model(
            categorical_write_array_mir(slurp("tests/fixtures/cat.tmir.sexp"),
                                        fn, false, false, true),
            gq_data);
      } catch (const std::exception& e) {
        (void)e;
        gq_accepted = false;
      }
      check(gq_accepted, fn + " accepts GQ parameter at data UDF formal");
    }

    DataMap d;
    d.set_int("outcome", 2);
    d.set_real_array("arg", {0.2, 0.3, 0.5});
    CompiledModel model =
        compile_model(categorical_check_mir("categorical_lpmf", true, 1, 3), d);
    Executor ex(std::move(model.graph));
    model.bind(ex);
    ex.params_data()[0] = 0.75;
    double grad = 0.0;
    expect_eq("categorical check zero target", ex.gradient(&grad), 0.75);
    expect_eq("categorical check disconnected gradient", grad, 1.0);

    DataMap effect_data;
    effect_data.set_int("outcome", 2);
    effect_data.set_real_array("arg", {0.2, 0.3, 0.5});
    std::optional<CompiledModel> effect_model;
    {
      stanli_test::StdoutCapture captured;
      effect_model = compile_model(categorical_effect_check_mir(), effect_data);
      check(captured.finish().empty(),
            "categorical outcome effect absent while compiling");
    }
    Executor effect_ex(std::move(effect_model->graph));
    effect_model->bind(effect_ex);
    effect_ex.params_data()[0] = 0.25;
    for (int i = 0; i < 2; ++i) {
      stanli_test::StdoutCapture captured;
      expect_eq("categorical effect target " + std::to_string(i),
                effect_ex.gradient(&grad), 0.25);
      check(captured.finish() == "categorical effect\n",
            "categorical outcome effect executes once " + std::to_string(i));
    }
  }

  {
    // Error path: an unsupported function is reported by name, never
    // silently miscompiled. Mutate a known-good fixture's density name.
    std::string txt = slurp("tests/fixtures/es.tmir.sexp");
    const std::string from = "normal_lpdf", to = "not_a_real_fn";
    for (size_t pos = txt.find(from); pos != std::string::npos;
         pos = txt.find(from, pos + to.size()))
      txt.replace(pos, from.size(), to);
    bool threw2 = false;
    try {
      compile_model(txt, data);
    } catch (const CompileError& e) {
      threw2 = std::string(e.what()).find("not_a_real_fn") != std::string::npos;
    }
    check(threw2, "unsupported function rejected with its name");
  }

  {
    // A declared-int variable supplied with non-integer values. CmdStan's
    // var_context rejects this at read time; stanli used to bind it as a
    // typeless real entry, and the failure surfaced later as whatever
    // consumer touched it first ("gather index must be int data").
    DataMap d = DataMap::from_json(
        R"({"k": 2, "idx": [1.0, 2.5], "lo": 2, "hi": 3, "i1": 1, "j1": 2,
            "rl": 2, "rh": 3, "m": 3, "Y": [1.5, -0.5, 2.0, 0.25],
            "Zm": [[1, 5], [2, 6], [3, 7], [4, 8]]})");
    bool rejected = false;
    std::string msg;
    try {
      compile_model(slurp("tests/fixtures/oob.tmir.sexp"), d);
    } catch (const std::invalid_argument& e) {
      rejected = true;
      msg = e.what();
    } catch (const std::exception& e) {
      msg = e.what();
    }
    check(rejected, "non-int data for int variable rejected: " + msg);
    check(msg.find("int variable contained non-int values") !=
                  std::string::npos &&
              msg.find("name=idx") != std::string::npos,
          "the rejection names the int variable: " + msg);
  }

  {
    // Data whose JSON shape disagrees with its declaration. CmdStan's
    // var_context validates every declared dimension before it reads a
    // value and throws std::invalid_argument naming the variable and both
    // shapes; stanli used to walk off the end of the short array instead,
    // which surfaced as whatever std::vector::at happened to say. A host
    // distinguishing bad data from a broken model reads the exception
    // type, so the type is the contract.
    DataMap d;
    d.set_int("J", 8);
    d.set_real_array("y", std::vector<double>(kY, kY + 7));
    d.set_real_array("sigma", std::vector<double>(kSigma, kSigma + 8));
    bool rejected = false;
    std::string msg;
    try {
      compile_model(slurp("tests/fixtures/es.tmir.sexp"), d);
    } catch (const std::invalid_argument& e) {
      rejected = true;
      msg = e.what();
    } catch (const std::exception& e) {
      msg = e.what();
    }
    check(rejected, "short data array rejected as invalid_argument: " + msg);
    check(msg.find("mismatch in dimension") != std::string::npos &&
              msg.find("name=y") != std::string::npos &&
              msg.find("(8)") != std::string::npos &&
              msg.find("(7)") != std::string::npos,
          "the rejection names the variable and both shapes: " + msg);

    // Too many values is the same disagreement; reading the declared
    // prefix and dropping the rest would be silent data loss.
    DataMap wide;
    wide.set_int("J", 8);
    wide.set_real_array("y", std::vector<double>(kY, kY + 8));
    wide.set_real_array("sigma", std::vector<double>(9, 1.0));
    bool wide_rejected = false;
    try {
      compile_model(slurp("tests/fixtures/es.tmir.sexp"), wide);
    } catch (const std::invalid_argument&) {
      wide_rejected = true;
    } catch (const std::exception&) {
    }
    check(wide_rejected, "over-long data array is rejected too");
  }

  {
    // `target += <container>`. Stan defines the increment for a container
    // `e` as adding `sum(e)`; lowering used to push the container's slot
    // straight into the target terms, where a scalar consumer read element
    // zero and the rest of the container reached neither lp nor gradient.
    // Every container shape a `target +=` can name is here, plus a
    // container expression, because the truncation was in the consumer and
    // so was blind to which shape produced the value.
    DataMap d = DataMap::from_json(R"({"N": 3, "w": [1.25, 2.5, 0.75]})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tpecont.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    // Unconstrained parameters throughout, so the target terms are the
    // five increments and nothing else: no jacobian term joins the fold.
    const double q[12] = {0.3, -1.2, 0.7,  2.1, -0.4, 1.6,
                          0.9, 1.3,  -2.2, 0.5, 1.1,  -0.6};
    for (int i = 0; i < 12; ++i) lex.params_data()[i] = q[i];
    double g[12], lp = lex.gradient(g);

    using stan::math::var;
    Eigen::Matrix<var, -1, 1> v(3);
    Eigen::Matrix<var, 1, -1> r(3);
    Eigen::Matrix<var, -1, -1> M(2, 2);
    std::vector<var> a(2);
    for (int i = 0; i < 3; ++i) v(i) = q[i];
    for (int i = 0; i < 3; ++i) r(i) = q[3 + i];
    // Matrices are column-major in the slot, as they are in Eigen, so the
    // reference's redux walks the same elements in the same order.
    for (int i = 0; i < 4; ++i) M(i % 2, i / 2) = q[6 + i];
    for (int i = 0; i < 2; ++i) a[i] = q[10 + i];
    var acc = stan::math::sum(v);
    acc = acc + stan::math::sum(r);
    acc = acc + stan::math::sum(M);
    acc = acc + stan::math::sum(a);
    acc = acc + stan::math::sum(stan::math::multiply(2.0, v));
    acc.grad();
    expect_eq("tpecont lp", lp, acc.val());
    for (int i = 0; i < 3; ++i)
      expect_eq("tpecont gv" + std::to_string(i), g[i], v(i).adj());
    for (int i = 0; i < 3; ++i)
      expect_eq("tpecont gr" + std::to_string(i), g[3 + i], r(i).adj());
    for (int i = 0; i < 4; ++i)
      expect_eq("tpecont gM" + std::to_string(i), g[6 + i],
                M(i % 2, i / 2).adj());
    for (int i = 0; i < 2; ++i)
      expect_eq("tpecont ga" + std::to_string(i), g[10 + i], a[i].adj());
    stan::math::recover_memory();
  }

  {
    // The same increment reached once per loop iteration: the loop unrolls
    // into one target term per pass, each of them a container.
    DataMap d = DataMap::from_json(R"({"N": 3})");
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tpeloop.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[7] = {0.3, -1.2, 0.7, 2.1, -0.4, 1.6, 0.9};
    for (int i = 0; i < 7; ++i) lex.params_data()[i] = q[i];
    double g[7], lp = lex.gradient(g);

    using stan::math::var;
    // array[3] vector[2] is outer-major: f[n] is a contiguous pair.
    std::vector<Eigen::Matrix<var, -1, 1>> f(3, Eigen::Matrix<var, -1, 1>(2));
    for (int n = 0; n < 3; ++n)
      for (int i = 0; i < 2; ++i) f[n](i) = q[2 * n + i];
    var z = q[6];
    var acc = stan::math::sum(f[0]);
    for (int n = 1; n < 3; ++n) acc = acc + stan::math::sum(f[n]);
    acc = acc + z * 0.5;
    acc.grad();
    expect_eq("tpeloop lp", lp, acc.val());
    for (int n = 0; n < 3; ++n)
      for (int i = 0; i < 2; ++i)
        expect_eq("tpeloop gf" + std::to_string(2 * n + i), g[2 * n + i],
                  f[n](i).adj());
    expect_eq("tpeloop gz", g[6], z.adj());
    stan::math::recover_memory();
  }

  {
    // The increment inside a parameter-dependent branch, which lowering
    // compiles into an island region: its register program has to sum the
    // container too, and both arms are checked because only one of them
    // runs per evaluation.
    DataMap d;
    CompiledModel lm =
        compile_model(slurp("tests/fixtures/tpeisland.tmir.sexp"), d);
    Executor lex(std::move(lm.graph));
    lm.bind(lex);
    const double q[4] = {0.3, -1.2, 0.7, 1.5};
    for (int arm = 0; arm < 2; ++arm) {
      const double zv = arm ? -1.5 : q[3];
      for (int i = 0; i < 3; ++i) lex.params_data()[i] = q[i];
      lex.params_data()[3] = zv;
      double g[4], lp = lex.gradient(g);

      using stan::math::var;
      Eigen::Matrix<var, -1, 1> v(3);
      for (int i = 0; i < 3; ++i) v(i) = q[i];
      var acc = zv > 0.0 ? stan::math::sum(v) : stan::math::sum(-v);
      acc.grad();
      const std::string tag = "tpeisland" + std::to_string(arm);
      expect_eq(tag + " lp", lp, acc.val());
      for (int i = 0; i < 3; ++i)
        expect_eq(tag + " gv" + std::to_string(i), g[i], v(i).adj());
      expect_eq(tag + " gz", g[3], 0.0);
      stan::math::recover_memory();
    }
  }

  // append_array concatenates scalar-element and container-element arrays in
  // outer-array order. O1 also materializes the loop sequence through an
  // unsized integer-array temporary whose first assignment supplies its shape.
  {
    DataMap d;
    d.set_int_array("selected", {2, 4});
    CompiledModel am =
        compile_model(slurp("tests/fixtures/unsized_append_loop.tmir.sexp"), d);
    Executor aex(std::move(am.graph));
    am.bind(aex);
    const double q[6] = {0.5, -0.7, 0.2, 1.1, -0.4, 0.8};
    for (int i = 0; i < 6; ++i) aex.params_data()[i] = q[i];
    double gradient[6] = {};
    expect_eq("append array lp", aex.gradient(gradient),
              10.0 * q[0] + 2.0 * q[3] + 3.0 * q[4]);
    const double want[6] = {10.0, 0.0, 0.0, 2.0, 3.0, 0.0};
    for (int i = 0; i < 6; ++i)
      expect_eq("append array gradient " + std::to_string(i), gradient[i],
                want[i]);
  }

  // DataMap observations are first-index-fast, unlike the graph's outer-major
  // array slots. A rank-two append used by constant indexing must preserve the
  // former order. An empty operand also takes its suffix shape from the
  // nonempty side, as stan-math does.
  {
    DataMap d = DataMap::from_json(R"({"a":[[1,2]],"b":[[10,20]]})");
    CompiledModel am =
        compile_model(slurp("tests/fixtures/append_regression.tmir.sexp"), d);
    Executor aex(std::move(am.graph));
    am.bind(aex);
    const double q[4] = {0.1, 0.5, -0.7, 1.2};
    for (int i = 0; i < 4; ++i) aex.params_data()[i] = q[i];
    double gradient[4] = {};
    expect_eq("rank-two append lp", aex.gradient(gradient), 6.0);
    const double want[4] = {30.0, 3.0, 3.0, 3.0};
    for (int i = 0; i < 4; ++i)
      expect_eq("rank-two append gradient " + std::to_string(i), gradient[i],
                want[i]);
  }

  // The same new MIR forms can occur inside parameter-dependent control.
  // That route is compiled by ProgramCompiler rather than the main lowerer:
  // it needs Cartesian matrix gathers, O1's empty Indexed wrapper, and
  // deferred shape adoption for an unsized loop-sequence temporary.
  {
    DataMap d;
    CompiledModel im =
        compile_model(slurp("tests/fixtures/pr236_island.tmir.sexp"), d);
    Executor iex(std::move(im.graph));
    im.bind(iex);
    const double positive[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::copy(positive, positive + 9, iex.params_data());
    double gradient[9] = {};
    expect_eq("PR236 island positive lp", iex.gradient(gradient), 75.0);
    const double positive_gradient[9] = {5, 0, 0, 2, 0, 2, 2, 0, 4};
    for (int i = 0; i < 9; ++i)
      expect_eq("PR236 island positive gradient " + std::to_string(i),
                gradient[i], positive_gradient[i]);

    std::copy(positive, positive + 9, iex.params_data());
    iex.params_data()[0] = -1.0;
    std::fill(gradient, gradient + 9, 0.0);
    expect_eq("PR236 island negative lp", iex.gradient(gradient), -1.0);
    for (int i = 0; i < 9; ++i)
      expect_eq("PR236 island negative gradient " + std::to_string(i),
                gradient[i], i == 0 ? 1.0 : 0.0);
  }

  // The non-O1 producer keeps this UDF call intact. Its data-only integer
  // array formals must retain their compile-time values when ProgramCompiler
  // enters the callee, or the matrix gather cannot resolve rows and columns.
  {
    DataMap d;
    CompiledModel im =
        compile_model(slurp("tests/fixtures/pr236_island_udf.tmir.sexp"), d);
    Executor iex(std::move(im.graph));
    im.bind(iex);
    const double positive[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::copy(positive, positive + 9, iex.params_data());
    double gradient[9] = {};
    expect_eq("PR236 island UDF lp", iex.gradient(gradient), 26.0);
    const double want[9] = {0, 0, 0, 1, 0, 1, 1, 0, 1};
    for (int i = 0; i < 9; ++i)
      expect_eq("PR236 island UDF gradient " + std::to_string(i), gradient[i],
                want[i]);
  }

  // O1 also uses deferred unsized temporaries for an appended integer loop
  // sequence and for arrays of row-vector literals. Both values are built
  // inside the runtime branch, then indexed by the unrolled source loop.
  {
    DataMap d;
    d.set_int_array("d", {2});
    CompiledModel im = compile_model(
        slurp("tests/fixtures/pr236_unsized_island.tmir.sexp"), d);
    Executor iex(std::move(im.graph));
    im.bind(iex);
    iex.params_data()[0] = 0.5;
    double gradient[1] = {};
    expect_eq("PR236 unsized island positive lp", iex.gradient(gradient), 6.5);
    expect_eq("PR236 unsized island positive gradient", gradient[0], 13.0);
    iex.params_data()[0] = -0.5;
    expect_eq("PR236 unsized island negative lp", iex.gradient(gradient), 0.0);
    expect_eq("PR236 unsized island negative gradient", gradient[0], 0.0);
  }

  // matrix_exp retains the square matrix view and differentiates every output
  // element through the full, nonsymmetric matrix exponential.
  {
    DataMap d;
    CompiledModel mm =
        compile_model(slurp("tests/fixtures/matrix_exp.tmir.sexp"), d);
    Executor mex(std::move(mm.graph));
    mm.bind(mex);
    const double q[4] = {0.2, -0.4, 0.7, -0.1};
    for (int i = 0; i < 4; ++i) mex.params_data()[i] = q[i];
    double gradient[4] = {};
    const double lp = mex.gradient(gradient);

    using stan::math::var;
    Eigen::Matrix<var, -1, -1> a(2, 2);
    for (int i = 0; i < 4; ++i) a.data()[i] = q[i];
    Eigen::Matrix<var, -1, -1> e = stan::math::matrix_exp(a);
    var reference = e(0, 0) - 0.7 * e(1, 0) + 1.3 * e(0, 1) + 0.4 * e(1, 1);
    reference.grad();
    expect_eq("matrix exp lp", lp, reference.val());
    for (int i = 0; i < 4; ++i)
      expect_eq("matrix exp gradient " + std::to_string(i), gradient[i],
                a.data()[i].adj());
    stan::math::recover_memory();
  }

  // add_diag preserves the matrix layout while adding a scalar to every
  // diagonal entry.  This is the scalar overload used by ctsem; the matrix
  // input remains active so the graph path also pins its reverse pullback.
  {
    DataMap d;
    CompiledModel am =
        compile_model(slurp("tests/fixtures/add_diag.tmir.sexp"), d);
    check(count_opcode(am, OP_ADD_DIAG) == 1,
          "add_diag lowers to its native matrix kernel");
    Executor aex(std::move(am.graph));
    am.bind(aex);
    const double q[4] = {0.2, -0.4, 0.7, -0.1};
    for (int i = 0; i < 4; ++i) aex.params_data()[i] = q[i];
    double gradient[4] = {};
    const double lp = aex.gradient(gradient);

    const double want[4] = {1.0, -0.7, 1.3, 0.4};
    const double want_lp =
        q[0] + 0.5 - 0.7 * q[1] + 1.3 * q[2] + 0.4 * (q[3] + 0.5);
    expect_eq("add_diag lp", lp, want_lp);
    for (int i = 0; i < 4; ++i)
      expect_eq("add_diag gradient " + std::to_string(i), gradient[i], want[i]);
  }

  // Transformed data and write_array recovery run MirInterp rather than the
  // graph kernels. Both new functions must exist there as well. The test-only
  // hook attaches the same interpreter used by ordinary late-truncation
  // recovery beside this fixture's complete write_array graph.
  {
    DataMap d = DataMap::from_json(R"({"a":[7],"m":[[0,0],[0,0]]})");
    check(test_setenv("STANLI_WA_FORCE_INTERP", "1", 1) == 0,
          "enable new-functions write_array interpreter");
    CompiledModel fm;
    try {
      fm = compile_model(slurp("tests/fixtures/interp_new_functions.tmir.sexp"),
                         d);
    } catch (...) {
      test_unsetenv("STANLI_WA_FORCE_INTERP");
      throw;
    }
    check(test_unsetenv("STANLI_WA_FORCE_INTERP") == 0,
          "disable new-functions write_array interpreter");
    Executor fex(std::move(fm.graph));
    fm.bind(fex);
    fex.params_data()[0] = 0.3;
    double gradient[1] = {};
    expect_eq("interpreter functions lp", fex.gradient(gradient), 3.3);
    expect_eq("interpreter functions gradient", gradient[0], 11.0);

    check(fm.write_array && fm.write_array->interp,
          "new functions interpreted write_array selected");
    if (fm.write_array && fm.write_array->interp) {
      fex.params_data()[0] = 0.3;
      fex.run_forward_only();
      WaRng rng(1234);
      const std::vector<double> row =
          fm.write_array->interp->eval(fm.constrained_env(fex), rng);
      const std::vector<std::string> names =
          CompiledModel::csv_names(fm.write_array->interp->columns());
      const auto expect_column = [&](const std::string& name, double want) {
        const auto it = std::find(names.begin(), names.end(), name);
        check(it != names.end(), "interpreted column " + name);
        if (it != names.end())
          expect_eq("interpreted value " + name,
                    row.at((size_t)std::distance(names.begin(), it)), want);
      };
      expect_column("g_joined.1", 0.3);
      expect_column("g_joined.2", 2.0);
      Eigen::MatrixXd input = Eigen::MatrixXd::Zero(2, 2);
      input(0, 0) = 0.3;
      const Eigen::MatrixXd output = stan::math::matrix_exp(input);
      expect_column("g_exp.1.1", output(0, 0));
      expect_column("g_exp.2.1", output(1, 0));
      expect_column("g_exp.1.2", output(0, 1));
      expect_column("g_exp.2.2", output(1, 1));
      expect_column("product", std::exp(0.3) * std::exp(2.0));
    }
  }

  // quad_form_sym at both overloads and both scalar types: a matrix B gives
  // 0.5 * (C + C'), a vector B the scalar B' A B, and stan-math associates
  // that scalar one way at double and the other at var. `a + a'` is exactly
  // symmetric, so the symmetry check passes.
  {
    DataMap d;
    const std::vector<double> dm = {1.5, 0.3,  -0.2, 0.3, 2.0,
                                    0.7, -0.2, 0.7,  1.1};
    const std::vector<double> dvv = {0.6, -1.2, 0.4};
    d.set_real_array("d", dm);
    d.set_real_array("dv", dvv);
    CompiledModel mm =
        compile_model(slurp("tests/fixtures/quad_form_sym.tmir.sexp"), d);
    Executor mex(std::move(mm.graph));
    mm.bind(mex);
    const double q[18] = {0.2, -0.4, 0.7, -0.1, 0.9,  0.3, -0.6, 0.5,  0.8,
                          1.1, -0.3, 0.2, 0.4,  -0.9, 0.6, 0.7,  -0.5, 0.15};
    for (int i = 0; i < 18; ++i) mex.params_data()[i] = q[i];
    double gradient[18] = {};
    const double lp = mex.gradient(gradient);

    using stan::math::var;
    Eigen::Matrix<var, -1, -1> a(3, 3), b(3, 2);
    Eigen::Matrix<var, -1, 1> v(3);
    for (int i = 0; i < 9; ++i) a.data()[i] = q[i];
    for (int i = 0; i < 6; ++i) b.data()[i] = q[9 + i];
    for (int i = 0; i < 3; ++i) v.data()[i] = q[15 + i];
    Eigen::MatrixXd dmat(3, 3);
    Eigen::VectorXd dvec(3);
    for (int i = 0; i < 9; ++i) dmat.data()[i] = dm[(size_t)i];
    for (int i = 0; i < 3; ++i) dvec.data()[i] = dvv[(size_t)i];

    const Eigen::Matrix<var, -1, -1> sym = a + a.transpose();
    const Eigen::Matrix<var, -1, -1> qm = stan::math::quad_form_sym(sym, b);
    var reference = qm(0, 0) - 0.7 * qm(1, 0) + 1.3 * qm(0, 1) + 0.4 * qm(1, 1);
    reference += 0.9 * stan::math::quad_form_sym(sym, v);
    reference += 1.7 * stan::math::quad_form_sym(dmat, dvec);
    reference += 0.3 * stan::math::quad_form_sym(dmat, v);
    reference.grad();
    expect_eq("quad form sym lp", lp, reference.val());
    for (int i = 0; i < 9; ++i)
      expect_eq("quad form sym da " + std::to_string(i), gradient[i],
                a.data()[i].adj());
    for (int i = 0; i < 6; ++i)
      expect_eq("quad form sym db " + std::to_string(i), gradient[9 + i],
                b.data()[i].adj());
    for (int i = 0; i < 3; ++i)
      expect_eq("quad form sym dv " + std::to_string(i), gradient[15 + i],
                v.data()[i].adj());
    stan::math::recover_memory();
  }

  // Stan Math's reverse-mode vector overload still evaluates 0.5 * (c + c)
  // for the scalar result. Preserve that operation at overflow rather than
  // returning the finite unsymmetrized c.
  {
    DataMap d;
    d.set_real_array("d", std::vector<double>(9, 0.0));
    d.set_real_array("dv", std::vector<double>(3, 0.0));
    CompiledModel qm =
        compile_model(slurp("tests/fixtures/quad_form_sym.tmir.sexp"), d);
    Executor qex(std::move(qm.graph));
    qm.bind(qex);
    std::fill(qex.params_data(), qex.params_data() + 18, 0.0);
    qex.params_data()[0] = 0.5;
    qex.params_data()[15] = 1e154;
    const double lp = qex.forward();
    check(std::isinf(lp) && lp > 0.0,
          "quad_form_sym active-vector scalar symmetrization overflow");
  }

  // The named solves, all six, at both dividend shapes. Each reaches the
  // same stan-math call the operator spellings do, so the reference is that
  // call -- with the dividend at the Eigen shape its Stan type implies,
  // which is the distinction the vector variant bit preserves.
  {
    DataMap d = DataMap::from_json(R"({
      "dm": [[0.3, -0.2], [0.7, 0.4], [-0.5, 0.9]],
      "ds": [[4.5, 0.2, -0.3], [0.2, 3.7, 0.4], [-0.3, 0.4, 4.1]],
      "dr": [[0.6, -0.8, 0.5], [0.2, 0.9, -0.4]],
      "dt": [[2.8, 9.1, -7.3], [-0.4, 3.2, 8.2], [0.6, -0.5, 2.9]]
    })");
    CompiledModel mm =
        compile_model(slurp("tests/fixtures/mdivide_named.tmir.sexp"), d);
    Executor mex(std::move(mm.graph));
    mm.bind(mex);
    // Three families of 54 reals: four divisors, then the matrix, vector,
    // transposed-matrix and row-vector dividends they each solve against.
    constexpr int kN = 162;
    double p[kN];
    for (int i = 0; i < kN; ++i) p[i] = 0.6 * std::sin(0.7 * (i + 1));
    for (int i = 0; i < kN; ++i) mex.params_data()[i] = p[i];
    double gradient[kN] = {};
    const double lp = mex.gradient(gradient);

    using stan::math::var;
    using VarMat = Eigen::Matrix<var, -1, -1>;
    VarMat div[3][4], mat[3][2];
    Eigen::Matrix<var, -1, 1> vec[3];
    Eigen::Matrix<var, 1, -1> row[3];
    int at = 0;
    const auto take = [&](auto& m, int rows, int cols) {
      m.resize(rows, cols);
      for (int i = 0; i < rows * cols; ++i) m.data()[i] = p[at++];
    };
    for (int f = 0; f < 3; ++f) {
      for (int k = 0; k < 4; ++k) take(div[f][k], 3, 3);
      take(mat[f][0], 3, 2);
      take(vec[f], 3, 1);
      take(mat[f][1], 3, 2);
      take(row[f], 1, 3);
    }
    // The divisors the fixture builds: a dominant diagonal for the general
    // and triangular solves, symmetric positive definite for _spd.
    const auto divisor = [](const VarMat& m, bool spd) {
      VarMat out = spd ? VarMat(m + m.transpose()) : m;
      for (int i = 0; i < 3; ++i) out(i, i) = out(i, i) + (spd ? 6.0 : 4.0);
      return out;
    };
    VarMat x[3][4];
    for (int f = 0; f < 3; ++f)
      for (int k = 0; k < 4; ++k) x[f][k] = divisor(div[f][k], f == 1);
    const VarMat t0 = mat[0][1].transpose(), t1 = mat[1][1].transpose(),
                 t2 = mat[2][1].transpose();
    Eigen::MatrixXd dm(3, 2), ds(3, 3), dr(2, 3), dt(3, 3);
    dm << 0.3, -0.2, 0.7, 0.4, -0.5, 0.9;
    ds << 4.5, 0.2, -0.3, 0.2, 3.7, 0.4, -0.3, 0.4, 4.1;
    dr << 0.6, -0.8, 0.5, 0.2, 0.9, -0.4;
    dt << 2.8, 9.1, -7.3, -0.4, 3.2, 8.2, 0.6, -0.5, 2.9;

    var reference = 1.0 * stan::math::mdivide_left(x[0][0], mat[0][0])(0, 0) +
                    -0.7 * stan::math::mdivide_left(x[0][1], vec[0])(1) +
                    1.3 * stan::math::mdivide_right(t0, x[0][2])(1, 2) +
                    -0.9 * stan::math::mdivide_right(row[0], x[0][3])(0);
    reference += 1.1 * stan::math::mdivide_left_spd(x[1][0], mat[1][0])(0, 0) +
                 0.6 * stan::math::mdivide_left_spd(x[1][1], vec[1])(1) +
                 -1.7 * stan::math::mdivide_right_spd(t1, x[1][2])(1, 2) +
                 0.8 * stan::math::mdivide_right_spd(row[1], x[1][3])(0);
    reference += 0.17 * stan::math::mdivide_left_spd(x[1][0], dm)(1, 0);
    reference += -0.23 * stan::math::mdivide_left_spd(ds, mat[1][0])(2, 1);
    reference += 0.31 * stan::math::mdivide_right_spd(dr, x[1][2])(0, 1);
    reference += -0.37 * stan::math::mdivide_right_spd(t1, ds)(1, 1);
    reference +=
        0.5 * stan::math::mdivide_left_tri_low(x[2][0], mat[2][0])(0, 0) +
        -1.2 * stan::math::mdivide_left_tri_low(x[2][1], vec[2])(1) +
        0.3 * stan::math::mdivide_right_tri_low(t2, x[2][2])(1, 2) +
        1.4 * stan::math::mdivide_right_tri_low(row[2], x[2][3])(0);
    reference += 0.41 * stan::math::mdivide_left_tri_low(x[2][0], dm)(1, 0);
    reference += -0.43 * stan::math::mdivide_left_tri_low(dt, mat[2][0])(2, 1);
    reference += 0.47 * stan::math::mdivide_right_tri_low(dr, x[2][2])(0, 1);
    reference += -0.53 * stan::math::mdivide_right_tri_low(t2, dt)(1, 1);

    reference.grad();
    // Separate solve kernels accumulate their target terms one ULP apart from
    // this single reference expression; gradients still pin every overload
    // bit-for-bit, and the value remains within the project's parity budget.
    expect_ulp("mdivide named lp", lp, reference.val());
    at = 0;
    const auto compare = [&](const std::string& tag, const auto& m) {
      for (int i = 0; i < m.size(); ++i, ++at)
        expect_eq(tag + std::to_string(i), gradient[at], m.data()[i].adj());
    };
    for (int f = 0; f < 3; ++f) {
      const std::string tag = "mdivide " + std::to_string(f) + " ";
      for (int k = 0; k < 4; ++k)
        compare(tag + "d" + std::to_string(k), div[f][k]);
      compare(tag + "m0", mat[f][0]);
      compare(tag + "v", vec[f]);
      compare(tag + "m2", mat[f][1]);
      compare(tag + "r", row[f]);
    }
    stan::math::recover_memory();
  }

  if (failures == 0) std::printf("test_lower OK\n");
  return failures == 0 ? 0 : 1;
}
