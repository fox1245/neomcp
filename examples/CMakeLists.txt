# The examples. Two ways to build them:
#
#   - as part of neomcp: configure neomcp with -DNEOMCP_BUILD_EXAMPLES=ON (the top-level CMakeLists.txt adds this folder);
#   - on their own, against an installed neomcp:
#       cmake -S examples -B build-examples -DCMAKE_PREFIX_PATH=<the prefix neomcp was installed to>
#       cmake --build build-examples
#
# The programs are built in this folder's binary directory; run_unattended.sh runs the ones that need no person.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.22)
    project(neomcp_examples LANGUAGES CXX)
    option(NEOMCP_WARNINGS_AS_ERRORS "Treat warnings in the examples as errors" OFF)
    # The components every installed neomcp has, and those a configuration may leave out (the HTTP client without
    # libcurl, the child-process transport on a C library without the posix_spawn file actions).
    find_package(neomcp CONFIG REQUIRED COMPONENTS server stdio http client client_in_process
        OPTIONAL_COMPONENTS client_stdio client_http)
    message(STATUS "neomcp ${neomcp_VERSION} from ${neomcp_DIR}")
endif()

set(NEOMCP_EXAMPLE_TARGETS "")

# One program: examples/<name>.cpp linked with the targets that follow. The target is example_<name>, the program <name>.
function(neomcp_example name)
    add_executable(example_${name} ${name}.cpp)
    set_target_properties(example_${name} PROPERTIES OUTPUT_NAME ${name})
    target_compile_features(example_${name} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(example_${name} PRIVATE /W4)
        if(NEOMCP_WARNINGS_AS_ERRORS)
            target_compile_options(example_${name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(example_${name} PRIVATE -Wall -Wextra -Wpedantic)
        if(NEOMCP_WARNINGS_AS_ERRORS)
            target_compile_options(example_${name} PRIVATE -Werror)
        endif()
    endif()
    target_link_libraries(example_${name} PRIVATE ${ARGN})
    set(NEOMCP_EXAMPLE_TARGETS ${NEOMCP_EXAMPLE_TARGETS} example_${name} PARENT_SCOPE)
endfunction()

# 1 and 2: servers.
neomcp_example(stdio_server neomcp::server neomcp::stdio)
neomcp_example(http_server neomcp::server neomcp::http)
# 5 and 6: a client and a server in one process.
neomcp_example(client_in_process neomcp::client_in_process)
neomcp_example(elicitation neomcp::client_in_process)
# 8: a server that asks for authorization.
neomcp_example(protected_server neomcp::server neomcp::http)

# 3: a client of a child process; a build without that transport leaves it out.
if(TARGET neomcp::client_stdio)
    neomcp_example(client_stdio neomcp::client_stdio)
else()
    message(STATUS "neomcp examples: neomcp::client_stdio is not available, so client_stdio is not built")
endif()

# 4 and 7: clients of an HTTP server; a build without the client's HTTP transport (NEOMCP_HTTP_CLIENT=OFF) leaves
# them out.
if(TARGET neomcp::client_http)
    neomcp_example(client_http neomcp::client_http)
    neomcp_example(oauth_client neomcp::client_http)
else()
    message(STATUS "neomcp examples: neomcp::client_http is not available, so client_http and oauth_client are not built")
endif()

# Every example, as one target: cmake --build <dir> --target neomcp_examples
add_custom_target(neomcp_examples)
add_dependencies(neomcp_examples ${NEOMCP_EXAMPLE_TARGETS})
