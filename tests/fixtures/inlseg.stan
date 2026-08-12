// An inlined callee whose declaration sizes query the shape of a computed
// argument: --O1 turns `rows(beta)` into `rows(segment(beta, 1, 2))` at
// the call site, so size evaluation must answer shape queries on
// expressions, not just variables.
functions {
  real csum(vector beta) {
    vector[rows(beta) + 1] u;
    u = append_row(rep_vector(0.0, 1), beta);
    return sum(cumulative_sum(u));
  }
}
data {
  int N;
}
parameters {
  vector[N] beta;
}
model {
  target += csum(segment(beta, 1, 2));
  beta ~ normal(0, 1);
}
