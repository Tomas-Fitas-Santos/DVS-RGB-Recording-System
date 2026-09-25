# DVS-RGB Recording System

This branch records **DVXplorer events and Daheng MER2-302-56U3C frames together** on one Raspberry Pi 5. Both cameras must be present to start. Their acquisition is concurrent; their device clocks are not electrically synchronized by this software.

## What it does

- Shows a live, polarity-colored event preview on the Pi display. This is an event visualization, not a conventional intensity image; a still scene may look black.
- Shows a live color Daheng preview beside the DVXplorer view while idle and during recording. The RGB preview samples BayerRG8 frames at up to 10 Hz and reduces them to at most 640 × 480 with a simple 2 × 2 Bayer color conversion. It is for framing/focus checks; saved raw frames retain their original resolution and rate. Preview work is skipped if the RGB writer queue is under pressure.
- Writes every received DVXplorer event with its camera timestamp to an `.aedat4` recording. The preview is sampled separately and is never used as the recording input.
- Saves received Daheng frames in a matching `.aedat4.rgb.raw` file as uncompressed BayerRG8. An `.aedat4.rgb.frames.csv` index records frame ID, camera timestamp ticks, host UTC and monotonic receipt times, byte offset, and size for each frame. A bounded 16-frame queue separates RGB capture from disk writes. Queue overflow or RGB capture/write error interrupts the joint recording.
- Shows RGB frame counts, missing frame IDs and incomplete frames live. The existing performance/temperature/storage monitoring includes RGB work in app CPU and process writes; the CSV and report track RGB frame rate, raw throughput and gap counts.
- Enables both external trigger edges and records them in the same AEDAT4 file, if the DVXplorer receives an external signal. This does not generate trigger pulses or synchronize camera clocks on its own.
- Stores the camera name, acquisition parameters, UTC host start/end times, and counts in an adjacent `.aedat4.json` file. Camera timestamps in AEDAT4 are authoritative for event-to-trigger alignment; the host times are operational metadata.
- Provides a touchscreen-friendly settings screen. Settings and recording directory can be changed only between recordings. The DVXplorer worker owns AEDAT4 capture and preview; separate RGB capture and writer threads share a bounded queue. The Qt GUI runs on its normal thread. No thread is pinned to a core.
- Remembers successfully applied settings across app restarts on the Pi.
- Optional 1 Hz performance, temperature, and storage diagnostics during capture, shown on the live screen and saved beside each recording.

ON/OFF contrast defaults to 9. The recorder applies no software event filtering and saves every event delivered by the DVXplorer.

## Build on the Raspberry Pi

