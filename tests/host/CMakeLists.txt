cmake_minimum_required(VERSION 3.16)
project(nivalo_esp32_host_tests LANGUAGES CXX)

enable_testing()

add_executable(nivalo_reconnect_policy_tests
    test_reconnect_policy.cpp
    ../../src/NivaloReconnectPolicy.cpp
)
target_include_directories(nivalo_reconnect_policy_tests PRIVATE ../../src)
target_compile_features(nivalo_reconnect_policy_tests PRIVATE cxx_std_11)

if(MSVC)
    target_compile_options(nivalo_reconnect_policy_tests PRIVATE /W4 /WX)
else()
    target_compile_options(nivalo_reconnect_policy_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_test(NAME nivalo_reconnect_policy_tests COMMAND nivalo_reconnect_policy_tests)
