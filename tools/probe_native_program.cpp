// Developer-only feasibility comparator. Emit a bounded Program/AdjProgram
// subset as C++; no production backend, cache, runtime export or dependency.
#include <stanli/island.hpp>
#include <stanli/program.hpp>
#include <stanli/optable.hpp>
#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace stanli;
namespace {
using Bridge = void (*)(void*, int, int, double*, double*);
using Generated = void (*)(double*, double*, const double*, Bridge, void*);
void demand(bool b, const char* message) {
  if (!b) throw std::runtime_error(message);
}
std::vector<IslandProg> programs() {
  std::vector<IslandProg> result;
  for (int steps : {8, 128}) {
    IslandProg p;
    p.n_regs = 2;
    p.ins = {{0, 2}};
    int last = 1;
    for (int i = 0; i < steps; ++i) {
      int product = p.n_regs++, sum = p.n_regs++;
      p.code.emplace_back(Program::MUL, product, last, 0);
      p.code.emplace_back(Program::ADD, sum, product, 1);
      last = sum;
    }
    p.out_regs = {last};
    result.push_back(std::move(p));
  }
  {
    IslandProg p;
    p.n_regs = 5;
    p.ins = {{0, 1, -1, 0, false}, {1, 2}};
    p.code = {{Program::JZ, 3, 0},     {Program::MUL, 3, 1, 2},
              {Program::JMP, 4},       {Program::ADD, 3, 1, 2},
              {Program::MUL, 1, 1, 2}, {Program::ADD, 4, 3, 1}};
    p.out_regs = {4, 3};
    result.push_back(std::move(p));
  }
  {
    IslandProg p;
    p.n_regs = 8;
    p.ins = {{0, 4}};
    p.out_regs = {4, 5, 6, 7};
    Program::Call call;
    call.opcode = OP_CHOLESKY;
    call.n_in = 1;
    call.in[0] = 0;
    call.in_len[0] = 4;
    call.out = 4;
    call.out_len = 4;
    call.idata = {2};
    demand(bind_call(call), "Cholesky bind failed");
    p.calls.push_back(call);
    p.code.emplace_back(Program::CALL, 0, 0);
    result.push_back(std::move(p));
  }
  for (auto& p : result) demand(gen_adjoint(p), "adjoint generation refused");
  return result;
}
std::string cell(const char* name, int index) {
  return std::string(name) + "[" + std::to_string(index) + "]";
}
void forward(std::ostream& o, const Program& p) {
  for (size_t pc = 0; pc < p.code.size(); ++pc) {
    const auto& i = p.code[pc];
    const auto d = cell("r", i.dst), a = cell("r", i.a), b = cell("r", i.b);
    o << "L" << pc << ":;\n";
    switch (i.code) {
      case Program::CONST:
        o << d << "=pool[" << i.a << "];\n";
        break;
      case Program::FILL:
      case Program::CONSTR:
        for (int k = 0; k < i.len; ++k)
          o << cell("r", i.dst + k) << "=pool["
            << i.a + (i.code == Program::FILL ? 0 : k) << "];\n";
        break;
      case Program::MOV:
        o << d << "=" << a << ";\n";
        break;
      case Program::MOVR:
        for (int k = 0; k < i.len; ++k)
          o << cell("r", i.dst + k) << "=" << cell("r", i.a + k) << ";\n";
        break;
      case Program::ADD:
        o << d << "=" << a << "+" << b << ";\n";
        break;
      case Program::MUL:
        o << d << "=" << a << "*" << b << ";\n";
        break;
      case Program::JZ:
        o << "if(" << a << "==0.0) goto L" << i.dst << ";\n";
        break;
      case Program::JMP:
        o << "goto L" << i.dst << ";\n";
        break;
      case Program::CALL:
        o << "bridge(context," << i.a << ",0,r,a);\n";
        break;
      default:
        throw std::runtime_error("forward subset refused opcode " +
                                 std::to_string(i.code));
    }
  }
  o << "L" << p.code.size() << ":;\n";
}
void reverse_segment(std::ostream& o, const AdjProgram& ap, int begin,
                     int end) {
  for (int pc = begin; pc < end; ++pc) {
    const auto& i = ap.code[pc];
    auto d = cell("a", i.dst), a = cell("a", i.a), b = cell("a", i.b);
    o << "{\n";
    switch (i.code) {
      case Program::CONST:
        o << d << "=0.0;\n";
        break;
      case Program::FILL:
      case Program::CONSTR:
        for (int k = 0; k < i.len; ++k) o << cell("a", i.dst + k) << "=0.0;\n";
        break;
      case Program::MOV:
        o << "double t=" << d << ";" << d << "=0.0;" << a << "+=t;\n";
        break;
      case Program::MOVR:
        if (i.a == i.dst) {
          for (int k = 0; k < i.len; ++k)
            o << "{double t=" << cell("a", i.dst + k) << ";"
              << cell("a", i.dst + k) << "=0.0;" << cell("a", i.a + k)
              << "+=t;}\n";
          break;
        }
        demand(i.a + i.len <= i.dst || i.dst + i.len <= i.a,
               "reverse subset refuses overlapping MOVR");
        for (int k = 0; k < i.len; ++k)
          o << cell("a", i.a + k) << "+=" << cell("a", i.dst + k) << ";\n";
        for (int k = 0; k < i.len; ++k) o << cell("a", i.dst + k) << "=0.0;\n";
        break;
      case Program::ADD:
        o << "double t=" << d << ";" << d << "=0.0;" << a << "+=t;" << b
          << "+=t;\n";
        break;
      case Program::MUL:
        o << "double t=" << d << ";" << d << "=0.0;" << a
          << "+=" << cell("r", i.vb) << "*t;" << b << "+=" << cell("r", i.va)
          << "*t;\n";
        break;
      case Program::CALL:
        o << "bridge(context," << i.a << ",1,r,a);\n";
        break;
      default:
        throw std::runtime_error("reverse subset refused opcode " +
                                 std::to_string(i.code));
    }
    o << "}\n";
  }
}
void emit(const std::vector<IslandProg>& ps, const char* path) {
  std::ofstream o(path);
  o << "using Bridge=void(*)(void*,int,int,double*,double*);\n";
  for (size_t n = 0; n < ps.size(); ++n) {
    const auto& p = ps[n];
    o << "extern \"C\" void f" << n
      << "(double*r,double*a,const double*pool,Bridge bridge,void*context){\n";
    forward(o, p);
    o << "}\n";
    o << "extern \"C\" void b" << n
      << "(double*r,double*a,const double*pool,Bridge bridge,void*context){\n";
    if (p.adj.segments.empty())
      reverse_segment(o, p.adj, 0, p.adj.code.size());
    else
      for (const auto& s : p.adj.segments) {
        o << "if(r[" << s.guard << "]!=0.0){\n";
        reverse_segment(o, p.adj, s.begin, s.end);
        o << "}\n";
      }
    o << "}\n";
    std::cout << "PROGRAM " << n << " forward=" << p.code.size()
              << " reverse=" << p.adj.code.size() << " values=" << p.n_regs
              << " adjoints=" << p.adj.n_regs << '\n';
  }
  demand(bool(o), "writing source failed");
}
void bridge(void* context, int index, int backward, double* values,
            double* adj) {
  auto& p = *static_cast<IslandProg*>(context);
  const auto& c = p.calls.at(index);
  if (!backward) {
    run_call(c, values);
    return;
  }
  KernelCtx ctx;
  ctx.n_in = c.n_in;
  for (int k = 0; k < c.n_in; ++k) {
    ctx.in[k] = {values + c.bwd_value_in[k], c.in_len[k]};
    ctx.in_adj[k] = {
        (c.input_adjoint_mask & (1u << k)) ? adj + c.bwd_adj_in[k] : nullptr,
        c.in_len[k]};
  }
  ctx.out = {values + c.bwd_value_out, c.out_len};
  ctx.out_adj_vec = {adj + c.bwd_adj_out, c.out_len};
  ctx.out_adj = c.out_len == 1 ? adj[c.bwd_adj_out] : 0.0;
  ctx.variant = c.variant;
  ctx.scratch = values + c.scratch;
  ctx.idata = c.idata.data();
  ctx.n_idata = c.idata.size();
  ctx.udata = c.udata_owner.get();
  c.backward(ctx);
  std::fill_n(adj + c.bwd_adj_out, c.out_len, 0.0);
}
bool equivalent(double a, double b) {
  return std::memcmp(&a, &b, sizeof(double)) == 0 ||
         (std::isnan(a) && std::isnan(b));
}
void evaluate(IslandProg& p, std::vector<double>& v, std::vector<double>& a,
              Generated f, Generated b, const std::vector<double>& in) {
  std::copy(in.begin(), in.end(), v.begin());
  if (f)
    f(v.data(), a.data(), p.pool.data(), bridge, &p);
  else
    run_program(p, v.data());
  std::fill(a.begin(), a.end(), 0.0);
  for (size_t k = p.out_regs.size(); k-- > 0;)
    a[p.adj.adj_reg[p.out_regs[k]]] += (k % 2 ? -0.75 : 1.25);
  if (b)
    b(v.data(), a.data(), p.pool.data(), bridge, &p);
  else
    run_adjoint(p, p.adj, v.data(), a.data());
}
template <class F>
double measure(F work, double seconds) {
  auto start = std::chrono::steady_clock::now();
  size_t n = 0;
  do {
    for (int i = 0; i < 100; ++i) {
      work();
      ++n;
    }
  } while (
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count() < seconds);
  return std::chrono::duration<double, std::nano>(
             std::chrono::steady_clock::now() - start)
             .count() /
         n;
}
void test(std::vector<IslandProg>& ps, const char* path) {
  const auto load_start = std::chrono::steady_clock::now();
  void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  demand(handle, dlerror());
  std::cout << "LOAD_US "
            << std::chrono::duration<double, std::micro>(
                   std::chrono::steady_clock::now() - load_start)
                   .count()
            << '\n';
  for (size_t n = 0; n < ps.size(); ++n) {
    auto& p = ps[n];
    auto f = reinterpret_cast<Generated>(
        dlsym(handle, ("f" + std::to_string(n)).c_str()));
    auto b = reinterpret_cast<Generated>(
        dlsym(handle, ("b" + std::to_string(n)).c_str()));
    demand(f && b, "missing generated entry");
    std::vector<double> v(p.n_regs), w(p.n_regs), a(p.adj.n_regs),
        z(p.adj.n_regs);
    std::vector<double> in = n < 2    ? std::vector<double>{.9, .01}
                             : n == 2 ? std::vector<double>{1, .7, 1.3}
                                      : std::vector<double>{2, .2, .2, 3};
    for (int trial = 0; trial < 12; ++trial) {
      auto inputs = in;
      if (n == 2) inputs[0] = trial % 2;
      if (n < 2 && trial == 1) inputs[0] = -0.0;
      if (n < 2 && trial == 2) inputs[0] = INFINITY;
      if (n < 2 && trial == 3) inputs[0] = NAN;
      if (n == 3 && trial == 1) inputs[0] = -1;
      std::string e1, e2;
      try {
        evaluate(p, v, a, nullptr, nullptr, inputs);
      } catch (const std::exception& e) {
        e1 = e.what();
      }
      try {
        evaluate(p, w, z, f, b, inputs);
      } catch (const std::exception& e) {
        e2 = e.what();
      }
      demand(e1 == e2, "exception mismatch");
      if (!e1.empty()) continue;
      for (int r : p.out_regs)
        demand(equivalent(v[r], w[r]), "forward mismatch");
      for (const auto& li : p.ins)
        if (li.active)
          for (int k = 0; k < li.len; ++k)
            demand(equivalent(a[p.adj.adj_reg[li.reg + k]],
                              z[p.adj.adj_reg[li.reg + k]]),
                   "weighted adjoint mismatch");
    }
    std::cout << "PARITY " << n
              << " passed changing-input/reuse/weighted-adjoint checks\n";
    auto vm = [&] { evaluate(p, v, a, nullptr, nullptr, in); };
    auto native = [&] { evaluate(p, w, z, f, b, in); };
    measure(vm, .2);
    measure(native, .2);
    for (int sample = 0; sample < 6; ++sample) {
      double tv, tn;
      if (sample % 2) {
        tn = measure(native, .25);
        tv = measure(vm, .25);
      } else {
        tv = measure(vm, .25);
        tn = measure(native, .25);
      }
      std::cout << "BENCH " << n << ' ' << sample << ' ' << tv << ' ' << tn
                << '\n';
    }
  }
  dlclose(handle);
}
}  // namespace
int main(int argc, char** argv) {
  try {
    demand(argc == 3,
           "usage: probe_native_program --emit output.cpp | --run module");
    auto ps = programs();
    if (std::string(argv[1]) == "--emit")
      emit(ps, argv[2]);
    else if (std::string(argv[1]) == "--run")
      test(ps, argv[2]);
    else
      throw std::runtime_error("unknown mode");
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
