// `p ~ dirichlet(a)` over an array of simplexes, the shape a hierarchical
// Dirichlet is written in.
//
// The kernel took one theta vector and read the whole slot as it, so the
// vectorized form reached stan-math as a single simplex of N*K and threw
// on the length mismatch against alpha. It failed loudly, but it failed:
// only the explicit `for (n in 1:N) p[n] ~ dirichlet(a)` worked.
//
// The outcome is data here so the reference stays a plain lpdf call. That
// also pins the array[N] vector[K] data layout, which mnarr.stan covers
// for multi_normal. N and K differ on purpose.
data {
  int N;
  int K;
  array[N] vector[K] p;
}
parameters {
  vector<lower=0>[K] a;
}
model {
  p ~ dirichlet(a);
}
