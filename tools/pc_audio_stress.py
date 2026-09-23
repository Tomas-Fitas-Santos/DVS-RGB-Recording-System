#!/usr/bin/env python3
"""PC audio stimulus and offline DVXplorer AEDAT4 timing check.

Point the camera at a visible, contrasting mark on an exposed speaker cone.
The Pi runs the ordinary C++ recorder; this program runs only on the PC.
"""

import argparse
from array import array
from datetime import datetime, timezone
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time


def parse_roi(text):
    try:
        roi = tuple(map(int, text.split(",")))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("ROI must be x0,y0,x1,y1") from exc
    if len(roi) != 4 or roi[0] < 0 or roi[1] < 0 or roi[2] <= roi[0] or roi[3] <= roi[1]:
        raise argparse.ArgumentTypeError("ROI must satisfy 0 <= x0 < x1, 0 <= y0 < y1")
    return roi


def start_monitor(destination, path, seconds):
    """Optional low-rate Pi monitoring over one SSH connection, no Pi installation."""
    if destination.startswith("-"):
        raise ValueError("invalid SSH destination")
    path.parent.mkdir(parents=True, exist_ok=True)
    file = path.open("w", encoding="utf-8")
    file.write("utc_epoch_s,temp_millic,throttled,cpu_percent_lifetime,rss_kib\n")
    file.flush()
    script = (f"for ((i=0;i<{seconds};i++)); do "
              "t=$(date -u +%s); "
              "temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null); "
              "throttle=$(vcgencmd get_throttled 2>/dev/null); "
              "read -r cpu rss < <(ps -C dvxplorer_recorder -o %cpu=,rss= | head -n 1); "
              "printf '%s,%s,%s,%s,%s\\n' \"$t\" \"$temp\" \"$throttle\" \"$cpu\" \"$rss\"; "
              "sleep 1; done")
    process = subprocess.Popen(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
                                destination, "bash -c " + shlex.quote(script)], stdout=file,
                               stderr=subprocess.PIPE, text=True)
    return process, file


