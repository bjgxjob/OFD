cmake_minimum_required(VERSION 3.16)
project(ofd_workspace LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OFD_BUILD_GUI "Build ofd-gui desktop app" ON)
option(OFD_BUILD_LIBOFD_TESTS "Build libOFD tests" OFF)

set(LIBOFD_BUILD_TESTS ${OFD_BUILD_LIBOFD_TESTS} CACHE BOOL "Build libOFD tests" FORCE)

add_subdirectory(libOFD)

if (OFD_BUILD_GUI)
    add_subdirectory(ofd-gui)
endif()

