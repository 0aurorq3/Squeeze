# Verification: bitrate-only compression

Validated on Windows x64 with an AMD Ryzen 7 7700 and NVIDIA GeForce RTX 5070 Ti.

## Preservation requirements

- Export sizes are controlled only by video/audio bitrate. The compression plan contains no resolution or frame-rate setting.
- Export has no resizing, padding, cropping, pixel rotation, frame-rate conversion, or 60 fps / 4096-pixel cap. Coded dimensions, SAR, and rotation metadata are preserved.
- VFR presentation timestamps pass through unchanged. Encoder B-frame reordering is disabled for VFR MP4 timing.
- A resized JPEG is used only for the UI thumbnail and is never an encoding input.
- Audio is not explicitly downmixed or resampled. The output format remains H.264/AAC MP4.
- Runtime validation rejects an encoder that changes geometry, rotation, or a known frame count and tries the next encoder.

## Checks and actual encodes

- The release build completes without compiler warnings; 22 core checks pass on this host.
- Integration checks decode source and output, compare frame counts and every normalized presentation timestamp within 10 microseconds, and verify bytes, duration, exact resolution, SAR, rotation, audio presence/channels, and source preservation.
- The 8-second Japanese-named H.264/AAC sample retains 1280×720, 30 fps, all 240 frames and timestamps, plus audio, against a 300,000-byte target:
  - NVIDIA Quality: 247,753 bytes.
  - NVIDIA Balanced: 244,017 bytes.
  - NVIDIA Speed: 268,622 bytes.
  - CPU Quality: 245,104 bytes.
  - CPU Balanced: 245,042 bytes.
  - CPU Speed: 267,255 bytes.
- A 120 fps clip retains 320×180, 120 fps, all 240 frames and timestamps: 93,465 bytes against 100,000 bytes.
- A wide clip retains 5120×288, 24 fps, all 24 frames and timestamps: 145,112 bytes against 150,000 bytes.
- A VFR clip retains 320×180, all 45 frames and the original varying frame intervals, with the same 1.95-second duration and average frame rate. NVIDIA produces 71,848 bytes and CPU 57,618 bytes against 80,000 bytes.
- A 90-degree MOV retains original 1280×720 coded dimensions, rotation metadata, all 240 frames and timestamps: 246,862 bytes against 300,000 bytes.
- An anamorphic source retains 720×576 and SAR 16:15, all 100 frames and timestamps: 149,368 bytes against 150,000 bytes.
- An odd-dimension FFV1/MKV source retains 641×361, all 48 frames and timestamps through CPU H.264 4:4:4: 42,325 bytes against 100,000 bytes.
- Active CPU cancellation removes incomplete output and two-pass logs while preserving the source.

## Packaging and hardware scope

The app is one exe with embedded engines and static non-system runtime code. Prior GUI/portability checks verified an isolated copy with PATH limited to Windows System32, video loading, encoding, animations, completion, and stable input rendering. The UI refresh retains this packaging design.

Detection returns NVIDIA NVENC followed by x264 on this host. AMD AMF and Intel QSV are compiled and implemented, but this PC has no AMD/Intel GPU for physical verification. Additional PCs and Windows 10 were not available for physical testing. The build targets Windows 10/11 x64.

Final executable: dist/Squeeze.exe, 5,828,608 bytes.

SHA-256: 3d44936f2212ff9a2e75e7d6f59b3e0a12837083f40810d2109e3a881f7be5b3.

## v1.0.1 rendering compatibility

- The UI now uses Direct2D software rasterization with GDI presentation. It does not create a display-GPU render target. Video hardware detection and encoding remain unchanged.
- Optional icon decode/upload failures no longer abort the entire paint operation. Embedded PNGs use the Windows PNG decoder directly. Thumbnail pixels are cached independently of render targets, and failed draws invalidate and recreate graphics resources.
- The release GUI was visually checked at 100% scaling. A separate instrumented executable verified 125% scaling with unavailable icon resources and a forced render-target loss together: cards, labels, numeric input, buttons, and the animated units menu remained visible and responsive.
- All 22 core checks pass. An actual GUI encode of the 8-second sample produces 244,983 bytes against 300,000 bytes and retains 1280×720, 30 fps, all 240 frames, and the 8-second duration.
- The reported Windows 11 / RTX 4060 Laptop / i7-13620H machine was not locally available. The failure conditions were simulated on the development PC.
- API reference: [Direct2D GDI render targets](https://learn.microsoft.com/en-us/windows/win32/api/d2d1/nn-d2d1-id2d1dcrendertarget).

## MIT source release

- Squeeze's application code, build scripts, tests, and original icon use the MIT License. Third-party software and the supplied font retain their own licenses.
- The root LICENSE, source documentation, and in-app About labels were updated. The release build completes without warnings and all 22 core checks pass.
- The typeface is included as binary resource data. Application icons are loaded from embedded PNG data, with no icon-construction geometry in the application code. Public builds consume the included image assets directly and require no icon-generation script or Python dependency.

## UI refresh and SF Pro Display alignment

- The supplied SF Pro Display data replaces Inter and is embedded unchanged as a PE resource from `assets/resources/typeface.bin`, loaded into a private DirectWrite font collection and registered privately for the native GDI numeric input. No font installation or adjacent font file is needed at runtime. The custom About panel also uses SF Pro Display.
- Numeric input positioning uses the actual font line height to center the edit control inside the target-size field. Internal edit margins are reset to match the field padding. Native checks covered single digits, two digits, and a longer decimal value.
- Ready and other status titles use 24 px SF Pro Display with medium weight for a larger, natural appearance. Ready is centered on the action-button axis. Processing and result status use a centered two-line group, with consistent panel padding and aligned action controls.
- Setting labels, preset buttons, Browse, and unit selection use 16 px text; the main action uses 18 px text. Progress details use 14 px text with stronger gray contrast, and video metadata uses 13 px text.
- Redundant helper sentences, the file-type hint, and the lower-panel status dot are removed.
- Preset selection uses a critically damped spring with continuous velocity and synchronized text-color transitions. Base controls are painted before the moving highlight, eliminating border/hover overdraw during motion. Text trimming objects are cached once rather than allocated every animation frame.
- The units menu slides/fades and expands/retracts in both directions. Hit testing follows its visible bounds; closing state is painted until the animation finishes. Windows reduced-motion preferences remain respected.
- Native visual checks confirmed the cleaned panel, preset transitions, and opening/closing the units menu during the earlier Inter refresh. The first SF Pro Display build was checked for labels, numeric input alignment, Ready alignment, and processing status alignment; a real encode displayed 494 fps. The current typography refinement was checked with an empty Ready screen and a real 8-second video through Ready, Compressing, and Done, including decimal input and the larger action-button text.
- A real 30-second 1920×1080, 60 fps clip displayed a live 252 fps processing rate. The lower panel shows progress, measured encoder throughput, and ETA. The output is 4,791,253 bytes against a 5,100,000-byte target and retains 1920×1080, 60 fps, all 1,800 frames, and full 30-second duration.
- The updated engine still passes the existing resolution/frame-timestamp integration checks on the 1280×720, 30 fps clip. Its new output was 244,746 bytes against 300,000 bytes, with 240 original frame timestamps retained and audio present.
