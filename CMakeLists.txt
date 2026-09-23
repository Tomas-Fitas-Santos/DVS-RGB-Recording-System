cmake_minimum_required(VERSION 3.21)
project(dvxplorer_recorder VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
find_package(dv-processing REQUIRED)
find_package(Threads REQUIRED)

add_executable(dvxplorer_recorder src/main.cpp)
target_link_libraries(dvxplorer_recorder PRIVATE Qt6::Widgets dv::processing Threads::Threads)
target_compile_options(dvxplorer_recorder PRIVATE -Wall -Wextra -Wpedantic)
