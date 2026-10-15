cmake_minimum_required(VERSION 3.16)

# Default to Release so `cmake -B build` without an explicit type is
# reproducible (optimised, no debug symbols).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(wf_recorder_lite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(wf-recorder-lite
    src/main.cpp
    src/mainwindow.cpp
    src/areaoverlay.cpp)
# Qt >= 6.8's qt_add_executable() no longer implies AUTOMOC; set it
# explicitly so the recipe works across Qt6 point releases.
set_target_properties(wf-recorder-lite PROPERTIES AUTOMOC ON)
target_link_libraries(wf-recorder-lite PRIVATE Qt6::Widgets)
target_compile_options(wf-recorder-lite PRIVATE -Wall -Wextra)

# Unit tests (pure logic; the GUI process paths are covered by the CI
# offscreen smoke test).
enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Test)
qt_add_executable(wf-recorder-lite-tests tests/tst_pathtools.cpp)
set_target_properties(wf-recorder-lite-tests PROPERTIES AUTOMOC ON)
target_link_libraries(wf-recorder-lite-tests PRIVATE Qt6::Test)
target_include_directories(wf-recorder-lite-tests PRIVATE src)
target_compile_options(wf-recorder-lite-tests PRIVATE -Wall -Wextra)
add_test(NAME pathtools COMMAND wf-recorder-lite-tests)
