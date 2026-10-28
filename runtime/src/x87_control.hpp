// A new Windows x64 thread starts at 53-bit x87 precision while a MinGW main
// thread runs at 64, so long double results would depend on which thread
// computed them. A helper thread takes its creator's control word instead.
#ifndef STANLI_X87_CONTROL_HPP
#define STANLI_X87_CONTROL_HPP

namespace stanli {
namespace detail {

class X87ControlWord {
 public:
  static X87ControlWord of_this_thread() {
    X87ControlWord w;
#if defined(_WIN32) && defined(__x86_64__)
    __asm__ volatile("fnstcw %0" : "=m"(w.word_));
#endif
    return w;
  }
  void apply_to_this_thread() const {
#if defined(_WIN32) && defined(__x86_64__)
    __asm__ volatile("fldcw %0" : : "m"(word_));
#endif
  }

 private:
  unsigned short word_ = 0;
};

}  // namespace detail
}  // namespace stanli

#endif
