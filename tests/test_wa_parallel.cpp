// write_array is lowered on a second thread while log_prob is lowered.
//
// What has to hold: the lowered programs, columns, names, notes, warnings and
// errors are those of a sequential construction (STANLI_PARALLEL_WA=0),
// whichever side fails and whenever; the second thread is always joined; and
// constructing many models at once, from several threads, gives each caller
// what it would have got alone.
#include "env_helpers.hpp"
#include "lowering_digest.hpp"
#include "../runtime/src/lower_work.hpp"
#include "../runtime/src/lower_worker.hpp"
#include "../runtime/third_party/bridgestan.h"

#include <stanli/bridgestan_internal.hpp>
#include <stanli/compile.hpp>
#include <stanli/execution_report.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/threading.hpp>

#include <stan/math/rev/core/chainablestack.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <functional>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#if defined(__SANITIZE_THREAD__)
#define STANLI_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STANLI_UNDER_TSAN 1
#endif
#endif

namespace {

#ifdef STANLI_UNDER_TSAN
constexpr size_t kStride = 2;
#else
constexpr size_t kStride = 1;
#endif

namespace fs = std::filesystem;
namespace ld = stanli::lowering_digest;

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

void expect_same(const std::string& what, const std::string& got,
                 const std::string& want) {
  if (got == want) return;
  ++failures;
  std::string g, w;
  std::istringstream gs(got), ws(want);
  std::printf("FAIL %s\n", what.c_str());
  for (;;) {
    const bool a = static_cast<bool>(std::getline(gs, g));
    const bool b = static_cast<bool>(std::getline(ws, w));
    if (!a && !b) break;
    if (!a) g.clear();
    if (!b) w.clear();
    if (g != w) {
      std::printf("  parallel:   %.300s\n  sequential: %.300s\n", g.c_str(),
                  w.c_str());
      break;
    }
  }
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

// Threads in this process right now, or -1 where there is no way to count
// them. A write_array worker that is not joined shows up here.
int live_threads() {
#if defined(__APPLE__)
  thread_act_array_t list = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return -1;
  for (mach_msg_type_number_t i = 0; i < count; ++i)
    mach_port_deallocate(mach_task_self(), list[i]);
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list),
                count * sizeof(thread_act_t));
  return static_cast<int>(count);
#elif defined(__linux__)
  int n = 0;
  for (const auto& e : fs::directory_iterator("/proc/self/task")) {
    (void)e;
    ++n;
  }
  return n;
#else
  return -1;
#endif
}

struct Fixture {
  std::string name, mir;
  stanli::DataMap data;
};

std::vector<Fixture> load_fixtures() {
  std::vector<std::string> names;
  const std::string suffix = ".tmir.sexp";
  for (const auto& e : fs::directory_iterator("tests/fixtures")) {
    const std::string p = e.path().filename().string();
    if (p.size() > suffix.size() &&
        p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0)
      names.push_back(p.substr(0, p.size() - suffix.size()));
  }
  std::sort(names.begin(), names.end());
  std::vector<Fixture> out;
  for (const std::string& name : names) {
    Fixture f;
    f.name = name;
    f.mir = slurp("tests/fixtures/" + name + ".tmir.sexp");
    const std::string json = "tests/fixtures/" + name + ".json";
    if (fs::exists(json)) {
      try {
        f.data = stanli::DataMap::from_json(slurp(json));
      } catch (const std::exception&) {
        continue;
      }
    }
    out.push_back(std::move(f));
  }
  return out;
}

struct Run {
  std::string text;
  bool parallel = false;
  bool has_write_array = false;
};

