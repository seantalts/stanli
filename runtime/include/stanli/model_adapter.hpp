// Adapter satisfying the slice of the stan model concept the samplers and
// the point-estimate services use. The samplers reach the executor through
// the stan::model::gradient and log_prob_propto specializations at the end
// of this file; initialize and the optimizers still go through
// log_prob<propto, jacobian, var>.
//
// propto and jacobian template flags are ignored: the graph is fixed at
// compile time, Jacobian terms included and each density's propto choice
// baked into its variant bits.
#ifndef STANLI_MODEL_ADAPTER_HPP
#define STANLI_MODEL_ADAPTER_HPP

#include <stanli/graph.hpp>

#include <stan/math.hpp>
#include <stan/model/gradient.hpp>
#include <stan/model/log_prob_propto.hpp>

#include <functional>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace stanli {

// The CSV side of the model concept, supplied by whoever built the
// write_array graph (or its interpreter fallback). The samplers never
// need this; the optimizers and Pathfinder do, because they report a
// point rather than a stream of draws and have to name and constrain it.
//
// Kept as a callback rather than a second Executor inside the adapter so
// that the two write_array paths -- compiled graph and per-draw
// interpreter -- stay where they already are, in compile.hpp and capi.
struct WriteArray {
  std::vector<std::string> names;  // flattened, CmdStan's column order
  // q holds num_params_r() unconstrained values; out receives
  // names.size() doubles.
  std::function<void(const double* q, double* out)> row;
};

class ExecutorModel {
 public:
  explicit ExecutorModel(Executor& ex, const WriteArray* wa = nullptr)
      : ex_(&ex), wa_(wa) {}

  size_t num_params_r() const { return static_cast<size_t>(ex_->n_params()); }
  Executor& executor() const { return *ex_; }

  template <bool propto, bool jacobian, typename T>
  T log_prob(Eigen::Matrix<T, -1, 1>& q, std::ostream* /*msgs*/) const {
    const int64_t n = ex_->n_params();
    if constexpr (std::is_same_v<T, double>) {
      for (int64_t i = 0; i < n; ++i) ex_->params_data()[i] = q(i);
      try {
        return ex_->forward();
      } catch (const std::exception&) {
        return -std::numeric_limits<double>::infinity();
      }
    } else {
      static_assert(std::is_same_v<T, stan::math::var>,
                    "adapter supports double and var");
      Eigen::Map<Eigen::VectorXd>(ex_->params_data(), n) =
          stan::math::value_of(q);
      double* grad =
          stan::math::ChainableStack::instance_->memalloc_.alloc_array<double>(
              n);
      double value;
      try {
        value = ex_->gradient(grad);
      } catch (const std::exception&) {
        // Rejected point (domain error in a kernel): -inf with no gradient,
        // which the sampler treats as a divergence.
        return T(-std::numeric_limits<double>::infinity());
      }
      stan::math::vari** varis =
          stan::math::ChainableStack::instance_->memalloc_
              .alloc_array<stan::math::vari*>(n);
      for (int64_t i = 0; i < n; ++i) varis[i] = q(i).vi_;
      return stan::math::var(new stan::math::precomputed_gradients_vari(
          value, (size_t)n, varis, grad));
    }
  }

  // The std::vector form of the same call. stan::model::log_prob_grad and
  // log_prob_propto each have two overloads and the optimizers reach for
  // this one; forwarding keeps a single definition of what log_prob means.
  template <bool propto, bool jacobian, typename T>
  T log_prob(std::vector<T>& params_r, std::vector<int>& /*params_i*/,
             std::ostream* msgs) const {
    Eigen::Matrix<T, -1, 1> q = Eigen::Map<Eigen::Matrix<T, -1, 1>>(
        params_r.data(), (Eigen::Index)params_r.size());
    return log_prob<propto, jacobian, T>(q, msgs);
  }

  // ---- the CSV side, for the point-estimate services -------------------
  // include_tp / include_gq are ignored: the write_array graph is built
  // once, whole, and there is no cheaper prefix of it to serve. That
  // matches what these callers want (both pass true, true) and it is
  // stated rather than silently assumed.
  // APPENDS. Pathfinder pushes lp_approx__ and lp__ first and then calls
  // this to add the model's own columns, so assigning here would wipe
  // them and leave every row two values wider than its header.
  void constrained_param_names(std::vector<std::string>& names,
                               bool /*include_tp*/ = true,
                               bool /*include_gq*/ = true) const {
    if (wa_ != nullptr) {
      names.insert(names.end(), wa_->names.begin(), wa_->names.end());
      return;
    }
    // No write_array graph: the unconstrained parameters are all there is
    // to name, and they are what write_array returns in that case too.
    std::vector<std::string> q;
    unconstrained_param_names(q);
    names.insert(names.end(), q.begin(), q.end());
  }
  void unconstrained_param_names(std::vector<std::string>& names,
                                 bool /*include_tp*/ = true,
                                 bool /*include_gq*/ = true) const {
    for (int64_t i = 0; i < ex_->n_params(); ++i)
      names.push_back("q." + std::to_string(i + 1));
  }

