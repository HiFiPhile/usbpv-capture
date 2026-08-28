# Examples

The examples came from the vendor SDK and are kept primarily as API references.
The production automation entry point is `src/usbpv_capture.cpp`.

- `cpp/vendor_sample.cpp` is the original dynamic-loading C++ console sample.
- `python/usbpv_test.py` and `csharp/` are language bindings/examples. Their
  defaults use explicit high speed and exclude SOF/NAK.
- `vendor-projects/` contains the original Qt and Visual Studio project files,
  adjusted for the repository layout.

The examples do not use the bounded writer queue, safe packet mask, or pcapng
output implemented by `usbpv_capture`.
