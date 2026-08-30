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

# The one exception: walnuts.cpp wraps the vendored walnutpie headers,
# which are written against C++20 concepts. It includes no stan-math --
# just the executor interface and Eigen -- so the newer standard stays
# contained to that translation unit.
set_source_files_properties(runtime/src/walnuts.cpp PROPERTIES
  COMPILE_OPTIONS "-std=c++20"
  INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/runtime/third_party)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Sanitizers, for running the test suite under. Empty by default and never
# a shipped flavour: this is a way to run ctest, not a build anyone
# installs, so it deliberately does not touch STANLI_BUILD_ID.
#
# Global rather than an option on the stanmath interface target, because
# the bugs worth catching do not respect target boundaries -- the vendored
# SUNDIALS C and the C++20 walnuts translation unit have to be
# instrumented on the same terms as the kernels.
#
# Address is what the two memory bugs that reached main in August wanted:
# both produced WRONG NUMBERS rather than a crash in the common case (a
# GLM outcome mapped `rows` integers out of a 3-integer group; the island
# register file handed out a var from an already-recovered nested tape),
# and only the first of those is a heap overflow ASan can see. The second
# is invisible to any sanitizer, because stan-math's arena recovers by
# rewinding a bump pointer (stack_alloc.hpp recover_nested) and never
# returns the bytes to the allocator.
#
# `address,undefined` works too, and is clean: all 51 tests pass with zero
# runtime errors on Apple clang 17, arm64, with -fno-sanitize-recover
# below in force. CI runs `address` alone only because that result has not
# been reproduced on the Linux toolchain, whose UBSan differs -- the two
# bugs above are both ASan's kind, so the job leads with what it can show
# working, and adding `,undefined` here is the whole change if someone
# confirms it there.
set(STANLI_SANITIZE "" CACHE STRING
    "Sanitizers to build the tests with, e.g. `address` (empty: none)")
