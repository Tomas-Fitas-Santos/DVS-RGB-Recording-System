# DVS-RGB Recording System

This branch currently contains the **DVXplorer-only recorder** for one Raspberry Pi 5. The RGB camera and cross-device synchronization are separate later stages.

## What it does

- Shows a live, polarity-colored event preview on the Pi display. This is an event visualization, not a conventional intensity image; a still scene may look black.
- Writes every received DVXplorer event with its camera timestamp to an `.aedat4` recording. The preview is sampled separately and is never used as the recording input.
- Enables both external trigger edges and records them in the same AEDAT4 file, if the DVXplorer receives an external signal. This does not generate trigger pulses or synchronize camera clocks on its own.
- Stores the camera name, acquisition parameters, UTC host start/end times, and counts in an adjacent `.aedat4.json` file. Camera timestamps in AEDAT4 are authoritative for event-to-trigger alignment; the host times are operational metadata.
- Provides a touchscreen-friendly settings screen. Settings and recording directory can be changed only between recordings. One worker thread handles USB capture, AEDAT4 writing, and the cheap preview rasterization; the Qt GUI runs on its normal thread. There is no unbounded event queue or separate processing thread.
- Remembers successfully applied settings across app restarts on the Pi.
- Optional 1 Hz performance, temperature, and storage diagnostics during capture, shown on the live screen and saved beside each recording.

ON/OFF contrast defaults to 9. The recorder applies no software event filtering and saves every event delivered by the DVXplorer.

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
2. Open **Settings** while idle. Choose an output directory, adjust ON/OFF contrast or preview interval, and select any monitoring options. Tap **Apply settings** and wait for the acknowledgement.
3. Tap **Start recording**. A new UTC-named AEDAT4 file is created. The event and trigger counters update while recording. **Settings** is disabled.
4. Tap **Stop recording** and wait for **Saved ...** before disconnecting power. The AEDAT4 writer finalizes its index when closed.
5. Inspect the file with `dv-filestat -v /path/to/file.aedat4` or open it with `dv::io::MonoCameraRecording` / DV GUI. When testing the external trigger wiring, verify the file actually contains trigger events and that the count rises.

The app stops a recording when free space drops below 128 MiB, and refuses to start when less than 256 MiB is available. An interrupted session is marked in its metadata if the process is still running. Sudden power loss can leave the current AEDAT4 incomplete, so stop and wait for completion before powering down.

## Recording diagnostics

The Settings page has three independent checkboxes, changeable only between recordings:

| Option | Live display and `.aedat4.monitor.csv` fields |
| --- | --- |
| Performance | Received event and trigger rates, process CPU usage across all application threads (it may exceed 100% on a multicore Pi), whole-system CPU usage, process resident memory, CPU frequency, and maximum time between capture-loop starts. |
| Temperature | Pi SoC temperature from `/sys/class/thermal/thermal_zone0/temp`. A blank CSV value or “unavailable” means that path could not be read. |
| Storage | AEDAT4 file growth, process write rate from `/proc/self/io`, block-device write rate and I/O busy time from Linux sysfs, longest and total writer-call time, writer finalization time, free filesystem space, system-wide dirty and writeback memory, and system-wide I/O pressure `some avg10`. The recording JSON identifies the filesystem’s block device when Linux exposes it, such as `mmcblk0p2` or `nvme0n1p1`. |

Enabled options are sampled approximately once per second by the existing camera worker. They do not create another thread or pin work to a core. The GUI retains its normal Qt thread. If any monitoring option is enabled, the app saves a `.aedat4.monitor.csv` next to the recording, and its `.aedat4.json` records the selected options, storage device, final file size, and any monitoring-file error. A CSV error does not stop event recording.

To compare SD card and SSD, collect several minutes of recordings with comparable scenes and settings. Compare sustained file growth, process write rate, and block-device write rate against event rate, and look for writer-call spikes, capture-loop delays, growing dirty data, I/O pressure, declining free space, and temperature alongside CPU frequency. Linux can buffer writes in RAM; the process write count includes other files, block-device counters include other processes on that filesystem, and dirty memory and I/O pressure cover the whole system. These readings help locate a bottleneck but cannot alone prove zero sensor event loss.

## Verification status

The implementation follows the [DVXplorer settings and trigger APIs](https://dv-processing.inivation.com/master/reading_data.html) and [AEDAT4 writer API](https://dv-processing.inivation.com/master/writing_data.html). This environment has no DVXplorer, Qt development files, or dv-processing C++ headers, so device compilation, USB throughput, trigger capture, touch layout, and long-duration storage tests must be run on the Pi. The first field test should check a full record/stop/playback cycle and inspect event/trigger counts before collecting research data.
