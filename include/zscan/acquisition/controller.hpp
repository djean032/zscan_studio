#pragma once

#include "zscan/schema/binary.hpp"

#include <cstddef>
#include <string>

namespace zscan::acquisition {

enum class State {
    Idle,
    Setup,
    Preflight,
    OperatorPreparation,
    DarkAcquisition,
    EnergyCalibration,
    SamplePreparation,
    SampleAcquisition,
    Review,
    Published,
    Failed,
    Cancelled,
    EmergencyStopped,
};

class Controller final {
public:
    Controller();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const std::string& acquisition_id() const noexcept { return acquisition_id_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] std::size_t current_position() const noexcept { return current_position_; }
    [[nodiscard]] std::size_t position_count() const noexcept { return position_count_; }
    [[nodiscard]] std::size_t accepted_readings() const noexcept { return accepted_readings_; }
    [[nodiscard]] std::size_t required_readings() const noexcept { return required_readings_; }

    void configure(std::size_t position_count, std::size_t required_readings);
    bool begin_setup();
    bool complete_setup(bool valid);
    bool complete_preflight(bool valid);
    bool confirm_operator_preparation();
    bool complete_dark();
    bool complete_energy(bool valid);
    bool confirm_sample_preparation();
    bool record_accepted_reading();
    bool complete_position();
    bool complete_sample();
    bool publish();

    void fail(std::string reason);
    void cancel();
    void emergency_stop();
    void restart();

private:
    bool transition(State expected, State next, std::string message);
    void new_acquisition();

    State state_ { State::Idle };
    std::string acquisition_id_;
    std::string message_ { "No acquisition configured" };
    std::size_t current_position_ { 0 };
    std::size_t position_count_ { 0 };
    std::size_t accepted_readings_ { 0 };
    std::size_t required_readings_ { 0 };
};

} // namespace zscan::acquisition
