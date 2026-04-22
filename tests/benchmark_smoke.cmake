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
    "\"schema_version\":1"
    "\"run_id\":\""
    "\"type\":\"baseline_status\""
    "\"transport\":\"iceoryx\""
    "\"available\":false"
    "\"detail\":\""
    "\"transport\":\"fastipc_copy\""
    "\"transport\":\"fastipc_zero_copy\""
    "\"transport\":\"unix_domain_socket\""
    "\"transport\":\"pipe\""
    "\"access_pattern\":\"transport_only\""
    "\"access_pattern\":\"touch_memory\""
    "\"payload_bytes\":64"
    "\"payload_bytes\":1048576"
    "\"case_id\":"
    "\"trial\":1"
    "\"status\":\"ok\""
    "\"completed_round_trips\":"
    "\"logical_messages\":"
    "\"payload_bytes_transferred\":"
    "\"messages_per_second\":"
    "\"p50_us\":"
    "\"p95_us\":"
    "\"p99_us\":"
    "\"p99_9_us\":"
    "\"max_us\":"
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
if(NOT benchmark_result_count EQUAL 16)
    message(
        FATAL_ERROR
        "expected 16 benchmark results, got ${benchmark_result_count}")
endif()

string(
    REGEX MATCHALL
    "\"type\":\"baseline_status\""
    benchmark_baselines
    "${benchmark_output}")
list(LENGTH benchmark_baselines benchmark_baseline_count)
if(NOT benchmark_baseline_count EQUAL 5)
    message(
        FATAL_ERROR
        "expected 5 baseline statuses, got ${benchmark_baseline_count}")
endif()

string(REPLACE "\r\n" "\n" benchmark_output "${benchmark_output}")
string(REPLACE "\n" ";" benchmark_lines "${benchmark_output}")
set(environment_count 0)
set(structured_baseline_count 0)
set(structured_result_count 0)
set(iceoryx_unavailable_seen FALSE)
set(expected_run_id "")
set(result_case_ids)

foreach(line IN LISTS benchmark_lines)
    if(line STREQUAL "")
        continue()
    endif()
    string(JSON line_type ERROR_VARIABLE json_error GET "${line}" type)
    if(NOT json_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "invalid JSONL record: ${json_error}: ${line}")
    endif()

    string(JSON schema_version GET "${line}" schema_version)
    if(NOT schema_version EQUAL 1)
        message(FATAL_ERROR "unexpected schema version: ${schema_version}")
    endif()
    string(JSON run_id GET "${line}" run_id)
    if(run_id STREQUAL "")
        message(FATAL_ERROR "empty run_id")
    endif()
    if(expected_run_id STREQUAL "")
        set(expected_run_id "${run_id}")
    elseif(NOT run_id STREQUAL expected_run_id)
        message(FATAL_ERROR "run_id changed within one invocation")
    endif()

    if(line_type STREQUAL "environment")
        math(EXPR environment_count "${environment_count} + 1")
    elseif(line_type STREQUAL "baseline_status")
        math(EXPR structured_baseline_count
             "${structured_baseline_count} + 1")
        string(JSON transport GET "${line}" transport)
        string(JSON detail GET "${line}" detail)
        if(detail STREQUAL "")
            message(FATAL_ERROR "baseline detail must not be empty")
        endif()
        if(transport STREQUAL "iceoryx")
            string(JSON available GET "${line}" available)
            if(available)
                message(FATAL_ERROR "iceoryx unexpectedly reported available")
            endif()
            set(iceoryx_unavailable_seen TRUE)
        endif()
    elseif(line_type STREQUAL "result")
        math(EXPR structured_result_count
             "${structured_result_count} + 1")
        string(JSON case_id GET "${line}" case_id)
        string(JSON trial GET "${line}" trial)
        string(JSON status GET "${line}" status)
        string(JSON payload_bytes GET "${line}" payload_bytes)
        string(JSON iterations GET "${line}" iterations)
        string(JSON completed GET "${line}" completed_round_trips)
        string(JSON logical_messages GET "${line}" logical_messages)
        string(JSON transferred GET "${line}" payload_bytes_transferred)
        string(JSON p50 GET "${line}" p50_us)
        string(JSON p95 GET "${line}" p95_us)
        string(JSON p99 GET "${line}" p99_us)
        string(JSON p99_9 GET "${line}" p99_9_us)
        string(JSON maximum GET "${line}" max_us)

        if(NOT trial EQUAL 1 OR NOT status STREQUAL "ok")
            message(FATAL_ERROR "invalid trial/status record: ${line}")
        endif()
        list(FIND result_case_ids "${case_id}" duplicate_case)
        if(NOT duplicate_case EQUAL -1)
            message(FATAL_ERROR "duplicate self-test case_id: ${case_id}")
        endif()
        list(APPEND result_case_ids "${case_id}")

        math(EXPR expected_messages "${iterations} * 2")
        math(EXPR expected_bytes
             "${expected_messages} * ${payload_bytes}")
        if(NOT completed EQUAL iterations OR
           NOT logical_messages EQUAL expected_messages OR
           NOT transferred EQUAL expected_bytes)
            message(FATAL_ERROR "exact-count invariant failed: ${line}")
        endif()
        if(p50 GREATER p95 OR p95 GREATER p99 OR
           p99 GREATER p99_9 OR p99_9 GREATER maximum)
            message(FATAL_ERROR "quantile order invariant failed: ${line}")
        endif()
    else()
        message(FATAL_ERROR "unknown JSONL record type: ${line_type}")
    endif()
