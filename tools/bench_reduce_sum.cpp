// Fixed-partition feasibility probe and --native integration benchmark.
// Probe children are separately bound callback models; native mode times
// ordinary whole-model compilation with retained reduction operations.
#include "reduce_sum_imports.hpp"
#include <stanli/compile.hpp>
#include <stanli/nuts.hpp>
#include <stanli/reduce_sum.hpp>
#include <stan/math/prim/fun/digamma.hpp>
#include <stan/math/rev/core/chainablestack.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/resource.h>
#endif

// Diagnostics only: ordinary C++ new/new[], not aligned allocation, malloc,
// or Stan arena cursor movement. Disabled during the timing samples.
namespace allocations {
std::atomic<bool> enabled{false};
std::atomic<size_t> calls{0}, bytes{0};
}  // namespace allocations
void* operator new(std::size_t n) {
  if (void* p = std::malloc(n ? n : 1)) {
    if (allocations::enabled.load(std::memory_order_relaxed)) {
      allocations::calls.fetch_add(1, std::memory_order_relaxed);
      allocations::bytes.fetch_add(n, std::memory_order_relaxed);
    }
    return p;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using Clock = std::chrono::steady_clock;
double ns(Clock::time_point start) {
  return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}
struct Config {
  int n = 10000, p = 3, kind = 1, active = 0, chunks = 4, threads = 4;
  int samples = 10, iterations = 0;
  bool check_only = false, profile = false, imports = false;
  bool worker_publish = true, native = false;
  std::string mir = "tests/fixtures/reduce_sum_parallel_probe.tmir.sexp";
};
void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
std::string slurp(const std::string& path) {
  std::ifstream f(path);
  require(bool(f), "cannot read " + path);
  std::ostringstream out;
  out << f.rdbuf();
  return out.str();
}
double peak_rss_bytes() {
#ifndef _WIN32
  rusage r{};
  if (getrusage(RUSAGE_SELF, &r)) return -1;
#ifdef __APPLE__
  return r.ru_maxrss;
#else
  return r.ru_maxrss * 1024.0;
#endif
#else
  return -1;
#endif
}

// Workers persist; the calling thread also executes jobs. No task allocation
// per dispatch. All jobs finish before exceptions are inspected in chunk order.
class Team {
 public:
  Team(int threads, size_t jobs, std::function<void(size_t)> work)
      : jobs_(jobs), work_(std::move(work)), errors_(jobs) {
    require(threads == 1 || stanli::thread_safe_build(),
            "TLS-safe build required");
    try {
      for (int i = 1; i < threads; ++i)
        workers_.emplace_back([this] { worker(); });
      std::unique_lock<std::mutex> lock(mu_);
      done_.wait(lock, [&] { return ready_ == workers_.size(); });
      if (startup_error_) std::rethrow_exception(startup_error_);
    } catch (...) {
      stop();
      throw;
    }
  }
  ~Team() { stop(); }
  Team(const Team&) = delete;
  Team& operator=(const Team&) = delete;
  void run() {
    std::fill(errors_.begin(), errors_.end(), nullptr);
    {
      std::lock_guard<std::mutex> lock(mu_);
      next_.store(0, std::memory_order_relaxed);
      remaining_ = workers_.size();
      ++generation_;
    }
    wake_.notify_all();
    consume();
    {
      std::unique_lock<std::mutex> lock(mu_);
      done_.wait(lock, [&] { return remaining_ == 0; });
    }
    for (const auto& error : errors_)
      if (error) std::rethrow_exception(error);
  }

 private:
  void consume() {
    for (size_t i = next_.fetch_add(1, std::memory_order_relaxed); i < jobs_;
         i = next_.fetch_add(1, std::memory_order_relaxed)) {
      try {
        work_(i);
      } catch (...) {
        errors_[i] = std::current_exception();
      }
    }
  }
  void worker() {
    // No Stan vars survive a job; each thread owns its nested-tape stack.
    std::unique_ptr<stan::math::ChainableStack> tape;
    try {
      tape = std::make_unique<stan::math::ChainableStack>();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mu_);
      if (!startup_error_) startup_error_ = std::current_exception();
      ++ready_;
      done_.notify_all();
      return;
    }
    std::unique_lock<std::mutex> lock(mu_);
    ++ready_;
    done_.notify_all();
    size_t seen = 0;
    for (;;) {
      wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
      if (stopping_) return;
      seen = generation_;
      lock.unlock();
      consume();
      lock.lock();
      --remaining_;
      done_.notify_all();
    }
  }
  void stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_)
      if (worker.joinable()) worker.join();
  }
  size_t jobs_, ready_ = 0, remaining_ = 0, generation_ = 0;
  std::function<void(size_t)> work_;
  std::vector<std::exception_ptr> errors_;
  std::exception_ptr startup_error_;
  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable wake_, done_;
  std::atomic<size_t> next_{0};
  bool stopping_ = false;
};

struct Result {
  double lp = 0;
  std::vector<double> grad;
  explicit Result(size_t n) : grad(n) {}
};

