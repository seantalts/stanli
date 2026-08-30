// Complete alpha-renaming of scan_normalize_generic.stan.  The independent
// score/state declarations are permuted as an additional ordering check.
data {
  int<lower=2> width;
  array[width] int<lower=0, upper=1> admitted;
  array[width] int<lower=0, upper=1> renewal;
  vector[width] instants;
  vector[width] forcing;
  vector[width] response;
}
parameters {
  real slope;
  real origin;
  real<lower=0> noise;
}
model {
  array[width] real portions;
  real position = origin;
  for (place in 1 : width)
    portions[place] = 0;
  int predecessor = 0;

  for (epoch in 0 : width) {
    int location;
    location = epoch ? epoch : 1;
    if (epoch == 0 || admitted[location]) {
      if (epoch == 0)
        position = origin;
      else if (renewal[location])
        position = origin + slope * forcing[location];
      else
        position = 0.8 * position
                   + slope * forcing[location]
                   + instants[location] - instants[predecessor];
      if (epoch > 0) {
        portions[location] += normal_lpdf(response[location] | position, noise);
        portions[location] += normal_lpdf(0 | position, 2);
      }
    }
    predecessor = epoch;
  }
  target += sum(portions);
}
