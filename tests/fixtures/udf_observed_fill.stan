functions {
  array[] int whichequals(array[] int x, int test) {
    array[size(x)] int check;
    array[size(x)] int which;
    int count = 0;
    for (i in 1:size(x)) {
      check[i] = x[i] != test;
      if (check[i]) {
        count += 1;
        which[count] = i;
      }
    }
    return which[1:count];
  }

  real active_pick(real x) {
    return x > 0 ? 2 * x : 4 * x;
  }
}

parameters {
  real theta;
}

model {
  array[3] int graph_local_fill;
  array[3] int selected = whichequals(graph_local_fill, 0);
  target += sum(selected) * theta;
  target += active_pick(theta);
}
