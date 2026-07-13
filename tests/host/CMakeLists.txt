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

add_executable(nivalo_cli_provisioning_policy_tests
    test_cli_provisioning_policy.cpp
    ../../src/NivaloCliProvisioningPolicy.cpp
)
target_include_directories(nivalo_cli_provisioning_policy_tests PRIVATE ../../src)
target_compile_features(nivalo_cli_provisioning_policy_tests PRIVATE cxx_std_11)

if(MSVC)
    target_compile_options(nivalo_cli_provisioning_policy_tests PRIVATE /W4 /WX)
else()
    target_compile_options(nivalo_cli_provisioning_policy_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_test(NAME nivalo_cli_provisioning_policy_tests COMMAND nivalo_cli_provisioning_policy_tests)

add_executable(nivalo_cli_frame_tests
    test_cli_frame.cpp
    ../../src/NivaloCliFrame.cpp
)
target_include_directories(nivalo_cli_frame_tests PRIVATE ../../src)
target_compile_features(nivalo_cli_frame_tests PRIVATE cxx_std_11)

if(MSVC)
    target_compile_options(nivalo_cli_frame_tests PRIVATE /W4 /WX)
else()
    target_compile_options(nivalo_cli_frame_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_test(NAME nivalo_cli_frame_tests COMMAND nivalo_cli_frame_tests)

add_executable(nivalo_cli_transaction_tests test_cli_transaction.cpp)
target_include_directories(nivalo_cli_transaction_tests PRIVATE ../../src)
target_compile_features(nivalo_cli_transaction_tests PRIVATE cxx_std_11)

if(MSVC)
    target_compile_options(nivalo_cli_transaction_tests PRIVATE /W4 /WX)
else()
    target_compile_options(nivalo_cli_transaction_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_test(NAME nivalo_cli_transaction_tests COMMAND nivalo_cli_transaction_tests)

add_executable(nivalo_provisioning_time_policy_tests test_provisioning_time_policy.cpp)
target_include_directories(nivalo_provisioning_time_policy_tests PRIVATE ../../src)
target_compile_features(nivalo_provisioning_time_policy_tests PRIVATE cxx_std_11)

if(MSVC)
    target_compile_options(nivalo_provisioning_time_policy_tests PRIVATE /W4 /WX)
else()
    target_compile_options(nivalo_provisioning_time_policy_tests PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_test(NAME nivalo_provisioning_time_policy_tests COMMAND nivalo_provisioning_time_policy_tests)