Use a 64-bit Raspberry Pi OS installation with a graphical desktop, a C++23-capable compiler compatible with your installed dv-processing version (current dv-processing requires at least GCC 13 or Clang 18), CMake, Qt 6 Widgets development files, the **C++ dv-processing development package** including its CMake config, and the **Daheng Galaxy Linux ARM64 SDK**. The Python `dv-processing` wheel alone is not sufficient. Follow [iniVation's dv-processing installation instructions](https://dv-processing.inivation.com/master/installation.html) for your OS and architecture. Confirm DVXplorer permissions and connection with `dv-list-devices` if installed.

Copy the supplied `Galaxy_Linux-arm64_Gige-U3_2.4.2507.8231.zip` to the Pi. From the directory containing it, install the Daheng USB3 driver and udev rules, and keep the extracted SDK headers:

```bash
unzip Galaxy_Linux-arm64_Gige-U3_2.4.2507.8231.zip -d galaxy-sdk
cd galaxy-sdk/Galaxy_Linux-arm64_Gige-U3_2.4.2507.8231/Galaxy_Linux-arm64_Gige-U3_2.4.2507.8231
bash Galaxy_camera.run
```

The installer is interactive and may request `sudo`; unplug and reconnect the RGB camera after it installs the udev rules. Note the absolute path of the resulting `Galaxy_camera` directory (containing `inc/` and `lib/armv8/`). Return to the repo root and build:

```bash
sudo apt update
sudo apt install cmake ninja-build qt6-base-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGALAXY_SDK_ROOT=/absolute/path/to/Galaxy_camera
cmake --build build -j2
./build/dvxplorer_recorder
```

If `dv-processing` is not found, install its C++ development package or pass its installation prefix to CMake with `-DCMAKE_PREFIX_PATH=/path/to/prefix`. If your default compiler is too old, select a supported compiler before the first CMake configure, for example `-DCMAKE_CXX_COMPILER=g++-13` if GCC 13 is installed. Build in Release mode to reduce Pi CPU load. Pass `GALAXY_SDK_ROOT` even if `libgxiapi.so` is under `/usr/lib`: the supplied installer does not copy C headers to a standard include path.

### Pi 5 Galaxy library page-size compatibility

If the executable fails before opening with `liblog4cplus_gx.so: ELF load command address/offset not page-aligned`, check `getconf PAGE_SIZE`. The supplied Galaxy ARM64 library has 4 KiB-aligned load segments and does not load under a 16 KiB page kernel. The Pi 5's default `kernel_2712.img` commonly uses 16 KiB pages. Ask Daheng for a 16 KiB-compatible SDK library if available. To run this supplied SDK with the Raspberry Pi OS 4 KiB kernel instead, first verify that `/boot/firmware/kernel8.img` exists, back up `/boot/firmware/config.txt`, and add this at the **end** of that file:

```ini
[pi5]
kernel=kernel8.img
```

Reboot and confirm `getconf PAGE_SIZE` reports `4096`, then run the recorder again. The 4 KiB kernel has different Pi 5 optimizations; repeat capture/storage/thermal measurements under the kernel actually used for field recording. If `getconf PAGE_SIZE` already reports `4096`, investigate the installed shared library rather than changing the kernel.

## Field use

1. Connect both cameras over USB 3 and the SSD to a blue Pi USB 3 port. The Pi 5 has two blue ports, so three USB 3 peripherals require a suitable **powered USB 3 hub** on the other port or another storage interface. They share USB bandwidth; test the chosen physical topology under real load.
2. Open **Settings** while idle. Choose the SSD output directory, adjust contrast and preview interval, and select monitoring. If several Daheng cameras are attached, enter the intended RGB serial. Tap **Apply settings** and wait for the acknowledgement.
3. Confirm both live previews are visible, then tap **Start recording**. The app briefly reopens the exact Daheng model for BayerRG8/free-running acquisition and creates matching AEDAT4 and RGB raw/index files. Event, trigger and RGB counters update. **Settings** is disabled.
4. Tap **Stop recording** and wait for **Saved ...** before disconnecting power. The RGB queue drains and both files close; the AEDAT4 index finalizes and the app saves a report.
5. Inspect the file with `dv-filestat -v /path/to/file.aedat4` or open it with `dv::io::MonoCameraRecording` / DV GUI. When testing the external trigger wiring, verify the file actually contains trigger events and that the count rises.

The app stops a recording when free space drops below 1 GiB, and refuses to start when less than 2 GiB is available; it checks every 500 ms while recording. An interrupted session is marked in its metadata if the process is still running. Sudden power loss can leave the current AEDAT4 incomplete, so stop and wait for completion before powering down.

The RGB raw file is tightly packed BayerRG8: each frame has exactly `width × height` bytes with no per-frame header. Use `byte_offset` and `bytes` in the CSV to read a frame, then demosaic BayerRG8 in analysis. The JSON reports resolution, serial, frame count and errors. At full 2048 × 1536 and 56 fps, RGB alone produces about **168 MiB/s** of raw image data (plus DV events). Check the combined SSD throughput and RGB gap/queue-overflow counts. Zero frame-ID gaps cannot prove the sensor itself never missed a frame.

**Synchronization:** Daheng camera timestamps are native ticks and DVXplorer event/trigger timestamps are native microseconds; their epochs and units are not interchangeable. Host UTC times provide approximate ordering, not precise exposure-to-event alignment. For precise alignment, wire a camera exposure/trigger signal to the DVXplorer external trigger input, verify electrical compatibility, and validate edges against RGB frame IDs in a real test. The app records DVXplorer trigger events but does not generate pulses or assume a fixed clock offset.

## Recording diagnostics

The Settings page has three independent checkboxes, changeable only between recordings:

| Option | Live display and `.aedat4.monitor.csv` fields |
| --- | --- |
| Performance | Received event and trigger rates, application CPU use normalized to all online cores (100% means the entire Pi CPU capacity), whole-system CPU usage, application RAM, CPU frequency, and the longest interval between capture-loop starts. Event counts on screen use commas every three digits. |
| Temperature | Pi SoC temperature from `/sys/class/thermal/thermal_zone0/temp`. A blank CSV value or “unavailable” means that path could not be read. Treat it as a trend indicator; Raspberry Pi recommends `vcgencmd measure_temp` for an accurate instantaneous reading. |
| Storage | AEDAT4 file growth, process write rate from `/proc/self/io`, block-device write rate and I/O busy time from Linux sysfs, longest and total writer-call time, relative capture lag, writer finalization time, free filesystem space, system-wide dirty and writeback memory, and system-wide I/O pressure `some avg10`. The recording JSON identifies the filesystem’s block device when Linux exposes it, such as `mmcblk0p2` or `nvme0n1p1`. |

Enabled options are sampled approximately once per second by the existing DVXplorer camera worker. Monitoring does not create another thread or pin work to a core. RGB acquisition and file writing have dedicated threads because frame transfers and SSD writes must not stall the DV event loop. The OS schedules these threads on available cores. If any monitoring option is enabled, the app saves a `.aedat4.monitor.csv` next to the recording, and its `.aedat4.json` records the selected options, storage device, final file size, and any monitoring-file error. A CSV error does not stop event recording.

After **every recording**, the app also saves a human-readable `.aedat4.report.md` beside the AEDAT4 and JSON files. It includes the session outcome, settings, recorded event and trigger totals, peak recorded events in a one-second camera-timestamp bin, file size, finalization time, and (when monitoring was enabled) a minimum/mean/maximum table for every metric column in the CSV. A **peak event-intake interval** section selects the one-second monitoring sample with the highest recorded event rate and lists every metric from that same sample, including unavailable fields. Event and trigger rates are saved whenever any monitoring option is on so that this comparison works even if performance monitoring is off. The camera-timestamp peak count and host-sampled peak intake use different time windows and may differ. The CSV remains the complete time series with individual sample times, and the report names it. Report generation happens after capture stops on the existing worker; a report error is shown in the app and recorded in JSON. Abrupt power loss or process termination cannot produce a final report.

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

The implementation follows the [DVXplorer settings and trigger APIs](https://dv-processing.inivation.com/master/reading_data.html), [AEDAT4 writer API](https://dv-processing.inivation.com/master/writing_data.html) and the supplied Daheng Galaxy ARM64 headers/examples. This environment has no cameras, Qt development files or dv-processing C++ headers. The RGB translation unit was syntax-checked against the supplied SDK; a full app build, USB throughput, frame decoding, trigger capture, touch layout and long-duration storage tests must be run on the Pi. First check a short record/stop/playback cycle, RGB raw offsets and DV event counts; then inspect RGB gaps/queue overflows, I/O stalls, Pi temperature and relative DV capture lag in a longer simultaneous load.
