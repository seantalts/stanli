// Native host for the recorded CmdStan comparison in
// test_function_reference.py. Link the shipped C ABI library so sanitizer
// initialization happens at startup.
#include <stanli/function.hpp>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  try {
    if (argc < 3)
      throw std::runtime_error(
          "usage: capi_function_reference MODEL.tmir.sexp X [X ...]");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("could not open function MIR");
    const std::string mir((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    char err[8192] = {};
    std::unique_ptr<stanli_function, decltype(&stanli_function_free)> function(
        stanli_function_new_from_mir(mir.c_str(), "combined", err, sizeof err),
        stanli_function_free);
    if (!function) throw std::runtime_error(err);

    // Round-trip every returned double for the independent Python ULP check.
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    const auto writer = [](void*, int is_int, const double* reals,
                           size_t real_size, const int*, size_t int_size,
                           const int64_t* dims, size_t dim_size) -> int {
      std::cout << "{\"is_int\":" << is_int << ",\"values\":[";
      for (size_t i = 0; i < real_size; ++i) {
        if (i) std::cout << ',';
        std::cout << reals[i];
      }
      std::cout << "],\"integer_size\":" << int_size << ",\"dims\":[";
      for (size_t i = 0; i < dim_size; ++i) {
        if (i) std::cout << ',';
        std::cout << dims[i];
      }
      std::cout << "]}\n";
      return std::cout ? 0 : 1;
    };
    // Reuse one handle, including repeated input points, just like a binding.
    for (int i = 2; i < argc; ++i) {
      const double x = std::stod(argv[i]);
      const stanli_function_argument arg{"x", 0, &x, nullptr, 1, nullptr, 0};
      if (stanli_function_call_values(function.get(), &arg, 1, writer, nullptr,
                                      err, sizeof err) != 0)
        throw std::runtime_error(err);
    }
    return std::cout ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "capi_function_reference: " << e.what() << '\n';
    return 1;
  }
}
