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
# dpotri comes from Accelerate's newer LAPACK interface (dpotri$NEWLAPACK),
# whose threaded inverse is right where the legacy dpotri_ is sometimes
# wrong. The others keep the legacy names.
set(routines dgemm_ dgemv_ dpotrf_ dpotri$NEWLAPACK dtrsm_ dtrmm_ dsymm_
             dtrtri_ dtrmv_)
set(imported "")
foreach(routine IN LISTS routines)
  string(REPLACE "$" "\\$" pattern "${routine}")
  if(symbols MATCHES "(^|\n)[ \t]*_?${pattern}(\n|$)")
    list(APPEND imported ${routine})
  endif()
endforeach()

if(symbols MATCHES "(^|\n)[ \t]*_?dpotri_(\n|$)")
  message(FATAL_ERROR "${LIBRARY} imports the legacy dpotri_")
endif()
if(EXPECTED STREQUAL "none")
  if(imported)
    message(FATAL_ERROR "${LIBRARY} imports ${imported} but the build has no BLAS provider")
  endif()
  if(symbols MATCHES "BLASSetThreading|BLASGetThreading")
    message(FATAL_ERROR "${LIBRARY} imports Accelerate's threading controls but the build has no BLAS provider")
  endif()
  if(APPLE OR CMAKE_HOST_APPLE)
    execute_process(COMMAND otool -L "${LIBRARY}" OUTPUT_VARIABLE libraries)
    if(libraries MATCHES "Accelerate")
      message(FATAL_ERROR "${LIBRARY} links Accelerate but the build has no BLAS provider:\n${libraries}")
    endif()
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
  # The binary targets macOS 11.0, and dpotri$NEWLAPACK exists from macOS
  # 13.3, so the library loads on older systems only with a weak import.
  execute_process(COMMAND "${NM}" -m -u "${LIBRARY}"
                  RESULT_VARIABLE result OUTPUT_VARIABLE described)
  if(NOT result EQUAL 0 OR
     NOT described MATCHES "weak external _dpotri\\$NEWLAPACK ")
    message(FATAL_ERROR "${LIBRARY} does not import dpotri$NEWLAPACK weakly:\n${described}")
  endif()
endif()
