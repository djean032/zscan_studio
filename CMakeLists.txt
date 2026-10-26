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
  src/schema/session_validator.cpp src/schema/derived.cpp src/schema/derived_arrays.cpp)
target_compile_features(zscan_schema PUBLIC cxx_std_23)
target_include_directories(zscan_schema PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(zscan_schema PUBLIC OpenSSL::Crypto PRIVATE tomlplusplus::tomlplusplus)

option(ZSCAN_ENABLE_WORKUP "Build the optional zfit_cpp workup adapter" OFF)
option(ZSCAN_ENABLE_FITTING "Build the optional zfit_cpp fit bridge" OFF)
if(ZSCAN_ENABLE_FITTING)
  set(ZSCAN_ENABLE_WORKUP ON)
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
    tests/schema/derived_tests.cpp tests/schema/derived_arrays_tests.cpp)
  target_link_libraries(zscan_schema_tests PRIVATE zscan_schema Catch2::Catch2WithMain)
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
