cmake_minimum_required(VERSION 3.28)
project(Crossroadsfinder C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

file(GLOB CUBIOMES_SOURCES "cubiomes/*.c")
list(REMOVE_ITEM CUBIOMES_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cubiomes/tests.c")

add_executable(Crossroadsfinder main.cpp ${CUBIOMES_SOURCES})
target_include_directories(Crossroadsfinder PRIVATE cubiomes)

if(UNIX)
    target_link_libraries(Crossroadsfinder m pthread)
endif()

if(NOT MSVC)
    target_compile_options(Crossroadsfinder PRIVATE -O3 -march=native -ffast-math -funroll-loops)
endif()