#ifndef STANLI_KERNELS_DENSITY_PARTIALS_HPP
#define STANLI_KERNELS_DENSITY_PARTIALS_HPP

#include <stan/math/prim/fun/Eigen.hpp>
#include <cstdint>

namespace stanli {

// Density pullbacks contract retained partials into a separate adjoint
// buffer. Coordinates are independent: preserve each multiply-then-add,
// without introducing a reduction or changing argument/sweep order.
inline void add_scaled_partials(double* adj, const double* partials,
                                double seed, int64_t n) {
  constexpr int64_t width = Eigen::internal::packet_traits<double>::size;
  // Wide packets have costly alignment prologues and tails on short maps.
  // Retain Eigen for aligned full packets and larger buffers, and leave the
  // established narrow-packet path unchanged.
  if constexpr (width > 2) {
    const bool aligned =
        reinterpret_cast<uintptr_t>(adj) % (width * sizeof(double)) == 0;
    if (n < 2 * width || (n < 16 * width && !aligned)) {
      using Packet = typename Eigen::internal::packet_traits<double>::type;
      const Packet weight = Eigen::internal::pset1<Packet>(seed);
      int64_t i = 0;
      for (; i + width <= n; i += width) {
        const Packet scaled = Eigen::internal::pmul(
            weight, Eigen::internal::ploadu<Packet>(partials + i));
        Eigen::internal::pstoreu(adj + i, Eigen::internal::padd(
            Eigen::internal::ploadu<Packet>(adj + i), scaled));
      }
      for (; i < n; ++i) adj[i] += seed * partials[i];
      return;
    }
  }
  Eigen::Map<Eigen::ArrayXd>(adj, n) +=
      seed * Eigen::Map<const Eigen::ArrayXd>(partials, n);
}

}  // namespace stanli

#endif
