// write_array lowered beside log_prob, seen through the shipped C ABI: what a
// binding reads from a model (names, columns, warnings, a write_array row, a
// gradient) and what a multi-chain run writes through the per-chain
// write_array clones are those of a sequential construction, including when
// many models are built at once from several threads.
#include "env_helpers.hpp"

#include <stanli/capi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
std::mutex print_mutex;

void expect(bool ok, const std::string& what) {
  if (ok) return;
  std::lock_guard<std::mutex> lock(print_mutex);
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

void expect_same(const std::string& what, const std::string& got,
                 const std::string& want) {
  if (got == want) return;
  std::lock_guard<std::mutex> lock(print_mutex);
  ++failures;
  std::printf("FAIL %s\n  got  %.400s\n  want %.400s\n", what.c_str(),
              got.c_str(), want.c_str());
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct EnvGuard {
  std::string name;
  EnvGuard(const char* n, const char* v) : name(n) { test_setenv(n, v, 1); }
  ~EnvGuard() { test_unsetenv(name.c_str()); }
};

void put_bits(std::ostringstream& o, double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, sizeof bits);
  o << " " << std::hex << bits << std::dec;
}

struct Model {
  std::string name, mir, data;
};

std::string account(const Model& m) {
  std::ostringstream o;
  stanli_model_opts opts;
  stanli_model_opts_init(&opts);
  opts.seed = 7;
  char err[8192]{};
  stanli_model* model = stanli_model_new_with_opts(
      m.mir.c_str(), m.data.c_str(), &opts, err, sizeof err);
  if (model == nullptr) return std::string("error: ") + err + "\n";

  const int64_t n = stanli_n_unconstrained(model);
  const int64_t n_con = stanli_n_constrained(model);
  o << "unc " << n << " con " << n_con << "\n";
  for (int64_t i = 0; i < n_con; ++i)
    o << stanli_constrained_name(model, i) << " ";
  const int64_t cols = stanli_wa_n_columns(model);
  o << "\nwa " << cols << " gq_start " << stanli_wa_n_generated_start(model)
    << "\n";
  for (int64_t i = 0; i < cols; ++i)
    o << stanli_wa_column_name(model, i) << " ";
  o << "\nwarnings [" << stanli_warnings(model) << "] td_rng "
    << stanli_transformed_data_rng(model) << " reduce_sum "
    << stanli_reduce_sum_count(model) << "\n";

  std::vector<double> q(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i)
    q[static_cast<size_t>(i)] = 0.1 + 0.05 * static_cast<double>(i % 7);
  double lp = 0;
  std::vector<double> grad(static_cast<size_t>(n));
  const int grc = stanli_grad(model, q.data(), &lp, grad.data());
  o << "grad rc " << grc;
  put_bits(o, lp);
  for (double g : grad) put_bits(o, g);
  o << "\n";

  if (cols > 0) {
    std::vector<double> row(static_cast<size_t>(cols));
    stanli_wa_seed_chain(model, 99, 0);
    const int rc = stanli_wa_row(model, q.data(), row.data());
    o << "wa_row rc " << rc;
    for (double v : row) put_bits(o, v);
    o << "\n";
  }

  stanli_sample_opts so;
  stanli_sample_opts_init(&so);
  so.seed = 11;
  so.chains = 3;
  so.warmup = 30;
  so.samples = 30;
  so.num_threads = stanli_thread_safe() ? 3 : 1;
  const int64_t rows = stanli_n_stored_draws(&so);
  const int64_t width = cols > 0 ? cols : n_con;
  std::vector<double> draws(static_cast<size_t>(so.chains * rows * n));
  std::vector<double> values(static_cast<size_t>(so.chains * rows * width));
  const int src = stanli_sample_multi_metric(
      model, &so, nullptr, 0, draws.data(), nullptr, values.data(), nullptr,
      nullptr, nullptr, nullptr, nullptr, nullptr, err, sizeof err);
  o << "sample rc " << src;
  for (double d : draws) put_bits(o, d);
  o << "\nvalues";
  for (double v : values) put_bits(o, v);
  o << "\n";
  stanli_model_free(model);
  return o.str();
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::vector<Model> models;
  for (const char* name :
       {"conj", "wapar_wa_fallback", "alg_gauss", "gq_bounded_block",
        "wapar_lp_error", "wapar_both_error"}) {
    Model m;
    m.name = name;
    m.mir = slurp(std::string("tests/fixtures/") + name + ".tmir.sexp");
    m.data = slurp(std::string("tests/fixtures/") + name + ".json");
    if (m.data.empty()) m.data = "{}";
    expect(!m.mir.empty(), std::string("fixture present: ") + name);
    models.push_back(std::move(m));
  }

  std::map<std::string, std::string> sequential;
  {
    EnvGuard seq("STANLI_PARALLEL_WA", "0");
    for (const Model& m : models) sequential[m.name] = account(m);
  }
  EnvGuard force("STANLI_PARALLEL_WA", "1");
  for (const Model& m : models) {
    expect_same("C ABI " + m.name + ": parallel vs sequential", account(m),
                sequential[m.name]);
  }
  expect(sequential["conj"].find("sample rc 0") != std::string::npos,
         "the reference run samples");
  expect(sequential["wapar_wa_fallback"].find("MIR interpreter") !=
             std::string::npos,
         "the interpreter warning reaches the binding");
  expect(sequential["wapar_lp_error"].find("append_row column mismatch") !=
             std::string::npos,
         "the log_prob error reaches the binding");

  {
    EnvGuard strict("STANLI_NO_INTERPRETER", "1");
    std::map<std::string, std::string> want;
    {
      EnvGuard seq("STANLI_PARALLEL_WA", "0");
      for (const Model& m : models) want[m.name] = account(m);
    }
    for (const Model& m : models)
      expect_same("C ABI " + m.name + " under STANLI_NO_INTERPRETER",
                  account(m), want[m.name]);
    expect(want["wapar_wa_fallback"].find("STANLI_NO_INTERPRETER") !=
               std::string::npos,
           "the strict switch refuses the fallback model");
    expect(want["wapar_both_error"].find("append_row column mismatch") !=
               std::string::npos,
           "the log_prob error wins when both sides fail");
  }

  if (stanli_thread_safe()) {
    constexpr int kThreads = 6;
    std::atomic<int> mismatches{0};
    for (const bool same_model : {true, false}) {
      std::vector<std::thread> threads;
      for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
          for (size_t i = 0; i < models.size(); ++i) {
            const Model& m =
                same_model
                    ? models[0]
                    : models[(i + static_cast<size_t>(t)) % models.size()];
            if (account(m) != sequential[m.name]) {
              ++mismatches;
              std::lock_guard<std::mutex> lock(print_mutex);
              std::printf("FAIL concurrent C ABI construction of %s differs\n",
                          m.name.c_str());
            }
          }
        });
      for (auto& t : threads) t.join();
    }
    expect(mismatches == 0, "concurrent constructions match sequential ones");
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
