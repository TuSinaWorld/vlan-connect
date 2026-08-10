cmake_minimum_required(VERSION 3.5)
get_filename_component(VLAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(VLAN_TEST_BUILD_ROOT
    "${VLAN_ROOT}/build-tests" ABSOLUTE)
get_filename_component(VLAN_BINARY_DIR
    "${CMAKE_BINARY_DIR}" ABSOLUTE)
file(TO_CMAKE_PATH "${VLAN_TEST_BUILD_ROOT}" VLAN_TEST_BUILD_ROOT_NORMALIZED)
file(TO_CMAKE_PATH "${VLAN_BINARY_DIR}" VLAN_BINARY_DIR_NORMALIZED)
string(FIND "${VLAN_BINARY_DIR_NORMALIZED}/"
    "${VLAN_TEST_BUILD_ROOT_NORMALIZED}/" VLAN_BUILD_ROOT_OFFSET)
if(NOT VLAN_BUILD_ROOT_OFFSET EQUAL 0)
    message(FATAL_ERROR
        "Configure isolated tests below ${VLAN_TEST_BUILD_ROOT}")
endif()

project(vlan_protocol_v8_tests C CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(VLAN_TEST_PLATFORM windows)
else()
    set(VLAN_TEST_PLATFORM linux)
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING
        "Isolated test build configuration" FORCE)
endif()

function(vlan_test_output target)
    set(base "${VLAN_ROOT}/build-tests/${VLAN_TEST_PLATFORM}")
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${base}/${CMAKE_BUILD_TYPE}")
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target} PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${config_upper}"
            "${base}/${config}")
    endforeach()
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
    if(MSVC AND MSVC_VERSION LESS_EQUAL 1900)
        target_compile_options(${target} PRIVATE
            "/FI${VLAN_ROOT}/common/msvc2015_compat.h")
    endif()
endfunction()

set(VLAN_COMMON_INCLUDES
    "${VLAN_ROOT}/common"
    "${VLAN_ROOT}/3rdparty/monocypher")
set(VLAN_MONOCYPHER
    "${VLAN_ROOT}/3rdparty/monocypher/monocypher.c")

add_executable(protocol_v8_tests
    common/protocol_v8_tests.cpp
    ${VLAN_MONOCYPHER})
target_include_directories(protocol_v8_tests PRIVATE
    ${VLAN_COMMON_INCLUDES})
vlan_test_output(protocol_v8_tests)

add_executable(server_auth_file_tests
    server/auth_file_tests.cpp)
target_include_directories(server_auth_file_tests PRIVATE
    "${VLAN_ROOT}/server/src")
vlan_test_output(server_auth_file_tests)

add_executable(server_session_tests
    server/server_session_state_tests.cpp
    ${VLAN_MONOCYPHER})
target_include_directories(server_session_tests PRIVATE
    ${VLAN_COMMON_INCLUDES}
    "${VLAN_ROOT}/server/src")
vlan_test_output(server_session_tests)

add_executable(gui_signal_tests
    gui/gui_signal_tests.cpp)
target_include_directories(gui_signal_tests PRIVATE
    ${VLAN_COMMON_INCLUDES})
vlan_test_output(gui_signal_tests)

add_executable(cli_signal_tests
    cli/cli_signal_tests.cpp)
target_include_directories(cli_signal_tests PRIVATE
    ${VLAN_COMMON_INCLUDES}
    "${VLAN_ROOT}/client-cli/src")
if(WIN32)
    target_link_libraries(cli_signal_tests PRIVATE ws2_32)
endif()
vlan_test_output(cli_signal_tests)

add_executable(data_channel_tests
    common/data_channel_tests.cpp
    ${VLAN_MONOCYPHER})
target_include_directories(data_channel_tests PRIVATE
    ${VLAN_COMMON_INCLUDES})
vlan_test_output(data_channel_tests)

add_executable(data_plane_tests
    gui/data_plane_tests.cpp
    ${VLAN_MONOCYPHER})
