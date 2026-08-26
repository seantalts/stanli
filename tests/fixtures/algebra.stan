// Legacy algebra_solver: the initial guess is allowed to be a parameter but
// has no derivative; theta is the parameter vector differentiated through
// the implicit solve.  The empty map_rect is the stanc3 mother-model edge
// case and must return vector[0] without invoking its UDF.
functions {
  vector system(vector x, vector theta, array[] real x_r, array[] int x_i) {
    vector[2] f;
    f[1] = x[1] + theta[1] * x[2] - x_r[1] * x_i[1];
    f[2] = x[2] - exp(theta[2]);
    return f;
  }

  vector never_called(vector shared, vector job, data array[] real x_r,
                      data array[] int x_i) {
    reject("empty map_rect invoked its UDF");
    return rep_vector(0.0, 1);
  }
}
data {
  array[1] real x_r;
  array[1] int x_i;
  vector[2] theta_data;
  array[0, 0] real no_real_jobs;
  array[0, 0] int no_int_jobs;
}
parameters {
  vector[2] guess;
  vector[2] theta;
}
transformed parameters {
  vector[2] z = algebra_solver(system, guess, theta, x_r, x_i);
  vector[2] z_tol =
      algebra_solver(system, guess, theta, x_r, x_i, 1e-12, 1e-10, 1000);
  vector[2] z_data = algebra_solver(system, guess, theta_data, x_r, x_i);
}
model {
  vector[0] shared;
  array[0] vector[0] no_jobs;
  target += 0.7 * z[1] - 0.2 * z[2];
  target += 0.3 * z_tol[1] + 0.4 * z_tol[2];
  target += 0.1 * sum(z_data);
  target += sum(map_rect(never_called, shared, no_jobs, no_real_jobs,
                         no_int_jobs));
}
