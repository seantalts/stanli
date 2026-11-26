data { int<lower=1> N; array[N] int<lower=1, upper=4> R; }
parameters { ordered[3] c; }
model {
  c ~ normal(0, 1.5);
  vector[4] p;
  p[1] = 1 - inv_logit(-c[1]);
  p[2] = inv_logit(-c[1]) - inv_logit(-c[2]);
  p[3] = inv_logit(-c[2]) - inv_logit(-c[3]);
  p[4] = inv_logit(-c[3]);
  for (i in 1:N) R[i] ~ categorical(p);
}
