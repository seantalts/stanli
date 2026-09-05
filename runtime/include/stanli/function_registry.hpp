// Unified discovery and common metadata for registered Stan Math functions.
// Family-specific descriptors retain the contracts that are genuinely unique
// to ordinary builtins or probability functions.
#ifndef STANLI_FUNCTION_REGISTRY_HPP
#define STANLI_FUNCTION_REGISTRY_HPP

#include <stanli/builtin_registry.hpp>
#include <stanli/density_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace stanli {

namespace mir {
struct Expr;
}

enum class FunctionFamily : uint8_t { Builtin, Density };

struct FunctionSpec {
  std::string_view name;
  std::variant<BuiltinSpec, DensitySpec> payload;

  FunctionSpec(std::string_view name, BuiltinSpec builtin)
      : name(name), payload(std::move(builtin)) {}
  FunctionSpec(std::string_view name, DensitySpec density)
      : name(name), payload(std::move(density)) {}

  FunctionFamily family() const;
  uint16_t opcode() const;
  size_t arity() const;
  FunctionArgumentKind result() const;
  int activity_mask() const;
  const BuiltinSpec* builtin() const;
  const DensitySpec* density() const;
  FunctionArgumentKind argument_kind(size_t index) const;
  bool accepts(size_t call_arity, uint64_t integer_arguments) const;
};

const std::vector<FunctionSpec>& function_specs();
bool function_registered(std::string_view name);
bool function_arity_registered(std::string_view name, size_t arity);
// Compatibility lookup for callers which only need a family payload. Typed
// execution paths should use the overload resolver below.
const FunctionSpec* function_spec(std::string_view name, size_t arity,
                                  FunctionFamily family);
const FunctionSpec* function_spec(std::string_view name, FunctionFamily family);
const std::vector<const FunctionSpec*>& function_specs(FunctionFamily family);
// Resolve a concrete numeric overload. Bits in integer_arguments correspond
// to source arguments; clear bits denote real-valued scalar or container
// leaves. A registry real accepts Stan's ordinary int-to-real promotion.
// Result kind breaks ties but cannot be a hard gate: pinned optimized MIR has
// legacy synthesized probability expressions with incorrect UInt metadata.
const FunctionSpec* function_spec(std::string_view name, size_t arity,
                                  uint64_t integer_arguments,
                                  FunctionArgumentKind result);

// MIR-facing overload used by every execution backend. Complex and unknown
// leaves fail closed rather than being mistaken for real-valued overloads.
const FunctionSpec* function_spec(const mir::Expr& call);

}  // namespace stanli

#endif
