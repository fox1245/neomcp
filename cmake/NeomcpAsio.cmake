# Standalone Asio, header-only. A project that already provides the target asio::asio keeps its own.
if(NOT TARGET asio::asio)
    include(FetchContent)
    # Asio 1.38.2 (tag asio-1-38-2). The repository has no CMake project, so nothing of it is added to the build.
    FetchContent_Declare(neomcp_asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG 8806a6803cde7054c3049d3666d3ec36786568c5
        SOURCE_SUBDIR no-cmake-project-here)
    FetchContent_MakeAvailable(neomcp_asio)

    find_package(Threads REQUIRED)
    add_library(neomcp_asio INTERFACE)
    add_library(asio::asio ALIAS neomcp_asio)
    # Warnings in the headers of a fetched dependency are not this project's to report.
    # The headers are under asio/include; the include entry at the repository root is a symbolic link to it, which a
    # checkout on Windows may not follow.
    target_include_directories(neomcp_asio SYSTEM INTERFACE "${neomcp_asio_SOURCE_DIR}/asio/include")
    target_compile_definitions(neomcp_asio INTERFACE ASIO_STANDALONE)
    target_link_libraries(neomcp_asio INTERFACE Threads::Threads)
    if(WIN32)
        # Asio needs to know the oldest Windows version it runs on (here Windows 10) and links the socket libraries.
        target_compile_definitions(neomcp_asio INTERFACE _WIN32_WINNT=0x0A00)
        target_link_libraries(neomcp_asio INTERFACE ws2_32 mswsock)
    endif()
endif()
