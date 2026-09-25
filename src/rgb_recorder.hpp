#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <GxIAPI.h>

namespace dvxrec {

// Owns the Galaxy device and two bounded RGB threads. The capture thread never
// waits for disk I/O; a full queue interrupts the session instead of hiding loss.
class RgbRecorder final {
public:
    struct Preview {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgb;
    };
    using PreviewCallback = std::function<void(Preview)>;

    struct Summary {
        std::string serial;
        std::string startUtc;
        std::string endUtc;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t frames = 0;
        uint64_t missingFrameIds = 0;
        uint64_t incompleteFrames = 0;
        uint64_t queueOverflows = 0;
        uint64_t bytes = 0;
        std::string error;
    };

    explicit RgbRecorder(std::filesystem::path aedatPath, std::string requestedSerial = {},
        PreviewCallback previewCallback = {}, bool previewOnly = false);
    ~RgbRecorder();
    RgbRecorder(const RgbRecorder &) = delete;
    RgbRecorder &operator=(const RgbRecorder &) = delete;

    void start();
    Summary stop();
    Summary snapshot() const;
    const std::filesystem::path &rawPath() const { return rawPath_; }
    const std::filesystem::path &indexPath() const { return indexPath_; }

private:
    struct Frame {
        uint64_t id = 0;
        uint64_t cameraTicks = 0;
        int64_t hostSteadyNs = 0;
        std::string hostUtc;
        std::vector<char> pixels;
    };
    void captureLoop() noexcept;
    void writeLoop() noexcept;
    void fail(std::string message);
    void closeDevice() noexcept;

    std::filesystem::path rawPath_;
    std::filesystem::path indexPath_;
    std::string requestedSerial_;
    PreviewCallback previewCallback_;
    bool previewOnly_ = false;
    GX_DEV_HANDLE device_ = nullptr;
    bool libraryOpen_ = false;
    bool streamOn_ = false;
    std::ofstream raw_;
    std::ofstream index_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Frame> queue_;
    Summary summary_;
    std::atomic<bool> stopping_{false};
    std::thread capture_;
    std::thread writer_;
};

} // namespace dvxrec
