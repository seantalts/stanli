parameters {
  real x;
}
transformed parameters {
  real s = 0;
  for (iteration in 1:32) {
    int n = x > 0 ? 1 : (x < 0 ? 2 : 0);
    array[n] real a;
    // The array retains its declaration-time extent after n changes.
    n = 0;
    for (i in 1:size(a)) a[i] = x;
    s += sum(a) + size(a) + num_elements(a);
  }
}
model {
  x ~ std_normal();
  target += s;
}
