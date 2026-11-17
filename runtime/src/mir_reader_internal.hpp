#ifndef STANLI_MIR_READER_INTERNAL_HPP
#define STANLI_MIR_READER_INTERNAL_HPP

#include <stanli/mir.hpp>

#include <string_view>

namespace stanli {
namespace mir {
namespace detail {

// Apply the name resolution and binding checks shared by both wire formats.
// This records the overload each user-function call site selects.
void finalize_program(Program& program, bool strict_variable_metadata = false);

// The SizedType of a declaration stanc3 left unsized. A scalar has a fixed
// width and keeps the sized spelling; a container carries its view, and its
// first whole-variable assignment supplies the shape.
SizedType unsized_declaration(const UnsizedView& view, std::string raw);

// The view a sized or unsized declaration gives its variable.
UnsizedView declaration_view(const SizedType& type);

// Compact portable MIR is a closed-world format, so reject inconsistent
// kind-specific shapes and redundant type metadata before finalization. The
// legacy reader deliberately remains permissive because its unsupported-form
// fallbacks accept some unusual debug-MIR shapes.
void validate_portable_program(const Program& program);

Program read_portable_program(std::string_view bytes);

}  // namespace detail
}  // namespace mir
}  // namespace stanli

#endif
