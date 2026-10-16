cmake_minimum_required(VERSION 3.16)

# Default to Release so `cmake -B build` without an explicit type is
# reproducible (optimised, no debug symbols).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(wf_recorder_lite VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
# gtkmm-4.0 pulls in gtk4, cairomm, pangomm, giomm (see gtkmm-4.0.pc).
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)

add_executable(wf-recorder-lite
    src/main.cpp
    src/mainwindow.cpp
    src/areaoverlay.cpp)
target_include_directories(wf-recorder-lite PRIVATE ${GTKMM_INCLUDE_DIRS})
target_link_libraries(wf-recorder-lite PRIVATE ${GTKMM_LIBRARIES})
target_compile_options(wf-recorder-lite PRIVATE -Wall -Wextra ${GTKMM_CFLAGS_OTHER})

# Unit tests (pure logic; the GUI process paths are covered by the CI
# offscreen smoke test).
enable_testing()
add_executable(wf-recorder-lite-tests tests/tst_pathtools.cpp)
target_include_directories(wf-recorder-lite-tests PRIVATE src)
target_compile_options(wf-recorder-lite-tests PRIVATE -Wall -Wextra)
add_test(NAME pathtools COMMAND wf-recorder-lite-tests)
