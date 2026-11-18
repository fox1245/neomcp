# A project that uses neomcp the way the README says: the repository is added as a subdirectory and two programs link
# its targets. This project is not part of neomcp's own build. CI configures it against a checkout:
#
#   cmake -S tests/consumer -B build-consumer -DNEOMCP_SOURCE_DIR=<the checkout>
#
# The two programs are the README's, kept as calc.cpp and calc_http.cpp; check_readme.py fails when either differs from
# the README.
cmake_minimum_required(VERSION 3.20)
project(neomcp_consumer LANGUAGES CXX)

set(NEOMCP_SOURCE_DIR "" CACHE PATH "The neomcp checkout to build against")
if(NOT EXISTS "${NEOMCP_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Pass -DNEOMCP_SOURCE_DIR=<the neomcp checkout>")
endif()

# A consumer that wants the library's own warnings to stop its build says so before adding it.
set(NEOMCP_WARNINGS_AS_ERRORS ON)

add_subdirectory("${NEOMCP_SOURCE_DIR}" neomcp)

# neomcp must not change this project: it builds no tests of its own and sets no language level for its parent.
if(NEOMCP_BUILD_TESTS)
    message(SEND_ERROR "neomcp: NEOMCP_BUILD_TESTS is ON in a project that uses neomcp as a subdirectory")
endif()
if(DEFINED CMAKE_CXX_STANDARD)
    message(SEND_ERROR "neomcp: the parent's language level was set")
endif()
if(TARGET neomcp_core_test OR TARGET GTest::gtest)
    message(SEND_ERROR "neomcp: its tests or GoogleTest were added to a project that uses neomcp as a subdirectory")
endif()

# The README's programs, built with the warnings a consumer is likely to turn on.
function(neomcp_consumer_program target source)
    add_executable(${target} ${source})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

neomcp_consumer_program(calc calc.cpp)
target_link_libraries(calc PRIVATE neomcp::server neomcp::stdio)

neomcp_consumer_program(calc_http calc_http.cpp)
target_link_libraries(calc_http PRIVATE neomcp::server neomcp::http)
