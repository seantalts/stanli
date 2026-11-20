// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 0, "lo": [], "hi": [], "s": [], "o": [], "after": -3}
// Every elementwise transform, and cumulative_sum, over zero elements, with
// zero-size bounds, offsets and multipliers.
data { int N; vector[N] lo; vector[N] hi; vector[N] s; vector[N] o; real after; }
parameters {
  vector<lower=lo>[N] a;
  vector<upper=hi>[N] b;
  vector<lower=lo, upper=hi>[N] c;
  vector<lower=lo, upper=1>[N] c2;
  vector<lower=-1, upper=hi>[N] c3;
  vector<offset=o, multiplier=s>[N] d;
  vector<multiplier=s>[N] d2;
  vector<offset=o>[N] d3;
  ordered[N] e;
  positive_ordered[N] f;
  real t;
}
model {
  t ~ normal(0, 1);
  target += sum(cumulative_sum(a)) + sum(cumulative_sum(d)) + sum(c) + sum(c2) + sum(c3) + sum(b) + sum(d2) + sum(d3) + sum(e) + sum(f);
}
