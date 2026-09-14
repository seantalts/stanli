// The generated-quantities RNG vocabulary, shared by everything that has to
// agree on it: the graph lowering, the register program's region compiler,
// the OP_RNG kernel, and the write_array interpreter. One enum and one set of
// classifiers, so no two of them can disagree about which Stan function a
// draw is, how many arguments it takes, or whether its result is an integer.
//
// The draw helpers themselves live in wa_interp.hpp, because they need the
// caller-owned stream; a compiler only needs to classify, so it can include
// this and nothing else.
#ifndef STANLI_RNG_FAMILY_HPP
#define STANLI_RNG_FAMILY_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace stanli {

// The scalar-argument graph-native RNG tranche.
enum class ScalarRng : uint8_t {
  PoissonLog,
  Uniform,
  Bernoulli,
  Normal,
  Lognormal,
  Binomial,
  Gumbel,
  BetaBinomial,
  Exponential,
  // Preserve the existing container-variant numbers (9 through 11).
  Poisson = 12,
  StudentT,
  BernoulliLogit,
};

// OP_RNG's first non-scalar-argument variant. Keep it outside ScalarRng:
// scalar_rng_draw's `nargs` counts scalar doubles, whereas categorical has
// one logical argument containing an arbitrary number of probabilities.
inline constexpr uint8_t kCategoricalRngVariant =
    static_cast<uint8_t>(ScalarRng::Exponential) + 1;
inline constexpr uint8_t kMultiNormalRngVariant = kCategoricalRngVariant + 1;
inline constexpr uint8_t kDirichletRngVariant = kMultiNormalRngVariant + 1;

// The Stan spelling of each family, so the graph lowering and the register
// program's region compiler recognize exactly the same set. Null for a name
// outside the tranche.
const ScalarRng* scalar_rng_family(const std::string& name);
size_t scalar_rng_arity(ScalarRng family);
bool scalar_rng_is_int(ScalarRng family);

}  // namespace stanli

#endif
