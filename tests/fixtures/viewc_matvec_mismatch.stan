data { matrix[2, 3] X; }
parameters { vector[2] b; }
model { target += sum(X * b); }
