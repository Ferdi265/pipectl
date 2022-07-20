cmake_minimum_required(VERSION 3.20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(pipectl C)

# options
option(INSTALL_DOCUMENTATION "install wl-mirror manual pages" OFF)

# manual pages
add_subdirectory(man)
if (${BUILD_DOCUMENTATION})
    build_scdoc_man_page(pipectl 1)
endif()

# main target
file(GLOB sources src/*.c)
add_executable(pipectl ${sources})
target_compile_options(pipectl PRIVATE -Wall -Wextra)

# installation rules
include(GNUInstallDirs)

install(TARGETS pipectl DESTINATION "${CMAKE_INSTALL_BINDIR}")

if (${INSTALL_DOCUMENTATION})
    install_scdoc_man_page(pipectl 1)
endif()
