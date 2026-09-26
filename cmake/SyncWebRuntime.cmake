# Also used by the packaging scripts in --skip-cmake mode.
if(NOT IS_DIRECTORY "${ARIAREAD_SOURCE_DIR}/bindings/web/ariaread/web")
    message(FATAL_ERROR "AriaRead Web source directory is missing")
endif()
if(NOT ARIAREAD_OUTPUT_DIR)
    message(FATAL_ERROR "ARIAREAD_OUTPUT_DIR is required")
endif()

set(web_source "${ARIAREAD_SOURCE_DIR}/bindings/web/ariaread/web")
set(web_output "${ARIAREAD_OUTPUT_DIR}/web")
file(MAKE_DIRECTORY "${web_output}")
# Remove assets deleted from the source tree, while preserving unrelated runtime files.
file(GLOB_RECURSE old_assets RELATIVE "${web_output}" "${web_output}/*")
foreach(asset IN LISTS old_assets)
    if(NOT EXISTS "${web_source}/${asset}")
        file(REMOVE "${web_output}/${asset}")
    endif()
endforeach()
file(GLOB_RECURSE source_assets RELATIVE "${web_source}" "${web_source}/*")
foreach(asset IN LISTS source_assets)
    # COPYONLY compares content, including edits with the same timestamp.
    configure_file("${web_source}/${asset}" "${web_output}/${asset}" COPYONLY)
endforeach()

if(ARIAREAD_MINGW)
    # MinGW runtimes are compiler dependencies; Aria DLLs already build into bin.
    foreach(dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libgcc_s_sjlj-1.dll
                libstdc++-6.dll libwinpthread-1.dll)
        execute_process(COMMAND "${ARIAREAD_CXX_COMPILER}" "-print-file-name=${dll}"
            OUTPUT_VARIABLE dll_path OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE compiler_result)
        if(compiler_result EQUAL 0 AND EXISTS "${dll_path}")
            file(COPY "${dll_path}" DESTINATION "${ARIAREAD_OUTPUT_DIR}")
        endif()
    endforeach()
endif()
message(STATUS "AriaRead runtime: ${ARIAREAD_OUTPUT_DIR}")
