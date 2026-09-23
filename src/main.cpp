#include <dv-processing/io/camera/dvxplorer.hpp>
#include <dv-processing/io/mono_camera_writer.hpp>

#include <QApplication>
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
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace dvxrec {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct Settings {
    QString outputDirectory = QDir::homePath() + "/DVXplorerRecordings";
    int contrastOn = 9;
    int contrastOff = 9;
    int backgroundActivityTicks = 0; // Hardware filter units: 250 microseconds.
    int refractoryTicks = 0;         // Hardware filter units: 250 microseconds.
    int previewIntervalMs = 50;
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
    settings.backgroundActivityTicks = std::clamp(saved.value("backgroundActivityTicks", 0).toInt(), 0, 65535);
    settings.refractoryTicks = std::clamp(saved.value("refractoryTicks", 0).toInt(), 0, 65535);
    settings.previewIntervalMs = std::clamp(saved.value("previewIntervalMs", 50).toInt(), 33, 250);
    return settings;
}

void saveSettings(const Settings &settings) {
    QSettings saved("BirdHKE", "DVXplorerRecorder");
    saved.setValue("outputDirectory", settings.outputDirectory);
    saved.setValue("contrastOn", settings.contrastOn);
    saved.setValue("contrastOff", settings.contrastOff);
    saved.setValue("backgroundActivityTicks", settings.backgroundActivityTicks);
    saved.setValue("refractoryTicks", settings.refractoryTicks);
    saved.setValue("previewIntervalMs", settings.previewIntervalMs);
}

fs::path nativePath(const QString &path) {
    return fs::u8path(path.toUtf8().constData());
}

QString utcNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
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
        camera.setBackgroundActivityFilter(settings.backgroundActivityTicks);
        camera.setRefractoryPeriodFilter(settings.refractoryTicks);
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
            {"background_activity_250us", session.settings.backgroundActivityTicks},
            {"refractory_250us", session.settings.refractoryTicks},
            {"preview_interval_ms", session.settings.previewIntervalMs},
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
        while (true) {
            if (popCommands().quit) {
                return;
            }

            std::optional<dv::io::MonoCameraWriter> writer;
            std::optional<Session> session;
            try {
                dv::io::camera::DVXplorer camera{};
                Settings current = initial_;
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

                auto finish = [&](const QString &state, const QString &reason = QString{}) {
                    if (!writer) {
                        return;
                    }
                    writer.reset(); // AEDAT4 index and buffered packets are finalized here.
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
                            if (fs::space(directory).available < 256ULL * 1024 * 1024) {
                                throw std::runtime_error("Less than 256 MiB free in output directory");
                            }
                            fs::path output;
                            const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzzZ");
                            for (unsigned index = 0;; ++index) {
                                const QString name = "DVXplorer_" + stamp
                                    + (index ? "_" + QString::number(index) : QString{}) + ".aedat4";
                                output = directory / nativePath(name);
                                if (!fs::exists(output) && !fs::exists(output.string() + ".json")) {
                                    break;
                                }
                            }
                            writer.emplace(output.string(), camera);
                            session = Session{output, current, QString::fromStdString(camera.getCameraName()), utcNow()};
                            saveMetadata(*session, "recording");
                            emit statistics(0, 0);
                            emit recordingState(true, QString("Recording %1").arg(QString::fromStdString(output.filename().string())));
                        }
                        catch (const std::exception &e) {
                            writer.reset();
                            session.reset();
                            emit recordingState(false, QString("Cannot start: %1").arg(e.what()));
                        }
                    }

                    if (auto events = camera.getNextEventBatch(); events && !events->empty()) {
                        if (writer) {
                            writer->writeEvents(*events); // Native timestamps and all received events.
                            session->events += events->size();
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
                            for (const auto &trigger : *triggers) {
                                writer->writeTrigger(trigger);
                            }
                            session->triggers += triggers->size();
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
                        if (fs::space(session->file.parent_path()).available < 128ULL * 1024 * 1024) {
                            throw std::runtime_error("Low disk space (under 128 MiB)");
                        }
                        lastDiskCheck = now;
                    }
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
                    writer.reset();
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
        background_ = spin(0, 65535, 0, formContainer);
        refractory_ = spin(0, 65535, 0, formContainer);
        interval_ = spin(33, 250, 50, formContainer);
        loadSettings(settings_);
        form->addRow("ON contrast (0-17)", on_);
        form->addRow("OFF contrast (0-17)", off_);
        form->addRow("Background filter (250 us units; 0 = off)", background_);
        form->addRow("Refractory filter (250 us units; 0 = off)", refractory_);
        form->addRow("Preview interval (ms)", interval_);
        auto *note = new QLabel("Hardware filters remove events before saving. Keep both at 0 for an unfiltered stream. "
            "The preview setting affects only the screen. Settings are locked during recording.", formContainer);
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
            settings_ = std::move(draft);
            busy_ = true;
            updateButtons();
            pages_->setCurrentIndex(0);
            status_->setText("Applying settings...");
            recorder_.apply(settings_);
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
            if (success) saveSettings(settings_);
            status_->setText(message);
            updateButtons();
        });
        connect(&recorder_, &Recorder::statistics, this, [this](quint64 events, quint64 triggers) {
            counts_->setText(QString("Events: %1 | Triggers: %2").arg(events).arg(triggers));
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
        return {output_->text().trimmed(), on_->value(), off_->value(),
            background_->value(), refractory_->value(), interval_->value()};
    }

    void loadSettings(const Settings &settings) {
        output_->setText(settings.outputDirectory);
        on_->setValue(settings.contrastOn);
        off_->setValue(settings.contrastOff);
        background_->setValue(settings.backgroundActivityTicks);
        refractory_->setValue(settings.refractoryTicks);
        interval_->setValue(settings.previewIntervalMs);
    }

    void updateButtons() {
        record_->setText(recording_ ? "Stop recording" : "Start recording");
        record_->setEnabled(recording_ || (ready_ && !busy_));
        settingsButton_->setEnabled(ready_ && !recording_ && !busy_);
    }

    Recorder &recorder_;
    Settings settings_;
    QStackedWidget *pages_ = nullptr;
    QLabel *preview_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *counts_ = nullptr;
    QPushButton *record_ = nullptr;
    QPushButton *settingsButton_ = nullptr;
    QPushButton *apply_ = nullptr;
    QLineEdit *output_ = nullptr;
    QSpinBox *on_ = nullptr;
    QSpinBox *off_ = nullptr;
    QSpinBox *background_ = nullptr;
    QSpinBox *refractory_ = nullptr;
    QSpinBox *interval_ = nullptr;
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
