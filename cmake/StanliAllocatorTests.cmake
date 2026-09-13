# Also used by the small, standalone five-platform CI ownership gate.
function(stanli_add_allocator_probe)
  # Model both the reused runtime objects and a separately linked C archive.
  # Neither original may be rewritten while constructing the private DSO.
  add_library(stanli_allocator_reused OBJECT
    "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/private_allocator_reused.cpp")
  add_library(stanli_allocator_dependency STATIC
    "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/private_allocator_dependency.cpp")
  set_target_properties(stanli_allocator_reused stanli_allocator_dependency
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
  add_library(stanli_allocator_test_probe SHARED
    "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/private_allocator_probe.cpp"
    $<TARGET_OBJECTS:stanli_allocator_reused>)
  target_link_libraries(stanli_allocator_test_probe PRIVATE
    stanmath Threads::Threads stanli_allocator_dependency)
  # The input source queries ownership. mimalloc is linked by the integration
  # function, after the copied archive has been made.
  target_include_directories(stanli_allocator_test_probe PRIVATE
    "${STANLI_ALLOCATOR_SOURCE_DIR}/deps/mimalloc/include")
  if(APPLE)
    target_link_options(stanli_allocator_test_probe PRIVATE
      "-Wl,-exported_symbols_list,${STANLI_ALLOCATOR_SOURCE_DIR}/tests/private_allocator_exports.txt")
    set_property(TARGET stanli_allocator_test_probe APPEND PROPERTY LINK_DEPENDS
      "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/private_allocator_exports.txt")
  elseif(WIN32)
    target_sources(stanli_allocator_test_probe PRIVATE
      "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/private_allocator_exports.def")
    target_link_options(stanli_allocator_test_probe PRIVATE -static)
  else()
    target_link_options(stanli_allocator_test_probe PRIVATE -static-libstdc++ -static-libgcc
      "-Wl,--version-script,${STANLI_ALLOCATOR_SOURCE_DIR}/tests/private_allocator_exports.map")
    set_property(TARGET stanli_allocator_test_probe APPEND PROPERTY LINK_DEPENDS
      "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/private_allocator_exports.map")
  endif()
  stanli_link_private_allocator(stanli_allocator_test_probe)
  add_executable(test_private_allocator
    "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/test_private_allocator.cpp"
    $<TARGET_OBJECTS:stanli_allocator_reused>)
  target_include_directories(test_private_allocator PRIVATE
    "${STANLI_ALLOCATOR_SOURCE_DIR}/runtime/include")
  target_link_libraries(test_private_allocator PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(test_private_allocator stanli_allocator_test_probe)
  add_test(NAME test_private_allocator_probe_exports
    COMMAND "${CMAKE_COMMAND}" -DLIBRARY=$<TARGET_FILE:stanli_allocator_test_probe>
      -DNM=${CMAKE_NM} -DOBJDUMP=${CMAKE_OBJDUMP} -DPLATFORM=${CMAKE_SYSTEM_NAME} -DPROBE=ON
      -P "${STANLI_ALLOCATOR_SOURCE_DIR}/tests/check_allocator_exports.cmake")
endfunction()
