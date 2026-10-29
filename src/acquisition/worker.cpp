#include "zscan/acquisition/worker.hpp"
#include "zscan/acquisition/persistence.hpp"
#include "zscan/acquisition/session.hpp"
#include "zscan/hardware/mock.hpp"

#include <QString>

namespace zscan::acquisition {

Worker::Worker(RunConfiguration configuration,
    hardware::HardwareConfiguration hardware_configuration, std::string cuvette_id, bool mock_mode,
    QObject* parent)
    : QObject(parent)
    , configuration_(std::move(configuration))
    , hardware_configuration_(std::move(hardware_configuration))
    , cuvette_id_(std::move(cuvette_id))
    , mock_mode_(mock_mode)
{
}

void Worker::run()
{
    auto hardware = mock_mode_
        ? std::expected<hardware::HardwareBundle, std::string>(hardware::HardwareBundle {
              std::make_unique<hardware::MockStage>(), std::make_unique<hardware::MockDetector>() })
        : hardware::make_hardware(hardware_configuration_);
    if (!hardware) {
        emit finished(false, QString::fromStdString(hardware.error()));
        return;
    }
    if (mock_mode_) {
        auto& stage = static_cast<hardware::MockStage&>(*hardware->stage);
        auto& detector = static_cast<hardware::MockDetector&>(*hardware->detector);
        for (double position : configuration_.positions_m) {
            stage.responses.push_back(hardware::StagePosition { position, position });
            for (std::uint32_t i = 0; i < configuration_.capture.target_accepted_readings; ++i)
                detector.responses.push_back(hardware::DetectorCapture { 1.0, 0.8 });
        }
    }
    configuration_.progress_callback = [this](const RunProgress& progress) {
        emit this->progress(progress.position_index, progress.position_count,
            progress.accepted_readings, progress.required_readings, progress.rejected_attempts);
    };
    {
        std::lock_guard lock(runner_mutex_);
        runner_ = std::make_unique<Runner>(configuration_, *hardware->stage, *hardware->detector);
        if (cancel_requested_.load())
            runner_->request_cancel();
        if (emergency_requested_.load())
            runner_->request_emergency_stop();
    }
    outcome_ = runner_->run();
    if (outcome_.error.empty() && !cuvette_id_.empty()) {
        const auto persisted = publish_scan(configuration_.session_root, outcome_, cuvette_id_);
        if (!persisted) {
            outcome_.error = persisted.error().message;
            outcome_.scan.end.result = schema::binary::ScanResult::Incomplete;
        }
    }
    emit finished(outcome_.scan.end.result == schema::binary::ScanResult::Complete,
        QString::fromStdString(outcome_.error));
}

void Worker::cancel()
{
    cancel_requested_.store(true);
    std::lock_guard lock(runner_mutex_);
    if (runner_)
        runner_->request_cancel();
}

void Worker::emergency_stop()
{
    emergency_requested_.store(true);
    std::lock_guard lock(runner_mutex_);
    if (runner_)
        runner_->request_emergency_stop();
}

} // namespace zscan::acquisition
