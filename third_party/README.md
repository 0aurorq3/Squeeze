# Embedded dependencies

The Squeeze application code, build scripts, tests, and original icon are licensed under MIT. Third-party dependencies and the supplied font retain their own licenses. The embedded engine is FFmpeg 8.0.1, built with `--enable-gpl --enable-version3` and static dependencies. The engine's exact configuration is in `scripts/build-engine.sh`. The original FFmpeg configuration/license text can also be read with the extracted engine's `-buildconf` / `-L` switches.

Corresponding source archives are provided in `sources/`:

The supplied SF Pro Display Regular typeface is stored as binary resource data in `assets/resources/typeface.bin` and embedded unchanged. Its original font metadata and licensing terms are retained. The prior Inter font is no longer embedded; `Inter-OFL.txt` records the notice for that earlier asset.

- FFmpeg 8.0.1: `https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz`; GPL-3.0-or-later for this build.
- x264: commit `b35605ace3ddf7c1a5d67a2eb553f034aef41d55`, version `0.165.r3222.b35605a`, MSYS2 package revision 3; GPL-2.0-or-later. The upstream source mirror archive, matching MSYS2 PKGBUILD, and all three packaging patches are included. Static library build: apply the patches, configure with `--host=x86_64-w64-mingw32 --enable-static --enable-shared`, then `make` / `make install`. The library has no FFmpeg dependency; the optional x264 command-line application does.
- Intel oneVPL/libvpl 2.17.0; MIT. Static dispatcher; the GPU implementation comes from the PC's Intel driver.
- NVIDIA nv-codec-headers `n12.1.14.0`; MIT. The runtime comes from the PC's NVIDIA driver.
- AMD AMF headers 1.5.2; MIT. The complete `amf/public/include` source subtree and upstream license are included, matching the headers-only build dependency. The runtime comes from the PC's AMD driver.
- dav1d 1.5.4; BSD-2-Clause.
- zimg 3.0.6; WTFPL, with the copyright/license information retained in its source archive.
- zlib 1.3.2; zlib license.

MSYS2 MinGW GCC 16.2.0 supplies the statically linked compiler runtime. GCC runtime, C++ runtime, MinGW CRT, and pthread license notices are retained in their named subdirectories. Windows system DLLs are supplied by Windows. GPU runtime and driver DLLs are loaded from the destination PC's driver installation.

The native application and engine binaries are linked without non-system DLL imports. Source archives keep their upstream license files. SHA-256 hashes are recorded in `sources/SHA256SUMS.txt`. The original icon is included as ready-to-use PNG and ICO image assets.

`scripts/package-source.ps1` creates `source/Squeeze-1.0-source.zip` containing the app, build scripts, tests, icon, license files, and dependency sources. The app can be carried separately as one exe; keep this corresponding source package available with redistributions.
