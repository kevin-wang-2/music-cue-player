find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO  REQUIRED portaudio-2.0)
pkg_check_modules(SNDFILE    REQUIRED sndfile)
pkg_check_modules(SAMPLERATE REQUIRED samplerate)

# nlohmann/json — header-only, used by ShowFile
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(mcp_engine STATIC
    src/AudioFile.cpp
    src/AudioEngine.cpp
    src/Scheduler.cpp
    src/CueList.cpp
    src/ShowFile.cpp
    src/FadeData.cpp
    src/StreamReader.cpp
)

target_include_directories(mcp_engine
    PUBLIC
        include
    PRIVATE
        ${PORTAUDIO_INCLUDE_DIRS}
        ${SNDFILE_INCLUDE_DIRS}
        ${SAMPLERATE_INCLUDE_DIRS}
)

target_link_directories(mcp_engine
    PUBLIC
        ${PORTAUDIO_LIBRARY_DIRS}
        ${SNDFILE_LIBRARY_DIRS}
        ${SAMPLERATE_LIBRARY_DIRS}
)

# LIBRARIES preserves "-framework Foo" as single tokens; LDFLAGS splits them.
target_link_libraries(mcp_engine
    PUBLIC
        ${PORTAUDIO_LIBRARIES}
        ${SNDFILE_LIBRARIES}
        ${SAMPLERATE_LIBRARIES}
        nlohmann_json::nlohmann_json
)

target_compile_options(mcp_engine PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cue_runner   src/cue_runner.cpp)
add_executable(show_editor  src/show_editor.cpp)
target_link_libraries(cue_runner  PRIVATE mcp_engine)
target_link_libraries(show_editor PRIVATE mcp_engine)

enable_testing()
add_subdirectory(tests)
