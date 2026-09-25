#include <dv-processing/io/camera/dvxplorer.hpp>
#include <dv-processing/io/mono_camera_writer.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QMutexLocker>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QStringList>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace dvxrec {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Settings {
    QString outputDirectory = QDir::homePath() + "/DVXplorerRecordings";
    int contrastOn = 9;
    int contrastOff = 9;
    int previewIntervalMs = 50;
    bool performanceMonitoring = false;
    bool temperatureMonitoring = false;
    bool storageMonitoring = false;
};

Settings loadSavedSettings() {
    QSettings saved("BirdHKE", "DVXplorerRecorder");
    Settings settings;
    settings.outputDirectory = saved.value("outputDirectory", settings.outputDirectory).toString();
    if (settings.outputDirectory.trimmed().isEmpty()) {
        settings.outputDirectory = QDir::homePath() + "/DVXplorerRecordings";
    }
    settings.contrastOn = std::clamp(saved.value("contrastOn", 9).toInt(), 0, 17);
    settings.contrastOff = std::clamp(saved.value("contrastOff", 9).toInt(), 0, 17);
    settings.previewIntervalMs = std::clamp(saved.value("previewIntervalMs", 50).toInt(), 33, 250);
    settings.performanceMonitoring = saved.value("performanceMonitoring", false).toBool();
    settings.temperatureMonitoring = saved.value("temperatureMonitoring", false).toBool();
    settings.storageMonitoring = saved.value("storageMonitoring", false).toBool();
    return settings;
}

void saveSettings(const Settings &settings) {
    QSettings saved("BirdHKE", "DVXplorerRecorder");
    saved.setValue("outputDirectory", settings.outputDirectory);
    saved.setValue("contrastOn", settings.contrastOn);
    saved.setValue("contrastOff", settings.contrastOff);
    saved.setValue("previewIntervalMs", settings.previewIntervalMs);
    saved.setValue("performanceMonitoring", settings.performanceMonitoring);
    saved.setValue("temperatureMonitoring", settings.temperatureMonitoring);
    saved.setValue("storageMonitoring", settings.storageMonitoring);
}

fs::path nativePath(const QString &path) {
    return fs::path(path.toUtf8().constData());
}

QString utcNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

std::optional<double> readNumber(const char *path, double divisor = 1.0) {
    std::ifstream file(path);
    double value = 0;
    if (file >> value) {
        return value / divisor;
    }
    return std::nullopt;
}

std::optional<double> residentMemoryMiB() {
    std::ifstream file("/proc/self/statm");
    unsigned long long pages = 0, resident = 0;
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize > 0 && (file >> pages >> resident)) {
        return static_cast<double>(resident) * pageSize / (1024.0 * 1024.0);
    }
    return std::nullopt;
}

std::optional<unsigned long long> writtenBytes() {
    std::ifstream file("/proc/self/io");
    std::string key;
    unsigned long long value = 0;
    while (file >> key >> value) {
        if (key == "write_bytes:") {
            return value;
        }
    }
    return std::nullopt;
}

struct CpuTicks {
    unsigned long long busy = 0;
    unsigned long long total = 0;
};

std::optional<CpuTicks> systemCpuTicks() {
    std::ifstream file("/proc/stat");
    std::string label;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    unsigned long long irq = 0, softirq = 0, steal = 0;
    if (file >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal
        && label == "cpu") {
        const auto busy = user + nice + system + irq + softirq + steal;
        return CpuTicks{busy, busy + idle + iowait};
    }
    return std::nullopt;
}

