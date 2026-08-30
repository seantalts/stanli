// A scheduled recurrence with independent exact-one and zero-or-one loops.
// The third loop's entry depends on the inner iterator; only its backedge can go.
// Data switches expose multi-trip, early-continue, and guard-read near misses.
data {
  int<lower=34> N;
  int<lower=1> L;
  int<lower=0, upper=1> early_exit;
  int<lower=0, upper=1> unsafe_read;
  int<lower=1, upper=2> limit_a;
  int<lower=1, upper=2> limit_b;
  vector[N] observed;
}
parameters {
  real theta;
}
model {
  real state = 0;
  array[N] real row_score;
  for (row in 0 : (N - 1)) {
    if (row == 0) {
      state = theta;
    } else {
      for (lane in 1:L) {
        int cursor_a = 0;
        int found_a = 0;
        while (found_a == 0 && cursor_a < limit_a) {
          cursor_a += 1;
          found_a += cursor_a == limit_a;
          state += theta * (0.0009765625 * lane);
        }

        int cursor_b = 0;
        int found_b = 0;
        while (found_b == 0 && cursor_b < limit_b) {
          cursor_b += 1;
          found_b += cursor_b == limit_b;
          state -= theta * (0.00048828125 * lane);
        }

        int cursor_c = 0;
        int found_c = lane == 2;
        while (found_c == 0 && cursor_c < 1) {
          cursor_c += 1;
          found_c += lane == L;
          state += theta * (0.001953125 * lane);
        }

        if (early_exit) {
          int cursor_d = 0;
          while (cursor_d < 2) {
            cursor_d += 1;
            if (theta > 0) continue;
            state += theta * 0.00390625;
            cursor_d += 1;
          }
        }

        if (unsafe_read) {
          array[1] int accepted = {0};
          int cursor_e = 0;
          while (accepted[cursor_e + 1] == 0 && cursor_e < 1) {
            cursor_e += 1;
            state += theta * 0.0078125;
          }
        }
      }
    }
    row_score[row + 1] = normal_lpdf(observed[row + 1] | state, 1);
  }
  target += sum(row_score);
}
