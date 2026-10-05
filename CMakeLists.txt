cmake_minimum_required(VERSION 3.25)
project(QuantLib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
)

FetchContent_MakeAvailable(pybind11)

pybind11_add_module(test MODULE tests/test1.cpp)
pybind11_add_module(QuantLib MODULE src/main.cpp)