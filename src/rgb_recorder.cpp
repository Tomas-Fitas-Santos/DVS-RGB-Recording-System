#include "rgb_recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dvxrec {
namespace {
std::string utcNow() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto seconds = floor<std::chrono::seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - seconds).count();
    const auto epoch = system_clock::to_time_t(now);
    std::tm parts{};
    gmtime_r(&epoch, &parts);
    char buffer[40];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &parts);
    char result[48];
    std::snprintf(result, sizeof(result), "%s.%03dZ", buffer, static_cast<int>(millis));
    return result;
}

int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void check(GX_STATUS status, const char *operation) {
    if (status != GX_STATUS_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed (Galaxy status "
            + std::to_string(status) + ")");
    }
}

std::string cameraString(GX_DEV_HANDLE device, const char *feature) {
    GX_STRING_VALUE value{};
    check(GXGetStringValue(device, feature, &value), feature);
    return value.strCurValue;
}
} // namespace

RgbRecorder::RgbRecorder(std::filesystem::path aedatPath, std::string requestedSerial)
    : rawPath_(aedatPath.string() + ".rgb.raw"),
      indexPath_(aedatPath.string() + ".rgb.frames.csv"),
      requestedSerial_(std::move(requestedSerial)) {}

RgbRecorder::~RgbRecorder() {
    stop();
}

void RgbRecorder::closeDevice() noexcept {
    if (streamOn_) {
        GXStreamOff(device_);
        streamOn_ = false;
    }
    if (device_) {
        GXCloseDevice(device_);
        device_ = nullptr;
    }
    if (libraryOpen_) {
        GXCloseLib();
        libraryOpen_ = false;
    }
}

void RgbRecorder::start() {
    try {
        check(GXInitLib(), "GXInitLib");
        libraryOpen_ = true;
        uint32_t count = 0;
        check(GXUpdateAllDeviceList(&count, 1000), "GXUpdateAllDeviceList");
        for (uint32_t i = 1; i <= count; ++i) {
            GX_DEV_HANDLE candidate = nullptr;
            if (GXOpenDeviceByIndex(i, &candidate) != GX_STATUS_SUCCESS) continue;
            try {
                const auto model = cameraString(candidate, "DeviceModelName");
                const auto serial = cameraString(candidate, "DeviceSerialNumber");
                if (model == "MER2-302-56U3C"
                    && (requestedSerial_.empty() || serial == requestedSerial_)) {
                    device_ = candidate;
                    summary_.serial = serial;
                    break;
                }
            }
            catch (...) {
                GXCloseDevice(candidate);
                throw;
            }
            GXCloseDevice(candidate);
        }
        if (!device_) throw std::runtime_error("MER2-302-56U3C not found (check Galaxy SDK, USB and serial)");

        check(GXSetEnumValueByString(device_, "PixelFormat", "BayerRG8"), "PixelFormat BayerRG8");
        check(GXSetEnumValueByString(device_, "AcquisitionMode", "Continuous"), "AcquisitionMode");
        check(GXSetEnumValueByString(device_, "TriggerSelector", "FrameStart"), "TriggerSelector FrameStart");
        check(GXSetEnumValueByString(device_, "TriggerMode", "Off"), "TriggerMode Off");
        GX_INT_VALUE width{}, height{}, payload{};
        check(GXGetIntValue(device_, "Width", &width), "Width");
        check(GXGetIntValue(device_, "Height", &height), "Height");
        check(GXGetIntValue(device_, "PayloadSize", &payload), "PayloadSize");
        if (width.nCurValue <= 0 || height.nCurValue <= 0 || payload.nCurValue <= 0
            || payload.nCurValue > std::numeric_limits<int32_t>::max()) {
            throw std::runtime_error("Invalid RGB dimensions or payload size");
        }
        summary_.width = static_cast<uint32_t>(width.nCurValue);
        summary_.height = static_cast<uint32_t>(height.nCurValue);
        raw_.open(rawPath_, std::ios::binary | std::ios::trunc);
        index_.open(indexPath_, std::ios::trunc);
        if (!raw_ || !index_) throw std::runtime_error("Cannot create RGB raw/index files");
        index_ << "frame_index,frame_id,camera_timestamp_ticks,host_utc,host_steady_ns,byte_offset,bytes,width,height,pixel_format\n";
        if (!index_) throw std::runtime_error("Cannot write RGB index header");
        check(GXSetAcqusitionBufferNumber(device_, 16), "GXSetAcqusitionBufferNumber");
        check(GXStreamOn(device_), "GXStreamOn");
        streamOn_ = true;
        summary_.startUtc = utcNow();
        writer_ = std::thread([this] { writeLoop(); });
        capture_ = std::thread([this] { captureLoop(); });
    }
    catch (...) {
        stop();
        throw;
    }
}

