// Runs a fuzz target once over each input file named on the command line, or
// over each file in a named directory, for builds without libFuzzer. CTest
// replays every seed corpus this way, so the harnesses build and pass on
// every platform.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  std::vector<fs::path> files;
  for (int i = 1; i < argc; ++i) {
    if (fs::is_directory(argv[i])) {
      for (const auto& e : fs::directory_iterator(argv[i]))
        if (e.is_regular_file()) files.push_back(e.path());
    } else {
      files.emplace_back(argv[i]);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "no inputs\n");
    return 1;
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    std::ifstream s(f, std::ios::binary);
    if (!s) {
      std::fprintf(stderr, "cannot read %s\n", f.string().c_str());
      return 1;
    }
    // One spare byte, so that an empty input still has a valid pointer.
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(s), {});
    bytes.push_back(0);
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size() - 1);
  }
  std::printf("replayed %zu inputs\n", files.size());
  return 0;
}
