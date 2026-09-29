// Native host for test_execution_report_cli.py so sanitizer builds initialize
// their runtime before loading the shipped C ABI library.
#include <stanli/capi.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: capi_execution_report MODEL.tmir.sexp");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("could not open model MIR");
    const std::string mir((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    char err[8192] = {};
    std::unique_ptr<stanli_model, decltype(&stanli_model_free)> model(
        stanli_model_new(mir.c_str(), "{}", err, sizeof err),
        stanli_model_free);
    if (!model) throw std::runtime_error(err);
    std::cout << "{\"columns\":" << stanli_wa_n_columns(model.get()) << "}\n";
    return std::cout ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "capi_execution_report: " << e.what() << '\n';
    return 1;
  }
}
