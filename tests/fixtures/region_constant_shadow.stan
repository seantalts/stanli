parameters {
  real theta;
}
model {
  {
    real k = 0;
    target += k;
  }
  if (theta > 0)
    for (k in 1:2)
      if (k > 0.5) target += theta;
  {
    real m = 0;
    target += m;
  }
  if (theta > 0) {
    real m = theta;
    if (m > 0.05) target += m;
  }
}
