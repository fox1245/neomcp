# The child-process transport of the client: it starts the server as a child process and speaks to it over the child's
# standard input and output. It defines the forms of Client::open that take a ChildProcess. The line splitter and the
# SIGPIPE guard come from neomcp::io, whose headers are internal, so the target sees the sources' folder privately.
#
# On POSIX systems other than Apple's the child is started with posix_spawn and the file actions that change its
# directory and close every descriptor above the standard three (posix_spawn_file_actions_addchdir_np and
# posix_spawn_file_actions_addclosefrom_np; glibc 2.34 has both). Where the C library lacks them the target is not
# built, and neither are its tests.
if(NOT WIN32 AND NOT APPLE)
    include(CheckCXXSymbolExists)
    check_cxx_symbol_exists(posix_spawn_file_actions_addclosefrom_np "spawn.h" NEOMCP_HAVE_SPAWN_CLOSEFROM)
    check_cxx_symbol_exists(posix_spawn_file_actions_addchdir_np "spawn.h" NEOMCP_HAVE_SPAWN_CHDIR)
    if(NOT NEOMCP_HAVE_SPAWN_CLOSEFROM OR NOT NEOMCP_HAVE_SPAWN_CHDIR)
        message(STATUS "neomcp: the C library has no posix_spawn file actions to close descriptors and change the directory; neomcp::client_stdio is not built")
        return()
    endif()
endif()

add_library(neomcp_client_stdio
    src/client_stdio/environment.cpp
    src/client_stdio/posix_child.cpp
    src/client_stdio/transport.cpp
    src/client_stdio/windows_child.cpp)
add_library(neomcp::client_stdio ALIAS neomcp_client_stdio)
target_include_directories(neomcp_client_stdio
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neomcp_client_stdio PUBLIC neomcp::client PRIVATE neomcp::io asio::asio Threads::Threads)
target_compile_features(neomcp_client_stdio PUBLIC cxx_std_20)
neomcp_configure_target(neomcp_client_stdio)
