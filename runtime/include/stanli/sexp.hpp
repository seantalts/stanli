// Minimal s-expression parser for stanc3 MIR dumps (--debug-optimized-mir
// and --debug-transformed-mir share the format).
// Atoms are bare tokens (including <opaque>) or double-quoted strings with
// backslash escapes.
#ifndef STANLI_SEXP_HPP
#define STANLI_SEXP_HPP

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace stanli {
namespace sexp {

struct Node {
  std::string atom;
  std::vector<Node> kids;
  bool leaf = true;

  bool is_atom() const { return leaf; }
  size_t size() const { return kids.size(); }
  const Node& operator[](size_t i) const { return kids.at(i); }
  bool head_is(const char* sym) const {
    return !leaf && !kids.empty() && kids[0].leaf && kids[0].atom == sym;
  }
};

namespace detail {

inline void skip_ws(std::string_view s, size_t& i) {
  while (i < s.size() &&
         (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r'))
    ++i;
}

inline Node parse_at(std::string_view s, size_t& i) {
  skip_ws(s, i);
  if (i >= s.size())
    throw std::runtime_error("sexp: unexpected end at position " +
                             std::to_string(i));
  if (s[i] == '(') {
    Node n;
    n.leaf = false;
    ++i;
    for (;;) {
      skip_ws(s, i);
      if (i >= s.size())
        throw std::runtime_error("sexp: unclosed list at position " +
                                 std::to_string(i));
      if (s[i] == ')') {
        ++i;
        return n;
      }
      n.kids.push_back(parse_at(s, i));
    }
  }
  if (s[i] == ')')
    throw std::runtime_error("sexp: unexpected ) at position " +
                             std::to_string(i));
  Node n;
  if (s[i] == '"') {
    ++i;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) ++i;
      n.atom.push_back(s[i]);
      ++i;
    }
    if (i >= s.size())
      throw std::runtime_error("sexp: unclosed string at position " +
                               std::to_string(i));
    ++i;  // closing quote
    return n;
  }
  while (i < s.size() && s[i] != '(' && s[i] != ')' && s[i] != '"' &&
         s[i] != ' ' && s[i] != '\n' && s[i] != '\t' && s[i] != '\r') {
    n.atom.push_back(s[i]);
    ++i;
  }
  return n;
}

}  // namespace detail

inline Node parse(std::string_view text) {
  size_t i = 0;
  Node n = detail::parse_at(text, i);
  detail::skip_ws(text, i);
  if (i != text.size())
    throw std::runtime_error("sexp: trailing content at position " +
                             std::to_string(i));
  return n;
}

}  // namespace sexp
}  // namespace stanli

#endif
