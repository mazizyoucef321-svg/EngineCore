cmake_minimum_required(VERSION 3.20)
project(EngineCore CXX)

if(NOT WIN32)
    message(FATAL_ERROR "EngineCore targets Windows x64 (MSVC) only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. ImGui location
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")

# 2. Include paths
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# 3. Engine library: every module, not just the orchestrator
add_library(EngineCoreLib STATIC
    src/core/thread_steering.cpp
    src/core/dpc_steering_engine.cpp
    src/core/engine_orchestrator.cpp
    src/core/security_manager.cpp
    src/shader/vault_purging.cpp
    src/telemetry/vram_monitor.cpp
    src/network/pso_cloud_network.cpp
)

target_compile_definitions(EngineCoreLib PUBLIC NOMINMAX UNICODE _UNICODE)

# 4. Executable: GUI subsystem (WIN32 keyword goes here, not on add_library)
add_executable(EngineCoreApp WIN32
    src/main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
)

target_compile_definitions(EngineCoreApp PRIVATE NOMINMAX UNICODE _UNICODE)

# 5. Link everything the engine modules + ImGui backends need
target_link_libraries(EngineCoreApp PRIVATE
    EngineCoreLib
    d3d12.lib
    dxgi.lib
    dxguid.lib
    winhttp.lib
    bcrypt.lib
    advapi32.lib
)