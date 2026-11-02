// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data {
  matrix[2, 2] A = [[2, 1], [1, 3]];
  matrix[3, 2] T = [[1, 2], [3, 4], [5, 7]];
  matrix[2, 2] B = [[1, 2], [0.5, -1]];
  vector[2] v = [1, 2]';
  if (abs(determinant(A) - 5) > 1e-12) reject("determinant");
  if (abs(log_determinant_spd(A) - log(5)) > 1e-12) reject("log_determinant_spd");
  if (max(abs(to_vector(chol2inv(cholesky_decompose(A)) - inverse(A)))) > 1e-12)
    reject("chol2inv");
  if (max(abs(to_vector(T * generalized_inverse(T) * T - T))) > 1e-10)
    reject("generalized_inverse");
  if (max(abs(to_vector(qr_Q(T) * qr_R(T) - T))) > 1e-10) reject("qr");
  if (max(abs(to_vector(qr_thin_Q(T) * qr_thin_R(T) - T))) > 1e-10)
    reject("qr_thin");
  if (rows(qr_Q(T)) != 3 || cols(qr_Q(T)) != 3 || rows(qr_R(T)) != 3
      || rows(qr_thin_Q(T)) != 3 || cols(qr_thin_Q(T)) != 2
      || rows(qr_thin_R(T)) != 2 || cols(qr_thin_R(T)) != 2)
    reject("qr shapes");
  if (max(abs(to_vector(matrix_power(A, 3) - A * A * A))) > 1e-12)
    reject("matrix_power");
  if (abs(trace_quad_form(A, B) - sum(diagonal(B' * A * B))) > 1e-12)
    reject("trace_quad_form");
  if (abs(trace_quad_form(A, v) - v' * A * v) > 1e-12)
    reject("trace_quad_form vector");
  if (abs(trace_gen_quad_form(B, A, B) - sum(diagonal(B * B' * A * B))) > 1e-12)
    reject("trace_gen_quad_form");
  if (abs(trace_dot(A, B) - sum(diagonal(A * B))) > 1e-12) reject("trace_dot");
  if (max(abs(to_vector(matrix_exp_multiply(A, B) - matrix_exp(A) * B))) > 1e-10)
    reject("matrix_exp_multiply");
  if (max(abs(to_vector(scale_matrix_exp_multiply(0.3, A, B)
                        - matrix_exp(0.3 * A) * B))) > 1e-10)
    reject("scale_matrix_exp_multiply");
  if (abs(norm1(v) - 3) > 1e-12 || abs(norm2(v) - sqrt(5)) > 1e-12)
    reject("norm");
  if (abs(distance(v, [4, 6]') - 5) > 1e-12) reject("distance");
}
parameters {
  matrix[2, 2] P;
  vector[2] w;
}
model {
  matrix[2, 2] S = crossprod(P) + diag_matrix(rep_vector(1.0, 2));
  matrix[3, 2] M = [[P[1, 1], 2], [3, P[2, 2]], [5, w[1]]];
  matrix[2, 2] I2 = diag_matrix(rep_vector(1.0, 2));
  if (abs(determinant(S) - (S[1, 1] * S[2, 2] - S[1, 2] * S[2, 1])) > 1e-9)
    reject("determinant param");
  if (abs(log_determinant_spd(S) - log_determinant(S)) > 1e-9)
    reject("log_determinant_spd param");
  if (max(abs(to_vector(chol2inv(cholesky_decompose(S)) - inverse(S)))) > 1e-9)
    reject("chol2inv param");
  if (max(abs(to_vector(M * generalized_inverse(M) * M - M))) > 1e-8)
    reject("generalized_inverse param");
  if (max(abs(to_vector(qr_Q(M) * qr_R(M) - M))) > 1e-8) reject("qr param");
  if (max(abs(to_vector(qr_thin_Q(M) * qr_thin_R(M) - M))) > 1e-8)
    reject("qr_thin param");
  if (max(abs(to_vector(matrix_power(S, 2) - S * S))) > 1e-8)
    reject("matrix_power param");
  if (abs(trace_quad_form(S, P) - sum(diagonal(P' * S * P))) > 1e-8)
    reject("trace_quad_form param");
  if (abs(trace_quad_form(S, w) - w' * S * w) > 1e-8)
    reject("trace_quad_form vector param");
  if (abs(trace_gen_quad_form(P, S, P) - sum(diagonal(P * P' * S * P))) > 1e-8)
    reject("trace_gen_quad_form param");
  if (abs(trace_dot(S, P) - sum(diagonal(S * P))) > 1e-8) reject("trace_dot param");
  if (max(abs(to_vector(matrix_exp_multiply(S, I2) - matrix_exp(S)))) > 1e-6)
    reject("matrix_exp_multiply param");
  if (max(abs(to_vector(scale_matrix_exp_multiply(w[1], I2, P)
                        - matrix_exp(w[1] * I2) * P))) > 1e-8)
    reject("scale_matrix_exp_multiply param");
  if (abs(norm1(w) - (abs(w[1]) + abs(w[2]))) > 1e-9) reject("norm1 param");
  if (abs(norm2(w) - sqrt(dot_self(w))) > 1e-9) reject("norm2 param");
  if (abs(distance(w, [0, 0]') - sqrt(dot_self(w))) > 1e-9)
    reject("distance param");
  target += -0.5 * dot_self(to_vector(P)) - 0.5 * dot_self(w);
}
