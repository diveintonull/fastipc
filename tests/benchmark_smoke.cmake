if(NOT DEFINED FASTIPC_BENCHMARK)
    message(FATAL_ERROR "FASTIPC_BENCHMARK was not provided")
endif()

execute_process(
    COMMAND "${FASTIPC_BENCHMARK}" --self-test
    RESULT_VARIABLE benchmark_result
    OUTPUT_VARIABLE benchmark_output
    ERROR_VARIABLE benchmark_error
    TIMEOUT 30)

if(NOT benchmark_result EQUAL 0)
    message(
        FATAL_ERROR
        "benchmark self-test failed (${benchmark_result}): ${benchmark_error}")
endif()

set(
    required_fragments
    "\"type\":\"environment\""
    "\"transport\":\"shared_memory\""
    "\"transport\":\"unix_domain_socket\""
    "\"transport\":\"pipe\""
    "\"payload_bytes\":64"
    "\"payload_bytes\":1048576"
    "\"messages_per_second\":"
    "\"p50_us\":"
    "\"p95_us\":"
    "\"p99_us\":"
    "\"cpu_time_ms\":"
    "\"voluntary_context_switches\":"
    "\"involuntary_context_switches\":"
    "\"parent_peak_rss_kib\":"
    "\"child_peak_rss_kib\":")

foreach(fragment IN LISTS required_fragments)
    string(FIND "${benchmark_output}" "${fragment}" fragment_position)
    if(fragment_position EQUAL -1)
        message(FATAL_ERROR "benchmark output missing: ${fragment}")
    endif()
endforeach()

string(
    REGEX MATCHALL
    "\"type\":\"result\""
    benchmark_results
    "${benchmark_output}")
list(LENGTH benchmark_results benchmark_result_count)
if(NOT benchmark_result_count EQUAL 6)
    message(
        FATAL_ERROR
        "expected 6 benchmark results, got ${benchmark_result_count}")
endif()
