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
| Performance | Received event and trigger rates, application CPU use normalized to all online cores (100% means the entire Pi CPU capacity), whole-system CPU usage, application RAM, CPU frequency, and the longest interval between capture-loop starts. Event counts on screen use commas every three digits. |
| Temperature | Pi SoC temperature from `/sys/class/thermal/thermal_zone0/temp`. A blank CSV value or “unavailable” means that path could not be read. Treat it as a trend indicator; Raspberry Pi recommends `vcgencmd measure_temp` for an accurate instantaneous reading. |
| Storage | AEDAT4 file growth, process write rate from `/proc/self/io`, block-device write rate and I/O busy time from Linux sysfs, longest and total writer-call time, relative capture lag, writer finalization time, free filesystem space, system-wide dirty and writeback memory, and system-wide I/O pressure `some avg10`. The recording JSON identifies the filesystem’s block device when Linux exposes it, such as `mmcblk0p2` or `nvme0n1p1`. |

Enabled options are sampled approximately once per second by the existing camera worker. They do not create another thread or pin work to a core. The GUI retains its normal Qt thread. If any monitoring option is enabled, the app saves a `.aedat4.monitor.csv` next to the recording, and its `.aedat4.json` records the selected options, storage device, final file size, and any monitoring-file error. A CSV error does not stop event recording.

The **Saved** message and JSON include the peak number of recorded events in any one-second camera-timestamp bin. This is measured while recording even if monitoring is off. The live event-rate display instead divides events received since the last monitoring sample by its actual elapsed time.

The live labels mean:

| Label | What it measures |
| --- | --- |
| App RAM (formerly RSS) | Physical RAM pages resident for this recorder process, in MiB. |
| AEDAT file growth | Change in the visible `.aedat4` file size per elapsed second. The writer or Linux may buffer data, so it is not the SD card's actual physical write speed. |
| SD/SSD writes | Completed sector writes per elapsed second on the block device containing the output folder; it includes other applications writing to that partition. It can be unavailable on non-block filesystems. |
| Longest gap between capture checks (formerly loop max) | Largest time between starts of successive capture-loop passes during the last sampling interval, including time spent writing and rendering the preview. It is not an event-loss count. |
| Slowest AEDAT write (formerly writer max) | Longest individual AEDAT writer call for one event batch or trigger batch during the interval. It includes encoding and buffering and does not mean the data was physically flushed to storage. |
| Relative capture lag | Additional delay in receiving the newest camera timestamp relative to the first recorded event batch. It can increase while the worker is blocked, but is not an absolute camera-to-host latency measurement. A camera timestamp reset restarts the baseline. |

**Lost events:** The app shows `unavailable` for accumulated lost events and lost events per second, and leaves the CSV numeric fields empty (`loss_count_status=unavailable`). A displayed zero would incorrectly imply proof of no loss. The DVXplorer has a device-side USB-buffer drop counter, but the public camera interface used by this recorder does not expose a complete event-loss count. Host queues can also drop packets, and sensor-side losses cannot always be counted. The JSON sets `event_loss_count_available=false`. Treat write stalls and growing relative capture lag as warning signs, not as a measured number of missing events. [iniVation describes the distinct loss locations](https://docs.inivation.com/help/faq.html#can-events-be-lost) and [the DVXplorer API lists its device counters](https://dv-processing.inivation.com/master/api.html).

To compare SD card and SSD, collect several minutes of recordings with comparable scenes and settings. Compare sustained file growth, process write rate, and block-device write rate against event rate, and look for writer-call spikes, capture-loop delays, growing dirty data, I/O pressure, declining free space, and temperature alongside CPU frequency. Linux can buffer writes in RAM; the process write count includes other files, block-device counters include other processes on that filesystem, and dirty memory and I/O pressure cover the whole system. These readings help locate a bottleneck but cannot alone prove zero sensor event loss.

## Verification status

The implementation follows the [DVXplorer settings and trigger APIs](https://dv-processing.inivation.com/master/reading_data.html) and [AEDAT4 writer API](https://dv-processing.inivation.com/master/writing_data.html). This environment has no DVXplorer, Qt development files, or dv-processing C++ headers, so device compilation, USB throughput, trigger capture, touch layout, and long-duration storage tests must be run on the Pi. The first field test should check a full record/stop/playback cycle and inspect event/trigger counts before collecting research data.
