cmake_minimum_required(VERSION 3.20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(pipectl C)

# options
option(INSTALL_DOCUMENTATION "install wl-mirror manual pages" OFF)

# manual pages
find_program(SCDOC scdoc)
if (SCDOC)
    add_subdirectory(man)
    build_scdoc_man_page(pipectl 1)
endif()

# main target
file(GLOB sources src/*.c)
add_executable(pipectl ${sources})
target_compile_options(pipectl PRIVATE -Wall -Wextra)

# installation rule
install(TARGETS pipectl DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")

if (SCDOC AND ${INSTALL_DOCUMENTATION})
    install_scdoc_man_page(pipectl 1)
endif()
