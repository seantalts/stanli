parameters {
  real theta;
}
model {
  {
    real m = 1;
    target += m;
  }
  if (theta > 0) {
    {
      real m = 5 * theta;
      target += m;
    }
    {
      real m;
      if (theta > 0.5)
        m = theta;
      else
        m = 2 * theta;
      target += m;
    }
  }
  {
    real m = 3;
    target += m;
  }
}
