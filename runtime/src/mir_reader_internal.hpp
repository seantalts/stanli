#ifndef STANLI_MIR_READER_INTERNAL_HPP
#define STANLI_MIR_READER_INTERNAL_HPP

#include <stanli/mir.hpp>

#include <string_view>

namespace stanli {
namespace mir {
namespace detail {

// Apply the name resolution and binding checks shared by both wire formats.
// This mutates overloaded user-function names and their call sites.
void finalize_program(Program& program, bool strict_variable_metadata = false);

// Portable v1 is a closed-world format, so reject inconsistent kind-specific
// shapes and redundant type metadata before the shared finalization. The
// legacy reader deliberately remains permissive because its unsupported-form
// fallbacks accept some unusual debug-MIR shapes.
void validate_portable_program(const Program& program);

Program read_portable_program(std::string_view text);

}  // namespace detail
}  // namespace mir
}  // namespace stanli

#endif
