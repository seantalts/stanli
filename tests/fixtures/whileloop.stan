data {
  int N;
}
parameters {
  real theta;
}
model {
  int i = 1;
  int halves = 5;
  array[2] int idx;
  real acc = 0;
  real position = 0;
  real step = 1.0 / ceil(N / 2.0);
  matrix[3, 2] m = rep_matrix(theta, 3, 2);
  while (i <= N) {
    acc += i * theta;
    i += 1;
  }
  while (position < 1) {
    acc += theta;
    position += step;
  }
  while (halves > 1) {
    halves = halves %/% 2;
    acc += halves * theta;
  }
  for (k in 1:2) {
    idx[k] = k + 1;
  }
  acc += sum(m[idx]);
  theta ~ std_normal();
  target += acc;
}
