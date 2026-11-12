// Matrix solves: `A \ B` and `B / A`.
//
// stanc3 spells both with the ordinary division operators, so nothing in the
// MIR name says "solve" -- only the divisor's type does. Getting that rule
// wrong is silent: `B / A` reads as elementwise division, every shape lines
// up, and the model returns a wrong density with a wrong gradient. So this
// pins three things at once.
//
//   * the discriminator: a matrix divisor is a solve, a scalar divisor and
//     `./` are not, and the graph must pick the same one the MIR
//     interpreter picks (mir_interp.hpp, `LDivide__`);
//   * the numbers: lp and gradient against a var reference that calls the
//     same stan::math functions CmdStan's generated code would, in the same
//     order, for all four shapes -- matrix/matrix, row_vector/matrix,
//     matrix\vector, matrix\matrix -- with a data divisor and a parameter
//     divisor, so both operand adjoints are exercised;
//   * generated quantities agreeing before an effect: solve.stan and
//     solverng.stan have identical deterministic columns except for one
//     trailing scalar RNG draw. The shared columns must remain bitwise equal,
//     and the compiled draw must consume the caller's exact Stan stream.
#include "expect.hpp"

#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

// The draw both halves of the test run at. Spread so no two parameters
// share a value and A + B stays well away from singular.
std::vector<double> draw() {
  std::vector<double> q(21);
  for (size_t i = 0; i < q.size(); ++i) q[i] = 0.07 * (double)i - 0.6;
  return q;
}

using stan::math::var;
using VarM = Eigen::Matrix<var, -1, -1>;

// The model block of tests/fixtures/solve.stan, term for term and in
// declaration order, on the stan::math calls CmdStan's generated code makes.
double reference(const std::vector<double>& q, const Eigen::MatrixXd& A,
                 std::vector<double>& grad) {
  VarM B(3, 3);
  Eigen::Matrix<var, 1, -1> rv(3);
  Eigen::Matrix<var, -1, 1> v(3);
  VarM W(3, 2);
  for (int i = 0; i < 9; ++i) B.data()[i] = q[(size_t)i];
  for (int i = 0; i < 3; ++i) rv(i) = q[(size_t)(9 + i)];
  for (int i = 0; i < 3; ++i) v(i) = q[(size_t)(12 + i)];
  for (int i = 0; i < 6; ++i) W.data()[i] = q[(size_t)(15 + i)];
  VarM P = stan::math::add(A, B);
  var acc = stan::math::sum(stan::math::mdivide_right(B, A));
  acc += stan::math::sum(stan::math::mdivide_right(rv, A));
  acc += stan::math::sum(stan::math::mdivide_left(A, v));
  acc += stan::math::sum(stan::math::mdivide_left(A, W));
  acc += stan::math::sum(stan::math::mdivide_right(rv, P));
  acc += stan::math::sum(stan::math::mdivide_left(P, W));
  acc += stan::math::sum(stan::math::divide(B, 2.0));
  acc += stan::math::sum(stan::math::elt_divide(B, A));
  acc.grad();
  grad.assign(21, 0.0);
  for (int i = 0; i < 9; ++i) grad[(size_t)i] = B.data()[i].adj();
  for (int i = 0; i < 3; ++i) grad[(size_t)(9 + i)] = rv(i).adj();
  for (int i = 0; i < 3; ++i) grad[(size_t)(12 + i)] = v(i).adj();
  for (int i = 0; i < 6; ++i) grad[(size_t)(15 + i)] = W.data()[i].adj();
  const double lp = acc.val();
  stan::math::recover_memory();
  return lp;
}

}  // namespace

