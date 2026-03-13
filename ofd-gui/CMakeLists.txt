cmake_minimum_required(VERSION 3.16)
project(ofd_gui LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(ofd-gui
    src/main.cpp
    src/main_window.cpp
    src/ofd_facade.cpp
)

target_include_directories(ofd-gui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ofd-gui
    PRIVATE
        Qt6::Widgets
        libofd
)

