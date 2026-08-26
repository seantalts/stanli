#include <stanli/mir_decode.hpp>
#include <stanli/sexp.hpp>

#include "mir_reader_internal.hpp"

#include <cstddef>
#include <stdexcept>

namespace stanli {
namespace {

bool is_space(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

}  // namespace

mir::Program decode_program(std::string_view text) {
  size_t first = 0;
  while (first < text.size() && is_space(text[first])) ++first;
  if (first == text.size()) throw std::runtime_error("mir: empty input");

  if (text[first] == '{') return mir::detail::read_portable_program(text);
  if (text[first] == '(') return mir::read_program(sexp::parse(text));

  throw std::runtime_error(
      "mir: unrecognized input format (expected portable JSON or legacy "
      "S-expression)");
}

}  // namespace stanli
