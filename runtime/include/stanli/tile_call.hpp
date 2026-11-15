#ifndef STANLI_TILE_CALL_HPP
#define STANLI_TILE_CALL_HPP

#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>

#include <cstdint>

namespace stanli {

enum class TileCallKind : uint8_t { None, Density, Unary };

inline TileCallKind tile_call_kind(uint16_t opcode) {
  switch (opcode) {
#define STANLI_TILE_DENSITY_CASE(code, fn, n, tier) \
  case code:                                        \
    return TileCallKind::Density;
    STANLI_SCALAR_DENSITY_LIST(STANLI_TILE_DENSITY_CASE)
#undef STANLI_TILE_DENSITY_CASE
#define STANLI_TILE_UNARY_CASE(code, fn, value, delta, topology) case code:
    STANLI_SCALAR_UNARY_LIST(STANLI_TILE_UNARY_CASE)
#undef STANLI_TILE_UNARY_CASE
    case OP_LOGIT:
    case OP_LOG1M:
    case OP_EXPV:
    case OP_SQRT:
    case OP_SQUARE:
    case OP_TANHV:
    case OP_NEG:
      return TileCallKind::Unary;
    default:
      return TileCallKind::None;
  }
}

// A tile runs a density call one lane per element.
inline uint8_t tile_call_variant(TileCallKind kind, uint8_t variant) {
  return kind == TileCallKind::Density ? elementwise_density(variant) : variant;
}

// Only the summed form of a density becomes a tile call.
inline bool tile_call_accepts(TileCallKind kind, uint8_t variant) {
  return kind != TileCallKind::Density || !DensityMode::of(variant).elementwise;
}

}  // namespace stanli

#endif