void check_team(int threads) {
  std::vector<int> visited(4, 0);
  bool fail = true;
  Team team(threads, visited.size(), [&](size_t i) {
    ++visited[i];
    if (fail && (i == 1 || i == 3))
      throw std::domain_error("chunk " + std::to_string(i));
  });
  std::string message;
  try {
    team.run();
  } catch (const std::domain_error& error) {
    message = error.what();
  }
  require(message == "chunk 1", "exception selection depends on scheduling");
  require(
      std::all_of(visited.begin(), visited.end(), [](int n) { return n == 1; }),
      "exception returned before jobs drained");
  fail = false;
  team.run();
  require(
      std::all_of(visited.begin(), visited.end(), [](int n) { return n == 2; }),
      "team did not recover after an exception");
}
struct Child {
  std::unique_ptr<stanli::Executor> ex;
  Result result;
  double prep_ns = 0;
  size_t value_bytes = 0, adjoint_bytes = 0, scratch_bytes = 0;
  size_t ops = 0, slots = 0;
  bool compacted = false;
  std::vector<reduce_sum_probe::Import> imports, scatter;
  Child(const std::string& mir, const stanli::DataMap& data, size_t nparams,
        bool compact = false)
      : result(nparams) {
    const auto begin = Clock::now();
    auto cm = stanli::compile_model(mir, data);
    int64_t original_params = 0;
    for (const auto& slot : cm.graph.slots) {
      if (!slot.is_param) continue;
      require(slot.len >= 0 &&
                  slot.len <= static_cast<int64_t>(nparams) - original_params,
              "original child parameter layout exceeds parent");
      original_params += slot.len;
    }
    require(original_params == static_cast<int64_t>(nparams),
            "original child parameter layout differs from parent");
    imports.push_back({0, 0, static_cast<int64_t>(nparams)});
    if (compact)
      compacted =
          reduce_sum_probe::compact_imports(cm.graph, cm.fills, imports);
    scatter = imports;
    for (const auto& slot : cm.graph.slots) value_bytes += 8 * slot.len;
    for (const auto& op : cm.graph.ops) {
      const auto& kernel = stanli::kernel(op.opcode);
      if (kernel.scratch_size)
        scratch_bytes += 8 * kernel.scratch_size(op, cm.graph.slots.data());
    }
    ops = cm.graph.ops.size();
    slots = cm.graph.slots.size();
    ex = std::make_unique<stanli::Executor>(std::move(cm.graph));
    cm.bind(*ex);
    int64_t imported = 0;
    for (const auto& in : imports) imported += in.len;
    require(ex->n_params() == imported, "parameter layout mismatch");
    result.grad = std::vector<double>(imported);
    adjoint_bytes = 8 * ex->adjoint_storage_size();
    prep_ns = ns(begin);
  }
  void pack(const std::vector<double>& q) {
    for (const auto& in : imports)
      std::copy_n(q.data() + in.parent, in.len, ex->params_data() + in.local);
  }
  void evaluate() {
    double empty = 0;
    result.lp = ex->gradient(result.grad.empty() ? &empty : result.grad.data());
  }
  void publish(Result& merged, bool exclusive) const {
    for (const auto& in : scatter)
      if (in.exclusive == exclusive)
        for (int64_t j = 0; j < in.len; ++j)
          merged.grad[in.parent + j] += result.grad[in.local + j];
  }
};

