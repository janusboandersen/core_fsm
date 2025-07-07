# Builds the tests for core_fsm.
# Read this file as if appended to the core_fsm/CMakeLists.txt
# Responsibility:
# - Get GoogleTest
# - Build test executable
# - Discover tests after build

# --- CMP0135, use newer download time to trigger rebuild of GoogleTest ---
cmake_policy(SET CMP0135 NEW)
set(DOWNLOAD_EXTRACT_TIMESTAMP true)

# --- Enable CMake's testing harness ---
enable_testing()

# --- Ready GoogleTest ---
include(FetchContent)
FetchContent_Declare(
    GoogleTest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)

FetchContent_MakeAvailable(googletest)
include(GoogleTest)                         # Test-discovery helpers

# --- Test executable and linking ---
add_executable(test_core_fsm 
    test_fsm_gtest.cpp
)
target_link_libraries(test_core_fsm
    PRIVATE
        core_fsm::core_fsm
        GTest::gtest_main
)

# --- Strict ISO C++20 ---
target_compile_features(test_core_fsm PRIVATE cxx_std_20)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Test-discovery, equivalent to test_core_foo --gtest_list_tests ---
gtest_discover_tests(test_core_fsm DISCOVERY_MODE POST_BUILD)