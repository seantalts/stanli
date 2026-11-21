#include <stanli/rejection.hpp>
#include <stanli/estimate.hpp>

#include "initialize.hpp"
#include "service_model.hpp"

#include <stan/callbacks/interrupt.hpp>
#include <stan/callbacks/logger.hpp>
#include <stan/callbacks/structured_writer.hpp>
#include <stan/callbacks/writer.hpp>
#include <stan/io/array_var_context.hpp>
#include <stan/io/empty_var_context.hpp>
#include <stan/services/pathfinder/single.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <type_traits>

namespace stanli {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Pathfinder writes each draw as an Eigen ROW vector
// (`Eigen::Matrix<double, 1, -1>`), and every operator() on
// stan::callbacks::writer is a no-op unless overridden, so a collector
// has to override that form. The header row arrives as a vector of strings,
// and write_times afterwards uses the string and no-argument forms, which
// stay no-ops.
//
// Row layout, fixed by the service: lp_approx__, lp__, path__, then one
// column per constrained parameter. With no write_array attached the
// model's write_array is the identity, so those columns are the
// unconstrained draw.
class DrawCollector : public stan::callbacks::writer {
 public:
  // Every base overload stays reachable: declaring one hides the rest,
  // and write_times uses the string and no-argument forms.
  using stan::callbacks::writer::operator();

  void operator()(const std::vector<std::string>& names) override {
    n_params_ = (int)names.size() - kLeadingColumns;
  }
  void operator()(const Eigen::Matrix<double, 1, -1>& row) override {
    if (row.size() < kLeadingColumns) return;
    lp_approx.push_back(row(0));
    lp.push_back(row(1));
    draws.emplace_back(row.data() + kLeadingColumns, row.data() + row.size());
  }

  int n_params() const { return n_params_; }

  std::vector<std::vector<double>> draws;
  std::vector<double> lp;
  std::vector<double> lp_approx;

 private:
  static constexpr int kLeadingColumns = 3;  // lp_approx__, lp__, path__
  int n_params_ = 0;
};

// The L-BFGS path, from the service's save_iterations diagnostics.
//
// Records arrive as they are produced, so the observer fires live. Stan
// keys a record by the iteration just completed but fills it with the
// point from BEFORE that step, so record k holds iterate k-1 and record
// "0" holds the same starting point as record 1. That also means the
// optimizer's final point is never written: the path ends one iterate
// short, which is why run_pathfinder clamps the selected index into it.
class PathCollector : public stan::callbacks::structured_writer {
 public:
  PathCollector(Executor& ex, const PathObserver& observe)
      : ex_(&ex), observe_(observe) {}

  // The service writes a dozen other keys through the base no-ops.
  using stan::callbacks::structured_writer::begin_record;
  using stan::callbacks::structured_writer::write;

  // Stan 2.40 uses fixed-width writer overloads, while Pathfinder emits
  // size_t/long too. On macOS those are distinct types: forward integrals
  // explicitly so overload resolution remains unambiguous on every host.
  template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
  void write(const std::string& key, T value) {
    if constexpr (std::is_signed_v<T>)
      stan::callbacks::structured_writer::write(key,
                                                static_cast<long long>(value));
    else
      stan::callbacks::structured_writer::write(key,
                                                static_cast<uint64_t>(value));
  }

  void begin_record(const std::string& key) override {
    record_ = std::atoi(key.c_str());
  }
  // A log density the service computed while this record was open. During
  // an iteration's record those are its ELBO draws.
  void note_lp(double lp) { lp_by_record_[record_].push_back(lp); }
  // The record whose first lp values are `elbo_lp`, bit for bit: the
  // iteration whose ELBO draws the service returned. -1 when none is.
  int record_with_lp(const std::vector<double>& elbo_lp) const {
    const auto same = [](double a, double b) {
      return std::memcmp(&a, &b, sizeof a) == 0;
    };
    for (const auto& [record, lps] : lp_by_record_)
      if (record > 0 && lps.size() >= elbo_lp.size() &&
          std::equal(elbo_lp.begin(), elbo_lp.end(), lps.begin(), same))
        return record;
    return -1;
  }
  void write(const std::string& key, const Eigen::VectorXd& vec) override {
    if (key != "unconstrained_parameters") return;
    const int iter = record_ > 0 ? record_ - 1 : 0;
    if (iter != (int)path.size()) return;  // record 1 repeats record 0
    PathIterate it;
    it.iter = iter;
    it.lp = lp_at(vec);
    path.push_back(it);
    if (observe_) observe_(it);
  }

