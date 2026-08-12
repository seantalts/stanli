#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <utility>

namespace stanli {

double wa_probe_point(int64_t i, int variant) {
  switch (variant) {
    case 1:
      return 0.02 * static_cast<double>((i % 5) - 2);
    case 2:
      return 0.0;
    default:
      return 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
  }
}

WaInterp::WaInterp(std::shared_ptr<const mir::Program> prog,
                   std::map<std::string, DataMap::Entry> base_env)
    : prog_(std::move(prog)), base_env_(std::move(base_env)) {
  for (const auto& f : prog_->fun_defs) funs_[f.name] = &f;
}

std::vector<double> WaInterp::eval(
    const std::map<std::string, DataMap::Entry>& params, WaRng& rng) {
  std::vector<double> row;
  MirInterp<double>* cur = nullptr;
  MirHooks h;
  h.stmt = [this, &cur, &params, &row](const mir::Stmt& s) {
    if (s.kind == mir::Stmt::Decl && s.read_transform)
      return read_param(*cur, s, params);
    if (s.kind == mir::Stmt::NRFunApp && s.fn_name == "FnWriteParam")
      return write_param(*cur, s, row);
    // The section guards: note the boundary and let the interpreter run
    // the statement, whose condition is false (both flags are on).
    if (!have_cols_) {
      const mir::EmitGuard eg = mir::emit_guard(s);
      if (eg == mir::EmitGuard::TransformedParams) {
        n_tp_start_ = cols_.size();
        saw_tp_ = true;
      } else if (eg == mir::EmitGuard::GeneratedQuantities) {
        n_gq_start_ = cols_.size();
        saw_gq_ = true;
      }
    }
    return false;
  };
  h.fun = [this, &cur, &rng](const mir::Expr& e, DataMap::Entry* out) {
    return rng_fun(*cur, e, out, rng) || ode_fun(*cur, e, out);
  };
  MirInterp<double> in(funs_, "write_array", std::move(h));
  cur = &in;
  in.env() = base_env_;
  in.run(prog_->generate_quantities);
  if (!have_cols_) {
    // A section with no guard of its own contributes no columns, so it
    // starts where the CSV ends -- except that a missing first guard
    // falls back to the second boundary, so the two cannot come out
    // ordered backwards.
    if (!saw_gq_) n_gq_start_ = cols_.size();
    if (!saw_tp_) n_tp_start_ = n_gq_start_;
  }
  have_cols_ = true;
  return row;
}

bool WaInterp::read_param(MirInterp<double>& in, const mir::Stmt& s,
                          const std::map<std::string, DataMap::Entry>& params) {
  auto it = params.find(s.decl_id);
  if (it == params.end())
    throw CompileError(
        "stanli write_array: no constrained value supplied "
        "for parameter " +
        s.decl_id);
  DataMap::Entry e = it->second;
  if (!s.decl_type.dims.empty()) {
    std::vector<int64_t> dims;
    int64_t len = 1;
    for (const auto& d : s.decl_type.dims) {
      dims.push_back(in.as_int(d));
      len *= dims.back();
    }
    if (len != (int64_t)std::max(e.r.size(), e.i.size()))
      throw CompileError("stanli write_array: constrained shape mismatch for " +
                         s.decl_id);
    e.dims = std::move(dims);
  }
  in.env()[s.decl_id] = std::move(e);
  return true;
}

bool WaInterp::write_param(MirInterp<double>& in, const mir::Stmt& s,
                           std::vector<double>& row) {
  const mir::Expr& v = s.fn_args.at(0);
  DataMap::Entry e = in.eval(v);
  const int64_t len = (int64_t)std::max(e.r.size(), e.i.size());
  if (!have_cols_) {
    // Arrays of containers arrive one element at a time (`theta[k]`), and
    // CmdStan names those columns outer-index-first: the index path joins
    // the column name. Same rule as the graph lowering's FnWriteParam.
    std::vector<long> ixs;
    const mir::Expr* base = &v;
    while (base->kind == mir::Expr::Indexed) {
      for (size_t k = base->args.size(); k-- > 1;) {
        if (base->args[k].name != "IndexSingle")
          throw CompileError(
              "stanli write_array: FnWriteParam under a non-scalar index");
        ixs.push_back(in.as_int(base->args[k].args[0]));
      }
      base = &base->args[0];
    }
    std::string name = base->name;
    if (name.empty()) {
      // The optimizer (--O1 constant propagation) replaced the write's
      // variable reference with the value itself, so the name is gone
      // from the statement. Writes happen in output_vars order, and a
      // substituted write is always a whole variable, so the name is the
      // output var after the last one written.
      const auto& ov = prog_->output_vars;
      size_t idx = 0;
      if (!last_written_.empty()) {
        auto it = std::find(ov.begin(), ov.end(), last_written_);
        if (it != ov.end()) idx = (size_t)(it - ov.begin()) + 1;
      }
      if (idx >= ov.size())
        throw CompileError(
            "stanli write_array: cannot name a substituted FnWriteParam");
      name = ov[idx];
    }
    last_written_ = name;
    for (auto it = ixs.rbegin(); it != ixs.rend(); ++it)
      name += "." + std::to_string(*it);
    using Naming = CompiledModel::ParamView::Naming;
    CompiledModel::ParamView pv{name, (int)row.size(), len};
    if (v.type_ == "UReal" || v.type_ == "UInt" || v.type_ == "UComplex") {
      pv.naming = Naming::Scalar;
    } else if (v.type_ == "UMatrix") {
      pv.naming = Naming::Matrix;
      pv.rows = e.dims.size() == 2 ? e.dims[0] : len;
    } else {
      pv.naming = Naming::Container;
    }
    cols_.push_back(pv);
  }
  for (int64_t k = 0; k < len; ++k)
    row.push_back(k < (int64_t)e.r.size() ? e.r[(size_t)k]
                                          : (double)e.i[(size_t)k]);
  return true;
}

namespace {

// The right-hand side handed to stan-math's integrators: the interpreter
// evaluates the model's own function at whatever times the solver picks.
// Everything is double here; generated quantities never differentiate.
struct InterpRhs {
  const std::map<std::string, const mir::FunDef*>* funs;
  const mir::FunDef* rhs;

