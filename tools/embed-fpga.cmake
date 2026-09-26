# Extract data only; never execute or load the proprietary capture library.
# firm_ng in the bundled ELF has file offset 0x29260 and size 0xabbf5.
file(SHA256 "${INPUT}" actual_hash)
if(NOT actual_hash STREQUAL "e0a7ecb5b6748fb13dce9acb035ddb3bd45cdbdf07336911f965b3d3d0967a69")
  message(FATAL_ERROR "Unknown USBPV firmware source; refusing offset-based extraction")
endif()
file(READ "${INPUT}" firmware OFFSET 168544 LIMIT 703477 HEX)
string(REPEAT "[0-9a-f][0-9a-f]" 16 line_pattern)
string(REGEX REPLACE "(${line_pattern})" "\\1\n" firmware "${firmware}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," firmware "${firmware}")
file(WRITE "${OUTPUT}" "// Generated from vendor firm_ng. See docs/native-protocol.md.\nstatic const unsigned char kCh56xFpga[] = {${firmware}};\n")
