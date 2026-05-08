include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.6.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

include(CTest)
include(Catch)

# Each source file is its own executable so you can run only the relevant
# suite after a localised change:
#   ./build/bin/test_audio_file
#   ./build/bin/test_audio_engine
#   ./build/bin/test_scheduler
#   ./build/bin/test_cue_list
#
# Skip the hardware-dependent tests with the Catch2 tag filter:
#   ./build/bin/test_scheduler "~[audio_hw]"

function(add_engine_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SNDFILE_INCLUDE_DIRS}
    )
    target_link_libraries(${name} PRIVATE mcp_engine Catch2::Catch2WithMain)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    catch_discover_tests(${name})
endfunction()

add_engine_test(test_audio_file)
add_engine_test(test_audio_engine)
add_engine_test(test_scheduler)
add_engine_test(test_cue_list)
add_engine_test(test_timecode)
