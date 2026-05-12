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

# Plugin abstraction tests link against mcp_plugin only (no portaudio/sndfile).
add_executable(test_plugin test_plugin.cpp)
target_include_directories(test_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_plugin PRIVATE mcp_plugin Catch2::Catch2WithMain)
target_compile_options(test_plugin PRIVATE -Wall -Wextra)
catch_discover_tests(test_plugin)

# External plugin backend tests (MissingPluginProcessor, NativePluginBackend).
add_executable(test_external_plugin_backend test_external_plugin_backend.cpp)
target_include_directories(test_external_plugin_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_external_plugin_backend PRIVATE mcp_plugin Catch2::Catch2WithMain)
target_compile_options(test_external_plugin_backend PRIVATE -Wall -Wextra)
if(APPLE)
    target_link_libraries(test_external_plugin_backend PRIVATE
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreFoundation"
        "-framework Foundation"
    )
endif()
catch_discover_tests(test_external_plugin_backend)

# AU adapter offline spike test (macOS only; non-Apple builds compile but skip).
add_executable(test_au_offline test_au_offline.cpp)
target_include_directories(test_au_offline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_au_offline PRIVATE mcp_plugin Catch2::Catch2WithMain)
target_compile_options(test_au_offline PRIVATE -Wall -Wextra)
if(APPLE)
    target_link_libraries(test_au_offline PRIVATE
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreFoundation"
        "-framework Foundation"
    )
endif()
catch_discover_tests(test_au_offline)