int main() {
  using namespace stanli;
  const DataMap d = DataMap::from_json(slurp("tests/fixtures/solve.json"));
  Eigen::MatrixXd A(3, 3);
  A << 2.0, 0.5, -1.0, 1.0, 3.0, 0.25, -0.5, 0.75, 4.0;
  const std::vector<double> q = draw();

  // forward_value_only() is the model's double instantiation, not merely a
  // gradient graph with its backward sweep skipped. That distinction matters
  // for mdivide_left: stan-math's active overload uses HouseholderQR, while
  // the prim overload uses FullPivLU. A Hilbert matrix makes the two value
  // paths observably different and pins each Executor entry point to the one
  // CmdStan calls.
  {
    constexpr int n = 5;
    Eigen::MatrixXd a(n, n);
    Eigen::VectorXd b(n);
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) a(i, j) = 1.0 / (i + j + 1.0);
    b << 1.0, -2.0, 3.0, -4.0, 5.0;

    const double double_ref = stan::math::sum(stan::math::mdivide_left(a, b));
    Eigen::Matrix<var, -1, -1> av = a.cast<var>();
    Eigen::Matrix<var, -1, 1> bv = b.cast<var>();
    const var active = stan::math::sum(stan::math::mdivide_left(av, bv));
    const double active_ref = active.val();
    check(double_ref != active_ref,
          "value-only solve fixture distinguishes LU from QR");
    stan::math::recover_memory();

    Graph g;
    const int as = g.add_slot(n * n, true);
    const int bs = g.add_slot(n, true);
    const int solved = g.add_slot(n, false);
    const int total = g.add_slot(1, false);
    g.add_op(OP_MDIVIDE_LEFT, {as, bs}, solved, {n, 1});
    g.ops.back().variant = 1u | 2u | 4u | 8u;
    g.add_op(OP_SUM_VEC, {solved}, total);
    g.result_slot = total;
    Executor sex(std::move(g));
    for (int i = 0; i < n * n; ++i) sex.params_data()[i] = a.data()[i];
    for (int i = 0; i < n; ++i) sex.params_data()[n * n + i] = b(i);
    expect_eq("active solve forward", sex.forward(), active_ref);
    expect_eq("active solve value-only", sex.forward_value_only(), double_ref);
  }

  CompiledModel cm = compile_model(slurp("tests/fixtures/solve.tmir.sexp"), d);
  check(cm.n_unconstrained == 21, "solve 21 unconstrained");

  // The discriminator, read back off the graph. Eight divisions in the
  // model block: six solves (three each way, covering all four shapes) and
  // the two that stay elementwise, `B / 2.0` and `B ./ A`.
  std::map<std::string, int> ops;
  for (const Op& op : cm.graph.ops) ops[opcode_name(op.opcode)]++;
  check(ops["OP_MDIVIDE_RIGHT"] == 3,
        "three right solves, got " + std::to_string(ops["OP_MDIVIDE_RIGHT"]));
  check(ops["OP_MDIVIDE_LEFT"] == 3,
        "three left solves, got " + std::to_string(ops["OP_MDIVIDE_LEFT"]));
  check(ops["OP_DIV"] == 2,
        "two elementwise divisions, got " + std::to_string(ops["OP_DIV"]));

  // lp and gradient.
  {
    Executor ex(std::move(cm.graph));
    cm.bind(ex);
    for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
    std::vector<double> grad(21, 0.0);
    const double lp = ex.gradient(grad.data());
    std::vector<double> gref;
    const double lpref = reference(q, A, gref);
    expect_eq("lp", lp, lpref);
    // The project's 2 ULP budget, for the gradient only. The solve kernels
    // are the nested-tape tier: each replays its own call on a fresh tape, so
    // an input whose adjoint has more than one contributor gets those
    // contributions summed as (0+c1)+(0+c2) where one CmdStan tape sums them
    // as ((s+c1)+c2). Measured on this fixture: 2 ULP on one entry of dB,
    // which reaches two solves through the transformed parameter P. lp and
    // every write_array column are bitwise and are checked as such.
    for (size_t i = 0; i < grad.size(); ++i)
      expect_ulp("grad[" + std::to_string(i) + "]", grad[i], gref[i], 2);
  }

  // Generated quantities: the whole section lowers, so the graph -- not the
  // per-draw interpreter -- produces the CSV row. That is the point of
  // giving the solves kernels at all; a truncated write_array drags the
  // entire section through mir_interp.hpp for every draw.
  check(cm.write_array.has_value(), "solve has a write_array");
  check(cm.write_array->truncated.empty(),
        "solve write_array not truncated: " + cm.write_array->truncated);
  check(cm.write_array->interp == nullptr, "solve needs no interpreter");

  const auto names = CompiledModel::csv_names(cm.write_array->columns);
  std::vector<double> row;
  {
    Executor wex(std::move(cm.write_array->graph));
    cm.write_array->bind(wex);
    for (size_t i = 0; i < q.size(); ++i) wex.params_data()[i] = q[i];
    wex.run_forward_only();
    for (const auto& c : cm.write_array->columns) {
      const double* p = wex.value_ptr(c.slot);
      for (int64_t k = 0; k < c.len; ++k) row.push_back(p[k]);
    }
  }
  check(row.size() == names.size(), "one value per column name");

  // Generated quantities run at double, so the reference is the prim call
  // -- which is also exactly what mir_interp.hpp evaluates.
  {
    Eigen::VectorXd vd(3);
    Eigen::RowVectorXd rvd(3);
    for (int i = 0; i < 3; ++i) rvd(i) = q[(size_t)(9 + i)];
    for (int i = 0; i < 3; ++i) vd(i) = q[(size_t)(12 + i)];
    const Eigen::VectorXd gv = stan::math::mdivide_left(A, vd);
    const Eigen::RowVectorXd grv = stan::math::mdivide_right(rvd, A);
    std::map<std::string, double> by_name;
    for (size_t k = 0; k < names.size() && k < row.size(); ++k)
      by_name[names[k]] = row[k];
    for (int i = 0; i < 3; ++i) {
      const std::string ix = std::to_string(i + 1);
      check(by_name.count("gv." + ix) == 1, "gv." + ix + " is a column");
      check(by_name.count("grv." + ix) == 1, "grv." + ix + " is a column");
      expect_eq("gv." + ix, by_name["gv." + ix], gv(i));
      expect_eq("grv." + ix, by_name["grv." + ix], grv(i));
    }
  }

  // The same generated quantities plus a trailing graph-native normal draw.
  // Every deterministic column must remain bitwise equal, and reseeding the
  // caller-owned stream must reproduce the stochastic column.
  {
    CompiledModel im =
        compile_model(slurp("tests/fixtures/solverng.tmir.sexp"), d);
    check(im.write_array && im.write_array->truncated.empty(),
          "solverng write_array compiles");
    check(im.write_array && im.write_array->interp == nullptr,
          "solverng stays on graph");
    Executor ex(std::move(im.write_array->graph));
    im.write_array->bind(ex);
    for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
    WaRng rng(1);
    const auto draw = [&] {
      ex.run_forward_only(EvalState{&rng});
      std::vector<double> values;
      for (const auto& c : im.write_array->columns) {
        const double* p = ex.value_ptr(c.slot);
        for (int64_t i = 0; i < c.len; ++i)
          values.push_back(p[c.storage_index(i)]);
      }
      return values;
    };
    const std::vector<double> irow = draw();
    const auto inames = CompiledModel::csv_names(im.write_array->columns);
    check(irow.size() == inames.size(), "solverng row matches its names");
    // solverng adds one trailing column; everything before it is shared.
    check(inames.size() == names.size() + 1, "one extra RNG column");
    for (size_t k = 0; k < names.size() && k < irow.size(); ++k) {
      check(inames[k] == names[k], "column " + std::to_string(k) + " name");
      expect_eq("solverng " + names[k], irow[k], row[k]);
    }
    const std::vector<double> second = draw();
    rng.seed(1);
    const std::vector<double> reseeded = draw();
    check(irow != second, "solverng stream advances");
    check(irow == reseeded, "solverng reseed reproduces row");
  }

  return finish("test_solve");
}
