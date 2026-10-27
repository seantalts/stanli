get_filename_component(_STANLI_EMBED_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

find_program(STANLI_BASH bash)

function(_stanli_bash_in_root OUT_RESULT OUT_TEXT SCRIPT)
  execute_process(
    COMMAND ${STANLI_BASH} -c "source tools/stanc_embed/provenance.sh && ${SCRIPT}"
            stanli ${ARGN}
    WORKING_DIRECTORY "${_STANLI_EMBED_ROOT}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _text
    ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(${OUT_RESULT} "${_result}" PARENT_SCOPE)
  set(${OUT_TEXT} "${_text}" PARENT_SCOPE)
endfunction()

# STANLI_STANC_EMBED_OBJ is empty (no embedded compiler), "auto" (the object
# this tree's sources produce, in deps/stanc3/embed/<key>/), or a path to
# an object. Either way the object must carry the provenance stamp for
# this tree's compiler inputs, so a stale one cannot be linked.
function(stanli_resolve_stanc_embed)
  if(NOT STANLI_STANC_EMBED_OBJ)
    return()
  endif()
  if(NOT STANLI_BASH)
    message(FATAL_ERROR
      "bash is required to check the embedded stanc object's provenance")
  endif()
  file(GLOB _inputs LIST_DIRECTORIES false CONFIGURE_DEPENDS
       "${_STANLI_EMBED_ROOT}/compiler/native/*"
       "${_STANLI_EMBED_ROOT}/compiler/ocaml/*"
       "${_STANLI_EMBED_ROOT}/tools/stanc_embed/*")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${_inputs} "${_STANLI_EMBED_ROOT}/deps/pins.env")

  set(_object "${STANLI_STANC_EMBED_OBJ}")
  if(_object STREQUAL "auto")
    _stanli_bash_in_root(_result _directory stanc_embed_dir)
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR "Could not compute the embedded stanc directory")
    endif()
    set(_object "${_directory}/stanc_embed.o")
  endif()
  file(TO_CMAKE_PATH "${_object}" _object)

  _stanli_bash_in_root(_result _unused "stanc_embed_artifact_matches \"$1\"" "${_object}")
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR
      "${_object} is missing or does not match this tree's pinned stanc3 "
      "source and compiler inputs. Build it with: tools/dev_setup.sh --embed --no-build "
      "(or tools/stanc_embed/build.sh deps/stanc3-src)")
  endif()
  set(STANLI_STANC_EMBED_OBJ "${_object}" PARENT_SCOPE)
endfunction()

# The default stanc executable must come from the pinned source revision.
function(stanli_check_default_stanc DEFAULT_PATH)
  if(NOT STANLI_STANC_EXECUTABLE STREQUAL DEFAULT_PATH)
    return()
  endif()
  get_filename_component(_directory "${DEFAULT_PATH}" DIRECTORY)
  set(_source_file "${_directory}/stanc.src")
  file(STRINGS "${_STANLI_EMBED_ROOT}/deps/pins.env" _pin
       REGEX "^STANC3_SRC_SHA=" LIMIT_COUNT 1)
  string(REPLACE "STANC3_SRC_SHA=" "" _pin "${_pin}")
  set(_built "")
  if(EXISTS "${_source_file}")
    file(STRINGS "${_source_file}" _built LIMIT_COUNT 1)
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${_STANLI_EMBED_ROOT}/deps/pins.env")
  if(EXISTS "${_source_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${_source_file}")
  endif()
  if(NOT _built STREQUAL _pin)
    message(FATAL_ERROR
      "${DEFAULT_PATH} was built from stanc3 '${_built}' but deps/pins.env "
      "pins '${_pin}'. Rebuild it with: tools/dev_setup.sh --no-build")
  endif()
endfunction()
