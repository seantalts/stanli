// An all-data propto density with no differentiable edge. Preparation still
// evaluates it, so Stan Math's support checks run (tests/test_lower.cpp).
data {
  int n;
  int N;
  int a;
  int b;
}
model {
  target += hypergeometric_lupmf(n | N, a, b);
}
