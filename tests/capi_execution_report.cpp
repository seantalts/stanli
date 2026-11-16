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
    const bool report =
        argc == 3 && std::string(argv[2]) == "--execution-report";
    if (argc != 2 && !report)
      throw std::runtime_error(
          "usage: capi_execution_report MODEL.tmir.sexp [--execution-report]");
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("could not open model MIR");
    const std::string mir((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    char err[8192] = {};
    stanli_model_opts opts;
    stanli_model_opts_init(&opts);
    opts.execution_report = report ? 1 : 0;
    std::unique_ptr<stanli_model, decltype(&stanli_model_free)> model(
        stanli_model_new_with_opts(mir.c_str(), "{}", &opts, err, sizeof err),
        stanli_model_free);
    if (!model) throw std::runtime_error(err);
    std::cout << "{\"columns\":" << stanli_wa_n_columns(model.get()) << "}\n";
    return std::cout ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "capi_execution_report: " << e.what() << '\n';
    return 1;
  }
}
