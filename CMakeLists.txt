cmake_minimum_required(VERSION 3.28)
project(Crossroadsfinder C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

if(MSVC)
    # Statisches Linken der Runtime für Portabilität
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")

file(GLOB CUBIOMES_SOURCES "cubiomes/*.c")
list(REMOVE_ITEM CUBIOMES_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cubiomes/tests.c")

set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# GLFW Optionen
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(external/glfw)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# --- QUELLDATEIEN ZUSAMMENFÜHREN ---
set(PROJECT_SOURCES main.cpp ${CUBIOMES_SOURCES} ${IMGUI_SOURCES})

# Wenn wir auf Windows sind, fügen wir die Resource-Datei für das Icon hinzu
if(WIN32)
    # Stelle sicher, dass die Datei resources.rc im selben Ordner wie diese CMakeLists.txt liegt!
    set(PROJECT_SOURCES ${PROJECT_SOURCES} resources.rc)
endif()

# --- EXECUTABLE ERSTELLEN ---
# Hier nutzen wir jetzt die oben definierte Liste ${PROJECT_SOURCES}
add_executable(Crossroadsfinder ${PROJECT_SOURCES})

target_include_directories(Crossroadsfinder PRIVATE
        cubiomes
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        external/glfw/include
)

target_link_libraries(Crossroadsfinder PRIVATE
        glfw
        ${OPENGL_LIBRARIES}
        Threads::Threads
)

if(UNIX)
    target_link_libraries(Crossroadsfinder PRIVATE m)
endif()

if(NOT MSVC)
    target_compile_options(Crossroadsfinder PRIVATE -O3 -march=native -ffast-math -funroll-loops)
else()
    target_compile_options(Crossroadsfinder PRIVATE /O2 /Oi /Ot)
endif()

# Blendet die Konsole im Hintergrund auf Windows aus
set_target_properties(Crossroadsfinder PROPERTIES WIN32_EXECUTABLE TRUE)