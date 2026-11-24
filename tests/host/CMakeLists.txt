cmake_minimum_required(VERSION 3.16)
project(nivalo_esp32_host_tests LANGUAGES C CXX)

enable_testing()
# MinGW: avoid picking up an unrelated libstdc++ DLL from PATH at test time.
if(MINGW)
    add_link_options(-static)
endif()
include(FetchContent)

# --- Protocol fixtures -------------------------------------------------------------------------
# The v1.1 tests replay nivalo-protocol's fixtures and vectors. CI checks out the pinned commit
# (NIVALO_PROTOCOL_REF in .github/workflows/ci.yml); locally point NIVALO_PROTOCOL_DIR at a checkout.
if(NOT DEFINED NIVALO_PROTOCOL_DIR AND DEFINED ENV{NIVALO_PROTOCOL_DIR})
    set(NIVALO_PROTOCOL_DIR "$ENV{NIVALO_PROTOCOL_DIR}")
endif()
if(NOT DEFINED NIVALO_PROTOCOL_DIR)
    get_filename_component(NIVALO_PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../nivalo-protocol" ABSOLUTE)
endif()
if(NOT EXISTS "${NIVALO_PROTOCOL_DIR}/fixtures/v1.1/vectors/command-execution.json")
    message(FATAL_ERROR "nivalo-protocol fixtures not found; set NIVALO_PROTOCOL_DIR (now: ${NIVALO_PROTOCOL_DIR})")
endif()
message(STATUS "nivalo-protocol fixtures: ${NIVALO_PROTOCOL_DIR}")

# --- Pinned third-party sources ----------------------------------------------------------------
# ArduinoJson and mbedtls at the versions the firmware builds against (ArduinoJson 6.21.6 from
# library.json; mbedtls 2.28.7 is the one bundled with Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7).
FetchContent_Declare(
    arduinojson
    URL https://github.com/bblanchon/ArduinoJson/releases/download/v6.21.6/ArduinoJson-v6.21.6.h
    URL_HASH SHA256=8e68afefdb56071b8a34415539818eae3c974b19c0fa7693017f8418d4c85fe0
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_Declare(
    mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v2.28.7.tar.gz
    URL_HASH SHA256=1df6073f0cf6a4e1953890bf5e0de2a8c7e6be50d6d6c69fa9fefcb1d14e981a
    # Download only; the sources are compiled below with a minimal configuration.
    SOURCE_SUBDIR do-not-add
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(arduinojson mbedtls)

set(NIVALO_HOST_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/include")
file(WRITE "${NIVALO_HOST_INCLUDE}/ArduinoJson.h" "#include \"${arduinojson_SOURCE_DIR}/ArduinoJson-v6.21.6.h\"\n")

file(GLOB NIVALO_MBEDTLS_SOURCES "${mbedtls_SOURCE_DIR}/library/*.c")
add_library(nivalo_host_mbedtls STATIC ${NIVALO_MBEDTLS_SOURCES})
target_include_directories(nivalo_host_mbedtls PUBLIC "${mbedtls_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(nivalo_host_mbedtls PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_host_config.h")
if(NOT MSVC)
    target_compile_options(nivalo_host_mbedtls PRIVATE -w)
endif()

function(nivalo_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endfunction()

# --- Existing policy tests ---------------------------------------------------------------------
add_executable(nivalo_reconnect_policy_tests
    test_reconnect_policy.cpp
    ../../src/NivaloReconnectPolicy.cpp
)
target_include_directories(nivalo_reconnect_policy_tests PRIVATE ../../src)
target_compile_features(nivalo_reconnect_policy_tests PRIVATE cxx_std_11)
nivalo_warnings(nivalo_reconnect_policy_tests)
add_test(NAME nivalo_reconnect_policy_tests COMMAND nivalo_reconnect_policy_tests)

add_executable(nivalo_cli_provisioning_policy_tests
    test_cli_provisioning_policy.cpp
    ../../src/NivaloCliProvisioningPolicy.cpp
)
target_include_directories(nivalo_cli_provisioning_policy_tests PRIVATE ../../src)
target_compile_features(nivalo_cli_provisioning_policy_tests PRIVATE cxx_std_11)
nivalo_warnings(nivalo_cli_provisioning_policy_tests)
add_test(NAME nivalo_cli_provisioning_policy_tests COMMAND nivalo_cli_provisioning_policy_tests)

add_executable(nivalo_cli_frame_tests
    test_cli_frame.cpp
    ../../src/NivaloCliFrame.cpp
)
target_include_directories(nivalo_cli_frame_tests PRIVATE ../../src)
target_compile_features(nivalo_cli_frame_tests PRIVATE cxx_std_11)
nivalo_warnings(nivalo_cli_frame_tests)
add_test(NAME nivalo_cli_frame_tests COMMAND nivalo_cli_frame_tests)

add_executable(nivalo_cli_transaction_tests test_cli_transaction.cpp)
target_include_directories(nivalo_cli_transaction_tests PRIVATE ../../src)
target_compile_features(nivalo_cli_transaction_tests PRIVATE cxx_std_11)
nivalo_warnings(nivalo_cli_transaction_tests)
add_test(NAME nivalo_cli_transaction_tests COMMAND nivalo_cli_transaction_tests)

add_executable(nivalo_provisioning_time_policy_tests test_provisioning_time_policy.cpp)
target_include_directories(nivalo_provisioning_time_policy_tests PRIVATE ../../src)
target_compile_features(nivalo_provisioning_time_policy_tests PRIVATE cxx_std_11)
nivalo_warnings(nivalo_provisioning_time_policy_tests)
add_test(NAME nivalo_provisioning_time_policy_tests COMMAND nivalo_provisioning_time_policy_tests)

add_executable(nivalo_nivalolink_receive_sequence_tests test_nivalolink_receive_sequence.cpp)
target_include_directories(nivalo_nivalolink_receive_sequence_tests PRIVATE ../../src)
target_compile_features(nivalo_nivalolink_receive_sequence_tests PRIVATE cxx_std_11)
nivalo_warnings(nivalo_nivalolink_receive_sequence_tests)
add_test(NAME nivalo_nivalolink_receive_sequence_tests COMMAND nivalo_nivalolink_receive_sequence_tests)

add_executable(nivalo_nivalolink_command_poll_policy_tests
    test_nivalolink_command_poll_policy.cpp
    ../../src/NivaloLinkCommandPollPolicy.cpp
)
target_include_directories(nivalo_nivalolink_command_poll_policy_tests PRIVATE ../../src)
target_compile_features(nivalo_nivalolink_command_poll_policy_tests PRIVATE cxx_std_11)
nivalo_warnings(nivalo_nivalolink_command_poll_policy_tests)
add_test(NAME nivalo_nivalolink_command_poll_policy_tests COMMAND nivalo_nivalolink_command_poll_policy_tests)

# --- Protocol v1.1 -----------------------------------------------------------------------------
# The firmware's own protocol units, compiled for the host against the same libraries.
add_library(nivalo_v11 STATIC
    ../../src/NivaloWire.cpp
    ../../src/NivaloJsonStrict.cpp
    ../../src/NivaloArguments.cpp
    ../../src/NivaloDownlink.cpp
    ../../src/NivaloClock.cpp
    ../../src/NivaloOtaPolicy.cpp
    ../../src/NivaloImageIdentity.cpp
    ../../src/NivaloJournal.cpp
    ../../src/NivaloExecutor.cpp
    ../../src/NivaloCatalog.cpp
    ../../src/NivaloUplink.cpp
)
target_include_directories(nivalo_v11 PUBLIC ../../src "${NIVALO_HOST_INCLUDE}")
target_compile_features(nivalo_v11 PUBLIC cxx_std_11)
# The mock image of the protocol's OTA vectors: product 1111... at security version 7.
target_compile_definitions(nivalo_v11 PUBLIC
    ARDUINOJSON_USE_LONG_LONG=1
    ARDUINOJSON_USE_DOUBLE=1
    NIVALO_PRODUCT_ID="11111111-1111-4111-8111-111111111111"
    NIVALO_SECURITY_VERSION=7
)
target_link_libraries(nivalo_v11 PUBLIC nivalo_host_mbedtls)
nivalo_warnings(nivalo_v11)

foreach(name wire execution ota catalog uplink)
    add_executable(nivalo_v11_${name}_tests test_v11_${name}.cpp)
    target_link_libraries(nivalo_v11_${name}_tests PRIVATE nivalo_v11)
    target_compile_definitions(nivalo_v11_${name}_tests PRIVATE NIVALO_PROTOCOL_DIR="${NIVALO_PROTOCOL_DIR}")
    nivalo_warnings(nivalo_v11_${name}_tests)
    add_test(NAME nivalo_v11_${name}_tests COMMAND nivalo_v11_${name}_tests)
endforeach()

# The durable protocol state (NivaloDeviceStore) against a map-backed stand-in for Preferences, so
# identity changes and unreadable records are exercised on the real store code.
add_executable(nivalo_v11_store_tests test_v11_store.cpp ../../src/NivaloDeviceStore.cpp)
target_include_directories(nivalo_v11_store_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fakes")
target_link_libraries(nivalo_v11_store_tests PRIVATE nivalo_v11)
target_compile_definitions(nivalo_v11_store_tests PRIVATE NIVALO_PROTOCOL_DIR="${NIVALO_PROTOCOL_DIR}")
nivalo_warnings(nivalo_v11_store_tests)
add_test(NAME nivalo_v11_store_tests COMMAND nivalo_v11_store_tests)

# Every uplink the firmware builds is checked against the protocol's reference model.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    set(NIVALO_UPLINK_SAMPLES "${CMAKE_CURRENT_BINARY_DIR}/uplink-samples.json")
    set_tests_properties(nivalo_v11_uplink_tests PROPERTIES
        ENVIRONMENT "NIVALO_UPLINK_SAMPLES=${NIVALO_UPLINK_SAMPLES}"
        FIXTURES_SETUP nivalo_uplink_samples)
    add_test(NAME nivalo_v11_uplink_reference_model
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../validate_v11_uplinks.py"
            --protocol "${NIVALO_PROTOCOL_DIR}" --samples "${NIVALO_UPLINK_SAMPLES}")
    set_tests_properties(nivalo_v11_uplink_reference_model PROPERTIES FIXTURES_REQUIRED nivalo_uplink_samples)
endif()