struct Probe {
  Config cfg;
  std::vector<double> x, y, q;
  std::unique_ptr<Child> whole;
  std::vector<std::unique_ptr<Child>> children, compact;
  std::vector<std::unique_ptr<Child>>* selected = nullptr;
  std::unique_ptr<Team> team;
  Result merged;
  double team_ns = 0, pack_ns = 0, execute_ns = 0, merge_ns = 0;
  explicit Probe(const Config& c, const std::string& mir)
      : cfg(c),
        x(c.n),
        y(c.n),
        q(c.p + (c.active ? c.n : 0)),
        merged(q.size()) {
    for (int i = 0; i < c.n; ++i) {
      x[i] = 0.2 * std::sin(i * 0.17);
      y[i] = 0.3 * std::cos(i * 0.11) - 0.1;
    }
    stanli::DataMap data;
    data.set_int("N", c.n);
    data.set_int("P", c.p);
    data.set_int("kind", c.kind);
    data.set_int("active_slice", c.active);
    data.set_real_array("x", x);
    data.set_real_array("y", y);
    data.set_int("chunk", 0);
    data.set_int("start", 1);
    data.set_int("end", c.n);
    whole = std::make_unique<Child>(mir, data, q.size());
    data.set_int("chunk", 1);
    for (int k = 0; k < c.chunks; ++k) {
      data.set_int("start", int64_t(c.n) * k / c.chunks + 1);
      data.set_int("end", int64_t(c.n) * (k + 1) / c.chunks);
      children.push_back(std::make_unique<Child>(mir, data, q.size()));
      if (cfg.imports)
        compact.push_back(std::make_unique<Child>(mir, data, q.size(), true));
    }
    if (cfg.imports) {
      std::vector<std::vector<reduce_sum_probe::Import>> maps;
      for (const auto& child : compact) maps.push_back(child->imports);
      auto scatter = reduce_sum_probe::scatter_plan(maps, q.size());
      for (size_t k = 0; k < compact.size(); ++k) {
        compact[k]->scatter = std::move(scatter[k]);
        if (!cfg.worker_publish)
          for (auto& span : compact[k]->scatter) span.exclusive = false;
      }
    }
    const auto begin = Clock::now();
    team = std::make_unique<Team>(c.threads, children.size(), [this](size_t k) {
      auto& child = (*selected)[k];
      child->evaluate();
      child->publish(merged, true);
    });
    team_ns = ns(begin);
    point(0);
  }
  void point(int index) {
    for (size_t i = 0; i < q.size(); ++i)
      q[i] = 0.1 + 0.025 * int(i % 7) - 0.04 * index;
  }
  const Result& run(int mode, bool phases = false) {
    auto begin = phases ? Clock::now() : Clock::time_point{};
    selected = mode >= 3 ? &compact : &children;
    if (mode == 0) {
      whole->pack(q);
    } else {
      for (auto& child : *selected) child->pack(q);
      // Exclusive worker publications need a clean destination before dispatch.
      // This clear is included in end-to-end and packing-phase measurements.
      merged.lp = 0;
      std::fill(merged.grad.begin(), merged.grad.end(), 0.0);
    }
    if (phases) {
      pack_ns = ns(begin);
      begin = Clock::now();
    }
    if (mode == 0)
      whole->evaluate();
    else if (mode == 1 || mode == 3)
      for (auto& child : *selected) {
        child->evaluate();
        child->publish(merged, true);
      }
    else
      team->run();
    if (phases) {
      execute_ns = ns(begin);
      begin = Clock::now();
    }
    if (mode == 0) return whole->result;
    // Only shared spans need canonical chunk-order accumulation after joining.
    for (const auto& child : *selected) {
      merged.lp += child->result.lp;
      child->publish(merged, false);
    }
    if (phases) merge_ns = ns(begin);
    return merged;
  }
  Result oracle() const {
    Result out(q.size());
    const double sigma = std::exp(q[1]), nu = 2 + sigma;
    const double mean =
        2 * q[0] + 0.001 * std::accumulate(q.begin(), q.begin() + cfg.p, 0.0);
    const double pi = std::acos(-1.0);
    for (int i = 0; i < cfg.n; ++i) {
      const double residual = (cfg.active ? q[cfg.p + i] : y[i]) - x[i] - mean;
      const double z2 = residual * residual / (sigma * sigma);
      double dm, ds;
      if (cfg.kind == 0) {
        out.lp += -0.5 * std::log(2 * pi) - std::log(sigma) - 0.5 * z2;
        dm = residual / (sigma * sigma);
        ds = z2 - 1;
      } else {
        out.lp += std::lgamma(0.5 * (nu + 1)) - std::lgamma(0.5 * nu) -
                  0.5 * std::log(nu * pi) - std::log(sigma) -
                  0.5 * (nu + 1) * std::log1p(z2 / nu);
        dm = (nu + 1) * residual / (sigma * sigma * (nu + z2));
        const double dn =
            0.5 * (stan::math::digamma(0.5 * (nu + 1)) -
                   stan::math::digamma(0.5 * nu) - 1 / nu -
                   std::log1p(z2 / nu) + (nu + 1) * z2 / (nu * (nu + z2)));
        ds = -1 + (nu + 1) * z2 / (nu + z2) + sigma * dn;
      }
      for (int j = 0; j < cfg.p; ++j) out.grad[j] += 0.001 * dm;
      out.grad[0] += 2 * dm;
      out.grad[1] += ds;
      if (cfg.active) out.grad[cfg.p + i] = -dm;
    }
    return out;
  }
};

