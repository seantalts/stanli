data {
  int N;
  array[N] int y;
}
parameters {
  real mu;
  vector[3] thres;
}
model {
  for (n in 1:N) {
    int k = 1;
    real lp = 0;
    while (k <= min(y[n], num_elements(thres))) {
      lp += log1m_inv_logit(thres[k] - mu);
      k += 1;
    }
    target += lp;
  }
}
