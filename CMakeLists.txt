cmake_minimum_required(VERSION 4.1.0)
project(PyQuantLib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(PYBIND11_FINDPYTHON ON)
find_package(Python 3.12 REQUIRED COMPONENTS Interpreter Development.Module)

include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
)
FetchContent_MakeAvailable(pybind11)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
# Asio and Beast are header-only (apt: libboost-dev)
find_package(Boost 1.83 REQUIRED CONFIG)

pybind11_add_module(PyQuantLib
    src/main.cpp
    src/Parser/Parser.cpp
    src/OrderBook/OrderBook.cpp
)

target_link_libraries(PyQuantLib PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

# Enable AVX2 for SIMD JSON parsing
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(PyQuantLib PRIVATE -mavx2 -mbmi2)
endif()
