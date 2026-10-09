// The model Stan's services expect, over an Executor.
//
// ExecutorModel is shaped for the sampler: a rejected point is -inf and
// write_array ignores the generator it is handed. A generated Stan model
// differs in three ways the services depend on, and this adapter follows the
// generated model in each:
//
//   1. A rejected point throws. ADVI redraws a Monte Carlo sample whose
//      gradient throws; handed -inf with a zero gradient it would keep it.
//   2. write_array draws generated quantities from the SERVICE's generator,
//      between the service's own draws. That interleaving is what makes a
//      seed mean what it means in CmdStan.
//   3. write_array(include_gq = false) draws nothing. random_var_context
//      calls it while initializing, before the first real draw.
//
// propto and jacobian stay ignored, exactly as in ExecutorModel.
#ifndef STANLI_SERVICE_MODEL_HPP
#define STANLI_SERVICE_MODEL_HPP

#include <stanli/model_adapter.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/callbacks/interrupt.hpp>
#include <stan/callbacks/logger.hpp>
#include <stan/callbacks/writer.hpp>
#include <stan/io/array_var_context.hpp>
#include <stan/io/empty_var_context.hpp>
#include <stan/services/util/create_rng.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace stanli {
namespace detail {

// Thrown to abandon a run the caller asked to stop. Deliberately not a
// std::exception: the services catch those and turn them into error codes.
struct Interrupted {};

// What a run needs from its host besides the executor.
struct ServiceHooks {
  // The CSV columns and the row that fills them, drawing from `rng`. With no
  // row the columns are the unconstrained parameters themselves.
  const std::vector<std::string>* names = nullptr;
  std::function<void(const double* q, double* out, WaRng& rng)> row;
  // Asked about every 100 ms; true stops the run.
  std::function<bool()> poll;
};

class ServiceModel : public ExecutorModel {
 public:
  ServiceModel(Executor& ex, const ServiceHooks& hooks)
      : ExecutorModel(ex), hooks_(&hooks) {}

  void check_interrupt() const {
    if (!hooks_->poll) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_poll_ < std::chrono::milliseconds(100)) return;
    last_poll_ = now;
    if (hooks_->poll()) throw Interrupted{};
  }

  double value(const double* q) const {
    check_interrupt();
    Executor& ex = executor();
    const int64_t n = ex.n_params();
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[i];
    return ex.forward();
  }

  double value_and_gradient(const double* q, double* grad) const {
    check_interrupt();
    Executor& ex = executor();
    const int64_t n = ex.n_params();
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[i];
    return ex.gradient(grad);
  }

  template <bool propto, bool jacobian, typename T>
  T log_prob(Eigen::Matrix<T, -1, 1>& q, std::ostream* /*msgs*/) const {
    if constexpr (std::is_same_v<T, double>) {
      return value(q.data());
    } else {
      static_assert(std::is_same_v<T, stan::math::var>,
                    "adapter supports double and var");
      const int64_t n = executor().n_params();
      auto& stack = stan::math::ChainableStack::instance_->memalloc_;
      double* point = stack.alloc_array<double>((size_t)n);
      double* grad = stack.alloc_array<double>((size_t)n);
      stan::math::vari** varis =
          stack.alloc_array<stan::math::vari*>((size_t)n);
      for (int64_t i = 0; i < n; ++i) {
        point[i] = q(i).val();
        varis[i] = q(i).vi_;
      }
      const double lp = value_and_gradient(point, grad);
      return stan::math::var(new stan::math::precomputed_gradients_vari(
          lp, (size_t)n, varis, grad));
    }
  }

  template <bool propto, bool jacobian, typename T>
  T log_prob(std::vector<T>& params_r, std::vector<int>& /*params_i*/,
             std::ostream* msgs) const {
    Eigen::Matrix<T, -1, 1> q = Eigen::Map<Eigen::Matrix<T, -1, 1>>(
        params_r.data(), (Eigen::Index)params_r.size());
    return log_prob<propto, jacobian, T>(q, msgs);
  }

  // Appends, like ExecutorModel's: the services push their own leading
  // columns first.
  void constrained_param_names(std::vector<std::string>& names,
                               bool /*include_tp*/ = true,
                               bool /*include_gq*/ = true) const {
    if (hooks_->names == nullptr || !hooks_->row) {
      unconstrained_param_names(names);
      return;
    }
    names.insert(names.end(), hooks_->names->begin(), hooks_->names->end());
  }

