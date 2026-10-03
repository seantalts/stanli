#ifndef STANLI_TILE_CALL_HPP
#define STANLI_TILE_CALL_HPP

#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>

#include <cstdint>

namespace stanli {

enum class TileCallKind : uint8_t { None, Density };

inline TileCallKind tile_call_kind(uint16_t opcode) {
  switch (opcode) {
#define STANLI_TILE_DENSITY_CASE(code, fn, n, tier) \
  case code:                                        \
    return TileCallKind::Density;
    STANLI_SCALAR_DENSITY_LIST(STANLI_TILE_DENSITY_CASE)
#undef STANLI_TILE_DENSITY_CASE
    default:
      return TileCallKind::None;
  }
}

inline uint8_t tile_call_variant(TileCallKind kind, uint8_t variant,
                                 int n_in) {
  if (kind != TileCallKind::Density) return variant;
  const uint8_t all = (uint8_t)((1u << n_in) - 1u);
  const uint8_t mask = variant == 0 ? all : (uint8_t)(variant & 0x3fu);
  return (uint8_t)(0x40u | (variant & 0x80u) | mask);
}

}  // namespace stanli

#endif