Run construct(const Fixture& f, unsigned seed = 1, bool values = true) {
  Run r;
  try {
    stanli::CompiledModel cm = stanli::compile_model(f.mir, f.data, seed);
    r.has_write_array = cm.write_array.has_value();
    r.parallel = cm.write_array && cm.write_array->lowered_in_parallel;
    // Evaluating a graph needs an autodiff stack on this thread; the
    // construction above must not.
    std::optional<stan::math::ChainableStack> evaluation_stack;
    if (stan::math::ChainableStack::instance_ == nullptr)
      evaluation_stack.emplace();
    r.text = ld::describe(cm) + (values ? ld::describe_values(cm) : "");
  } catch (const std::exception& e) {
    r.text = ld::describe_exception(e);
  }
  return r;
}

Run construct_sequential(const Fixture& f, unsigned seed = 1) {
  EnvGuard seq("STANLI_PARALLEL_WA", "0");
  return construct(f, seed);
}

Run construct_parallel(const Fixture& f, unsigned seed = 1,
                       bool values = true) {
  EnvGuard force("STANLI_PARALLEL_WA", "1");
  return construct(f, seed, values);
}

const bool kThreaded =
    stanli::thread_safe_build() && std::thread::hardware_concurrency() > 1;

// ---------------------------------------------------------------------------

const Fixture* find(const std::vector<Fixture>& fixtures, const char* name) {
  for (const Fixture& f : fixtures)
    if (f.name == name) return &f;
  expect(false, std::string("fixture present: ") + name);
  return nullptr;
}

void test_corpus_equivalence(const std::vector<Fixture>& fixtures) {
  int with_wa = 0, ran_parallel = 0, ran_by_default = 0, threw = 0;
  int visited = 0;
  for (size_t i = 0; i < fixtures.size(); i += kStride) {
    const Fixture& f = fixtures[i];
    ++visited;
    const Run seq = construct_sequential(f);
    const Run par = construct_parallel(f);
    const Run fallback = construct(f);
    expect_same("parallel vs sequential: " + f.name, par.text, seq.text);
    expect_same("default vs sequential: " + f.name, fallback.text, seq.text);
    expect(!seq.parallel, "forced sequential stays on one thread: " + f.name);
    with_wa += par.has_write_array;
    ran_parallel += par.parallel;
    ran_by_default += fallback.parallel;
    threw += par.text.compare(0, 6, "THROWS") == 0;
    if (par.has_write_array)
      expect(par.parallel == kThreaded,
             "write_array lowered on a second thread exactly when threads "
             "are available: " +
                 f.name);
  }
  std::printf(
      "corpus: %d of %zu fixtures, %d with write_array, %d lowered in "
      "parallel when forced, %d by default, %d refused\n",
      visited, fixtures.size(), with_wa, ran_parallel, ran_by_default, threw);
  expect(with_wa >= 100 / static_cast<int>(kStride),
         "enough fixtures reach write_array");
  expect(ran_parallel == (kThreaded ? with_wa : 0),
         "every write_array of the corpus took the forced path");
  expect(ran_by_default < with_wa,
         "the default leaves small models on one thread");
}

void test_default_choice(const std::vector<Fixture>& fixtures) {
  const Fixture* tiny = find(fixtures, "conj");
  const Fixture* heavy = find(fixtures, "wapar_heavy");
  if (!tiny || !heavy) return;
  expect(!construct(*tiny).parallel,
         "a model with little to lower stays on one thread");
  expect(construct(*heavy).parallel == kThreaded,
         "a model with a lot to lower on both sides uses a second thread");
  expect_same("heavy model by default vs sequential", construct(*heavy).text,
              construct_sequential(*heavy).text);
  {
    EnvGuard force("STANLI_PARALLEL_WA", "force");
    expect(construct(*tiny).parallel == kThreaded,
           "STANLI_PARALLEL_WA=force uses the second thread for any model");
  }
  {
    EnvGuard automatic("STANLI_PARALLEL_WA", "auto");
    expect(construct(*heavy).parallel == kThreaded,
           "STANLI_PARALLEL_WA=auto is the default");
  }
  {
    EnvGuard unknown("STANLI_PARALLEL_WA", "sometimes");
    expect(!construct(*heavy).parallel,
           "an unrecognized STANLI_PARALLEL_WA keeps one thread");
  }
}

