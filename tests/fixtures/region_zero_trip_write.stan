// The else arm writes v only inside a loop over an empty array, so the
// region compiles no write to v and has no live-out. v keeps its value.
parameters {
  real theta;
}

transformed parameters {
  vector[1] v = [1.5]';
  if (theta <= -0.25) {
    real unused = v[1];
  } else {
    for (e in rep_array(0, 0)) {
      v *= 0.5;
    }
  }
}

model {
  target += v[1] * theta;
}
