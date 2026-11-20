// mean() adds its terms the way Stan Math's Eigen reduction does: in order
// for autodiff values and for strided data views, in packets for contiguous
// data. With these values the two orders give 0.25 and 0.5.
data {
  vector[4] d;
  array[4] real a;
  matrix[2, 4] M;
}
parameters {
  real t;
}
transformed parameters {
  vector[4] y = t * 0 + d;
  matrix[2, 4] Y = t * 0 + M;
}
model {
  target += mean(y) + 2 * mean(M[1]) + 4 * mean(Y[1]);
  target += 8 * mean(segment(y, 1, 4)) + 16 * mean(Y) + 32 * mean(d);
  target += 64 * mean(a);
}
generated quantities {
  real m_d = mean(d);
  real m_a = mean(a);
  real m_y = mean(y);
  real m_row = mean(M[1]);
  real m_M = mean(M);
}
