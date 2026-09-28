#!/usr/bin/env bash
set -euo pipefail
# Run in the private MSYS2 MINGW64 toolchain. No runtime DLLs are shipped.
project="$(cd "$(dirname "$0")/.." && pwd)"
cache="$(cygpath -u "${SQUEEZE_BUILD_ROOT:-$LOCALAPPDATA/SqueezeBuild}")"
mkdir -p "$cache/sources" "$project/generated"
cd "$cache/sources"
nvversion=12.1.14.0
if [ ! -f "nv-codec-headers-n$nvversion.tar.gz" ]; then
  /usr/bin/curl -fL --retry 3 "https://github.com/FFmpeg/nv-codec-headers/archive/refs/tags/n$nvversion.tar.gz" -o "nv-codec-headers-n$nvversion.tar.gz"
fi
if [ ! -d "nv-codec-headers-n$nvversion" ]; then tar -xf "nv-codec-headers-n$nvversion.tar.gz"; fi
make -C "nv-codec-headers-n$nvversion" PREFIX=/mingw64 install
version=8.0.1
if [ ! -f "ffmpeg-$version.tar.xz" ]; then
  /usr/bin/curl -fL --retry 3 "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o "ffmpeg-$version.tar.xz"
fi
if [ ! -d "ffmpeg-$version" ]; then tar -xf "ffmpeg-$version.tar.xz"; fi
cd "ffmpeg-$version"
export PKG_CONFIG_PATH=/mingw64/lib/pkgconfig
  ./configure --arch=x86_64 --target-os=mingw32 --cc=gcc --cxx=g++ --enable-small \
    --pkg-config=pkgconf --pkg-config-flags=--static --enable-static --disable-shared \
    --disable-debug --disable-doc --disable-autodetect --disable-network \
    --disable-everything --enable-gpl --enable-version3 --enable-w32threads \
    --enable-libx264 --enable-libvpl --enable-amf --enable-ffnvcodec \
    --enable-nvenc --enable-libdav1d --enable-libzimg --enable-zlib \
    --enable-decoder=h264,hevc,libdav1d,vp8,vp9,mpeg4,mpeg2video,mpeg1video,mjpeg,mjpegb,prores,dnxhd,wmv1,wmv2,wmv3,vc1,flv,h263,h263i,h263p,theora,rawvideo,wrapped_avframe,qtrle,cfhd,huffyuv,utvideo,ffv1,aac,aac_fixed,aac_latm,mp1,mp2,mp3,mp3float,ac3,eac3,dca,alac,flac,opus,vorbis,wmav1,wmav2,wmapro,wmalossless,amrnb,amrwb,pcm_s8,pcm_u8,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be,pcm_alaw,pcm_mulaw,adpcm_ima_wav,adpcm_ms \
    --enable-parser=h264,hevc,av1,vp8,vp9,mpeg4video,mpegvideo,mjpeg,vc1,aac,aac_latm,mpegaudio,ac3,dca,flac,opus,vorbis \
    --enable-encoder=libx264,h264_nvenc,h264_amf,h264_qsv,aac,mjpeg \
    --enable-demuxer=mov,matroska,avi,flv,mpegts,mpegps,asf,ogg,h264,hevc,m4v,mpegvideo,ivf,webm_dash_manifest,wav,aac,mp3,flac \
    --enable-muxer=mp4,mov,null,image2 --enable-protocol=file,pipe \
    --enable-indev=lavfi \
    --enable-filter=scale,format,fps,setsar,transpose,hflip,vflip,null,anull,aresample,aformat,color,crop,trim,atrim,setpts,asetpts,zscale,tonemap \
    --extra-cflags='-Os -ffunction-sections -fdata-sections' \
    --extra-ldflags='-static -static-libgcc -static-libstdc++ -Wl,--gc-sections' \
    --extra-libs='-lstdc++ -lwinpthread'
make -j12 ffmpeg.exe ffprobe.exe
strip ffmpeg.exe ffprobe.exe
cp ffmpeg.exe ffprobe.exe "$project/generated/"
echo "Built the portable engine:"
ls -lh "$project/generated/ffmpeg.exe" "$project/generated/ffprobe.exe"
