execute_process(COMMAND "${EXE}" --help
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR output MATCHES "--library")
  message(FATAL_ERROR "Unexpected native-only help: ${result}\n${output}\n${error}")
endif()

# Removed backend selection must fail at argument parsing, before opening
# hardware or creating capture files, for both entry points.
foreach(command IN ITEMS list capture)
  set(args)
  if(command STREQUAL "capture")
    set(args --speed high --output "${CMAKE_CURRENT_BINARY_DIR}/cli-rejected.pcapng")
  endif()
  execute_process(COMMAND "${EXE}" "${command}" ${args} --library missing-vendor-library
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 2 OR NOT output MATCHES "\"code\":\"usage\"")
    message(FATAL_ERROR "${command} accepted legacy selection: ${result}\n${output}\n${error}")
  endif()
endforeach()