def play(args):
    try:
        import numpy as np
        import sounddevice as sd
    except ImportError as exc:
        raise SystemExit("On the PC install dependencies: python -m pip install numpy sounddevice") from exc
    if not 1 <= args.hz <= 600 or not 3 <= args.duration <= 300:
        raise ValueError("frequency must be 1..600 Hz and duration 3..300 s")
    if not 0 < args.amplitude <= 0.5 or args.sample_rate < 8 * args.hz:
        raise ValueError("amplitude must be (0,0.5], sample rate at least 8 times frequency")
    if not 0 <= args.lead <= 60:
        raise ValueError("lead must be 0..60 seconds")
    manifest_path = Path(args.manifest)
    if manifest_path.exists():
        raise ValueError(f"Manifest already exists: {manifest_path}")
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    count = round(args.duration * args.sample_rate)
    samples = np.arange(count, dtype=np.float64)
    signal = (args.amplitude * np.sin(2 * np.pi * args.hz * samples / args.sample_rate)).astype("float32")
    # Avoid an abrupt click at start and stop; the middle retains the full tone.
    ramp = min(count // 4, round(0.02 * args.sample_rate))
    if ramp:
        signal[:ramp] *= np.linspace(0, 1, ramp, dtype="float32")
        signal[-ramp:] *= np.linspace(1, 0, ramp, dtype="float32")
    monitor = None
    monitor_file = None
    manifest = {"protocol": "pc-audio-stress-v1", "requested_hz": args.hz,
                "sample_rate_hz": args.sample_rate, "sample_count": count,
                "duration_s": count / args.sample_rate, "amplitude": args.amplitude,
                "output_device": args.device, "audio_status": "not_started"}
    try:
        if args.ssh:
            monitor, monitor_file = start_monitor(args.ssh, Path(args.monitor),
                                                   int(args.lead + args.duration + 15))
        print(f"Start the normal recorder on the Pi. Tone starts in {args.lead:g} s; "
              f"{args.hz:g} Hz for {args.duration:g} s.", flush=True)
        time.sleep(args.lead)
        manifest["pc_play_started_utc"] = datetime.now(timezone.utc).isoformat()
        sd.play(signal, samplerate=args.sample_rate, device=args.device, blocking=False)
        status = sd.wait()  # Blocks until the whole buffered waveform is played.
        manifest["pc_play_ended_utc"] = datetime.now(timezone.utc).isoformat()
        manifest["audio_status"] = str(status) if status else "complete_no_reported_errors"
        manifest["audio_output_underflow"] = bool(status and status.output_underflow)
        print("Tone complete. Stop the Pi recording and wait for Saved.")
    except KeyboardInterrupt:
        sd.stop()
        manifest["audio_status"] = "interrupted"
        print("Audio interrupted. Stop and finalize the Pi recording.", file=sys.stderr)
    finally:
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if monitor is not None:
            monitor.terminate()
            try:
                monitor.wait(timeout=3)
            except subprocess.TimeoutExpired:
                monitor.kill()
                monitor.wait()
            stderr = monitor.stderr.read()
            monitor.stderr.close()
            monitor_file.close()
            if stderr:
                print(f"Pi monitor: {stderr.strip()}", file=sys.stderr)
    print(f"Saved PC timing manifest: {manifest_path}")


def read_roi_events(path, roi, polarity):
    try:
        import dv_processing as dv
    except ImportError as exc:
        raise SystemExit("On the analysis PC install dv-processing Python") from exc
    reader = dv.io.MonoCameraRecording(str(path))
    if not reader.isEventStreamAvailable():
        raise ValueError("No event stream in AEDAT4")
    timestamps = array("q")
    total_events = 0
    while reader.isRunning():
        batch = reader.getNextEventBatch()
        if batch is None:
            continue
        total_events += len(batch)
        for event in batch:
            if roi[0] <= event.x() < roi[2] and roi[1] <= event.y() < roi[3] \
                    and (polarity == "both" or bool(event.polarity()) == (polarity == "on")):
                timestamps.append(int(event.timestamp()))
    return timestamps, total_events


def window_metrics(timestamps_us, frequency_hz, seconds, min_events, start_us=None):
    """Assess second-by-second optical coverage at the expected tone period.

    The first selected event is an approximate start; no PC/camera clock sync exists.
    """
    import numpy as np
    events = np.frombuffer(timestamps_us, dtype=np.int64)
    if not len(events):
        return []
    start = int(events[0] if start_us is None else start_us)
    period = 1_000_000 / frequency_hz
    windows = []
    for index in range(int(seconds)):
        left = int(np.searchsorted(events, start + index * 1_000_000))
        right = int(np.searchsorted(events, start + (index + 1) * 1_000_000))
        segment = events[left:right].astype(np.float64)
        if not len(segment):
            windows.append({"second": index, "events": 0, "cycle_coverage": 0.0,
                            "coherence_f": 0.0, "coherence_2f": 0.0})
            continue
        relative = segment - (start + index * 1_000_000)
        # A sine-driven edge may produce one or two bursts per cycle. Optimize
        # phase within this window; the camera and sound card clocks can drift.
        coverage = 0.0
        cycles = max(1, round(frequency_hz))
        for phase in np.linspace(0, period, 32, endpoint=False):
            bins = np.floor((relative + phase) / period).astype(np.int64)
            counts = np.bincount(bins[bins < cycles], minlength=cycles)
            coverage = max(coverage, float(np.count_nonzero(counts >= min_events) / cycles))
        phase_radians = 2 * np.pi * (relative / period)
        coherence_f = abs(np.exp(1j * phase_radians).mean())
        coherence_2f = abs(np.exp(2j * phase_radians).mean())
        windows.append({"second": index, "events": len(segment),
                        "cycle_coverage": round(coverage, 5),
                        "coherence_f": round(float(coherence_f), 5),
                        "coherence_2f": round(float(coherence_2f), 5)})
    return windows


def analyze(args):
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    if manifest.get("protocol") != "pc-audio-stress-v1":
        raise ValueError("Wrong manifest format")
    timestamps, total_events = read_roi_events(args.aedat4, args.roi, args.polarity)
    # Trim the start/end transients. Keep 1-second complete windows.
    seconds = max(0, int(manifest["duration_s"]) - 1)
    metrics = window_metrics(timestamps, manifest["requested_hz"], seconds, args.min_events,
                             args.start_us)
    good_windows = sum(w["cycle_coverage"] >= args.coverage and
                       max(w["coherence_f"], w["coherence_2f"]) >= args.coherence
                       for w in metrics)
    report = {"protocol": "pc-audio-stress-analysis-v1", "aedat4": str(args.aedat4),
              "frequency_hz": manifest["requested_hz"], "total_recorded_events": total_events,
              "selected_roi_events": len(timestamps), "roi": args.roi, "polarity": args.polarity,
              "complete_windows": seconds, "responsive_windows": good_windows,
              "audio_output_underflow": manifest.get("audio_output_underflow"),
              "audio_status": manifest.get("audio_status"), "windows": metrics,
              "analysis_start_us": args.start_us if args.start_us is not None else
                                   (timestamps[0] if timestamps else None),
              "interpretation": "Optical frequency response only; no electrical ground-truth reference."}
    sidecar = Path(str(args.aedat4) + ".json")
    if sidecar.exists():
        metadata = json.loads(sidecar.read_text(encoding="utf-8"))
        report["pi_recording_state"] = metadata.get("recording_state")
        report["pi_event_count_matches"] = metadata.get("event_count") == total_events
        report["pi_trigger_count"] = metadata.get("trigger_count")
    monitor = Path(args.monitor) if args.monitor else None
    if monitor is not None and monitor.exists():
        temperatures = []
        throttling = False
        for line in monitor.read_text(encoding="utf-8").splitlines()[1:]:
            fields = line.split(",")
            if len(fields) < 3:
                continue
            try:
                temperatures.append(int(fields[1]) / 1000)
                throttling |= int(fields[2].split("=")[-1], 16) != 0
            except ValueError:
                pass
        report["pi_monitor"] = {"samples": len(temperatures),
                                "max_temp_c": max(temperatures, default=None),
                                "throttling_reported": throttling}
    report["result"] = "CONSISTENT_OPTICAL_RESPONSE" if (
        good_windows == seconds and seconds > 0 and manifest.get("audio_output_underflow") is False
        and report.get("pi_recording_state", "complete") == "complete"
        and report.get("pi_event_count_matches", True)
        and not report.get("pi_monitor", {}).get("throttling_reported", False)) else "INCONCLUSIVE"
    output = Path(args.report)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{report['result']} at {report['frequency_hz']} Hz: {good_windows}/{seconds} "
          f"one-second windows responsive, {len(timestamps)} selected ROI events.")
    print(f"Report: {output}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sound = sub.add_parser("play", help="PC: play a sine tone through a visible speaker")
    sound.add_argument("--hz", type=float, required=True)
    sound.add_argument("--duration", type=float, default=30)
    sound.add_argument("--sample-rate", type=int, default=48000)
    sound.add_argument("--amplitude", type=float, default=0.1)
    sound.add_argument("--lead", type=float, default=5)
    sound.add_argument("--device", type=int, help="sounddevice output device index")
    sound.add_argument("--manifest", default="audio_stress_manifest.json")
    sound.add_argument("--ssh", help="optional Pi SSH destination for temperature and CPU sampling")
    sound.add_argument("--monitor", default="pi_stress_monitor.csv")
    check = sub.add_parser("analyze", help="PC: analyze an AEDAT4 copied from the Pi")
    check.add_argument("aedat4", type=Path)
    check.add_argument("--manifest", default="audio_stress_manifest.json")
    check.add_argument("--roi", type=parse_roi, required=True)
    check.add_argument("--start-us", type=int, help="camera timestamp of first stimulus event, if ROI has pre-tone noise")
    check.add_argument("--polarity", choices=("on", "off", "both"), default="on")
    check.add_argument("--min-events", type=int, default=2)
    check.add_argument("--coverage", type=float, default=0.99)
    check.add_argument("--coherence", type=float, default=0.15)
    check.add_argument("--monitor", help="monitor CSV produced by play --ssh for this trial")
    check.add_argument("--report", default="audio_stress_report.json")
    args = parser.parse_args(argv)
    if args.command == "play":
        play(args)
    else:
        if args.min_events < 1 or not 0 <= args.coverage <= 1 or not 0 <= args.coherence <= 1:
            parser.error("invalid event, coverage or coherence threshold")
        analyze(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as exc:
        sys.exit(f"Error: {exc}")
