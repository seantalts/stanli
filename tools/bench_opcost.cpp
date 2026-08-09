// Spike: decompose the per-op cost of a scalar normal_lpdf op.
//   A. executor gradient over a graph of N scalar normal_lpdf ops
//   B. same graph, forward only
//   E. executor gradient over the same topology with trivial OP_ADD ops
//   C. direct kernel forward+backward calls on a prebuilt KernelCtx
//   D. plain double math (value + 3 partials), the floor
// A-C = executor loop + ctx assembly + dispatch; C-D = recorder/sink; E is
// the executor overhead measured with a near-free kernel body.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace stanli;

static constexpr int N = 4096;
static constexpr int REPS = 2000;

// Mirrors lower.cpp's reduce_terms: chained ADD_N, 6 inputs per op.
static int reduce_terms(Graph& g, std::vector<int> terms) {
  while (terms.size() > 1) {
    std::vector<int> next;
    for (size_t i = 0; i < terms.size(); i += 6) {
      const size_t n = std::min<size_t>(6, terms.size() - i);
      if (n == 1) {
        next.push_back(terms[i]);
        continue;
      }
      const int out = g.add_slot(1, false);
      Op op;
      op.opcode = OP_ADD_N;
      op.out = out;
      op.n_in = 0;
      for (size_t k = 0; k < n; ++k) op.in[op.n_in++] = terms[i + k];
      g.ops.push_back(op);
      next.push_back(out);
    }
    terms = std::move(next);
  }
  return terms[0];
}

// N ops of `opcode(y_i, mu, sigma)` (density) or `add(y_i, mu)` (trivial),
// reduced to one scalar. Returns op count including the reduction tree.
static Graph make_graph(bool density, int* n_ops) {
  Graph g;
  const int mu = g.add_slot(1, true);
  const int sigma = g.add_slot(1, true);
  std::vector<int> terms;
  for (int i = 0; i < N; ++i) {
    const int y = g.add_slot(1, false);
    const int lp = g.add_slot(1, false);
    if (density) {
      // variant: y data, mu+sigma active, full lpdf (radon-style target+=)
      const int id = g.add_op(OP_NORMAL_LPDF, {y, mu, sigma}, lp);
      g.ops[id].variant = 0x06;
    } else {
      g.add_op(OP_ADD, {y, mu}, lp);
    }
    terms.push_back(lp);
  }
  g.result_slot = reduce_terms(g, terms);
  *n_ops = static_cast<int>(g.ops.size());
  (void)sigma;
  return g;
}

static void fill_inputs(Executor& ex) {
  ex.params_data()[0] = 0.3;  // mu
  ex.params_data()[1] = 1.2;  // sigma
  // data slots follow the params in the arena; y_i deterministic
  for (int i = 0; i < N; ++i)
    ex.params_data()[2 + 2 * i] = 0.1 * ((i % 17) - 8);
}

template <typename F>
static double time_ns(int reps, F&& f) {
  f();  // warmup
  f();
  auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < reps; ++r) f();
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

int main() {
  double sink = 0;
  std::vector<double> grad(2);

  int ops_d = 0, ops_t = 0;
  Executor exd(make_graph(true, &ops_d));
  Executor ext(make_graph(false, &ops_t));
  fill_inputs(exd);
  fill_inputs(ext);

  const double a_ns = time_ns(REPS, [&] { sink += exd.gradient(grad.data()); });
  const double b_ns = time_ns(REPS, [&] { exd.run_forward_only(); });
  const double e_ns = time_ns(REPS, [&] { sink += ext.gradient(grad.data()); });

  // C: direct kernel calls, ctx prebuilt once (what a bind-time-ctx executor
  // would do per op).
  double yv = 0.5, muv = 0.3, sigv = 1.2, lpv = 0;
  double y_adj_dummy = 0, mu_adj = 0, sig_adj = 0;
  double scratch[3] = {0, 0, 0};
  KernelCtx ctx;
  ctx.n_in = 3;
  ctx.in[0] = {&yv, 1};
  ctx.in[1] = {&muv, 1};
  ctx.in[2] = {&sigv, 1};
  ctx.out = {&lpv, 1};
  ctx.variant = 0x06;
  ctx.scratch = scratch;
  ctx.in_adj[0] = {nullptr, 1};  // y is data
  ctx.in_adj[1] = {&mu_adj, 1};
  ctx.in_adj[2] = {&sig_adj, 1};
  ctx.out_adj = 1.0;
  ctx.out_adj_vec = {&lpv, 1};
  const Kernel& k = kernel(OP_NORMAL_LPDF);
  const double c_ns = time_ns(REPS, [&] {
    for (int i = 0; i < N; ++i) {
      yv = 0.1 * ((i % 17) - 8);
      k.forward(ctx);
      k.backward(const_cast<KernelCtx&>(ctx));
      sink += lpv;
    }
  });

  // D: the math floor on plain doubles.
  const double d_ns = time_ns(REPS, [&] {
    for (int i = 0; i < N; ++i) {
      const double y = 0.1 * ((i % 17) - 8);
      const double z = (y - muv) / sigv;
      const double lp = -0.5 * z * z - std::log(sigv) -
                        0.918938533204672742;  // -0.5*log(2*pi)
      const double dmu = z / sigv;
      const double dsig = (z * z - 1.0) / sigv;
      sink += lp + dmu + dsig;
    }
  });

  std::printf("graph ops: density=%d trivial=%d (N=%d density/add ops)\n",
              ops_d, ops_t, N);
  std::printf(
      "A exec grad density : %8.1f ns total, %6.2f ns/op, %6.2f "
      "ns/density-op\n",
      a_ns, a_ns / ops_d, a_ns / N);
  std::printf("B exec fwd density  : %8.1f ns total, %6.2f ns/op\n", b_ns,
              b_ns / ops_d);
  std::printf("E exec grad trivial : %8.1f ns total, %6.2f ns/op\n", e_ns,
              e_ns / ops_t);
  std::printf("C kernel-only f+b   : %8.1f ns total, %6.2f ns/call\n", c_ns,
              c_ns / N);
  std::printf("D math floor        : %8.1f ns total, %6.2f ns/iter\n", d_ns,
              d_ns / N);
  std::printf("sink=%g\n", sink);
  return 0;
}
