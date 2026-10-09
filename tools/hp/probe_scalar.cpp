#include <boost/multiprecision/cpp_bin_float.hpp>
#include <stan/math/prim.hpp>
#include <iostream>

using mp =
    boost::multiprecision::number<boost::multiprecision::cpp_bin_float<50>,
                                  boost::multiprecision::et_off>;

#ifdef SPECIALIZE_TRAITS
namespace stan {
template <>
struct is_stan_scalar<mp> : std::true_type {};
}  // namespace stan
#endif
#ifdef SPECIALIZE_ARITHMETIC
namespace std {
template <>
struct is_arithmetic<mp> : std::true_type {};
}  // namespace std
#endif

int main() {
  mp y = 1.3, mu = 0.2, sigma = 2;
#ifdef WITH_CONSTRAIN
  mp x = stan::math::lb_constrain(mp(-0.4), mp(0));
  std::cout << x << "\n";
  mp lp0 = 0;
  mp x2 = stan::math::lb_constrain(mp(-0.4), mp(0), lp0);
  std::cout << x2 << " " << lp0 << "\n";
#endif
#ifdef WITH_DENSITY
  mp lp = stan::math::normal_lpdf<true>(y, mu, sigma);
  std::cout << lp << "\n";
#endif
}
