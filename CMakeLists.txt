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
find_package(boost_asio  CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)

pybind11_add_module(PyQuantLib
    src/main.cpp
    src/Parser/Parser.cpp
)

target_link_libraries(PyQuantLib PRIVATE
    Boost::asio
    Boost::beast
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

# Enable AVX2 for SIMD JSON parsing
if(MSVC)
    target_compile_options(PyQuantLib PRIVATE /arch:AVX2)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(PyQuantLib PRIVATE -mavx2 -mbmi2)
endif()

if(WIN32)

    target_compile_definitions(PyQuantLib PRIVATE _WIN32_WINNT=0x0A00)
    if(MINGW)
        target_link_libraries(PyQuantLib PRIVATE ws2_32 mswsock)
    endif()
endif()