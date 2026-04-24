if(NOT DEFINED AUTORUNTIME_DDS_RPC_BENCHMARK)
  message(FATAL_ERROR "AUTORUNTIME_DDS_RPC_BENCHMARK was not provided")
endif()

execute_process(
  COMMAND "${AUTORUNTIME_DDS_RPC_BENCHMARK}" --self-test
  RESULT_VARIABLE benchmark_result
  OUTPUT_VARIABLE benchmark_output
  ERROR_VARIABLE benchmark_error
  TIMEOUT 25)
if(NOT benchmark_result EQUAL 0)
  message(FATAL_ERROR
    "DDS RPC benchmark self-test failed (${benchmark_result}): ${benchmark_error}")
endif()

set(required_fragments
  "\"type\":\"environment\""
  "\"schema_version\":1"
  "\"benchmark\":\"autoruntime_dds_rpc\""
  "\"source_revision\":\""
  "\"type\":\"result\""
  "\"status\":\"ok\""
  "\"transport\":\"cyclonedds\""
  "\"api\":\"autoruntime_node_service_client\""
  "\"topology\":\"same_process_two_participants\""
  "\"payload_bytes\":64"
  "\"expected_calls\":50"
  "\"completed_calls\":50"
  "\"call_failures\":0"
  "\"payload_mismatches\":0"
  "\"service_validation_failures\":0"
  "\"logical_payload_bytes\":6400"
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
    message(FATAL_ERROR "DDS RPC benchmark output missing: ${fragment}")
  endif()
endforeach()

string(REPLACE "\r\n" "\n" benchmark_output "${benchmark_output}")
string(REPLACE "\n" ";" benchmark_lines "${benchmark_output}")
set(environment_count 0)
set(result_count 0)
set(expected_run_id "")
foreach(line IN LISTS benchmark_lines)
  if(line STREQUAL "")
    continue()
  endif()
  string(JSON line_type ERROR_VARIABLE json_error GET "${line}" type)
  if(NOT json_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "invalid DDS RPC JSONL record: ${json_error}: ${line}")
  endif()
  string(JSON schema_version GET "${line}" schema_version)
  string(JSON run_id GET "${line}" run_id)
  if(NOT schema_version EQUAL 1 OR run_id STREQUAL "")
    message(FATAL_ERROR "invalid DDS RPC schema/run_id: ${line}")
  endif()
  if(expected_run_id STREQUAL "")
    set(expected_run_id "${run_id}")
  elseif(NOT run_id STREQUAL expected_run_id)
    message(FATAL_ERROR "DDS RPC run_id changed within one invocation")
  endif()

  if(line_type STREQUAL "environment")
    math(EXPR environment_count "${environment_count} + 1")
    string(JSON source_revision GET "${line}" source_revision)
    if(source_revision STREQUAL "")
      message(FATAL_ERROR "empty DDS RPC source revision")
    endif()
  elseif(line_type STREQUAL "result")
    math(EXPR result_count "${result_count} + 1")
    string(JSON status GET "${line}" status)
    string(JSON trial GET "${line}" trial)
    string(JSON payload_bytes GET "${line}" payload_bytes)
    string(JSON expected_calls GET "${line}" expected_calls)
    string(JSON completed_calls GET "${line}" completed_calls)
    string(JSON call_failures GET "${line}" call_failures)
    string(JSON payload_mismatches GET "${line}" payload_mismatches)
    string(JSON validation_failures GET "${line}" service_validation_failures)
    string(JSON logical_payload_bytes GET "${line}" logical_payload_bytes)
    string(JSON p50 GET "${line}" p50_us)
    string(JSON p95 GET "${line}" p95_us)
    string(JSON p99 GET "${line}" p99_us)
    string(JSON p99_9 GET "${line}" p99_9_us)
    string(JSON maximum GET "${line}" max_us)
    math(EXPR expected_payload_bytes "${expected_calls} * ${payload_bytes} * 2")
    if(NOT status STREQUAL "ok" OR
       NOT trial EQUAL 1 OR
       NOT payload_bytes EQUAL 64 OR
       NOT expected_calls EQUAL 50 OR
       NOT completed_calls EQUAL expected_calls OR
       NOT call_failures EQUAL 0 OR
       NOT payload_mismatches EQUAL 0 OR
       NOT validation_failures EQUAL 0 OR
       NOT logical_payload_bytes EQUAL expected_payload_bytes)
      message(FATAL_ERROR "DDS RPC exact-count invariant failed: ${line}")
    endif()
    if(p50 GREATER p95 OR p95 GREATER p99 OR
       p99 GREATER p99_9 OR p99_9 GREATER maximum)
      message(FATAL_ERROR "DDS RPC quantile order failed: ${line}")
    endif()
  else()
    message(FATAL_ERROR "unknown DDS RPC JSONL record type: ${line_type}")
  endif()
endforeach()
if(NOT environment_count EQUAL 1 OR NOT result_count EQUAL 1)
  message(FATAL_ERROR
    "expected 1 environment and 1 result, got ${environment_count}/${result_count}")
endif()

execute_process(
  COMMAND "${AUTORUNTIME_DDS_RPC_BENCHMARK}"
    --payload 64 --iterations 40 --warmup 10 --trials 2
  RESULT_VARIABLE trials_result
  OUTPUT_VARIABLE trials_output
  ERROR_VARIABLE trials_error
  TIMEOUT 25)
if(NOT trials_result EQUAL 0)
  message(FATAL_ERROR
    "DDS RPC two-trial run failed (${trials_result}): ${trials_error}")
endif()
string(REGEX MATCHALL "\"type\":\"result\"" trial_results "${trials_output}")
list(LENGTH trial_results trial_result_count)
if(NOT trial_result_count EQUAL 2)
  message(FATAL_ERROR
    "expected 2 DDS RPC trial results, got ${trial_result_count}")
endif()
foreach(fragment IN ITEMS
    "\"case_id\":1"
    "\"trial\":1"
    "\"trial\":2"
    "\"expected_calls\":40"
    "\"completed_calls\":40")
  string(FIND "${trials_output}" "${fragment}" fragment_position)
  if(fragment_position EQUAL -1)
    message(FATAL_ERROR "DDS RPC trial output missing: ${fragment}")
  endif()
endforeach()
