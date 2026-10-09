#include <boost/multiprecision/cpp_bin_float.hpp>
#include <iostream>

using mp = boost::multiprecision::number<
    boost::multiprecision::cpp_bin_float<50>, boost::multiprecision::et_off>;

namespace stan {
namespace math {
inline double value_of_rec(const mp& x) { return static_cast<double>(x); }
inline double value_of(const mp& x) { return static_cast<double>(x); }
}  // namespace math
}  // namespace stan

#ifdef SPECIALIZE_ARITHMETIC
namespace std {
template <>
struct is_arithmetic<mp> : std::true_type {};
template <>
struct is_floating_point<mp> : std::true_type {};
}  // namespace std
#endif
#include <stan/math/fwd.hpp>
#include <stan/math/prim.hpp>

using fv = stan::math::fvar<mp>;

int main() {
  std::cout.precision(45);
  fv x(mp("-0.4"), mp(1));
  fv e = exp(x);
  std::cout << e.val_ << " " << e.d_ << "\n";
#ifdef WITH_LB
  fv c = stan::math::lb_constrain(x, fv(mp(0)));
  std::cout << c.val_ << " " << c.d_ << "\n";
#endif
#ifdef WITH_DENSITY
  fv y(mp("1.3")), mu(mp("0.2")), sigma(mp("2"), mp(1));
  fv lp = stan::math::normal_lpdf<true>(y, mu, sigma);
  std::cout << lp.val_ << " " << lp.d_ << "\n";
#endif
}