struct Error {
  double absolute = 0, max_scaled = 0;
};
void compare(const Result& got, const Result& want, bool exact, Error& error) {
  require(got.grad.size() == want.grad.size(), "gradient size");
  const auto one = [&](double a, double b) {
    require(std::isfinite(a) && std::isfinite(b),
            "unexpected nonfinite result");
    const double delta = std::abs(a - b);
    error.absolute = std::max(error.absolute, delta);
    error.max_scaled =
        std::max(error.max_scaled, delta / (1e-10 + 1e-10 * std::abs(b)));
    if (exact)
      require(std::memcmp(&a, &b, sizeof(double)) == 0,
              "chunk results differ bitwise");
    else
      require(delta <= 1e-10 + 1e-10 * std::abs(b),
              "analytic/whole-slice tolerance exceeded");
  };
  one(got.lp, want.lp);
  for (size_t j = 0; j < got.grad.size(); ++j) one(got.grad[j], want.grad[j]);
}
// An unrelated arithmetic graph exercises the planner independently of Stan
// source names, density callbacks, and the parallel fixture's parameter layout.
void check_import_planner() {
  using namespace stanli;
  using reduce_sum_probe::Fills;
  using reduce_sum_probe::Import;
  Graph original;
  const int unused = original.add_slot(5, true);
  const int data = original.add_slot(9, false);
  const int p = original.add_slot(10, true);
  original.add_slot(23, false);  // Dead storage left by an earlier rewrite.
  const int a = original.add_slot(3, false), b = original.add_slot(3, false);
  const int d = original.add_slot(3, false), ab = original.add_slot(3, false);
  const int v = original.add_slot(3, false), sum = original.add_slot(1, false);
  original.add_op(OP_SLICE, {p}, a, {2});
  original.add_op(OP_SLICE, {p}, b,
                  {3});  // Overlapping alias, same source slot.
  original.add_op(OP_SLICE, {data}, d, {4});
  original.add_op(OP_ADD, {a, b}, ab);
  original.add_op(OP_MUL, {ab, d}, v);
  original.add_op(OP_SUM_VEC, {v}, sum);
  original.result_slot = sum;
  Fills fills{{data, {1, 2, 3, 4, 5, 6, 7, 8, 9}}};
  for (bool full_consumer : {false, true}) {
    Graph source(original);
    if (full_consumer) {
      const int whole = source.add_slot(1, false);
      const int total = source.add_slot(1, false);
      source.add_op(OP_SUM_VEC, {p}, whole);
      source.add_op(OP_ADD, {sum, whole}, total);
      source.result_slot = total;
    }
    Graph compact(source);
    Fills compact_fills(fills);
    std::vector<Import> imports;
    require(reduce_sum_probe::compact_imports(compact, compact_fills, imports),
            "arithmetic import proof refused");
    require(compact.slots[unused].len == 0 && compact.slots[data].len == 3 &&
                compact.slots[p].len == (full_consumer ? 10 : 4) &&
                imports.size() == 1 &&
                imports[0].parent == (full_consumer ? 5 : 7),
            "input hull/alias/full-consumer plan");
    Executor baseline(source), candidate(compact);
    for (const auto& fill : fills)
      std::copy(fill.second.begin(), fill.second.end(),
                baseline.value_ptr(fill.first));
    for (const auto& fill : compact_fills)
      std::copy(fill.second.begin(), fill.second.end(),
                candidate.value_ptr(fill.first));
    for (int point = 0; point < 4; ++point) {
      for (int64_t i = 0; i < baseline.n_params(); ++i)
        baseline.params_data()[i] = point == 3 ? -0.0 : 0.13 * (i - point);
      for (const auto& in : imports)
        std::copy_n(baseline.params_data() + in.parent, in.len,
                    candidate.params_data() + in.local);
      Result want(baseline.n_params()), local(candidate.n_params()),
          got(baseline.n_params());
      want.lp = baseline.gradient(want.grad.data());
      local.lp = candidate.gradient(local.grad.data());
      got.lp = local.lp;
      for (const auto& in : imports)
        for (int64_t j = 0; j < in.len; ++j)
          got.grad[in.parent + j] += local.grad[in.local + j];
      Error error;
      compare(got, want, true, error);
    }
  }
  // Scalar and empty fixed reads also compact; a fully dead parameter block
  // leaves a zero-length gradient, while the executor still computes the sum.
  for (bool empty : {false, true}) {
    Graph source;
    const int parameter = source.add_slot(10, true);
    const int read = source.add_slot(empty ? 0 : 1, false);
    const int sum = source.add_slot(1, false);
    source.add_op(empty ? OP_SLICE : OP_INDEX, {parameter}, read,
                  {empty ? 10 : 6});
    source.add_op(OP_SUM_VEC, {read}, sum);
    source.result_slot = sum;
    Graph compact(source);
    Fills no_fills;
    std::vector<Import> imports;
    require(reduce_sum_probe::compact_imports(compact, no_fills, imports) &&
                compact.slots[parameter].len == (empty ? 0 : 1),
            "scalar/empty import plan");
    Executor baseline(source), candidate(compact);
    std::fill_n(baseline.params_data(), 10, 0.25);
    if (!empty) candidate.params_data()[0] = 0.25;
    Result want(10), got(10), local(candidate.n_params());
    want.lp = baseline.gradient(want.grad.data());
    double dummy = 0;
    got.lp = candidate.gradient(empty ? &dummy : local.grad.data());
    if (!empty) got.grad[imports[0].parent] += local.grad[0];
    Error error;
    compare(got, want, true, error);
  }
  // Unknown geometry/payloads and malformed fixed reads refuse before mutation.
  for (int kind = 0; kind < 7; ++kind) {
    Graph graph(original);
    Fills unchanged(fills);
    std::vector<Import> imports{{11, 12, 13, true}};
    int payload = 0;
    if (kind == 0) graph.ops.back().udata = &payload;
    if (kind == 1) graph.ops.front().dyn_extent_in = 1;
    if (kind == 2) graph.ops.front().opcode = OP_DYNAMIC_SLICE;
    if (kind == 3) graph.ops.front().opcode = OP_SET_INDEX_INPLACE;
    if (kind == 4) graph.idata_pool.front()[0] = 99;
    if (kind == 5) graph.ops.front().opcode = OP_LOOP;
    if (kind == 6)
      graph.slots[unused].len = std::numeric_limits<int64_t>::max();
    const auto* before = graph.ops.front().idata;
    const int offset = before[0];
    require(!reduce_sum_probe::compact_imports(graph, unchanged, imports),
            "unsafe import plan accepted");
    require(graph.slots[p].len == 10 && graph.slots[data].len == 9 &&
                graph.ops.front().idata == before && before[0] == offset &&
                unchanged == fills && imports.size() == 1 &&
                imports[0].parent == 11 && imports[0].local == 12 &&
                imports[0].len == 13 && imports[0].exclusive,
            "import refusal changed caller state");
  }
  // Shared and exclusive portions of one import must be split correctly.
  const auto scatter =
      reduce_sum_probe::scatter_plan({{{2, 0, 4}}, {{4, 0, 4}}}, 10);
  require(scatter[0].size() == 2 && scatter[1].size() == 2 &&
              scatter[0][0].exclusive && !scatter[0][1].exclusive &&
              !scatter[1][0].exclusive && scatter[1][1].exclusive &&
              scatter[0][0].len == 2 && scatter[1][1].parent == 6,
          "partial-overlap scatter ownership");
}