if(STANLI_SANITIZE)
  # -g even in Release: a sanitizer report without a symbolized stack is a
  # bug number nobody can act on.
  add_compile_options(-fsanitize=${STANLI_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${STANLI_SANITIZE})
  # UndefinedBehaviorSanitizer prints and CARRIES ON by default, so a
  # suite that trips it still exits 0 and CI still goes green. Trap it
  # instead: a check nobody's exit code depends on is the failure this
  # whole job exists to close.
  if(STANLI_SANITIZE MATCHES "undefined")
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
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

# A webR side module needs relocatable code in every object that reaches the
# final wasm dylib. Make the mode self-contained instead of relying on callers
# to remember a second CMake flag; the ordinary browser build stays non-PIC.
option(STANLI_WASM_SIDE
  "Build libstanli.so as an Emscripten side module for webR" OFF)
if(EMSCRIPTEN AND STANLI_WASM_SIDE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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

# Identifies this runtime binary, for callers that cache artifacts next to
# the library that produced them (the BridgeStan lib pair keys its manifest
# on it). The git sha names the source; the flavour bits name the build
# choices that change what the same source produces. Uncommitted edits do
# not move it -- it identifies a build of the project, not a working tree.
execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short=12 HEAD
                OUTPUT_VARIABLE STANLI_GIT_SHA
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT STANLI_GIT_SHA)
  set(STANLI_GIT_SHA "nogit")
endif()
set(STANLI_BUILD_ID
  "abi1-${STANLI_GIT_SHA}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(STANLI_LITE_LP)
  string(APPEND STANLI_BUILD_ID "-litelp")
endif()
if(STANLI_THREADS)
  string(APPEND STANLI_BUILD_ID "-threads")
endif()

# Only this tiny translation unit reads the build id. Keeping this definition
# source-scoped is important: the id contains the git SHA, so placing it on a
# runtime target invalidates every stan-math-heavy object on every commit even
# when none of those sources or headers changed.
set_source_files_properties(runtime/src/build_id.cpp PROPERTIES
  COMPILE_DEFINITIONS STANLI_BUILD_ID="${STANLI_BUILD_ID}")

# One runtime source inventory. The implementation is compiled once as a PIC
# object library and reused by the shared library and the test/static library.
# These targets intentionally keep separate link behavior; recompiling every
# stan-math-heavy translation unit merely to change the final archive kind
# used to make a clean native build do the expensive half of its work twice.
set(STANLI_RUNTIME_SOURCES
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
  runtime/kernels/scalar_binary.cpp
  runtime/kernels/scalar_unary_ad.cpp
  runtime/kernels/algebra.cpp
  runtime/kernels/ode.cpp
  runtime/src/ode_prog.cpp
  runtime/src/program.cpp
  runtime/kernels/constrain.cpp
  runtime/kernels/eltwise_expr.cpp
  runtime/kernels/mixture.cpp
  runtime/kernels/message.cpp
  runtime/kernels/rng.cpp
  runtime/kernels/island.cpp
  runtime/src/island.cpp
  runtime/src/adjoint.cpp
  runtime/src/program_density.cpp
  runtime/src/nuts.cpp
  runtime/src/walnuts.cpp
  runtime/src/diagnose.cpp
  runtime/src/estimate.cpp
  runtime/src/pathfinder.cpp
  runtime/src/mir_reader.cpp
  runtime/src/mir_decode.cpp
  runtime/src/portable_mir_v2_reader.cpp
  runtime/src/lower.cpp
  runtime/src/init_interp.cpp
  runtime/src/unconstrain.cpp
  runtime/src/wa_interp.cpp
  runtime/src/message_sink.cpp
  runtime/src/executor_pool.cpp
  runtime/src/reroll.cpp
  runtime/src/inplace.cpp
  runtime/src/constfold.cpp
  runtime/src/cse.cpp
  runtime/src/partition.cpp
  runtime/src/data.cpp
  runtime/src/data_var_context.cpp
  runtime/src/program_softmax.cpp)

if(NOT EMSCRIPTEN)
  list(APPEND STANLI_RUNTIME_SOURCES runtime/src/bridgestan_abi.cpp)
endif()

add_library(stanli_runtime_objects OBJECT ${STANLI_RUNTIME_SOURCES})
target_include_directories(stanli_runtime_objects PRIVATE runtime/include)
target_link_libraries(stanli_runtime_objects PRIVATE stanmath)
if(NOT EMSCRIPTEN)
  # Preserve Threads::Threads' compile requirements (notably -pthread on
  # Linux) now that these sources are compiled outside their final targets.
  target_link_libraries(stanli_runtime_objects PRIVATE Threads::Threads)
endif()
# Native objects also land in stanli_shared. Emscripten's browser build should
# remain non-PIC, while the webR configuration opts into PIC globally.
if(NOT EMSCRIPTEN)
  set_target_properties(stanli_runtime_objects PROPERTIES
                        POSITION_INDEPENDENT_CODE ON)
endif()

# This tiny compatibility definition used to be compiled into every test and
# tool separately (more than sixty times). An object library retains the
# required per-executable symbol while compiling its source only once.
add_library(stanli_tbb_stub OBJECT tests/tbb_stub.cpp)
if(NOT EMSCRIPTEN)
  set_target_properties(stanli_tbb_stub PROPERTIES
                        POSITION_INDEPENDENT_CODE ON)
endif()

if(NOT EMSCRIPTEN)
add_library(stanli_shared SHARED
  runtime/src/capi.cpp
  runtime/src/function.cpp
  runtime/src/build_id.cpp
  $<TARGET_OBJECTS:stanli_runtime_objects>
  $<TARGET_OBJECTS:stanli_tbb_stub>)
target_include_directories(stanli_shared PUBLIC runtime/include)
# ${CMAKE_DL_LIBS}: bs_model_construct calls dladdr to find its own path.
target_link_libraries(stanli_shared
                      PUBLIC stanmath sundials Threads::Threads ${CMAKE_DL_LIBS})
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

# Sanitizer builds keep using a shared C++ runtime. The reason is disk, not
# elegance: 52 statically linked test binaries each carry their own
# instrumented copy of the runtime and its debug info, and under ASan that
# tree overran a disk which started with 86 GB free -- the link step died of
# "No space left on device" three runs straight. One shared object holds the
# single copy; the binaries shrink to their own test code; CMake's build-tree
# RPATH makes ctest find it with no further ceremony. Unlike stanli_shared
# this library keeps every symbol visible, which is what the tests link
# against.
if(STANLI_SANITIZE)
  set(_stanli_kind SHARED)
else()
  set(_stanli_kind STATIC)
endif()
add_library(stanli ${_stanli_kind}
  runtime/src/build_id.cpp
  $<TARGET_OBJECTS:stanli_runtime_objects>)
if(STANLI_SANITIZE)
  # Not libstanli: stanli_shared already owns that name -- it is the
  # file the wheel ships -- and two targets writing one .dylib means the
  # last one clobbers the first, which surfaced as test_capi failing to
  # find the C ABI it links.
  set_target_properties(stanli PROPERTIES OUTPUT_NAME stanli_core)
  # The archive let every binary satisfy the TBB symbols with its own
  # tests/tbb_stub.cpp; a shared library must either resolve them at
  # link time or defer them to the executable. Defer, so the stub
  # arrangement is unchanged: macOS needs it said out loud, Linux
  # already allows undefined symbols in a shared object.
  if(APPLE)
    target_link_options(stanli PRIVATE -Wl,-undefined,dynamic_lookup)
  endif()
endif()
target_include_directories(stanli PUBLIC runtime/include)
# The BridgeStan facade is native-only: it finds its own sidecar manifest
# with dladdr, and a browser has neither dlopen nor a second process to
# load the pair. Nothing under wasm references a bs_ symbol, so this only
# saves compiling it.
if(NOT EMSCRIPTEN)
  # PUBLIC, so anything linking the runtime sees the same decoration the
  # definitions were compiled with. Without it the reference header's
  # BS_PUBLIC is __declspec(dllimport) on Windows, and a test linking the
  # static library asks for __imp_bs_* from a DLL that does not exist.
  # Ignored everywhere else: BS_PUBLIC is a visibility attribute there.
  target_compile_definitions(stanli PUBLIC BRIDGESTAN_EXPORT=1)
endif()
target_link_libraries(stanli PUBLIC stanmath sundials)
if(NOT EMSCRIPTEN)
  # ${CMAKE_DL_LIBS}: bs_model_construct calls dladdr. Not under
  # Emscripten, whose libc already carries it.
  target_link_libraries(stanli PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# Native Clang developer builds put the same C++ implementation behind one
# test-only shared library. Keep the ordinary static target as well: stanli_run
# and stanli_check are installed executables and must not acquire a runtime
# dependency on a library that is deliberately absent from the install set.
# Non-Clang and Windows builds retain their established static test linkage;
# sanitizer builds already use the shared `stanli` target above.
set(_stanli_shared_test_runtime_default OFF)
if(NOT EMSCRIPTEN AND NOT WIN32 AND
   CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
  set(_stanli_shared_test_runtime_default ON)
endif()
option(STANLI_SHARED_TEST_RUNTIME
  "Link native tests to one shared C++ runtime in Clang developer builds"
  ${_stanli_shared_test_runtime_default})

set(STANLI_TEST_RUNTIME_TARGET stanli)
if(STANLI_SHARED_TEST_RUNTIME AND NOT STANLI_SANITIZE AND
   NOT EMSCRIPTEN AND NOT WIN32)
  add_library(stanli_test_runtime SHARED
    runtime/src/build_id.cpp
    $<TARGET_OBJECTS:stanli_runtime_objects>)
  set_target_properties(stanli_test_runtime PROPERTIES
                        OUTPUT_NAME stanli_test_core)
  target_include_directories(stanli_test_runtime PUBLIC runtime/include)
  target_compile_definitions(stanli_test_runtime PUBLIC BRIDGESTAN_EXPORT=1)
  target_link_libraries(stanli_test_runtime
                        PUBLIC stanmath sundials Threads::Threads
                               ${CMAKE_DL_LIBS})
  # Test executables continue to provide the single-threaded TBB stub. Linux
  # permits that unresolved shared-library reference by default; Darwin needs
  # the equivalent policy stated explicitly.
  if(APPLE)
    target_link_options(stanli_test_runtime PRIVATE
                        -Wl,-undefined,dynamic_lookup)
  endif()
  set(STANLI_TEST_RUNTIME_TARGET stanli_test_runtime)
endif()

# The browser/WASM build: the same runtime and C ABI over the MIR-text
# entry point (no embedded stanc; stanc.js supplies the MIR in JS). One
# .js + .wasm pair, MODULARIZE'd so web pages and Node both load it.
if(EMSCRIPTEN)
  # The C API surface the wasm artifacts export: without an explicit list
  # wasm-ld garbage-collects the capi functions, which nothing inside the
  # module references (SIDE_MODULE=1 does not root them either). The list
  # is read out of capi.h so a new entry point exports without anyone
  # remembering this file; the R bridge resolves whatever subset it needs
  # (stanli_abi_version among it, which the hand-kept browser list missed).
  file(READ runtime/include/stanli/capi.h _capi_h)
  string(REGEX MATCHALL "(stanli|bs)_[a-zA-Z0-9_]+[ \t]*\\(" _capi_decls
         "${_capi_h}")
  set(_capi_names "")
  foreach(_d ${_capi_decls})
    string(REGEX REPLACE "[ \t]*\\($" "" _d "${_d}")
    list(APPEND _capi_names "_${_d}")
  endforeach()
  list(REMOVE_DUPLICATES _capi_names)
  list(JOIN _capi_names "," STANLI_CAPI_EXPORTS)
  add_executable(stanli_wasm runtime/src/capi.cpp
    $<TARGET_OBJECTS:stanli_tbb_stub>)
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
    "SHELL:-s EXPORTED_FUNCTIONS=${STANLI_CAPI_EXPORTS},_malloc,_free"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString,stringToNewUTF8,HEAPF64,addFunction,removeFunction"
    "SHELL:-s ALLOW_TABLE_GROWTH=1")

# The webR runtime: the same capi surface as a relocatable SIDE_MODULE
# that webR's main module dlopens, which is how the R package loads
# libstanli.so on every platform. Memory, stack, and environment belong
# to the main module, so none of stanli_wasm's -s settings apply; the
# whole tree must be compiled -fPIC, which is why this is its own build
# configuration (-DSTANLI_WASM_SIDE=ON) rather than a second target in
# the browser build. The emsdk version must match the webR release the
# artifact is for -- side modules are not binary compatible across
# webR's Emscripten bumps.
if(STANLI_WASM_SIDE)
  # add_executable, not add_library: Emscripten's CMake platform archives
  # a SHARED library instead of linking it, so the side module is spelled
  # as an "executable" whose output name and SIDE_MODULE flag make emcc
  # emit a relocatable wasm dylib.
  add_executable(stanli_webr runtime/src/capi.cpp
    $<TARGET_OBJECTS:stanli_tbb_stub>)
  target_link_libraries(stanli_webr PRIVATE stanli)
  # POSITION_INDEPENDENT_CODE initializes libraries, but an executable source
  # is otherwise compiled as PIE/non-PIC. capi.cpp belongs in the side module.
  target_compile_options(stanli_webr PRIVATE -fPIC)
  set_target_properties(stanli_webr PROPERTIES
    OUTPUT_NAME libstanli
    SUFFIX ".so")
  target_link_options(stanli_webr PRIVATE
    -fwasm-exceptions
    "SHELL:-s SIDE_MODULE=2"
    "SHELL:-s EXPORTED_FUNCTIONS=${STANLI_CAPI_EXPORTS}")
endif()
endif()

if(NOT EMSCRIPTEN)
enable_testing()

# Runtime and lowering tests consume stanc's legacy MIR, but the generated
# text is not source. Build it once from the checked-in Stan models with the
# same pinned compiler the source-level lit tests use. Keeping the output in
# tests/fixtures preserves the established test and developer-tool paths while
# .gitignore keeps a build from dirtying the checkout.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(STANLI_FIXTURE_PYTHON "${Python3_EXECUTABLE}")
if(WIN32)
  set(_stanli_stanc_default
      "${CMAKE_CURRENT_SOURCE_DIR}/deps/stanc3/stanc.exe")
else()
  set(_stanli_stanc_default
      "${CMAKE_CURRENT_SOURCE_DIR}/deps/stanc3/stanc")
endif()
set(STANLI_STANC_EXECUTABLE "${_stanli_stanc_default}" CACHE FILEPATH
    "Pinned stanc executable used to generate the legacy MIR test corpus")
if(NOT EXISTS "${STANLI_STANC_EXECUTABLE}")
  message(FATAL_ERROR
    "Pinned stanc not found at ${STANLI_STANC_EXECUTABLE}; "
    "run tools/dev_setup.sh --no-build or set STANLI_STANC_EXECUTABLE")
endif()
file(GLOB STANLI_FIXTURE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*.stan")
set(STANLI_MIR_FIXTURES)
foreach(STANLI_FIXTURE_SOURCE IN LISTS STANLI_FIXTURE_SOURCES)
  get_filename_component(STANLI_FIXTURE_STEM
                         "${STANLI_FIXTURE_SOURCE}" NAME_WE)
  list(APPEND STANLI_MIR_FIXTURES
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${STANLI_FIXTURE_STEM}.tmir.sexp")
endforeach()
list(LENGTH STANLI_FIXTURE_SOURCES STANLI_FIXTURE_COUNT)
string(JOIN "\n" STANLI_FIXTURE_MANIFEST_CONTENT
       ${STANLI_FIXTURE_SOURCES})
set(STANLI_FIXTURE_MANIFEST
    "${CMAKE_CURRENT_BINARY_DIR}/stanli_mir_fixture_sources.txt")
file(GENERATE OUTPUT "${STANLI_FIXTURE_MANIFEST}"
     CONTENT "${STANLI_FIXTURE_MANIFEST_CONTENT}\n")
set(_stanli_fixture_dependencies
    ${STANLI_FIXTURE_SOURCES}
    "${STANLI_FIXTURE_MANIFEST}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_fixtures.py"
    "${STANLI_STANC_EXECUTABLE}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/stanc3/stanc.src")
  list(APPEND _stanli_fixture_dependencies
       "${CMAKE_CURRENT_SOURCE_DIR}/deps/stanc3/stanc.src")
endif()
add_custom_command(
  OUTPUT ${STANLI_MIR_FIXTURES}
  COMMAND ${STANLI_FIXTURE_PYTHON}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_fixtures.py"
          --stanc "${STANLI_STANC_EXECUTABLE}"
  DEPENDS ${_stanli_fixture_dependencies}
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Generating ${STANLI_FIXTURE_COUNT} legacy MIR fixtures"
  VERBATIM)
add_custom_target(stanli_mir_fixtures ALL DEPENDS ${STANLI_MIR_FIXTURES})

# Pull-request validation uses Clang so its cheap test translation units can
# link to the optimized runtime objects below. Shipped platform builds may use
# their established compiler; this switch changes test scaffolding only when
# the frontend supports the shared-PCH policy we validate in CI.
set(STANLI_FAST_TEST_TUS OFF)
if(NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
  set(STANLI_FAST_TEST_TUS ON)
endif()

if(STANLI_FAST_TEST_TUS)
# One precompiled header for the test targets, built once and reused.
#
# The reason to care is CI, not an ordinary cached build. A change to a widely
# included header invalidates most test objects, and roughly 3.6 seconds of
# every test translation unit is otherwise spent re-reading <stan/math.hpp>.
# CMake's default per-target PCH would build one copy for every executable, so
# REUSE_FROM is essential here.
#
# recorder.hpp must come first. stan-math's prim value_of calls value_of
# unqualified from inside a template, so stanli's rvar overload must be
# declared before that template is defined.
set(STANLI_TEST_PCH <stanli/recorder.hpp> <stan/math.hpp>)

# REUSE_FROM requires identical compiler flags between the PCH owner and its
# consumers. Both therefore compile at -O0. These flags affect only the test
# scaffolding; the linked stanli runtime object library remains a Release build.
function(stanli_configure_test_tu NAME)
  target_compile_options(${NAME} PRIVATE -O0)
  target_precompile_headers(${NAME} REUSE_FROM stanli_test_pch)
endfunction()

# The owner exists only to produce the shared PCH. Do not use tbb_stub.cpp as
# its source: force-including stan-math would collide with the class definition
# in the stub's non-Apple branch.
add_library(stanli_test_pch OBJECT tests/pch_anchor.cpp)
target_link_libraries(stanli_test_pch PRIVATE ${STANLI_TEST_RUNTIME_TARGET})
target_compile_options(stanli_test_pch PRIVATE -O0)
target_precompile_headers(stanli_test_pch PRIVATE ${STANLI_TEST_PCH})
endif()

# Single-threaded tests stub the one TBB symbol stan-math's rev core needs.
function(stanli_add_test NAME)
  add_executable(${NAME} tests/${NAME}.cpp
    $<TARGET_OBJECTS:stanli_tbb_stub>)
  add_dependencies(${NAME} stanli_mir_fixtures)
  target_link_libraries(${NAME} PRIVATE ${STANLI_TEST_RUNTIME_TARGET})
  if(STANLI_FAST_TEST_TUS)
    stanli_configure_test_tu(${NAME})
  endif()
  # Tests run from the repo root so fixture paths are stable.
  add_test(NAME ${NAME} COMMAND ${NAME}
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

# The native sampler. With the embed object it is one self-contained
# binary -- .stan in, CmdStan-shaped CSV out, no toolchain and no separate
# stanc to find -- which is the whole point of an interpreter for a
# language whose reference implementation compiles C++ per model.
add_library(stanli_stanc_process STATIC tools/stanc_process.cpp)
add_executable(stanli_run tools/stanli_run.cpp
  $<TARGET_OBJECTS:stanli_tbb_stub>)
target_link_libraries(stanli_run PRIVATE stanli stanli_stanc_process)
stanli_embed_stanc(stanli_run)
add_executable(stanli_check tools/stanli_check.cpp
  $<TARGET_OBJECTS:stanli_tbb_stub>)
target_link_libraries(stanli_check PRIVATE stanli stanli_stanc_process)
add_dependencies(stanli_check stanli_mir_fixtures)
add_executable(test_stanc_process tests/test_stanc_process.cpp)
target_link_libraries(test_stanc_process PRIVATE stanli_stanc_process)
add_test(NAME test_stanc_process COMMAND test_stanc_process)
add_test(NAME test_check_precompiled_mir
         COMMAND stanli_check tests/fixtures/ar1.stan tests/fixtures/ar1.json
                 --mir tests/fixtures/ar1.tmir.sexp
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_check_precompiled_mir PROPERTIES
  PASS_REGULAR_EXPRESSION "OK ")
add_test(NAME test_check_precompiled_mir_conflict
         COMMAND ${CMAKE_COMMAND}
                 -DSTANLI_CHECK=$<TARGET_FILE:stanli_check>
                 -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_mir_conflict.cmake)
add_executable(bench_grad tools/bench_grad.cpp
  $<TARGET_OBJECTS:stanli_tbb_stub>)
target_link_libraries(bench_grad PRIVATE stanli)
add_dependencies(bench_grad stanli_mir_fixtures)
add_executable(bench_mir_decode tools/bench_mir_decode.cpp
  $<TARGET_OBJECTS:stanli_tbb_stub>)
target_link_libraries(bench_mir_decode PRIVATE stanli)
add_dependencies(bench_mir_decode stanli_mir_fixtures)
add_test(NAME test_prep_only
         COMMAND bench_grad tests/fixtures/ar1.tmir.sexp
                 tests/fixtures/ar1.json --prep
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_prep_only PROPERTIES
  ENVIRONMENT "STANLI_PROFILE_PREP=0"
  PASS_REGULAR_EXPRESSION "[0-9]+\\.[0-9]+ 24"
  FAIL_REGULAR_EXPRESSION "stanli_prep")
add_test(NAME test_prep_profile
         COMMAND bench_grad tests/fixtures/ar1.tmir.sexp
                 tests/fixtures/ar1.json --prep
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_prep_profile PROPERTIES
  ENVIRONMENT "STANLI_PROFILE_PREP=1"
  PASS_REGULAR_EXPRESSION
    "stanli_prep graph=driver stage=total ns=[0-9]+")
add_test(NAME test_prep_profile_reroll_dispositions
         COMMAND bench_grad tests/fixtures/ar1.tmir.sexp
                 tests/fixtures/ar1.json --prep
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_prep_profile_reroll_dispositions PROPERTIES
  ENVIRONMENT "STANLI_PROFILE_PREP=1"
  PASS_REGULAR_EXPRESSION
    "packed_rows=[0-9]+ term_density=[0-9]+ element_density=[0-9]+ term_widen=[0-9]+ element_store=[0-9]+")
add_executable(bench_opcost tools/bench_opcost.cpp
  $<TARGET_OBJECTS:stanli_tbb_stub>)
target_link_libraries(bench_opcost PRIVATE stanli)
# Developer-only Program::CALL dispatch/context benchmark.  It builds a
# synthetic, model-independent register program so CALL overhead can be
# measured without attributing a kernel's own arithmetic to the mechanism.
add_executable(bench_program_call EXCLUDE_FROM_ALL
  tools/bench_program_call.cpp tests/tbb_stub.cpp)
target_link_libraries(bench_program_call PRIVATE stanli)
# Developer-only Executor metadata/clone evaluator. Fresh baseline/candidate
# processes make its peak-RSS output comparable across layout changes.
add_executable(bench_executor_clone EXCLUDE_FROM_ALL
  tools/bench_executor_clone.cpp tests/tbb_stub.cpp)
target_link_libraries(bench_executor_clone PRIVATE stanli)
add_test(NAME test_bench_opcost_smoke COMMAND bench_opcost --smoke)
set_tests_properties(test_bench_opcost_smoke PROPERTIES
  PASS_REGULAR_EXPRESSION "sink=")
# Developer-only ODE RHS generated-adjoint spike.  Not a test and not installed.
add_executable(bench_rhs_adjoint EXCLUDE_FROM_ALL
  tools/bench_rhs_adjoint.cpp tests/tbb_stub.cpp)
target_link_libraries(bench_rhs_adjoint PRIVATE stanli)
add_dependencies(bench_rhs_adjoint stanli_mir_fixtures)
# Developer-only complete-solve sensitivity ceilings.  They retain the merged
# runtime as the oracle and install no experimental path in libstanli.
add_executable(bench_ode_ceiling EXCLUDE_FROM_ALL
  tools/bench_ode_ceiling.cpp tests/tbb_stub.cpp)
target_link_libraries(bench_ode_ceiling PRIVATE stanli)
add_executable(bench_ode_cvodes_ceiling EXCLUDE_FROM_ALL
  tools/bench_ode_cvodes_ceiling.cpp tests/tbb_stub.cpp)
target_link_libraries(bench_ode_cvodes_ceiling PRIVATE stanli)
add_executable(bench_dispatch tools/bench_dispatch.cpp)
add_executable(dump_ops tools/dump_ops.cpp
  $<TARGET_OBJECTS:stanli_tbb_stub>)
target_link_libraries(dump_ops PRIVATE stanli)
add_executable(dump_islands tools/dump_islands.cpp
  $<TARGET_OBJECTS:stanli_tbb_stub>)
target_link_libraries(dump_islands PRIVATE stanli)

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

set(STANLI_TESTS
  test_smoke test_executor test_recorder test_densities test_matvec test_eigen
  test_mnc
  test_legacy test_eight_schools test_glm test_nuts test_walnuts test_sexp
  test_mir test_mir_decode test_transforms test_eltwise test_scalar_binary
  test_lower
  test_lower_views
  test_lower_view_contract test_lower_array_contract test_data test_e2e
  test_reroll test_profile test_inplace test_pass_safety test_sampling
  test_ode_prog test_mir_program_conformance test_mir_checks
  test_structured_checks test_mir_unary_fallback test_constfold test_cse
  test_partition
  test_solve test_cross_path
  test_write_array test_pool test_bridgestan test_mixture test_island
  test_adjoint test_diagnose test_multichain test_newtrans test_rejectprint
  test_odevariadic test_algebra test_estimate test_brmsmono
  test_structured_arrays test_pathfinder test_unconstrain
  test_init_interp)
foreach(STANLI_TEST IN LISTS STANLI_TESTS)
  stanli_add_test(${STANLI_TEST})
endforeach()

# The C ABI lives only in the shared library. Test it through that shipped
# boundary rather than compiling capi.cpp into the C++ unit-test library.
# Under the fast-test policy it still gets -O0, but deliberately not the shared
# PCH: force-including stan-math would pull TBB references across the C boundary
# this executable is meant to verify in isolation.
add_executable(test_capi tests/test_capi.cpp)
target_link_libraries(test_capi PRIVATE stanli_shared)
add_dependencies(test_capi stanli_mir_fixtures)
if(STANLI_FAST_TEST_TUS)
  target_compile_options(test_capi PRIVATE -O0)
endif()
add_test(NAME test_capi COMMAND test_capi
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_function tests/test_function.cpp)
target_link_libraries(test_function PRIVATE stanli_shared)
add_dependencies(test_function stanli_mir_fixtures)
if(STANLI_FAST_TEST_TUS)
  target_compile_options(test_function PRIVATE -O0)
endif()
add_test(NAME test_function COMMAND test_function
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# The conformance gate has its own failure modes (parser drift, a broad policy
# rule, nonfinite comparison, partition loss) which C++ runtime tests cannot
# exercise. Python 3.11 supplies the TOML parser in the standard library;
# standalone runs on older Python use the Tomli backport in requirements.txt.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME test_corpus_bench
           COMMAND ${Python3_EXECUTABLE} tests/test_corpus_bench.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME test_gen_web_models
           COMMAND ${Python3_EXECUTABLE} tests/test_gen_web_models.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME test_conformance_harness
           COMMAND ${Python3_EXECUTABLE} tests/test_conformance.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME test_vectorize_ab
           COMMAND ${Python3_EXECUTABLE} tests/test_vectorize_ab.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  # Source-level lit cases are one marked .stan file each. CMake discovers
  # both the dedicated tests/lit tree and existing fixtures that opt in, then
  # gives every case its own CTest entry. Adding coverage needs no build-system
  # edit and no new C++ target. The runner invokes stanli_check, whose compiler
  # is provisioned by the ordinary developer setup; a missing compiler is a
  # test failure rather than a skip.
  file(GLOB_RECURSE STANLI_LIT_CANDIDATES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.stan")
  # The PASS/XFAIL marker controls the broken label below, so changing a
  # source's status must re-run configuration even though the file set did
  # not change.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${STANLI_LIT_CANDIDATES})
  foreach(STANLI_LIT_CASE IN LISTS STANLI_LIT_CANDIDATES)
    file(STRINGS "${STANLI_LIT_CASE}" STANLI_LIT_MARKER LIMIT_COUNT 1
         REGEX "^// STANLI-LIT: (PASS|XFAIL)$")
    if(STANLI_LIT_MARKER)
      file(RELATIVE_PATH STANLI_LIT_REL
           "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${STANLI_LIT_CASE}")
      string(REGEX REPLACE "^lit/" "" STANLI_LIT_REL "${STANLI_LIT_REL}")
      string(REGEX REPLACE "\\.stan$" "" STANLI_LIT_NAME
             "${STANLI_LIT_REL}")
      string(REGEX REPLACE "[^A-Za-z0-9_]" "_" STANLI_LIT_NAME
             "${STANLI_LIT_NAME}")
      set(STANLI_LIT_TEST "lit_${STANLI_LIT_NAME}")
      add_test(NAME ${STANLI_LIT_TEST}
               COMMAND ${Python3_EXECUTABLE} tests/lit/run.py
                       ${STANLI_LIT_CASE}
                       $<TARGET_FILE:stanli_check>
               WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
      set(STANLI_LIT_LABELS lit)
      if(STANLI_LIT_MARKER MATCHES "XFAIL$")
        list(APPEND STANLI_LIT_LABELS broken)
      endif()
      set_tests_properties(${STANLI_LIT_TEST} PROPERTIES
        LABELS "${STANLI_LIT_LABELS}")
    endif()
  endforeach()
  # Friendly local entry points. These deliberately invoke CTest rather than
  # duplicating discovery in a shell script, so a developer runs exactly the
  # same per-file tests and labels as CI. STANLI_LIT_JOBS is a cache setting
  # for smaller machines; the repository default matches the j24 developer
  # workflow used for the native build.
  set(STANLI_LIT_JOBS "24" CACHE STRING
      "Parallel jobs used by the lit and lit-broken targets")
  add_custom_target(lit
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            --parallel ${STANLI_LIT_JOBS} -L lit
    DEPENDS stanli_check
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running source-level Stan lit tests")
  add_custom_target(lit-broken
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            --parallel ${STANLI_LIT_JOBS} -L broken
    DEPENDS stanli_check
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running known-broken Stan lit tests")
  # The model census's coverage ratchet, over synthetic rows plus the
  # checked-in baseline. The census itself needs deps/stanc3-src, a stanc
  # and a CmdStan and so runs by hand; what runs here is the gate it grew,
  # which is the part that decides whether a run failed. Pure Python and no
  # shell stub, so no WIN32 guard.
  add_test(NAME test_census
           COMMAND ${Python3_EXECUTABLE} tests/test_census.py
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  # The corpus replay's per-point verdicts, over a stub stanli_check. The
  # oracle itself needs posteriordb and a real binary and so runs in CI
  # rather than here; what runs here is the classification it depends on,
  # which is what let a segfault at an unreferenced point go unreported.
  #
  # Not on Windows: the stub is a /bin/sh script that kills itself with
  # SIGSEGV, and what the test asserts is that a negative returncode is
  # read as a crash. Both halves are POSIX, and so is the corpus check
  # this defends, which only ever runs on Linux and macOS.
  if(NOT WIN32)
    add_test(NAME test_verify_refs
             COMMAND ${Python3_EXECUTABLE} tests/test_verify_refs.py
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  endif()
endif()
endif()  # NOT EMSCRIPTEN
