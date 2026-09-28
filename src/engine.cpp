#include "engine.hpp"
#include "json.hpp"
#include "payload.hpp"
#include <compressapi.h>
#include <bcrypt.h>
#include <dxgi1_2.h>
#include <shlobj.h>
#include <fstream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <memory>
#include <sstream>
#include <iomanip>
#include <limits>
#include <thread>

namespace squeeze {
using Clock = std::chrono::steady_clock;
struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE value = nullptr) : h(value) {}
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::wstring utf16(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring quoteArg(const std::wstring &value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"')
            out.append(slashes * 2 + 1, L'\\');
        else
            out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L'\"';
    return out;
}
static std::string fileHash(const fs::path &file) {
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    DWORD length = 0, result = 0;
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length),
                      &result, 0);
    std::vector<UCHAR> object(length);
    std::array<UCHAR, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        auto n = input.gcount();
        if (n > 0)
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(n), 0);
    }
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream out;
    for (auto b : digest)
        out << std::hex << std::setfill('0') << std::setw(2) << int(b);
    return out.str();
}
Engine::Engine(fs::path external) : directory(std::move(external)) {}
fs::path Engine::ensure() {
    std::lock_guard lock(setupMutex);
    if (!directory.empty())
        return directory;
    PWSTR local = nullptr;
    check(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)),
          "Could not prepare the video engine");
    fs::path base(local);
    CoTaskMemFree(local);
    auto destination = base / L"Squeeze" / L"engine" / utf16(payload::fingerprint);
    fs::create_directories(destination);
    auto ffmpeg = destination / L"ffmpeg.exe", ffprobe = destination / L"ffprobe.exe";
    Handle mutex(
        CreateMutexW(nullptr, FALSE, (L"Local\\SqueezeEngine-" + utf16(payload::fingerprint)).c_str()));
    DWORD wait = WaitForSingleObject(mutex.h, 60000);
    check(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED, "The video engine is busy. Try again");
    struct Unlock {
        HANDLE h;
        ~Unlock() {
            ReleaseMutex(h);
        }
    } unlock{mutex.h};
    if (fileHash(ffmpeg) != payload::ffmpegHash || fileHash(ffprobe) != payload::ffprobeHash) {
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
        check(resource != nullptr, "The video engine is missing");
        auto data = LockResource(LoadResource(nullptr, resource));
        DWORD size = SizeofResource(nullptr, resource);
        DECOMPRESSOR_HANDLE decompressor = nullptr;
        check(CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor) != FALSE,
              "Could not prepare the video engine");
        struct DecompressCleanup {
            DECOMPRESSOR_HANDLE h;
            ~DecompressCleanup() {
                CloseDecompressor(h);
            }
        } cleanup{decompressor};
        std::vector<unsigned char> raw(payload::ffmpegSize + payload::ffprobeSize);
        SIZE_T actual = 0;
        check(Decompress(decompressor, data, size, raw.data(), raw.size(), &actual) != FALSE &&
                  actual == raw.size(),
              "The video engine is damaged");
        auto write = [&](const fs::path &path, size_t offset, size_t count, const char *expected) {
            fs::path temporary = path;
            temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                check(bool(output), "Could not prepare the video engine");
                output.write(reinterpret_cast<const char *>(raw.data() + offset),
                             static_cast<std::streamsize>(count));
                check(bool(output), "Could not prepare the video engine");
            }
            if (fileHash(temporary) != expected) {
                std::error_code ec;
                fs::remove(temporary, ec);
                throw std::runtime_error("The video engine is damaged");
            }
            if (!MoveFileExW(temporary.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                std::error_code ec;
                fs::remove(temporary, ec);
                throw std::runtime_error("Could not prepare the video engine");
            }
        };
        write(ffmpeg, 0, payload::ffmpegSize, payload::ffmpegHash);
        write(ffprobe, payload::ffmpegSize, payload::ffprobeSize, payload::ffprobeHash);
    }
    directory = destination;
    return directory;
}
ProcessResult runProcess(const fs::path &executable, const std::vector<std::wstring> &args,
                         std::atomic_bool &cancel, int timeoutSeconds,
                         const std::function<void(const std::string &)> &onLine) {
    if (cancel.load())
        return {ERROR_CANCELLED, {}, true};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE readHandle = nullptr, writeHandle = nullptr;
    check(CreatePipe(&readHandle, &writeHandle, &attributes, 0) != FALSE, "Could not start the video engine");
    Handle readPipe(readHandle), writePipe(writeHandle);
    check(SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0) != FALSE,
          "Could not start the video engine");
    Handle nullInput(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(nullInput.h != INVALID_HANDLE_VALUE, "Could not start the video engine");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writeHandle;
    startup.StartupInfo.hStdError = writeHandle;
    startup.StartupInfo.hStdInput = nullInput.h;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributeStorage(attributeBytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    check(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes) != FALSE,
          "Could not start the video engine");
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST p;
        ~AttributeCleanup() {
            DeleteProcThreadAttributeList(p);
        }
    } attributeCleanup{startup.lpAttributeList};
    HANDLE inherited[]{writeHandle, nullInput.h};
    check(UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                    sizeof(inherited), nullptr, nullptr) != FALSE,
          "Could not start the video engine");
    std::wstring command = quoteArg(executable.wstring());
    for (const auto &arg : args) {
        command += L' ';
        command += quoteArg(arg);
    }
    PROCESS_INFORMATION process{};
    check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                         &startup.StartupInfo, &process) != FALSE,
          "Could not start the video engine");
    Handle processHandle(process.hProcess), threadHandle(process.hThread),
        job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.h ||
        !SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job.h, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        throw std::runtime_error("Could not start the video engine");
    }
    ResumeThread(process.hThread);
    CloseHandle(writePipe.h);
    writePipe.h = nullptr;
    ProcessResult result;
    std::string pending;
    auto start = Clock::now();
    bool ended = false, timedOut = false;
    while (true) {
        if (!ended && (cancel.load() ||
                       (timeoutSeconds > 0 && Clock::now() - start > std::chrono::seconds(timeoutSeconds)))) {
            result.cancelled = cancel.load();
            timedOut = !result.cancelled;
            TerminateJobObject(job.h, result.cancelled ? ERROR_CANCELLED : ERROR_TIMEOUT);
            ended = true;
        }
        DWORD available = 0;
        if (PeekNamedPipe(readHandle, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buffer[16384];
            DWORD received = 0;
            if (ReadFile(readHandle, buffer, std::min<DWORD>(available, sizeof(buffer)), &received,
                         nullptr) &&
                received) {
                result.output.append(buffer, received);
                if (result.output.size() > 2 * 1024 * 1024)
                    result.output.erase(0, result.output.size() - 1024 * 1024);
                if (onLine) {
                    pending.append(buffer, received);
                    size_t position = 0;
                    while ((position = pending.find_first_of("\r\n")) != std::string::npos) {
                        if (position)
                            onLine(pending.substr(0, position));
                        pending.erase(0, position + 1);
                    }
                    if (pending.size() > 65536)
                        pending.clear();
                }
            }
            continue;
        }
        if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) {
            if (!PeekNamedPipe(readHandle, nullptr, 0, nullptr, &available, nullptr) || !available)
                break;
        }
    }
    if (onLine && !pending.empty())
        onLine(pending);
    GetExitCodeProcess(process.hProcess, &result.code);
    if (timedOut)
        result.code = ERROR_TIMEOUT;
    return result;
}
static void append(std::vector<std::wstring> &to, const std::vector<std::wstring> &from) {
    to.insert(to.end(), from.begin(), from.end());
}
std::vector<std::wstring> presetArgs(Brand brand, Preset preset) {
    const int i = static_cast<int>(preset);
    switch (brand) {
    case Brand::Nvidia:
        return {L"-preset",    std::array{L"p7", L"p4", L"p1"}[i],
                L"-tune",      L"hq",
                L"-rc",        L"vbr",
                L"-multipass", std::array{L"fullres", L"qres", L"disabled"}[i]};
    case Brand::Amd:
        return {L"-usage", L"transcoding", L"-quality", std::array{L"quality", L"balanced", L"speed"}[i],
                L"-rc",    L"vbr_peak"};
    case Brand::Intel:
        return {L"-preset", std::array{L"slow", L"medium", L"veryfast"}[i]};
    case Brand::Cpu:
        return {L"-preset", std::array{L"slow", L"medium", L"veryfast"}[i]};
    }
    return {};
}
std::vector<Encoder> candidateEncoders() {
    std::vector<Encoder> nvidia{{Brand::Nvidia, L"h264_nvenc", {}}}, amd, intel;
    // NVENC's default device selection tries supported NVIDIA adapters.
    IDXGIFactory1 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
        for (UINT index = 0;; ++index) {
            IDXGIAdapter1 *adapter = nullptr;
            if (FAILED(factory->EnumAdapters1(index, &adapter)))
                break;
            if (!adapter)
                continue;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            adapter->Release();
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            if (desc.VendorId == 0x1002)
                amd.push_back({Brand::Amd,
                               L"h264_amf",
                               {L"-init_hw_device", L"d3d11va=sqamd:" + std::to_wstring(index),
                                L"-filter_hw_device", L"sqamd"}});
            if (desc.VendorId == 0x8086)
                intel.push_back(
                    {Brand::Intel,
                     L"h264_qsv",
                     {L"-init_hw_device",
                      L"qsv=sqintel,child_device=" + std::to_wstring(index) + L",child_device_type=d3d11va",
                      L"-filter_hw_device", L"sqintel"}});
        }
        factory->Release();
    }
    // Default probes also cover adapters not exposed by DXGI (for example a headless GPU).
    amd.push_back({Brand::Amd, L"h264_amf", {}});
    intel.push_back({Brand::Intel, L"h264_qsv", {}});
    nvidia.insert(nvidia.end(), amd.begin(), amd.end());
    nvidia.insert(nvidia.end(), intel.begin(), intel.end());
    nvidia.push_back({Brand::Cpu, L"libx264", {}});
    return nvidia;
}
std::vector<Encoder> Engine::detect(std::atomic_bool &cancel) {
    std::unique_lock lock(detectionMutex, std::defer_lock);
    while (!lock.try_lock()) {
        if (cancel.load())
            return {};
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (detected)
        return encoders;
    auto exe = ensure() / L"ffmpeg.exe";
    std::vector<Encoder> usable;
    for (const auto &encoder : candidateEncoders()) {
        if (cancel.load())
            break;
        if (encoder.brand == Brand::Cpu) {
            usable.push_back(encoder);
            continue;
        }
        auto args = encoder.deviceArgs;
        append(args, {L"-hide_banner", L"-loglevel", L"error", L"-nostdin", L"-f", L"lavfi", L"-i",
                      L"color=c=black:s=256x256:r=30", L"-frames:v", L"4", L"-an", L"-c:v", encoder.codec,
                      L"-pix_fmt", L"nv12", L"-b:v", L"500000"});
        append(args, presetArgs(encoder.brand, Preset::Balanced));
        append(args, {L"-f", L"null", L"-"});
        auto result = runProcess(exe, args, cancel, 12);
        if (!result.code && !result.cancelled)
            usable.push_back(encoder);
    }
    if (!cancel.load()) {
        encoders = usable;
        detected = true;
    }
    return usable;
}
static double rational(const std::string &text, double fallback) {
    auto slash = text.find_first_of("/:");
    try {
        if (slash == std::string::npos)
            return std::stod(text);
        double numerator = std::stod(text.substr(0, slash)), denominator = std::stod(text.substr(slash + 1));
        return denominator > 0 ? numerator / denominator : fallback;
    } catch (...) {
        return fallback;
    }
}
Media Engine::probe(const fs::path &file, std::atomic_bool &cancel) {
    check(fs::is_regular_file(file), "Choose a video file");
    auto result = runProcess(
        ensure() / L"ffprobe.exe",
        {L"-v", L"error", L"-show_format", L"-show_streams", L"-of", L"json", file.wstring()}, cancel, 45);
    if (result.cancelled)
        throw std::runtime_error("Cancelled");
    check(result.code == 0, "Could not read this video");
    auto root = json::parse(result.output);
    Media media;
    media.path = file;
    media.bytes = fs::file_size(file);
    media.duration = root["format"]["duration"].number();
    for (const auto &stream : root["streams"].array) {
        if (stream["codec_type"].text == "video" && stream["disposition"]["attached_pic"].number() != 1 &&
            media.videoIndex < 0) {
            media.videoIndex = static_cast<int>(stream["index"].number(-1));
            media.width = static_cast<int>(stream["width"].number());
            media.height = static_cast<int>(stream["height"].number());
            media.pixelAspect = rational(stream["sample_aspect_ratio"].text, 1);
            if (!std::isfinite(media.pixelAspect) || media.pixelAspect <= 0)
                media.pixelAspect = 1;
            media.frames = static_cast<int64_t>(stream["nb_frames"].number(-1));
            media.fps = rational(stream["avg_frame_rate"].text, 0);
            const double nominalFps = rational(stream["r_frame_rate"].text, 0);
            media.variableFrameRate =
                media.fps > 0 && nominalFps > 0 && std::abs(media.fps / nominalFps - 1) > 0.000001;
            if (!(media.fps > 0))
                media.fps = rational(stream["r_frame_rate"].text, 30);
            if (!(media.duration > 0))
                media.duration = stream["duration"].number();
            media.rotation = static_cast<int>(stream["tags"]["rotate"].number());
            for (const auto &side : stream["side_data_list"].array)
                if (!side["rotation"].text.empty())
                    media.rotation = static_cast<int>(side["rotation"].number());
        }
        if (stream["codec_type"].text == "audio" && media.audioIndex < 0) {
            media.audioIndex = static_cast<int>(stream["index"].number(-1));
            media.channels = static_cast<int>(stream["channels"].number(2));
        }
    }
    check(media.videoIndex >= 0 && media.width > 0 && media.height > 0, "No video found in this file");
    check(std::isfinite(media.duration) && media.duration > 0, "This video has no readable duration");
    if (!std::isfinite(media.fps) || media.fps <= 0)
        media.fps = 30;
    return media;
}
Plan makePlan(const Media &media, uint64_t target) {
    check(media.duration > 0 && std::isfinite(media.duration) && media.width > 0 && media.height > 0,
          "Could not read this video");
    check(target > 0 && target <= 1000000000000ULL, "Enter a size between 0 and 1 TB");
    Plan plan;
    plan.targetBytes = target;
    // Frame rate is used only to estimate mux overhead, never to retime the video.
    const double overhead = 8192 + media.duration * (media.fps + 50) * 16;
    const double bitrate = (double(target) * 0.965 - overhead) * 8 / media.duration;
    const bool audio = media.audioIndex >= 0;
    const int64_t maxAudio = media.channels == 1 ? 64000 : 128000;
    plan.audioBps = audio ? static_cast<int64_t>(std::clamp(bitrate * 0.13, 32000.0, double(maxAudio))) : 0;
    plan.videoBps = static_cast<int64_t>(std::min(800000000.0, bitrate - plan.audioBps));
    check(plan.videoBps >= 32000, "Target is too small. Increase the size");
    return plan;
}
fs::path Engine::thumbnail(const Media &media, std::atomic_bool &cancel) {
    // This resized JPEG is for the UI preview only; it is never an encoding input.
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    auto file = fs::path(temp) / (L"squeeze-preview-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64()) + L".jpg");
    auto result = runProcess(ensure() / L"ffmpeg.exe",
                             {L"-hide_banner",
                              L"-loglevel",
                              L"error",
                              L"-nostdin",
                              L"-y",
                              L"-ss",
                              std::to_wstring(std::min(1.0, media.duration / 4)),
                              L"-i",
                              media.path.wstring(),
                              L"-map",
                              L"0:" + std::to_wstring(media.videoIndex),
                              L"-frames:v",
                              L"1",
                              L"-vf",
                              L"scale=720:360:force_original_aspect_ratio=decrease",
                              L"-c:v",
                              L"mjpeg",
                              L"-q:v",
                              L"4",
                              L"-update",
                              L"1",
                              file.wstring()},
                             cancel, 20);
    if (result.code || result.cancelled) {
        std::error_code ec;
        fs::remove(file, ec);
        return {};
    }
    return file;
}
static fs::path uniqueOutput(const fs::path &source) {
    for (unsigned index = 0; index < 10000; ++index) {
        auto name = source.stem().wstring() + L"_compressed" +
                    (index ? L" (" + std::to_wstring(index) + L")" : L"") + L".mp4";
        auto path = source.parent_path() / name;
        HANDLE file =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return path;
        }
        DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Cannot save here. Choose a writable video folder");
    }
    throw std::runtime_error("Too many output files in this folder");
}
fs::path Engine::compress(const Media &media, uint64_t target, Preset preset, std::atomic_bool &cancel,
                          const Progress &progress, bool cpuOnly) {
    auto plan = makePlan(media, target);
    auto exe = ensure() / L"ffmpeg.exe";
    if (progress)
        progress({L"Preparing", 0, -1});
    auto available = cpuOnly ? std::vector<Encoder>{{Brand::Cpu, L"libx264", {}}} : detect(cancel);
    if (cancel.load())
        throw std::runtime_error("Cancelled");
    auto final = uniqueOutput(media.path);
    auto partial = media.path.parent_path() / (L".squeeze-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                               std::to_wstring(GetTickCount64()) + L".mp4");
    auto passlog = partial;
    passlog += L"-pass";
    struct Files {
        fs::path final, partial, pass;
        bool done = false;
        ~Files() {
            std::error_code ec;
            fs::remove(partial, ec);
            for (const auto &suffix : {L"-0.log", L"-0.log.mbtree", L"-0.log.temp", L"-0.log.mbtree.temp"})
                fs::remove(fs::path(pass.wstring() + suffix), ec);
            if (!done)
                fs::remove(final, ec);
        }
    } files{final, partial, passlog};
    for (const auto &encoder : available) {
        int64_t videoBps = plan.videoBps;
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (cancel.load())
                throw std::runtime_error("Cancelled");
            bool cpu = encoder.brand == Brand::Cpu;
            bool failed = false;
            std::wstring phase = attempt ? L"Refining" : L"Compressing";
            if (progress)
                progress({phase, 0, -1});
            for (int pass = cpu ? 1 : 0; pass <= (cpu ? 2 : 0); ++pass) {
                auto args = encoder.deviceArgs;
                append(args, {L"-hide_banner", L"-loglevel", L"error", L"-nostdin", L"-y", L"-stats_period",
                              L"0.2", L"-noautorotate", L"-i", media.path.wstring(), L"-map",
                              L"0:" + std::to_wstring(media.videoIndex), L"-map_metadata", L"-1",
                              L"-map_chapters", L"-1", L"-sn", L"-dn"});
                // Preserve coded dimensions, SAR, rotation metadata, and original frame timestamps.
                // Odd dimensions use CPU 4:4:4 instead of rounding, padding, or scaling the image.
                const bool odd = media.width % 2 != 0 || media.height % 2 != 0;
                const std::wstring pixelFormat = cpu ? (odd ? L"yuv444p" : L"yuv420p") : L"nv12";
                append(args, {L"-pix_fmt", pixelFormat, L"-fps_mode:v", L"passthrough", L"-enc_time_base:v",
                              L"demux", L"-c:v", encoder.codec, L"-b:v", std::to_wstring(videoBps),
                              L"-maxrate", std::to_wstring(std::min<int64_t>(1000000000, videoBps * 13 / 10)),
                              L"-bufsize", std::to_wstring(std::min<int64_t>(1800000000, videoBps * 2))});
                append(args, presetArgs(encoder.brand, preset));
                // Avoid encoder-specific DTS reordering shortening VFR MP4 tracks.
                // Frames and their presentation timestamps still pass through unchanged.
                if (media.variableFrameRate)
                    append(args, {L"-bf", L"0"});
                if (cpu)
                    append(args, {L"-pass", std::to_wstring(pass), L"-passlogfile", passlog.wstring()});
                if (pass == 1 || media.audioIndex < 0)
                    args.push_back(L"-an");
                else
                    append(args, {L"-map", L"0:" + std::to_wstring(media.audioIndex), L"-c:a", L"aac",
                                  L"-b:a", std::to_wstring(plan.audioBps)});
                append(args, {L"-progress", L"pipe:1"});
                if (pass == 1)
                    append(args, {L"-f", L"null", L"-"});
                else
                    append(args, {L"-movflags", L"+faststart", L"-f", L"mp4", partial.wstring()});
                auto start = Clock::now();
                double lastFraction = 0;
                double reportedFps = 0, encodedFrames = 0, seconds = 0;
                auto result = runProcess(exe, args, cancel, 0, [&](const std::string &line) {
                    try {
                        if (line.rfind("fps=", 0) == 0) {
                            reportedFps = std::stod(line.substr(4));
                            return;
                        }
                        if (line.rfind("frame=", 0) == 0) {
                            encodedFrames = std::stod(line.substr(6));
                            return;
                        }
                        if (line.rfind("out_time_us=", 0) == 0) {
                            seconds = std::stod(line.substr(12)) / 1000000;
                            return;
                        }
                    } catch (...) {
                        return;
                    }
                    if (line.rfind("progress=", 0) != 0 || !progress)
                        return;
                    double fraction = std::clamp(seconds / media.duration, 0.0, 0.99);
                    fraction = std::max(lastFraction, fraction);
                    lastFraction = fraction;
                    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
                    int remaining = seconds > 0.2
                                        ? static_cast<int>(std::min(
                                              2147483000.0, elapsed * (media.duration - seconds) / seconds))
                                        : -1;
                    if (cpu) {
                        fraction = (pass == 1 ? 0 : 0.5) + fraction * 0.5;
                        if (pass == 1 && remaining >= 0)
                            remaining += static_cast<int>(elapsed + remaining);
                    }
                    double fps =
                        reportedFps > 0 ? reportedFps : (elapsed > 0.1 ? encodedFrames / elapsed : 0);
                    progress({phase, fraction, remaining, std::isfinite(fps) ? std::max(0.0, fps) : 0});
                });
                if (result.cancelled || cancel.load())
                    throw std::runtime_error("Cancelled");
                if (result.code) {
                    failed = true;
                    break;
                }
            }
            if (failed)
                break; // Try the next usable adapter/brand, then CPU.
            std::error_code ec;
            uint64_t bytes = fs::file_size(partial, ec);
            if (ec || !bytes)
                break;
            if (bytes <= target) {
                if (progress)
                    progress({L"Finishing", 0.995, -1});
                auto output = probe(partial, cancel);
                const double tolerance = std::max(0.25, 2.0 / media.fps);
                check(output.duration + tolerance >= media.duration, "The full video could not be saved");
                check(media.audioIndex < 0 || output.audioIndex >= 0, "The audio could not be saved");
                if (output.width != media.width || output.height != media.height ||
                    std::abs(output.pixelAspect - media.pixelAspect) > 0.00001 ||
                    output.rotation != media.rotation || (media.frames >= 0 && output.frames != media.frames))
                    break; // Reject an encoder that alters geometry or drops frames; try the next one.
                if (cancel.load())
                    throw std::runtime_error("Cancelled");
                check(MoveFileExW(partial.c_str(), final.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
                      "Could not save the compressed video");
                files.done = true;
                if (progress)
                    progress({L"Done", 1, 0});
                return final;
            }
            double fixedBytes =
                double(plan.audioBps) * media.duration / 8 + 8192 + media.duration * (media.fps + 50) * 16;
            double factor = (double(target) * 0.95 - fixedBytes) / std::max(1.0, double(bytes) - fixedBytes);
            videoBps = static_cast<int64_t>(double(videoBps) * std::clamp(factor, 0.05, 0.9));
            if (videoBps < 8000)
                break;
        }
    }
    throw std::runtime_error("Could not reach this size. Try a larger target");
}
} // namespace squeeze
