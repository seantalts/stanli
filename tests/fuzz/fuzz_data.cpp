// Fuzzes the data JSON reader that every interface passes model data to.
#include <stanli/data.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  try {
    stanli::DataMap::from_json(
        std::string(reinterpret_cast<const char*>(data), size));
  } catch (const std::exception&) {
  }
  return 0;
}
