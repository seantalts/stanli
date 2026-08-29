functions {
  vector choose(vector x) {
    int changed = 0;
    vector[2] out = x;
    if (x[1] > 0) {
      changed = 1;
      out[1] = 2 * x[1];
    }
    if (changed) {
      out[2] += x[1];
      return out;
    } else {
      return x;
    }
  }
}
parameters {
  vector[2] x;
}
model {
  target += sum(choose(x));
}
generated quantities {
  vector[2] chosen = choose(x);
}
