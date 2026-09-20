// Matrix solve kernels are compiled separately from unrelated matrix and
// density operations so solve-template changes stay local to this family.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/packet.hpp>
#include <stan/math.hpp>
#include <type_traits>

namespace stanli {
namespace {
using MatD = Eigen::MatrixXd;
using VecD = Eigen::VectorXd;
using MapM = Eigen::Map<MatD>;
using CMapM = Eigen::Map<const MatD>;
using CMapV = Eigen::Map<const VecD>;

// ---- matrix solves: `A \ B` and `B / A` -----------------------------------
// Argument order is the operator's, so in = {A, B} for the left solve and
// {B, A} for the right one; idata = {n, k} with n the divisor's order and k
// the dividend's other extent. Output has the dividend's shape.
//
// Both operand types are part of the answer here, not just of the speed,
// because stan-math solves through whatever it is handed:
//
//   * scalar type. mdivide_left at double is prim, a FullPivLU solve; at var
//     it is the hand-written rev overload, whose value is a HouseholderQR
//     solve instead. mdivide_right has no rev overload at all, so the prim
//     template runs at whatever scalar type reaches it. Variant bit 0 says
//     the result is active, bits 2 and 3 preserve the divisor and dividend
//     scalar types independently. That distinction also selects stan-math's
//     vv/vd/dv SPD and triangular pullbacks, whose floating-point association
//     is observably different.
//   * Eigen shape. A vector dividend passed as a one-column matrix takes
//     Eigen's matrix code paths, which reassociate: 1 ULP on the value and
//     on the adjoint, measured on `A \ v`. Variant bit 1 says the dividend
//     is a vector (a column under `\`, a row under `/`) rather than a
//     matrix that happens to be narrow.
//
// The MIR interpreter makes both distinctions too -- the scalar one falls
// out of overload resolution on its own T -- which is what keeps the two
// halves of the runtime answering the same thing.
//   * factorisation family. The Stan language names three, and they are
//     different answers rather than different speeds: the plain solve
//     factors a general matrix, `_spd` takes an LLT of a symmetric positive
//     definite one, and `_tri_low` reads only the lower triangle and never
//     looks at the rest. Each gets its own opcode; `Kind` selects the call.
enum class SolveKind { Plain, Spd, TriLow };

template <bool Left, SolveKind Kind, typename A, typename B>
auto solve_at(const A& a, const B& b) {
  if constexpr (Kind == SolveKind::Spd) {
    if constexpr (Left) {
      return stan::math::mdivide_left_spd(a, b);
    } else {
      return stan::math::mdivide_right_spd(b, a);
    }
  } else if constexpr (Kind == SolveKind::TriLow) {
    if constexpr (Left) {
      return stan::math::mdivide_left_tri_low(a, b);
    } else {
      return stan::math::mdivide_right_tri_low(b, a);
    }
  } else {
    if constexpr (Left) {
      return stan::math::mdivide_left(a, b);
    } else {
      return stan::math::mdivide_right(b, a);
    }
  }
}

// The Eigen type of a dividend: a matrix, or the vector its side implies.
template <bool Left, bool Vec, typename T>
using Dividend = std::conditional_t<
    !Vec, Eigen::Matrix<T, -1, -1>,
    std::conditional_t<Left, Eigen::Matrix<T, -1, 1>, Eigen::Matrix<T, 1, -1>>>;

// Numerical part of the pinned Matrix<var> left overloads. In particular,
// calling the prim plain solve here would change QR to LU. Keep both the
// dividend's Eigen shape and the vv/dv versus vd evaluation style: these
// select different Eigen paths even when the mathematical result is equal.
template <SolveKind Kind, bool DividendVar, typename B>
auto solve_active_left_values(const MatD& a, const B& b,
                               double* qr_scratch = nullptr) {
  using Result = typename B::PlainObject;
  if constexpr (Kind == SolveKind::Spd) {
    constexpr const char* function = "mdivide_left_spd";
    stan::math::check_multiplicable(function, "A", a, "b", b);
    stan::math::check_symmetric(function, "A", a);
    stan::math::check_not_nan(function, "A", a);
    if (a.size() == 0) return Result(0, b.cols());
    const Eigen::LLT<MatD> factor(a);
    stan::math::check_pos_definite(function, "A", factor);
    Result result;
    if constexpr (DividendVar) {
      result = b;
      factor.solveInPlace(result);
    } else {
      result = factor.solve(b);
    }
    return result;
  } else if constexpr (Kind == SolveKind::TriLow) {
    constexpr const char* function = "mdivide_left_tri";
    stan::math::check_square(function, "A", a);
    stan::math::check_multiplicable(function, "A", a, "b", b);
    if (a.rows() == 0) return MatD(0, b.cols());
    // The rev overload always solves into a dynamic matrix map, including
    // vector dividends. Its vd overload instead reads the original B type.
    MatD result(b.rows(), b.cols());
    MapM out(result.data(), result.rows(), result.cols());
    const CMapM divisor(a.data(), a.rows(), a.cols());
    if constexpr (DividendVar) {
      out = b;
      out = divisor.template triangularView<Eigen::Lower>().solve(out);
    } else {
      out = divisor.template triangularView<Eigen::Lower>().solve(b);
    }
    return result;
  } else {
    constexpr const char* function = "mdivide_left";
    stan::math::check_square(function, "A", a);
    stan::math::check_multiplicable(function, "A", a, "B", b);
    if (a.size() == 0) return Result(0, b.cols());
    const Eigen::HouseholderQR<MatD> factor(a);
    if (qr_scratch) {
      // Per-execution scratch is retained and rebound by the executor and
      // numerical frames. It must not be shared by repeated calls at a site.
      MapM(qr_scratch, a.rows(), a.cols()) = factor.matrixQR();
      Eigen::Map<VecD>(qr_scratch + a.size(), a.rows()) = factor.hCoeffs();
    }
    return Result(factor.solve(b));
  }
}

template <bool Left, SolveKind Kind, bool DividendVar, bool Vec>
void solve_active_values(KernelCtx& ctx) {
  static_assert(Left || Kind == SolveKind::Spd);
  const int64_t n = ctx.idata[0], k = ctx.idata[1];
  const int ai = Left ? 0 : 1, bi = Left ? 1 : 0;
  const MatD a = CMapM(ctx.in[ai].data, n, n);
  const Dividend<Left, Vec, double> b =
      CMapM(ctx.in[bi].data, Left ? n : k, Left ? k : n);
  if constexpr (Left) {
    const auto result =
        solve_active_left_values<Kind, DividendVar>(a, b, ctx.scratch);
    std::copy_n(result.data(), result.size(), ctx.out.data);
  } else {
    // The right-SPD wrapper checks under its own name, then invokes the
    // left-SPD rev overload with A unchanged and the dividend transposed.
    constexpr const char* function = "mdivide_right_spd";
    stan::math::check_multiplicable(function, "b", b, "A", a);
    stan::math::check_symmetric(function, "A", a);
    stan::math::check_not_nan(function, "A", a);
    if (a.size() == 0) return;
    const Dividend<true, Vec, double> bt = b.transpose();
    const auto result = solve_active_left_values<Kind, DividendVar>(a, bt);
    MapM(ctx.out.data, k, n) = result.transpose();
  }
}

// Replay the exact operand scalar types on a nested tape and write the
// value out only for the right plain/triangular templates, which perform
// scalar-var arithmetic. Eligible left/SPD overloads already factor doubles;
// execute that numerical work without constructing an unused reverse tape.
template <bool Left, SolveKind Kind, bool DivisorVar, bool DividendVar,
          bool Vec>
void solve_var(KernelCtx& ctx) {
  if constexpr (Left || Kind == SolveKind::Spd) {
    solve_active_values<Left, Kind, DividendVar, Vec>(ctx);
    return;
  }
  using stan::math::var;
  static_assert(DivisorVar || DividendVar);
  using DivisorScalar = std::conditional_t<DivisorVar, var, double>;
  using DividendScalar = std::conditional_t<DividendVar, var, double>;
  const int64_t n = ctx.idata[0], k = ctx.idata[1];
  const int ai = Left ? 0 : 1, bi = Left ? 1 : 0;
  const int64_t br = Left ? n : k, bc = Left ? k : n;
  stan::math::nested_rev_autodiff nested;
  Eigen::Matrix<DivisorScalar, -1, -1> a(n, n);
  for (int64_t i = 0; i < n * n; ++i) a.data()[i] = ctx.in[ai].data[i];
  Dividend<Left, Vec, DividendScalar> b(br, bc);
  for (int64_t i = 0; i < br * bc; ++i) b.data()[i] = ctx.in[bi].data[i];
  auto out = solve_at<Left, Kind>(a, b);
  for (Eigen::Index i = 0; i < out.size(); ++i)
    ctx.out.data[i] = out.data()[i].val();
}

template <SolveKind Kind, Eigen::UpLoType Tri = Eigen::Lower>
void left_adjoint(const MatD& a, const MatD& x, const MatD& g, bool divisor_var,
                  bool dividend_var, MatD* adj_a, MatD* adj_b,
                  const double* qr_scratch = nullptr) {
  if constexpr (Kind == SolveKind::Spd) {
    const Eigen::LLT<MatD> fac(a);
    if (dividend_var) {
      const MatD y = fac.solve(g);
      if (adj_b) *adj_b += y;
      if (divisor_var) *adj_a -= y * x.transpose();
    } else if (divisor_var) {
      *adj_a -= fac.solve(MatD(g * x.transpose()));
    }
  } else if constexpr (Kind == SolveKind::TriLow) {
    const auto tri = a.template triangularView<Tri>();
    if (dividend_var) {
      const MatD y = tri.transpose().solve(g);
      if (adj_b) *adj_b += y;
      if (divisor_var) {
        MatD full = MatD::Zero(a.rows(), a.cols());
        full.template triangularView<Tri>() = -(y * x.transpose());
        *adj_a += full;
      }
    } else if (divisor_var) {
      MatD full = MatD::Zero(a.rows(), a.cols());
      full.template triangularView<Tri>() =
          -MatD(tri.transpose().solve(MatD(g * x.transpose())));
      *adj_a += full;
    }
  } else {
    const auto solve_transpose = [&](const auto& q, const auto& qr) -> MatD {
      return q * MatD(qr.template triangularView<Eigen::Upper>()
                          .transpose().solve(g));
    };
    MatD y;
    if (qr_scratch) {
      const MatD qr = CMapM(qr_scratch, a.rows(), a.cols());
      const VecD coefficients = CMapV(qr_scratch + a.size(), a.rows());
      y = solve_transpose(Eigen::householderSequence(qr, coefficients), qr);
    } else {
      // Right plain solves and direct kernel callers without scratch keep
      // the established recomputation path.
      const Eigen::HouseholderQR<MatD> qr(a);
      y = solve_transpose(qr.householderQ(), qr.matrixQR());
    }
    if (dividend_var && adj_b) *adj_b += y;
    if (divisor_var) *adj_a -= y * x.transpose();
  }
}

template <SolveKind Kind>
void right_adjoint(const MatD& a, const MatD& x, const MatD& g,
                   bool divisor_var, bool dividend_var, MatD* adj_a,
                   MatD* adj_b) {
  constexpr Eigen::UpLoType kFlipped =
      Kind == SolveKind::TriLow ? Eigen::Upper : Eigen::Lower;
  MatD adj_a_t = MatD::Zero(a.cols(), a.rows());
  MatD adj_b_t = MatD::Zero(x.cols(), x.rows());
  left_adjoint<Kind, kFlipped>(
      a.transpose(), x.transpose(), g.transpose(), divisor_var, dividend_var,
      divisor_var ? &adj_a_t : nullptr, dividend_var ? &adj_b_t : nullptr);
  if (divisor_var) {
    if constexpr (Kind == SolveKind::Spd) {
      *adj_a += adj_a_t;
    } else {
      *adj_a += adj_a_t.transpose();
    }
  }
  if (dividend_var) *adj_b += adj_b_t.transpose();
}

template <bool Left, SolveKind Kind, bool Vec>
void solve_double(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0], k = ctx.idata[1];
  const int ai = Left ? 0 : 1, bi = Left ? 1 : 0;
  const int64_t br = Left ? n : k, bc = Left ? k : n;
  MatD a = CMapM(ctx.in[ai].data, n, n);
  Dividend<Left, Vec, double> b(br, bc);
  for (int64_t i = 0; i < br * bc; ++i) b.data()[i] = ctx.in[bi].data[i];
  auto out = solve_at<Left, Kind>(a, b);
  for (Eigen::Index i = 0; i < out.size(); ++i) ctx.out.data[i] = out.data()[i];
}

template <bool Left, SolveKind Kind = SolveKind::Plain>
void solve_active_fwd(KernelCtx& ctx, bool vec) {
  // Bits 2 and 3 are the exact divisor/dividend scalar types. Activity 0 on
  // an active result is the pre-detail encoding; retain its old vv behavior
  // for an in-memory graph built by an older caller.
  switch ((ctx.variant >> 2u) & 3u) {
    case 1u:
      vec ? solve_var<Left, Kind, true, false, true>(ctx)
          : solve_var<Left, Kind, true, false, false>(ctx);
      return;
    case 2u:
      vec ? solve_var<Left, Kind, false, true, true>(ctx)
          : solve_var<Left, Kind, false, true, false>(ctx);
      return;
    default:
      vec ? solve_var<Left, Kind, true, true, true>(ctx)
          : solve_var<Left, Kind, true, true, false>(ctx);
      return;
  }
}

template <bool Left, SolveKind Kind = SolveKind::Plain>
void solve_fwd(KernelCtx& ctx) {
  const bool vec = (ctx.variant & 2u) != 0;
  // forward_value_only() is CmdStan's log_prob<double> path. Even when the
  // same graph carries adjoints for gradient(), it must take the prim solve:
  // mdivide_left's active overload uses HouseholderQR while its double
  // overload uses FullPivLU, and those are observably different answers on
  // ill-conditioned inputs.
  if ((ctx.variant & 1u) && !values_only()) {
    solve_active_fwd<Left, Kind>(ctx, vec);
  } else {
    vec ? solve_double<Left, Kind, true>(ctx)
        : solve_double<Left, Kind, false>(ctx);
  }
}

template <bool Left, SolveKind Kind = SolveKind::Plain>
void solve_bwd(KernelCtx& ctx) {
  const int64_t n = ctx.idata[0], k = ctx.idata[1];
  const int ai = Left ? 0 : 1, bi = Left ? 1 : 0;
  const int64_t br = Left ? n : k, bc = Left ? k : n;
  bool divisor_var = true, dividend_var = true;
  switch ((ctx.variant >> 2u) & 3u) {
    case 1u:
      dividend_var = false;
      break;
    case 2u:
      divisor_var = false;
      break;
    default:
      break;
  }
  const CMapM a(ctx.in[ai].data, n, n);
  const CMapM x(ctx.out.data, br, bc);
  const CMapM g(ctx.out_adj_vec.data, br, bc);
  MatD adj_a = MatD::Zero(n, n);
  MatD adj_b = MatD::Zero(br, bc);
  if constexpr (Left) {
    left_adjoint<Kind>(a, x, g, divisor_var, dividend_var,
                       divisor_var ? &adj_a : nullptr,
                       dividend_var ? &adj_b : nullptr,
                       (ctx.variant & 1u) && n > 0 ? ctx.scratch : nullptr);
  } else {
    right_adjoint<Kind>(a, x, g, divisor_var, dividend_var,
                        divisor_var ? &adj_a : nullptr,
                        dividend_var ? &adj_b : nullptr);
  }
  if (divisor_var && ctx.in_adj[ai].data)
    MapM(ctx.in_adj[ai].data, n, n) += adj_a;
  if (dividend_var && ctx.in_adj[bi].data)
    MapM(ctx.in_adj[bi].data, br, bc) += adj_b;
}

int64_t left_solve_scratch(const Op& op, const Slot*) {
  const int64_t n = op.idata[0];
  // Inactive/value-only plain solves use LU and have no QR to retain.
  return (op.variant & 1u) && n > 0 ? n * (n + 1) : 0;
}

}  // namespace

void register_solve_kernels() {
  register_kernel(OP_MDIVIDE_LEFT,
                  Kernel{solve_fwd<true>, solve_bwd<true>, left_solve_scratch});
  register_kernel(OP_MDIVIDE_RIGHT,
                  Kernel{solve_fwd<false>, solve_bwd<false>, nullptr});
  register_kernel(OP_MDIVIDE_LEFT_SPD,
                  Kernel{solve_fwd<true, SolveKind::Spd>,
                         solve_bwd<true, SolveKind::Spd>, nullptr});
  register_kernel(OP_MDIVIDE_RIGHT_SPD,
                  Kernel{solve_fwd<false, SolveKind::Spd>,
                         solve_bwd<false, SolveKind::Spd>, nullptr});
  register_kernel(OP_MDIVIDE_LEFT_TRI_LOW,
                  Kernel{solve_fwd<true, SolveKind::TriLow>,
                         solve_bwd<true, SolveKind::TriLow>, nullptr});
  register_kernel(OP_MDIVIDE_RIGHT_TRI_LOW,
                  Kernel{solve_fwd<false, SolveKind::TriLow>,
                         solve_bwd<false, SolveKind::TriLow>, nullptr});
}
}  // namespace stanli