std::string csvNumber(const std::optional<double> value) {
    if (!value || !std::isfinite(*value)) {
        return {};
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << *value;
    return text.str();
}

std::optional<std::pair<double, double>> dirtyAndWritebackMiB() {
    std::ifstream file("/proc/meminfo");
    std::string name, units;
    unsigned long long amount = 0;
    std::optional<double> dirty, writeback;
    while (file >> name >> amount >> units) {
        if (name == "Dirty:") dirty = amount / 1024.0;
        if (name == "Writeback:") writeback = amount / 1024.0;
        if (dirty && writeback) return std::pair{*dirty, *writeback};
    }
    return std::nullopt;
}

std::optional<double> ioPressureSomeAvg10() {
    std::ifstream file("/proc/pressure/io");
    std::string line;
    if (!std::getline(file, line) || !line.starts_with("some ")) return std::nullopt;
    const auto pos = line.find("avg10=");
    if (pos == std::string::npos) return std::nullopt;
    try {
        return std::stod(line.substr(pos + 6));
    }
    catch (...) {
        return std::nullopt;
    }
}

fs::path blockDeviceSysfsPath(const fs::path &directory) {
    struct stat details{};
    if (::stat(directory.c_str(), &details) != 0) return {};
    return fs::path("/sys/dev/block") /
        (std::to_string(major(details.st_dev)) + ":" + std::to_string(minor(details.st_dev)));
}

std::string storageDevice(const fs::path &directory) {
    const auto sysDevice = blockDeviceSysfsPath(directory);
    if (sysDevice.empty()) return "unknown";
    std::error_code error;
    const auto resolved = fs::canonical(sysDevice, error);
    return error ? "unknown" : resolved.filename().string();
}

struct BlockStats {
    unsigned long long sectorsWritten = 0;
    unsigned long long ioMillis = 0;
};

std::optional<BlockStats> readBlockStats(const fs::path &path) {
    if (path.empty()) return std::nullopt;
    std::ifstream file(path);
    unsigned long long fields[11]{};
    for (auto &field : fields) {
        if (!(file >> field)) return std::nullopt;
    }
    return BlockStats{fields[6], fields[9]}; // 512-byte sectors written; milliseconds active.
}

qint64 completedFileSize(const fs::path &file) {
    std::error_code error;
    const auto bytes = fs::file_size(file, error);
    return error ? -1 : static_cast<qint64>(bytes);
}

// The worker owns the USB capture and AEDAT4 writer. No camera/writer method runs
// on the GUI thread. A single latest-frame slot prevents preview backpressure.
class Recorder final : public QObject {
    Q_OBJECT

public:
    explicit Recorder(Settings initial) : initial_(std::move(initial)) {}

    void launch() {
        thread_ = std::thread([this] { run(); });
    }

    ~Recorder() override {
        {
            std::lock_guard lock(commandsMutex_);
            quit_ = true;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void apply(Settings settings) {
        std::lock_guard lock(commandsMutex_);
        apply_ = std::move(settings);
    }

    void start(Settings settings) {
        std::lock_guard lock(commandsMutex_);
        start_ = std::move(settings);
    }

    void stop() {
        std::lock_guard lock(commandsMutex_);
        stop_ = true;
    }

    bool takePreview(QImage &image) {
        QMutexLocker lock(&previewMutex_);
        if (latestPreview_.isNull()) {
            return false;
        }
        image = std::move(latestPreview_);
        latestPreview_ = {};
        return true;
    }

signals:
    void cameraStatus(const QString &message, bool ready);
    void recordingState(bool recording, const QString &message);
    void settingsApplied(bool success, const QString &message);
    void statistics(quint64 events, quint64 triggers);
    void monitoring(const QString &message);

private:
    struct Commands {
        std::optional<Settings> apply;
        std::optional<Settings> start;
        bool stop = false;
        bool quit = false;
    };

    Commands popCommands() {
        std::lock_guard lock(commandsMutex_);
        Commands commands{std::move(apply_), std::move(start_), stop_, quit_};
        apply_.reset();
        start_.reset();
        stop_ = false;
        return commands;
    }

    static void configure(dv::io::camera::DVXplorer &camera, const Settings &settings) {
        camera.setContrastThresholdOn(settings.contrastOn);
        camera.setContrastThresholdOff(settings.contrastOff);
        // Both edges are retained for later alignment with the RGB shutter/trigger.
        camera.setDetectorRisingEdges(true);
        camera.setDetectorFallingEdges(true);
        camera.setDetectorRunning(true);
    }

    struct Session {
        fs::path file;
        Settings settings;
        QString cameraName;
        QString startUtc;
        quint64 events = 0;
        quint64 triggers = 0;
        fs::path monitorFile;
        QString monitorError;
        std::string storageDeviceName;
        std::optional<double> finalizeMs;
    };

    static void saveMetadata(const Session &session, const QString &state, const QString &error = {}) {
        QJsonObject json{
            {"format", "AEDAT4"},
            {"camera", session.cameraName},
            {"recording_state", state},
            {"start_utc", session.startUtc},
            {"end_utc", state == "recording" ? QString{} : utcNow()},
            {"event_count", static_cast<qint64>(session.events)},
            {"trigger_count", static_cast<qint64>(session.triggers)},
            {"contrast_on", session.settings.contrastOn},
            {"contrast_off", session.settings.contrastOff},
            {"preview_interval_ms", session.settings.previewIntervalMs},
            {"performance_monitoring", session.settings.performanceMonitoring},
            {"temperature_monitoring", session.settings.temperatureMonitoring},
            {"storage_monitoring", session.settings.storageMonitoring},
            {"storage_device", QString::fromStdString(session.storageDeviceName)},
            {"aedat_bytes", completedFileSize(session.file)},
            {"writer_finalize_ms", session.finalizeMs
                ? QJsonValue(*session.finalizeMs) : QJsonValue()},
            {"monitor_csv", session.monitorFile.empty()
                ? QString{} : QString::fromStdString(session.monitorFile.filename().string())},
            {"monitor_error", session.monitorError},
            {"error", error}
        };
        const QString sidecar = QString::fromStdString(session.file.string()) + ".json";
        QSaveFile file(sidecar);
        if (!file.open(QIODevice::WriteOnly)) {
            throw std::runtime_error("Cannot open recording metadata file");
        }
        const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
        if (file.write(bytes) != bytes.size() || !file.commit()) {
            throw std::runtime_error("Cannot save recording metadata file");
        }
    }

    void run() {
        Settings current = initial_;
        while (true) {
            if (popCommands().quit) {
                return;
            }

            std::optional<dv::io::MonoCameraWriter> writer;
            std::optional<Session> session;
            std::optional<std::ofstream> monitorLog;
            try {
                dv::io::camera::DVXplorer camera{};
                configure(camera, current);
                const auto resolution = camera.getEventResolution();
                if (!resolution || resolution->width <= 0 || resolution->height <= 0) {
                    throw std::runtime_error("DVXplorer did not report an event resolution");
                }
                emit cameraStatus(QString("Ready: %1 (%2 x %3)")
                    .arg(QString::fromStdString(camera.getCameraName()))
                    .arg(resolution->width).arg(resolution->height), true);

                QImage preview(resolution->width, resolution->height, QImage::Format_RGB888);
                preview.fill(Qt::black);
                auto lastPreview = std::chrono::steady_clock::now();
                auto lastStatistics = lastPreview;
                auto lastDiskCheck = lastPreview;
                auto lastMonitoring = lastPreview;
                auto sessionStart = lastPreview;
                auto lastLoopStart = lastPreview;
                auto previousCpuClock = std::clock();
                auto previousSystemTicks = current.performanceMonitoring
                    ? systemCpuTicks() : std::optional<CpuTicks>{};
                auto previousWrittenBytes = current.storageMonitoring
                    ? writtenBytes() : std::optional<unsigned long long>{};
                quint64 previousEvents = 0, previousTriggers = 0;
                double maxPollGapMs = 0;
                double maxWriterCallMs = 0;
                double writerTimeMs = 0;
                std::optional<double> lastDiskFreeMiB;
                std::optional<std::uintmax_t> previousFileSize;
                fs::path blockStatsFile;
                std::optional<BlockStats> previousBlockStats;

                auto sampleMonitor = [&](const std::chrono::steady_clock::time_point now,
                                         const bool finalSample = false) {
                    if (!current.performanceMonitoring && !current.temperatureMonitoring
                        && !current.storageMonitoring) {
                        return;
                    }
                    const double elapsed = std::chrono::duration<double>(now - lastMonitoring).count();
                    if (elapsed <= 0 || (!finalSample && elapsed < 1.0)) {
                        return;
                    }
                    std::optional<double> processCpu, systemCpu, rssMiB, writeMiBs, frequencyMHz;
                    std::optional<double> fileMiBs, dirtyMiB, writebackMiB, ioPressure;
                    std::optional<double> deviceWriteMiBs, deviceBusyPercent;
                    std::optional<double> temperatureC;
                    if (current.performanceMonitoring) {
                        const auto cpuNow = std::clock();
                        if (cpuNow != static_cast<std::clock_t>(-1)
                            && previousCpuClock != static_cast<std::clock_t>(-1)) {
                            processCpu = 100.0 * (cpuNow - previousCpuClock) / CLOCKS_PER_SEC / elapsed;
                        }
                        previousCpuClock = cpuNow;
                        const auto systemNow = systemCpuTicks();
                        if (systemNow && previousSystemTicks && systemNow->total > previousSystemTicks->total
                            && systemNow->busy >= previousSystemTicks->busy) {
                            systemCpu = 100.0 * (systemNow->busy - previousSystemTicks->busy)
                                / (systemNow->total - previousSystemTicks->total);
                        }
                        previousSystemTicks = systemNow;
                        rssMiB = residentMemoryMiB();
                        frequencyMHz = readNumber(
                            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 1000.0);
                    }
                    if (current.storageMonitoring) {
                        const auto writtenNow = writtenBytes();
                        if (writtenNow && previousWrittenBytes && *writtenNow >= *previousWrittenBytes) {
                            writeMiBs = static_cast<double>(*writtenNow - *previousWrittenBytes)
                                / (1024.0 * 1024.0) / elapsed;
                        }
                        previousWrittenBytes = writtenNow;
                        if (session) {
                            std::error_code error;
                            const auto bytes = fs::file_size(session->file, error);
                            if (!error) {
                                if (previousFileSize && bytes >= *previousFileSize) {
                                    fileMiBs = static_cast<double>(bytes - *previousFileSize)
                                        / (1024.0 * 1024.0) / elapsed;
                                }
                                previousFileSize = bytes;
                            }
                        }
                        if (const auto backlog = dirtyAndWritebackMiB()) {
                            dirtyMiB = backlog->first;
                            writebackMiB = backlog->second;
                        }
                        ioPressure = ioPressureSomeAvg10();
                        const auto blockNow = readBlockStats(blockStatsFile);
                        if (blockNow && previousBlockStats
                            && blockNow->sectorsWritten >= previousBlockStats->sectorsWritten
                            && blockNow->ioMillis >= previousBlockStats->ioMillis) {
                            deviceWriteMiBs = static_cast<double>(
                                blockNow->sectorsWritten - previousBlockStats->sectorsWritten)
                                * 512.0 / (1024.0 * 1024.0) / elapsed;
                            deviceBusyPercent = (blockNow->ioMillis - previousBlockStats->ioMillis)
                                / (elapsed * 10.0);
                        }
                        previousBlockStats = blockNow;
                    }
                    if (current.temperatureMonitoring) {
                        temperatureC = readNumber("/sys/class/thermal/thermal_zone0/temp", 1000.0);
                    }
                    const double eventRate = session ? (session->events - previousEvents) / elapsed : 0;
                    const double triggerRate = session ? (session->triggers - previousTriggers) / elapsed : 0;
                    QStringList summary;
                    if (current.performanceMonitoring) {
                        summary << QString("Events %1/s | CPU %2% | Loop max %3 ms | RSS %4 MiB")
                            .arg(QString::number(eventRate, 'f', 0))
                            .arg(processCpu ? QString::number(*processCpu, 'f', 0) : "n/a")
                            .arg(QString::number(maxPollGapMs, 'f', 1))
                            .arg(rssMiB ? QString::number(*rssMiB, 'f', 0) : "n/a");
                    }
                    if (current.temperatureMonitoring) {
                        summary << (temperatureC
                            ? QString("Temperature %1 C").arg(QString::number(*temperatureC, 'f', 1))
                            : "Temperature unavailable");
                    }
                    if (current.storageMonitoring) {
                        summary << QString("Storage file %1 MiB/s | device %2 MiB/s | busy %3% | writer max %4 ms")
                            .arg(fileMiBs ? QString::number(*fileMiBs, 'f', 1) : "n/a")
                            .arg(deviceWriteMiBs ? QString::number(*deviceWriteMiBs, 'f', 1) : "n/a")
                            .arg(deviceBusyPercent ? QString::number(*deviceBusyPercent, 'f', 0) : "n/a")
                            .arg(QString::number(maxWriterCallMs, 'f', 1));
                    }
                    if (session && !session->monitorError.isEmpty()) {
                        summary << session->monitorError;
                    }
                    emit monitoring(summary.join(" | "));
                    if (session && monitorLog) {
                        *monitorLog << utcNow().toStdString() << ','
                            << std::chrono::duration<double>(now - sessionStart).count() << ','
                            << session->events << ',' << session->triggers << ','
                            << (current.performanceMonitoring ? csvNumber(eventRate) : "") << ','
                            << (current.performanceMonitoring ? csvNumber(triggerRate) : "") << ','
                            << csvNumber(processCpu) << ',' << csvNumber(systemCpu) << ','
                            << csvNumber(rssMiB) << ',' << csvNumber(frequencyMHz) << ','
                            << (current.performanceMonitoring ? csvNumber(maxPollGapMs) : "") << ','
                            << csvNumber(temperatureC) << ','
                            << (current.storageMonitoring ? csvNumber(lastDiskFreeMiB) : "") << ','
                            << csvNumber(fileMiBs) << ',' << csvNumber(writeMiBs) << ','
                            << (current.storageMonitoring ? csvNumber(maxWriterCallMs) : "") << ','
                            << (current.storageMonitoring ? csvNumber(writerTimeMs) : "") << ','
                            << csvNumber(dirtyMiB) << ',' << csvNumber(writebackMiB) << ','
                            << csvNumber(ioPressure) << ',' << csvNumber(deviceWriteMiBs) << ','
                            << csvNumber(deviceBusyPercent) << ','
                            << (session->finalizeMs ? csvNumber(*session->finalizeMs) : "") << '\n'
                            << std::flush;
                        if (!monitorLog->good()) {
                            session->monitorError = "Monitoring CSV write failed";
                            monitorLog.reset();
                            emit monitoring("Monitoring log failed; event recording continues");
                        }
                    }
                    previousEvents = session ? session->events : 0;
                    previousTriggers = session ? session->triggers : 0;
                    maxPollGapMs = 0;
                    maxWriterCallMs = 0;
                    writerTimeMs = 0;
                    lastMonitoring = now;
                };

                auto finish = [&](const QString &state, const QString &reason = QString{}) {
                    if (!writer) {
                        return;
                    }
                    const auto beforeFinalize = std::chrono::steady_clock::now();
                    writer.reset(); // AEDAT4 index and buffered packets are finalized here.
                    session->finalizeMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - beforeFinalize).count();
                    sampleMonitor(std::chrono::steady_clock::now(), true);
                    monitorLog.reset();
                    try {
                        saveMetadata(*session, state, reason);
                    }
                    catch (const std::exception &e) {
                        emit cameraStatus(QString("Metadata error: %1").arg(e.what()), true);
                    }
                    const QString path = QString::fromStdString(session->file.string());
                    emit recordingState(false, state == "complete"
                        ? QString("Saved %1").arg(path)
                        : QString("Recording interrupted: %1 (%2)").arg(reason, path));
                    session.reset();
                };

                while (camera.isRunning()) {
                    const auto loopStart = std::chrono::steady_clock::now();
                    if (session && current.performanceMonitoring) {
                        maxPollGapMs = std::max(maxPollGapMs,
                            std::chrono::duration<double, std::milli>(loopStart - lastLoopStart).count());
                    }
                    lastLoopStart = loopStart;
                    const Commands commands = popCommands();
                    if (commands.quit) {
                        finish("complete");
                        return;
                    }
                    if (commands.stop) {
                        finish("complete");
                    }
                    if (commands.apply && !writer) {
                        try {
                            configure(camera, *commands.apply);
                            current = *commands.apply;
                            lastMonitoring = std::chrono::steady_clock::now();
                            previousCpuClock = std::clock();
                            previousSystemTicks = current.performanceMonitoring
                                ? systemCpuTicks() : std::optional<CpuTicks>{};
                            previousWrittenBytes = current.storageMonitoring
                                ? writtenBytes() : std::optional<unsigned long long>{};
                            blockStatsFile.clear();
                            previousBlockStats.reset();
                            emit monitoring(current.performanceMonitoring || current.temperatureMonitoring
                                || current.storageMonitoring ? "Monitoring enabled" : "Monitoring off");
                            emit settingsApplied(true, "Camera settings applied");
                        }
                        catch (const std::exception &e) {
                            emit settingsApplied(false, QString("Settings failed: %1").arg(e.what()));
                        }
                    }
                    if (commands.start && !writer) {
                        try {
                            configure(camera, *commands.start);
                            current = *commands.start;
                            const fs::path directory = nativePath(current.outputDirectory);
                            fs::create_directories(directory);
                            const auto availableBytes = fs::space(directory).available;
                            if (availableBytes < 256ULL * 1024 * 1024) {
                                throw std::runtime_error("Less than 256 MiB free in output directory");
                            }
                            lastDiskFreeMiB = availableBytes / (1024.0 * 1024.0);
                            fs::path output;
                            const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzzZ");
                            for (unsigned index = 0;; ++index) {
                                const QString name = "DVXplorer_" + stamp
                                    + (index ? "_" + QString::number(index) : QString{}) + ".aedat4";
                                output = directory / nativePath(name);
                                if (!fs::exists(output) && !fs::exists(output.string() + ".json")
                                    && !fs::exists(output.string() + ".monitor.csv")) {
                                    break;
                                }
                            }
                            writer.emplace(output.string(), camera);
                            session = Session{output, current, QString::fromStdString(camera.getCameraName()), utcNow()};
                            if (current.performanceMonitoring || current.temperatureMonitoring
                                || current.storageMonitoring) {
                                session->monitorFile = fs::path(output.string() + ".monitor.csv");
                                session->storageDeviceName = storageDevice(directory);
                                if (current.storageMonitoring) {
                                    const auto device = blockDeviceSysfsPath(directory);
                                    blockStatsFile = device.empty() ? fs::path{} : device / "stat";
                                }
                                monitorLog.emplace(session->monitorFile, std::ios::out | std::ios::trunc);
                                if (!monitorLog->is_open()) {
                                    session->monitorError = "Cannot create monitoring CSV";
                                    session->monitorFile.clear();
                                    monitorLog.reset();
                                    emit monitoring("Monitoring log unavailable; event recording continues");
                                }
                                else {
                                    *monitorLog << "utc,elapsed_s,events_total,triggers_total,event_rate_eps,"
                                        "trigger_rate_hz,process_cpu_pct,system_cpu_pct,rss_mib,cpu_freq_mhz,"
                                        "max_poll_gap_ms,temperature_c,free_disk_mib,file_growth_mib_s,"
                                        "process_write_mib_s,max_writer_call_ms,writer_time_ms,dirty_mib,"
                                        "writeback_mib,io_psi_some_avg10,device_write_mib_s,"
                                        "device_io_busy_pct,writer_finalize_ms\n" << std::flush;
                                    if (!monitorLog->good()) {
                                        session->monitorError = "Cannot write monitoring CSV header";
                                        monitorLog.reset();
                                        emit monitoring("Monitoring log unavailable; event recording continues");
                                    }
                                }
                            }
                            sessionStart = std::chrono::steady_clock::now();
                            lastMonitoring = sessionStart;
                            lastLoopStart = sessionStart;
                            previousCpuClock = std::clock();
                            previousSystemTicks = current.performanceMonitoring
                                ? systemCpuTicks() : std::optional<CpuTicks>{};
                            previousWrittenBytes = current.storageMonitoring
                                ? writtenBytes() : std::optional<unsigned long long>{};
                            previousFileSize.reset();
                            previousBlockStats = current.storageMonitoring
                                ? readBlockStats(blockStatsFile) : std::optional<BlockStats>{};
                            previousEvents = previousTriggers = 0;
                            maxPollGapMs = maxWriterCallMs = writerTimeMs = 0;
                            saveMetadata(*session, "recording");
                            emit statistics(0, 0);
                            emit recordingState(true, QString("Recording %1").arg(QString::fromStdString(output.filename().string())));
                        }
                        catch (const std::exception &e) {
                            monitorLog.reset();
                            writer.reset();
                            session.reset();
                            emit recordingState(false, QString("Cannot start: %1").arg(e.what()));
                        }
                    }

                    if (auto events = camera.getNextEventBatch(); events && !events->isEmpty()) {
                        if (writer) {
                            const auto beforeWrite = current.storageMonitoring
                                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                            writer->writeEvents(*events); // Native timestamps and all received events.
                            session->events += events->size();
                            if (current.storageMonitoring) {
                                const double millis = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - beforeWrite).count();
                                maxWriterCallMs = std::max(maxWriterCallMs, millis);
                                writerTimeMs += millis;
                            }
                        }
                        // Only the display is sampled; the recorded stream is never sampled.
                        const size_t stride = std::max<size_t>(1, events->size() / 12000);
                        size_t position = 0;
                        for (const auto &event : *events) {
                            if (position++ % stride != 0) {
                                continue;
                            }
                            if (event.x() >= 0 && event.y() >= 0 && event.x() < resolution->width
                                && event.y() < resolution->height) {
                                auto *pixel = preview.scanLine(event.y()) + event.x() * 3;
                                if (event.polarity()) {
                                    pixel[0] = 30; pixel[1] = 205; pixel[2] = 255;
                                }
                                else {
                                    pixel[0] = 255; pixel[1] = 100; pixel[2] = 160;
                                }
                            }
                        }
                    }
                    if (auto triggers = camera.getNextTriggerBatch(); triggers && !triggers->empty()) {
                        if (writer) {
                            const auto beforeWrite = current.storageMonitoring
                                ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                            for (const auto &trigger : *triggers) {
                                writer->writeTrigger(trigger);
                            }
                            session->triggers += triggers->size();
                            if (current.storageMonitoring) {
                                const double millis = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - beforeWrite).count();
                                maxWriterCallMs = std::max(maxWriterCallMs, millis);
                                writerTimeMs += millis;
                            }
                        }
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastPreview >= std::chrono::milliseconds(current.previewIntervalMs)) {
                        {
                            QMutexLocker lock(&previewMutex_);
                            latestPreview_ = std::move(preview);
                        }
                        preview = QImage(resolution->width, resolution->height, QImage::Format_RGB888);
                        preview.fill(Qt::black);
                        lastPreview = now;
                    }
                    if (writer && now - lastStatistics >= 500ms) {
                        emit statistics(session->events, session->triggers);
                        lastStatistics = now;
                    }
                    if (writer && now - lastDiskCheck >= 2s) {
                        const auto availableBytes = fs::space(session->file.parent_path()).available;
                        lastDiskFreeMiB = availableBytes / (1024.0 * 1024.0);
                        if (availableBytes < 128ULL * 1024 * 1024) {
                            throw std::runtime_error("Low disk space (under 128 MiB)");
                        }
                        lastDiskCheck = now;
                    }
                    sampleMonitor(now);
                    if (now - lastPreview < 2ms) {
                        std::this_thread::sleep_for(1ms);
                    }
                }
                finish("interrupted", "Camera disconnected");
                emit cameraStatus("Camera disconnected; retrying", false);
            }
            catch (const std::exception &e) {
                const QString error = QString::fromUtf8(e.what());
                if (writer && session) {
                    const auto beforeFinalize = std::chrono::steady_clock::now();
                    writer.reset();
                    session->finalizeMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - beforeFinalize).count();
                    monitorLog.reset();
                    try {
                        saveMetadata(*session, "interrupted", error);
                    }
                    catch (...) {
                        // Keep the original capture/write error visible.
                    }
                    emit recordingState(false, "Recording interrupted: " + error);
                }
                emit cameraStatus("Camera error: " + error + "; retrying", false);
            }
            for (int i = 0; i < 20; ++i) {
                std::this_thread::sleep_for(100ms);
                std::lock_guard lock(commandsMutex_);
                if (quit_) {
                    return;
                }
            }
        }
    }

    std::mutex commandsMutex_;
    const Settings initial_;
    std::optional<Settings> apply_;
    std::optional<Settings> start_;
    bool stop_ = false;
    bool quit_ = false;
    QMutex previewMutex_;
    QImage latestPreview_;
    std::thread thread_;
};

