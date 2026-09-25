cmake_minimum_required(VERSION 3.16)

project(llm_cpp_agent
    VERSION 1.0.0
    DESCRIPTION "C++ clients for LingoFuse LLM services and agent toolchain"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug/Release)" FORCE)
endif()

# ---------------------------------------------------------------------------
# LingoFuse C++ interface directory
# ---------------------------------------------------------------------------
#
# The user MUST pass -DLINGOFUSE_CPP_DIR=<path> at configure time.
# No auto-search is performed: the operator names the directory
# explicitly, so the build never silently picks up a stale or
# unintended copy of the headers.
#
# The directory must contain:
#   LingoFuse.h
#   LingoFuse.c
#   LingoFuse.hpp
#   lf_io.hpp
#   json.hpp

set(LINGOFUSE_CPP_DIR "" CACHE PATH
    "Absolute path to the LingoFuse C++ interface directory \
(containing LingoFuse.h, LingoFuse.c, LingoFuse.hpp, lf_io.hpp, json.hpp). \
This value is REQUIRED; there is no auto-detection.")

if(NOT LINGOFUSE_CPP_DIR)
    message(FATAL_ERROR
        "LINGOFUSE_CPP_DIR is not set.\n"
        "Configure with:\n"
        "  cmake -DLINGOFUSE_CPP_DIR=<path to LingoFuse/cpp> ...\n"
        "The directory must contain:\n"
        "  LingoFuse.h  LingoFuse.c  LingoFuse.hpp  lf_io.hpp  json.hpp")
endif()

if(NOT EXISTS "${LINGOFUSE_CPP_DIR}/LingoFuse.h"
   OR NOT EXISTS "${LINGOFUSE_CPP_DIR}/LingoFuse.c"
   OR NOT EXISTS "${LINGOFUSE_CPP_DIR}/LingoFuse.hpp"
   OR NOT EXISTS "${LINGOFUSE_CPP_DIR}/lf_io.hpp"
   OR NOT EXISTS "${LINGOFUSE_CPP_DIR}/json.hpp")
    message(FATAL_ERROR
        "LINGOFUSE_CPP_DIR does not contain the required files.\n"
        "  Value: ${LINGOFUSE_CPP_DIR}\n"
        "Expected to find:\n"
        "  LingoFuse.h  LingoFuse.c  LingoFuse.hpp  lf_io.hpp  json.hpp")
endif()

message(STATUS "Using LINGOFUSE_CPP_DIR: ${LINGOFUSE_CPP_DIR}")

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Source / header inventory
# ---------------------------------------------------------------------------

