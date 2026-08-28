CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic

ifeq ($(OS),Windows_NT)
TARGET := usbpv_capture.exe
LDLIBS := -static
SAMPLE_LIBS := -lpthread
else
TARGET := usbpv_capture
LDLIBS := -ldl -pthread
SAMPLE_LIBS := -ldl -pthread
endif

.PHONY: all clean sample

all: $(TARGET)

$(TARGET): src/usbpv_capture.cpp include/usbpv_lib.h
	$(CXX) $(CXXFLAGS) -Iinclude -o $@ src/usbpv_capture.cpp $(LDLIBS)

# Preserve the vendor's original console sample as an explicit target.
sample: usbpv_test_lib

usbpv_test_lib: examples/cpp/vendor_sample.cpp include/usbpv_lib.h
	$(CXX) -O2 -std=gnu++11 -Wall -Wextra -Iinclude -o $@ examples/cpp/vendor_sample.cpp $(SAMPLE_LIBS)

clean:
	rm -f $(TARGET) usbpv_test_lib
