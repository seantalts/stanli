// Single-threaded tests: stan-math's rev core registers a TBB thread observer
// so each worker gets its own AD tape. We run one thread and build no TBB, so
// provide the one symbol the observer base class needs.
#if defined(__APPLE__)
extern "C" void stanli_observe_stub(void*, bool) {}
asm(".globl __ZN3tbb8internal26task_scheduler_observer_v37observeEb\n"
    "__ZN3tbb8internal26task_scheduler_observer_v37observeEb = "
    "_stanli_observe_stub\n");
#else
namespace tbb {
namespace internal {
class task_scheduler_observer_v3 {
 public:
  void observe(bool);
};
void task_scheduler_observer_v3::observe(bool) {}
}  // namespace internal
}  // namespace tbb
#endif
