#include "stanc_process.hpp"

#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace stanli::tooling {
namespace {

#ifdef _WIN32

class Handle {
 public:
  explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : handle_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    reset(other.release());
    return *this;
  }
  ~Handle() { reset(); }

  HANDLE get() const { return handle_; }
  HANDLE release() {
    const HANDLE handle = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return handle;
  }
  void reset(HANDLE handle = INVALID_HANDLE_VALUE) {
    if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr)
      CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_;
};

std::runtime_error windows_error(const char* operation) {
  return std::runtime_error(std::string(operation) + " failed (Windows error " +
                            std::to_string(GetLastError()) + ")");
}

std::wstring widen_utf8(const std::string& text) {
  if (text.empty()) return {};
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (size == 0) throw windows_error("MultiByteToWideChar");
  std::wstring wide(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), wide.data(),
                          size) == 0)
    throw windows_error("MultiByteToWideChar");
  return wide;
}

// CreateProcess receives one command-line string even when the application is
// named separately. Quote each argv element by the CommandLineToArgvW rules so
// spaces and trailing backslashes survive unchanged in the child.
std::wstring quote_windows_arg(const std::wstring& arg) {
  if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring quoted(1, L'"');
  size_t backslashes = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(ch);
    }
    backslashes = 0;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

std::string run_process_windows(const std::string& exe,
                                const std::vector<std::string>& args,
                                bool inherit_stderr) {
  SECURITY_ATTRIBUTES inheritable{};
  inheritable.nLength = sizeof(inheritable);
  inheritable.bInheritHandle = TRUE;

  HANDLE raw_read = INVALID_HANDLE_VALUE;
  HANDLE raw_write = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&raw_read, &raw_write, &inheritable, 0))
    throw windows_error("CreatePipe");
  Handle read_pipe(raw_read);
  Handle write_pipe(raw_write);
  if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0))
    throw windows_error("SetHandleInformation");

  Handle null_device(CreateFileW(
      L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (null_device.get() == INVALID_HANDLE_VALUE)
    throw windows_error("CreateFileW(NUL)");

  const std::wstring executable = widen_utf8(exe);
  std::wstring command_line = quote_windows_arg(executable);
  for (const auto& arg : args) {
    command_line.push_back(L' ');
    command_line += quote_windows_arg(widen_utf8(arg));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_device.get();
  startup.hStdOutput = write_pipe.get();
  startup.hStdError =
      inherit_stderr ? GetStdHandle(STD_ERROR_HANDLE) : null_device.get();
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                      &process))
    throw windows_error("CreateProcessW");
  Handle process_handle(process.hProcess);
  Handle thread_handle(process.hThread);
  write_pipe.reset();
  null_device.reset();

  std::string out;
  std::array<char, 1 << 16> buffer;
  for (;;) {
    DWORD read_size = 0;
    if (!ReadFile(read_pipe.get(), buffer.data(),
                  static_cast<DWORD>(buffer.size()), &read_size, nullptr)) {
      if (GetLastError() == ERROR_BROKEN_PIPE) break;
      throw windows_error("ReadFile");
    }
    if (read_size == 0) break;
    out.append(buffer.data(), read_size);
  }
  if (WaitForSingleObject(process_handle.get(), INFINITE) != WAIT_OBJECT_0)
    throw windows_error("WaitForSingleObject");
  // _popen(..., "r") used to give callers a text-mode stream. Preserve that
  // behavior now that ReadFile sees the child's raw CRLF bytes.
  std::string normalized;
  normalized.reserve(out.size());
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
    normalized.push_back(out[i]);
  }
  return normalized;
}

#else

