cmake_minimum_required(VERSION 3.20)

project(FastIPC VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

option(FASTIPC_BUILD_TESTS "Build FastIPC behavior tests" ON)
option(FASTIPC_BUILD_LEGACY "Expose the imported libsharedmemory target" OFF)

add_library(
    fastipc
    STATIC
        src/shared_memory_transport.cpp
        src/unix_domain_socket_transport.cpp
        src/status.cpp)
add_library(FastIPC::fastipc ALIAS fastipc)
target_compile_features(fastipc PUBLIC cxx_std_20)
target_include_directories(
    fastipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        fastipc
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion)
endif()

if(FASTIPC_BUILD_LEGACY)
    add_library(fastipc_upstream_legacy INTERFACE)
    target_compile_features(fastipc_upstream_legacy INTERFACE cxx_std_20)
    target_include_directories(
        fastipc_upstream_legacy
        INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
endif()

if(BUILD_TESTING AND FASTIPC_BUILD_TESTS)
    add_executable(fastipc_tests tests/transport_test.cpp)
    target_link_libraries(fastipc_tests PRIVATE FastIPC::fastipc Threads::Threads)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            fastipc_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion)
    endif()
    add_test(NAME fastipc.transport COMMAND fastipc_tests)

    add_executable(
        fastipc_uds_tests
        tests/unix_domain_socket_test.cpp)
    target_link_libraries(
        fastipc_uds_tests
        PRIVATE FastIPC::fastipc Threads::Threads)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            fastipc_uds_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion)
    endif()
    add_test(NAME fastipc.uds COMMAND fastipc_uds_tests)

    set(
        FASTIPC_FAULT_CASES
        peer_missing
        producer_crash
        consumer_crash
        restart
        slow_consumer
        timeout
        malformed_header
        version_mismatch
        stale_shared_memory
        queue_full
        queue_empty
        rapid_restart)
    foreach(fault_case IN LISTS FASTIPC_FAULT_CASES)
        add_test(
            NAME fastipc.fault.${fault_case}
            COMMAND fastipc_tests --case ${fault_case})
        set_tests_properties(
            fastipc.fault.${fault_case}
            PROPERTIES LABELS fault)
    endforeach()
endif()
