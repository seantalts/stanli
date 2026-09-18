#include "../tools/stanc_process.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
const char* const kPortableName = "stanli-compile.exe";
#else
const char* const kPortableName = "stanli-compile";
#endif

void set_path(const std::string& value) {
#ifdef _WIN32
  _putenv_s("PATH", value.c_str());
#else
  setenv("PATH", value.c_str(), 1);
#endif
}

fs::path install_fake(const fs::path& self, const fs::path& target) {
  fs::copy_file(self, target);
  fs::permissions(
      target,
      fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
      fs::perm_options::add);
  return target;
}

void expect(const std::string& got, const std::string& want, const char* what) {
  if (got != want)
    throw std::runtime_error(std::string(what) + ": got \"" + got +
                             "\", want \"" + want + "\"");
}

}  // namespace

int main(int argc, char** argv) {
  // A copied instance of this test is the fake compiler used by the parent:
  // stock stanc's argument shape, or stanli-compile's.
  if (argc == 4) {
    if (std::string(argv[1]) != "--O1" ||
        std::string(argv[2]) != "--debug-optimized-mir")
      return 2;
    std::cout << "MIR from " << argv[3] << '\n';
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--model-only") {
    std::cout << "PORTABLE from " << argv[2] << '\n';
    std::cerr << "fake compiler warning\n";
    return 0;
  }
  if (argc != 1) return 2;

  const auto nonce =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() /
                        ("stanli stanc ' process " + std::to_string(nonce));
  try {
    fs::create_directories(root / "empty");
    fs::create_directories(root / "on path");
    const fs::path self = fs::absolute(argv[0]);
#ifdef _WIN32
    const fs::path compiler =
        install_fake(self, root / "fake stanc ' compiler.exe");
#else
    const fs::path compiler =
        install_fake(self, root / "fake stanc ' compiler");
#endif
    const fs::path model = root / "model with ' quote.stan";
    std::ofstream(model)
        << "parameters { real y; } model { y ~ normal(0, 1); }\n";

    expect(
        stanli::tooling::run_stanc_process(compiler.string(), model.string()),
        "MIR from " + model.string() + "\n", "stanc argument shape");

    const fs::path beside = install_fake(self, root / kPortableName);
    expect(
        stanli::tooling::run_portable_compiler(beside.string(), model.string()),
        "PORTABLE from " + model.string() + "\n",
        "stanli-compile argument shape");

    const fs::path on_path =
        install_fake(self, root / "on path" / kPortableName);
    set_path((root / "on path").string());
    expect(stanli::tooling::find_portable_compiler(root.string()),
           beside.string(), "stanli-compile beside the directory");
    expect(stanli::tooling::find_portable_compiler((root / "empty").string()),
           on_path.string(), "stanli-compile on PATH");
    set_path((root / "empty").string());
    expect(stanli::tooling::find_portable_compiler((root / "empty").string()),
           "", "no stanli-compile anywhere");

    fs::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::error_code ignored;
    fs::remove_all(root, ignored);
    return 1;
  }
}
