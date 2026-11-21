// Small helpers for replaying Eigen's packet phase after a graph value has
// been materialized into a contiguous slot.
#ifndef STANLI_EXTREMA_GROUPING_HPP
#define STANLI_EXTREMA_GROUPING_HPP

#include <stanli/expression_layout.hpp>
#include <stanli/kernel_types.hpp>
#include <stanli/pairwise_sum.hpp>

#include <stan/math/prim.hpp>

#include <cstdint>
#include <utility>

namespace stanli {

// Number of doubles by which a contiguous view's offset changes the first
// aligned packet. One means every offset is already lane zero.
inline constexpr int64_t extrema_phase_modulus() {
  using Packet = typename Eigen::internal::packet_traits<double>::type;
  constexpr int64_t alignment = alignof(Packet);
  return alignment > static_cast<int64_t>(sizeof(double))
             ? alignment / static_cast<int64_t>(sizeof(double))
             : 1;
}

// Replay Eigen's LinearVectorizedTraversal using the packet phase of a view
// beginning `offset` elements into an aligned owning container. Unaligned
// loads let us retain that grouping without first copying the values to a
// similarly phased address.
template <typename Func>
inline double reduce_phased(const double* data, int64_t len, int64_t offset,
                            const Func& func) {
  using Packet = typename Eigen::internal::packet_traits<double>::type;
  constexpr int64_t packet_size = Eigen::internal::packet_traits<double>::size;
  constexpr int64_t phase_modulus = extrema_phase_modulus();

  int64_t aligned_start =
      (phase_modulus - offset % phase_modulus) % phase_modulus;
  if (aligned_start >= len) aligned_start = len;
  const int64_t aligned_size2 =
      ((len - aligned_start) / (2 * packet_size)) * (2 * packet_size);
  const int64_t aligned_size =
      ((len - aligned_start) / packet_size) * packet_size;
  const int64_t aligned_end2 = aligned_start + aligned_size2;
  const int64_t aligned_end = aligned_start + aligned_size;

  if (aligned_size == 0) {
    double result = data[0];
    for (int64_t i = 1; i < len; ++i) result = func(result, data[i]);
    return result;
  }

  Packet packet0 = Eigen::internal::ploadu<Packet>(data + aligned_start);
  if (aligned_size > packet_size) {
    Packet packet1 =
        Eigen::internal::ploadu<Packet>(data + aligned_start + packet_size);
    for (int64_t i = aligned_start + 2 * packet_size; i < aligned_end2;
         i += 2 * packet_size) {
      packet0 =
          func.packetOp(packet0, Eigen::internal::ploadu<Packet>(data + i));
      packet1 = func.packetOp(
          packet1, Eigen::internal::ploadu<Packet>(data + i + packet_size));
    }
    packet0 = func.packetOp(packet0, packet1);
    if (aligned_end > aligned_end2)
      packet0 = func.packetOp(
          packet0, Eigen::internal::ploadu<Packet>(data + aligned_end2));
  }

  double result = func.predux(packet0);
  for (int64_t i = 0; i < aligned_start; ++i) result = func(result, data[i]);
  for (int64_t i = aligned_end; i < len; ++i) result = func(result, data[i]);
  return result;
}

inline double extrema_phased(const double* data, int64_t len, int64_t offset,
                             bool maximum) {
  if (maximum)
    return reduce_phased(
        data, len, offset,
        Eigen::internal::scalar_max_op<double, double, Eigen::PropagateFast>());
  return reduce_phased(
      data, len, offset,
      Eigen::internal::scalar_min_op<double, double, Eigen::PropagateFast>());
}

inline double prod_phased(const double* data, int64_t len, int64_t offset) {
  return reduce_phased(data, len, offset,
                       Eigen::internal::scalar_product_op<double, double>());
}

// The grouping of Eigen's sum over an operand with this source layout, and
// the phase offset. A layout that is not known to allow packets adds in
// order.
inline std::pair<SumGrouping, int64_t> sum_grouping(
    const ExpressionLayout& layout) {
  if (!layout.packet_access()) return {SumGrouping::InOrder, 0};
  if (layout.direct_access() && layout.element_offset != 0)
    return {SumGrouping::Phased, layout.element_offset};
  return {SumGrouping::Packet, 0};
}

// A short sum keeps the grouping, so it is bit for bit Stan Math's; a longer
// one, except a pairwise sum, is compensated and close to the exact sum.
inline double sum_grouped(const double* data, int64_t len, SumGrouping grouping,
                          int64_t offset) {
  if (len == 0) return 0.0;
  if (len > kPlainSumMax && grouping != SumGrouping::Pairwise)
    return compensated_sum(data, len);
  switch (grouping) {
    case SumGrouping::Phased:
      return reduce_phased(data, len, offset,
                           Eigen::internal::scalar_sum_op<double>());
    case SumGrouping::Packet: {
      // The same-type cast has no DirectAccessBit, so Eigen starts its
      // packets at lane zero instead of at the first aligned address.
      const Eigen::Map<const Eigen::VectorXd> input(data, len);
      return input.unaryExpr(Eigen::internal::core_cast_op<double, double>())
          .sum();
    }
    case SumGrouping::Pairwise:
      return pairwise_sum(data, len);
    case SumGrouping::InOrder:
      break;
  }
  double sum = 0.0;
  for (int64_t i = 0; i < len; ++i) sum += data[i];
  return sum;
}

}  // namespace stanli

#endif
