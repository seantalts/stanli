functions {
  real noisy(data real x) {
    print("effect ", x);
    return x;
  }
}
data { real x; }
parameters { real q; }
model { target += q + noisy(x); }
