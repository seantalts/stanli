// Decoder for the MIR formats accepted at the stanli compiler boundary.
#ifndef STANLI_MIR_DECODE_HPP
#define STANLI_MIR_DECODE_HPP

#include <stanli/mir.hpp>

#include <string_view>

namespace stanli {

// Decode either a versioned stanli portable-MIR envelope or the legacy stanc3
// S-expression dump. Format selection is based only on the first non-space
// byte; malformed portable MIR is never retried as legacy MIR.
mir::Program decode_program(std::string_view text);

}  // namespace stanli

#endif