  template <typename RNG>
  void write_array(RNG& /*rng*/, std::vector<double>& params_r,
                   std::vector<int>& /*params_i*/, std::vector<double>& values,
                   bool /*include_tp*/ = true, bool /*include_gq*/ = true,
                   std::ostream* /*msgs*/ = nullptr) const {
    if (wa_ == nullptr) {
      values.assign(params_r.begin(), params_r.end());
      return;
    }
    values.assign(wa_->names.size(), 0.0);
    wa_->row(params_r.data(), values.data());
  }

  // Pathfinder's Eigen form of the same call.
  template <typename RNG>
  void write_array(RNG& /*rng*/, Eigen::Matrix<double, -1, 1>& params_r,
                   Eigen::Matrix<double, -1, 1>& values,
                   bool /*include_tp*/ = true, bool /*include_gq*/ = true,
                   std::ostream* /*msgs*/ = nullptr) const {
    if (wa_ == nullptr) {
      values = params_r;
      return;
    }
    values.setZero((Eigen::Index)wa_->names.size());
    wa_->row(params_r.data(), values.data());
  }

  // Every unconstrained parameter is a scalar as far as a random init is
  // concerned, so each gets an empty dimension list.
  void get_dims(std::vector<std::vector<size_t>>& dims,
                bool /*include_tp*/ = true, bool /*include_gq*/ = true) const {
    dims.assign((size_t)ex_->n_params(), std::vector<size_t>{});
  }

  // stan::services::util::initialize wants these. get_param_names names
  // the parameters in an initialization failure message.
  void get_param_names(std::vector<std::string>& names,
                       bool /*include_tp*/ = true,
                       bool /*include_gq*/ = true) const {
    unconstrained_param_names(names);
  }

  // Constrained-scale inits do not come through this adapter. The inverse
  // parameter transforms exist now (stanli::InitInterp, reached through
  // CompiledModel::transform_inits, stanli_unconstrain_inits, or
  // bs_param_unconstrain), but they need the model's MIR section, which this
  // adapter deliberately does not hold -- it sees an Executor and nothing
  // else. Every caller in this repository unconstrains at its own boundary
  // and hands the samplers a free vector, so nothing reaches this; throwing
  // keeps it that way, because quietly returning zeros would start a run
  // somewhere the user did not ask for.
  template <typename Context>
  void transform_inits(const Context& /*context*/,
                       std::vector<int>& /*params_i*/,
                       std::vector<double>& /*params_r*/,
                       std::ostream* /*msgs*/ = nullptr) const {
    throw std::runtime_error(
        "this adapter does not take inits on the constrained scale: "
        "unconstrain them first (stanli_unconstrain_inits, "
        "bs_param_unconstrain, or CompiledModel::transform_inits) and pass "
        "the free vector.");
  }

 private:
  Executor* ex_;
  const WriteArray* wa_ = nullptr;
};

}  // namespace stanli

namespace stan {
namespace model {

template <>
inline void gradient<stanli::ExecutorModel>(const stanli::ExecutorModel& model,
                                            const Eigen::VectorXd& x, double& f,
                                            Eigen::VectorXd& grad_f,
                                            callbacks::logger& logger) {
  stanli::Executor& ex = model.executor();
  const int64_t n = ex.n_params();
  Eigen::Map<Eigen::VectorXd>(ex.params_data(), n) = x;
  grad_f.resize(n);
  try {
    f = ex.gradient(grad_f.data());
  } catch (const std::exception& e) {
    logger.error(
        "Informational Message: The current Metropolis proposal "
        "is about to be rejected because of the following issue:");
    logger.error(e.what());
    logger.error(
        "If this warning occurs sporadically, such as for highly "
        "constrained variable types like covariance matrices, "
        "then the sampler is fine,");
    logger.error(
        "but if this warning occurs often then your model may be "
        "either severely ill-conditioned or misspecified.");
    logger.error("");
    f = -std::numeric_limits<double>::infinity();
    grad_f.setZero();
  }
}

template <>
inline double log_prob_propto<true, stanli::ExecutorModel>(
    const stanli::ExecutorModel& model, Eigen::VectorXd& params_r,
    std::ostream* msgs) {
  return model.template log_prob<true, true, double>(params_r, msgs);
}

}  // namespace model
}  // namespace stan

#endif
