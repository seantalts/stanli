functions {
  int count_lp(real y) {
    target += y;
    return 2;
  }
}
parameters {
  real p;
}
model {
  if (count_lp(p)) ; else ;
  for (i in count_lp(p) : 3) ;
}
