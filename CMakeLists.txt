cmake_minimum_required(VERSION 3.16)

# Before project(): the deployment target is baked in at compiler-probe
# time. Left alone it defaults to whatever the build machine runs, so the
# wheel would claim a floor it does not honor. Must agree with the
# platform tags in tools/build_wheel.sh. MACOSX_DEPLOYMENT_TARGET in the
# environment still wins.
execute_process(COMMAND uname -s OUTPUT_VARIABLE _host_os
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(_host_os STREQUAL "Darwin" AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET}
                             AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  execute_process(COMMAND uname -m OUTPUT_VARIABLE _host_arch
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_host_arch STREQUAL "arm64")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
  else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
  endif()
endif()

project(stanli C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/deps)
file(GLOB _eigen ${DEPS}/math/lib/eigen_*)
file(GLOB _boost ${DEPS}/math/lib/boost_*)
file(GLOB _sundials ${DEPS}/math/lib/sundials_*)
file(GLOB _tbb ${DEPS}/math/lib/tbb_*)

# std::thread in nuts.cpp (parallel chains). libstdc++ needs -pthread at
# LINK time for it, and every consumer of the static library inherits the
# translation unit whether or not it ever starts a chain -- so this is
# PUBLIC on the libraries rather than added per executable. macOS hides
# the requirement (pthreads live in libSystem), which is why this only
# ever fails on Linux, and only in CI.
#
# Not under Emscripten: wasm threads need -pthread plus a
# cross-origin-isolated page, thread_safe_build() is false there, and
# emscripten's libc resolves the symbols without the flag.
if(NOT EMSCRIPTEN)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
endif()

# Everything that includes stan-math (or stan) links this interface target.
add_library(stanmath INTERFACE)
target_include_directories(stanmath INTERFACE
  ${DEPS}/math
  ${DEPS}/stan/src
  ${_eigen}
  ${_boost}
  ${_sundials}/include
  ${_tbb}/include)
target_compile_definitions(stanmath INTERFACE _REENTRANT BOOST_DISABLE_ASSERTS)
# Pin FP contraction off: clang otherwise forms FMAs differently across
# template instantiations of the same math, which breaks bitwise parity
# between the kernels and the var-path references (measured: 2 ULP on
# eight-schools d/dmu). Cost is well inside the performance budget.
target_compile_options(stanmath INTERFACE -ffp-contract=off)

# Half the library is the propto family: stan-math drops a term based on
# the argument TYPES, so an exact `~` needs the density instantiated once
# per activity mask on top of the full form. Dropping it leaves a build
# whose gradients are bit-identical, with an lp__ that sits a per-model
# constant above CmdStan's. A given seed still draws a different chain,
# because lp enters the Hamiltonian additively and a shifted lp rounds
# differently: the same sampler statistically, not the same bytes.
#
# Off everywhere, browser included. The browser build used to default it
# on to save 7 MB of wasm, which meant the demo reported an lp__ nobody
# could compare against CmdStan. Every build now reports the same lp__.
# See density_tier in optable.hpp and docs/lite-lp.md.
option(STANLI_LITE_LP
  "Drop the propto instantiation family: half the size, lp__ off by a constant"
  OFF)
if(STANLI_LITE_LP)
  target_compile_definitions(stanmath INTERFACE STANLI_LITE_LP=1)
endif()

# Chains in threads. stan-math's autodiff stack is a plain static unless
# STAN_THREADS is defined, in which case it is thread_local (__thread on
# gcc/clang). The legacy kernels and tape islands build NESTED var tapes
# on that stack, so without this two chains in two threads share one tape
# and corrupt each other -- which is why stanli_thread_safe() exists and
# why num_threads is CLAMPED rather than honoured on a build without it.
#
# On by default, because the cost was measured and is not there. The TLS
# indirection lands on every var operation, so the model to worry about
# is one dominated by the nested-tape path: a 200-step ordered_logistic
# recurrence spending 72% of its gradient inside a legacy op measured
# 44,695 ns without and 44,370 ns with, and eight schools 221.4 against
# 223.0 -- noise in both directions. Against that, 8 chains on 8 threads
# is 5.9x, and the output is byte-identical to the sequential run.
#
# Off under Emscripten: wasm threads need -pthread and a
# cross-origin-isolated page, and the browser build runs one chain per
# worker instead.
option(STANLI_THREADS
  "thread_local autodiff stack: parallel chains (measured free; see docs/benchmarks.md)"
  ON)
if(STANLI_THREADS AND NOT EMSCRIPTEN)
  target_compile_definitions(stanmath INTERFACE STAN_THREADS=1)
endif()

if(EMSCRIPTEN)
  target_compile_options(stanmath INTERFACE -fwasm-exceptions)
  # SIMD128. Measured with tools/bench_wasm.cjs on this machine: geomean
  # 457 -> 449 ns/gradient over the fixtures, radon_pooled 107.9 -> 105.7
  # us, nes 29.1 -> 25.9 us (11%), for 0.03 MB gzipped. The reason it is
  # safe is the interesting part: every corpus model's gradients come out
  # BITWISE identical to the scalar build (checked through
  # tools/wasm_check.sh against the CmdStan references, and directly on
  # radon_pooled and nes at three points each). With -ffp-contract=off
  # still in force the vectorization Eigen takes here is elementwise, and
  # elementwise is order-preserving; a vectorized reduction would
  # reassociate and would have shown up immediately. Baseline support is
  # Chrome 91, Firefox 89, Safari 16.4.
  target_compile_options(stanmath INTERFACE -msimd128)
  # The density kernels instantiate every activity mask times every shape
  # times summed/elementwise: 21 MB of object at -O3. The browser build
  # trades their straight-line speed for size.
  set_source_files_properties(
    runtime/kernels/densities_common.cpp
    runtime/kernels/densities_common_b.cpp
    runtime/kernels/densities_lpmf.cpp
    runtime/kernels/densities_glm.cpp
    runtime/kernels/densities_rest.cpp
    runtime/kernels/densities_rest_b.cpp
    runtime/kernels/densities_cdf_a.cpp
    runtime/kernels/densities_cdf_b.cpp
    PROPERTIES COMPILE_OPTIONS -Oz)
  # stan-math's rev core includes TBB 2020 headers unconditionally, and
  # that TBB predates wasm: its machine layer selects by OS macro and
  # errors out on anything unknown. Steer it to the portable
  # __atomic-builtins port (machine/gcc_generic.h), which sits behind the
  # __linux__ branch. Emscripten's libc is musl, so the pretense is thin.
  target_compile_definitions(stanmath INTERFACE
    __linux__=1 TBB_USE_GCC_BUILTINS=1)
endif()

# The distributable shared library: C ABI + all kernels, TBB stubbed
# (single-threaded until the payload ships real TBB).
# SUNDIALS (CVODES) for the ODE integrators: the same translation units
# CmdStan compiles, minus the profiler and the Fortran shims. Built once
# into a static library and linked into both stanli targets.
file(GLOB SUNDIALS_SRC
  ${_sundials}/src/cvodes/*.c
  ${_sundials}/src/sundials/*.c
  ${_sundials}/src/nvector/serial/*.c
  ${_sundials}/src/sunmatrix/band/*.c
  ${_sundials}/src/sunmatrix/dense/*.c
  ${_sundials}/src/sunlinsol/band/*.c
  ${_sundials}/src/sunlinsol/dense/*.c
  ${_sundials}/src/sunnonlinsol/newton/*.c
  ${_sundials}/src/sunnonlinsol/fixedpoint/*.c)
list(FILTER SUNDIALS_SRC EXCLUDE REGEX "sundials_profiler\\.c$")
list(FILTER SUNDIALS_SRC EXCLUDE REGEX "/f(nvector|sunmatrix|sunlinsol|sunnonlinsol)")
add_library(sundials STATIC ${SUNDIALS_SRC})
target_include_directories(sundials PUBLIC ${_sundials}/include
                           PRIVATE ${_sundials}/src ${_sundials}/src/sundials)
# stan-math patches sundials' logging through this header, which the
# upstream C sources expect to be force-included (as CmdStan does).
target_compile_options(sundials PRIVATE -w
  -include ${_sundials}/include/stan_sundials_printf_override.hpp)
set_target_properties(sundials PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT EMSCRIPTEN)
add_library(stanli_shared SHARED
  runtime/src/capi.cpp
  runtime/src/executor.cpp
  runtime/kernels/elementwise.cpp
  runtime/kernels/densities.cpp
  runtime/kernels/densities_common.cpp
  runtime/kernels/densities_lpmf.cpp
  runtime/kernels/densities_glm.cpp
  runtime/kernels/densities_common_b.cpp
  runtime/kernels/densities_rest.cpp
  runtime/kernels/densities_rest_b.cpp
  runtime/kernels/densities_cdf_a.cpp
  runtime/kernels/densities_cdf_b.cpp
  runtime/kernels/legacy_fns.cpp
  runtime/kernels/matrix_fns.cpp
  runtime/kernels/ode.cpp
  runtime/src/ode_prog.cpp
  runtime/kernels/constrain.cpp
  runtime/kernels/eltwise_expr.cpp
  runtime/kernels/mixture.cpp
  runtime/kernels/message.cpp
  runtime/kernels/island.cpp
  runtime/src/island.cpp
  runtime/src/nuts.cpp
  runtime/src/diagnose.cpp
  runtime/src/mir_reader.cpp
  runtime/src/lower.cpp
  runtime/src/wa_interp.cpp
  runtime/src/reroll.cpp
  runtime/src/inplace.cpp
  runtime/src/constfold.cpp
  runtime/src/data.cpp
  tests/tbb_stub.cpp)
target_include_directories(stanli_shared PUBLIC runtime/include)
target_link_libraries(stanli_shared PUBLIC stanmath sundials Threads::Threads)
set_target_properties(stanli_shared PROPERTIES OUTPUT_NAME stanli)

# Export only the C API. Everything else in here (stan-math, Eigen, the
# whole embedded OCaml compiler) is an implementation detail, and leaving
# 15k symbols exported costs 2.7 MB of symbol table, string table, and
# export trie in the shipped library.
if(APPLE)
  target_link_options(stanli_shared PRIVATE
    "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/tools/exported_symbols.txt")
  set_property(TARGET stanli_shared APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/exported_symbols.txt)
elseif(WIN32)
  # PE exports come from the .def; -static folds libstdc++, libgcc, and
  # winpthread into the dll so the wheel drops one self-contained file
  # onto machines with no mingw runtime.
  target_sources(stanli_shared PRIVATE tools/exported_symbols.def)
  set_target_properties(stanli_shared PROPERTIES PREFIX "")
  target_link_options(stanli_shared PRIVATE -static)
elseif(UNIX)
  target_link_options(stanli_shared PRIVATE
    "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/tools/exported_symbols.map")
  set_property(TARGET stanli_shared APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/exported_symbols.map)
  # The wheel is one library dropped onto an unknown machine, and a
  # dynamic libstdc++ would make it require whatever GLIBCXX version the
  # build container's toolchain happened to emit. Statically linking the
  # C++ runtime leaves glibc as the only versioned dependency, which is
  # what the manylinux tag is actually promising.
  target_link_options(stanli_shared PRIVATE
    -static-libstdc++ -static-libgcc)
endif()

# In-process stanc3: link the OCaml-compiled embed object
# (tools/stanc_embed/build.sh produces deps/stanc3/stanc_embed.o).
set(STANLI_STANC_EMBED_OBJ "" CACHE FILEPATH
    "stanc_embed.o built by tools/stanc_embed/build.sh; enables in-process stanc")
set(STANLI_OCAML_STDLIB "" CACHE PATH
    "OCaml stdlib dir (opam switch lib/ocaml) for caml/*.h")
# The embed object is a whole OCaml runtime, so it can only be linked into
# one artifact at a time -- hence a function rather than adding it to the
# static library every target shares.
function(stanli_embed_stanc TGT)
  if(STANLI_STANC_EMBED_OBJ)
    target_sources(${TGT} PRIVATE
      runtime/src/stanc_embed_c.cpp ${STANLI_STANC_EMBED_OBJ})
    target_include_directories(${TGT} PRIVATE ${STANLI_OCAML_STDLIB})
    target_compile_definitions(${TGT} PRIVATE STANLI_EMBED_STANC)
    target_link_libraries(${TGT} PRIVATE dl pthread)
  endif()
endfunction()
stanli_embed_stanc(stanli_shared)
endif()  # NOT EMSCRIPTEN

add_library(stanli STATIC
  runtime/src/executor.cpp
  runtime/kernels/elementwise.cpp
  runtime/kernels/densities.cpp
  runtime/kernels/densities_common.cpp
  runtime/kernels/densities_lpmf.cpp
  runtime/kernels/densities_glm.cpp
  runtime/kernels/densities_common_b.cpp
  runtime/kernels/densities_rest.cpp
  runtime/kernels/densities_rest_b.cpp
  runtime/kernels/densities_cdf_a.cpp
  runtime/kernels/densities_cdf_b.cpp
  runtime/kernels/legacy_fns.cpp
  runtime/kernels/matrix_fns.cpp
  runtime/kernels/ode.cpp
  runtime/src/ode_prog.cpp
  runtime/kernels/constrain.cpp
  runtime/kernels/eltwise_expr.cpp
  runtime/kernels/mixture.cpp
  runtime/kernels/message.cpp
  runtime/kernels/island.cpp
  runtime/src/island.cpp
  runtime/src/nuts.cpp
  runtime/src/diagnose.cpp
  runtime/src/mir_reader.cpp
  runtime/src/lower.cpp
  runtime/src/wa_interp.cpp
  runtime/src/reroll.cpp
  runtime/src/inplace.cpp
  runtime/src/constfold.cpp
  runtime/src/data.cpp)
target_include_directories(stanli PUBLIC runtime/include)
target_link_libraries(stanli PUBLIC stanmath sundials)
if(NOT EMSCRIPTEN)
  target_link_libraries(stanli PUBLIC Threads::Threads)
endif()

# The browser/WASM build: the same runtime and C ABI over the MIR-text
# entry point (no embedded stanc; stanc.js supplies the MIR in JS). One
# .js + .wasm pair, MODULARIZE'd so web pages and Node both load it.
if(EMSCRIPTEN)
  add_executable(stanli_wasm runtime/src/capi.cpp tests/tbb_stub.cpp)
  target_link_libraries(stanli_wasm PRIVATE stanli)
  set_target_properties(stanli_wasm PROPERTIES OUTPUT_NAME stanli)
  target_link_options(stanli_wasm PRIVATE
    -fwasm-exceptions
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_NAME=createStanli"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s MAXIMUM_MEMORY=4GB"
    "SHELL:-s STACK_SIZE=8MB"
    "SHELL:-s ENVIRONMENT=web,worker,node"
    "SHELL:-s EXPORTED_FUNCTIONS=_stanli_model_new,_stanli_model_new_from_stan,_stanli_has_embedded_stanc,_stanli_exact_lp,_stanli_model_free,_stanli_n_unconstrained,_stanli_grad,_stanli_sample,_stanli_n_constrained,_stanli_constrained_name,_stanli_constrain,_stanli_wa_n_columns,_stanli_wa_column_name,_stanli_wa_seed,_stanli_wa_row,_stanli_sample_stream,_malloc,_free"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString,stringToNewUTF8,HEAPF64,addFunction,removeFunction"
    "SHELL:-s ALLOW_TABLE_GROWTH=1")
endif()

if(NOT EMSCRIPTEN)
enable_testing()
# Single-threaded tests stub the one TBB symbol stan-math's rev core needs.
function(stanli_add_test NAME)
  add_executable(${NAME} tests/${NAME}.cpp tests/tbb_stub.cpp)
  target_link_libraries(${NAME} PRIVATE stanli)
  # Tests run from the repo root so fixture paths are stable.
  add_test(NAME ${NAME} COMMAND ${NAME}
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

# The native sampler. With the embed object it is one self-contained
# binary -- .stan in, CmdStan-shaped CSV out, no toolchain and no separate
# stanc to find -- which is the whole point of an interpreter for a
# language whose reference implementation compiles C++ per model.
add_executable(stanli_run tools/stanli_run.cpp tests/tbb_stub.cpp)
target_link_libraries(stanli_run PRIVATE stanli)
stanli_embed_stanc(stanli_run)
add_executable(stanli_check tools/stanli_check.cpp tests/tbb_stub.cpp)
target_link_libraries(stanli_check PRIVATE stanli)
add_executable(bench_grad tools/bench_grad.cpp tests/tbb_stub.cpp)
target_link_libraries(bench_grad PRIVATE stanli)
add_executable(bench_opcost tools/bench_opcost.cpp tests/tbb_stub.cpp)
target_link_libraries(bench_opcost PRIVATE stanli)
add_executable(bench_dispatch tools/bench_dispatch.cpp)
add_executable(dump_ops tools/dump_ops.cpp tests/tbb_stub.cpp)
target_link_libraries(dump_ops PRIVATE stanli)

# Installable: the native sampler, the parity checker, the shared
# library the language bindings load, and the headers a C++ caller
# needs. `cmake --install build-rel --prefix ...` is the whole story;
# the Python wheel does its own copying (tools/build_wheel.sh).
include(GNUInstallDirs)
install(TARGETS stanli_run stanli_check
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS stanli_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY runtime/include/stanli
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

stanli_add_test(test_smoke)
stanli_add_test(test_executor)
stanli_add_test(test_recorder)
stanli_add_test(test_densities)
stanli_add_test(test_matvec)
stanli_add_test(test_legacy)
stanli_add_test(test_eight_schools)
stanli_add_test(test_glm)
stanli_add_test(test_nuts)
stanli_add_test(test_sexp)
stanli_add_test(test_mir)
stanli_add_test(test_transforms)
stanli_add_test(test_eltwise)
stanli_add_test(test_lower)
stanli_add_test(test_data)
stanli_add_test(test_e2e)
stanli_add_test(test_reroll)
stanli_add_test(test_profile)
stanli_add_test(test_inplace)
stanli_add_test(test_pass_safety)
stanli_add_test(test_sampling)
stanli_add_test(test_ode_prog)
stanli_add_test(test_constfold)
stanli_add_test(test_write_array)
stanli_add_test(test_mixture)
stanli_add_test(test_island)
stanli_add_test(test_diagnose)
stanli_add_test(test_multichain)
stanli_add_test(test_newtrans)
stanli_add_test(test_rejectprint)
stanli_add_test(test_odevariadic)
endif()  # NOT EMSCRIPTEN
