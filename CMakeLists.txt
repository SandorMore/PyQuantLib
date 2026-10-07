cmake_minimum_required(VERSION 4.1.0)
project(QuantLib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# 1. Fetch pybind11
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
)
FetchContent_MakeAvailable(pybind11)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(Threads REQUIRED)

pybind11_add_module(QuantLib src/main.cpp)

target_link_libraries(QuantLib PRIVATE Boost::headers Threads::Threads)