// Developer-only two-by-two kernel-linking fixture; not a model compiler.
__attribute__((import_module("stanli"), import_name("forward"))) extern void
kernel_forward(unsigned);
__attribute__((import_module("stanli"), import_name("reverse"))) extern void
kernel_reverse(unsigned);
void forward(unsigned handle, double* matrix, double x) {
  matrix[0] = x * x + 1.0;
  matrix[1] = 0.2;
  matrix[2] = 0.2;
  matrix[3] = 3.0;
  kernel_forward(handle);
}
double reverse(unsigned handle, double* adjoints, double x) {
  kernel_reverse(handle);
  return (2.0 * x) * adjoints[0];
}
// Isolate the extra Wasm-to-Wasm call boundary from generated arithmetic.
void raw_forward(unsigned handle) { kernel_forward(handle); }
