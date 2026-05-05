cmake_minimum_required(VERSION 3.20)

include(FetchContent)

# ---- GLFW ------------------------------------------------------------------
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ---- Dear ImGui ------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.6
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

find_package(OpenGL REQUIRED)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)
# Suppress deprecation warnings on macOS (OpenGL still works through 14.x)
if(APPLE)
    target_compile_definitions(imgui_lib PUBLIC GL_SILENCE_DEPRECATION)
endif()

# ---- tinyfiledialogs -------------------------------------------------------
FetchContent_Declare(
    tinyfd
    GIT_REPOSITORY https://github.com/native-toolkit/libtinyfiledialogs.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tinyfd)

# ---- mcp_ui ----------------------------------------------------------------
# Compile tinyfiledialogs.c directly into the executable (it's a single file).
add_executable(mcp_ui
    src/main.cpp
    ${tinyfd_SOURCE_DIR}/tinyfiledialogs.c
)
target_include_directories(mcp_ui PRIVATE ${tinyfd_SOURCE_DIR})
target_link_libraries(mcp_ui PRIVATE mcp_engine imgui_lib)
target_compile_options(mcp_ui PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>
    $<$<COMPILE_LANGUAGE:C>:-Wno-deprecated-declarations>
)
if(APPLE)
    target_compile_definitions(mcp_ui PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(mcp_ui PRIVATE "-framework AppKit")
endif()
