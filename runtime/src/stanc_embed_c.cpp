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
#include <caml/memory.h>
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

// Called with the OCaml runtime lock held. Root every allocation while building
// the argument array: copying a later path may collect the earlier strings.
static char* invoke_callback(const value* fn, const char* stan_code,
                             const char* const* include_paths,
                             size_t include_path_count) {
  CAMLparam0();
  CAMLlocal3(code, paths, res);
  code = caml_copy_string(stan_code);
  if (include_paths == nullptr) {
    res = caml_callback_exn(*fn, code);
  } else {
    paths = caml_alloc(include_path_count, 0);
    for (size_t i = 0; i < include_path_count; ++i)
      Store_field(paths, i, caml_copy_string(include_paths[i]));
    res = caml_callback2_exn(*fn, code, paths);
  }
  char* out =
      Is_exception_result(res)
          ? error_result("embedded stanc callback raised an OCaml exception")
          : strdup(String_val(res));
  CAMLreturnT(char*, out);
}

// "OK<MIR>" or "ERR<message>"; caller frees with stanli_stanc_free.
static char* compile_callback(const char* stan_code, const char* entry,
                              const char* const* include_paths = nullptr,
                              size_t include_path_count = 0) {
  if (stan_code == nullptr) return error_result("Stan source is null");

  std::lock_guard<std::mutex> serial(compile_mutex);
  std::call_once(startup_once, start_ocaml);

  RegisteredThread thread;
  if (!thread.ok()) {
    return error_result(
        "could not register calling thread with the OCaml runtime");
  }

  RuntimeLock runtime;
  const value* fn = caml_named_value(entry);
  if (fn == nullptr) {
    return error_result("embedded stanc entry point not registered");
  }

  return invoke_callback(fn, stan_code, include_paths, include_path_count);
}

char* stanli_stanc_tmir(const char* stan_code) {
  return compile_callback(stan_code, "stanc_compile_tmir");
}

char* stanli_stanc_model_tmir(const char* stan_code) {
  return compile_callback(stan_code, "stanc_compile_model_tmir");
}

char* stanli_stanc_tmir_with_includes(const char* stan_code,
                                      const char* const* include_paths,
                                      size_t include_path_count) {
  // A non-null pointer selects the two-argument callback even for an empty
  // list.
  const char* empty = nullptr;
  return compile_callback(stan_code, "stanc_compile_tmir_with_includes",
                          include_paths ? include_paths : &empty,
                          include_path_count);
}

void stanli_stanc_free(char* p) { std::free(p); }

}  // extern "C"