set(LLM_CLIENT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/llm_client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/llm_client.hpp")

set(LLM_CPP_TOOL_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/llm_cpp_tool.cpp")

set(AGENT_SERVICE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_service.cpp")

set(AGENT_API_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_api.cpp")

set(LINGOFUSE_WRAPPER_SOURCES
    "${LINGOFUSE_CPP_DIR}/LingoFuse.c"
    "${LINGOFUSE_CPP_DIR}/LingoFuse.h")

set(LINGOFUSE_HEADERS
    "${LINGOFUSE_CPP_DIR}/LingoFuse.hpp"
    "${LINGOFUSE_CPP_DIR}/lf_io.hpp"
    "${LINGOFUSE_CPP_DIR}/json.hpp")

# ---------------------------------------------------------------------------
# lingofuse_c_wrapper: static library around the C ABI wrapper
# ---------------------------------------------------------------------------
#
# LingoFuse.c is the dynamic-loader C wrapper. It is compiled into a
# static library and linked into every consumer. It does NOT link
# against the LingoFuse runtime library at build time: the runtime is
# discovered at process start by the first LF_LoadLibrary() call.
#
# This target keeps its default output location (inside the build tree).

add_library(lingofuse_c_wrapper STATIC
    ${LINGOFUSE_WRAPPER_SOURCES})

source_group("LingoFuse" FILES ${LINGOFUSE_WRAPPER_SOURCES})

target_include_directories(lingofuse_c_wrapper PUBLIC
    "${LINGOFUSE_CPP_DIR}")

if(WIN32)
    target_link_libraries(lingofuse_c_wrapper PUBLIC kernel32)
elseif(APPLE)
    target_link_libraries(lingofuse_c_wrapper PUBLIC dl)
else()
    target_link_libraries(lingofuse_c_wrapper PUBLIC dl)
endif()

# ---------------------------------------------------------------------------
# llm_client: the C++ client library for LingoFuse LLM services
# ---------------------------------------------------------------------------
#
# Also keeps its default output location (inside the build tree).

add_library(llm_client STATIC
    ${LLM_CLIENT_SOURCES}
    ${LINGOFUSE_HEADERS})

source_group("Source" FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/llm_client.cpp")
source_group("Header" FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/llm_client.hpp")
source_group("LingoFuse\\Headers" FILES ${LINGOFUSE_HEADERS})

target_include_directories(llm_client PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LINGOFUSE_CPP_DIR}")

target_link_libraries(llm_client PUBLIC
    lingofuse_c_wrapper
    Threads::Threads)

if(MSVC)
    target_compile_options(llm_client PRIVATE /W4)
else()
    target_compile_options(llm_client PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# llm_cpp_tool: command-line client for LingoFuse LLM services
# ---------------------------------------------------------------------------

add_executable(llm_cpp_tool
    ${LLM_CPP_TOOL_SOURCES})

source_group("Source" FILES ${LLM_CPP_TOOL_SOURCES})

target_link_libraries(llm_cpp_tool PRIVATE llm_client)

if(MSVC)
    target_compile_options(llm_cpp_tool PRIVATE /W4)
else()
    target_compile_options(llm_cpp_tool PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# agent_service: LingoFuse agent beacon (C ABI)
# ---------------------------------------------------------------------------

add_executable(agent_service
    ${AGENT_SERVICE_SOURCES})

source_group("Source" FILES ${AGENT_SERVICE_SOURCES})

target_include_directories(agent_service PRIVATE
    "${LINGOFUSE_CPP_DIR}")

target_link_libraries(agent_service PRIVATE
    lingofuse_c_wrapper
    Threads::Threads)

if(MSVC)
    target_compile_options(agent_service PRIVATE /W4)
else()
    target_compile_options(agent_service PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# agent_api: LingoFuse arithmetic tool provider (C ABI)
# ---------------------------------------------------------------------------

add_executable(agent_api
    ${AGENT_API_SOURCES})

source_group("Source" FILES ${AGENT_API_SOURCES})

target_include_directories(agent_api PRIVATE
    "${LINGOFUSE_CPP_DIR}")

target_link_libraries(agent_api PRIVATE
    lingofuse_c_wrapper
    Threads::Threads)

if(MSVC)
    target_compile_options(agent_api PRIVATE /W4)
else()
    target_compile_options(agent_api PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Executable output location
# ---------------------------------------------------------------------------
#
# All three executables are placed directly next to the .cpp source
# files, i.e. in CMAKE_CURRENT_SOURCE_DIR. This matches the layout of
# the Pascal projects in the same tree, where the compiled binary sits
# alongside its source.
#
# Every configuration writes to the same path. Building Debug and then
# Release overwrites the previous binary. That is intentional: the
# source directory holds exactly one copy of each executable at any
# time.
#
# NOTE for multi-config generators (Visual Studio, Xcode): CMake
# requires an explicit per-config variable, otherwise it appends the
# configuration name to the path. All four per-config variables are
# therefore set to the same source directory.

set(_exe_targets
    llm_cpp_tool
    agent_service
    agent_api)

set_target_properties(${_exe_targets} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "----------------------------------------------------")
message(STATUS "llm_cpp_agent configuration")
message(STATUS "  LINGOFUSE_CPP_DIR : ${LINGOFUSE_CPP_DIR}")
message(STATUS "  CMAKE_BUILD_TYPE  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER_ID} "
               "${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Executable output : ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "  Library output    : build tree (default)")
message(STATUS "----------------------------------------------------")
message(STATUS "Build targets:")
message(STATUS "  lingofuse_c_wrapper  static library (C ABI loader)")
message(STATUS "  llm_client           static library (C++ LLM client)")
message(STATUS "  llm_cpp_tool         command-line LLM client")
message(STATUS "  agent_service        LingoFuse agent beacon (C ABI)")
message(STATUS "  agent_api            LingoFuse calculator tool "
               "provider (C ABI)")
message(STATUS "----------------------------------------------------")
message(STATUS "After building, place the LingoFuse runtime library")
message(STATUS "next to the executables (in ${CMAKE_CURRENT_SOURCE_DIR}):")
message(STATUS "  Windows: LingoFuse64.dll")
message(STATUS "  Linux:   liblingofuse.so")
message(STATUS "  macOS:   liblingofuse.dylib")
message(STATUS "----------------------------------------------------")