target_include_directories(data_plane_tests PRIVATE
    ${VLAN_COMMON_INCLUDES})
vlan_test_output(data_plane_tests)

if(WIN32)
    find_package(Qt5 COMPONENTS Core REQUIRED)
    add_executable(wintun_lifecycle_tests
        wintun/wintun_lifecycle_tests.cpp
        "${VLAN_ROOT}/client/src/core/tun_adapter.cpp"
        "${VLAN_ROOT}/client/src/ui/log_manager.cpp")
    set_target_properties(wintun_lifecycle_tests PROPERTIES
        AUTOMOC ON)
    target_include_directories(wintun_lifecycle_tests PRIVATE
        ${VLAN_COMMON_INCLUDES}
        "${VLAN_ROOT}/client/src/core"
        "${VLAN_ROOT}/client/src/ui")
    target_compile_definitions(wintun_lifecycle_tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(wintun_lifecycle_tests PRIVATE
        Qt5::Core iphlpapi)
else()
    add_executable(wintun_lifecycle_tests
        wintun/wintun_lifecycle_tests.cpp)

    find_package(Threads REQUIRED)
    add_executable(linux_tun_netlink_tests
        cli/linux_tun_netlink_tests.cpp
        "${VLAN_ROOT}/client-cli/src/cli_tun_linux.cpp")
    target_include_directories(linux_tun_netlink_tests PRIVATE
        ${VLAN_COMMON_INCLUDES}
        "${VLAN_ROOT}/client-cli/src")
    target_link_libraries(linux_tun_netlink_tests PRIVATE Threads::Threads)
    vlan_test_output(linux_tun_netlink_tests)

    add_executable(cli_raw_fec_tests
        cli/raw_fec_limits_tests.cpp
        "${VLAN_ROOT}/client-cli/src/cli_peer.cpp"
        "${VLAN_ROOT}/client-cli/src/cli_fec.cpp"
        "${VLAN_ROOT}/3rdparty/kcp/ikcp.c"
        "${VLAN_ROOT}/3rdparty/cm256cc/cm256.cpp"
        "${VLAN_ROOT}/3rdparty/cm256cc/gf256.cpp")
    target_include_directories(cli_raw_fec_tests PRIVATE
        ${VLAN_COMMON_INCLUDES}
        "${VLAN_ROOT}/client-cli/src"
        "${VLAN_ROOT}/3rdparty/kcp"
        "${VLAN_ROOT}/3rdparty/cm256cc")
    target_compile_definitions(cli_raw_fec_tests PRIVATE cm256cc_STATIC)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
        target_compile_definitions(cli_raw_fec_tests PRIVATE USE_SSSE3)
        target_compile_options(cli_raw_fec_tests PRIVATE -mssse3)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64|armv7.*)$")
        target_compile_definitions(cli_raw_fec_tests PRIVATE USE_NEON)
    else()
        message(FATAL_ERROR "Unsupported processor for cli_raw_fec_tests")
    endif()
    target_link_libraries(cli_raw_fec_tests PRIVATE Threads::Threads)
    vlan_test_output(cli_raw_fec_tests)
endif()
vlan_test_output(wintun_lifecycle_tests)

add_test(NAME protocol_v8_tests COMMAND protocol_v8_tests)
add_test(NAME server_auth_file_tests COMMAND server_auth_file_tests)
add_test(NAME server_session_tests COMMAND server_session_tests)
add_test(NAME gui_signal_tests COMMAND gui_signal_tests)
add_test(NAME cli_signal_tests COMMAND cli_signal_tests)
add_test(NAME data_channel_tests COMMAND data_channel_tests)
add_test(NAME data_plane_tests COMMAND data_plane_tests)
add_test(NAME wintun_lifecycle_tests COMMAND wintun_lifecycle_tests)
if(NOT WIN32)
    add_test(NAME linux_tun_netlink_tests COMMAND linux_tun_netlink_tests)
    add_test(NAME cli_raw_fec_tests COMMAND cli_raw_fec_tests)
endif()
