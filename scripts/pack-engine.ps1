param([string]$EngineDirectory = (Join-Path $PSScriptRoot '..\generated'))
$ErrorActionPreference = 'Stop'
$taskProject = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$taskGenerated = Join-Path $taskProject 'generated'
New-Item -ItemType Directory -Path $taskGenerated -Force | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SqueezePacker {
 [DllImport("cabinet.dll", SetLastError=true)] static extern bool CreateCompressor(uint algorithm, IntPtr alloc, out IntPtr handle);
 [DllImport("cabinet.dll", SetLastError=true)] static extern bool Compress(IntPtr handle, byte[] input, UIntPtr inputSize, byte[] output, UIntPtr outputSize, out UIntPtr written);
 [DllImport("cabinet.dll", SetLastError=true)] static extern bool SetCompressorInformation(IntPtr handle, uint kind, ref uint value, UIntPtr bytes);
 [DllImport("cabinet.dll")] static extern bool CloseCompressor(IntPtr handle);
 public static byte[] Pack(byte[] raw) {
  IntPtr h; if(!CreateCompressor(5,IntPtr.Zero,out h)) throw new Exception("Cannot create compressor");
  try { uint blockSize = 64 * 1024 * 1024;
   if(!SetCompressorInformation(h,1,ref blockSize,(UIntPtr)4)) throw new Exception("Cannot set engine compression block size");
   UIntPtr needed; Compress(h,raw,(UIntPtr)raw.Length,null,UIntPtr.Zero,out needed);
   byte[] result=new byte[(int)needed.ToUInt64()];
   if(!Compress(h,raw,(UIntPtr)raw.Length,result,(UIntPtr)result.Length,out needed)) throw new Exception("Compression failed");
   Array.Resize(ref result,(int)needed.ToUInt64()); return result;
  } finally { CloseCompressor(h); }
 }
}
'@
$taskFfmpeg = Join-Path $EngineDirectory 'ffmpeg.exe'
$taskFfprobe = Join-Path $EngineDirectory 'ffprobe.exe'
$taskA = [IO.File]::ReadAllBytes($taskFfmpeg)
$taskB = [IO.File]::ReadAllBytes($taskFfprobe)
$taskRaw = New-Object byte[] ($taskA.Length + $taskB.Length)
[Array]::Copy($taskA,0,$taskRaw,0,$taskA.Length)
[Array]::Copy($taskB,0,$taskRaw,$taskA.Length,$taskB.Length)
$taskPacked = [SqueezePacker]::Pack($taskRaw)
[IO.File]::WriteAllBytes((Join-Path $taskGenerated 'engine.bin'),$taskPacked)
$taskHashA = (Get-FileHash -LiteralPath $taskFfmpeg -Algorithm SHA256).Hash.ToLowerInvariant()
$taskHashB = (Get-FileHash -LiteralPath $taskFfprobe -Algorithm SHA256).Hash.ToLowerInvariant()
$taskHeader = @"
#pragma once
#include <cstddef>
namespace payload {
inline constexpr size_t ffmpegSize = $($taskA.Length);
inline constexpr size_t ffprobeSize = $($taskB.Length);
inline constexpr char ffmpegHash[] = "$taskHashA";
inline constexpr char ffprobeHash[] = "$taskHashB";
inline constexpr char fingerprint[] = "$($taskHashA.Substring(0,16))-$($taskHashB.Substring(0,8))";
}
"@
[IO.File]::WriteAllText((Join-Path $taskGenerated 'payload.hpp'),$taskHeader,[Text.UTF8Encoding]::new($false))
Write-Output ("Engine: {0:N2} MB raw, {1:N2} MB embedded" -f ($taskRaw.Length/1e6),($taskPacked.Length/1e6))
