CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic

ifeq ($(OS),Windows_NT)
TARGET := usbpv_capture.exe
LDLIBS := -static
LDLIBS += -lwinusb -lsetupapi
NATIVE_SOURCES := src/usbpv_native.cpp src/usbpv_protocol.cpp
NATIVE_HEADER := build-make/usbpv_fpga.hpp
NATIVE_CPPFLAGS := -Ibuild-make
SAMPLE_LIBS := -lpthread
else
TARGET := usbpv_capture
LDLIBS := -ldl -pthread
SAMPLE_LIBS := -ldl -pthread
endif

.PHONY: all clean sample

all: $(TARGET)

$(TARGET): src/usbpv_capture.cpp src/usbpv_output.cpp src/usbpv_output.hpp src/usbpv_queue.hpp src/usbpv_capture_queue.hpp include/usbpv_lib.h $(NATIVE_SOURCES) $(NATIVE_HEADER) src/usbpv_protocol.hpp src/usbpv_native.hpp
	$(CXX) $(CXXFLAGS) $(NATIVE_CPPFLAGS) -Iinclude -o $@ src/usbpv_capture.cpp src/usbpv_output.cpp $(NATIVE_SOURCES) $(LDLIBS)

build-make/usbpv_fpga.hpp: tools/embed-fpga.cmake vendor/linux-x64/libusbpv_lib.so
	cmake -E make_directory build-make
	cmake -DINPUT=vendor/linux-x64/libusbpv_lib.so -DOUTPUT=$@ -P tools/embed-fpga.cmake

# Preserve the vendor's original console sample as an explicit target.
sample: usbpv_test_lib

usbpv_test_lib: examples/cpp/vendor_sample.cpp include/usbpv_lib.h
	$(CXX) -O2 -std=gnu++11 -Wall -Wextra -Iinclude -o $@ examples/cpp/vendor_sample.cpp $(SAMPLE_LIBS)

clean:
	rm -f $(TARGET) usbpv_test_lib
