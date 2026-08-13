#ifndef STANLI_INITIALIZE_HPP
#define STANLI_INITIALIZE_HPP

#include <stanli/graph.hpp>

#include <boost/random/uniform_real_distribution.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace stanli {

enum class FixedInitPolicy { Validate, SkipValidation };

template <typename RNG>
void initialize_point(Executor& ex, RNG& rng, double radius, const double* init,
                      double* q, FixedInitPolicy fixed_policy) {
  const int64_t n = ex.n_params();
  for (int64_t i = 0; i < n; ++i) q[i] = 0.0;
  const bool fixed = init != nullptr || radius == 0.0;
  if (init != nullptr)
    for (int64_t i = 0; i < n; ++i) q[i] = init[i];
  // The sampler validates its one fixed point before stepsize search. The
  // optimizer historically hands fixed starts directly to L-BFGS.
  if (fixed && fixed_policy == FixedInitPolicy::SkipValidation) return;
  std::vector<double> grad((size_t)n);
  const auto acceptable = [&] {
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[i];
    try {
      // CmdStan checks the value before the gradient. They can disagree for
      // an ODE, whose two paths solve different-sized systems.
      if (!std::isfinite(ex.forward_value_only())) return false;
      if (!std::isfinite(ex.gradient(grad.data()))) return false;
      for (int64_t i = 0; i < n; ++i)
        if (!std::isfinite(grad[(size_t)i])) return false;
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };
  if (fixed) {
    if (acceptable()) return;
  } else {
    boost::random::uniform_real_distribution<double> dist(-radius, radius);
    for (int attempt = 0; attempt < 100; ++attempt) {
      for (int64_t i = 0; i < n; ++i) q[i] = dist(rng);
      if (acceptable()) return;
    }
  }
  if (init != nullptr)
    throw std::runtime_error(
        "initialization failed: the supplied unconstrained init has no "
        "finite log density and gradient");
  if (radius == 0.0)
    throw std::runtime_error(
        "initialization failed: the origin has no finite log density and "
        "gradient (init radius is 0)");
  throw std::runtime_error(
      "initialization failed: no draw in 100 attempts had finite log density "
      "and gradient");
}

}  // namespace stanli

#endif
