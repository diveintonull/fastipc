cmake_minimum_required(VERSION 3.20)

project(FastIPC VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

option(FASTIPC_BUILD_TESTS "Build FastIPC behavior tests" ON)
option(FASTIPC_BUILD_BENCHMARKS "Build the FastIPC benchmark runner" ON)

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

if(FASTIPC_BUILD_BENCHMARKS)
    set(FASTIPC_SOURCE_REVISION "unknown")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND
                ${GIT_EXECUTABLE}
                rev-parse
                --short=12
                HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE FASTIPC_SOURCE_REVISION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()

    add_executable(
        fastipc_benchmark
        benchmarks/benchmark_main.cpp
        benchmarks/benchmark_runner.cpp)
    target_link_libraries(
        fastipc_benchmark
        PRIVATE FastIPC::fastipc Threads::Threads)
    target_compile_definitions(
        fastipc_benchmark
        PRIVATE
            FASTIPC_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
            FASTIPC_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            FASTIPC_SOURCE_REVISION="${FASTIPC_SOURCE_REVISION}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            fastipc_benchmark
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion)
    endif()
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

    if(FASTIPC_BUILD_BENCHMARKS)
        add_test(
            NAME fastipc.benchmark_smoke
            COMMAND
                ${CMAKE_COMMAND}
                -DFASTIPC_BENCHMARK=$<TARGET_FILE:fastipc_benchmark>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_smoke.cmake)
        set_tests_properties(
            fastipc.benchmark_smoke
            PROPERTIES LABELS benchmark)
    endif()

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
