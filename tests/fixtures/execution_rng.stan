transformed data { real shape = 2; }
parameters { real<lower=0> rate; }
model { rate ~ normal(0, 1); }
generated quantities {
  array[2] real draw;
  {
    // Runtime-sized local storage deliberately exercises output fallback.
    array[rate > 0 ? 2 : 3] real temporary = gamma_rng(rep_vector(rate, rate > 0 ? 2 : 3), 1.5);
    draw = temporary[1:2];
  }
}
