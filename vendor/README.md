# Bundled vendor runtimes

- `windows-x64/` contains the runtime-isolated 64-bit Windows DLL bundle used
  by the agent executable.
- `windows-x86/` contains the original 32-bit Windows DLL bundle.
- `linux-x64/` contains the original 64-bit Linux shared library.
- `original/windows-x64/` preserves the unmodified Windows x64 inputs used to
  recreate the isolated bundle.
- `original/sample-binaries/` preserves the vendor's prebuilt SDK examples.

See [Windows runtime isolation](../docs/windows-runtime-isolation.md) before
replacing or rearranging the Windows x64 DLLs.
