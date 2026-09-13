# A DSO-local C allocator, not a process-wide malloc/new override. Never
# attach the shim to stanmath, the reusable runtime objects, or the CLI.
get_filename_component(STANLI_ALLOCATOR_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# Linux defaults are validated independently of the unresolved Apple
# large-vector regression. AUTO still fails closed for unsupported builds.
# Preserve any explicit/cached selection, including a SYSTEM opt-out.
set(_stanli_allocator_default SYSTEM)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_stanli_allocator_default AUTO)
endif()
set(STANLI_SHARED_ALLOCATOR "${_stanli_allocator_default}" CACHE STRING
    "Shared-library C allocator: AUTO, SYSTEM, or MIMALLOC")
set_property(CACHE STANLI_SHARED_ALLOCATOR PROPERTY STRINGS AUTO SYSTEM MIMALLOC)
if(NOT STANLI_SHARED_ALLOCATOR MATCHES "^(AUTO|SYSTEM|MIMALLOC)$")
  message(FATAL_ERROR "STANLI_SHARED_ALLOCATOR must be AUTO, SYSTEM, or MIMALLOC")
endif()

set(_stanli_allocator_arch "${CMAKE_OSX_ARCHITECTURES}")
if(NOT _stanli_allocator_arch)
  set(_stanli_allocator_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(_stanli_allocator_flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}")
set(_stanli_allocator_ipo "${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
set(_stanli_allocator_configs DEBUG RELEASE RELWITHDEBINFO MINSIZEREL
    ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
foreach(_config IN LISTS _stanli_allocator_configs)
  string(TOUPPER "${_config}" _config)
  string(APPEND _stanli_allocator_flags
    " ${CMAKE_C_FLAGS_${_config}} ${CMAKE_CXX_FLAGS_${_config}}")
  if(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${_config})
    set(_stanli_allocator_ipo ON)
  endif()
endforeach()
set(STANLI_PRIVATE_ALLOCATOR_SUPPORTED OFF)
set(_stanli_allocator_native OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND
   _stanli_allocator_arch MATCHES "^(arm64|x86_64|arm64;x86_64|x86_64;arm64)$")
  set(_stanli_allocator_native ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
       _stanli_allocator_arch MATCHES "^(aarch64|arm64|x86_64|AMD64)$")
  set(_stanli_allocator_native ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW AND
       _stanli_allocator_arch MATCHES "^(x86_64|AMD64|amd64)$")
  set(_stanli_allocator_native ON)
endif()
if(_stanli_allocator_native AND NOT EMSCRIPTEN AND NOT STANLI_SANITIZE AND
   NOT _stanli_allocator_flags MATCHES "(-fsanitize=|-flto|/GL|/fsanitize)" AND
   NOT _stanli_allocator_ipo AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
  set(STANLI_PRIVATE_ALLOCATOR_SUPPORTED ON)
endif()
set(STANLI_USE_PRIVATE_MIMALLOC OFF)
if(STANLI_SHARED_ALLOCATOR STREQUAL "MIMALLOC" AND
   NOT STANLI_PRIVATE_ALLOCATOR_SUPPORTED)
  message(FATAL_ERROR
    "Private mimalloc requires CMake >= 3.21 and non-sanitized, non-LTO macOS/Linux arm64 or x86_64, or MinGW Windows x86_64; use AUTO or SYSTEM")
elseif(NOT STANLI_SHARED_ALLOCATOR STREQUAL "SYSTEM" AND
       STANLI_PRIVATE_ALLOCATOR_SUPPORTED)
  set(STANLI_USE_PRIVATE_MIMALLOC ON)
endif()
if(STANLI_USE_PRIVATE_MIMALLOC)
  message(STATUS "Stanli shared C allocator: private mimalloc 3.5.1 (C++ new/delete unchanged)")
else()
  message(STATUS "Stanli shared C allocator: system")
endif()

function(stanli_link_private_allocator target)
  if(NOT STANLI_USE_PRIVATE_MIMALLOC)
    return()
  endif()
  if(CMAKE_VERSION VERSION_LESS 3.21)
    message(FATAL_ERROR "Private mimalloc requires CMake >= 3.21; upgrade or select SYSTEM")
  endif()
  # Read the pin from the dependency fetcher, rather than maintain two pins.
  file(STRINGS "${STANLI_ALLOCATOR_SOURCE_DIR}/deps/fetch.sh" _pin_line
       REGEX "^MIMALLOC_SHA=")
  string(REGEX REPLACE "^MIMALLOC_SHA=([0-9a-f]+).*" "\\1" _pin "${_pin_line}")
  set(_source "${STANLI_ALLOCATOR_SOURCE_DIR}/deps/mimalloc")
  execute_process(COMMAND git -C "${_source}" rev-parse HEAD
    OUTPUT_VARIABLE _actual OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT EXISTS "${_source}/CMakeLists.txt" OR NOT _actual STREQUAL _pin)
    message(FATAL_ERROR
      "Private mimalloc needs deps/mimalloc at ${_pin}; run deps/fetch.sh or select SYSTEM")
  endif()
  # Normal, function-local variables: upstream option() honors these without
  # forcing process-wide override choices into the parent project's cache.
  set(MI_OVERRIDE OFF)
  set(MI_OSX_INTERPOSE OFF)
  set(MI_OSX_ZONE OFF)
  set(MI_WIN_REDIRECT OFF)
  set(MI_BUILD_SHARED OFF)
  set(MI_BUILD_OBJECT OFF)
  set(MI_BUILD_STATIC ON)
  set(MI_BUILD_TESTS OFF)
  set(MI_NO_USE_CXX ON)
  set(MI_NO_OPT_ARCH ON)
  # Preserve scoped string settings through upstream set(... CACHE ...).
  set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MI_TLS_MODEL LOCAL_DYNAMIC)
  elseif(APPLE)
    set(MI_TLS_MODEL PTHREADS)
  elseif(WIN32)
    set(MI_TLS_MODEL WIN32)
  endif()
  if(NOT TARGET mimalloc-static)
    add_subdirectory("${_source}" "${CMAKE_BINARY_DIR}/private-mimalloc" EXCLUDE_FROM_ALL)
    set_property(TARGET mimalloc-static PROPERTY STANLI_PRIVATE_ALLOCATOR_OWNER TRUE)
  else()
    get_target_property(_owned mimalloc-static STANLI_PRIVATE_ALLOCATOR_OWNER)
    if(NOT _owned)
      message(FATAL_ERROR
        "An existing mimalloc-static target is not Stanli's private/no-override build; select SYSTEM")
    endif()
  endif()
  target_compile_definitions(${target} PRIVATE STANLI_PRIVATE_MIMALLOC=1)
  if(APPLE)
    # Mach-O's two-level namespace and the existing C API export allowlist
    # bind these C functions locally, without changing external libc++.
    target_sources(${target} PRIVATE
      "${STANLI_ALLOCATOR_SOURCE_DIR}/runtime/src/private_allocator_macos.cpp")
  else()
    # Do NOT wrap at the final link: Linux/MinGW statically link libstdc++,
    # and its new/delete implementations must keep their system C allocator.
    # Rewrite a COPY of only this target's sources/objects. The reusable
    # objects (CLI/tests), SUNDIALS and the C++ runtime remain untouched.
    if(NOT CMAKE_OBJCOPY)
      message(FATAL_ERROR "Private mimalloc needs the toolchain's objcopy; select SYSTEM to disable")
    endif()
    get_target_property(_sources ${target} SOURCES)
    # Export definition files belong to the final DLL, not its input archive.
    set(_final_sources)
    foreach(_source_file IN LISTS _sources)
      if(_source_file MATCHES "\\.def$")
        list(APPEND _final_sources "${_source_file}")
        list(REMOVE_ITEM _sources "${_source_file}")
      endif()
    endforeach()
    add_library(${target}_allocator_input STATIC ${_sources})
    set_target_properties(${target}_allocator_input PROPERTIES POSITION_INDEPENDENT_CODE ON)
    foreach(_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES)
      get_target_property(_value ${target} ${_property})
      if(_value)
        set_property(TARGET ${target}_allocator_input PROPERTY ${_property} "${_value}")
      endif()
    endforeach()
    get_target_property(_links ${target} LINK_LIBRARIES)
    if(_links)
      target_link_libraries(${target}_allocator_input PRIVATE ${_links})
    endif()
    set(_archive "${CMAKE_CURRENT_BINARY_DIR}/${target}-private-$<CONFIG>.a")
    set(_rename_args)
    foreach(_symbol malloc calloc realloc free aligned_alloc posix_memalign valloc)
      list(APPEND _rename_args "--redefine-sym=${_symbol}=stli_mi_${_symbol}")
      if(WIN32)
        list(APPEND _rename_args "--redefine-sym=__imp_${_symbol}=stli_mi_import_${_symbol}")
      endif()
    endforeach()
    add_custom_command(OUTPUT "${_archive}"
      COMMAND "${CMAKE_OBJCOPY}" ${_rename_args}
        "$<TARGET_FILE:${target}_allocator_input>" "${_archive}"
      DEPENDS ${target}_allocator_input
      COMMENT "Privatizing ${target}'s C allocation references (not libstdc++)"
      VERBATIM)
    add_custom_target(${target}_allocator_archive DEPENDS "${_archive}")
    add_dependencies(${target} ${target}_allocator_archive)
    set_property(TARGET ${target} PROPERTY SOURCES
      "${STANLI_ALLOCATOR_SOURCE_DIR}/runtime/src/private_allocator.cpp" ${_final_sources})
    # All original objects were direct shared-library inputs. Keep them all,
    # including registration/constructor-only objects, after archiving.
    # Dependency archives such as SUNDIALS must follow our archive, so their
    # members can satisfy the references it introduces (GNU link order).
    set_property(TARGET ${target} PROPERTY LINK_LIBRARIES "")
    target_link_libraries(${target} PRIVATE
      "-Wl,--whole-archive" "${_archive}" "-Wl,--no-whole-archive" ${_links})
  endif()
  target_link_libraries(${target} PRIVATE mimalloc-static)
endfunction()
