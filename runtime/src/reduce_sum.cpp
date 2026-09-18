#include <stanli/reduce_sum.hpp>
#include <stanli/nuts.hpp>
#include <stanli/optable.hpp>
#include <stan/math/rev/core/chainablestack.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace stanli {
bool values_only();
struct ReduceExecutionContext::Impl {
 public:
  explicit Impl(int threads) : threads_(threads) {
    if (threads < 1)
      throw std::invalid_argument("reduce_sum thread count must be positive");
    if (threads > 1 && !thread_safe_build())
      throw std::invalid_argument(
          "reduce_sum workers require a TLS-safe native build");
    try {
#if defined(STAN_THREADS) && !defined(__EMSCRIPTEN__)
      for (int i = 1; i < threads; ++i)
        workers_.emplace_back([this] { worker(); });
#endif
      std::unique_lock<std::mutex> lock(mu_);
      done_.wait(lock, [&] { return ready_ == workers_.size(); });
      if (startup_error_) std::rethrow_exception(startup_error_);
    } catch (...) {
      stop();
      throw;
    }
  }
  ~Impl() { stop(); }
  int threads() const { return threads_; }
  void run(size_t count, void* state, void (*work)(void*, size_t)) {
    if (busy_.test_and_set())
      throw std::logic_error("reduce_sum context already in use");
    struct Release {
      std::atomic_flag& busy;
      ~Release() { busy.clear(); }
    } release{busy_};
    errors_.resize(count);
    jobs_ = count;
    state_ = state;
    work_ = work;
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
        work_(state_, i);
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
  int threads_;
  std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
  void* state_ = nullptr;
  size_t jobs_ = 0, ready_ = 0, remaining_ = 0, generation_ = 0;
  void (*work_)(void*, size_t) = nullptr;
  std::vector<std::exception_ptr> errors_;
  std::exception_ptr startup_error_;
  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable wake_, done_;
  std::atomic<size_t> next_{0};
  bool stopping_ = false;
};

ReduceExecutionContext::ReduceExecutionContext(int threads)
    : impl_(std::make_unique<Impl>(threads)) {}
ReduceExecutionContext::~ReduceExecutionContext() = default;
int ReduceExecutionContext::threads() const { return impl_->threads(); }
void ReduceExecutionContext::run(size_t count, void* state,
                                 void (*work)(void*, size_t)) {
  impl_->run(count, state, work);
}

namespace {
struct ReductionState : KernelState {
  struct Binding {
    double* value;
    int input;
    int64_t offset, len, gradient;
  };
  struct Child {
    std::unique_ptr<Executor> executor;
    std::vector<Binding> bindings;
    std::vector<double> gradient;
    double value = 0;
  };
  std::vector<Child> children;
  KernelCtx* context = nullptr;
  bool values_only = false, ready = false;

  explicit ReductionState(const ReduceSumSpec& spec, const Op& op,
                          const Slot* slots) {
    if (spec.children.empty())
      throw std::logic_error("empty retained reduction");
    for (const auto& program : spec.children) {
      Child child;
      child.executor = std::make_unique<Executor>(program.graph);
      for (const auto& fill : program.fills)
        child.executor->set_values(fill.first, fill.second.data(),
                                   fill.second.size());
      const auto& graph = child.executor->graph();
      if (graph.result_slot < 0 || graph.slots.at(graph.result_slot).len != 1)
        throw std::logic_error("reduction child must return a scalar");
      int64_t parameters = 0;
      std::vector<int64_t> parameter_offset(graph.slots.size(), -1);
      for (size_t s = 0; s < graph.slots.size(); ++s)
        if (graph.slots[s].is_param) {
          parameter_offset[s] = parameters;
          parameters += graph.slots[s].len;
        }
      std::vector<bool> bound(graph.slots.size(), false);
      for (const auto& in : program.imports) {
        if (in.slot < 0 || size_t(in.slot) >= graph.slots.size() ||
            bound[in.slot] || in.input < 0 || in.input >= op.n_in ||
            in.offset < 0)
          throw std::logic_error("invalid reduction import");
        const auto len = graph.slots[in.slot].len;
        if (in.offset > slots[op.in[in.input]].len ||
            len > slots[op.in[in.input]].len - in.offset)
          throw std::logic_error("reduction import exceeds input");
        bound[in.slot] = true;
        child.bindings.push_back({child.executor->value_ptr(in.slot), in.input,
                                  in.offset, len, parameter_offset[in.slot]});
      }
      for (size_t s = 0; s < graph.slots.size(); ++s)
        if (graph.slots[s].is_param && graph.slots[s].len && !bound[s])
          throw std::logic_error("unbound reduction parameter");
      child.gradient.resize(parameters);
      children.push_back(std::move(child));
    }
  }
  static void forward_job(void* state, size_t i) {
    auto& self = *static_cast<ReductionState*>(state);
    auto& child = self.children[i];
    for (const auto& in : child.bindings)
      if (in.len)
        std::copy_n(self.context->in[in.input].data + in.offset, in.len,
                    in.value);
    child.value = self.values_only ? child.executor->forward_value_only()
                                   : child.executor->forward();
  }
  static void backward_job(void* state, size_t i) {
    auto& self = *static_cast<ReductionState*>(state);
    auto& child = self.children[i];
    child.executor->reverse(child.gradient.data(), self.context->out_adj);
  }
  void run(void (*job)(void*, size_t)) {
    if (context->eval_state && context->eval_state->reduce)
      context->eval_state->reduce->run(children.size(), this, job);
    else
      for (size_t i = 0; i < children.size(); ++i) job(this, i);
  }
};
void forward(KernelCtx& c) {
  auto& state = *static_cast<ReductionState*>(c.state);
  state.ready = false;
  state.context = &c;
  state.values_only = stanli::values_only();
  state.run(ReductionState::forward_job);
  double sum = 0;
  for (const auto& child : state.children) sum += child.value;
  c.out.data[0] = sum;
  state.ready = !state.values_only;
}
void backward(KernelCtx& c) {
  auto& state = *static_cast<ReductionState*>(c.state);
  if (!state.ready)
    throw std::logic_error("reduction reverse requires forward");
  state.ready = false;
  state.context = &c;
  state.run(ReductionState::backward_job);
  for (const auto& child : state.children)
    for (const auto& in : child.bindings)
      if (in.gradient >= 0 && c.in_adj[in.input].data)
        for (int64_t j = 0; j < in.len; ++j)
          c.in_adj[in.input].data[in.offset + j] +=
              child.gradient[in.gradient + j];
}
KernelState* make_state(const Op& op, const Slot* slots) {
  if (!op.udata) throw std::logic_error("missing reduction program");
  return new ReductionState(*static_cast<const ReduceSumSpec*>(op.udata), op,
                            slots);
}
}  // namespace
void register_reduce_sum_kernel() {
  register_kernel(OP_REDUCE_SUM, {forward, backward, nullptr, make_state});
}
}  // namespace stanli