  size_t n_columns() const {
    return hooks_->names != nullptr && hooks_->row ? hooks_->names->size()
                                                   : num_params_r();
  }

  template <typename RNG>
  void write_array(RNG& rng, std::vector<double>& params_r,
                   std::vector<int>& /*params_i*/, std::vector<double>& values,
                   bool /*include_tp*/ = true, bool include_gq = true,
                   std::ostream* /*msgs*/ = nullptr) const {
    values.assign(include_gq ? n_columns() : params_r.size(), 0.0);
    try {
      write_row(rng, params_r.data(), values.data(), include_gq);
    } catch (...) {
      // A generated model leaves nothing usable behind either; mcmc_writer
      // pads what is missing with NaN.
      values.clear();
      throw;
    }
  }

  template <typename RNG>
  void write_array(RNG& rng, Eigen::Matrix<double, -1, 1>& params_r,
                   Eigen::Matrix<double, -1, 1>& values,
                   bool /*include_tp*/ = true, bool include_gq = true,
                   std::ostream* /*msgs*/ = nullptr) const {
    values.setZero(
        (Eigen::Index)(include_gq ? n_columns() : (size_t)params_r.size()));
    write_row(rng, params_r.data(), values.data(), include_gq);
  }

  // init_context names each unconstrained coordinate, so the "transform" is
  // a lookup: the point is already on the scale the services work in.
  template <typename Context>
  void transform_inits(const Context& context, std::vector<int>& /*params_i*/,
                       std::vector<double>& params_r,
                       std::ostream* /*msgs*/ = nullptr) const {
    std::vector<std::string> names;
    unconstrained_param_names(names);
    params_r.resize(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
      const std::vector<double> value = context.vals_r(names[i]);
      if (value.size() != 1)
        throw std::runtime_error("starting point is missing " + names[i]);
      params_r[i] = value[0];
    }
  }

 private:
  template <typename RNG>
  void write_row(RNG& rng, const double* q, double* out,
                 bool include_gq) const {
    static_assert(std::is_same_v<RNG, stan::rng_t>,
                  "the services draw from stan::rng_t");
    if (!include_gq || !hooks_->row) {
      const size_t n = num_params_r();
      for (size_t i = 0; i < n; ++i) out[i] = q[i];
      return;
    }
    check_interrupt();
    // The row draws through a WaRng, which owns its generator. Lend it the
    // service's state for the call and take the advanced state back, on
    // every exit: a rejected row keeps the randomness it consumed.
    struct Lend {
      stan::rng_t& theirs;
      stan::rng_t& ours;
      Lend(stan::rng_t& t, stan::rng_t& o) : theirs(t), ours(o) {
        std::swap(theirs, ours);
      }
      ~Lend() { std::swap(theirs, ours); }
    } lend(rng, scratch_.gen());
    hooks_->row(q, out, scratch_);
  }

  const ServiceHooks* hooks_;
  mutable WaRng scratch_{0};
  mutable std::chrono::steady_clock::time_point last_poll_{};
};

// Stan's interrupt, asked between iterations by the services that have one.
class PollInterrupt : public stan::callbacks::interrupt {
 public:
  explicit PollInterrupt(const ServiceModel& model) : model_(&model) {}
  void operator()() override { model_->check_interrupt(); }

 private:
  const ServiceModel* model_;
};

// Level 0 info, 1 warning, 2 error; debug and fatal fold into those.
using LogSink = std::function<void(int level, const std::string& text)>;

class SinkLogger : public stan::callbacks::logger {
 public:
  explicit SinkLogger(const LogSink& sink) : sink_(&sink) {}

  void debug(const std::string&) override {}
  void debug(const std::stringstream&) override {}
  void info(const std::string& m) override { emit(0, m); }
  void info(const std::stringstream& m) override { emit(0, m.str()); }
  void warn(const std::string& m) override { emit(1, m); }
  void warn(const std::stringstream& m) override { emit(1, m.str()); }
  void error(const std::string& m) override { emit(2, m); }
  void error(const std::stringstream& m) override { emit(2, m.str()); }
  void fatal(const std::string& m) override { emit(2, m); }
  void fatal(const std::stringstream& m) override { emit(2, m.str()); }

  // Every error line of the run, for the caller's message buffer.
  const std::string& errors() const { return errors_; }

