cmake_minimum_required(VERSION 3.20)

# Qt requires CMake's automoc/autouic/autorcc
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Python 3 for Scriptlet cue support
if(APPLE AND NOT Python3_ROOT_DIR)
    # Prefer Homebrew Python over the macOS system stub
    set(Python3_ROOT_DIR "/opt/homebrew/opt/python@3.13")
endif()
find_package(Python3 COMPONENTS Development REQUIRED)
message(STATUS "Found Python3 ${Python3_VERSION} — ${Python3_EXECUTABLE}")

# ShowHelpers.cpp uses libsndfile + libsamplerate for audio collection/conversion.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE    REQUIRED sndfile)
pkg_check_modules(SAMPLERATE REQUIRED samplerate)

# Prefer Qt6; fall back to Qt5 if not found
find_package(Qt6 QUIET COMPONENTS Core Widgets Network)
if(Qt6_FOUND)
    set(QT_TARGET Qt6::Widgets)
    set(QT_NETWORK_TARGET Qt6::Network)
    message(STATUS "Found Qt6 ${Qt6_VERSION}")
else()
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets Network)
    set(QT_TARGET Qt5::Widgets)
    set(QT_NETWORK_TARGET Qt5::Network)
    message(STATUS "Found Qt5 ${Qt5_VERSION}")
endif()

set(QT_UI_SOURCES
    src/main.cpp
    src/ShowHelpers.h
    src/ShowHelpers.cpp
    src/CollectDialog.h
    src/CollectDialog.cpp
    src/AppModel.h
    src/AppModel.cpp
    src/SettingsDialog.h
    src/SettingsDialog.cpp
    src/FaderWidget.h
    src/FaderWidget.cpp
    src/WaveformView.h
    src/WaveformView.cpp
    src/CueTableView.h
    src/CueTableView.cpp
    src/InspectorWidget.h
    src/InspectorWidget.cpp
    src/TimelineGroupView.h
    src/TimelineGroupView.cpp
    src/SyncGroupView.h
    src/SyncGroupView.cpp
    src/MusicContextView.h
    src/MusicContextView.cpp
    src/CueListPanel.h
    src/CueListPanel.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/MCImport.h
    src/MCImport.cpp
    src/RenumberDialog.h
    src/RenumberDialog.cpp
    src/MidiInputManager.h
    src/MidiInputManager.cpp
    src/OscServer.h
    src/OscServer.cpp
    src/ControlsDialog.h
    src/ControlsDialog.cpp
    src/ScriptletEngine.h
    src/ScriptletEngine.cpp
    src/PythonEditor.h
    src/PythonEditor.cpp
    src/ScriptEditorWidget.h
    src/ScriptEditorWidget.cpp
    src/ScriptletLibraryDialog.h
    src/ScriptletLibraryDialog.cpp
    src/ProjectStatusDialog.h
    src/ProjectStatusDialog.cpp
    src/ShowInfoDialog.h
    src/ShowInfoDialog.cpp
)

if(APPLE)
    list(APPEND QT_UI_SOURCES src/platform_mac.mm)
endif()

add_executable(mcp_qt_ui ${QT_UI_SOURCES})

target_include_directories(mcp_qt_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/engine/include
    ${Python3_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
    ${SAMPLERATE_INCLUDE_DIRS}
)

target_link_libraries(mcp_qt_ui PRIVATE
    mcp_engine
    ${QT_TARGET}
    ${QT_NETWORK_TARGET}
    Python3::Python
)
if(APPLE)
    target_link_libraries(mcp_qt_ui PRIVATE "-framework Cocoa"
                                            "-framework SystemConfiguration")
endif()
if(WIN32)
    target_link_libraries(mcp_qt_ui PRIVATE iphlpapi)
endif()

target_compile_options(mcp_qt_ui PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>: -Wall -Wextra>
)

# macOS: set bundle info and enable Retina HiDPI
if(APPLE)
    set_target_properties(mcp_qt_ui PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Music Cue Player"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    )
    # Ad-hoc sign after every build so macOS can launch the app
    add_custom_command(TARGET mcp_qt_ui POST_BUILD
        COMMAND codesign --force --deep --sign -
                "$<TARGET_BUNDLE_DIR:mcp_qt_ui>"
        COMMENT "Ad-hoc signing mcp_qt_ui.app"
    )
endif()
