cmake_minimum_required(VERSION 3.22)
project(totk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(SDL3 CONFIG REQUIRED)

add_library(totk SHARED android_main.c)
target_link_libraries(totk PRIVATE SDL3::SDL3 vulkan log)

# Gradle packages only the libraries in CMake's output directory, so SDL3 is copied next to libtotk.so.
add_custom_command(TARGET totk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:totk>)
