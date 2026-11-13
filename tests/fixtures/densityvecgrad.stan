// unsupported_normal_lpdf_container, with a parameter feeding one of the
// container arguments: the density CALL has to run correctly under both the
// register machine's forward pass and its var-replay backward pass, not
// just reproduce a data-only constant lp.
data {
  int<lower=1> N;
  vector[N] y;
  row_vector<lower=0>[N] sigma;
}
parameters {
  real a;
}
model {
  row_vector[N] mu = rep_row_vector(a, N);
  int i = 1;
  while (i <= N) {
    target += normal_lpdf(y | mu, sigma);
    i += 1;
  }
  a ~ std_normal();
}
