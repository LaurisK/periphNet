# ==============================================================================
# PeriphNet Integration Test Application
#
# Build (CLI only, zero GUI deps):
#   cmake -B build -S tests/integration -DBUILD_GUI=OFF
#   cmake --build build -j8
#
# Build (with GUI, needs: sudo apt install libsdl2-dev libgl-dev):
#   cmake -B build -S tests/integration
#   cmake --build build -j8
# ==============================================================================

cmake_minimum_required(VERSION 3.22)
project(PeriphNetIntegration CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Project root for locating tools/trice, til.json, li.json at runtime
get_filename_component(PERIPHNET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# ==============================================================================
# Core library (POSIX only, no GUI deps)
# ==============================================================================

add_library(periphnet_core STATIC
    src/core/SerialPort.cpp
    src/core/TriceProcess.cpp
    src/core/Log.cpp
    src/core/Config.cpp
    src/core/Device.cpp
    src/core/TestRunner.cpp
    src/core/TestCases.cpp
    src/core/transport/SerialTriceSource.cpp
    src/core/transport/UdpTriceSource.cpp
    src/core/transport/UsbTriceSource.cpp
    src/core/transport/SerialCommandTransport.cpp
)

target_include_directories(periphnet_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(periphnet_core PUBLIC
    PERIPHNET_PROJECT_ROOT="${PERIPHNET_ROOT}"
)

find_package(Threads REQUIRED)
target_link_libraries(periphnet_core PUBLIC Threads::Threads)

# ==============================================================================
# CLI executable (always built)
# ==============================================================================

add_executable(periphnet_cli src/cli/main.cpp)
target_link_libraries(periphnet_cli PRIVATE periphnet_core)

# ==============================================================================
# GUI executable (optional — requires SDL2 + OpenGL)
# ==============================================================================

option(BUILD_GUI "Build the GUI application (requires SDL2 + OpenGL)" ON)

if(BUILD_GUI)
    find_package(SDL2 QUIET)
    find_package(OpenGL QUIET)

    if(SDL2_FOUND AND OpenGL_FOUND)
        # Fetch Dear ImGui (no CMakeLists.txt — we define the target manually)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        v1.91.8
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(imgui)

        add_library(imgui_lib STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        )

        target_include_directories(imgui_lib PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
        )

        target_link_libraries(imgui_lib PUBLIC
            SDL2::SDL2
            OpenGL::GL
        )

        # GUI application
        add_executable(periphnet_gui
            src/gui/main.cpp
            src/gui/App.cpp
            src/gui/SettingsTab.cpp
            src/gui/TriceLogTab.cpp
            src/gui/ConsoleTab.cpp
            src/gui/TestsTab.cpp
        )

        target_link_libraries(periphnet_gui PRIVATE
            periphnet_core
            imgui_lib
        )

        message(STATUS "GUI target enabled (SDL2 + OpenGL found)")
    else()
        message(STATUS "GUI target disabled (SDL2 or OpenGL not found)")
        message(STATUS "  Install with: sudo apt install libsdl2-dev libgl-dev")
    endif()
endif()
