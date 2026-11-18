cmake_minimum_required(VERSION 3.20)
project(neomcp VERSION 0.0.1 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NEOMCP_IS_TOP_LEVEL ON)
else()
    set(NEOMCP_IS_TOP_LEVEL OFF)
endif()

option(NEOMCP_BUILD_TESTS "Build the tests" ${NEOMCP_IS_TOP_LEVEL})
option(NEOMCP_WARNINGS_AS_ERRORS "Treat warnings in neomcp's own targets as errors" OFF)
option(NEOMCP_INTEROP_TESTS
    "Register the interoperability test, which drives the fixture server with the official Python SDK's client. Needs NEOMCP_INTEROP_PYTHON"
    OFF)
set(NEOMCP_INTEROP_PYTHON "" CACHE FILEPATH
    "With NEOMCP_INTEROP_TESTS: the Python interpreter that has tests/interop/requirements.txt installed")
option(NEOMCP_TEST_TIME_BOUNDS
    "Give the tests that check how cost grows with size a short time limit. Turn it off for instrumented builds, whose slowdown is not uniform across platforms"
    ON)

include(cmake/NeomcpNeojson.cmake)

# Language level and warnings for the targets of this project only; third-party targets keep their own settings.
function(neomcp_configure_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(NEOMCP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(NEOMCP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Every profiles/*.json is built into the library; the set is whatever the folder holds.
file(GLOB NEOMCP_PROFILE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/profiles/*.json")
list(SORT NEOMCP_PROFILE_FILES)
if(NOT NEOMCP_PROFILE_FILES)
    message(FATAL_ERROR "neomcp: profiles/ holds no *.json file")
endif()

set(NEOMCP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(NEOMCP_PROFILE_DATA_CPP "${NEOMCP_GENERATED_DIR}/neomcp_profile_data.cpp")
set(NEOMCP_PROFILE_LIST "${NEOMCP_GENERATED_DIR}/profile_files.txt")

# The list of files goes to the generator through a file that changes only when the list changes.
string(REPLACE ";" "\n" NEOMCP_PROFILE_LIST_TEXT "${NEOMCP_PROFILE_FILES}")
set(NEOMCP_PROFILE_LIST_OLD "")
if(EXISTS "${NEOMCP_PROFILE_LIST}")
    file(READ "${NEOMCP_PROFILE_LIST}" NEOMCP_PROFILE_LIST_OLD)
endif()
if(NOT NEOMCP_PROFILE_LIST_OLD STREQUAL "${NEOMCP_PROFILE_LIST_TEXT}\n")
    file(WRITE "${NEOMCP_PROFILE_LIST}" "${NEOMCP_PROFILE_LIST_TEXT}\n")
endif()

add_custom_command(
    OUTPUT "${NEOMCP_PROFILE_DATA_CPP}"
    COMMAND ${CMAKE_COMMAND}
        "-DLIST_FILE=${NEOMCP_PROFILE_LIST}"
        "-DOUTPUT=${NEOMCP_PROFILE_DATA_CPP}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedProfiles.cmake"
    DEPENDS ${NEOMCP_PROFILE_FILES} "${NEOMCP_PROFILE_LIST}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedProfiles.cmake"
    COMMENT "Embedding the revision profiles"
    VERBATIM)

add_library(neomcp_profile
    src/profile/profile.cpp
    src/profile/mechanisms.cpp
    src/profile/builtin.cpp
    "${NEOMCP_PROFILE_DATA_CPP}")
add_library(neomcp::profile ALIAS neomcp_profile)
target_include_directories(neomcp_profile
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_profile PRIVATE neojson::neojson)
# The public header uses C++20 library types, so every consumer compiles as C++20.
target_compile_features(neomcp_profile PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_profile)

# Describing C++ types: JSON Schema from the types and conversion of values. Header-only.
add_library(neomcp_describe INTERFACE)
add_library(neomcp::describe ALIAS neomcp_describe)
target_include_directories(neomcp_describe INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(neomcp_describe INTERFACE neojson::neojson)
target_compile_features(neomcp_describe INTERFACE cxx_std_20)

# JSON Schema 2020-12 validation. It depends on the JSON value type only.
add_library(neomcp_schema
    src/schema/compile.cpp
    src/schema/evaluate.cpp
    src/schema/pattern_scan.cpp
    src/schema/uri.cpp
    src/schema/validator.cpp
    src/schema/value.cpp)
add_library(neomcp::schema ALIAS neomcp_schema)
target_include_directories(neomcp_schema
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_schema PUBLIC neojson::neojson)
target_compile_features(neomcp_schema PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_schema)

# The protocol core: JSON-RPC messages, the revision-independent model, the members filter, the mechanisms and their
# registry, the session engine and the in-memory test peer. It does no I/O of its own.
find_package(Threads REQUIRED)

# std::stop_token is part of C++20, and a standard library may still ship it behind a flag. The public headers use
# it, so the flag that makes it available goes to every consumer of the core.
include(CheckCXXSourceCompiles)
set(NEOMCP_STOP_TOKEN_PROBE [[
#include <stop_token>
int main() {
    std::stop_source source;
    return source.get_token().stop_requested() ? 1 : 0;
}
]])
set(NEOMCP_STOP_TOKEN_OPTIONS "")
set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX20_STANDARD_COMPILE_OPTION}")
check_cxx_source_compiles("${NEOMCP_STOP_TOKEN_PROBE}" NEOMCP_HAS_STOP_TOKEN)
if(NOT NEOMCP_HAS_STOP_TOKEN)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_CXX20_STANDARD_COMPILE_OPTION} -fexperimental-library")
    set(CMAKE_REQUIRED_LINK_OPTIONS -fexperimental-library)
    check_cxx_source_compiles("${NEOMCP_STOP_TOKEN_PROBE}" NEOMCP_HAS_STOP_TOKEN_WITH_EXPERIMENTAL_LIBRARY)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT NEOMCP_HAS_STOP_TOKEN_WITH_EXPERIMENTAL_LIBRARY)
        message(FATAL_ERROR "neomcp: the standard library has no std::stop_token (C++20 <stop_token>)")
    endif()
    set(NEOMCP_STOP_TOKEN_OPTIONS -fexperimental-library)
endif()
unset(CMAKE_REQUIRED_FLAGS)

add_library(neomcp_core
    src/core/base64.cpp
    src/core/headers.cpp
    src/core/jsonrpc.cpp
    src/core/model.cpp
    src/core/uri_template.cpp
    src/core/wire.cpp
    src/core/mechanisms.cpp
    src/core/session.cpp
    src/core/peer.cpp)
add_library(neomcp::core ALIAS neomcp_core)
target_include_directories(neomcp_core
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_core PUBLIC neomcp::profile neojson::neojson Threads::Threads)
target_compile_features(neomcp_core PUBLIC cxx_std_20)
target_compile_options(neomcp_core PUBLIC ${NEOMCP_STOP_TOKEN_OPTIONS})
target_link_options(neomcp_core PUBLIC ${NEOMCP_STOP_TOKEN_OPTIONS})
neomcp_configure_target(neomcp_core)

# The server layer: Server, tools registered from C++ types, the handlers' Context, the worker pool and the thread
# of coroutine handlers. Coroutine handlers use standalone Asio, which appears in the public headers.
include(cmake/NeomcpAsio.cmake)

add_library(neomcp_server
    src/server/server.cpp)
add_library(neomcp::server ALIAS neomcp_server)
target_include_directories(neomcp_server PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(neomcp_server PUBLIC neomcp::core neomcp::describe neomcp::schema asio::asio)
target_compile_features(neomcp_server PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_server)

# The stdio transport: serve one client on standard input and output (or on any pair of byte streams).
add_library(neomcp_stdio
    src/stdio/stdio.cpp
    src/stdio/fd_streams.cpp)
add_library(neomcp::stdio ALIAS neomcp_stdio)
target_include_directories(neomcp_stdio
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_stdio PUBLIC neomcp::server)
target_compile_features(neomcp_stdio PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_stdio)

# The HTTP transport. The building blocks know nothing of the protocol: the request and response model, a body
# stream, server-sent events framing, media types, the Host and Origin policy, random bytes from the operating system,
# and one listener. On them: the protocol's endpoint (a handler without I/O) and HttpServer, which runs it on the
# listener. The HTTP library is header-only, used by one source file, and absent from the public headers.
include(cmake/NeomcpHttplib.cmake)

add_library(neomcp_http
    src/http/message.cpp
    src/http/body_stream.cpp
    src/http/sse.cpp
    src/http/media_type.cpp
    src/http/authority.cpp
    src/http/policy.cpp
    src/http/random.cpp
    src/http/listener.cpp
    src/http/timer.cpp
    src/http/endpoint.cpp
    src/http/http_server.cpp)
add_library(neomcp::http ALIAS neomcp_http)
target_include_directories(neomcp_http
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_http PUBLIC neomcp::server Threads::Threads PRIVATE neomcp_httplib asio::asio)
if(WIN32)
    target_link_libraries(neomcp_http PRIVATE ws2_32 bcrypt)
endif()
target_compile_features(neomcp_http PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_http)

if(NEOMCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
