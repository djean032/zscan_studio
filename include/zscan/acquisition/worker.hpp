#pragma once

#include "zscan/acquisition/runner.hpp"
#include "zscan/hardware/factory.hpp"

#include <QObject>
#include <atomic>
#include <memory>
#include <mutex>

namespace zscan::acquisition {

class Worker final : public QObject {
    Q_OBJECT
public:
    Worker(RunConfiguration configuration, hardware::HardwareConfiguration hardware_configuration,
        std::string cuvette_id = { }, bool mock_mode = false, QObject* parent = nullptr);

    [[nodiscard]] const RunOutcome& outcome() const noexcept { return outcome_; }

public slots:
    void run();
    void cancel();
    void emergency_stop();

signals:
    void progress(std::size_t position, std::size_t positions, std::size_t accepted,
        std::size_t required, std::size_t rejected);
    void finished(bool complete, QString message);

private:
    RunConfiguration configuration_;
    hardware::HardwareConfiguration hardware_configuration_;
    std::string cuvette_id_;
    bool mock_mode_ { false };
    std::unique_ptr<Runner> runner_;
    std::mutex runner_mutex_;
    std::atomic_bool cancel_requested_ { false };
    std::atomic_bool emergency_requested_ { false };
    RunOutcome outcome_;
};

} // namespace zscan::acquisition
