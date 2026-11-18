cmake_minimum_required(VERSION 3.20)
project(neomcp VERSION 0.0.1 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NEOMCP_IS_TOP_LEVEL ON)
else()
    set(NEOMCP_IS_TOP_LEVEL OFF)
endif()

option(NEOMCP_BUILD_TESTS "Build the tests" ${NEOMCP_IS_TOP_LEVEL})
option(NEOMCP_WARNINGS_AS_ERRORS "Treat warnings in neomcp's own targets as errors" OFF)
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
add_library(neomcp_core
    src/core/jsonrpc.cpp
    src/core/model.cpp
    src/core/wire.cpp
    src/core/mechanisms.cpp
    src/core/session.cpp)
add_library(neomcp::core ALIAS neomcp_core)
target_include_directories(neomcp_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(neomcp_core PUBLIC neomcp::profile neojson::neojson Threads::Threads)
target_compile_features(neomcp_core PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_core)

if(NEOMCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
