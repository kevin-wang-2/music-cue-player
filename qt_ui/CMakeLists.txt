cmake_minimum_required(VERSION 3.20)

# Qt requires CMake's automoc/autouic/autorcc
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Prefer Qt6; fall back to Qt5 if not found
find_package(Qt6 QUIET COMPONENTS Core Widgets)
if(Qt6_FOUND)
    set(QT_TARGET Qt6::Widgets)
    message(STATUS "Found Qt6 ${Qt6_VERSION}")
else()
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets)
    set(QT_TARGET Qt5::Widgets)
    message(STATUS "Found Qt5 ${Qt5_VERSION}")
endif()

set(QT_UI_SOURCES
    src/main.cpp
    src/ShowHelpers.h
    src/ShowHelpers.cpp
    src/AppModel.h
    src/AppModel.cpp
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
    src/DeviceDialog.h
    src/DeviceDialog.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

if(APPLE)
    list(APPEND QT_UI_SOURCES src/platform_mac.mm)
endif()

add_executable(mcp_qt_ui ${QT_UI_SOURCES})

target_include_directories(mcp_qt_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/engine/include
)

target_link_libraries(mcp_qt_ui PRIVATE
    mcp_engine
    ${QT_TARGET}
)
if(APPLE)
    target_link_libraries(mcp_qt_ui PRIVATE "-framework Cocoa")
endif()

target_compile_options(mcp_qt_ui PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>: -Wall -Wextra>
)

# macOS: set bundle info and enable Retina HiDPI
if(APPLE)
    set_target_properties(mcp_qt_ui PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "MCP"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    )
    # Ad-hoc sign after every build so macOS can launch the app
    add_custom_command(TARGET mcp_qt_ui POST_BUILD
        COMMAND codesign --force --deep --sign -
                "$<TARGET_BUNDLE_DIR:mcp_qt_ui>"
        COMMENT "Ad-hoc signing mcp_qt_ui.app"
    )
endif()
