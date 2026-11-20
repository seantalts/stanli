// ProgramCompiler: the constant pool, declarations, and closing the program.
#include <stanli/mir_prog.hpp>

namespace stanli {

int ProgramCompiler::pool_at(const double* v, int n) {
  for (size_t s = 0; s + (size_t)n <= p.pool.size(); ++s)
    if (std::memcmp(v, p.pool.data() + s, (size_t)n * sizeof(double)) == 0)
      return (int)s;
  const int at = (int)p.pool.size();
  p.pool.insert(p.pool.end(), v, v + n);
  return at;
}

Program::Instr ProgramCompiler::const_instr(int dst, const double* v, int n) {
  // Broadcast immutable constants, never active registers. In var replay
  // one leaf represents the fill; writes still replace individual handles.
  // Compare bits to retain signed zero and NaN payloads exactly.
  bool uniform = n > 1;
  for (int k = 1; uniform && k < n; ++k)
    uniform = std::memcmp(v, v + k, sizeof(double)) == 0;
  if (uniform)
    return Program::Instr{Program::FILL, dst, pool_at(v, 1), 0, 0, n};
  return Program::Instr{
      n == 1 ? Program::CONST : Program::CONSTR, dst, pool_at(v, n), 0, 0, n};
}

int64_t ProgramCompiler::sized_len(const mir::SizedType& t) {
  int64_t n = 1;
  for (const auto& d : t.dims) {
    const int64_t extent = cint(d);
    if (extent < 0 || (n != 0 && extent != 0 && n > kMaxRegs / extent))
      bail("declaration has an invalid or oversized shape");
    n *= extent;
  }
  return n;
}

Range ProgramCompiler::declare(const std::string& name, int len, Range view,
                               double fill) {
  Range r = view;
  r.reg = alloc(len);
  r.len = len;
  const std::vector<double> init((size_t)len, fill);
  emit_const(r.reg, init.data(), len);
  reals[name] = r;
  return r;
}

void ProgramCompiler::finish() {
  if (late_bound.empty() && hoisted_int_initializers.empty() &&
      unused_int_resets.empty())
    return;
  std::vector<Program::Instr> code = std::move(hoisted_int_initializers);
  for (const auto& [reg, len] : late_bound) {
    const std::vector<double> nan((size_t)len,
                                  std::numeric_limits<double>::quiet_NaN());
    code.push_back(const_instr(reg, nan.data(), len));
  }
  // Preserve the existing cheap prepend for programs with no markers to
  // remove. They need only a constant jump offset, not a relocation map.
  if (unused_int_resets.empty()) {
    const int n = static_cast<int>(code.size());
    for (auto& instr : p.code)
      if (instr.code == Program::JZ || instr.code == Program::JMP)
        instr.dst += n;
    p.code.insert(p.code.begin(), code.begin(), code.end());
    int_reset_at.clear();
    late_bound.clear();
    return;
  }
  std::vector<int> pc_map(p.code.size() + 1);
  for (size_t pc = 0; pc < p.code.size(); ++pc) {
    pc_map[pc] = static_cast<int>(code.size());
    if (!unused_int_resets.count(static_cast<int>(pc)))
      code.push_back(p.code[pc]);
  }
  pc_map.back() = static_cast<int>(code.size());
  for (auto& instr : code)
    if (instr.code == Program::JZ || instr.code == Program::JMP)
      instr.dst = pc_map.at(static_cast<size_t>(instr.dst));
  p.code = std::move(code);
  unused_int_resets.clear();
  int_reset_at.clear();
  late_bound.clear();
}

}  // namespace stanli
