if(NOT DEFINED FASTIPC_MPMC_BENCHMARK)
    message(FATAL_ERROR "FASTIPC_MPMC_BENCHMARK was not provided")
endif()

execute_process(
    COMMAND "${FASTIPC_MPMC_BENCHMARK}" --self-test
    RESULT_VARIABLE benchmark_result
    OUTPUT_VARIABLE benchmark_output
    ERROR_VARIABLE benchmark_error
    TIMEOUT 30)

if(NOT benchmark_result EQUAL 0)
    message(
        FATAL_ERROR
        "MPMC benchmark self-test failed (${benchmark_result}): ${benchmark_error}")
endif()

set(
    required_fragments
    "\"type\":\"environment\""
    "\"schema_version\":1"
    "\"benchmark\":\"fastipc_mpmc_contention\""
    "\"run_id\":\""
    "\"source_revision\":\""
    "\"type\":\"result\""
    "\"status\":\"ok\""
    "\"queue\":\"bounded_mpmc_sequence_futex_copy\""
    "\"access_pattern\":\"touch_memory\""
    "\"producers\":1"
    "\"consumers\":1"
    "\"producers\":2"
    "\"consumers\":2"
    "\"payload_bytes\":64"
    "\"messages_per_producer\":200"
    "\"warmup_messages_per_producer\":20"
    "\"expected_messages\":"
    "\"sent_messages\":"
    "\"received_messages\":"
    "\"missing_messages\":0"
    "\"duplicate_messages\":0"
    "\"checksum_errors\":0"
    "\"messages_per_second\":"
    "\"payload_mib_per_second\":"
    "\"p50_us\":"
    "\"p95_us\":"
    "\"p99_us\":"
    "\"p99_9_us\":"
    "\"max_us\":"
    "\"cpu_time_ms\":"
    "\"voluntary_context_switches\":"
    "\"involuntary_context_switches\":"
    "\"peak_rss_kib\":")

foreach(fragment IN LISTS required_fragments)
    string(FIND "${benchmark_output}" "${fragment}" fragment_position)
    if(fragment_position EQUAL -1)
        message(FATAL_ERROR "MPMC benchmark output missing: ${fragment}")
    endif()
endforeach()

string(REPLACE "\r\n" "\n" benchmark_output "${benchmark_output}")
string(REPLACE "\n" ";" benchmark_lines "${benchmark_output}")
set(environment_count 0)
set(result_count 0)
set(expected_run_id "")
set(topologies)

foreach(line IN LISTS benchmark_lines)
    if(line STREQUAL "")
        continue()
    endif()
    string(JSON line_type ERROR_VARIABLE json_error GET "${line}" type)
    if(NOT json_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "invalid MPMC JSONL record: ${json_error}: ${line}")
    endif()
    string(JSON schema_version GET "${line}" schema_version)
    if(NOT schema_version EQUAL 1)
        message(FATAL_ERROR "unexpected MPMC schema: ${schema_version}")
    endif()
    string(JSON run_id GET "${line}" run_id)
    if(run_id STREQUAL "")
        message(FATAL_ERROR "empty MPMC run_id")
    endif()
    if(expected_run_id STREQUAL "")
        set(expected_run_id "${run_id}")
    elseif(NOT run_id STREQUAL expected_run_id)
        message(FATAL_ERROR "MPMC run_id changed within one invocation")
    endif()

    if(line_type STREQUAL "environment")
        math(EXPR environment_count "${environment_count} + 1")
        string(JSON source_revision GET "${line}" source_revision)
        if(source_revision STREQUAL "")
            message(FATAL_ERROR "empty MPMC source revision")
        endif()
    elseif(line_type STREQUAL "result")
        math(EXPR result_count "${result_count} + 1")
        string(JSON status GET "${line}" status)
        string(JSON producers GET "${line}" producers)
        string(JSON consumers GET "${line}" consumers)
        string(JSON payload_bytes GET "${line}" payload_bytes)
        string(JSON messages_per_producer
               GET "${line}" messages_per_producer)
        string(JSON expected_messages GET "${line}" expected_messages)
        string(JSON sent_messages GET "${line}" sent_messages)
        string(JSON received_messages GET "${line}" received_messages)
        string(JSON missing_messages GET "${line}" missing_messages)
        string(JSON duplicate_messages GET "${line}" duplicate_messages)
        string(JSON checksum_errors GET "${line}" checksum_errors)
        string(JSON trial GET "${line}" trial)
        string(JSON p50 GET "${line}" p50_us)
        string(JSON p95 GET "${line}" p95_us)
        string(JSON p99 GET "${line}" p99_us)
        string(JSON p99_9 GET "${line}" p99_9_us)
        string(JSON maximum GET "${line}" max_us)

        math(EXPR calculated_expected
             "${producers} * ${messages_per_producer}")
        if(NOT status STREQUAL "ok" OR
           NOT trial EQUAL 1 OR
           NOT payload_bytes EQUAL 64 OR
           NOT expected_messages EQUAL calculated_expected OR
           NOT sent_messages EQUAL expected_messages OR
           NOT received_messages EQUAL expected_messages OR
           NOT missing_messages EQUAL 0 OR
           NOT duplicate_messages EQUAL 0 OR
           NOT checksum_errors EQUAL 0)
            message(FATAL_ERROR "MPMC exact-count invariant failed: ${line}")
        endif()
        if(p50 GREATER p95 OR p95 GREATER p99 OR
           p99 GREATER p99_9 OR p99_9 GREATER maximum)
            message(FATAL_ERROR "MPMC quantile order failed: ${line}")
        endif()
        list(APPEND topologies "${producers}x${consumers}")
    else()
        message(FATAL_ERROR "unknown MPMC JSONL record type: ${line_type}")
    endif()
endforeach()

if(NOT environment_count EQUAL 1)
    message(FATAL_ERROR
            "expected 1 MPMC environment record, got ${environment_count}")
endif()
if(NOT result_count EQUAL 2)
    message(FATAL_ERROR
            "expected 2 MPMC results, got ${result_count}")
endif()
list(FIND topologies "1x1" one_by_one)
list(FIND topologies "2x2" two_by_two)
if(one_by_one EQUAL -1 OR two_by_two EQUAL -1)
    message(FATAL_ERROR "MPMC self-test topology coverage is incomplete")
endif()

execute_process(
    COMMAND
        "${FASTIPC_MPMC_BENCHMARK}"
        --topology=2x2
        --payload=64
        --messages=100
        --warmup=10
        --trials=2
    RESULT_VARIABLE trials_result
    OUTPUT_VARIABLE trials_output
    ERROR_VARIABLE trials_error
    TIMEOUT 30)
if(NOT trials_result EQUAL 0)
    message(
        FATAL_ERROR
        "MPMC two-trial run failed (${trials_result}): ${trials_error}")
endif()
string(
    REGEX MATCHALL
    "\"type\":\"result\""
    trial_results
    "${trials_output}")
list(LENGTH trial_results trial_result_count)
if(NOT trial_result_count EQUAL 2)
    message(FATAL_ERROR
            "expected 2 MPMC trial results, got ${trial_result_count}")
endif()
foreach(fragment IN ITEMS
        "\"case_id\":1"
        "\"trial\":1"
        "\"trial\":2")
    string(FIND "${trials_output}" "${fragment}" fragment_position)
    if(fragment_position EQUAL -1)
        message(FATAL_ERROR "MPMC trial output missing: ${fragment}")
    endif()
endforeach()
