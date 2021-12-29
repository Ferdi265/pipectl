cmake_minimum_required(VERSION 3.20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(pipectl C)

# main target
file(GLOB sources src/*.c)
add_executable(pipectl ${sources})
target_compile_options(pipectl PRIVATE -Wall -Wextra)

# installation rule
install(TARGETS pipectl DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
