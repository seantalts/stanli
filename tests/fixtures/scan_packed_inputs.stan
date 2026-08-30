// Scheduled-scan input packing: one carry, seven active scalar invariants,
// and two streamed inactive row columns exceed Op::in[6]. Lowering must keep
// active invariants and inactive data in separate descriptors so row geometry
// never acquires parameter adjoints from descriptor-level cohabitation.
data {
  int<lower=1> N;
  array[N] int<lower=0, upper=1> new_subject;
  array[N] real row;
  array[N] real<lower=0> jitter;
  real gain;
  real shift;
}
parameters {
  real initial_state;
  real a;
  real b;
  real c;
  real d;
  real e;
  real f;
  real g;
}
model {
  real state;
  array[N] real llrow;

  for (i in 0 : (N - 1)) {
    if (i == 0) {
      state = initial_state;
    } else {
      if (new_subject[i + 1] == 1)
        state = a * state + b + c + d + e + f + g + gain + shift
                + row[i + 1];
      else
        state = g * state + f + e + d + c + b + a + gain - shift
                + row[i + 1];
    }
    llrow[i + 1] = normal_lpdf(row[i + 1] | state, 1 + jitter[i + 1]);
  }
  target += sum(llrow);
}