endforeach()

if(NOT environment_count EQUAL 1)
    message(
        FATAL_ERROR
        "expected 1 environment record, got ${environment_count}")
endif()
if(NOT structured_baseline_count EQUAL 5)
    message(
        FATAL_ERROR
        "expected 5 structured baseline records, got ${structured_baseline_count}")
endif()
if(NOT structured_result_count EQUAL 16)
    message(
        FATAL_ERROR
        "expected 16 structured result records, got ${structured_result_count}")
endif()
if(NOT iceoryx_unavailable_seen)
    message(FATAL_ERROR "missing unavailable iceoryx baseline status")
endif()

execute_process(
    COMMAND
        "${FASTIPC_BENCHMARK}"
        --transport=iceoryx
        --payload=64
        --iterations=1
        --warmup=1
    RESULT_VARIABLE iceoryx_result
    OUTPUT_VARIABLE iceoryx_output
    ERROR_VARIABLE iceoryx_error
    TIMEOUT 10)
if(NOT iceoryx_result EQUAL 3)
    message(
        FATAL_ERROR
        "explicit unavailable baseline should return 3, got ${iceoryx_result}: ${iceoryx_error}")
endif()
string(
    REGEX MATCHALL
    "\"type\":\"result\""
    iceoryx_results
    "${iceoryx_output}")
list(LENGTH iceoryx_results iceoryx_result_count)
if(NOT iceoryx_result_count EQUAL 0)
    message(FATAL_ERROR "unavailable iceoryx emitted a fake result")
endif()

execute_process(
    COMMAND
        "${FASTIPC_BENCHMARK}"
        --transport=pipe
        --access=transport_only
        --payload=64
        --iterations=2
        --warmup=1
        --trials=2
    RESULT_VARIABLE trials_result
    OUTPUT_VARIABLE trials_output
    ERROR_VARIABLE trials_error
    TIMEOUT 10)
if(NOT trials_result EQUAL 0)
    message(
        FATAL_ERROR
        "two-trial benchmark failed (${trials_result}): ${trials_error}")
endif()
string(
    REGEX MATCHALL
    "\"type\":\"result\""
    trial_results
    "${trials_output}")
list(LENGTH trial_results trial_result_count)
if(NOT trial_result_count EQUAL 2)
    message(
        FATAL_ERROR
        "expected 2 trial results, got ${trial_result_count}")
endif()
foreach(
    fragment
    IN ITEMS
        "\"case_id\":1"
        "\"trial\":1"
        "\"trial\":2")
    string(FIND "${trials_output}" "${fragment}" fragment_position)
    if(fragment_position EQUAL -1)
        message(FATAL_ERROR "trial output missing: ${fragment}")
    endif()
endforeach()

execute_process(
    COMMAND
        "${FASTIPC_BENCHMARK}"
        --transport=pipe
        --access=transport_only
        --payload=64
        --iterations=2
        --warmup=1
        --strict-baselines
    RESULT_VARIABLE strict_result
    OUTPUT_VARIABLE strict_output
    ERROR_VARIABLE strict_error
    TIMEOUT 10)
if(NOT strict_result EQUAL 3)
    message(
        FATAL_ERROR
        "strict baseline mode should return 3, got ${strict_result}: ${strict_error}")
endif()
string(
    REGEX MATCHALL
    "\"type\":\"result\""
    strict_results
    "${strict_output}")
list(LENGTH strict_results strict_result_count)
if(NOT strict_result_count EQUAL 1)
    message(
        FATAL_ERROR
        "strict mode must finish its available case, got ${strict_result_count} results")
endif()
