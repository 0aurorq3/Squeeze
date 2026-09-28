#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace squeeze {
namespace fs = std::filesystem;
enum class Preset { Quality, Balanced, Speed };
enum class Brand { Nvidia, Amd, Intel, Cpu };
struct Encoder {
    Brand brand;
    std::wstring codec;
    std::vector<std::wstring> deviceArgs;
};
struct Media {
    fs::path path;
    double duration = 0, fps = 30;
    double pixelAspect = 1;
    int width = 0, height = 0, videoIndex = -1, audioIndex = -1, channels = 0;
    int rotation = 0;
    uint64_t bytes = 0;
    int64_t frames = -1;
    bool variableFrameRate = false;
};
struct Plan {
    uint64_t targetBytes = 0;
    int64_t videoBps = 0, audioBps = 0;
};
struct Update {
    std::wstring phase;
    double fraction = 0;
    int secondsLeft = -1;
    double encodingFps = 0;
};
struct ProcessResult {
    DWORD code = 0;
    std::string output;
    bool cancelled = false;
};
using Progress = std::function<void(const Update &)>;
std::wstring utf16(const std::string &s);
std::string utf8(const std::wstring &s);
std::wstring quoteArg(const std::wstring &value);
Plan makePlan(const Media &media, uint64_t target);
std::vector<std::wstring> presetArgs(Brand brand, Preset preset);
std::vector<Encoder> candidateEncoders();
ProcessResult runProcess(const fs::path &executable, const std::vector<std::wstring> &args,
                         std::atomic_bool &cancel, int timeoutSeconds = 0,
                         const std::function<void(const std::string &)> &line = {});
class Engine {
    std::mutex setupMutex, detectionMutex;
    fs::path directory;
    std::vector<Encoder> encoders;
    bool detected = false;

  public:
    explicit Engine(fs::path external = {});
    fs::path ensure();
    std::vector<Encoder> detect(std::atomic_bool &cancel);
    Media probe(const fs::path &file, std::atomic_bool &cancel);
    fs::path thumbnail(const Media &media, std::atomic_bool &cancel);
    fs::path compress(const Media &media, uint64_t target, Preset preset, std::atomic_bool &cancel,
                      const Progress &progress, bool cpuOnly = false);
};
} // namespace squeeze
