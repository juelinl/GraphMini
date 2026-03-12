cmake_minimum_required(VERSION 3.20)
project(GraphMini)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g -ggdb -fno-omit-frame-pointer")
include(FetchContent)
if(APPLE)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -mno-omit-leaf-frame-pointer")
    if(NOT OpenMP_ROOT)
        execute_process(
                COMMAND brew --prefix libomp
                OUTPUT_VARIABLE OPENMP_ROOT_HINT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
        if(OPENMP_ROOT_HINT)
            set(OpenMP_ROOT "${OPENMP_ROOT_HINT}" CACHE PATH "Path to libomp" FORCE)
        endif()
    endif()
endif()
set(CXXOPTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CXXOPTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CXXOPTS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        cxxopts
        GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
        GIT_TAG v3.3.1
        GIT_SHALLOW TRUE)

set(TBB_TEST OFF CACHE BOOL "" FORCE)
set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TBB_STRICT OFF CACHE BOOL "" FORCE)
set(TBB4PY_BUILD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        oneTBB
        GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
        GIT_TAG v2022.3.0
        GIT_SHALLOW TRUE)

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 12.1.0
        GIT_SHALLOW TRUE)

set(PYBIND11_TEST OFF CACHE BOOL "" FORCE)
set(PYBIND11_INSTALL OFF CACHE BOOL "" FORCE)
set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
set(Python3_FIND_VIRTUALENV FIRST)
if(DEFINED ENV{VIRTUAL_ENV} AND EXISTS "$ENV{VIRTUAL_ENV}/bin/python")
    set(Python3_EXECUTABLE "$ENV{VIRTUAL_ENV}/bin/python" CACHE FILEPATH "Python executable" FORCE)
elseif(NOT EXISTS "/usr/include/python3.12/Python.h")
    if(EXISTS "$ENV{HOME}/miniconda3/bin/python")
        set(Python3_EXECUTABLE "$ENV{HOME}/miniconda3/bin/python" CACHE FILEPATH "Python executable" FORCE)
    elseif(EXISTS "$ENV{HOME}/anaconda3/bin/python")
        set(Python3_EXECUTABLE "$ENV{HOME}/anaconda3/bin/python" CACHE FILEPATH "Python executable" FORCE)
    endif()
endif()
find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.13.6
        GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(cxxopts oneTBB fmt pybind11)
find_package(OpenMP REQUIRED COMPONENTS CXX)

set(PROJECT_PROFILE_DIR "${PROJECT_SOURCE_DIR}/profile/")
set(PROJECT_LOG_DIR "${PROJECT_SOURCE_DIR}/log/")
set(PROJECT_PLAN_DIR "${PROJECT_SOURCE_DIR}/plan/")

configure_file(include/configure.h.in ${PROJECT_BINARY_DIR}/configure/configure.h)
message("PROJECT_BINARY_DIR: " ${PROJECT_BINARY_DIR})
message("PROJECT_SOURCE_DIR: " ${PROJECT_SOURCE_DIR})
message("CMAKE_CXX_FLAGS_DEBUG: " ${CMAKE_CXX_FLAGS_DEBUG})
message("CMAKE_CXX_FLAGS_RELEASE: " ${CMAKE_CXX_FLAGS_RELEASE})
include_directories(${PROJECT_BINARY_DIR}/configure)
include_directories(include)
include_directories(src)
add_subdirectory(src)
