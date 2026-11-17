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
SizedType unsized_declaration(const UnsizedType& view, std::string raw);

// Compact portable MIR is a closed-world format, so reject inconsistent
// kind-specific shapes and redundant type metadata before finalization. The
// legacy reader deliberately remains permissive because its unsupported-form
// fallbacks accept some unusual debug-MIR shapes.
void validate_portable_program(const Program& program);

Program read_portable_program(std::string_view bytes);

// Replace every tuple with one variable per leaf, so no later stage sees a
// tuple. Runs after overload resolution, which tells tuple signatures
// apart.
void eliminate_tuples(Program& program);

}  // namespace detail
}  // namespace mir
}  // namespace stanli

#endif
