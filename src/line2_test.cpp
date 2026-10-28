#include <GxIAPI.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(GX_STATUS status, const char *operation) {
    if (status != GX_STATUS_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed (Galaxy status "
            + std::to_string(status) + ")");
    }
}

std::string deviceText(const unsigned char (&value)[GX_INFO_LENGTH_64_BYTE]) {
    size_t length = 0;
    while (length < sizeof(value) && value[length]) ++length;
    return {reinterpret_cast<const char *>(value), length};
}

struct GalaxyDevice {
    bool initialized = false;
    GX_DEV_HANDLE handle = nullptr;

    ~GalaxyDevice() {
        if (handle) {
            // Leave the test output inactive even if a later SDK call fails.
            GXSetBoolValue(handle, "UserOutputValue", false);
            GXCloseDevice(handle);
        }
        if (initialized) GXCloseLib();
    }
};
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc > 2) {
            std::cerr << "Usage: daheng_line2_test [camera-serial]\n";
            return 2;
        }
        const std::string wantedSerial = argc == 2 ? argv[1] : "";
        GalaxyDevice camera;
        check(GXInitLib(), "GXInitLib");
        camera.initialized = true;

        uint32_t count = 0;
        check(GXUpdateAllDeviceList(&count, 1000), "GXUpdateAllDeviceList");
        uint32_t selected = 0;
        std::string selectedSerial;
        for (uint32_t index = 1; index <= count; ++index) {
            GX_DEVICE_INFO info{};
            check(GXGetDeviceInfo(index, &info), "GXGetDeviceInfo");
            if (info.emDevType != GX_DEVICE_CLASS_U3V) continue;
            if (deviceText(info.DevInfo.stU3VDevInfo.chModelName) != "MER2-302-56U3C") continue;
            const auto serial = deviceText(info.DevInfo.stU3VDevInfo.chSerialNumber);
            if (!wantedSerial.empty() && wantedSerial != serial) continue;
            if (selected) throw std::runtime_error("Multiple matching Daheng cameras; pass the camera serial");
            selected = index;
            selectedSerial = serial;
        }
        if (!selected) throw std::runtime_error("MER2-302-56U3C not found; close the recorder and check USB");

        check(GXOpenDeviceByIndex(selected, &camera.handle), "GXOpenDeviceByIndex");
        check(GXSetEnumValueByString(camera.handle, "LineSelector", "Line2"), "LineSelector Line2");
        check(GXSetEnumValueByString(camera.handle, "LineMode", "Output"), "LineMode Output");
        check(GXSetBoolValue(camera.handle, "LineInverter", false), "LineInverter Off");
        check(GXSetEnumValueByString(camera.handle, "LineSource", "UserOutput0"), "LineSource UserOutput0");
        check(GXSetEnumValueByString(camera.handle, "UserOutputSelector", "UserOutput0"),
            "UserOutputSelector UserOutput0");

        std::cout << "Testing Daheng " << selectedSerial << ": Line2 (pin 5) against GPIO GND (pin 2).\n"
                  << "Expect a 1 Hz square wave at the resistor's camera-side junction for 10 seconds.\n"
                  << "No frames or recordings are acquired.\n";
        for (int step = 0; step < 20; ++step) {
            check(GXSetBoolValue(camera.handle, "UserOutputValue", step % 2 == 0),
                "UserOutputValue");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        check(GXSetBoolValue(camera.handle, "UserOutputValue", false), "UserOutputValue Off");
        std::cout << "Line2 test complete.\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "Line2 test failed: " << error.what() << '\n';
        return 1;
    }
}
