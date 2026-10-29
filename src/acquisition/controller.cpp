#include "zscan/acquisition/controller.hpp"

#include <utility>

namespace zscan::acquisition {

Controller::Controller()
{
    new_acquisition();
}

void Controller::new_acquisition()
{
    const auto id = schema::binary::random_uuid_v4();
    acquisition_id_ = id ? id->to_string() : std::string { "unassigned" };
    current_position_ = 0;
    accepted_readings_ = 0;
}

void Controller::configure(std::size_t position_count, std::size_t required_readings)
{
    position_count_ = position_count;
    required_readings_ = required_readings;
    state_ = State::Setup;
    message_ = "Review the session and acquisition settings";
    new_acquisition();
}

bool Controller::transition(State expected, State next, std::string message)
{
    if (state_ != expected)
        return false;
    state_ = next;
    message_ = std::move(message);
    return true;
}

bool Controller::begin_setup()
{
    return transition(State::Idle, State::Setup, "Review the session and acquisition settings");
}

bool Controller::complete_setup(bool valid)
{
    if (state_ != State::Setup || !valid)
        return false;
    return transition(State::Setup, State::Preflight, "Run the hardware preflight checks");
}

bool Controller::complete_preflight(bool valid)
{
    if (state_ != State::Preflight || !valid)
        return false;
    return transition(State::Preflight, State::OperatorPreparation,
        "Follow the preparation instructions, then confirm readiness");
}

bool Controller::confirm_operator_preparation()
{
    return transition(State::OperatorPreparation, State::DarkAcquisition,
        "Dark acquisition in progress");
}

bool Controller::complete_dark()
{
    return transition(State::DarkAcquisition, State::EnergyCalibration,
        "Enter or import the energy calibration readings");
}

bool Controller::complete_energy(bool valid)
{
    if (state_ != State::EnergyCalibration || !valid)
        return false;
    return transition(State::EnergyCalibration, State::SamplePreparation,
        "Install the sample and confirm the beam path is ready");
}

bool Controller::confirm_sample_preparation()
{
    if (!transition(State::SamplePreparation, State::SampleAcquisition,
            "Sample acquisition in progress"))
        return false;
    current_position_ = 0;
    accepted_readings_ = 0;
    return true;
}

bool Controller::record_accepted_reading()
{
    if (state_ != State::SampleAcquisition || current_position_ >= position_count_)
        return false;
    ++accepted_readings_;
    return true;
}

bool Controller::complete_position()
{
    if (state_ != State::SampleAcquisition || current_position_ >= position_count_
        || accepted_readings_ < required_readings_)
        return false;
    ++current_position_;
    accepted_readings_ = 0;
    if (current_position_ < position_count_)
        message_ = "Position complete; moving to the next position";
    return true;
}

bool Controller::complete_sample()
{
    if (state_ != State::SampleAcquisition)
        return false;
    current_position_ = position_count_;
    return transition(State::SampleAcquisition, State::Review,
        "Review the completed acquisition before publishing");
}

bool Controller::publish()
{
    return transition(State::Review, State::Published, "Acquisition published successfully");
}

void Controller::fail(std::string reason)
{
    state_ = State::Failed;
    message_ = std::move(reason);
}

void Controller::cancel()
{
    if (state_ == State::Published || state_ == State::Idle)
        return;
    state_ = State::Cancelled;
    message_ = "Acquisition cancelled; incomplete data was sealed";
}

void Controller::emergency_stop()
{
    if (state_ == State::Published || state_ == State::Idle)
        return;
    state_ = State::EmergencyStopped;
    message_ = "Emergency stop engaged; acquisition must be restarted";
}

void Controller::restart()
{
    state_ = State::Setup;
    message_ = "Restart required: review settings before beginning a new acquisition";
    new_acquisition();
}

} // namespace zscan::acquisition
