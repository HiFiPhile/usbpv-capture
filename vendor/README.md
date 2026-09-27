# Archived vendor SDK binaries

`usbpv_capture` uses native WinUSB/libusb capture exclusively. These binaries
are retained as original SDK references and for the historical examples; they
are not loaded, copied, or installed by the capture application.

- `windows-x64/` contains the runtime-isolated 64-bit Windows DLL bundle.
- `windows-x86/` contains the original 32-bit Windows DLL bundle.
- `linux-x64/` contains the original 64-bit Linux shared library.
  Windows and Linux builds extract its `firm_ng` FPGA image as data, without
  executing this library; see [native protocol](../docs/reference/native-protocol.md).
- `original/windows-x64/` preserves the unmodified Windows x64 inputs used to
  recreate the isolated bundle.
- `original/sample-binaries/` preserves the vendor's prebuilt SDK examples.

See the archived [Windows runtime isolation guide](../docs/guides/windows-runtime-isolation.md)
for the provenance and preparation of the Windows x64 DLLs.
