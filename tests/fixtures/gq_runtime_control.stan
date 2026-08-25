parameters {
  vector[2] initial;
  array[2] simplex[2] transition;
  array[2] vector[2] choice;
  vector<lower=0>[2] scale;
  real aux1;
  real aux2;
  real aux3;
  real aux4;
  real aux5;
  real aux6;
}
generated quantities {
  int state = categorical_rng(softmax(initial));
  vector[2] chosen = choice[state];
  real chosen_scale = scale[state];
  array[2] real static_branch_draw;
  {
    // The condition is data-only because the loop variable is known while
    // unrolling. It must not turn the surrounding RNG block into a runtime
    // region merely because a pre-scan runs before the loop.
    for (t in 1 : 2) {
      if (t < 2) {
        static_branch_draw[t] = normal_rng(0, 1);
      } else {
        static_branch_draw[t] = normal_rng(1, 1);
      }
    }
  }
  array[4] int path;
  real score;
  {
    array[4, 2] int back;
    array[4, 2] real best;
    for (k in 1 : 2) {
      best[1, k] = initial[k];
    }
    for (t in 2 : 4) {
      for (k in 1 : 2) {
        best[t, k] = negative_infinity();
        for (j in 1 : 2) {
          real candidate = best[t - 1, j] + log(transition[j, k]);
          if (candidate > best[t, k]) {
            back[t, k] = j;
            best[t, k] = candidate;
          }
        }
      }
    }
    score = max(best[4]);
    for (k in 1 : 2) {
      if (best[4, k] == score) {
        path[4] = k;
      }
    }
    for (t in 1 : 3) {
      path[4 - t] = back[5 - t, path[5 - t]];
    }
    // Keep more than six graph values live across the region boundary.  The
    // island input-packing path must preserve each value and its offset.
    score += aux1 + aux2 + aux3 + aux4 + aux5 + aux6;
  }
}
