// Telling a rejected point from a bug.
#ifndef STANLI_REJECTION_HPP
#define STANLI_REJECTION_HPP

#include <stdexcept>

namespace stanli {

// Stan Math reports that a model cannot be evaluated at a point with
// std::domain_error, std::invalid_argument, std::out_of_range or
// std::length_error, or a std::runtime_error, and Stan's algorithms reject
// the point. Any other std::logic_error, chiefly a bare one, is stanli's own
// invariant failing: a bug, which must not pass as a rejected point.
inline bool is_internal_error(const std::exception& e) {
  return dynamic_cast<const std::logic_error*>(&e) != nullptr &&
         dynamic_cast<const std::domain_error*>(&e) == nullptr &&
         dynamic_cast<const std::invalid_argument*>(&e) == nullptr &&
         dynamic_cast<const std::out_of_range*>(&e) == nullptr &&
         dynamic_cast<const std::length_error*>(&e) == nullptr;
}

}  // namespace stanli

#endif
