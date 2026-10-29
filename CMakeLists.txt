cmake_minimum_required(VERSION 3.20)
project(zscan_studio_cpp LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de) # v3.4.0
FetchContent_MakeAvailable(tomlplusplus)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(zscan_schema src/schema/types.cpp src/schema/binary.cpp src/schema/scan.cpp
  src/schema/dark_energy.cpp src/schema/storage.cpp src/schema/toml_io.cpp
  src/schema/session_validator.cpp src/schema/derived.cpp src/schema/derived_arrays.cpp
  src/schema/validation_report.cpp src/schema/materials.cpp)
add_library(zscan_hardware_mock src/hardware/mock.cpp)
target_compile_features(zscan_hardware_mock PUBLIC cxx_std_23)
target_include_directories(zscan_hardware_mock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(zscan_acquisition_controller src/acquisition/controller.cpp)
target_compile_features(zscan_acquisition_controller PUBLIC cxx_std_23)
target_include_directories(zscan_acquisition_controller PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zscan_acquisition_controller PUBLIC zscan_schema)
add_library(zscan_acquisition_runner src/acquisition/runner.cpp)
target_compile_features(zscan_acquisition_runner PUBLIC cxx_std_23)
target_include_directories(zscan_acquisition_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zscan_acquisition_runner PUBLIC zscan_schema)
add_library(zscan_acquisition_persistence src/acquisition/persistence.cpp)
target_compile_features(zscan_acquisition_persistence PUBLIC cxx_std_23)
target_include_directories(zscan_acquisition_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zscan_acquisition_persistence PUBLIC zscan_acquisition_runner zscan_schema)
add_library(zscan_acquisition_session src/acquisition/session.cpp)
target_compile_features(zscan_acquisition_session PUBLIC cxx_std_23)
target_include_directories(zscan_acquisition_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zscan_acquisition_session PUBLIC zscan_acquisition_persistence
  PRIVATE tomlplusplus::tomlplusplus)
add_library(zscan_acquisition_dark src/acquisition/dark_runner.cpp)
target_compile_features(zscan_acquisition_dark PUBLIC cxx_std_23)
target_include_directories(zscan_acquisition_dark PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zscan_acquisition_dark PUBLIC zscan_schema)
target_compile_features(zscan_schema PUBLIC cxx_std_23)
target_include_directories(zscan_schema PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zscan_schema PUBLIC OpenSSL::Crypto PRIVATE tomlplusplus::tomlplusplus)

option(ZSCAN_ENABLE_WORKUP "Build the optional zfit_cpp workup adapter" OFF)
option(ZSCAN_ENABLE_FITTING "Build the optional zfit_cpp fit bridge" OFF)
option(ZSCAN_ENABLE_QT "Build the read-only Qt Widgets session application" OFF)
option(ZSCAN_ENABLE_NIDAQMX "Build the optional NI-DAQmx detector adapter" OFF)
if(ZSCAN_ENABLE_NIDAQMX)
  find_path(NIDAQMX_INCLUDE_DIR NIDAQmx.h REQUIRED)
  find_library(NIDAQMX_LIBRARY NAMES nidaqmx REQUIRED)
  add_library(zscan_nidaqmx src/hardware/nidaqmx.cpp)
  target_compile_features(zscan_nidaqmx PUBLIC cxx_std_23)
  target_include_directories(zscan_nidaqmx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${NIDAQMX_INCLUDE_DIR})
  target_link_libraries(zscan_nidaqmx PUBLIC zscan_schema ${NIDAQMX_LIBRARY})
endif()
if(ZSCAN_ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent QuickWidgets Graphs)
  set(CMAKE_AUTOMOC ON)
  add_executable(zscan_studio
    src/app/main.cpp
    src/app/session_window.cpp
    src/app/session_window.hpp
    src/app/acquisition_wizard.cpp
    src/app/acquisition_wizard.hpp
    src/app/material_editor.cpp
    src/app/material_editor.hpp
    src/app/curve_plot.cpp
    src/app/curve_plot.hpp)
  target_compile_features(zscan_studio PRIVATE cxx_std_23)
  target_sources(zscan_studio PRIVATE src/hardware/esp302.cpp src/hardware/factory.cpp)
  target_sources(zscan_studio PRIVATE src/acquisition/worker.cpp include/zscan/acquisition/worker.hpp)
  target_link_libraries(zscan_studio PRIVATE zscan_acquisition_controller zscan_acquisition_runner
    zscan_acquisition_session zscan_acquisition_dark zscan_hardware_mock)
  target_link_libraries(zscan_studio PRIVATE zscan_schema Qt6::Core Qt6::Widgets Qt6::Concurrent Qt6::QuickWidgets Qt6::Graphs Qt6::Network)
  target_compile_definitions(zscan_studio PRIVATE
    $<$<BOOL:${ZSCAN_ENABLE_NIDAQMX}>:ZSCAN_ENABLE_NIDAQMX>)
  if(ZSCAN_ENABLE_NIDAQMX)
    target_link_libraries(zscan_studio PRIVATE zscan_nidaqmx)
  endif()
  qt_add_resources(zscan_studio "plot_qml" PREFIX "/" FILES qml/CurvePlot.qml)
endif()
if(ZSCAN_ENABLE_FITTING)
  set(ZSCAN_ENABLE_WORKUP ON)
endif()
if(ZSCAN_ENABLE_QT AND ZSCAN_ENABLE_WORKUP)
  target_link_libraries(zscan_studio PRIVATE zscan_workup_adapter)
endif()
if(ZSCAN_ENABLE_QT AND ZSCAN_ENABLE_FITTING)
  target_link_libraries(zscan_studio PRIVATE zscan_fit_adapter)
endif()
if(ZSCAN_ENABLE_WORKUP)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/zfit_cpp/CMakeLists.txt")
    message(FATAL_ERROR "Initialize the pinned zfit_cpp submodule first")
  endif()
  set(ZFIT_ENABLE_CVODES ${ZSCAN_ENABLE_FITTING} CACHE BOOL "CVODES needed only for fitting" FORCE)
  set(ZFIT_ENABLE_FITTING ${ZSCAN_ENABLE_FITTING} CACHE BOOL "Optional fitting layer" FORCE)
  set(_zscan_build_testing "${BUILD_TESTING}")
  set(BUILD_TESTING OFF) # Do not register submodule tests in this build.
  add_subdirectory(external/zfit_cpp EXCLUDE_FROM_ALL)
  set(BUILD_TESTING "${_zscan_build_testing}")
  add_library(zscan_workup_adapter src/schema/workup_adapter.cpp)
  target_compile_features(zscan_workup_adapter PUBLIC cxx_std_23)
  target_include_directories(zscan_workup_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(zscan_workup_adapter PUBLIC zscan_schema zfit_workup)
  if(ZSCAN_ENABLE_FITTING)
    add_library(zscan_fit_adapter src/schema/fit_adapter.cpp)
    target_compile_features(zscan_fit_adapter PUBLIC cxx_std_23)
    target_include_directories(zscan_fit_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(zscan_fit_adapter PUBLIC zscan_workup_adapter zfit_fit)
  endif()
  if(ZSCAN_ENABLE_QT)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(zscan_wizard_tests tests/acquisition/wizard_tests.cpp
      src/app/acquisition_wizard.cpp src/app/acquisition_wizard.hpp
      src/app/material_editor.cpp src/app/material_editor.hpp
      src/acquisition/worker.cpp include/zscan/acquisition/worker.hpp
      src/hardware/esp302.cpp src/hardware/factory.cpp)
    target_include_directories(zscan_wizard_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(zscan_wizard_tests PRIVATE zscan_acquisition_controller
      zscan_acquisition_session zscan_acquisition_dark zscan_hardware_mock
      Qt6::Test Qt6::Widgets Qt6::Network)
    add_test(NAME mock_wizard_workflow COMMAND zscan_wizard_tests)
    set_tests_properties(mock_wizard_workflow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)

  add_executable(zscan_schema_tests tests/schema/types_tests.cpp tests/schema/binary_tests.cpp
    tests/schema/scan_tests.cpp tests/schema/dark_energy_tests.cpp tests/schema/storage_tests.cpp
    tests/schema/toml_io_tests.cpp tests/schema/session_validator_tests.cpp
    tests/schema/derived_tests.cpp tests/schema/derived_arrays_tests.cpp tests/hardware/mock_tests.cpp
    tests/acquisition/controller_tests.cpp tests/acquisition/runner_tests.cpp
    tests/acquisition/session_tests.cpp)
  target_link_libraries(zscan_schema_tests PRIVATE zscan_schema zscan_hardware_mock
    zscan_acquisition_controller zscan_acquisition_runner zscan_acquisition_session
    zscan_acquisition_dark Catch2::Catch2WithMain)
  target_compile_definitions(zscan_schema_tests PRIVATE ZSCAN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(zscan_schema_tests)
  if(ZSCAN_ENABLE_WORKUP)
    add_executable(zscan_workup_tests tests/schema/workup_adapter_tests.cpp)
    target_link_libraries(zscan_workup_tests PRIVATE zscan_workup_adapter Catch2::Catch2WithMain)
    catch_discover_tests(zscan_workup_tests)
  endif()
  if(ZSCAN_ENABLE_FITTING)
    add_executable(zscan_fit_adapter_tests tests/schema/fit_adapter_tests.cpp)
    target_link_libraries(zscan_fit_adapter_tests PRIVATE zscan_fit_adapter Catch2::Catch2WithMain)
    catch_discover_tests(zscan_fit_adapter_tests)
  endif()
endif()
