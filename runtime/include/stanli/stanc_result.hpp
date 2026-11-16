#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace stanli {

// What the embedded stanc3 returns: the portable MIR or an error, the
// compiler's warnings, and the O1 fallback notice when O1 was skipped.
struct StancResult {
  std::optional<std::string> mir;
  std::string error;
  std::vector<std::string> warnings;
  std::optional<std::string> o1_fallback;
};

inline constexpr int kStancModelOnly = 1;
inline constexpr int kStancFastMath = 2;

// Defined in stanc_embed_c.cpp, linked only into builds with
// STANLI_EMBED_STANC.
StancResult embedded_stanc_compile(const char* stan_code, const char* filename,
                                   const char* const* include_paths,
                                   size_t include_path_count, int flags);

}  // namespace stanli
