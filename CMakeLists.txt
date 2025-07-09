# Build core_fsm
# Can be built from ESP-IDF and as stand-alone for testing with GoogleTest.
#
# Check the configuration setup in 
# - CMakePresets.json
# - .vscode/settings.json
# - .vscode/tasks.json
#
# Use the task to run tests, Cmd+Shift+B (it's added to the build group).
#
# One-shot CLI from lib/core_fsm:
#   cmake --workflow --preset host --fresh
#
# CLI if running tests manually:
#   rm -rf build-host
#   mkdir -p build-host/ && cd build-host
#   cmake -S ../lib/core_fsm -B . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCORE_FSM_BUILD_TESTS=ON
#   cmake --build .
#   cd test && ctest --output-on-failure         <- if you want to use ctest bundled with CMake, see ctest -N or run specific with ctest -R
#   test/test_core_fsm                           <- if you want want GoogleTest's standard runner

cmake_minimum_required(VERSION 3.16)
project(core_fsm LANGUAGES CXX)

# --- Library ---
add_library(core_fsm STATIC src/fsm.cpp)
add_library(core_fsm::core_fsm ALIAS core_fsm)  # alias

# --- Includes ---
target_include_directories(core_fsm PUBLIC include)

# --- Strict ISO C++20 ---
target_compile_features(core_fsm PUBLIC cxx_std_20)
set_target_properties(core_fsm PROPERTIES CXX_EXTENSIONS OFF)

# --- Make the build-directory the top level for tests ---
enable_testing()

# --- Testing, set via CMake options ---
option(CORE_FSM_BUILD_TESTS "Build tests for core_fsm" OFF)

if(CORE_FSM_BUILD_TESTS AND NOT CMAKE_SCRIPT_MODE_FILE)
    add_subdirectory(test)
endif()