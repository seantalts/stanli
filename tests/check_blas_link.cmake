# The shipped library imports the BLAS and LAPACK routines of its large
# dense kernels exactly when the build is configured with a BLAS provider.
# The runtime's own report (test_blas_backend) only reflects a macro, so this
# reads the library's imported symbols.
find_program(NM nm)
if(NOT NM)
  message(FATAL_ERROR "nm not found")
endif()
execute_process(COMMAND "${NM}" -u "${LIBRARY}"
                RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "nm -u ${LIBRARY} failed: ${error}")
endif()
set(routines dgemm_ dgemv_ dpotrf_ dtrsm_ dtrmm_ dsymm_ dtrtri_ dtrmv_)
set(imported "")
foreach(routine IN LISTS routines)
  if(symbols MATCHES "(^|\n)[ \t]*_?${routine}(\n|$)")
    list(APPEND imported ${routine})
  endif()
endforeach()

if(EXPECTED STREQUAL "none")
  if(imported)
    message(FATAL_ERROR "${LIBRARY} imports ${imported} but the build has no BLAS provider")
  endif()
  return()
endif()

if(NOT imported STREQUAL routines)
  message(FATAL_ERROR "${LIBRARY} imports only [${imported}] of [${routines}]")
endif()
if(EXPECTED STREQUAL "accelerate")
  execute_process(COMMAND otool -L "${LIBRARY}" OUTPUT_VARIABLE libraries)
  if(NOT libraries MATCHES "/Accelerate.framework/")
    message(FATAL_ERROR "${LIBRARY} does not link Accelerate:\n${libraries}")
  endif()
endif()
