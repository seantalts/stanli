#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace stanli_test {

namespace detail {

#ifdef _WIN32
inline int file_descriptor(FILE* file) { return ::_fileno(file); }
inline int duplicate_fd(int fd) { return ::_dup(fd); }
inline int replace_fd(int from, int to) { return ::_dup2(from, to); }
inline int close_fd(int fd) { return ::_close(fd); }
#else
inline int file_descriptor(FILE* file) { return ::fileno(file); }
inline int duplicate_fd(int fd) { return ::dup(fd); }
inline int replace_fd(int from, int to) { return ::dup2(from, to); }
inline int close_fd(int fd) { return ::close(fd); }
#endif

}  // namespace detail

// Process-wide stdout capture for single-threaded tests. Construct only after
// setup whose output is intentionally excluded, then call finish() before any
// assertion diagnostic is printed.
class StdoutCapture {
 public:
  StdoutCapture() {
    std::fflush(stdout);
    file_ = std::tmpfile();
    if (!file_)
      throw std::runtime_error("tmpfile failed while capturing stdout");

    target_ = detail::file_descriptor(stdout);
    saved_ = detail::duplicate_fd(target_);
    if (target_ < 0 || saved_ < 0 ||
        detail::replace_fd(detail::file_descriptor(file_), target_) < 0) {
      if (saved_ >= 0) detail::close_fd(saved_);
      std::fclose(file_);
      file_ = nullptr;
      target_ = -1;
      saved_ = -1;
      throw std::runtime_error("dup2 failed while capturing stdout");
    }
  }

  StdoutCapture(const StdoutCapture&) = delete;
  StdoutCapture& operator=(const StdoutCapture&) = delete;

  ~StdoutCapture() {
    std::fflush(stdout);
    restore();
    if (file_) std::fclose(file_);
  }

  std::string finish() {
    if (finished_) return text_;
    std::fflush(stdout);
    restore();
    std::rewind(file_);
    char buffer[256];
    while (const size_t n = std::fread(buffer, 1, sizeof(buffer), file_))
      text_.append(buffer, n);
    if (std::ferror(file_))
      throw std::runtime_error("failed while reading captured stdout");
    finished_ = true;
    return text_;
  }

 private:
  void restore() noexcept {
    if (saved_ < 0) return;
    detail::replace_fd(saved_, target_);
    detail::close_fd(saved_);
    saved_ = -1;
  }

  FILE* file_ = nullptr;
  int target_ = -1;
  int saved_ = -1;
  bool finished_ = false;
  std::string text_;
};

}  // namespace stanli_test