  std::vector<PathIterate> path;

 private:
  double lp_at(const Eigen::VectorXd& q) const {
    // Safe between L-BFGS steps: every evaluation writes the whole
    // parameter buffer before reading it, so borrowing it here cannot
    // leave the optimizer looking at someone else's point.
    for (Eigen::Index i = 0; i < q.size(); ++i) ex_->params_data()[i] = q(i);
    try {
      return ex_->forward();
    } catch (const std::exception& e) {
      if (ends_run(e)) throw;
      return -std::numeric_limits<double>::infinity();
    }
  }

  Executor* ex_;
  PathObserver observe_;
  int record_ = 0;
  std::map<int, std::vector<double>> lp_by_record_;
};

// Errors are kept for PathfinderResult::message.
class PathLogger : public stan::callbacks::logger {
 public:
  void error(const std::string& message) override {
    if (!errors.empty()) errors += "; ";
    errors += message;
  }
  void error(const std::stringstream& message) override {
    error(message.str());
  }

  std::string errors;
};

// util::initialize reaches an explicit init only through transform_inits,
// which ExecutorModel refuses because the adapter holds no MIR section to
// invert with. The point here is already unconstrained -- a caller starting
// from constrained values unconstrains at its own boundary -- so it needs no
// transform at all.
class PathfinderModel : public ExecutorModel {
 public:
  PathfinderModel(Executor& ex, const double* init, PathCollector& path)
      : ExecutorModel(ex), init_(init), path_(path) {}

  // The service's ELBO and draw evaluations are this call, with no
  // gradient. Each value goes to the path record it was computed under.
  template <bool propto, bool jacobian, typename T>
  T log_prob(Eigen::Matrix<T, -1, 1>& q, std::ostream* msgs) const {
    const T lp = ExecutorModel::log_prob<propto, jacobian, T>(q, msgs);
    if constexpr (std::is_same_v<T, double> && !propto && jacobian)
      path_.note_lp(lp);
    return lp;
  }
  template <bool propto, bool jacobian, typename T>
  T log_prob(std::vector<T>& params_r, std::vector<int>& params_i,
             std::ostream* msgs) const {
    return ExecutorModel::log_prob<propto, jacobian, T>(params_r, params_i,
                                                        msgs);
  }

  template <typename Context>
  void transform_inits(const Context& /*context*/,
                       std::vector<int>& /*params_i*/,
                       std::vector<double>& params_r,
                       std::ostream* /*msgs*/ = nullptr) const {
    params_r.assign(init_, init_ + num_params_r());
  }

 private:
  const double* init_;
  PathCollector& path_;
};

}  // namespace