void RgbRecorder::fail(std::string message) {
    {
        std::lock_guard lock(mutex_);
        if (summary_.error.empty()) summary_.error = std::move(message);
    }
    stopping_ = true;
    ready_.notify_all();
}

void RgbRecorder::captureLoop() noexcept {
    try {
        GX_INT_VALUE payload{};
        check(GXGetIntValue(device_, "PayloadSize", &payload), "PayloadSize");
        std::vector<char> buffer(static_cast<size_t>(payload.nCurValue));
        uint64_t previousId = 0;
        bool havePreviousId = false;
        while (!stopping_) {
            GX_FRAME_DATA frame{};
            frame.pImgBuf = buffer.data();
            const auto status = GXGetImage(device_, &frame, 100);
            if (status == GX_STATUS_TIMEOUT) continue;
            check(status, "GXGetImage");
            if (frame.nStatus != GX_FRAME_STATUS_SUCCESS) {
                std::lock_guard lock(mutex_);
                ++summary_.incompleteFrames;
                continue;
            }
            if (frame.nPixelFormat != GX_PIXEL_FORMAT_BAYER_RG8
                || frame.nWidth != static_cast<int32_t>(summary_.width)
                || frame.nHeight != static_cast<int32_t>(summary_.height)
                || frame.nImgSize <= 0 || static_cast<size_t>(frame.nImgSize) > buffer.size()
                || static_cast<size_t>(frame.nImgSize) != size_t(summary_.width) * summary_.height) {
                throw std::runtime_error("RGB frame format/size changed during capture");
            }
            Frame item;
            item.id = frame.nFrameID;
            item.cameraTicks = frame.nTimestamp;
            item.hostSteadyNs = steadyNs();
            item.hostUtc = utcNow();
            item.pixels.assign(buffer.begin(), buffer.begin() + frame.nImgSize);
            bool overflow = false;
            {
                std::lock_guard lock(mutex_);
                if (havePreviousId && frame.nFrameID > previousId + 1)
                    summary_.missingFrameIds += frame.nFrameID - previousId - 1;
                previousId = frame.nFrameID;
                havePreviousId = true;
                if (queue_.size() == 16) {
                    ++summary_.queueOverflows;
                    overflow = true;
                }
                else queue_.push_back(std::move(item));
            }
            if (overflow) {
                fail("RGB writer queue full: storage cannot keep up with camera");
                break;
            }
            ready_.notify_one();
        }
    }
    catch (const std::exception &e) { fail(e.what()); }
}

void RgbRecorder::writeLoop() noexcept {
    try {
        for (;;) {
            Frame item;
            uint64_t index = 0, offset = 0;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) break;
                item = std::move(queue_.front());
                queue_.pop_front();
                index = summary_.frames;
                offset = summary_.bytes;
            }
            raw_.write(item.pixels.data(), static_cast<std::streamsize>(item.pixels.size()));
            index_ << index << ',' << item.id << ',' << item.cameraTicks << ','
                   << item.hostUtc << ',' << item.hostSteadyNs << ',' << offset << ','
                   << item.pixels.size() << ',' << summary_.width << ',' << summary_.height
                   << ",BayerRG8\n";
            if (!raw_ || !index_) throw std::runtime_error("RGB storage write failed");
            {
                std::lock_guard lock(mutex_);
                ++summary_.frames;
                summary_.bytes += item.pixels.size();
            }
        }
        raw_.flush();
        index_.flush();
        if (!raw_ || !index_) throw std::runtime_error("RGB storage flush failed");
    }
    catch (const std::exception &e) { fail(e.what()); }
}

RgbRecorder::Summary RgbRecorder::snapshot() const {
    std::lock_guard lock(mutex_);
    return summary_;
}

RgbRecorder::Summary RgbRecorder::stop() {
    stopping_ = true;
    ready_.notify_all();
    if (capture_.joinable()) capture_.join();
    if (writer_.joinable()) writer_.join();
    closeDevice();
    if (raw_.is_open()) raw_.close();
    if (index_.is_open()) index_.close();
    std::lock_guard lock(mutex_);
    if (!summary_.startUtc.empty() && summary_.endUtc.empty()) summary_.endUtc = utcNow();
    return summary_;
}

} // namespace dvxrec
