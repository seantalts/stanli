data {
  vector<lower=0>[2] observed;
}
parameters {
  real theta;
}
transformed parameters {
  vector<lower=0>[2] checked;
  checked[1] = 1;
  checked[2] = theta;
}
model {
  target += normal_lpdf(theta | observed[1] + observed[2], 1);
}
