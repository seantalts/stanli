#include <cstdlib>
// This exact object goes to the host executable and to the probe DSO.
extern "C" void* allocator_fixture_malloc(size_t n) { return std::malloc(n); }
