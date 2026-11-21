#ifndef STANLI_INITIALIZE_HPP
#define STANLI_INITIALIZE_HPP

#include <stanli/graph.hpp>
#include <stanli/rejection.hpp>

#include <boost/random/uniform_real_distribution.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace stanli {

// The start CmdStan's stan::services::util::initialize finds, on the
// unconstrained scale: `init` when given, the origin for radius 0, else up to
// 100 uniform(-radius, radius) draws, keeping the first with a finite log
// density and gradient. As in CmdStan, only a domain error (Stan's reject and
// argument checks) while evaluating the log density means "try another
// point"; any other error, and any error in the gradient, stops at once.
template <typename RNG>
void initialize_point(Executor& ex, RNG& rng, double radius, const double* init,
                      double* q) {
  const int64_t n = ex.n_params();
  for (int64_t i = 0; i < n; ++i) q[i] = 0.0;
  const bool fixed = init != nullptr || radius == 0.0;
  boost::random::uniform_real_distribution<double> dist(-radius, radius);
  if (init != nullptr) {
    // CmdStan draws its random context before it reads a supplied start, so
    // the chain's stream moves on by one draw per parameter either way.
    if (radius != 0.0)
      for (int64_t i = 0; i < n; ++i) dist(rng);
    for (int64_t i = 0; i < n; ++i) q[i] = init[i];
  }
  std::vector<double> grad((size_t)n);
  std::string first_error;
  const auto fatal = [](const std::exception& e) {
    if (is_internal_error(e)) throw;
    throw std::runtime_error(
        std::string("Unrecoverable error evaluating the log probability at "
                    "the initial value: ") +
        e.what());
  };
  const auto acceptable = [&] {
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[i];
    // CmdStan checks the value before the gradient. They can disagree for
    // an ODE, whose two paths solve different-sized systems.
    try {
      if (!std::isfinite(ex.forward_value_only())) return false;
    } catch (const std::domain_error& e) {
      if (first_error.empty()) first_error = e.what();
      return false;
    } catch (const std::exception& e) {
      fatal(e);
    }
    try {
      if (!std::isfinite(ex.gradient(grad.data()))) return false;
    } catch (const std::exception& e) {
      fatal(e);
    }
    for (int64_t i = 0; i < n; ++i)
      if (!std::isfinite(grad[(size_t)i])) return false;
    return true;
  };
  if (fixed) {
    if (acceptable()) return;
  } else {
    for (int attempt = 0; attempt < 100; ++attempt) {
      for (int64_t i = 0; i < n; ++i) q[i] = dist(rng);
      if (acceptable()) return;
    }
  }
  std::string message = "initialization failed: ";
  if (init != nullptr)
    message +=
        "the supplied unconstrained init has no finite log density and "
        "gradient";
  else if (radius == 0.0)
    message +=
        "the origin has no finite log density and gradient (init radius is 0)";
  else
    message += "no draw in 100 attempts had finite log density and gradient";
  if (!first_error.empty()) message += "; first error: " + first_error;
  throw std::runtime_error(message);
}

}  // namespace stanli

#endif
