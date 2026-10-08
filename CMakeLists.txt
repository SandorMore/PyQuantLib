cmake_minimum_required(VERSION 4.1.0)
project(QuantLib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(PYBIND11_FINDPYTHON ON)
find_package(Python 3.12 EXACT REQUIRED COMPONENTS Interpreter Development.Module)

include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6
)
FetchContent_MakeAvailable(pybind11)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(boost_asio  CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)

pybind11_add_module(QuantLib src/main.cpp)

target_link_libraries(QuantLib PRIVATE
    Boost::asio
    Boost::beast
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

if(WIN32)

    target_compile_definitions(QuantLib PRIVATE _WIN32_WINNT=0x0A00)
    if(MINGW)
        target_link_libraries(QuantLib PRIVATE ws2_32 mswsock)
    endif()
endif()