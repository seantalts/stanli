set(work "${WORK_DIR}")
file(MAKE_DIRECTORY "${work}")
configure_file("${SOURCE_DIR}/tests/fixtures/ar1.stan" "${work}/ar1.stan" COPYONLY)
configure_file("${SOURCE_DIR}/tests/fixtures/ar1.json" "${work}/ar1.json" COPYONLY)

function(expect_result label expected_code expected_line)
  execute_process(
    COMMAND "${STANLI_CHECK}" "${work}/ar1.stan" "${work}/ar1.json" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL expected_code OR NOT output MATCHES "(^|\n)${expected_line}")
    message(FATAL_ERROR
            "stanli_check ${label} returned ${result}: ${output}${error}")
  endif()
endfunction()

expect_result("--stanc ${STANC}" 0 "OK " --stanc "${STANC}")
expect_result("--stanc with a missing executable" 1 "COMPILE_FAIL "
              --stanc "${work}/no-such-stanc")
expect_result("--stanli-compile with a missing executable" 1 "COMPILE_FAIL "
              --stanli-compile "${work}/no-such-stanli-compile")

if(NOT WIN32)
  # A stand-in stanli-compile: prints already-compiled MIR for its model.
  set(fake "${work}/stanli-compile")
  file(WRITE "${fake}"
       "#!/bin/sh\nexec cat \"${SOURCE_DIR}/tests/fixtures/ar1.tmir.sexp\"\n")
  execute_process(COMMAND chmod +x "${fake}")
  expect_result("--stanli-compile ${fake}" 0 "OK " --stanli-compile "${fake}")
endif()
