// Decoder for the portable MIR that stanli's compiler writes.
#ifndef STANLI_MIR_DECODE_HPP
#define STANLI_MIR_DECODE_HPP

#include <stanli/mir.hpp>

#include <string_view>

namespace stanli {

// Decode a versioned stanli portable-MIR envelope (STANLI5:...). Any other
// text, including stock stanc's --debug-optimized-mir s-expressions, is
// rejected with an error that names the compilers that write portable MIR.
mir::Program decode_program(std::string_view text);

}  // namespace stanli

#endif