// Overlap is not merely a map property: run two partially overlapping
// callbacks on the actual team and compare the published gradient with serial
// execution over full parent inputs. Exercises shared and exclusive writes in
// the same imported slot, including more tasks than workers when threads == 1.
void check_scatter_publication(int threads) {
  using namespace stanli;
  using reduce_sum_probe::Import;
  std::vector<std::unique_ptr<Executor>> full, compact;
  std::vector<std::vector<Import>> imports;
  std::vector<Result> results;
  for (int offset : {2, 4}) {
    Graph source;
    const int p = source.add_slot(10, true), slice = source.add_slot(4, false);
    const int sum = source.add_slot(1, false);
    source.add_op(OP_SLICE, {p}, slice, {offset});
    source.add_op(OP_SUM_VEC, {slice}, sum);
    source.result_slot = sum;
    full.push_back(std::make_unique<Executor>(source));
    reduce_sum_probe::Fills fills;
    imports.emplace_back();
    require(reduce_sum_probe::compact_imports(source, fills, imports.back()),
            "overlapping callback imports refused");
    compact.push_back(std::make_unique<Executor>(std::move(source)));
    results.emplace_back(compact.back()->n_params());
  }
  const auto scatter = reduce_sum_probe::scatter_plan(imports, 10);
  Result published(10);
  const auto publish = [&](size_t k, bool exclusive) {
    for (const auto& span : scatter[k])
      if (span.exclusive == exclusive)
        for (int64_t j = 0; j < span.len; ++j)
          published.grad[span.parent + j] += results[k].grad[span.local + j];
  };
  Team team(std::min(threads, 2), 2, [&](size_t k) {
    results[k].lp = compact[k]->gradient(results[k].grad.data());
    publish(k, true);
  });
  for (int point = 0; point < 3; ++point) {
    Result expected(10);
    for (size_t k = 0; k < full.size(); ++k) {
      for (int j = 0; j < 10; ++j)
        full[k]->params_data()[j] = 0.125 * (j - point);
      for (const auto& in : imports[k])
        std::copy_n(full[k]->params_data() + in.parent, in.len,
                    compact[k]->params_data() + in.local);
      Result part(10);
      expected.lp += full[k]->gradient(part.grad.data());
      for (int j = 0; j < 10; ++j) expected.grad[j] += part.grad[j];
    }
    for (int repeat = 0; repeat < 5; ++repeat) {
      published.lp = 0;
      std::fill(published.grad.begin(), published.grad.end(), 0.0);
      team.run();
      for (size_t k = 0; k < compact.size(); ++k) {
        published.lp += results[k].lp;
        publish(k, false);
      }
      Error error;
      compare(published, expected, true, error);
    }
  }
}

