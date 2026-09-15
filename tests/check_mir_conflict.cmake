execute_process(
  COMMAND "${STANLI_CHECK}"
          "${SOURCE_DIR}/tests/fixtures/ar1.stan"
          "${SOURCE_DIR}/tests/fixtures/ar1.json"
          --mir "${SOURCE_DIR}/tests/fixtures/ar1.tmir.sexp"
          --stanc ignored
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)

if(NOT result EQUAL 2)
  message(FATAL_ERROR
          "stanli_check --mir/--stanc conflict returned ${result}: ${output}${error}")
endif()

set(expected "stanli_check: --stanc, --stanli-compile and --mir are mutually exclusive")
string(FIND "${error}" "${expected}" diagnostic_position)
if(diagnostic_position EQUAL -1)
  message(FATAL_ERROR
          "stanli_check --mir/--stanc conflict missed diagnostic: ${error}")
endif()