class MainWindow final : public QWidget {
public:
    MainWindow(Recorder &recorder, Settings initial) : recorder_(recorder), settings_(std::move(initial)) {
        setWindowTitle("DVXplorer Recorder");
        resize(850, 600);

        auto *root = new QVBoxLayout(this);
        pages_ = new QStackedWidget(this);
        root->addWidget(pages_);

        auto *live = new QWidget(this);
        auto *liveLayout = new QVBoxLayout(live);
        preview_ = new QLabel("Waiting for DVXplorer...", live);
        preview_->setAlignment(Qt::AlignCenter);
        preview_->setMinimumSize(320, 240);
        preview_->setStyleSheet("background:#101317;color:white;font-size:20px");
        liveLayout->addWidget(preview_, 1);
        status_ = new QLabel("Connecting to DVXplorer...", live);
        status_->setWordWrap(true);
        liveLayout->addWidget(status_);
        counts_ = new QLabel("Events: 0 | Triggers: 0", live);
        liveLayout->addWidget(counts_);
        monitorStatus_ = new QLabel("Monitoring off", live);
        monitorStatus_->setWordWrap(true);
        liveLayout->addWidget(monitorStatus_);
        auto *buttons = new QHBoxLayout;
        record_ = new QPushButton("Start recording", live);
        settingsButton_ = new QPushButton("Settings", live);
        record_->setMinimumHeight(52);
        settingsButton_->setMinimumHeight(52);
        buttons->addWidget(record_);
        buttons->addWidget(settingsButton_);
        liveLayout->addLayout(buttons);
        pages_->addWidget(live);

        auto *settingsPage = new QWidget(this);
        auto *settingsLayout = new QVBoxLayout(settingsPage);
        auto *scroll = new QScrollArea(settingsPage);
        scroll->setWidgetResizable(true);
        auto *formContainer = new QWidget(scroll);
        auto *form = new QFormLayout(formContainer);
        output_ = new QLineEdit(settings_.outputDirectory, formContainer);
        auto *browse = new QPushButton("Browse", formContainer);
        auto *outputRow = new QWidget(formContainer);
        auto *outputLayout = new QHBoxLayout(outputRow);
        outputLayout->setContentsMargins(0, 0, 0, 0);
        outputLayout->addWidget(output_);
        outputLayout->addWidget(browse);
        form->addRow("Recording directory", outputRow);
        on_ = spin(0, 17, 9, formContainer);
        off_ = spin(0, 17, 9, formContainer);
        interval_ = spin(33, 250, 50, formContainer);
        performance_ = new QCheckBox("Performance monitoring (CPU, memory, event rate, loop delays)", formContainer);
        temperature_ = new QCheckBox("Temperature monitoring", formContainer);
        storage_ = new QCheckBox("Storage monitoring (write rate, stalls, disk backlog)", formContainer);
        loadSettings(settings_);
        form->addRow("ON contrast (0-17)", on_);
        form->addRow("OFF contrast (0-17)", off_);
        form->addRow("Preview interval (ms)", interval_);
        form->addRow(performance_);
        form->addRow(temperature_);
        form->addRow(storage_);
        auto *note = new QLabel("ON/OFF contrast changes camera sensitivity. The preview interval affects only the screen. "
            "Monitoring samples once per second and saves a CSV beside the AEDAT4. "
            "Settings are locked during recording.", formContainer);
        note->setWordWrap(true);
        form->addRow(note);
        scroll->setWidget(formContainer);
        settingsLayout->addWidget(scroll);
        auto *settingsButtons = new QHBoxLayout;
        apply_ = new QPushButton("Apply settings", settingsPage);
        auto *cancel = new QPushButton("Cancel", settingsPage);
        apply_->setMinimumHeight(52);
        cancel->setMinimumHeight(52);
        settingsButtons->addWidget(apply_);
        settingsButtons->addWidget(cancel);
        settingsLayout->addLayout(settingsButtons);
        pages_->addWidget(settingsPage);

        connect(browse, &QPushButton::clicked, this, [this] {
            const QString folder = QFileDialog::getExistingDirectory(this, "Recording directory", output_->text());
            if (!folder.isEmpty()) output_->setText(folder);
        });
        connect(settingsButton_, &QPushButton::clicked, this, [this] {
            if (!recording_ && !busy_) pages_->setCurrentIndex(1);
        });
        connect(cancel, &QPushButton::clicked, this, [this] {
            loadSettings(settings_);
            pages_->setCurrentIndex(0);
        });
        connect(apply_, &QPushButton::clicked, this, [this] {
            Settings draft = readSettings();
            if (draft.outputDirectory.trimmed().isEmpty()) {
                status_->setText("Choose a recording directory");
                pages_->setCurrentIndex(0);
                return;
            }
            pendingSettings_ = std::move(draft);
            busy_ = true;
            updateButtons();
            pages_->setCurrentIndex(0);
            status_->setText("Applying settings...");
            recorder_.apply(*pendingSettings_);
        });
        connect(record_, &QPushButton::clicked, this, [this] {
            if (recording_) {
                record_->setEnabled(false);
                status_->setText("Finalizing AEDAT4...");
                recorder_.stop();
            }
            else if (ready_ && !busy_) {
                busy_ = true;
                updateButtons();
                status_->setText("Starting recording...");
                recorder_.start(settings_);
            }
        });

        connect(&recorder_, &Recorder::cameraStatus, this, [this](const QString &message, bool ready) {
            ready_ = ready;
            if (!ready) { busy_ = false; recording_ = false; }
            status_->setText(message);
            updateButtons();
        });
        connect(&recorder_, &Recorder::recordingState, this, [this](bool recording, const QString &message) {
            recording_ = recording;
            busy_ = false;
            status_->setText(message);
            updateButtons();
        });
        connect(&recorder_, &Recorder::settingsApplied, this, [this](bool success, const QString &message) {
            busy_ = false;
            if (success && pendingSettings_) {
                settings_ = *pendingSettings_;
                saveSettings(settings_);
            }
            pendingSettings_.reset();
            status_->setText(message);
            updateButtons();
        });
        connect(&recorder_, &Recorder::statistics, this, [this](quint64 events, quint64 triggers) {
            counts_->setText(QString("Events: %1 | Triggers: %2").arg(events).arg(triggers));
        });
        connect(&recorder_, &Recorder::monitoring, this, [this](const QString &message) {
            monitorStatus_->setText(message);
        });
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            QImage image;
            if (recorder_.takePreview(image)) {
                lastImage_ = QPixmap::fromImage(std::move(image));
                preview_->setPixmap(lastImage_.scaled(preview_->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
            }
        });
        timer->start(50);
        updateButtons();
    }

private:
    static QSpinBox *spin(int minimum, int maximum, int value, QWidget *parent) {
        auto *box = new QSpinBox(parent);
        box->setRange(minimum, maximum);
        box->setValue(value);
        box->setMinimumHeight(40);
        return box;
    }

