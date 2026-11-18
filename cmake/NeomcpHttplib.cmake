# The HTTP server library, header-only. A project that already provides the target neomcp_httplib keeps its own.
if(NOT TARGET neomcp_httplib)
    include(FetchContent)
    # cpp-httplib 0.67.0 (commit of the tag v0.67.0). Its own CMake project is not run: it would look for TLS and
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
