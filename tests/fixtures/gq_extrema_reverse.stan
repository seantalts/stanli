parameters {
  vector[2] x;
}
model {
  target += min(x);
}
