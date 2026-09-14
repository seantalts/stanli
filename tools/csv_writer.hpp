#pragma once

#include <fmt/format.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace stanli::tooling {

// Match %.17g in the CLI's C locale without per-field stdio locking or
// locale-aware conversion. Use {fmt} for the same fast conversion on older
// macOS and libstdc++ versions without floating-point std::to_chars. Bound
// buffering independently of the row width.
class CsvWriter {
 public:
  explicit CsvWriter(std::FILE* stream) : stream_(stream) {
    buffer_.reserve(65536 + 64);
  }

  void value(double value) {
    if (!first_) buffer_ += ',';
    first_ = false;
    // printf's NaN sign spelling differs across platforms. Preserve the
    // CLI's existing spelling for failed generated-quantity rows as well.
    if (std::isnan(value)) {
      char number[32];
      const int size = std::snprintf(number, sizeof(number), "%.17g", value);
      if (size < 0 || static_cast<size_t>(size) >= sizeof(number))
        throw std::runtime_error("CSV number formatting failed");
      buffer_.append(number, static_cast<size_t>(size));
    } else {
      fmt::format_to(std::back_inserter(buffer_), "{:.17g}", value);
    }
    if (buffer_.size() >= 65536) flush();
  }

  void end_row() {
    buffer_ += '\n';
    first_ = true;
    if (buffer_.size() >= 65536) flush();
  }

  void flush() {
    if (std::fwrite(buffer_.data(), 1, buffer_.size(), stream_) !=
        buffer_.size())
      throw std::runtime_error("CSV output write failed");
    buffer_.clear();
  }

 private:
  std::FILE* stream_;
  std::string buffer_;
  bool first_ = true;
};

}  // namespace stanli::tooling
