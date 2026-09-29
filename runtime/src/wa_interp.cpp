#include <stanli/wa_interp.hpp>

#include <stanli/function_registry.hpp>
#include <stanli/higher_order_eval.hpp>
#include <stanli/mir_interp.hpp>

#include <stan/math.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <limits>
#include <utility>

namespace stanli {

const ScalarRng* scalar_rng_family(const std::string& name) {
  // The unified FunctionSpec registry owns the name-to-family mapping; the
  // enum-keyed arity and integer-result helpers below stay the single
  // statement of each family's properties, which registration reuses.
  const FunctionSpec* spec = function_spec(name, FunctionFamily::Builtin);
  if (spec == nullptr || spec->builtin() == nullptr ||
      spec->builtin()->shape != BuiltinShapePolicy::Rng)
    return nullptr;
  return &spec->builtin()->rng;
}

size_t scalar_rng_arity(ScalarRng family) {
  switch (family) {
    case ScalarRng::StdNormal:
      return 0;
    case ScalarRng::ChiSquare:
    case ScalarRng::PoissonLog:
    case ScalarRng::Bernoulli:
    case ScalarRng::Exponential:
    case ScalarRng::Poisson:
    case ScalarRng::BernoulliLogit:
      return 1;
    case ScalarRng::Uniform:
    case ScalarRng::Normal:
    case ScalarRng::Lognormal:
    case ScalarRng::Binomial:
    case ScalarRng::Gumbel:
    case ScalarRng::Gamma:
    case ScalarRng::InvGamma:
    case ScalarRng::Beta:
    case ScalarRng::Cauchy:
    case ScalarRng::DoubleExponential:
    case ScalarRng::Logistic:
    case ScalarRng::Weibull:
    case ScalarRng::NegBinomial2:
    case ScalarRng::NegBinomial2Log:
      return 2;
    case ScalarRng::BetaBinomial:
    case ScalarRng::StudentT:
      return 3;
  }
  throw std::logic_error("unknown scalar RNG family");
}

bool scalar_rng_is_int(ScalarRng family) {
  return family == ScalarRng::PoissonLog || family == ScalarRng::Bernoulli ||
         family == ScalarRng::Poisson || family == ScalarRng::BernoulliLogit ||
         family == ScalarRng::Binomial || family == ScalarRng::BetaBinomial ||
         family == ScalarRng::NegBinomial2 ||
         family == ScalarRng::NegBinomial2Log;
}

double scalar_rng_draw(ScalarRng family, const double* args, size_t nargs,
                       WaRng& rng) {
  if (nargs != scalar_rng_arity(family) || (nargs != 0 && args == nullptr))
    throw std::logic_error("malformed scalar RNG arguments");
  stan::rng_t& g = rng.gen();
  switch (family) {
    case ScalarRng::StdNormal:
      return stan::math::std_normal_rng(g);
    case ScalarRng::Gamma:
      return stan::math::gamma_rng(args[0], args[1], g);
    case ScalarRng::InvGamma:
      return stan::math::inv_gamma_rng(args[0], args[1], g);
    case ScalarRng::Beta:
      return stan::math::beta_rng(args[0], args[1], g);
    case ScalarRng::ChiSquare:
      return stan::math::chi_square_rng(args[0], g);
    case ScalarRng::Cauchy:
      return stan::math::cauchy_rng(args[0], args[1], g);
    case ScalarRng::DoubleExponential:
      return stan::math::double_exponential_rng(args[0], args[1], g);
    case ScalarRng::Logistic:
      return stan::math::logistic_rng(args[0], args[1], g);
    case ScalarRng::Weibull:
      return stan::math::weibull_rng(args[0], args[1], g);
    case ScalarRng::NegBinomial2:
      return stan::math::neg_binomial_2_rng(args[0], args[1], g);
    case ScalarRng::NegBinomial2Log:
      return stan::math::neg_binomial_2_log_rng(args[0], args[1], g);
    case ScalarRng::PoissonLog:
      return static_cast<double>(stan::math::poisson_log_rng(args[0], g));
    case ScalarRng::Uniform:
      return stan::math::uniform_rng(args[0], args[1], g);
    case ScalarRng::Bernoulli:
      return static_cast<double>(stan::math::bernoulli_rng(args[0], g));
    case ScalarRng::Normal:
      return stan::math::normal_rng(args[0], args[1], g);
    case ScalarRng::Lognormal:
      return stan::math::lognormal_rng(args[0], args[1], g);
    case ScalarRng::Binomial:
      return static_cast<double>(
          stan::math::binomial_rng(static_cast<int>(args[0]), args[1], g));
    case ScalarRng::Gumbel:
      return stan::math::gumbel_rng(args[0], args[1], g);
    case ScalarRng::BetaBinomial:
      return static_cast<double>(stan::math::beta_binomial_rng(
          static_cast<int>(args[0]), args[1], args[2], g));
    case ScalarRng::Exponential:
      return stan::math::exponential_rng(args[0], g);
    case ScalarRng::Poisson:
      return static_cast<double>(stan::math::poisson_rng(args[0], g));
    case ScalarRng::StudentT:
      return stan::math::student_t_rng(args[0], args[1], args[2], g);
    case ScalarRng::BernoulliLogit:
      return static_cast<double>(stan::math::bernoulli_logit_rng(args[0], g));
  }
  throw std::logic_error("unknown scalar RNG family");
}

int vector_integer_rng_draw(const double* probabilities, size_t size,
                            WaRng& rng, uint8_t variant) {
  if (size != 0 && probabilities == nullptr)
    throw std::logic_error("malformed categorical RNG arguments");
  Eigen::VectorXd theta(static_cast<Eigen::Index>(size));
  for (size_t i = 0; i < size; ++i)
    theta[static_cast<Eigen::Index>(i)] = probabilities[i];
  switch (variant) {
    case kCategoricalRngVariant:
      return stan::math::categorical_rng(theta, rng.gen());
    case kCategoricalLogitRngVariant:
      // Upstream indexes its cumulative sum even for an empty input. Reject
      // that invalid distribution before it consumes the stream or reads OOB.
      stan::math::check_nonzero_size("categorical_logit_rng",
                                     "Log odds parameter", theta);
      return stan::math::categorical_logit_rng(theta, rng.gen());
    case kPoissonBinomialRngVariant:
      return stan::math::poisson_binomial_rng(theta, rng.gen());
    default:
      throw std::logic_error("unknown vector-to-integer RNG family");
  }
}

void multi_normal_rng_draw(const double* location, size_t location_size,
                           const double* covariance, size_t covariance_size,
                           size_t covariance_rows, size_t covariance_cols,
                           double* output, size_t output_size, WaRng& rng,
                           bool cholesky) {
  if ((location_size != 0 && location == nullptr) ||
      (covariance_size != 0 && covariance == nullptr) ||
      (output_size != 0 && output == nullptr) || output_size != location_size ||
      (covariance_rows != 0 &&
       covariance_cols >
           std::numeric_limits<size_t>::max() / covariance_rows) ||
      covariance_rows * covariance_cols != covariance_size)
    throw std::logic_error("malformed multi-normal RNG arguments");

  Eigen::VectorXd mu(static_cast<Eigen::Index>(location_size));
  for (size_t i = 0; i < location_size; ++i)
    mu[static_cast<Eigen::Index>(i)] = location[i];
  Eigen::MatrixXd sigma(static_cast<Eigen::Index>(covariance_rows),
                        static_cast<Eigen::Index>(covariance_cols));
  for (size_t j = 0; j < covariance_cols; ++j)
    for (size_t i = 0; i < covariance_rows; ++i)
      sigma(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) =
          covariance[j * covariance_rows + i];

  const Eigen::VectorXd draw =
      cholesky ? stan::math::multi_normal_cholesky_rng(mu, sigma, rng.gen())
               : stan::math::multi_normal_rng(mu, sigma, rng.gen());
  for (size_t i = 0; i < output_size; ++i)
    output[i] = draw[static_cast<Eigen::Index>(i)];
}

void dirichlet_rng_draw(const double* alpha, size_t alpha_size, double* output,
                        size_t output_size, WaRng& rng) {
  if ((alpha_size != 0 && alpha == nullptr) ||
      (output_size != 0 && output == nullptr) || output_size != alpha_size)
    throw std::logic_error("malformed dirichlet RNG arguments");
  Eigen::VectorXd a(static_cast<Eigen::Index>(alpha_size));
  for (size_t i = 0; i < alpha_size; ++i)
    a[static_cast<Eigen::Index>(i)] = alpha[i];
  const Eigen::VectorXd draw = stan::math::dirichlet_rng(a, rng.gen());
  for (size_t i = 0; i < output_size; ++i)
    output[i] = draw[static_cast<Eigen::Index>(i)];
}

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
  if (!have_cols_) {
    // A failed discovery may have emitted a prefix. Start the next attempt
    // afresh; otherwise retries duplicate columns before their first success.
    cols_.clear();
    n_tp_start_ = n_gq_start_ = 0;
    saw_tp_ = saw_gq_ = false;
    last_written_.clear();
  }
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
  h.fun = [this, &rng](MirInterp<double>& in, const mir::Expr& e,
                       DataMap::Entry* out) {
    return interpreted_rng_call(in, e, out, rng) ||
           evaluate_retained_higher_order(
               funs_, e, [&in](const mir::Expr& arg) { return in.eval(arg); },
               out);
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
    have_cols_ = true;
  }
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

bool interpreted_rng_call(MirInterp<double>& in, const mir::Expr& e,
                          DataMap::Entry* out, WaRng& rng) {
  stan::rng_t& g = rng.gen();
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
  // factor) matrix. Admitted shapes share their owning-Eigen helper with
  // OP_RNG so validation and engine schedules cannot drift between modes.
  if (base == "multi_normal" || base == "multi_normal_cholesky") {
    const auto& mu = av.at(0);
    const auto& S = av.at(1);
    const int64_t K = (int64_t)mu.r.size();
    out->dims = {K};
    out->r.resize(static_cast<size_t>(K));
    if (base == "multi_normal" ||
        (S.dims.size() == 2 && S.dims[0] == K && S.dims[1] == K)) {
      if (S.dims.size() != 2)
        throw std::logic_error("malformed multi-normal covariance shape");
      multi_normal_rng_draw(
          mu.r.data(), mu.r.size(), S.r.data(), S.r.size(),
          static_cast<size_t>(S.dims[0]), static_cast<size_t>(S.dims[1]),
          out->r.data(), out->r.size(), rng, base == "multi_normal_cholesky");
    } else {
      // Preserve the legacy adapter outside the square-vector contract.
      // Rectangular and vectorized shapes need a separate shape migration.
      Eigen::VectorXd m(K);
      for (int64_t i = 0; i < K; ++i) m[i] = mu.r[(size_t)i];
      Eigen::MatrixXd sig(K, K);
      for (int64_t j = 0; j < K; ++j)
        for (int64_t i = 0; i < K; ++i) sig(i, j) = S.r.at((size_t)(j * K + i));
      const Eigen::VectorXd draw =
          stan::math::multi_normal_cholesky_rng(m, sig, g);
      for (int64_t i = 0; i < K; ++i) out->r[static_cast<size_t>(i)] = draw[i];
    }
    return true;
  }

  // Whole-vector concentration argument, one simplex draw.
  if (base == "dirichlet") {
    const auto& alpha = av.at(0);
    const int64_t K = (int64_t)alpha.r.size();
    out->dims = {K};
    out->r.resize(static_cast<size_t>(K));
    dirichlet_rng_draw(alpha.r.data(), alpha.r.size(), out->r.data(),
                       out->r.size(), rng);
    return true;
  }

  // Whole-vector arguments with one integer draw. Use the same owning
  // Eigen instantiation and validation in every execution path.
  if (base == "poisson_binomial" || base == "categorical" ||
      base == "categorical_logit") {
    const uint8_t variant =
        base == "poisson_binomial"    ? kPoissonBinomialRngVariant
        : base == "categorical_logit" ? kCategoricalLogitRngVariant
                                      : kCategoricalRngVariant;
    const int k = vector_integer_rng_draw(av.at(0).r.data(), av.at(0).r.size(),
                                          rng, variant);
    out->is_int = true;
    out->i = {k};
    out->r = {static_cast<double>(k)};
    return true;
  }

  const ScalarRng* family = scalar_rng_family(f);
  if (!family) return false;
  const size_t arity = scalar_rng_arity(*family);
  if (av.size() != arity)
    throw std::logic_error("malformed scalar RNG arguments");

  // Elementwise with scalar broadcasting: one independent draw per element,
  // matching stan-math's vectorized rng semantics.
  size_t n = 1;
  for (const auto& a : av) n = std::max(n, a.r.size());
  bool is_int = true;
  for (size_t i = 0; i < n; ++i) {
    double v = 0.0;
    int vi = 0;
    bool iv = false;
    double args[3]{};
    for (size_t k = 0; k < arity; ++k) args[k] = sc(k, i);
    v = scalar_rng_draw(*family, args, arity, rng);
    iv = scalar_rng_is_int(*family);
    if (iv) vi = static_cast<int>(v);
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
