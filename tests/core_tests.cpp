#include "engine.hpp"
#include "json.hpp"
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <array>
#include <thread>
#include <chrono>

using namespace squeeze;
static int checks = 0;
static void require(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
static std::vector<double> frameTimes(Engine &engine, const Media &media, std::atomic_bool &cancel) {
    auto result = runProcess(engine.ensure() / L"ffprobe.exe",
                             {L"-v", L"error", L"-select_streams", std::to_wstring(media.videoIndex),
                              L"-show_frames", L"-show_entries", L"frame=best_effort_timestamp_time", L"-of",
                              L"json", media.path.wstring()},
                             cancel, 60);
    require(result.code == 0, "Frame timing probe failed");
    std::vector<double> times;
    auto root = json::parse(result.output);
    for (const auto &frame : root["frames"].array) {
        double time = frame["best_effort_timestamp_time"].number(-1);
        require(time >= 0, "Missing video frame timestamp");
        times.push_back(time);
    }
    return times;
}
int wmain(int argc, wchar_t **argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        for (const auto &argument : std::array<std::wstring, 6>{L"", L"plain", L"space here", L"quote\"here",
                                                                L"C:\\folder with spaces\\", L"日本語 \\"}) {
            auto command = L"test " + quoteArg(argument);
            int count = 0;
            auto parts = CommandLineToArgvW(command.c_str(), &count);
            require(parts && count == 2 && parts[1] == argument,
                    "Windows argument quoting did not round-trip");
            LocalFree(parts);
        }
        auto parsed = json::parse(
            R"({"streams":[{"width":1920,"codec_type":"video"}],"duration":"12.3","name":"a\"b\u65e5\ud83c\udfac"})");
        require(parsed["streams"].array.size() == 1, "JSON stream array missing");
        require(parsed["streams"].array[0]["width"].number() == 1920, "JSON number incorrect");
        require(std::abs(parsed["duration"].number() - 12.3) < 0.001, "JSON string number incorrect");
        require(parsed["name"].text == "a\"b日🎬", "JSON Unicode incorrect");
        bool rejected = false;
        try {
            json::parse("{\"bad\":");
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Truncated JSON accepted");
        Media m;
        m.duration = 120;
        m.width = 1920;
        m.height = 1080;
        m.fps = 30;
        m.audioIndex = 1;
        m.channels = 2;
        for (uint64_t bytes : {2000000ULL, 25000000ULL, 1000000000ULL}) {
            auto p = makePlan(m, bytes);
            require((p.videoBps + p.audioBps) * m.duration / 8 < bytes, "Bitrate exceeded target budget");
            require(p.audioBps >= 32000 && p.audioBps <= 128000, "Audio budget outside supported bounds");
        }
        m.audioIndex = -1;
        require(makePlan(m, 25000000).audioBps == 0, "Silent video got an audio budget");
        rejected = false;
        try {
            makePlan(m, 100);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Impossible size accepted");
        auto candidates = candidateEncoders();
        for (size_t i = 1; i < candidates.size(); ++i)
            require(candidates[i - 1].brand <= candidates[i].brand, "GPU priority is incorrect");
        std::cout << "PASS: " << checks << " core checks\n";
        if (argc > 2 && std::wstring(argv[1]) == L"--integration") {
            Engine engine;
            std::atomic_bool cancel = false;
            auto input = engine.probe(argv[2], cancel);
            require(input.duration > 0 && input.videoIndex >= 0, "Source probe failed");
            bool cpu = argc > 5 && std::wstring(argv[5]) == L"cpu";
            auto detected = engine.detect(cancel);
            require(!detected.empty() && detected.back().brand == Brand::Cpu, "CPU fallback missing");
            std::cout << "Usable:";
            for (const auto &encoder : detected)
                std::cout << " " << utf8(encoder.codec);
            std::cout << "\n";
            uint64_t target = argc > 3 ? std::stoull(argv[3]) : 300000;
            Preset preset = Preset::Balanced;
            if (argc > 4) {
                std::wstring value = argv[4];
                if (value == L"quality")
                    preset = Preset::Quality;
                if (value == L"speed")
                    preset = Preset::Speed;
            }
            auto sourceBytes = fs::file_size(input.path);
            auto out = engine.compress(input, target, preset, cancel, {}, cpu);
            auto result = engine.probe(out, cancel);
            require(result.bytes <= target, "Output exceeded the requested size");
            require(result.duration + 0.25 >= input.duration, "Output was truncated");
            require(result.width == input.width && result.height == input.height, "Resolution was changed");
            require(std::abs(result.pixelAspect - input.pixelAspect) < 0.00001,
                    "Pixel aspect ratio was changed");
            require(result.rotation == input.rotation, "Rotation metadata was changed");
            auto inputTimes = frameTimes(engine, input, cancel);
            auto resultTimes = frameTimes(engine, result, cancel);
            require(!inputTimes.empty() && resultTimes.size() == inputTimes.size(),
                    "Video frames were added or dropped");
            for (size_t i = 0; i < inputTimes.size(); ++i)
                require(std::abs((resultTimes[i] - resultTimes[0]) - (inputTimes[i] - inputTimes[0])) <
                            0.00001,
                        "Frame rate or VFR frame timing was changed");
            require(input.audioIndex < 0 || result.audioIndex >= 0, "Audio was lost");
            require(input.audioIndex < 0 || result.channels == input.channels, "Audio channels were changed");
            require(fs::file_size(input.path) == sourceBytes, "Source was changed");
            std::cout << "OUTPUT: " << utf8(out.wstring()) << "\n"
                      << "PASS: " << result.bytes << " <= " << target
                      << " bytes; duration=" << result.duration << "; resolution=" << result.width << "x"
                      << result.height << "; fps=" << result.fps << "; frames=" << resultTimes.size()
                      << "; timing=preserved; audio=" << (result.audioIndex >= 0) << "\n";
        }
        if (argc > 2 && std::wstring(argv[1]) == L"--cancel") {
            Engine engine;
            std::atomic_bool cancel = false;
            auto input = engine.probe(argv[2], cancel);
            size_t before = 0;
            for (auto &entry : fs::directory_iterator(input.path.parent_path()))
                if (entry.is_regular_file())
                    ++before;
            bool wasCancelled = false;
            try {
                engine.compress(
                    input, 1000000, Preset::Quality, cancel,
                    [&](const Update &update) {
                        if (update.fraction > 0.01)
                            cancel = true;
                    },
                    true);
            } catch (const std::exception &e) {
                wasCancelled = std::string(e.what()) == "Cancelled";
            }
            size_t after = 0;
            for (auto &entry : fs::directory_iterator(input.path.parent_path()))
                if (entry.is_regular_file())
                    ++after;
            require(wasCancelled, "Cancellation did not stop encoding");
            require(before == after, "Cancellation left a partial video or pass log");
            require(fs::exists(input.path), "Cancellation removed the source");
            std::cout << "PASS: active encoding cancelled and temporary files removed\n";
        }
        CoUninitialize();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        CoUninitialize();
        return 1;
    }
}
