# Bundled vendor runtimes

- `windows-x64/` contains the runtime-isolated 64-bit Windows DLL bundle used
  only when the agent explicitly selects the legacy `--library` backend.
- `windows-x86/` contains the original 32-bit Windows DLL bundle.
- `linux-x64/` contains the original 64-bit Linux shared library.
  Windows builds extract its `firm_ng` FPGA image as data, without executing
  this library; see [native protocol](../docs/native-protocol.md).
- `original/windows-x64/` preserves the unmodified Windows x64 inputs used to
  recreate the isolated bundle.
- `original/sample-binaries/` preserves the vendor's prebuilt SDK examples.

See [Windows runtime isolation](../docs/windows-runtime-isolation.md) before
replacing or rearranging the Windows x64 DLLs.
