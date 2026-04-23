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
        src/mpmc_shared_memory_transport.cpp
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
    find_package(iceoryx_posh CONFIG QUIET)
    if(iceoryx_posh_FOUND)
        set(
            FASTIPC_ICEORYX_BASELINE_DETAIL
            "iceoryx_posh was found, but the benchmark adapter is not implemented, so no measurement was emitted")
    else()
        set(
            FASTIPC_ICEORYX_BASELINE_DETAIL
            "iceoryx_posh CMake package was not found, so no measurement was emitted")
    endif()

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
            FASTIPC_SOURCE_REVISION="${FASTIPC_SOURCE_REVISION}"
            FASTIPC_ICEORYX_BASELINE_DETAIL="${FASTIPC_ICEORYX_BASELINE_DETAIL}")
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
    add_executable(
        fastipc_mpmc_benchmark
        benchmarks/mpmc_contention_main.cpp)
    target_link_libraries(
        fastipc_mpmc_benchmark
        PRIVATE FastIPC::fastipc Threads::Threads)
    target_compile_definitions(
        fastipc_mpmc_benchmark
        PRIVATE
            FASTIPC_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
            FASTIPC_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            FASTIPC_SOURCE_REVISION="${FASTIPC_SOURCE_REVISION}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            fastipc_mpmc_benchmark
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
        fastipc_zero_copy_tests
        tests/zero_copy_test.cpp)
    target_link_libraries(
        fastipc_zero_copy_tests
        PRIVATE FastIPC::fastipc Threads::Threads)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            fastipc_zero_copy_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion)
    endif()
    add_test(NAME fastipc.zero_copy COMMAND fastipc_zero_copy_tests)
    add_test(NAME fastipc.zero_copy.no_allocation
             COMMAND fastipc_zero_copy_tests
                     --case steady_state_allocation)
    add_test(NAME fastipc.fault.producer_loan_crash
             COMMAND fastipc_zero_copy_tests --case producer_loan_crash)
    add_test(NAME fastipc.fault.consumer_sample_crash
             COMMAND fastipc_zero_copy_tests --case consumer_sample_crash)
    add_test(NAME fastipc.fault.outstanding_handle_close
             COMMAND fastipc_zero_copy_tests --case outstanding_handle_close)
    add_test(NAME fastipc.fault.producer_paused_takeover
             COMMAND fastipc_zero_copy_tests --case producer_paused_takeover)
    add_test(NAME fastipc.fault.consumer_paused_takeover
             COMMAND fastipc_zero_copy_tests --case consumer_paused_takeover)
    set_tests_properties(
        fastipc.fault.producer_loan_crash
        fastipc.fault.consumer_sample_crash
        fastipc.fault.outstanding_handle_close
        fastipc.fault.producer_paused_takeover
        fastipc.fault.consumer_paused_takeover
        PROPERTIES LABELS fault)

    add_executable(
        fastipc_mpmc_tests
        tests/mpmc_test.cpp)
    target_include_directories(
        fastipc_mpmc_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(
        fastipc_mpmc_tests
        PRIVATE FastIPC::fastipc Threads::Threads)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            fastipc_mpmc_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion)
    endif()
    add_test(NAME fastipc.mpmc COMMAND fastipc_mpmc_tests)
    add_test(
        NAME fastipc.mpmc.threaded_exactly_once
        COMMAND fastipc_mpmc_tests --case threaded_exactly_once)
    add_test(
        NAME fastipc.mpmc.process_exactly_once
        COMMAND fastipc_mpmc_tests --case process_exactly_once)
    add_test(
        NAME fastipc.mpmc.abandoned_reservation
        COMMAND fastipc_mpmc_tests --case abandoned_reservation)
    set_tests_properties(
        fastipc.mpmc.abandoned_reservation
        PROPERTIES LABELS fault)

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
        add_test(
            NAME fastipc.mpmc.benchmark_smoke
            COMMAND
                ${CMAKE_COMMAND}
                -DFASTIPC_MPMC_BENCHMARK=$<TARGET_FILE:fastipc_mpmc_benchmark>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/mpmc_benchmark_smoke.cmake)
        set_tests_properties(
            fastipc.mpmc.benchmark_smoke
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
