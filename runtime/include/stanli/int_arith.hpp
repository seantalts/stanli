// Stan's integer arithmetic: the one implementation that the register
// programs, the integer kernels, the MIR interpreter and the folding in
// lowering and in the register compiler all call.
//
// stanc compiles Stan's int operators to C++ int arithmetic. Signed overflow
// is undefined there, so CmdStan's result depends on what its C++ compiler
// does; this wraps it in two's complement, as the hardware does. %/% is C++
// division, which is undefined for a zero divisor; this raises the error of
// Stan Math's divide(int, int). % is Stan Math's modulus(), which raises its
// own error for a zero divisor. Both truncate toward zero: -7 %/% 2 is -3
// and -7 % 2 is -1. The minimum int divided by -1 wraps to itself, with
// remainder 0.
#ifndef STANLI_INT_ARITH_HPP
#define STANLI_INT_ARITH_HPP

#include <stan/math/prim/fun/divide.hpp>
#include <stan/math/prim/fun/modulus.hpp>

#include <cstdint>

namespace stanli {

enum class IntOp { Add, Sub, Mul, Div, Mod };

inline int int_arith(IntOp op, int a, int b) {
  const uint32_t x = static_cast<uint32_t>(a), y = static_cast<uint32_t>(b);
  switch (op) {
    case IntOp::Add:
      return static_cast<int>(x + y);
    case IntOp::Sub:
      return static_cast<int>(x - y);
    case IntOp::Mul:
      return static_cast<int>(x * y);
    case IntOp::Div:
      if (b == -1) return static_cast<int>(0u - x);
      return stan::math::divide(a, b);
    case IntOp::Mod:
      if (b == -1) return 0;
      return stan::math::modulus(a, b);
  }
  return 0;
}

inline int int_negate(int a) {
  return static_cast<int>(0u - static_cast<uint32_t>(a));
}

}  // namespace stanli

#endif
