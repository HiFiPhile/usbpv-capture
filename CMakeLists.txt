cmake_minimum_required(VERSION 3.16)

project(usbpv_agent_capture VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

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
  # process has installed.
  target_link_options(usbpv_capture PRIVATE -static)
endif()

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(USBPV_FPGA_HEADER "${CMAKE_CURRENT_BINARY_DIR}/usbpv_fpga.hpp")
  add_custom_command(OUTPUT "${USBPV_FPGA_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
      "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/vendor/linux-x64/libusbpv_lib.so"
      "-DOUTPUT=${USBPV_FPGA_HEADER}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-fpga.cmake"
    DEPENDS vendor/linux-x64/libusbpv_lib.so tools/embed-fpga.cmake
    VERBATIM)
  target_sources(usbpv_capture PRIVATE src/usbpv_protocol.cpp "${USBPV_FPGA_HEADER}")
  target_include_directories(usbpv_capture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(WIN32)
  target_sources(usbpv_capture PRIVATE src/usbpv_native.cpp)
  target_link_libraries(usbpv_capture PRIVATE winusb setupapi advapi32 ole32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.21)
  target_sources(usbpv_capture PRIVATE src/usbpv_native_linux.cpp)
  target_link_libraries(usbpv_capture PRIVATE PkgConfig::LIBUSB)
else()
  message(FATAL_ERROR "Native capture supports Windows and Linux")
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME cli COMMAND "${CMAKE_COMMAND}"
    "-DEXE=$<TARGET_FILE:usbpv_capture>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.cmake")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(usbpv_native_linux_test tests/native_linux_test.cpp
      src/usbpv_native_linux.cpp src/usbpv_protocol.cpp "${USBPV_FPGA_HEADER}")
    target_compile_features(usbpv_native_linux_test PRIVATE cxx_std_17)
    target_include_directories(usbpv_native_linux_test PRIVATE include src
      "${CMAKE_CURRENT_BINARY_DIR}")
    # Link only the fake USB API: a missing stub must fail at link time rather
    # than accidentally opening real hardware during CTest.
    target_include_directories(usbpv_native_linux_test SYSTEM PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(usbpv_native_linux_test PRIVATE pthread)
    add_test(NAME native_linux COMMAND usbpv_native_linux_test)
    set_tests_properties(native_linux PROPERTIES TIMEOUT 20)
  endif()
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
  target_link_libraries(usbpv_capture PRIVATE pthread)
endif()

install(TARGETS usbpv_capture RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
