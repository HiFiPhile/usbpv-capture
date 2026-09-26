cmake_minimum_required(VERSION 3.16)

project(usbpv_agent_capture VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

if(WIN32)
  set(USBPV_VENDOR_RUNTIME_DEFAULT OFF)
else()
  set(USBPV_VENDOR_RUNTIME_DEFAULT ON)
endif()
option(USBPV_COPY_VENDOR_RUNTIME
       "Copy the bundled vendor runtime beside usbpv_capture"
       ${USBPV_VENDOR_RUNTIME_DEFAULT})

add_executable(usbpv_capture src/usbpv_capture.cpp src/usbpv_output.cpp)
target_compile_features(usbpv_capture PRIVATE cxx_std_17)
target_include_directories(usbpv_capture PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(MSVC)
  target_compile_options(usbpv_capture PRIVATE /W4 /permissive-)
else()
  target_compile_options(usbpv_capture PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
  # Keep the agent executable independent of whichever MinGW runtime a host
  # process has installed. The vendor DLL uses separately renamed runtimes.
  target_link_options(usbpv_capture PRIVATE -static)
endif()

if(WIN32)
  set(USBPV_FPGA_HEADER "${CMAKE_CURRENT_BINARY_DIR}/usbpv_fpga.hpp")
  add_custom_command(OUTPUT "${USBPV_FPGA_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
      "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/vendor/linux-x64/libusbpv_lib.so"
      "-DOUTPUT=${USBPV_FPGA_HEADER}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-fpga.cmake"
    DEPENDS vendor/linux-x64/libusbpv_lib.so tools/embed-fpga.cmake
    VERBATIM)
  target_sources(usbpv_capture PRIVATE src/usbpv_native.cpp
    src/usbpv_protocol.cpp "${USBPV_FPGA_HEADER}")
  target_include_directories(usbpv_capture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(usbpv_capture PRIVATE winusb setupapi)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(usbpv_device_scan_test tests/device_scan_test.cpp)
  target_compile_features(usbpv_device_scan_test PRIVATE cxx_std_17)
  target_include_directories(usbpv_device_scan_test PRIVATE src)
  if(MINGW)
    target_link_options(usbpv_device_scan_test PRIVATE -static)
  endif()
  add_test(NAME device_scan COMMAND usbpv_device_scan_test)
  add_executable(usbpv_protocol_test tests/protocol_test.cpp src/usbpv_protocol.cpp)
  target_compile_features(usbpv_protocol_test PRIVATE cxx_std_17)
  target_include_directories(usbpv_protocol_test PRIVATE include src)
  if(MINGW)
    target_link_options(usbpv_protocol_test PRIVATE -static)
  endif()
  add_test(NAME protocol COMMAND usbpv_protocol_test)
  add_executable(usbpv_output_test tests/output_test.cpp src/usbpv_output.cpp)
  target_compile_features(usbpv_output_test PRIVATE cxx_std_17)
  target_include_directories(usbpv_output_test PRIVATE src)
  if(MINGW)
    target_link_options(usbpv_output_test PRIVATE -static)
  elseif(UNIX)
    target_link_libraries(usbpv_output_test PRIVATE pthread)
  endif()
  add_test(NAME output COMMAND usbpv_output_test)
  set_tests_properties(output PROPERTIES TIMEOUT 30)
  add_executable(usbpv_queue_test tests/queue_test.cpp)
  target_compile_features(usbpv_queue_test PRIVATE cxx_std_17)
  target_include_directories(usbpv_queue_test PRIVATE src)
  if(MINGW)
    target_link_options(usbpv_queue_test PRIVATE -static)
  elseif(UNIX)
    target_link_libraries(usbpv_queue_test PRIVATE pthread)
  endif()
  add_test(NAME queue COMMAND usbpv_queue_test)
  set_tests_properties(queue PROPERTIES TIMEOUT 30)
  add_executable(usbpv_callback_test tests/callback_test.cpp)
  target_compile_features(usbpv_callback_test PRIVATE cxx_std_17)
  target_include_directories(usbpv_callback_test PRIVATE include src)
  if(MINGW)
    target_link_options(usbpv_callback_test PRIVATE -static)
  elseif(UNIX)
    target_link_libraries(usbpv_callback_test PRIVATE pthread)
  endif()
  add_test(NAME callback COMMAND usbpv_callback_test)
  set_tests_properties(callback PROPERTIES TIMEOUT 30)
endif()

if(UNIX)
  target_link_libraries(usbpv_capture PRIVATE dl pthread)
endif()

set(USBPV_RUNTIME_FILES)
if(WIN32)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(USBPV_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/windows-x64")
    set(USBPV_RUNTIME_FILES
      "${USBPV_RUNTIME_DIR}/usbpv_lib.dll"
      "${USBPV_RUNTIME_DIR}/upvgcc.dll"
      "${USBPV_RUNTIME_DIR}/upvstd.dll"
      "${USBPV_RUNTIME_DIR}/upvpth.dll")
  else()
    set(USBPV_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/windows-x86")
    set(USBPV_RUNTIME_FILES
      "${USBPV_RUNTIME_DIR}/usbpv_lib.dll"
      "${USBPV_RUNTIME_DIR}/libgcc_s_dw2-1.dll"
      "${USBPV_RUNTIME_DIR}/libstdc++-6.dll"
      "${USBPV_RUNTIME_DIR}/libwinpthread-1.dll")
  endif()
elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(USBPV_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/linux-x64")
  set(USBPV_RUNTIME_FILES "${USBPV_RUNTIME_DIR}/libusbpv_lib.so")
endif()

if(USBPV_COPY_VENDOR_RUNTIME AND USBPV_RUNTIME_FILES)
  foreach(runtime_file IN LISTS USBPV_RUNTIME_FILES)
    if(NOT EXISTS "${runtime_file}")
      message(FATAL_ERROR "Bundled USBPV runtime is missing: ${runtime_file}")
    endif()
    add_custom_command(TARGET usbpv_capture POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "${runtime_file}" "$<TARGET_FILE_DIR:usbpv_capture>"
      VERBATIM)
  endforeach()
endif()

install(TARGETS usbpv_capture RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES include/usbpv_lib.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(USBPV_COPY_VENDOR_RUNTIME AND USBPV_RUNTIME_FILES)
  install(FILES ${USBPV_RUNTIME_FILES} DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
