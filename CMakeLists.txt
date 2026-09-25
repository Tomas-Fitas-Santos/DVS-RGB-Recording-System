cmake_minimum_required(VERSION 3.21)
project(dvxplorer_recorder VERSION 0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
find_package(dv-processing REQUIRED)
find_package(Threads REQUIRED)

set(GALAXY_SDK_ROOT "" CACHE PATH "Extracted Daheng Galaxy_camera SDK directory")
find_path(GALAXY_INCLUDE_DIR GxIAPI.h
    HINTS "${GALAXY_SDK_ROOT}/inc" /usr/include /usr/local/include)
find_library(GALAXY_LIBRARY gxiapi
    HINTS "${GALAXY_SDK_ROOT}/lib/armv8" /usr/lib /usr/local/lib)
if(NOT GALAXY_INCLUDE_DIR OR NOT GALAXY_LIBRARY)
    message(FATAL_ERROR "Daheng Galaxy C SDK not found. Pass -DGALAXY_SDK_ROOT=/path/to/Galaxy_camera (the directory with inc/ and lib/armv8/), or install its headers and library.")
endif()

add_executable(dvxplorer_recorder src/main.cpp src/rgb_recorder.cpp)
target_include_directories(dvxplorer_recorder PRIVATE "${GALAXY_INCLUDE_DIR}")
target_link_libraries(dvxplorer_recorder PRIVATE Qt6::Widgets dv::processing Threads::Threads "${GALAXY_LIBRARY}")
target_compile_options(dvxplorer_recorder PRIVATE -Wall -Wextra -Wpedantic)
