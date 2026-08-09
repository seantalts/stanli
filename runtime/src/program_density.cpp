// The one switch over STANLI_SCALAR_DENSITY_LIST for the non-kernel
// callers (program_density.hpp says why there is only one list now).
//
// Everything here is generated from that list, so a density added to it
// reaches the register machine's forward, the generated adjoint and the
// MIR interpreter together, and cannot reach one and not another.
//
// recorder.hpp FIRST: it registers rvar's traits and value_of overloads,
// and stan-math's templates only find them if they are declared by the
// time those templates are parsed. The density shards open the same way.
#include <stanli/recorder.hpp>

#include <stanli/optable.hpp>
#include <stanli/program_density.hpp>

#include <stan/math.hpp>

namespace stanli {
namespace {

// Density ids are indices into the list, and this enum is what makes that
// true of the switch labels below as well: same list, same order.
enum : int {
#define STANLI_PD_ENUM(opc, fn, arity, tier) kId_##fn,
  STANLI_SCALAR_DENSITY_LIST(STANLI_PD_ENUM)
#undef STANLI_PD_ENUM
};

struct Entry {
  const char* name;
  uint16_t opcode;
  int arity;
};

constexpr Entry kDensities[] = {
#define STANLI_PD_ENTRY(opc, fn, arity, tier) {#fn, opc, arity},
    STANLI_SCALAR_DENSITY_LIST(STANLI_PD_ENTRY)
#undef STANLI_PD_ENTRY
};
constexpr int kCount = (int)(sizeof(kDensities) / sizeof(kDensities[0]));

// Four is the widest Stan gives these (student_t, skew_normal,
// exp_mod_normal, pareto_type_2, skew_double_exponential); a fifth would
// fail to compile here rather than silently drop an argument.
template <int Arity, typename F, typename T>
auto call_with(F&& f, const T* a) {
  static_assert(Arity >= 1 && Arity <= 4, "density arity out of range");
  if constexpr (Arity == 1) {
    return f(a[0]);
  } else if constexpr (Arity == 2) {
    return f(a[0], a[1]);
  } else if constexpr (Arity == 3) {
    return f(a[0], a[1], a[2]);
  } else {
    return f(a[0], a[1], a[2], a[3]);
  }
}

// The same, promoting doubles to the recording scalar on the way in.
template <int Arity, typename F>
void call_rvar(F&& f, const double* a) {
  static_assert(Arity >= 1 && Arity <= 4, "density arity out of range");
  if constexpr (Arity == 1) {
    f(rvar(a[0]));
  } else if constexpr (Arity == 2) {
    f(rvar(a[0]), rvar(a[1]));
  } else if constexpr (Arity == 3) {
    f(rvar(a[0]), rvar(a[1]), rvar(a[2]));
  } else {
    f(rvar(a[0]), rvar(a[1]), rvar(a[2]), rvar(a[3]));
  }
}

bool bad(int id) { return id < 0 || id >= kCount; }

}  // namespace

int program_density_count() { return kCount; }

int program_density_arity(int id) { return bad(id) ? 0 : kDensities[id].arity; }

const char* program_density_name(int id) {
  return bad(id) ? "" : kDensities[id].name;
}

int program_density_id_by_name(const std::string& name) {
  for (int i = 0; i < kCount; ++i)
    if (name == kDensities[i].name) return i;
  return -1;
}

int program_density_id_by_opcode(uint16_t opcode) {
  for (int i = 0; i < kCount; ++i)
    if (opcode == kDensities[i].opcode) return i;
  return -1;
}

template <typename T>
T program_density(int id, const T* args) {
  switch (id) {
#define STANLI_PD_CASE(opc, fn, arity, tier)                        \
  case kId_##fn:                                                    \
    return call_with<arity>(                                        \
        [](const auto&... x) { return stan::math::fn<false>(x...); }, args);
    STANLI_SCALAR_DENSITY_LIST(STANLI_PD_CASE)
#undef STANLI_PD_CASE
    default:
      return T(0.0);
  }
}

template double program_density<double>(int, const double*);
template stan::math::var program_density<stan::math::var>(
    int, const stan::math::var*);

void program_density_partials(int id, const double* args, double* partials) {
  const int n = program_density_arity(id);
  sink s;
  for (int k = 0; k < n; ++k) s.buf[k] = &partials[k];
  active_sink() = &s;
  switch (id) {
#define STANLI_PD_PARTIALS(opc, fn, arity, tier)                       \
  case kId_##fn:                                                       \
    call_rvar<arity>([](const auto&... x) { stan::math::fn<false>(x...); }, \
                     args);                                            \
    break;
    STANLI_SCALAR_DENSITY_LIST(STANLI_PD_PARTIALS)
#undef STANLI_PD_PARTIALS
    default:
      break;
  }
  active_sink() = nullptr;
}

}  // namespace stanli
