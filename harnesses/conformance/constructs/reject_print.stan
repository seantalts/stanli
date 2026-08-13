parameters {
  real theta;
}
model {
  print("construct theta = ", theta);
  if (theta < 0) {
    reject("negative construct theta: ", theta);
  }
  target += theta;
}
