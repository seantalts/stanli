#pragma once

#include "csv_writer.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace stanli::tooling {

// Per-chain CSV ordering without retaining a second draw matrix. Chain zero
// can stream directly when its header is known. Other chains format once
// into buffered, automatically deleted files and are copied in chain order.
class ChainCsv {
 public:
  explicit ChainCsv(bool direct)
      : file_(direct ? stdout : temporary(), Closer{!direct}),
        writer_(file_.get()) {}

  CsvWriter& writer() { return writer_; }
  void finish() { writer_.flush(); }

  // Before an interpreter discovers its columns, rejected rows contain only
  // sampler diagnostics. Fill their missing output columns at finalization.
  void copy_to(std::FILE* output, size_t pending_errors, size_t width) {
    if (file_.get() == output) return;
    if (std::fflush(file_.get()) || std::fseek(file_.get(), 0, SEEK_SET))
      throw std::runtime_error("cannot rewind chain CSV");
    CsvWriter padding(output);
    for (size_t i = 0; i < pending_errors; ++i) {
      char prefix[512];  // at most seven sampler values, independent of width
      if (!std::fgets(prefix, sizeof(prefix), file_.get()))
        throw std::runtime_error("missing rejected chain CSV row");
      const size_t n = std::strlen(prefix);
      if (n == 0 || prefix[n - 1] != '\n')
        throw std::runtime_error("invalid rejected chain CSV row");
      if (std::fwrite(prefix, 1, n - 1, output) != n - 1)
        throw std::runtime_error("CSV output write failed");
      if (n > 1 && width && std::fputc(',', output) == EOF)
        throw std::runtime_error("CSV output write failed");
      for (size_t j = 0; j < width; ++j)
        padding.value(std::numeric_limits<double>::quiet_NaN());
      padding.end_row();
      padding.flush();
    }
    std::array<char, 65536> buffer;
    while (const size_t n =
               std::fread(buffer.data(), 1, buffer.size(), file_.get()))
      if (std::fwrite(buffer.data(), 1, n, output) != n)
        throw std::runtime_error("CSV output write failed");
    if (std::ferror(file_.get()))
      throw std::runtime_error("chain CSV read failed");
  }

 private:
  struct Closer {
    bool owned;
    void operator()(std::FILE* file) const {
      if (owned && file) std::fclose(file);
    }
  };

  static std::FILE* temporary() {
#ifdef _WIN32
    // MSVC tmpfile uses the drive root. Respect the user's temporary folder
    // and delete the file when its handle closes, including error unwinding.
    wchar_t directory[MAX_PATH], path[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, directory);
    if (n == 0 || n >= MAX_PATH ||
        !GetTempFileNameW(directory, L"sli", 0, path))
      throw std::runtime_error("cannot create chain CSV temporary file");
    HANDLE handle = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      DeleteFileW(path);
      throw std::runtime_error("cannot open chain CSV temporary file");
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                   _O_BINARY | _O_RDWR);
    if (fd == -1) {
      CloseHandle(handle);
      throw std::runtime_error("cannot attach chain CSV descriptor");
    }
    std::FILE* file = _fdopen(fd, "w+b");
    if (!file) _close(fd);
#else
    std::FILE* file = std::tmpfile();
#endif
    if (!file) throw std::runtime_error("cannot open chain CSV temporary file");
    return file;
  }

  std::unique_ptr<std::FILE, Closer> file_;
  CsvWriter writer_;
};

}  // namespace stanli::tooling
