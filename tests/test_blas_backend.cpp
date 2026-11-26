// Which library the large dense kernels call, as the build was configured.
#include <stanli/dense_blas.hpp>

#include <cstdio>
#include <cstring>

#ifndef STANLI_EXPECTED_BLAS
#error "CMake passes STANLI_EXPECTED_BLAS"
#endif

int main() {
  const char* got = stanli::blas_provider();
  if (std::strcmp(got, STANLI_EXPECTED_BLAS) != 0) {
    std::printf("FAIL runtime BLAS provider is \"%s\", configured \"%s\"\n",
                got, STANLI_EXPECTED_BLAS);
    return 1;
  }
  std::printf("ok: BLAS provider %s\n", got);
  return 0;
}