void test_deep_nesting(const std::vector<Fixture>& fixtures) {
  const Fixture* deep = find(fixtures, "wapar_deep");
  if (!deep) return;
  const Run seq = construct_sequential(*deep);
  expect(seq.text.compare(0, 6, "THROWS") != 0,
         "the deeply nested model lowers on the calling thread: " + seq.text);
  const Run par = construct_parallel(*deep);
  expect(par.parallel == kThreaded, "the deep model used the second thread");
  expect_same("deeply nested model, parallel vs sequential", par.text,
              seq.text);
}

void test_data_payload() {
  std::map<std::string, stanli::DataMap::Entry> env;
  expect(stanli::lower_detail::data_payload_bytes(env) == 0,
         "no data, no payload");
  stanli::DataMap::Entry reals, ints;
  reals.r.assign(1000, 0.0);
  ints.is_int = true;
  ints.i.assign(500, 1);
  ints.r.assign(500, 1.0);
  env["y"] = reals;
  env["n"] = ints;
  expect(stanli::lower_detail::data_payload_bytes(env) ==
             (1000 + 500) * sizeof(double) + 500 * sizeof(int),
         "payload counts the real and integer arrays an entry holds");
}

void test_gates(const std::vector<Fixture>& fixtures) {
  const Fixture* model = nullptr;
  for (const Fixture& f : fixtures)
    if (f.name == "conj") model = &f;
  expect(model != nullptr, "conj fixture present");
  if (!model) return;
  const Run alone = construct_parallel(*model);
  expect(alone.parallel == kThreaded, "conj parallel when forced");
  {
    // An active execution trace is thread_local; the second thread would
    // record into nothing.
    stanli::ExecutionTrace trace;
    stanli::ExecutionTraceScope scope(trace);
    const Run traced = construct_parallel(*model);
    expect(!traced.parallel, "an active execution trace stays sequential");
    expect_same("traced construction", traced.text, alone.text);
  }
  {
    // Dump file numbers advance across both lowerings in order.
    const fs::path dir = fs::temp_directory_path() / "stanli_wa_parallel_dump";
    fs::remove_all(dir);
    EnvGuard dump("STANLI_DUMP_PASSES", dir.string().c_str());
    const Run dumped = construct_parallel(*model);
    expect(!dumped.parallel, "pass dumping stays sequential");
    expect_same("dumping construction", dumped.text, alone.text);
    fs::remove_all(dir);
  }
}

