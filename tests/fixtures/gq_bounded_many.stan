parameters {
  real a; real b; real c; real d;
  real e; real f; real g; real h;
}
generated quantities {
  real first;
  real second;
  {
    array[a > 0 ? 0 : 2] real left;
    array[b > 0 ? 3 : 1] real right;
    for (i in 1:size(left)) left[i] = a + b + c + d;
    for (j in 1:size(right)) right[j] = e + f + g + h;
    first = sum(left);
    second = sum(right);
  }
}
