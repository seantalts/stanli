// C bridge to the embedded stanc3 (OCaml, linked in via
// -output-complete-obj). Compiled into libstanli only when the embed object
// is available (STANLI_EMBED_STANC).
//
// OCaml runtime notes: caml_startup runs once; callbacks must come from a
// thread known to the OCaml runtime. v1 policy: all stanc calls happen on
// the thread that first called it (Python's ctypes calls satisfy this).
#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/mlvalues.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {

// "OK<MIR>" or "ERR<message>"; caller frees with stanli_stanc_free.
char* stanli_stanc_tmir(const char* stan_code) {
  static std::once_flag once;
  std::call_once(once, [] {
    static char arg0[] = "stanli";
    static char* argv[] = {arg0, nullptr};
    caml_startup(argv);
  });
  static const value* fn = nullptr;
  if (fn == nullptr) fn = caml_named_value("stanc_compile_tmir");
  if (fn == nullptr) {
    return strdup("ERRembedded stanc entry point not registered");
  }
  value res = caml_callback(*fn, caml_copy_string(stan_code));
  return strdup(String_val(res));
}

void stanli_stanc_free(char* p) { std::free(p); }

}  // extern "C"