    Settings readSettings() const {
        return {output_->text().trimmed(), on_->value(), off_->value(), interval_->value(),
            performance_->isChecked(), temperature_->isChecked(), storage_->isChecked()};
    }

    void loadSettings(const Settings &settings) {
        output_->setText(settings.outputDirectory);
        on_->setValue(settings.contrastOn);
        off_->setValue(settings.contrastOff);
        interval_->setValue(settings.previewIntervalMs);
        performance_->setChecked(settings.performanceMonitoring);
        temperature_->setChecked(settings.temperatureMonitoring);
        storage_->setChecked(settings.storageMonitoring);
    }

    void updateButtons() {
        record_->setText(recording_ ? "Stop recording" : "Start recording");
        record_->setEnabled(recording_ || (ready_ && !busy_));
        settingsButton_->setEnabled(ready_ && !recording_ && !busy_);
    }

    Recorder &recorder_;
    Settings settings_;
    std::optional<Settings> pendingSettings_;
    QStackedWidget *pages_ = nullptr;
    QLabel *preview_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *counts_ = nullptr;
    QLabel *monitorStatus_ = nullptr;
    QPushButton *record_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    QPushButton *apply_ = nullptr;
    QLineEdit *output_ = nullptr;
    QSpinBox *on_ = nullptr;
    QSpinBox *off_ = nullptr;
    QSpinBox *interval_ = nullptr;
    QCheckBox *performance_ = nullptr;
    QCheckBox *temperature_ = nullptr;
    QCheckBox *storage_ = nullptr;
    QPixmap lastImage_;
    bool ready_ = false;
    bool recording_ = false;
    bool busy_ = false;
};
} // namespace dvxrec

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    const dvxrec::Settings initial = dvxrec::loadSavedSettings();
    dvxrec::Recorder recorder(initial);
    dvxrec::MainWindow window(recorder, initial);
    window.show();
    recorder.launch();
    return application.exec();
}

#include "main.moc"
