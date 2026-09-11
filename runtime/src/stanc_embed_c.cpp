// C bridge to the embedded stanc3 (OCaml, linked in via
// -output-complete-obj). Compiled into libstanli only when the embed object
// is available (STANLI_EMBED_STANC).
//
// caml_startup registers its calling thread and leaves the runtime lock held.
// We release it after startup, then register every other C-created thread and
// acquire the lock around its callback.  stanc3 also has process-global state,
// so the outer mutex deliberately serializes whole compilations rather than
// relying on the OCaml runtime lock alone.
#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/mlvalues.h>
#include <caml/threads.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

std::once_flag startup_once;
std::mutex compile_mutex;

// Set only on the C thread that called caml_startup.  The runtime registered
// that thread itself; caml_c_thread_register would return 0 for it, the same
// value the API uses for an actual registration failure.
thread_local bool startup_thread = false;

void start_ocaml() {
  static char_os arg0[] = {'s', 't', 'a', 'n', 'l', 'i', '\0'};
  static char_os* argv[] = {arg0, nullptr};
  caml_startup(argv);
  startup_thread = true;

  // caml_startup returns with the main domain lock held.  Leaving it held
  // would deadlock the first callback arriving on any other C/Python thread.
  caml_release_runtime_system();
}

class RegisteredThread {
 public:
  RegisteredThread() {
    if (startup_thread) {
      registered_ = true;
    } else {
      unregister_ = caml_c_thread_register() != 0;
      registered_ = unregister_;
    }
  }

  RegisteredThread(const RegisteredThread&) = delete;
  RegisteredThread& operator=(const RegisteredThread&) = delete;

  ~RegisteredThread() {
    // caml_c_thread_unregister must be called without the runtime lock.  The
    // RuntimeLock below is destroyed before this object on every return path.
    if (unregister_) (void)caml_c_thread_unregister();
  }

  bool ok() const { return registered_; }

 private:
  bool registered_ = false;
  bool unregister_ = false;
};

class RuntimeLock {
 public:
  RuntimeLock() { caml_acquire_runtime_system(); }
  RuntimeLock(const RuntimeLock&) = delete;
  RuntimeLock& operator=(const RuntimeLock&) = delete;
  ~RuntimeLock() { caml_release_runtime_system(); }
};

char* error_result(const char* message) {
  const size_t n = std::strlen(message);
  char* out = static_cast<char*>(std::malloc(n + 4));
  if (out == nullptr) return nullptr;
  std::memcpy(out, "ERR", 3);
  std::memcpy(out + 3, message, n + 1);
  return out;
}

}  // namespace

extern "C" {

// "OK<MIR>" or "ERR<message>"; caller frees with stanli_stanc_free.
char* stanli_stanc_tmir(const char* stan_code) {
  if (stan_code == nullptr) return error_result("Stan source is null");

  std::lock_guard<std::mutex> serial(compile_mutex);
  std::call_once(startup_once, start_ocaml);

  RegisteredThread thread;
  if (!thread.ok()) {
    return error_result(
        "could not register calling thread with the OCaml runtime");
  }

  RuntimeLock runtime;
  static const value* fn = nullptr;
  if (fn == nullptr) fn = caml_named_value("stanc_compile_tmir");
  if (fn == nullptr) {
    return error_result("embedded stanc entry point not registered");
  }

  value res = caml_callback_exn(*fn, caml_copy_string(stan_code));
  if (Is_exception_result(res)) {
    return error_result("embedded stanc callback raised an OCaml exception");
  }
  return strdup(String_val(res));
}

void stanli_stanc_free(char* p) { std::free(p); }

}  // extern "C"
