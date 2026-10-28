// Exact execution of stanc3's structured CompilerInternal FnCheck statements.
// Geometry belongs to the declaration; storage only describes how to gather
// one leaf from the caller's flat buffer.
#ifndef STANLI_STRUCTURED_CHECK_HPP
#define STANLI_STRUCTURED_CHECK_HPP

#include <stanli/mir.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace stanli {

enum class StructuredLeaf : uint8_t { Vector, Matrix };
enum class StructuredStorage : uint8_t { ContiguousLeaves, FirstIndexFast };

struct StructuredCheckSpec {
  mir::Transform::Kind kind = mir::Transform::Unsupported;
  StructuredLeaf leaf = StructuredLeaf::Vector;
  StructuredStorage storage = StructuredStorage::ContiguousLeaves;
  std::string name;
  // Full constrained dimensions, outer array dimensions first and the
  // vector length or matrix rows/columns last.
  std::vector<int64_t> dims;
};

// Stan Math prefixes every validator message with the calling function's name.
// stanli has none worth showing, so validators are called with an empty name
// and the resulting ": " is dropped here.
template <typename F>
inline void without_check_prefix(F&& check) {
  const auto trimmed = [](const char* what) {
    std::string text(what);
    if (text.compare(0, 2, ": ") == 0) text.erase(0, 2);
    return text;
  };
  try {
    std::forward<F>(check)();
  } catch (const std::domain_error& e) {
    throw std::domain_error(trimmed(e.what()));
  } catch (const std::invalid_argument& e) {
    throw std::invalid_argument(trimmed(e.what()));
  }
}

// Invoke the corresponding Stan Math validator once per structured leaf.
// Throws its exact domain_error/invalid_argument on a bad value and
// logic_error when lowering supplied inconsistent immutable geometry.
void check_structured_value(const double* values, int64_t len,
                            const StructuredCheckSpec& spec);

}  // namespace stanli

#endif
