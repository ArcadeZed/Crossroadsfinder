cmake_minimum_required(VERSION 3.28)
project(Crossroadsfinder C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# --- Pfade für Submodule definieren ---
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")

# --- Cubiomes Quellen ---
file(GLOB CUBIOMES_SOURCES "cubiomes/*.c")
list(REMOVE_ITEM CUBIOMES_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cubiomes/tests.c")

# --- Dear ImGui Quellen ---
# ImGui hat kein eigenes CMake, wir kompilieren die Dateien einfach mit
set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# --- Abhängigkeiten ---

# 1. GLFW als Submodule einbinden (statt find_package)
# Wir schalten unnötige GLFW-Features aus, um den Build zu beschleunigen
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(external/glfw)

# 2. OpenGL und Threads finden (das sind System-Bibliotheken)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# --- Executable erstellen ---
add_executable(Crossroadsfinder
        main.cpp
        ${CUBIOMES_SOURCES}
        ${IMGUI_SOURCES}
)

# --- Include-Verzeichnisse ---
target_include_directories(Crossroadsfinder PRIVATE
        cubiomes
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        external/glfw/include # Wichtig für die GLFW-Header
)

# --- Linker Optionen ---
target_link_libraries(Crossroadsfinder PRIVATE
        glfw                  # Das ist das Target von add_subdirectory
        ${OPENGL_LIBRARIES}
        Threads::Threads
)

if(UNIX)
    target_link_libraries(Crossroadsfinder PRIVATE m)
endif()

# --- Optimierungen ---
if(NOT MSVC)
    target_compile_options(Crossroadsfinder PRIVATE -O3 -march=native -ffast-math -funroll-loops)
else()
    # Optimierungen für Windows/Visual Studio
    target_compile_options(Crossroadsfinder PRIVATE /O2 /Oi /Ot)
endif()

# --- Windows-spezifisch: Konsolenfenster ausblenden (optional) ---
# Wenn du die Konsole im Hintergrund nicht willst, aktiviere das hier:
# set_target_properties(Crossroadsfinder PROPERTIES WIN32_EXECUTABLE TRUE)