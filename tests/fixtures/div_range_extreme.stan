parameters {
  vector[2] a;
  vector[2] b;
  real q;
}
model {
  real take = q > 10 ? 1 : 0;
  real y = a[1];
  vector[2] av;
  for (i in 1:40)
    y = y * 1.00001 + a[1];
  av[1] = y;
  av[2] = a[2];
  {
    vector[2] z = av ./ b;
    y = z[1];
    for (i in 1:40)
      y = y * 1.00001 + 0.001;
    target += take * (y + z[2]);
  }
}
