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

#include <type_traits>

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

// The same, promoting the arguments Mask names to the recording scalar and
// leaving the rest as doubles. A double argument selects stan-math's
// arithmetic edge, which computes no partial.
template <unsigned Mask, int K>
auto bind_arg(const double* a) {
  if constexpr ((Mask >> K) & 1u) {
    return rvar(a[K]);
  } else {
    return a[K];
  }
}

template <int Arity, unsigned Mask, typename F>
auto call_rvar(F&& f, const double* a) {
  static_assert(Arity >= 1 && Arity <= 4, "density arity out of range");
  if constexpr (Arity == 1) {
    return f(bind_arg<Mask, 0>(a));
  } else if constexpr (Arity == 2) {
    return f(bind_arg<Mask, 0>(a), bind_arg<Mask, 1>(a));
  } else if constexpr (Arity == 3) {
    return f(bind_arg<Mask, 0>(a), bind_arg<Mask, 1>(a), bind_arg<Mask, 2>(a));
  } else {
    return f(bind_arg<Mask, 0>(a), bind_arg<Mask, 1>(a), bind_arg<Mask, 2>(a),
             bind_arg<Mask, 3>(a));
  }
}

// Run `f` with the mask as a constant. Densities whose tier does not carry
// STANLI_DENSITY_FULL_MASKS get one all-active binding instead of 2^arity of
// them: the value is the same either way, and the instantiations are what a
// density costs to compile.
template <int Arity, int Tier, typename F>
void with_mask(unsigned mask, F&& f) {
  constexpr unsigned kAll = (1u << Arity) - 1u;
  if constexpr ((Tier & STANLI_DENSITY_FULL_MASKS) == 0) {
    f(std::integral_constant<unsigned, kAll>{});
  } else {
    switch (mask & kAll) {
#define STANLI_PD_MASK_CASE(k)                            \
  case (unsigned)k:                                       \
    if constexpr ((unsigned)k <= kAll)                    \
      f(std::integral_constant<unsigned, (unsigned)k>{}); \
    break;
      STANLI_PD_MASK_CASE(1)
      STANLI_PD_MASK_CASE(2)
      STANLI_PD_MASK_CASE(3)
      STANLI_PD_MASK_CASE(4)
      STANLI_PD_MASK_CASE(5)
      STANLI_PD_MASK_CASE(6)
      STANLI_PD_MASK_CASE(7)
      STANLI_PD_MASK_CASE(8)
      STANLI_PD_MASK_CASE(9)
      STANLI_PD_MASK_CASE(10)
      STANLI_PD_MASK_CASE(11)
      STANLI_PD_MASK_CASE(12)
      STANLI_PD_MASK_CASE(13)
      STANLI_PD_MASK_CASE(14)
      STANLI_PD_MASK_CASE(15)
#undef STANLI_PD_MASK_CASE
      default:  // no argument wants a partial; the caller skips the call
        break;
    }
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
#define STANLI_PD_CASE(opc, fn, arity, tier) \
  case kId_##fn:                             \
    return call_with<arity>(                 \
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

bool program_density_partials(int id, unsigned mask, const double* args,
                              double* partials) {
  const int n = program_density_arity(id);
  sink s;
  for (int k = 0; k < n; ++k) {
    s.buf[k] = (mask >> k) & 1u ? &partials[k] : nullptr;
    s.len[k] = 1;
  }
  sink_scope active(s);
  switch (id) {
#define STANLI_PD_PARTIALS(opc, fn, arity, tier)                          \
  case kId_##fn:                                                          \
    with_mask<arity, density_tier(tier)>(mask, [&](auto m) {              \
      record_probability_call([&] {                                       \
        return call_rvar<arity, m.value>(                                 \
            [](const auto&... x) { return stan::math::fn<false>(x...); }, \
            args);                                                        \
      });                                                                 \
    });                                                                   \
    break;
    STANLI_SCALAR_DENSITY_LIST(STANLI_PD_PARTIALS)
#undef STANLI_PD_PARTIALS
    default:
      break;
  }
  return s.deposited;
}

}  // namespace stanli
