# Build

Install Visual Studio C++ build tools with a Windows SDK and MSYS2 MINGW64 at `%LOCALAPPDATA%\SqueezeBuild\msys64`.

In the MSYS2 MINGW64 shell:

```sh
pacman -S --needed make curl mingw-w64-x86_64-gcc mingw-w64-x86_64-nasm \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-libx264 \
  mingw-w64-x86_64-libvpl mingw-w64-x86_64-amf-headers \
  mingw-w64-x86_64-dav1d mingw-w64-x86_64-zimg mingw-w64-x86_64-zlib
```

From the repository root in PowerShell:

```powershell
.\scripts\build.ps1 -RebuildEngine
```

Output: `dist/Squeeze.exe`.

Once the engine is built, use `.\scripts\build.ps1` to rebuild the app. `-ToolRoot` selects a different toolchain directory.

Image and binary resources are included and used directly by the build. No asset-generation step is required. Generated engines and build outputs are excluded from Git.

## Checks

```powershell
.\build\SqueezeTests.exe
```

See [Verification](VERIFICATION.md) for encoding and portability results.
