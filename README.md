# DVS-RGB Recording System

This branch currently contains the **DVXplorer-only recorder** for one Raspberry Pi 5. The RGB camera and cross-device synchronization are separate later stages.

## What it does

- Shows a live, polarity-colored event preview on the Pi display. This is an event visualization, not a conventional intensity image; a still scene may look black.
- Writes every received DVXplorer event with its camera timestamp to an `.aedat4` recording. The preview is sampled separately and is never used as the recording input.
- Enables both external trigger edges and records them in the same AEDAT4 file, if the DVXplorer receives an external signal. This does not generate trigger pulses or synchronize camera clocks on its own.
- Stores the camera name, acquisition parameters, UTC host start/end times, and counts in an adjacent `.aedat4.json` file. Camera timestamps in AEDAT4 are authoritative for event-to-trigger alignment; the host times are operational metadata.
- Provides a touchscreen-friendly settings screen. Settings and recording directory can be changed only between recordings. One worker thread handles USB capture, AEDAT4 writing, and the cheap preview rasterization; the Qt GUI runs on its normal thread. There is no unbounded event queue or separate processing thread.
- Remembers successfully applied settings across app restarts on the Pi.

Hardware background activity and refractory filtering **remove events before they reach the recorder**. Both are off by default to retain all events the camera delivers. ON/OFF contrast defaults to 9, the DVXplorer default in the current API documentation. The 250 µs filter values are hardware units, not milliseconds.

## Build on the Raspberry Pi

Use a 64-bit Raspberry Pi OS installation with a graphical desktop, a C++23-capable compiler compatible with your installed dv-processing version (current dv-processing requires at least GCC 13 or Clang 18), CMake, Qt 6 Widgets development files, and the **C++ dv-processing development package** including its CMake config. The Python `dv-processing` wheel alone is not sufficient. Follow [iniVation's dv-processing installation instructions](https://dv-processing.inivation.com/master/installation.html) for your OS and architecture. Confirm camera permissions and connection with `dv-list-devices` if installed.

```bash
sudo apt update
sudo apt install cmake ninja-build qt6-base-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/dvxplorer_recorder
```

If `dv-processing` is not found, install its C++ development package or pass its installation prefix to CMake with `-DCMAKE_PREFIX_PATH=/path/to/prefix`. If your default compiler is too old, select a supported compiler before the first CMake configure, for example `-DCMAKE_CXX_COMPILER=g++-13` if GCC 13 is installed. Build in Release mode to reduce Pi CPU load.

## Field use

1. Connect a full-size DVXplorer to a USB 3 port. Start the app and wait for the camera name and resolution to appear.
2. Open **Settings** while idle. Choose an output directory and adjust ON/OFF contrast or hardware filters if needed. Tap **Apply settings** and wait for the acknowledgement.
3. Tap **Start recording**. A new UTC-named AEDAT4 file is created. The event and trigger counters update while recording. **Settings** is disabled.
4. Tap **Stop recording** and wait for **Saved ...** before disconnecting power. The AEDAT4 writer finalizes its index when closed.
5. Inspect the file with `dv-filestat -v /path/to/file.aedat4` or open it with `dv::io::MonoCameraRecording` / DV GUI. When testing the external trigger wiring, verify the file actually contains trigger events and that the count rises.

The app stops a recording when free space drops below 128 MiB, and refuses to start when less than 256 MiB is available. An interrupted session is marked in its metadata if the process is still running. Sudden power loss can leave the current AEDAT4 incomplete, so stop and wait for completion before powering down.

## Verification status

The implementation follows the [DVXplorer settings and trigger APIs](https://dv-processing.inivation.com/master/reading_data.html) and [AEDAT4 writer API](https://dv-processing.inivation.com/master/writing_data.html). This environment has no DVXplorer, Qt development files, or dv-processing C++ headers, so device compilation, USB throughput, trigger capture, touch layout, and long-duration storage tests must be run on the Pi. The first field test should check a full record/stop/playback cycle and inspect event/trigger counts before collecting research data.
