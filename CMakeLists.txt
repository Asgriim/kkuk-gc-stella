cmake_minimum_required(VERSION 3.20)

project(stella_gc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(stella_gc STATIC
    gc.cpp
    internal/gc_stats.hpp
    internal/stella_abi.hpp
)
target_include_directories(stella_gc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(stella_runtime OBJECT
    runtime/runtime.c
    runtime/scalar.c
    runtime/main.c
)
target_include_directories(stella_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

foreach(example IN ITEMS double playlist chat history)
    add_executable(${example} examples/${example}.c)
    target_link_libraries(${example} PRIVATE stella_runtime stella_gc)
endforeach()