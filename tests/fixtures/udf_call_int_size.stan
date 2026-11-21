functions {
  int half(int a) {
    int i = 0;
    while (2 * (i + 1) <= a) i += 1;
    return i;
  }
  vector first_half(vector v) {
    vector[half(num_elements(v))] h = v[1:half(num_elements(v))];
    return h;
  }
}
data { int N; }
parameters { vector[N] mu; }
model { target += normal_lpdf(first_half(mu) | 0, 1); }
