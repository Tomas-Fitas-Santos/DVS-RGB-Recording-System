#!/usr/bin/env bash
# Build and control the recorder shown on the Raspberry Pi's HDMI desktop.
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
app="$root_dir/build/dvxplorer_recorder"
state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/dvs-rgb-recorder"
pid_file="$state_dir/app.pid"
log_file="$state_dir/app.log"
mkdir -p -- "$state_dir"

# Serialize competing SSH commands. The recorder itself closes this descriptor.
exec 9>"$state_dir/control.lock"
flock -x 9

running_pid() {
    local pid executable
    [[ -r "$pid_file" ]] || return 1
    IFS= read -r pid < "$pid_file" || return 1
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    executable="$(readlink "/proc/$pid/exe" 2>/dev/null)" || return 1
    [[ "$executable" == "$app" || "$executable" == "$app (deleted)" ]] || return 1
    printf '%s\n' "$pid"
}

unmanaged_pid() {
    local process executable pid
    for process in /proc/[0-9]*; do
        pid="${process##*/}"
        executable="$(readlink "$process/exe" 2>/dev/null)" || continue
        if [[ "$executable" == "$app" || "$executable" == "$app (deleted)" ]]; then
            printf '%s\n' "$pid"
            return 0
        fi
    done
    return 1
}

build_app() {
    if running_pid >/dev/null; then
        echo "Stop the recorder before rebuilding it." >&2
        return 1
    fi
    if unmanaged_pid >/dev/null; then
        echo "A recorder started outside this launcher is still running; close it first." >&2
        return 1
    fi
    if [[ ! -f "$root_dir/build/build.ninja" && ! -f "$root_dir/build/Makefile" ]]; then
        echo "Configure the build first with CMake and your GALAXY_SDK_ROOT setting." >&2
        return 1
    fi
    cmake --build "$root_dir/build" -j2
}

start_app() {
    local runtime socket candidate pid
    if pid="$(running_pid)"; then
        echo "Recorder already running (PID $pid)."
        return 0
    fi
    if pid="$(unmanaged_pid)"; then
        echo "Recorder already running outside this launcher (PID $pid); close it first." >&2
        return 1
    fi
    [[ -x "$app" ]] || { echo "No built recorder. Run: bash scripts/recorder.sh build" >&2; return 1; }
    rm -f -- "$pid_file"

    runtime="/run/user/$(id -u)"
    socket=""
    if [[ -n "${WAYLAND_DISPLAY:-}" && -S "$runtime/$WAYLAND_DISPLAY" ]]; then
        socket="$WAYLAND_DISPLAY"
    else
        for candidate in "$runtime"/wayland-*; do
            if [[ -S "$candidate" ]]; then
                socket="${candidate##*/}"
                break
            fi
        done
    fi

    if [[ -n "$socket" ]]; then
        nohup env XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY="$socket" QT_QPA_PLATFORM=wayland \
            "$app" >"$log_file" 2>&1 < /dev/null 9>&- &
    elif [[ -S /tmp/.X11-unix/X0 ]]; then
        nohup env DISPLAY=:0 QT_QPA_PLATFORM=xcb \
            "$app" >"$log_file" 2>&1 < /dev/null 9>&- &
    else
        echo "No HDMI desktop display found. Log in to the Pi's graphical desktop first." >&2
        return 1
    fi
    pid=$!
    printf '%s\n' "$pid" > "$pid_file"
    sleep 1
    if ! running_pid >/dev/null; then
        rm -f -- "$pid_file"
        echo "Recorder exited during launch; see $log_file" >&2
        tail -n 20 "$log_file" >&2
        return 1
    fi
    echo "Recorder open on the Pi HDMI desktop (PID $pid)."
    echo "Use the buttons on that screen to start and stop recording. Log: $log_file"
}

stop_app() {
    local pid attempt
    if ! pid="$(running_pid)"; then
        if pid="$(unmanaged_pid)"; then
            echo "Recorder PID $pid was opened outside this launcher; close it on the HDMI desktop first." >&2
            return 1
        fi
        rm -f -- "$pid_file"
        echo "Recorder is not running."
        return 0
    fi
    echo "Requesting a clean stop of PID $pid; waiting for file finalization..."
    kill -TERM "$pid"
    for ((attempt=0; attempt<600; ++attempt)); do
        if ! running_pid >/dev/null; then
            rm -f -- "$pid_file"
            echo "Recorder stopped. Log: $log_file"
            tail -n 6 "$log_file" || true
            return 0
        fi
        sleep 0.2
    done
    echo "Still running after 120 seconds. Do not unplug storage; inspect $log_file" >&2
    return 1
}

case "${1:-}" in
    build)   build_app ;;
    start)   start_app ;;
    stop)    stop_app ;;
    restart) stop_app && build_app && start_app ;;
    status)
        if pid="$(running_pid)"; then echo "Recorder running (PID $pid).";
        elif pid="$(unmanaged_pid)"; then echo "Recorder running outside launcher (PID $pid).";
        else echo "Recorder not running."; fi ;;
    logs)    tail -n 40 "$log_file" ;;
    *)
        echo "Usage: bash scripts/recorder.sh {build|start|stop|restart|status|logs}" >&2
        exit 2 ;;
esac
