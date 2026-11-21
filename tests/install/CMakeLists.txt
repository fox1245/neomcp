# A project that uses an installed neomcp: it finds the package, builds one program per group of components and runs
# them as tests. It is not part of neomcp's own build. CI installs neomcp and configures this project against the
# install prefix:
#
#   cmake --install build --prefix <prefix>
#   cmake -S tests/install -B build-installed -DCMAKE_PREFIX_PATH=<prefix>
#   cmake --build build-installed
#   ctest --test-dir build-installed
#
# The tests also check that the package refuses what it should: a component that is not installed and versions
# outside its compatibility rule (refusals/).
cmake_minimum_required(VERSION 3.22)
project(neomcp_installed LANGUAGES CXX)

# The components every installed neomcp has, and those a configuration may leave out (the HTTP client without
# libcurl, the child-process transport on a C library without the posix_spawn file actions).
set(NEOMCP_ALWAYS server stdio http client client_in_process)
set(NEOMCP_OPTIONAL client_stdio client_http)
find_package(neomcp CONFIG REQUIRED COMPONENTS ${NEOMCP_ALWAYS} OPTIONAL_COMPONENTS ${NEOMCP_OPTIONAL})
message(STATUS "neomcp ${neomcp_VERSION} from ${neomcp_DIR}")
foreach(component IN LISTS NEOMCP_OPTIONAL)
    message(STATUS "neomcp component ${component}: ${neomcp_${component}_FOUND}")
endforeach()

enable_testing()

# The programs are built with the warnings a consumer is likely to turn on, with neomcp's headers as the package
# gives them (outside this project, so not reported as its own).
function(neomcp_installed_program target source)
    add_executable(${target} ${source})
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

neomcp_installed_program(installed_server server.cpp)
target_link_libraries(installed_server PRIVATE neomcp::server neomcp::stdio)

neomcp_installed_program(installed_in_process in_process.cpp)
target_link_libraries(installed_in_process PRIVATE neomcp::client_in_process)
add_test(NAME Installed.InProcess COMMAND installed_in_process)

if(neomcp_client_stdio_FOUND)
    neomcp_installed_program(installed_stdio_client stdio_client.cpp)
    target_link_libraries(installed_stdio_client PRIVATE neomcp::client_stdio)
    add_test(NAME Installed.Stdio COMMAND installed_stdio_client $<TARGET_FILE:installed_server>)
endif()

if(neomcp_client_http_FOUND)
    neomcp_installed_program(installed_http http.cpp)
    target_link_libraries(installed_http PRIVATE neomcp::http neomcp::client_http)
    add_test(NAME Installed.Http COMMAND installed_http)
endif()

# A host that serves and calls over every transport links the servers' and the clients' components into one program;
# two of them that defined one name with external linkage would fail to link here, or in a shared build would bind one
# definition for both.
if(neomcp_client_stdio_FOUND AND neomcp_client_http_FOUND)
    neomcp_installed_program(installed_host http.cpp)
    target_link_libraries(installed_host PRIVATE neomcp::server neomcp::stdio neomcp::http neomcp::client
        neomcp::client_stdio neomcp::client_http)
    add_test(NAME Installed.Host COMMAND installed_host)
endif()

# Each case configures refusals/ against the same package; the project there fails when find_package does not decide
# as the case expects. The control case asks for the version found here and must succeed.
# The generator and compiler of this project go to each case (a generator for several configurations chooses its
# compiler itself).
set(neomcp_case_tools "-G${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND neomcp_case_tools "-A${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND neomcp_case_tools "-T${CMAKE_GENERATOR_TOOLSET}")
endif()
get_property(neomcp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT neomcp_multi_config)
    list(APPEND neomcp_case_tools "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()
function(neomcp_package_case name version component expected)
    add_test(NAME Installed.Package.${name}
        COMMAND ${CMAKE_COMMAND}
            -S "${CMAKE_CURRENT_SOURCE_DIR}/refusals"
            -B "${CMAKE_CURRENT_BINARY_DIR}/refusals/${name}"
            ${neomcp_case_tools}
            "-Dneomcp_DIR=${neomcp_DIR}"
            "-DREQUEST_VERSION=${version}"
            "-DREQUEST_COMPONENT=${component}"
            "-DEXPECTED=${expected}")
endfunction()
math(EXPR neomcp_next_minor "${neomcp_VERSION_MINOR} + 1")
math(EXPR neomcp_next_patch "${neomcp_VERSION_PATCH} + 1")
neomcp_package_case(control ${neomcp_VERSION} server found)
neomcp_package_case(unknown_component ${neomcp_VERSION} no_such_component refused)
neomcp_package_case(next_patch ${neomcp_VERSION_MAJOR}.${neomcp_VERSION_MINOR}.${neomcp_next_patch} server refused)
neomcp_package_case(next_minor ${neomcp_VERSION_MAJOR}.${neomcp_next_minor}.0 server refused)
# An older patch of the same minor version is accepted, an older minor version is not (only where there is one).
if(neomcp_VERSION_PATCH GREATER 0)
    neomcp_package_case(older_patch ${neomcp_VERSION_MAJOR}.${neomcp_VERSION_MINOR}.0 server found)
endif()
if(neomcp_VERSION_MINOR GREATER 0)
    math(EXPR neomcp_older_minor "${neomcp_VERSION_MINOR} - 1")
    neomcp_package_case(older_minor ${neomcp_VERSION_MAJOR}.${neomcp_older_minor}.0 server refused)
endif()