std::string run_process_posix(const std::string& exe,
                              const std::vector<std::string>& args,
                              bool inherit_stderr) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  int descriptors[2];
  if (pipe(descriptors) != 0)
    throw std::runtime_error(std::string("pipe failed: ") +
                             std::strerror(errno));

  const pid_t child = fork();
  if (child == -1) {
    const int error = errno;
    close(descriptors[0]);
    close(descriptors[1]);
    throw std::runtime_error(std::string("fork failed: ") +
                             std::strerror(error));
  }
  if (child == 0) {
    close(descriptors[0]);
    if (dup2(descriptors[1], STDOUT_FILENO) == -1) _exit(127);
    close(descriptors[1]);
    if (!inherit_stderr) {
      const int null_error = open("/dev/null", O_WRONLY);
      if (null_error == -1 || dup2(null_error, STDERR_FILENO) == -1) _exit(127);
      close(null_error);
    }
    execv(exe.c_str(), argv.data());
    _exit(127);
  }

  close(descriptors[1]);
  std::string out;
  std::array<char, 1 << 16> buffer;
  for (;;) {
    const ssize_t read_size =
        read(descriptors[0], buffer.data(), buffer.size());
    if (read_size > 0) {
      out.append(buffer.data(), static_cast<size_t>(read_size));
      continue;
    }
    if (read_size == -1 && errno == EINTR) continue;
    if (read_size == -1) {
      const int error = errno;
      close(descriptors[0]);
      while (waitpid(child, nullptr, 0) == -1 && errno == EINTR) {
      }
      throw std::runtime_error(std::string("read failed: ") +
                               std::strerror(error));
    }
    break;
  }
  close(descriptors[0]);
  while (waitpid(child, nullptr, 0) == -1) {
    if (errno != EINTR)
      throw std::runtime_error(std::string("waitpid failed: ") +
                               std::strerror(errno));
  }
  return out;
}

#endif

std::string run_process(const std::string& exe,
                        const std::vector<std::string>& args,
                        bool inherit_stderr) {
#ifdef _WIN32
  return run_process_windows(exe, args, inherit_stderr);
#else
  return run_process_posix(exe, args, inherit_stderr);
#endif
}

bool is_executable_file(const std::string& path) {
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(widen_utf8(path).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
         access(path.c_str(), X_OK) == 0;
#endif
}

}  // namespace

std::string run_stanc_process(const std::string& stanc,
                              const std::string& model) {
  return run_process(stanc, {"--O1", "--debug-optimized-mir", model}, false);
}

std::string run_portable_compiler(const std::string& compiler,
                                  const std::string& model) {
  return run_process(compiler, {"--model-only", model}, true);
}

std::string executable_directory() {
  std::string path;
#ifdef _WIN32
  std::wstring wide(32768, L'\0');
  const DWORD n =
      GetModuleFileNameW(nullptr, wide.data(), static_cast<DWORD>(wide.size()));
  if (n == 0 || n >= wide.size()) return "";
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(n), nullptr,
                          0, nullptr, nullptr);
  if (size <= 0) return "";
  path.assign(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(n), path.data(),
                      size, nullptr, nullptr);
  const size_t slash = path.find_last_of("\\/");
#else
  char resolved[PATH_MAX];
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string raw(size, '\0');
  if (_NSGetExecutablePath(raw.data(), &size) != 0) return "";
  raw.resize(std::strlen(raw.c_str()));
  path =
      realpath(raw.c_str(), resolved) != nullptr ? std::string(resolved) : raw;
#else
  const ssize_t n = readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
  if (n <= 0) return "";
  path.assign(resolved, static_cast<size_t>(n));
#endif
  const size_t slash = path.find_last_of('/');
#endif
  return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string find_portable_compiler(const std::string& directory) {
#ifdef _WIN32
  const char* const name = "stanli-compile.exe";
  const char separator = ';';
  const char slash = '\\';
#else
  const char* const name = "stanli-compile";
  const char separator = ':';
  const char slash = '/';
#endif
  if (!directory.empty()) {
    const std::string beside = directory + slash + name;
    if (is_executable_file(beside)) return beside;
  }
  const char* path = std::getenv("PATH");
  if (path == nullptr) return "";
  const std::string entries = path;
  size_t start = 0;
  while (start <= entries.size()) {
    size_t end = entries.find(separator, start);
    if (end == std::string::npos) end = entries.size();
    const std::string dir = entries.substr(start, end - start);
    if (!dir.empty()) {
      const std::string candidate = dir + slash + name;
      if (is_executable_file(candidate)) return candidate;
    }
    start = end + 1;
  }
  return "";
}

}  // namespace stanli::tooling