PathfinderResult run_pathfinder(Executor& ex, const PathfinderConfig& cfg,
                                const PathObserver& observe) {
  PathfinderResult out;
  out.selected_elbo = kNaN;
  out.khat = kNaN;

  PathCollector path(ex, observe);
  PathfinderModel model(ex, cfg.init, path);
  DrawCollector draws;
  PathLogger logger;
  stan::callbacks::writer init_writer;
  detail::ThrottledPoll poll(&cfg.poll);
  struct Interrupt : stan::callbacks::interrupt {
    detail::ThrottledPoll* poll;
    explicit Interrupt(detail::ThrottledPoll* p) : poll(p) {}
    void operator()() override { poll->check(); }
  } interrupt(&poll);

  // An empty context leaves initialization to the service's own uniform
  // draw, which is byte for byte the one cmdstan_init_point makes for the
  // same (seed, chain_id): same create_rng stream, same distribution over
  // the same parameters, same finiteness retry. That is what puts
  // Pathfinder on NUTS's starting point.
  stan::io::empty_var_context empty;
  std::vector<std::string> names;
  std::vector<std::vector<size_t>> dims;
  std::vector<double> values;
  if (cfg.init != nullptr) {
    model.unconstrained_param_names(names);
    dims.assign(names.size(), std::vector<size_t>{});
    values.assign(cfg.init, cfg.init + names.size());
  }
  stan::io::array_var_context fixed(names, values, dims);
  const stan::io::var_context& init = cfg.init != nullptr
                                          ? (const stan::io::var_context&)fixed
                                          : (const stan::io::var_context&)empty;

  const auto start = std::chrono::steady_clock::now();
  try {
    out.return_code = stan::services::pathfinder::pathfinder_lbfgs_single(
        model, init, cfg.seed, (unsigned int)cfg.chain_id, cfg.init_radius,
        cfg.history_size, cfg.init_alpha, cfg.tol_obj, cfg.tol_rel_obj,
        cfg.tol_grad, cfg.tol_rel_grad, cfg.tol_param, cfg.num_iterations,
        cfg.num_elbo_draws, std::max(cfg.num_draws, cfg.num_elbo_draws),
        /*save_iterations=*/true, /*refresh=*/0, interrupt, logger, init_writer,
        draws, path);
  } catch (const detail::Interrupted&) {
    if (stan::math::empty_nested()) stan::math::recover_memory();
    out.interrupted = true;
    return out;
  } catch (const std::exception& e) {
    out.return_code = 1;
    out.message = e.what();
  }
  out.elapsed_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - start)
                       .count();

  // The service writes the selected iterate's ELBO draws first. With at
  // least num_elbo_draws draws requested it writes all of them, and the
  // draws past num_draws come from no further random numbers.
  const size_t n_elbo = (size_t)std::max(cfg.num_elbo_draws, 0);
  int selected = -1;
  if (n_elbo > 0 && draws.lp.size() >= n_elbo) {
    Eigen::ArrayXd ratio((Eigen::Index)n_elbo);
    for (size_t i = 0; i < n_elbo; ++i)
      ratio((Eigen::Index)i) = draws.lp[i] - draws.lp_approx[i];
    out.selected_elbo = ratio.mean();
    selected = path.record_with_lp(
        std::vector<double>(draws.lp.begin(), draws.lp.begin() + n_elbo));
  }
  const size_t keep =
      std::min(draws.draws.size(), (size_t)std::max(cfg.num_draws, 0));
  draws.draws.resize(keep);
  draws.lp.resize(keep);
  draws.lp_approx.resize(keep);
  out.draws = std::move(draws.draws);
  out.lp = std::move(draws.lp);
  out.lp_approx = std::move(draws.lp_approx);
  out.path = std::move(path.path);
  if (out.message.empty()) out.message = logger.errors;
  if (!out.path.empty() && selected >= 0)
    out.selected_iter = std::min(selected, (int)out.path.size() - 1);

  std::vector<double> ratios(out.lp.size());
  for (size_t i = 0; i < ratios.size(); ++i)
    ratios[i] = out.lp[i] - out.lp_approx[i];
  out.khat = pareto_khat(std::move(ratios));
  return out;
}

// Zhang & Stephens (2009), as the loo package implements it: a grid of
// candidate theta, each scored by the profile log likelihood of the
// generalized Pareto with that theta, then averaged with the resulting
// weights. The final shrink toward 0.5 is the weakly informative prior
// the PSIS paper recommends for small tails.
double pareto_khat(std::vector<double> log_ratios) {
  const double S = (double)log_ratios.size();
  const size_t M = (size_t)std::min(0.2 * S, 3.0 * std::sqrt(S));
  if (M < 5) return kNaN;
  std::sort(log_ratios.begin(), log_ratios.end());
  const size_t start = log_ratios.size() - M;
  const double cutoff = log_ratios[start - 1];
  std::vector<double> x(M);
  for (size_t i = 0; i < M; ++i) x[i] = log_ratios[start + i] - cutoff;

  const double N = (double)M;
  const size_t grid = 30 + (size_t)std::sqrt(N);
  const double xstar = x[(size_t)std::floor(N / 4.0 + 0.5) - 1];
  // A tail with no spread, which is what an exact approximation gives:
  // the ratios are constant, the weights are uniform, and there is no
  // shape to fit. The fit itself would divide by this scale.
  if (!(xstar > 0)) return 0.0;
  std::vector<double> theta(grid), k(grid), profile(grid);
  for (size_t j = 0; j < grid; ++j) {
    theta[j] =
        1.0 / x.back() +
        (1.0 - std::sqrt((double)grid / ((double)j + 0.5))) / (3 * xstar);
    double mean = 0;
    for (double v : x) mean += std::log1p(-theta[j] * v);
    k[j] = mean / N;
    profile[j] = N * (std::log(-theta[j] / k[j]) - k[j] - 1);
  }
  const double top = *std::max_element(profile.begin(), profile.end());
  double norm = 0, theta_hat = 0;
  for (size_t j = 0; j < grid; ++j) norm += std::exp(profile[j] - top);
  for (size_t j = 0; j < grid; ++j)
    theta_hat += theta[j] * std::exp(profile[j] - top) / norm;

  double khat = 0;
  for (double v : x) khat += std::log1p(-theta_hat * v);
  khat /= N;
  const double prior_weight = 10.0;
  return khat * N / (N + prior_weight) +
         prior_weight * 0.5 / (N + prior_weight);
}

}  // namespace stanli
