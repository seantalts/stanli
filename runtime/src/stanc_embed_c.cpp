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

#include <stanli/stanc_result.hpp>

#include <mutex>
#include <string>

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

stanli::StancResult error_result(const char* message) {
  stanli::StancResult out;
  out.error = message;
  return out;
}

std::string ocaml_string(value v) {
  return std::string(String_val(v), caml_string_length(v));
}

// Field order follows embed_result in compiler/native/stanc_embed.ml.
stanli::StancResult read_result(value res) {
  stanli::StancResult out;
  const value outcome = Field(res, 0);
  if (Tag_val(outcome) == 0)
    out.mir = ocaml_string(Field(outcome, 0));
  else
    out.error = ocaml_string(Field(outcome, 0));
  const value warnings = Field(res, 1);
  for (mlsize_t i = 0; i < Wosize_val(warnings); ++i)
    out.warnings.push_back(ocaml_string(Field(warnings, i)));
  const value o1_fallback = Field(res, 2);
  if (Is_some(o1_fallback))
    out.o1_fallback = ocaml_string(Some_val(o1_fallback));
  return out;
}

}  // namespace

// Called with the OCaml runtime lock held. Root every allocation while building
// the argument array: copying a later path may collect the earlier strings.
static stanli::StancResult invoke_callback(
    const value* fn, const char* stan_code, const char* filename,
    const char* const* include_paths, size_t include_path_count, int flags) {
  CAMLparam0();
  CAMLlocal3(code, paths, name);
  CAMLlocal2(res, flag_value);
  code = caml_copy_string(stan_code);
  paths = caml_alloc(include_path_count, 0);
  for (size_t i = 0; i < include_path_count; ++i)
    Store_field(paths, i, caml_copy_string(include_paths[i]));
  name = caml_copy_string(filename != nullptr ? filename : "");
  flag_value = Val_int(flags);
  value args[4] = {code, paths, name, flag_value};
  res = caml_callbackN_exn(*fn, 4, args);
  stanli::StancResult out =
      Is_exception_result(res)
          ? error_result("embedded stanc callback raised an OCaml exception")
          : read_result(res);
  CAMLreturnT(stanli::StancResult, out);
}

namespace stanli {

StancResult embedded_stanc_compile(const char* stan_code, const char* filename,
                                   const char* const* include_paths,
                                   size_t include_path_count, int flags) {
  if (stan_code == nullptr) return error_result("Stan source is null");

  std::lock_guard<std::mutex> serial(compile_mutex);
  std::call_once(startup_once, start_ocaml);

  RegisteredThread thread;
  if (!thread.ok()) {
    return error_result(
        "could not register calling thread with the OCaml runtime");
  }

  RuntimeLock runtime;
  const value* fn = caml_named_value("stanc_compile");
  if (fn == nullptr) {
    return error_result("embedded stanc entry point not registered");
  }

  return invoke_callback(fn, stan_code, filename, include_paths,
                         include_path_count, flags);
}

}  // namespace stanli