  // Templated like ode.cpp's MirRhs: the old integrate_ode interface
  // probes var instantiations even when every input is double.
  template <typename T_y, typename T_param>
  std::vector<stan::return_type_t<T_y, T_param>> operator()(
      const double& t, const std::vector<T_y>& y,
      const std::vector<T_param>& theta, const std::vector<double>& x_r,
      const std::vector<int>& x_i, std::ostream* = nullptr) const {
    using T = stan::return_type_t<T_y, T_param>;
    std::vector<T> tv{T(t)}, yv(y.begin(), y.end()),
        thv(theta.begin(), theta.end()), xrv(x_r.begin(), x_r.end());
    MirInterp<T> ev(*funs, "ODE function");
    return ev.call(*rhs, {tv, yv, thv, xrv}, {x_i});
  }
};

}  // namespace

bool WaInterp::ode_fun(MirInterp<double>& in, const mir::Expr& e,
                       DataMap::Entry* out) {
  if (e.name.rfind("integrate_ode_", 0) != 0) return false;
  if (e.args.size() < 7)
    throw CompileError("stanli write_array: " + e.name + " arity");
  auto fit = funs_.find(e.args[0].name);
  if (fit == funs_.end())
    throw CompileError("stanli write_array: unknown right-hand side " +
                       e.args[0].name);
  const auto vec = [&](size_t k) { return in.eval(e.args[k]).r; };
  const std::vector<double> z0 = vec(1);
  const double t0 = vec(2).at(0);
  const std::vector<double> ts = vec(3);
  const std::vector<double> theta = vec(4);
  const std::vector<double> x_r = vec(5);
  DataMap::Entry xie = in.eval(e.args[6]);
  std::vector<int> x_i = xie.i;
  if (x_i.empty())
    for (double v : xie.r) x_i.push_back((int)v);
  const bool stiff = e.name.find("bdf") != std::string::npos;
  // Solver defaults match the lowering's (and stan-math's own): rk45
  // 1e-6/1e-6/1e6, bdf 1e-10/1e-10/1e8.
  double rtol = stiff ? 1e-10 : 1e-6, atol = rtol;
  long max_steps = stiff ? 100000000 : 1000000;
  if (e.args.size() >= 10) {
    rtol = vec(7).at(0);
    atol = vec(8).at(0);
    max_steps = (long)vec(9).at(0);
  }
  InterpRhs f{&funs_, fit->second};
  const auto sol =
      stiff ? stan::math::integrate_ode_bdf(f, z0, t0, ts, theta, x_r, x_i,
                                            nullptr, rtol, atol, max_steps)
            : stan::math::integrate_ode_rk45(f, z0, t0, ts, theta, x_r, x_i,
                                             nullptr, rtol, atol, max_steps);
  // array[N, S]: Fortran storage to match the interpreter's N-D indexing.
  const int64_t N = (int64_t)sol.size();
  const int64_t S = N > 0 ? (int64_t)sol[0].size() : 0;
  out->dims = {N, S};
  out->r.resize((size_t)(N * S));
  for (int64_t n = 0; n < N; ++n)
    for (int64_t k = 0; k < S; ++k)
      out->r[(size_t)(n + N * k)] = sol[(size_t)n][(size_t)k];
  return true;
}

bool WaInterp::rng_fun(MirInterp<double>& in, const mir::Expr& e,
                       DataMap::Entry* out, WaRng& rng) {
  boost::ecuyer1988& g = rng.gen();
  const std::string& f = e.name;
  if (f.size() < 5 || f.compare(f.size() - 4, 4, "_rng") != 0) return false;
  const std::string base = f.substr(0, f.size() - 4);

  std::vector<DataMap::Entry> av;
  for (const auto& a : e.args) av.push_back(in.eval(a));
  const auto sc = [&](size_t k, size_t i) -> double {
    const auto& r = av[k].r;
    return r.size() == 1 ? r[0] : r.at(i);
  };

  // Vector-valued draw from a mean vector and covariance (or Cholesky
  // factor) matrix.
  if (base == "multi_normal" || base == "multi_normal_cholesky") {
    const auto& mu = av.at(0);
    const auto& S = av.at(1);
    const int64_t K = (int64_t)mu.r.size();
    Eigen::VectorXd m(K);
    for (int64_t i = 0; i < K; ++i) m[i] = mu.r[(size_t)i];
    Eigen::MatrixXd sig(K, K);
    for (int64_t j = 0; j < K; ++j)
      for (int64_t i = 0; i < K; ++i) sig(i, j) = S.r.at((size_t)(j * K + i));
    const Eigen::VectorXd draw =
        base == "multi_normal"
            ? stan::math::multi_normal_rng(m, sig, g)
            : stan::math::multi_normal_cholesky_rng(m, sig, g);
    out->dims = {K};
    for (int64_t i = 0; i < K; ++i) out->r.push_back(draw[i]);
    return true;
  }

  // Whole-vector argument, one categorical draw.
  if (base == "categorical" || base == "categorical_logit") {
    Eigen::VectorXd th((int64_t)av.at(0).r.size());
    for (size_t i = 0; i < av[0].r.size(); ++i) th[(int64_t)i] = av[0].r[i];
    const int k = base == "categorical"
                      ? stan::math::categorical_rng(th, g)
                      : stan::math::categorical_logit_rng(th, g);
    out->is_int = true;
    out->i = {k};
    out->r = {(double)k};
    return true;
  }

  // Elementwise with scalar broadcasting: one independent draw per element,
  // matching stan-math's vectorized rng semantics.
  size_t n = 1;
  for (const auto& a : av) n = std::max(n, a.r.size());
  bool is_int = true;
  for (size_t i = 0; i < n; ++i) {
    double v = 0.0;
    int vi = 0;
    bool iv = false;
    if (base == "normal") {
      v = stan::math::normal_rng(sc(0, i), sc(1, i), g);
    } else if (base == "std_normal") {
      v = stan::math::std_normal_rng(g);
    } else if (base == "lognormal") {
      v = stan::math::lognormal_rng(sc(0, i), sc(1, i), g);
    } else if (base == "uniform") {
      v = stan::math::uniform_rng(sc(0, i), sc(1, i), g);
    } else if (base == "gamma") {
      v = stan::math::gamma_rng(sc(0, i), sc(1, i), g);
    } else if (base == "inv_gamma") {
      v = stan::math::inv_gamma_rng(sc(0, i), sc(1, i), g);
    } else if (base == "beta") {
      v = stan::math::beta_rng(sc(0, i), sc(1, i), g);
    } else if (base == "exponential") {
      v = stan::math::exponential_rng(sc(0, i), g);
    } else if (base == "chi_square") {
      v = stan::math::chi_square_rng(sc(0, i), g);
    } else if (base == "cauchy") {
      v = stan::math::cauchy_rng(sc(0, i), sc(1, i), g);
    } else if (base == "double_exponential") {
      v = stan::math::double_exponential_rng(sc(0, i), sc(1, i), g);
    } else if (base == "logistic") {
      v = stan::math::logistic_rng(sc(0, i), sc(1, i), g);
    } else if (base == "student_t") {
      v = stan::math::student_t_rng(sc(0, i), sc(1, i), sc(2, i), g);
    } else if (base == "weibull") {
      v = stan::math::weibull_rng(sc(0, i), sc(1, i), g);
    } else if (base == "bernoulli") {
      vi = stan::math::bernoulli_rng(sc(0, i), g);
      iv = true;
    } else if (base == "bernoulli_logit") {
      vi = stan::math::bernoulli_logit_rng(sc(0, i), g);
      iv = true;
    } else if (base == "binomial") {
      vi = stan::math::binomial_rng((int)sc(0, i), sc(1, i), g);
      iv = true;
    } else if (base == "poisson") {
      vi = stan::math::poisson_rng(sc(0, i), g);
      iv = true;
    } else if (base == "poisson_log") {
      vi = stan::math::poisson_log_rng(sc(0, i), g);
      iv = true;
    } else if (base == "neg_binomial_2") {
      vi = stan::math::neg_binomial_2_rng(sc(0, i), sc(1, i), g);
      iv = true;
    } else if (base == "neg_binomial_2_log") {
      vi = stan::math::neg_binomial_2_log_rng(sc(0, i), sc(1, i), g);
      iv = true;
    } else {
      return false;
    }
    if (iv) {
      out->i.push_back(vi);
      out->r.push_back((double)vi);
    } else {
      is_int = false;
      out->r.push_back(v);
    }
  }
  out->is_int = is_int;
  if (!is_int) out->i.clear();
  if (n > 1) out->dims = {(int64_t)n};
  return true;
}

}  // namespace stanli
