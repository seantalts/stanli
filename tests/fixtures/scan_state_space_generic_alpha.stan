// A mechanical alpha-renaming of scan_state_space_generic.stan.  Independent
// model-local carry declarations are also permuted to ensure lowering is not
// accidentally coupled to declaration order.
data {
  int<lower=2> n_steps;
  int<lower=1> n_groups;
  array[n_steps] int<lower=0, upper=1> boundary;
  array[n_steps] int<lower=0, upper=1> usable;
  array[n_steps] int<lower=1, upper=n_groups> membership;
  vector[n_steps] moments;
  real<lower=1e-12> slice_limit;
  matrix[n_groups, 1] design;
  vector[n_steps] measurement;
  int<lower=0, upper=1> choose_indexed;
  array[n_steps] int<lower=0, upper=2> path_mark;
}
parameters {
  real velocity;
  real origin;
  real seed_cache;
  real<lower=0> scale;
  vector[n_groups] random_offset;
}
model {
  array[n_steps] real pieces;
  real coefficient = velocity;
  real history = seed_cache;
  real position = origin;

  for (tick in 0 : (n_steps - 1)) {
    if (tick == 0) {
      position = origin;
      history = seed_cache;
      coefficient = velocity;
    } else {
      int bucket;
      bucket = membership[tick + 1];
      if (usable[tick + 1]) {
        if (boundary[tick + 1]) {
          position = origin + coefficient * design[bucket, 1];
          history = seed_cache;
          coefficient = velocity + 0.1 * design[bucket, 1];
        } else {
          real progress = 0;
          real span = moments[tick + 1] - moments[tick];
          real increment = span / ceil(span / slice_limit);
          while (progress < span - 1e-10) {
            progress += increment;
            position += increment * coefficient;
          }
          history = 0.75 * history + position;
          coefficient = 0.9 * coefficient + velocity;
        }
        if (choose_indexed)
          position += random_offset[bucket];
        else
          position += velocity * design[bucket, 1];
        if (path_mark[tick + 1] == 1)
          history += 0.125 * velocity;
        else if (path_mark[tick + 1] == 2)
          history -= 0.25 * velocity;
      }
    }
    pieces[tick + 1]
        = normal_lpdf(measurement[tick + 1] | position + history, scale);
  }
  target += sum(pieces);
}
