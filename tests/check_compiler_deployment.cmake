# Exercise the actual install rules, then move the installation away from the
# source/build trees. Neither tool may depend on the checkout or PATH.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/empty-path")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}"
          --prefix "${WORK_DIR}/installed" --component NativeTools
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "install failed: ${output}${error}")
endif()
file(RENAME "${WORK_DIR}/installed" "${WORK_DIR}/moved installation")
set(bin "${WORK_DIR}/moved installation/${BINDIR}")
set(portable "${bin}/stanli-compile${EXE_SUFFIX}")
if(EMBEDDED AND EXISTS "${portable}")
  message(FATAL_ERROR "embedded deployment unexpectedly needs a compiler sidecar")
elseif(NOT EMBEDDED AND NOT EXISTS "${portable}")
  message(FATAL_ERROR "standalone deployment omitted stanli-compile")
endif()

configure_file("${SOURCE_DIR}/tests/fixtures/ar1.stan"
               "${WORK_DIR}/model with spaces.stan" COPYONLY)
configure_file("${SOURCE_DIR}/tests/fixtures/ar1.json"
               "${WORK_DIR}/data.json" COPYONLY)
# Leave a valid stock compiler at the old implicit fallback location. A
# missing/broken Stanli compiler must still fail rather than select this one.
file(MAKE_DIRECTORY "${WORK_DIR}/deps/stanc3")
configure_file("${STANC}" "${WORK_DIR}/deps/stanc3/stanc${EXE_SUFFIX}" COPYONLY)
set(ENV{STANC} "")
set(ENV{PATH} "${WORK_DIR}/empty-path")

function(expect_tool tool expected_code)
  set(extra)
  if(tool STREQUAL "stanli_run")
    set(extra --warmup 0 --samples 1 --max-depth 1)
  endif()
  execute_process(
    COMMAND "${bin}/${tool}${EXE_SUFFIX}" "${WORK_DIR}/model with spaces.stan"
            "${WORK_DIR}/data.json" ${extra} ${ARGN}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT "${result}" STREQUAL "${expected_code}")
    message(FATAL_ERROR "${tool} ${ARGN}: expected ${expected_code}, got ${result}: ${output}${error}")
  endif()
  if(expected_code EQUAL 0 AND output STREQUAL "")
    message(FATAL_ERROR "${tool} produced no output")
  endif()
endfunction()

foreach(tool stanli_check stanli_run)
  expect_tool(${tool} 0)
  expect_tool(${tool} 0 --stanc "${STANC}")
  expect_tool(${tool} 1 --stanc "${WORK_DIR}/missing-stanc")
  expect_tool(${tool} 1 --stanli-compile "${WORK_DIR}/missing-compiler")
  expect_tool(${tool} 2 --stanc "${STANC}" --stanli-compile "${portable}")
  if(NOT EMBEDDED)
    expect_tool(${tool} 0 --stanli-compile "${portable}")
  endif()
endforeach()

if(NOT EMBEDDED)
  file(REMOVE "${portable}")
  foreach(tool stanli_check stanli_run)
    expect_tool(${tool} 1)
  endforeach()
endif()
# Stock stanc invoked with stanli-compile's argv emits C++, not portable MIR.
# Embedded tools ignore this broken sidecar; standalone tools must fail.
configure_file("${STANC}" "${portable}" COPYONLY)
foreach(tool stanli_check stanli_run)
  if(EMBEDDED)
    expect_tool(${tool} 0)
  else()
    expect_tool(${tool} 1)
  endif()
endforeach()
