cmake_minimum_required(VERSION 3.21)

# Geode v5 is built with C++23 and its public headers hard-require it:
# Geode/platform/platform.hpp contains
#   static_assert(GEODE_CXX_STANDARD >= 202302L,
#                 "Geode requires C++23 support to build! ... change CMAKE_CXX_STANDARD from 20 to 23")
# so requesting cxx_std_20 (as the original spec did) cannot compile.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(ClickBeforeFrames VERSION 1.0.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR
        "Unable to find Geode SDK! Please define the GEODE_SDK environment "
        "variable to point at your Geode SDK directory."
    )
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Hooks up mod.json metadata, the resource files and linking against Geode.
setup_geode_mod(${PROJECT_NAME})