void array(const char* name, const std::vector<double>& xs) {
  std::printf(",\"%s\":[", name);
  for (size_t i = 0; i < xs.size(); ++i)
    std::printf("%s%.9g", i ? "," : "", xs[i]);
  std::printf("]");
}
void run_case(const Config& cfg, const std::string& mir) {
  Probe probe(cfg, mir);
  const auto first = Clock::now();
  probe.run(2);
  const double first_ns = ns(first);
  double compact_first_ns = 0;
  if (cfg.imports) {
    const auto begin = Clock::now();
    probe.run(4);
    compact_first_ns = ns(begin);
  }
  Error error;
  for (int point = 0; point < 3; ++point) {
    probe.point(point);
    const Result whole = probe.run(0), seq = probe.run(1);
    compare(seq, whole, false, error);
    compare(whole, probe.oracle(), false, error);
    if (cfg.imports) compare(probe.run(3), seq, true, error);
    for (int repeat = 0; repeat < 3; ++repeat) {
      compare(probe.run(2), seq, true, error);
      if (cfg.imports) compare(probe.run(4), seq, true, error);
    }
  }
  if (cfg.n) {
    probe.q[1] = -1000;  // sigma underflow => worker domain errors.
    bool rejected = false;
    try {
      probe.run(cfg.imports ? 4 : 2);
    } catch (const std::domain_error&) {
      rejected = true;
    }
    require(rejected, "expected worker domain error");
    probe.point(0);
    const Result seq = probe.run(1);
    compare(probe.run(cfg.imports ? 4 : 2), seq, true, error);
  }
  probe.point(0);
  // The parent/child graphs share immutable payloads, but cloned executor
  // contexts must refer to their own arenas and parameter values.
  {
    probe.run(1);
    stanli::Executor clone(*probe.children.front()->ex);
    Result cloned(probe.q.size());
    cloned.lp = clone.gradient(cloned.grad.data());
    compare(cloned, probe.children.front()->result, true, error);
    if (cfg.imports) {
      probe.run(3);
      stanli::Executor compact_clone(*probe.compact.front()->ex);
      Result compact_result(compact_clone.n_params());
      double empty = 0;
      compact_result.lp = compact_clone.gradient(
          compact_result.grad.empty() ? &empty : compact_result.grad.data());
      compare(compact_result, probe.compact.front()->result, true, error);
    }
  }
  const int modes = cfg.imports ? 5 : 3;
  for (int i = 0; i < 5; ++i)
    for (int mode = 0; mode < modes; ++mode) probe.run(mode);
  int iterations = cfg.iterations;
  if (!iterations) {
    const auto begin = Clock::now();
    for (int i = 0; i < 5; ++i) probe.run(0);
    iterations = std::max(2, std::min(10000, int(3e7 / (ns(begin) / 5))));
  }
  std::vector<double> timings[5];
  double sink = 0;
  if (!cfg.check_only)
    for (int sample = 0; sample < cfg.samples; ++sample) {
      // Rotate every mode; reverse the cycle on alternating samples.
      for (int j = 0; j < modes; ++j) {
        const int mode = (sample + (sample % 2 ? modes - 1 - j : j)) % modes;
        const auto begin = Clock::now();
        for (int i = 0; i < iterations; ++i) sink += probe.run(mode).lp;
        timings[mode].push_back(ns(begin) / iterations);
      }
    }
  std::vector<double> pack, execution, merge, compact_pack, compact_execute,
      compact_merge;
  for (int i = 0; i < 20; ++i) {
    probe.run(2, true);
    pack.push_back(probe.pack_ns);
    execution.push_back(probe.execute_ns);
    merge.push_back(probe.merge_ns);
    if (cfg.imports) {
      probe.run(4, true);
      compact_pack.push_back(probe.pack_ns);
      compact_execute.push_back(probe.execute_ns);
      compact_merge.push_back(probe.merge_ns);
    }
  }
  allocations::calls.store(0);
  allocations::bytes.store(0);
  allocations::enabled.store(true);
  probe.run(cfg.imports ? 4 : 2);
  allocations::enabled.store(false);
  const size_t new_calls = allocations::calls.load(),
               new_bytes = allocations::bytes.load();
  if (cfg.profile) {
    probe.whole->ex->set_profile(true);
    for (auto& child : probe.children) child->ex->set_profile(true);
    for (auto& child : probe.compact) child->ex->set_profile(true);
    for (int i = 0; i < 20; ++i) {
      probe.run(0);
      probe.run(2);
      if (cfg.imports) probe.run(4);
    }
    std::fprintf(stderr, "whole (20 evaluations)\n%s\n",
                 probe.whole->ex->profile_report().c_str());
    for (size_t i = 0; i < probe.children.size(); ++i)
      std::fprintf(stderr, "chunk %zu (20 evaluations)\n%s\n", i,
                   probe.children[i]->ex->profile_report().c_str());
    for (size_t i = 0; i < probe.compact.size(); ++i)
      std::fprintf(stderr, "compact chunk %zu (20 evaluations)\n%s\n", i,
                   probe.compact[i]->ex->profile_report().c_str());
  }
  size_t values = 0, adjoints = 0, scratch = 0, ops = 0, slots = 0;
  double prep_ns = 0;
  for (const auto& child : probe.children) {
    values += child->value_bytes;
    adjoints += child->adjoint_bytes;
    scratch += child->scratch_bytes;
    ops += child->ops;
    slots += child->slots;
    prep_ns += child->prep_ns;
  }
  std::printf(
      "{\"n\":%d,\"p\":%d,\"kind\":%d,\"active\":%d,\"chunks\":%d,\"threads\":%"
      "d,"
      "\"iterations\":%d,\"bitwise_pass\":true,\"max_abs_error\":%.17g,\"max_"
      "tolerance_fraction\":%.9g,"
      "\"whole_prep_ns\":%.9g,\"chunks_prep_ns\":%.9g,\"team_setup_ns\":%.9g,"
      "\"first_parallel_ns\":%.9g,"
      "\"whole_value_bytes\":%zu,\"whole_adjoint_bytes\":%zu,\"whole_scratch_"
      "bytes\":%zu,"
      "\"chunk_value_bytes\":%zu,\"chunk_adjoint_bytes\":%zu,\"chunk_scratch_"
      "bytes\":%zu,"
      "\"chunk_ops\":%zu,\"chunk_slots\":%zu,\"parameter_copy_bytes\":%zu,"
      "\"ordinary_new_calls\":%zu,\"ordinary_new_bytes\":%zu,\"peak_process_"
      "rss_bytes\":%.0f,\"sink\":%.9g",
      cfg.n, cfg.p, cfg.kind, cfg.active, cfg.chunks, cfg.threads, iterations,
      error.absolute, error.max_scaled, probe.whole->prep_ns, prep_ns,
      probe.team_ns, first_ns, probe.whole->value_bytes,
      probe.whole->adjoint_bytes, probe.whole->scratch_bytes, values, adjoints,
      scratch, ops, slots, probe.q.size() * sizeof(double) * cfg.chunks,
      new_calls, new_bytes, peak_rss_bytes(), sink);
  if (cfg.imports) {
    size_t cv = 0, ca = 0, cs = 0, copied = 0, shared = 0, exclusive = 0,
           accepted = 0;
    double prep = 0;
    for (const auto& child : probe.compact) {
      cv += child->value_bytes;
      ca += child->adjoint_bytes;
      cs += child->scratch_bytes;
      prep += child->prep_ns;
      accepted += child->compacted;
      copied += child->ex->n_params() * sizeof(double);
      for (const auto& in : child->scatter)
        (in.exclusive ? exclusive : shared) += in.len * sizeof(double);
    }
    std::printf(
        ",\"compact_value_bytes\":%zu,\"compact_adjoint_bytes\":%zu,"
        "\"compact_scratch_bytes\":%zu,\"compact_parameter_copy_bytes\":%zu,"
        "\"compact_shared_merge_bytes\":%zu,\"compact_exclusive_publish_"
        "bytes\":%zu,"
        "\"compact_accepted_chunks\":%zu,\"compact_prep_ns\":%.9g,\"compact_"
        "first_ns\":%.9g,\"worker_publish\":%s",
        cv, ca, cs, copied, shared, exclusive, accepted, prep, compact_first_ns,
        cfg.worker_publish ? "true" : "false");
    array("compact_serial_ns", timings[3]);
    array("compact_parallel_ns", timings[4]);
    array("compact_pack_phase_ns", compact_pack);
    array("compact_execution_dispatch_phase_ns", compact_execute);
    array("compact_merge_phase_ns", compact_merge);
  }
  array("whole_ns", timings[0]);
  array("serial_chunks_ns", timings[1]);
  array("parallel_chunks_ns", timings[2]);
  array("pack_phase_ns", pack);
  array("execution_dispatch_phase_ns", execution);
  array("merge_phase_ns", merge);
  std::printf("}\n");
}

