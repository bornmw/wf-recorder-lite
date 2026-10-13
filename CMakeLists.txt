cmake_minimum_required(VERSION 3.16)
project(wl_recorder_ui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(wl-recorder-ui
    src/main.cpp
    src/mainwindow.cpp
    src/areaoverlay.cpp)
target_link_libraries(wl-recorder-ui PRIVATE Qt6::Widgets)
