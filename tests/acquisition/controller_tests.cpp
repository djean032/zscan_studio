#include "zscan/acquisition/controller.hpp"

#include <catch2/catch_test_macros.hpp>

using zscan::acquisition::Controller;
using zscan::acquisition::State;

TEST_CASE("acquisition controller enforces the guided order")
{
    Controller controller;
    controller.configure(2, 2);
    REQUIRE(controller.state() == State::Setup);
    REQUIRE_FALSE(controller.complete_preflight(true));
    REQUIRE(controller.complete_setup(true));
    REQUIRE(controller.complete_preflight(true));
    REQUIRE(controller.confirm_operator_preparation());
    REQUIRE(controller.complete_dark());
    REQUIRE_FALSE(controller.complete_energy(false));
    REQUIRE(controller.complete_energy(true));
    REQUIRE(controller.confirm_sample_preparation());
    REQUIRE_FALSE(controller.complete_position());
    REQUIRE(controller.record_accepted_reading());
    REQUIRE(controller.record_accepted_reading());
    REQUIRE(controller.complete_position());
    REQUIRE(controller.record_accepted_reading());
    REQUIRE(controller.record_accepted_reading());
    REQUIRE(controller.complete_position());
    REQUIRE(controller.complete_sample());
    REQUIRE(controller.publish());
    REQUIRE(controller.state() == State::Published);
}

TEST_CASE("failed acquisition restart receives a new acquisition id")
{
    Controller controller;
    controller.configure(1, 1);
    const auto first_id = controller.acquisition_id();
    controller.fail("DAQ trigger timeout");
    REQUIRE(controller.state() == State::Failed);
    controller.restart();
    REQUIRE(controller.state() == State::Setup);
    REQUIRE(controller.acquisition_id() != first_id);
}

TEST_CASE("cancel and emergency stop are terminal until restart")
{
    Controller controller;
    controller.configure(1, 1);
    controller.cancel();
    REQUIRE(controller.state() == State::Cancelled);
    REQUIRE_FALSE(controller.complete_setup(true));
    controller.restart();
    controller.emergency_stop();
    REQUIRE(controller.state() == State::EmergencyStopped);
    controller.restart();
    REQUIRE(controller.state() == State::Setup);
}