 private:
  void emit(int level, const std::string& text) {
    if (level == 2) {
      if (!errors_.empty()) errors_ += "; ";
      errors_ += text;
    }
    if (*sink_) (*sink_)(level, text);
  }

  const LogSink* sink_;
  std::string errors_;
};

// An init context for util::initialize. It uses a supplied point only when
// the context names every parameter, so a supplied point is named in full;
// with none, the empty context leaves the service to its own uniform draw.
inline std::unique_ptr<stan::io::var_context> init_context(
    const ServiceModel& model, const double* init) {
  if (init == nullptr) return std::make_unique<stan::io::empty_var_context>();
  std::vector<std::string> names;
  model.unconstrained_param_names(names);
  std::vector<double> values(init, init + names.size());
  std::vector<std::vector<size_t>> dims(names.size());
  return std::make_unique<stan::io::array_var_context>(names, values, dims);
}

// Collects the rows a service writes: `leading` service columns, then the
// model's. The services use three row types between them (std::vector for
// fixed_param, Laplace and ADVI; an Eigen row vector for Pathfinder), and
// every writer overload is a no-op unless overridden, so both are taken.
class RowCollector : public stan::callbacks::writer {
 public:
  explicit RowCollector(size_t leading) : leading_(leading) {}

  // Timing and progress text arrive through the string and no-argument
  // forms, which stay no-ops.
  using stan::callbacks::writer::operator();

  void operator()(const std::vector<std::string>& names) override {
    n_columns_ = names.size() >= leading_ ? names.size() - leading_ : 0;
  }
  void operator()(const std::vector<double>& row) override {
    take(row.data(), row.size());
  }
  void operator()(const Eigen::Matrix<double, 1, -1>& row) override {
    take(row.data(), (size_t)row.size());
  }

  size_t n_columns() const { return n_columns_; }
  size_t rows() const { return n_rows_; }
  // Service column `k` of row `i`.
  double lead(size_t i, size_t k) const { return lead_[i * leading_ + k]; }
  std::vector<double>& values() { return values_; }

 private:
  void take(const double* row, size_t n) {
    if (n != leading_ + n_columns_) return;
    lead_.insert(lead_.end(), row, row + leading_);
    values_.insert(values_.end(), row + leading_, row + n);
    ++n_rows_;
  }

  size_t leading_;
  size_t n_columns_ = 0;
  size_t n_rows_ = 0;
  std::vector<double> lead_;
  std::vector<double> values_;
};

// Runs `service`, which returns Stan's error code, and maps every way it can
// end onto `out`: success, a logged failure, a thrown one, or an interrupt.
template <typename Result, typename Service>
void run_service(Result& out, const SinkLogger& logger, const char* what,
                 Service&& service) {
  try {
    out.return_code = service();
    if (out.return_code != 0)
      out.message = logger.errors().empty() ? std::string(what) + " failed"
                                            : logger.errors();
  } catch (const Interrupted&) {
    // The run was abandoned mid-evaluation; drop whatever it left on the
    // autodiff stack.
    stan::math::recover_memory();
    out.interrupted = true;
    out.return_code = 0;
  } catch (const std::exception& e) {
    out.return_code = 1;
    out.message = e.what();
  }
}

}  // namespace detail
}  // namespace stanli

namespace stan {
namespace model {

// The services reach the gradient through these two, and the value through
// log_prob_propto. Without them the generic forms would wrap every
// evaluation in a second autodiff pass over precomputed gradients.
template <>
inline void gradient<stanli::detail::ServiceModel>(
    const stanli::detail::ServiceModel& model, const Eigen::VectorXd& x,
    double& f, Eigen::VectorXd& grad_f, std::ostream* /*msgs*/) {
  grad_f.resize(x.size());
  f = model.value_and_gradient(x.data(), grad_f.data());
}

template <>
inline void gradient<stanli::detail::ServiceModel>(
    const stanli::detail::ServiceModel& model, const Eigen::VectorXd& x,
    double& f, Eigen::VectorXd& grad_f, callbacks::logger& /*logger*/) {
  grad_f.resize(x.size());
  f = model.value_and_gradient(x.data(), grad_f.data());
}

template <>
inline double log_prob_propto<true, stanli::detail::ServiceModel>(
    const stanli::detail::ServiceModel& model, Eigen::VectorXd& params_r,
    std::ostream* /*msgs*/) {
  return model.value(params_r.data());
}

}  // namespace model
}  // namespace stan

#endif
