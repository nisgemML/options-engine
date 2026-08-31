# Drives tools/replay_trace.cpp as a single ctest step: record a trace,
# then replay it in a second process and byte-diff the fills. Two separate
# process invocations of ${REPLAY_BIN} matter here — it proves the engine
# reproduces the same output from a cold start given the same recorded
# input, not just within one still-warm process.
#
# Expects -DREPLAY_BIN=<path> -DWORK_DIR=<path> on the cmake -P command line.

if(NOT DEFINED REPLAY_BIN OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "run_replay_determinism.cmake requires -DREPLAY_BIN and -DWORK_DIR")
endif()

set(TRACE_FILE "${WORK_DIR}/ctest_replay_trace.bin")

execute_process(
    COMMAND "${REPLAY_BIN}" record 424242 500000 "${TRACE_FILE}"
    RESULT_VARIABLE RECORD_RESULT
)
if(NOT RECORD_RESULT EQUAL 0)
    message(FATAL_ERROR "replay_trace record failed with exit code ${RECORD_RESULT}")
endif()

execute_process(
    COMMAND "${REPLAY_BIN}" replay "${TRACE_FILE}"
    RESULT_VARIABLE REPLAY_RESULT
)
if(NOT REPLAY_RESULT EQUAL 0)
    message(FATAL_ERROR "replay_trace replay failed with exit code ${REPLAY_RESULT} — engine is non-deterministic on this input")
endif()

file(REMOVE "${TRACE_FILE}" "${TRACE_FILE}.fills")
