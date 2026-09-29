parameters { real<lower=0> rate; }
model { rate ~ normal(0, 1); }
generated quantities {
  array[2] real draw;
  {
    array[rate > 0 ? 2 : 3] real temporary = gamma_rng(rep_vector(-1, rate > 0 ? 2 : 3), 1.5);
    draw = temporary[1:2];
  }
}
