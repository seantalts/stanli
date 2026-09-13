// Load the shipping DSO after host allocations; no allocator is linked into
// this executable. The small companion DSO tests the same shim's internals.
#include <stanli/capi.h>
#ifndef STANLI_ALLOCATOR_PROBE_ONLY
#include "../runtime/third_party/bridgestan.h"
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <vector>
extern "C" void* allocator_fixture_malloc(size_t);

static void* open_library(const char* path) {
#ifdef _WIN32
  return LoadLibraryA(path);
#else
  void* result = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!result) std::fprintf(stderr, "%s\n", dlerror());
  return result;
#endif
}
static void* lookup(void* library, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
  return dlsym(library, name);
#endif
}
static int close_library(void* library) {
#ifdef _WIN32
  return FreeLibrary(static_cast<HMODULE>(library)) ? 0 : -1;
#else
  return dlclose(library);
#endif
}

static void require(bool value, const char* why) {
  if (!value) {
    std::fprintf(stderr, "FAIL private allocator: %s\n", why);
    std::abort();
  }
}
template <class F> static F symbol(void* library, const char* name) {
  auto f = reinterpret_cast<F>(lookup(library, name));
  require(f != nullptr, name);
  return f;
}

int main(int argc, char** argv) {
  require(argc == 3, "expected shipping and probe libraries");
  void* before = std::malloc(8192);
  require(before != nullptr, "host allocation");
  std::memset(before, 0x5a, 8192);
#ifdef __APPLE__
  malloc_zone_t* host_zone = malloc_zone_from_ptr(before);
  require(host_zone != nullptr, "host allocation must have a system zone");
#endif
#ifndef STANLI_ALLOCATOR_PROBE_ONLY
  void* runtime = open_library(argv[1]);
  require(runtime != nullptr, "dlopen shipping library");
  require(lookup(runtime, "mi_malloc") == nullptr, "private mi_* escaped the shipping library");
#ifdef _WIN32
  require(lookup(runtime, "malloc") == nullptr, "shipping DLL exported malloc");
#else
  require(dlsym(runtime, "malloc") == dlsym(RTLD_DEFAULT, "malloc"),
          "shipping library exported malloc");
#endif
  const auto build_id = symbol<decltype(&stanli_build_id)>(runtime, "stanli_build_id");
  require(std::strstr(build_id(), "-mimalloc-3.5.1-private-c") != nullptr,
          "shipping build does not identify its allocator");
#endif

  for (int lifetime = 0; lifetime < 4; ++lifetime) {
    void* probe = open_library(argv[2]);
    require(probe != nullptr, "dlopen probe");
    auto owns = symbol<bool (*)(const void*)>(probe, "allocator_test_owns");
    auto alloc = symbol<void* (*)(size_t, int)>(probe, "allocator_test_alloc");
    auto release = symbol<void (*)(void*)>(probe, "allocator_test_free");
    auto cpp_delete = symbol<void (*)(void*)>(probe, "allocator_test_delete");
    auto resize = symbol<void* (*)(void*, size_t)>(probe, "allocator_test_realloc");
    auto eigen = symbol<int (*)()>(probe, "allocator_test_eigen");
    auto thread_test = symbol<int (*)()>(probe, "allocator_test_thread");
    require(!owns(before), "host pointer was adopted by private mimalloc");
#ifndef _WIN32
    require(dlsym(RTLD_DEFAULT, "mi_malloc") == nullptr, "mi_* leaked globally");
#endif
#ifdef _WIN32
    constexpr int families[] = {0, 1, 2, 5, 6};  // UCRT has no POSIX allocation APIs.
#else
    constexpr int families[] = {0, 1, 2, 3, 4, 5, 6};
#endif
    for (int kind : families) {
      void* p = alloc(1024, kind);
      // Mach-O local malloc also catches separately linked C dependencies;
      // ELF/PE selectively rewrite only the Stanli input archive.
#ifdef __APPLE__
      const bool expected_private = kind != 2;
#else
      const bool expected_private = kind != 2 && kind != 6;
#endif
      require(p && owns(p) == expected_private, "C/new/dependency ownership split");
      if (kind == 3 || kind == 4) require(reinterpret_cast<uintptr_t>(p) % 64 == 0, "alignment");
      if (kind == 1)
        for (size_t i = 0; i < 1024; ++i)
          require(static_cast<unsigned char*>(p)[i] == 0, "calloc zeroing");
      if (kind == 2) cpp_delete(p); else release(p);
    }
    void* original_object = allocator_fixture_malloc(1024);
    require(original_object && !owns(original_object), "reusable original object was rewritten");
    std::free(original_object);
    release(nullptr);
    void* p = resize(nullptr, 1024);
    require(p && owns(p), "realloc(NULL)");
    std::memset(p, 0x3c, 1024);
    p = resize(p, 8192);
    require(p && owns(p), "private realloc");
    for (size_t i = 0; i < 1024; ++i)
      require(static_cast<unsigned char*>(p)[i] == 0x3c, "private realloc data");
    require(resize(p, std::numeric_limits<size_t>::max()) == nullptr, "failed realloc");
    require(static_cast<unsigned char*>(p)[0] == 0x3c, "failed realloc lost original");
    release(p);

    void* foreign = std::malloc(1024);
    require(foreign && !owns(foreign), "fresh host malloc redirected");
    std::memset(foreign, 0x37, 1024);
    foreign = resize(foreign, 8192);
    require(foreign && !owns(foreign), "foreign realloc switched owner");
    for (size_t i = 0; i < 1024; ++i)
      require(static_cast<unsigned char*>(foreign)[i] == 0x37, "foreign realloc data");
    release(foreign);
    require(eigen() == 1, "Eigen/exception integration");
    require(thread_test() == 1, "DSO-created thread teardown");
    std::vector<void*> cross_thread(4);
    for (void*& value : cross_thread) value = alloc(4096, 0);
    std::vector<std::thread> threads;
    for (int c = 0; c < 4; ++c) threads.emplace_back([&, c] {
      release(cross_thread[c]);
      for (int i = 0; i < 10000; ++i) {
        void* value = alloc(128 + (i % 16) * 64, 0);
        require(value && owns(value), "worker allocation owner");
        release(value);
      }
      require(eigen() == 1, "worker Eigen/exception integration");
    });
    for (auto& thread : threads) thread.join();
    require(close_library(probe) == 0, "probe handle teardown");
  }

#ifndef STANLI_ALLOCATOR_PROBE_ONLY
  auto bs_create = symbol<decltype(&bs_model_construct)>(runtime, "bs_model_construct");
  auto bs_release = symbol<decltype(&bs_free_error_msg)>(runtime, "bs_free_error_msg");
  for (int i = 0; i < 16; ++i) {
    char* error = nullptr;
    require(bs_create("{not json", 1, &error) == nullptr && error && *error,
            "BridgeStan must return an owned error string");
#ifdef __APPLE__
    require(malloc_zone_from_ptr(error) == nullptr, "BridgeStan error string is not private");
#endif
    bs_release(error);
  }
  auto embedded = symbol<decltype(&stanli_has_embedded_stanc)>(runtime, "stanli_has_embedded_stanc");
  if (embedded()) {
    auto compile = symbol<decltype(&stanli_stan_to_mir)>(runtime, "stanli_stan_to_mir");
    auto string_free = symbol<decltype(&stanli_string_free)>(runtime, "stanli_string_free");
    auto model_new = symbol<decltype(&stanli_model_new)>(runtime, "stanli_model_new");
    auto model_free = symbol<decltype(&stanli_model_free)>(runtime, "stanli_model_free");
    auto grad = symbol<decltype(&stanli_grad)>(runtime, "stanli_grad");
    char err[8192]{};
    char* mir = compile("parameters { real x; } model { x ~ normal(0, 1); }", err, sizeof err);
    require(mir != nullptr, err);
#ifdef __APPLE__
    require(malloc_zone_from_ptr(mir) == nullptr, "C API string did not use private malloc");
#endif
    stanli_model* model = model_new(mir, "{}", err, sizeof err);
    string_free(mir);
    require(model != nullptr, err);
#ifdef __APPLE__
    require(malloc_zone_from_ptr(model) != nullptr, "C++ model handle changed allocator");
#endif
    for (int i = 0; i < 10000; ++i) {
      double q = (i % 8 - 4) * 0.1, lp = 0, g = 0;
      require(grad(model, &q, &lp, &g) == 0 && std::isfinite(lp) && g == -q,
              "shipping C API gradient");
    }
    model_free(model);
    require(compile("this is not Stan", err, sizeof err) == nullptr, "compiler error path");
  }
#endif
#ifdef __APPLE__
  require(malloc_zone_from_ptr(before) == host_zone, "host allocator changed");
#endif
  for (size_t i = 0; i < 8192; ++i)
    require(static_cast<unsigned char*>(before)[i] == 0x5a, "preexisting host data corrupted");
  std::free(before);
  // Keep the actual runtime loaded until process exit, as Python/R do. Its
  // embedded compiler's unload semantics are not part of this allocator test.
#ifdef STANLI_ALLOCATOR_PROBE_ONLY
  std::puts("private allocator ownership, Eigen, and threads passed (standalone probe)");
#else
  std::puts("private allocator ownership, C API, threads, and exports passed");
#endif
}
