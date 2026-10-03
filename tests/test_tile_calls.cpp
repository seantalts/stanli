// A CALL kernel may run once over a tile's active lanes only if its
// elementwise form computes, lane for lane, exactly what the scalar call
// computes: same value, same scratch partials, same adjoints, same
// exception. This runs every kernel the lane executor would batch through
// both forms on random and edge inputs and compares bits.
#include <stanli/kernel_types.hpp>
#include <stanli/optable.hpp>
#include <stanli/tile_call.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <typeinfo>
#include <vector>

namespace {

using stanli::Desc;
using stanli::KernelCtx;

int failures = 0;
int checks = 0;
int threw_trials = 0;
int clean_trials = 0;

void check(bool ok, const std::string& what) {
  ++checks;
  if (!ok) {
    ++failures;
    if (failures <= 40) std::printf("FAIL %s\n", what.c_str());
  }
}

bool same_bits(double a, double b) {
  return std::memcmp(&a, &b, sizeof(double)) == 0;
}

bool same_value(double a, double b) {
  return (std::isnan(a) && std::isnan(b)) || same_bits(a, b);
}

struct Entry {
  uint16_t opcode;
  const char* name;
  int nargs;
};

std::vector<Entry> density_entries() {
  std::vector<Entry> v;
#define STANLI_TEST_ENTRY(code, fn, n, tier) \
  v.push_back({stanli::code, #fn, n});
  STANLI_SCALAR_DENSITY_LIST(STANLI_TEST_ENTRY)
#undef STANLI_TEST_ENTRY
  return v;
}

struct Rng {
  std::mt19937_64 gen;
  explicit Rng(uint64_t seed) : gen(seed) {}
  double u() { return std::uniform_real_distribution<double>(0.0, 1.0)(gen); }
  size_t pick(size_t n) { return (size_t)(gen() % n); }
  double value() {
    static const double edges[] = {
        0.0,
        -0.0,
        1.0,
        -1.0,
        0.5,
        2.0,
        1e-300,
        1e300,
        -1e300,
        1e-8,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max(),
        700.0,
        -700.0,
    };
    const double r = u();
    if (r < 0.2) return edges[pick(sizeof(edges) / sizeof(edges[0]))];
    if (r < 0.6) return 0.02 + 5.0 * u();
    if (r < 0.75) return u();
    return std::normal_distribution<double>(0.0, 3.0)(gen);
  }
};

struct Outcome {
  bool threw = false;
  std::string what;
};

template <typename F>
Outcome attempt(F&& f) {
  Outcome o;
  try {
    f();
  } catch (const std::exception& e) {
    o.threw = true;
    o.what = std::string(typeid(e).name()) + ": " + e.what();
  }
  return o;
}

constexpr double kSentinel = 7.25;

void test_kernel(const Entry& e, uint8_t scalar_variant, int lanes, Rng& rng) {
  using namespace stanli;
  const Kernel* k = find_kernel(e.opcode);
  const std::string tag = std::string(e.name) +
                          " variant=" + std::to_string((int)scalar_variant) +
                          " lanes=" + std::to_string(lanes);
  if (!k || !k->forward || !k->backward) {
    check(false, tag + ": kernel registered");
    return;
  }
  const int n = e.nargs;
  const int n_scratch = n + 1;
  const uint8_t eltv =
      tile_call_variant(TileCallKind::Density, scalar_variant, n);

  std::vector<std::vector<double>> in((size_t)n);
  std::vector<bool> shared((size_t)n);
  for (int a = 0; a < n; ++a) {
    shared[(size_t)a] = rng.u() < 0.3;
    const int len = shared[(size_t)a] ? 1 : lanes;
    for (int l = 0; l < len; ++l) in[(size_t)a].push_back(rng.value());
  }
  const auto arg = [&](int a, int l) {
    return in[(size_t)a][shared[(size_t)a] ? 0 : (size_t)l];
  };

  std::vector<double> out(lanes, 0.0);
  std::vector<std::vector<double>> scratch(
      (size_t)lanes, std::vector<double>((size_t)n_scratch, kSentinel));
  std::vector<Outcome> fwd((size_t)lanes);
  for (int l = 0; l < lanes; ++l) {
    double args[6];
    for (int a = 0; a < n; ++a) args[a] = arg(a, l);
    KernelCtx ctx;
    ctx.n_in = n;
    for (int a = 0; a < n; ++a) ctx.in[a] = Desc{&args[a], 1};
    ctx.out = Desc{&out[(size_t)l], 1};
    ctx.variant = scalar_variant;
    ctx.scratch = scratch[(size_t)l].data();
    fwd[(size_t)l] = attempt([&] { k->forward(ctx); });
  }

  std::vector<double> bout((size_t)lanes, kSentinel);
  std::vector<double> bscratch((size_t)n_scratch * lanes, kSentinel);
  KernelCtx bctx;
  bctx.n_in = n;
  for (int a = 0; a < n; ++a)
    bctx.in[a] = Desc{in[(size_t)a].data(), (int64_t)in[(size_t)a].size()};
  bctx.out = Desc{bout.data(), lanes};
  bctx.variant = eltv;
  bctx.scratch = bscratch.data();
  const Outcome batched = attempt([&] { k->forward(bctx); });

  const Outcome* first = nullptr;
  for (const auto& o : fwd)
    if (o.threw) {
      first = &o;
      break;
    }
  ++(first ? threw_trials : clean_trials);
  check(batched.threw == (first != nullptr),
        tag + ": batched throws iff a lane throws");
  if (first) {
    check(batched.what == first->what,
          tag + ": first failing lane's exception (" + batched.what + " vs " +
              first->what + ")");
    return;
  }
  for (int l = 0; l < lanes; ++l) {
    check(same_bits(bout[(size_t)l], out[(size_t)l]),
          tag + ": out[" + std::to_string(l) + "]");
    for (int c = 0; c < n_scratch; ++c)
      check(same_bits(bscratch[(size_t)c * lanes + l],
                      scratch[(size_t)l][(size_t)c]),
            tag + ": scratch[" + std::to_string(c) + "] lane " +
                std::to_string(l));
  }

  uint8_t adj_mask = (uint8_t)(rng.pick(1u << n));
  if (rng.u() < 0.3) adj_mask = (uint8_t)((1u << n) - 1);
  std::vector<std::vector<double>> cell((size_t)n);
  std::vector<std::vector<double>> bcell((size_t)n);
  for (int a = 0; a < n; ++a)
    for (int l = 0; l < lanes; ++l) {
      const double v = rng.u() < 0.3 ? 0.0 : rng.value();
      cell[(size_t)a].push_back(v);
    }
  bcell = cell;
  std::vector<double> seed((size_t)lanes);
  for (auto& s : seed) s = rng.u() < 0.2 ? 0.0 : rng.value();

  std::vector<std::vector<double>> want = cell;
  for (int l = 0; l < lanes; ++l) {
    double args[6], adj[6];
    for (int a = 0; a < n; ++a) {
      args[a] = arg(a, l);
      adj[a] = cell[(size_t)a][(size_t)l];
    }
    double oa = seed[(size_t)l];
    KernelCtx ctx;
    ctx.n_in = n;
    for (int a = 0; a < n; ++a) {
      ctx.in[a] = Desc{&args[a], 1};
      ctx.in_adj[a] = (adj_mask >> a) & 1 ? Desc{&adj[a], 1} : Desc{nullptr, 1};
    }
    ctx.out = Desc{&out[(size_t)l], 1};
    ctx.variant = scalar_variant;
    ctx.scratch = scratch[(size_t)l].data();
    ctx.out_adj = oa;
    ctx.out_adj_vec = Desc{&oa, 1};
    k->backward(ctx);
    for (int a = 0; a < n; ++a) want[(size_t)a][(size_t)l] = adj[a];
  }
  KernelCtx rctx;
  rctx.n_in = n;
  for (int a = 0; a < n; ++a) {
    rctx.in[a] = Desc{in[(size_t)a].data(), (int64_t)in[(size_t)a].size()};
    rctx.in_adj[a] = (adj_mask >> a) & 1 ? Desc{bcell[(size_t)a].data(), lanes}
                                         : Desc{nullptr, lanes};
  }
  rctx.out = Desc{bout.data(), lanes};
  rctx.variant = eltv;
  rctx.scratch = bscratch.data();
  rctx.out_adj = lanes == 1 ? seed[0] : 0.0;
  rctx.out_adj_vec = Desc{seed.data(), lanes};
  k->backward(rctx);
  for (int a = 0; a < n; ++a)
    for (int l = 0; l < lanes; ++l)
      check(same_bits(bcell[(size_t)a][(size_t)l], want[(size_t)a][(size_t)l]),
            tag + ": adjoint of arg " + std::to_string(a) + " lane " +
                std::to_string(l));
}

void test_density_kernels() {
  const std::vector<Entry> entries = density_entries();
  check(entries.size() >= 27, "the scalar density list has its entries");
  for (const Entry& e : entries) {
    check(stanli::tile_call_kind(e.opcode) == stanli::TileCallKind::Density,
          std::string(e.name) + " is marked for tile calls");
    if (stanli::tile_call_kind(e.opcode) != stanli::TileCallKind::Density)
      continue;
    Rng rng(0x9e3779b97f4a7c15ull ^ e.opcode);
    std::vector<uint8_t> variants = {0};
    for (unsigned m = 0; m < (1u << e.nargs); ++m) {
      variants.push_back((uint8_t)m);
      variants.push_back((uint8_t)(m | 0x80u));
    }
    for (uint8_t v : variants)
      for (int lanes : {1, 5, 64})
        for (int trial = 0; trial < 6; ++trial) test_kernel(e, v, lanes, rng);
  }
}

void test_unmarked_kernels() {
  using stanli::TileCallKind;
  check(stanli::tile_call_kind(stanli::OP_POISSON_LPMF) == TileCallKind::None,
        "an integer-outcome density is not marked");
  check(stanli::tile_call_kind(stanli::OP_NORMAL_LCDF) == TileCallKind::None,
        "a cdf is not marked");
  check(stanli::tile_call_kind(stanli::OP_ADD) == TileCallKind::None,
        "a shape-dispatched arithmetic op is not marked");
}

std::vector<Entry> unary_candidates() {
  std::vector<Entry> v;
#define STANLI_TEST_UNARY(code, fn, value, delta, topology) \
  v.push_back({stanli::code, #fn, 1});
  STANLI_SCALAR_UNARY_LIST(STANLI_TEST_UNARY)
#undef STANLI_TEST_UNARY
  v.push_back({stanli::OP_LOGIT, "logit", 1});
  v.push_back({stanli::OP_LOG1M, "log1m", 1});
  v.push_back({stanli::OP_INV_LOGIT, "inv_logit", 1});
  v.push_back({stanli::OP_EXPV, "exp", 1});
  v.push_back({stanli::OP_LOGV, "log", 1});
  v.push_back({stanli::OP_SQRT, "sqrt", 1});
  v.push_back({stanli::OP_SQUARE, "square", 1});
  v.push_back({stanli::OP_TANHV, "tanh", 1});
  v.push_back({stanli::OP_NEG, "neg", 1});
  return v;
}

bool unary_trial(const Entry& e, int lanes, bool want_adj, Rng& rng) {
  using namespace stanli;
  const Kernel* k = find_kernel(e.opcode);
  if (!k || !k->forward || !k->backward) return false;
  std::vector<double> x((size_t)lanes), seed((size_t)lanes),
      cell((size_t)lanes);
  for (int l = 0; l < lanes; ++l) {
    x[(size_t)l] = rng.u() < 0.5 ? rng.u() : rng.value();
    seed[(size_t)l] = rng.value();
    cell[(size_t)l] = rng.u() < 0.3 ? 0.0 : rng.value();
  }
  std::vector<double> out((size_t)lanes), want = cell;
  bool ok = true;
  std::optional<Outcome> first;
  for (int l = 0; l < lanes; ++l) {
    double xi = x[(size_t)l], yi = 0.0, adj = cell[(size_t)l],
           oa = seed[(size_t)l];
    KernelCtx ctx;
    ctx.n_in = 1;
    ctx.in[0] = Desc{&xi, 1};
    ctx.in_adj[0] = want_adj ? Desc{&adj, 1} : Desc{nullptr, 1};
    ctx.out = Desc{&yi, 1};
    const Outcome o = attempt([&] { k->forward(ctx); });
    if (o.threw && !first) first = o;
    if (o.threw) continue;
    out[(size_t)l] = yi;
    ctx.out_adj = oa;
    ctx.out_adj_vec = Desc{&oa, 1};
    k->backward(ctx);
    want[(size_t)l] = adj;
  }
  std::vector<double> bout((size_t)lanes, kSentinel), badj = cell, bseed = seed;
  KernelCtx ctx;
  ctx.n_in = 1;
  ctx.in[0] = Desc{x.data(), lanes};
  ctx.out = Desc{bout.data(), lanes};
  const Outcome batched = attempt([&] { k->forward(ctx); });
  if (first) return batched.threw && batched.what == first->what;
  if (batched.threw) return false;
  for (int l = 0; l < lanes; ++l)
    ok = ok && same_value(bout[(size_t)l], out[(size_t)l]);
  ctx.in_adj[0] = want_adj ? Desc{badj.data(), lanes} : Desc{nullptr, lanes};
  ctx.out_adj = bseed[0];
  ctx.out_adj_vec = Desc{bseed.data(), lanes};
  k->backward(ctx);
  for (int l = 0; l < lanes; ++l)
    ok = ok && same_value(badj[(size_t)l], want[(size_t)l]);
  return ok;
}

void test_unary_kernels() {
  int marked = 0;
  for (const Entry& e : unary_candidates()) {
    Rng rng(0x2545f4914f6cdd1dull ^ e.opcode);
    bool ok = true;
    for (int lanes : {1, 5, 64})
      for (int trial = 0; trial < 100; ++trial)
        ok = unary_trial(e, lanes, trial % 4 != 0, rng) && ok;
    const bool is_marked =
        stanli::tile_call_kind(e.opcode) == stanli::TileCallKind::Unary;
    marked += is_marked;
    check(is_marked == ok,
          std::string(e.name) + (ok ? " is bitwise equal in both forms but not "
                                      "marked"
                                    : " differs between forms but is marked"));
  }
  check(marked >= 20, "most scalar unary kernels are marked");
}

}  // namespace

int main() {
  test_density_kernels();
  test_unmarked_kernels();
  test_unary_kernels();
  check(threw_trials > 100 && clean_trials > 1000,
        "inputs cover both throwing and clean cases (" +
            std::to_string(threw_trials) + " / " +
            std::to_string(clean_trials) + ")");
  if (failures) {
    std::printf("%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("test_tile_calls: %d checks passed\n", checks);
  return 0;
}
