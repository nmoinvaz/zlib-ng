if(NOT OUTPUT)
    if(WIN32)
        set(OUTPUT NUL)
    else()
        set(OUTPUT /dev/null)
    endif()
endif()

string(TIMESTAMP START_TIME "%s" UTC)
if(INPUT)
    execute_process(COMMAND ${COMMAND}
        RESULT_VARIABLE CMD_RESULT
        INPUT_FILE ${INPUT}
        OUTPUT_FILE ${OUTPUT})
else()
    execute_process(COMMAND ${COMMAND}
        RESULT_VARIABLE CMD_RESULT
        OUTPUT_FILE ${OUTPUT})
endif()
string(TIMESTAMP END_TIME "%s" UTC)
math(EXPR EXEC_TIME "${END_TIME} - ${START_TIME}")

set(EXIT_RESULT ${CMD_RESULT})
if(SUCCESS_EXIT)
    list(FIND SUCCESS_EXIT ${CMD_RESULT} _INDEX)
    if (${_INDEX} GREATER -1)
        set(EXIT_RESULT 0)
    endif()
endif()

if(EVENT)
    # We use default python command because we don't know what python version
    # python command is mapped to on Windows and python modules are installed by CI
    # using the default python command
    cmake_host_system_information(RESULT SYSTEM_HOSTNAME QUERY HOSTNAME)
    execute_process(COMMAND python ${CMAKE_CURRENT_SOURCE_DIR}/cmake/track-test.py
        "--user_id=${SYSTEM_HOSTNAME}"
        "--event_name=${EVENT}"
        --cmd_result=${CMD_RESULT}
        --exit_result=${EXIT_RESULT}
        --elapsed_time=${EXEC_TIME}
        "--input_file=${INPUT}"
        "--output_file=${OUTPUT}"
        ${COMMAND})
endif()

if(EXIT_RESULT)
    message(FATAL_ERROR "${COMMAND} failed: ${EXIT_RESULT}")
endif()