// Matched ordinary-model path. The isolated probe supplies an independent
// analytic oracle; only native/default executors are timed here.
void run_native(const Config& cfg, const std::string& mir) {
  using namespace stanli;
  Probe oracle(cfg, mir);
  DataMap data;
  data.set_int("N", cfg.n);
  data.set_int("P", cfg.p);
  data.set_int("kind", cfg.kind);
  data.set_int("active_slice", cfg.active);
  data.set_real_array("x", oracle.x);
  data.set_real_array("y", oracle.y);
  data.set_int("chunk", 0);
  data.set_int("start", 1);
  data.set_int("end", cfg.n);
  CompileOptions options;
  options.reduce_sum_threads = cfg.chunks;
  const auto start = Clock::now();
  auto cm = compile_model(mir, data, 1, options);
  const double compile_ns = ns(start);
  size_t retained = 0, child_bytes = 0;
  for (const auto& op : cm.graph.ops)
    if (op.opcode == OP_REDUCE_SUM) {
      ++retained;
      for (const auto& child :
           static_cast<const ReduceSumSpec*>(op.udata)->children) {
        Executor count(child.graph);
        child_bytes +=
            8 * (count.mutable_value_size() + count.shared_data_size() +
                 count.adjoint_storage_size() + count.n_params());
        for (const auto& child_op : child.graph.ops) {
          const auto& kernel = stanli::kernel(child_op.opcode);
          if (kernel.scratch_size)
            child_bytes +=
                8 * kernel.scratch_size(child_op, child.graph.slots.data());
        }
      }
    }
  require(retained == size_t(cfg.n >= options.reduce_sum_min_elements &&
                             cfg.chunks > 1),
          "native eligibility unexpectedly changed");
  const auto bind = Clock::now();
  Executor native(std::move(cm.graph));
  cm.bind(native);
  ReduceExecutionContext team(cfg.threads);
  const double bind_ns = ns(bind);
  native.set_reduce_context(&team);
  Result actual(native.n_params());
  Error error;
  for (int point = 0; point < 4; ++point) {
    oracle.point(point);
    std::copy(oracle.q.begin(), oracle.q.end(), native.params_data());
    actual.lp = native.gradient(actual.grad.data());
    compare(actual, oracle.run(0), false, error);
    compare(actual, oracle.oracle(), false, error);
    const Result expected = actual;
    native.set_reduce_context(nullptr);
    actual.lp = native.gradient(actual.grad.data());
    compare(actual, expected, true, error);
    native.set_reduce_context(&team);
    require(native.forward_value_only() == expected.lp,
            "native value only differs");
  }
  const int iterations = cfg.iterations
                             ? cfg.iterations
                             : std::max(20, 2000000 / std::max(1, cfg.n));
  auto run = [&](int mode) {
    if (mode == 0)
      oracle.whole->evaluate();
    else {
      native.set_reduce_context(mode == 1 ? nullptr : &team);
      actual.lp = native.gradient(actual.grad.data());
    }
  };
  oracle.whole->pack(oracle.q);
  for (int i = 0; i < 20; ++i)
    for (int mode = 0; mode < 3; ++mode) run(mode);
  std::vector<double> times[3];
  if (!cfg.check_only)
    for (int sample = 0; sample < cfg.samples; ++sample)
      for (int step = 0; step < 3; ++step) {
        const int mode = (sample + step) % 3;
        const auto began = Clock::now();
        for (int i = 0; i < iterations; ++i) run(mode);
        times[mode].push_back(ns(began) / iterations);
      }
  allocations::calls = 0;
  allocations::bytes = 0;
  allocations::enabled = true;
  for (int i = 0; i < 20; ++i) run(2);
  allocations::enabled = false;
  std::printf(
      "{\"native\":true,\"n\":%d,\"p\":%d,\"active\":%d,\"kind\":%d,"
      "\"threads\":%d,\"chunks\":%d,\"retained\":%zu,\"child_bytes\":%zu,"
      "\"compile_ns\":%.9g,\"bind_team_ns\":%.9g,\"max_scaled_error\":%.9g,"
      "\"warm_new_calls\":%zu,\"iterations\":%d,\"peak_rss_bytes\":%.9g",
      cfg.n, cfg.p, cfg.active, cfg.kind, cfg.threads, cfg.chunks, retained,
      child_bytes, compile_ns, bind_ns, error.max_scaled,
      size_t(allocations::calls.load()), iterations, peak_rss_bytes());
  const char* names[] = {"whole_ns", "native_serial_ns", "native_parallel_ns"};
  for (int mode = 0; mode < 3; ++mode) {
    std::printf(",\"%s\":[", names[mode]);
    for (size_t i = 0; i < times[mode].size(); ++i)
      std::printf("%s%.9g", i ? "," : "", times[mode][i]);
    std::printf("]");
  }
  std::printf("}\n");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--check-only") {
        cfg.check_only = true;
        continue;
      }
      if (arg == "--owner-merge") {
        cfg.worker_publish = false;
        continue;
      }
      if (arg == "--native") {
        cfg.native = true;
        continue;
      }
      if (arg == "--imports") {
        cfg.imports = true;
        continue;
      }
      if (arg == "--profile") {
        cfg.profile = true;
        continue;
      }
      require(i + 1 < argc, "missing option value");
      const std::string value = argv[++i];
      if (arg == "--mir") {
        cfg.mir = value;
        continue;
      }
      size_t consumed = 0;
      const int v = std::stoi(value, &consumed);
      require(consumed == value.size(), "invalid integer");
      if (arg == "--n")
        cfg.n = v;
      else if (arg == "--p")
        cfg.p = v;
      else if (arg == "--kind")
        cfg.kind = v;
      else if (arg == "--active")
        cfg.active = v;
      else if (arg == "--chunks")
        cfg.chunks = v;
      else if (arg == "--threads")
        cfg.threads = v;
      else if (arg == "--samples")
        cfg.samples = v;
      else if (arg == "--iterations")
        cfg.iterations = v;
      else
        throw std::runtime_error("unknown option: " + arg);
    }
    require(cfg.n >= 0 && cfg.p >= 2 &&
                int64_t(cfg.n) + cfg.p <= std::numeric_limits<int>::max() &&
                cfg.chunks > 0 && cfg.chunks <= 1024 && cfg.threads > 0 &&
                cfg.threads <= cfg.chunks && cfg.samples > 0 &&
                cfg.iterations >= 0 && (cfg.kind == 0 || cfg.kind == 1) &&
                (cfg.active == 0 || cfg.active == 1),
            "invalid configuration");
    stan::math::ChainableStack owner_tape;
    check_team(cfg.threads);
    if (cfg.imports) {
      check_import_planner();
      check_scatter_publication(cfg.threads);
    }
    if (cfg.native)
      run_native(cfg, slurp(cfg.mir));
    else
      run_case(cfg, slurp(cfg.mir));
    return 0;
  } catch (const std::exception& e) {
    allocations::enabled.store(false);
    std::fprintf(stderr, "bench_reduce_sum: %s\n", e.what());
    return 1;
  }
}
