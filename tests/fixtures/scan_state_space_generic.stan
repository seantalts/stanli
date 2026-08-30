// A compact scheduled state-space recurrence.  Data select reset and
// continuation rows in a deliberately nonalternating pattern.  The local
// group alias selects numeric data, while selecting parameters through that
// alias is an explicit near miss for scheduled lowering.
data {
  int<lower=2> T;
  int<lower=1> G;
  array[T] int<lower=0, upper=1> restart;
  array[T] int<lower=0, upper=1> enabled;
  array[T] int<lower=1, upper=G> cohort;
  vector[T] timestamps;
  real<lower=1e-12> max_step;
  matrix[G, 1] predictors;
  vector[T] observed;
  int<lower=0, upper=1> indexed_effects;
  array[T] int<lower=0, upper=2> fingerprint;
}
parameters {
  real drift;
  real initial_level;
  real initial_memory;
  real<lower=0> sigma;
  vector[G] group_effect;
}
model {
  real level = initial_level;
  real memory = initial_memory;
  real transition = drift;
  array[T] real row_score;

  for (cursor in 0 : (T - 1)) {
    if (cursor == 0) {
      level = initial_level;
      memory = initial_memory;
      transition = drift;
    } else {
      int group_key;
      group_key = cohort[cursor + 1];
      if (enabled[cursor + 1]) {
        if (restart[cursor + 1]) {
          level = initial_level + transition * predictors[group_key, 1];
          memory = initial_memory;
          transition = drift + 0.1 * predictors[group_key, 1];
        } else {
          real elapsed = 0;
          real delta = timestamps[cursor + 1] - timestamps[cursor];
          real slice = delta / ceil(delta / max_step);
          while (elapsed < delta - 1e-10) {
            elapsed += slice;
            level += slice * transition;
          }
          memory = 0.75 * memory + level;
          transition = 0.9 * transition + drift;
        }
        if (indexed_effects)
          level += group_effect[group_key];
        else
          level += drift * predictors[group_key, 1];
        if (fingerprint[cursor + 1] == 1)
          memory += 0.125 * drift;
        else if (fingerprint[cursor + 1] == 2)
          memory -= 0.25 * drift;
      }
    }
    row_score[cursor + 1]
        = normal_lpdf(observed[cursor + 1] | level + memory, sigma);
  }
  target += sum(row_score);
}
