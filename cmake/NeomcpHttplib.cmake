# The HTTP server library, header-only. A project that already provides the target neomcp_httplib keeps its own.
#
# The version of cpp-httplib the listener is written against. src/http/listener.cpp copies one function of it
# (httplib::Server::process_and_close_socket) and stops the build when the header it is compiled with is another
# version; a change of the commit below goes with a change of this version, after that function was compared again.
set(NEOMCP_HTTPLIB_VERSION "0.67.0")
if(NOT TARGET neomcp_httplib)
    include(FetchContent)
    # The commit of the tag of NEOMCP_HTTPLIB_VERSION. Its own CMake project is not run: it would look for TLS and
    # compression libraries, and neomcp uses none of them. Only the header is used.
    FetchContent_Declare(neomcp_httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG 7800c3dc5b33285cbb18baa5998de4ff7ef63515
        SOURCE_SUBDIR no-cmake-project-here)
    FetchContent_MakeAvailable(neomcp_httplib)

    find_package(Threads REQUIRED)
    add_library(neomcp_httplib INTERFACE)
    # Warnings in the header of a fetched dependency are not this project's to report.
    target_include_directories(neomcp_httplib SYSTEM INTERFACE "${neomcp_httplib_SOURCE_DIR}")
    target_link_libraries(neomcp_httplib INTERFACE Threads::Threads)
    if(WIN32)
        target_link_libraries(neomcp_httplib INTERFACE ws2_32)
    endif()
endif()
