// run_nuts initialization: the retry over random starts, and the failures
// when no start is finite. test_sampler_parity owns the sampled draws.
#include <stanli/nuts.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void expect_in(const std::string& what, double got, double lo,
                      double hi) {
  if (!(got >= lo && got <= hi)) {
    ++failures;
    std::printf("FAIL %-18s got %.6g want in [%g, %g]\n", what.c_str(), got, lo,
                hi);
  }
}

int main() {
  using namespace stanli;

  // ---- initialization retry (the accel_gp class of failure) --------------
  // The target is finite only for x < 0 (it evaluates log(-x)), and
  // CmdStan's init draws uniform(-2, 2). boost::ecuyer1988's first output
  // for every small seed is the top of the range, +2.000, so EVERY seed's
  // first draw lands in the dead half and the retry path is provably
  // exercised. Before the retry landed, all of these failed outright, the
  // way accel_gp did on 3 of 5 seeds.
  {
    for (uint32_t seed = 1; seed <= 8; ++seed) {
      Graph g;
      const int x = g.add_slot(1, true);
      const int nx = g.add_slot(1, false);
      g.add_op(OP_NEG, {x}, nx);
      const int lx = g.add_slot(1, false);
      g.add_op(OP_LOGV, {nx}, lx);
      const int zero = g.add_slot(1, false);
      const int one = g.add_slot(1, false);
      const int lp = g.add_slot(1, false);
      const int id = g.add_op(OP_NORMAL_LPDF, {lx, zero, one}, lp);
      g.ops[(size_t)id].variant = 0x01;  // outcome active
      g.result_slot = lp;
      Executor ex(std::move(g));
      ex.value_ptr(zero)[0] = 0.0;
      ex.value_ptr(one)[0] = 1.0;

      NutsConfig cfg;
      cfg.seed = seed;
      cfg.warmup = 100;
      cfg.samples = 50;
      try {
        auto draws = run_nuts(ex, cfg);
        expect_in("init-retry draws seed " + std::to_string(seed),
                  (double)draws.size(), cfg.samples, cfg.samples);
        for (const auto& q : draws)
          if (!(q[0] < 0.0)) {
            ++failures;
            std::printf("FAIL init-retry seed %u drew x >= 0\n", seed);
            break;
          }
      } catch (const std::exception& e) {
        ++failures;
        std::printf("FAIL init-retry seed %u threw: %s\n", seed, e.what());
      }
    }
  }
  // A target that is non-finite EVERYWHERE must fail with the clear
  // initialization error, not a stepsize mystery.
  {
    Graph g;
    const int x = g.add_slot(1, true);
    const int nanc = g.add_slot(1, false);
    const int one = g.add_slot(1, false);
    const int lp = g.add_slot(1, false);
    const int id = g.add_op(OP_NORMAL_LPDF, {x, nanc, one}, lp);
    g.ops[(size_t)id].variant = 0x01;
    g.result_slot = lp;
    Executor ex(std::move(g));
    ex.value_ptr(nanc)[0] = std::nan("");
    ex.value_ptr(one)[0] = 1.0;
    NutsConfig cfg;
    cfg.seed = 7;
    cfg.warmup = 10;
    cfg.samples = 5;
    bool threw_init = false;
    try {
      run_nuts(ex, cfg);
    } catch (const std::exception& e) {
      threw_init =
          std::string(e.what()).find("initialization") != std::string::npos;
    }
    if (!threw_init) {
      ++failures;
      std::printf("FAIL all-NaN target: wanted the initialization error\n");
    }
  }
  // A target that throws must report what it threw, not just that no draw
  // was finite.
  {
    for (double radius : {2.0, 0.0}) {
      Graph g;
      const int x = g.add_slot(1, true);
      const int zero = g.add_slot(1, false);
      const int bad = g.add_slot(1, false);
      const int lp = g.add_slot(1, false);
      const int id = g.add_op(OP_NORMAL_LPDF, {x, zero, bad}, lp);
      g.ops[(size_t)id].variant = 0x01;
      g.result_slot = lp;
      Executor ex(std::move(g));
      ex.value_ptr(zero)[0] = 0.0;
      ex.value_ptr(bad)[0] = -1.0;
      NutsConfig cfg;
      cfg.seed = 11;
      cfg.warmup = 10;
      cfg.samples = 5;
      cfg.init_radius = radius;
      std::string what;
      try {
        run_nuts(ex, cfg);
      } catch (const std::exception& e) {
        what = e.what();
      }
      const std::string tag =
          "throwing target radius " + std::to_string(radius);
      if (what.find("initialization failed") == std::string::npos ||
          what.find("Scale parameter") == std::string::npos) {
        ++failures;
        std::printf("FAIL %s: got \"%s\"\n", tag.c_str(), what.c_str());
      }
    }
  }

  if (failures == 0) std::printf("test_nuts OK\n");
  return failures == 0 ? 0 : 1;
}