// The kernel can take a moment to retire a thread that has been joined, so
// the count is polled down to the baseline; a thread that was never joined
// never gets there.
bool threads_settle_to(int baseline) {
  for (int i = 0; i < 200; ++i) {
    if (live_threads() <= baseline) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

void test_threads_joined(const std::vector<Fixture>& fixtures) {
  const int before = live_threads();
  if (before < 0) return;
  for (size_t i = 0; i < fixtures.size(); i += kStride) {
    const Fixture& f = fixtures[i];
    construct_parallel(f, 1, false);
    if (!threads_settle_to(before)) {
      expect(false, "construction left a thread behind: " + f.name);
      return;
    }
  }
}

bool same_thrown_both_ways(const Fixture& f, const std::string& what,
                           const std::string& want_message) {
  const Run par = construct_parallel(f);
  const Run seq = construct_sequential(f);
  expect_same(what + ": parallel vs sequential", par.text, seq.text);
  const bool ok = par.text.find(want_message) != std::string::npos;
  expect(ok, what + ": throws \"" + want_message + "\", got " + par.text);
  return ok;
}

void test_errors(const std::vector<Fixture>& fixtures) {
  const Fixture* lp = find(fixtures, "wapar_lp_error");
  const Fixture* wa = find(fixtures, "wapar_wa_fallback");
  const Fixture* both = find(fixtures, "wapar_both_error");
  if (!lp || !wa || !both) return;
  const int before = live_threads();
  const std::string lp_message = "append_row column mismatch";
  const std::string no_path = "STANLI_NO_INTERPRETER is set";

  // log_prob fails; write_array lowers cleanly beside it.
  same_thrown_both_ways(*lp, "log_prob error", lp_message);
  // write_array falls back to the interpreter: a note on the model, and under
  // STANLI_NO_INTERPRETER the construction error.
  {
    const Run par = construct_parallel(*wa);
    const Run seq = construct_sequential(*wa);
    expect_same("write_array fallback: parallel vs sequential", par.text,
                seq.text);
    expect(
        par.text.find("fallbacks [generated quantities") != std::string::npos,
        "write_array fallback is noted");
    expect(par.parallel == kThreaded,
           "the fallback model lowered on the second thread");
    EnvGuard strict("STANLI_NO_INTERPRETER", "1");
    same_thrown_both_ways(*wa, "STANLI_NO_INTERPRETER", no_path);
  }
  // Both sides have something to say: the log_prob error is the one a
  // sequential construction reaches first, with or without the strict switch.
  same_thrown_both_ways(*both, "both sides fail", lp_message);
  {
    EnvGuard strict("STANLI_NO_INTERPRETER", "1");
    same_thrown_both_ways(*both, "both sides fail under STANLI_NO_INTERPRETER",
                          lp_message);
  }
  if (before >= 0)
    expect(threads_settle_to(before),
           "failed constructions joined their thread");
}

// The worker beside the lowering, with failures the models above cannot
// produce: any exception type on the second thread.
void test_worker() {
  using stanli::lower_detail::LowerWorker;
  const int before = live_threads();
  {
    LowerWorker w;
    bool ran = false;
    expect(w.start([&] { ran = true; }), "worker starts");
    w.rethrow_if_failed();
    expect(ran, "worker body ran before rethrow_if_failed returned");
  }
  {
    LowerWorker w;
    w.start([] { throw std::bad_alloc(); });
    bool got = false;
    try {
      w.rethrow_if_failed();
    } catch (const std::bad_alloc&) {
      got = true;
    }
    expect(got, "an exception that is not a logic_error comes back unchanged");
  }
  {
    LowerWorker w;
    w.start([] { throw 7; });
    bool got = false;
    try {
      w.rethrow_if_failed();
    } catch (int v) {
      got = v == 7;
    }
    expect(got, "a non-std exception comes back unchanged");
  }
  {
    // The creating thread throws first: the destructor still waits for the
    // body, which keeps using state that outlives the throw.
    std::atomic<bool> finished{false};
    bool caught = false;
    try {
      LowerWorker w;
      w.start([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        finished = true;
        throw std::runtime_error("worker error that must not escape");
      });
      throw std::logic_error("primary error");
    } catch (const std::logic_error& e) {
      caught = std::string(e.what()) == "primary error";
    }
    expect(caught, "the creating thread's error is the one that surfaces");
    expect(finished, "the worker finished before the stack unwound past it");
  }
  {
    // Deep recursion on the worker, past what a small default stack allows.
    std::function<int(int)> down = [&](int n) {
      volatile char pad[512];
      pad[0] = static_cast<char>(n);
      return n == 0 ? 0 : 1 + down(n - 1) + pad[0] * 0;
    };
    int depth = 0;
    LowerWorker w;
    w.start([&] { depth = down(8000); });
    w.rethrow_if_failed();
    expect(depth == 8000, "worker stack holds deep lowering recursion");
  }
  if (before >= 0) expect(threads_settle_to(before), "workers joined");
}

// The BridgeStan facade builds its model through the same compile_model, then
// derives the column names, the interpreter probe and the warning from it.
struct BsRun {
  std::string text;
  std::vector<std::string> messages;
};

std::mutex g_messages_mutex;
std::vector<std::string>* g_messages = nullptr;

BsRun bs_construct(const Fixture& f) {
  BsRun run;
  const std::string json = "tests/fixtures/" + f.name + ".json";
  char* err = nullptr;
  bs_model* m = bs_model_from_mir(
      f.mir.c_str(), fs::exists(json) ? json.c_str() : nullptr, 1234, &err);
  if (m == nullptr) {
    run.text = std::string("bs error: ") + (err ? err : "");
    bs_free_error_msg(err);
    return run;
  }
  std::ostringstream o;
  o << bs_name(m) << "|" << bs_model_info(m) << "\n";
  for (const bool tp : {false, true})
    for (const bool gq : {false, true})
      o << tp << gq << " " << bs_param_num(m, tp, gq) << " "
        << bs_param_names(m, tp, gq) << "\n";
  o << "unc " << bs_param_unc_num(m) << " " << bs_param_unc_names(m) << "\n";
  const int n = bs_param_unc_num(m);
  std::vector<double> q(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) q[static_cast<size_t>(i)] = ld::point(i);
  bs_rng* rng = bs_rng_construct(1234, &err);
  std::vector<double> out(static_cast<size_t>(bs_param_num(m, true, true)));
  if (bs_param_constrain(m, true, true, q.data(), out.data(), rng, &err) == 0) {
    o << "constrain";
    for (double v : out) ld::put_bits(o, v);
    o << "\n";
  } else {
    o << "constrain error " << (err ? err : "") << "\n";
    bs_free_error_msg(err);
  }
  bs_rng_destruct(rng);
  double lp = 0;
  std::vector<double> grad(static_cast<size_t>(n));
  if (bs_log_density_gradient(m, true, true, q.data(), &lp, grad.data(),
                              &err) == 0) {
    o << "lp";
    ld::put_bits(o, lp);
    for (double g : grad) ld::put_bits(o, g);
    o << "\n";
  } else {
    bs_free_error_msg(err);
  }
  bs_model_destruct(m);
  run.text = o.str();
  return run;
}

const char* const kFacadeModels[] = {"conj", "wapar_wa_fallback",
                                     "gq_bounded_block", "alg_gauss",
                                     "wapar_lp_error"};

std::string bs_account(const Fixture& f, std::vector<std::string>* messages) {
  std::vector<std::string> mine;
  {
    std::lock_guard<std::mutex> lock(g_messages_mutex);
    g_messages = &mine;
  }
  const BsRun run = bs_construct(f);
  {
    std::lock_guard<std::mutex> lock(g_messages_mutex);
    g_messages = nullptr;
  }
  if (messages) *messages = mine;
  return run.text;
}

void test_bridgestan_facade(const std::vector<Fixture>& fixtures) {
  stanli::set_message_sink([](const char* text, std::size_t len) {
    std::lock_guard<std::mutex> lock(g_messages_mutex);
    if (g_messages) g_messages->emplace_back(text, len);
  });
  struct Reference {
    const Fixture* f;
    std::string text;
    std::vector<std::string> messages;
  };
  std::vector<Reference> refs;
  {
    EnvGuard seq("STANLI_PARALLEL_WA", "0");
    for (const char* name : kFacadeModels) {
      const Fixture* f = find(fixtures, name);
      if (!f) continue;
      Reference r{f, {}, {}};
      r.text = bs_account(*f, &r.messages);
      refs.push_back(std::move(r));
    }
  }
  bool saw_warning = false;
  for (const Reference& r : refs) saw_warning |= !r.messages.empty();
  expect(saw_warning, "the facade announces an interpreter fallback");
  EnvGuard force("STANLI_PARALLEL_WA", "1");
  for (const Reference& r : refs) {
    std::vector<std::string> messages;
    const std::string text = bs_account(*r.f, &messages);
    expect_same("facade " + r.f->name + " parallel vs sequential", text,
                r.text);
    expect(messages == r.messages,
           "facade " + r.f->name + " messages parallel vs sequential");
  }
  if (stanli::thread_safe_build()) {
    // Many facades at once, the same model and different ones. Messages go to
    // a process-wide sink, so the per-thread capture above is replaced by a
    // count: every construction announces exactly what the reference did.
    std::atomic<int> bad{0};
    std::atomic<size_t> announced{0}, expected{0};
    std::vector<std::string> all;
    stanli::set_message_sink([&](const char* text, std::size_t len) {
      std::lock_guard<std::mutex> lock(g_messages_mutex);
      all.emplace_back(text, len);
    });
    constexpr int kThreads = 6;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
      threads.emplace_back([&, t] {
        for (int round = 0; round < 4; ++round)
          for (size_t i = 0; i < refs.size(); ++i) {
            const Reference& r =
                refs[(i + static_cast<size_t>(t)) % refs.size()];
            BsRun run = bs_construct(*r.f);
            if (run.text != r.text) ++bad;
            expected += r.messages.size();
          }
      });
    for (auto& t : threads) t.join();
    announced = all.size();
    expect(bad == 0, "concurrent facade constructions match sequential ones");
    expect(announced == expected,
           "concurrent facade constructions announce what sequential ones do");
    std::set<std::string> distinct(all.begin(), all.end());
    std::set<std::string> want;
    for (const Reference& r : refs)
      want.insert(r.messages.begin(), r.messages.end());
    expect(distinct == want,
           "concurrent facade warnings are the sequential ones");
  }
  stanli::set_message_sink(nullptr);
}

void test_concurrent_constructions(const std::vector<Fixture>& fixtures) {
  if (!stanli::thread_safe_build()) return;
  std::vector<const Fixture*> with_gq;
  std::map<std::string, std::string> reference;
  for (const Fixture& f : fixtures) {
    const Run r = construct_sequential(f);
    // Reading wapar_deep's MIR needs more stack than a std::thread has.
    if (!r.has_write_array || f.name == "wapar_deep") continue;
    with_gq.push_back(&f);
    reference[f.name] = r.text;
  }
  expect(!with_gq.empty(), "concurrent test has models");
  if (with_gq.empty()) return;

  EnvGuard force("STANLI_PARALLEL_WA", "1");
  constexpr int kThreads = 6;
  std::mutex out_mutex;
  std::atomic<int> mismatches{0}, constructed{0};
  const auto worker = [&](int id, bool same_model) {
    for (size_t i = 0; i < with_gq.size(); i += 2 * kStride) {
      const Fixture& f =
          same_model ? *with_gq[0] : *with_gq[(i + id) % with_gq.size()];
      const Run r = construct(f);
      ++constructed;
      if (r.text != reference[f.name]) {
        ++mismatches;
        std::lock_guard<std::mutex> lock(out_mutex);
        std::printf("FAIL concurrent construction of %s differs\n",
                    f.name.c_str());
      }
    }
  };
  for (const bool same_model : {true, false}) {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
      threads.emplace_back(worker, t, same_model);
    for (auto& t : threads) t.join();
  }
  expect(mismatches == 0, "concurrent constructions match sequential ones");
  std::printf("concurrent: %d constructions on %d threads\n",
              constructed.load(), kThreads);
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const std::vector<Fixture> fixtures = load_fixtures();
  expect(fixtures.size() > 100, "fixtures found");
  const auto start = std::chrono::steady_clock::now();
  const auto phase = [&](const char* name) {
    std::printf(
        "phase %s (%.0f s)\n", name,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count());
  };
  phase("corpus equivalence");
  test_corpus_equivalence(fixtures);
  phase("default choice");
  test_default_choice(fixtures);
  phase("deep nesting");
  test_deep_nesting(fixtures);
  phase("data payload");
  test_data_payload();
  phase("gates");
  test_gates(fixtures);
  phase("threads joined");
  test_threads_joined(fixtures);
  phase("errors");
  test_errors(fixtures);
  phase("worker");
  test_worker();
  phase("bridgestan facade");
  test_bridgestan_facade(fixtures);
  phase("concurrent constructions");
  test_concurrent_constructions(fixtures);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("OK\n");
  return 0;
}
