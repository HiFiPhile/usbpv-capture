# USBPV runtime isolation

`vendor/windows-x64/usbpv_lib.dll` is the original vendor binary with only its
PE dependency-name strings changed. It imports uniquely named copies of its
2018 MinGW runtimes:

```
upvgcc.dll
upvstd.dll
upvpth.dll
```

Unique names allow these legacy runtimes to coexist in one process with current
MSYS2 UCRT64 versions of `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, and
`libwinpthread-1.dll`. They also prevent the legacy files from shadowing GCC's
runtime when GCC starts internal programs such as `cc1.exe`.

The unmodified inputs are archived under `vendor/original/windows-x64`.

## Recreate the isolated binaries

From PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\prepare_windows_runtime.ps1
```

The script reads the original x64 bundle and deploys patched copies to
`vendor/windows-x64`. A different vendor source directory can be supplied with
`-SourceDirectory`.
