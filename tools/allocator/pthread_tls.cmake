# Test-only project hook, selected explicitly with CMAKE_PROJECT_INCLUDE.
# Override only the upstream allocator target after it has been configured;
# keep all native TLS compiler flags dlopen-safe and shipping policy unchanged.
include_guard(GLOBAL)
function(stanli_diagnostic_pthread_tls)
  if(NOT STANLI_BUILD_ALLOCATOR_BENCHMARK OR NOT TARGET mimalloc-static)
    message(FATAL_ERROR "pthread TLS diagnostic requires the private benchmark build")
  endif()
  get_target_property(_defs mimalloc-static COMPILE_DEFINITIONS)
  list(REMOVE_ITEM _defs MI_TLS_MODEL_LOCAL=1 MI_TLS_MODEL_PTHREADS=1)
  list(APPEND _defs MI_TLS_MODEL_PTHREADS=1)
  set_property(TARGET mimalloc-static PROPERTY COMPILE_DEFINITIONS "${_defs}")
  message(STATUS "TEST ONLY: mimalloc pthread TLS backend (native TLS flags unchanged)")
endfunction()
cmake_language(DEFER CALL stanli_diagnostic_pthread_tls)
